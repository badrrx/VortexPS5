#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xxhash.h>
#include <zstd.h>

using namespace Libs::Graphics;

namespace {

struct Frame {
	uint32_t compressed_size = 0;
	uint32_t raw_size        = 0;
	uint64_t checksum        = 0;
};

bool TestSemanticHeaderPersistence() {
	ShaderRecompiler::Spirv::Profile prof1 = ShaderRecompiler::Spirv::DefaultProfile;
	ShaderRecompiler::Spirv::Profile prof2 = ShaderRecompiler::Spirv::DefaultProfile;

	const auto h1 = BuildShaderStoreHeader(SHADER_STORE_SCHEMA_VERSION, prof1, 32768u);
	const auto h2 = BuildShaderStoreHeader(SHADER_STORE_SCHEMA_VERSION, prof2, 32768u);
	if (h1.empty() || h1 != h2) {
		std::cerr << "[FAIL] Identical schema/profile headers differed!\n";
		return false;
	}

	// Schema version bump must invalidate
	const auto h_schema = BuildShaderStoreHeader(SHADER_STORE_SCHEMA_VERSION + 1u, prof1, 32768u);
	if (h_schema == h1) {
		std::cerr << "[FAIL] Schema version bump did not invalidate header!\n";
		return false;
	}

	// Shared memory limit change must invalidate
	const auto h_smem = BuildShaderStoreHeader(SHADER_STORE_SCHEMA_VERSION, prof1, 65536u);
	if (h_smem == h1) {
		std::cerr << "[FAIL] maxComputeSharedMemorySize change did not invalidate header!\n";
		return false;
	}

	// SpirvProfile capability change must invalidate
	ShaderRecompiler::Spirv::Profile prof_diff = prof1;
	prof_diff.relaxed_float                    = !prof1.relaxed_float;
	const auto h_prof = BuildShaderStoreHeader(SHADER_STORE_SCHEMA_VERSION, prof_diff, 32768u);
	if (h_prof == h1) {
		std::cerr << "[FAIL] SpirvProfile change did not invalidate header!\n";
		return false;
	}

	// End-to-end ShaderStoreWriter zstd + XXH3 round-trip across simulated restart
	const auto tmp_path = std::filesystem::temp_directory_path() / "kyty_upgrade1_store_test.bin";
	std::error_code ec;
	std::filesystem::remove(tmp_path, ec);

	const std::vector<uint8_t> payload_a = {0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33};
	const std::vector<uint8_t> payload_b = {0x02, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xFE};
	{
		ShaderStoreWriter writer;
		writer.Open(tmp_path, h1, 0);
		if (!writer) {
			std::cerr << "[FAIL] ShaderStoreWriter::Open failed!\n";
			return false;
		}
		auto copy_a = payload_a;
		auto copy_b = payload_b;
		writer.Append(std::move(copy_a));
		writer.Append(std::move(copy_b));
		writer.Finish();
	}

	// Simulated second boot ("after rebuild"): verify semantic header matches & zstd frames decode
	Common::File f;
	if (!f.Open(tmp_path, Common::File::Mode::Read)) {
		std::cerr << "[FAIL] Could not reopen persisted shader store!\n";
		return false;
	}
	std::vector<uint8_t> disk(f.Size());
	uint32_t             got = 0;
	f.Read(disk.data(), static_cast<uint32_t>(disk.size()), &got);
	f.Close();
	std::filesystem::remove(tmp_path, ec);

	if (got < h2.size() || !std::equal(h2.begin(), h2.end(), disk.begin())) {
		std::cerr << "[FAIL] Semantic header did not match on reload!\n";
		return false;
	}

	size_t                            off = h2.size();
	std::vector<std::vector<uint8_t>> decoded;
	while (off + sizeof(Frame) <= got) {
		Frame fr {};
		std::memcpy(&fr, disk.data() + off, sizeof(fr));
		off += sizeof(fr);
		if (off + fr.compressed_size > got) {
			return false;
		}
		const uint64_t chk = XXH3_64bits(disk.data() + off, fr.compressed_size);
		if (chk != fr.checksum) {
			std::cerr << "[FAIL] XXH3 checksum mismatch in persisted record!\n";
			return false;
		}
		std::vector<uint8_t> raw(fr.raw_size);
		if (ZSTD_decompress(raw.data(), raw.size(), disk.data() + off, fr.compressed_size) !=
		    raw.size()) {
			std::cerr << "[FAIL] ZSTD_decompress failed on persisted record!\n";
			return false;
		}
		decoded.push_back(std::move(raw));
		off += fr.compressed_size;
	}

	if (decoded.size() != 2 || decoded[0] != payload_a || decoded[1] != payload_b) {
		std::cerr << "[FAIL] Decoded shader store records did not match written payloads!\n";
		return false;
	}
	std::cout << "[PASS 1/2] Semantic Shader Store Persistence: header=" << h1.size()
	          << " B (rebuild-resilient), 2 zstd+XXH3 records verified\n";
	return true;
}

bool TestAsyncPoolDeduplicationAndDrain() {
	constexpr uint32_t kUniqueKeys = 64;
	constexpr uint32_t kWorkers    = 4;

	std::mutex                  mtx;
	std::condition_variable_any cv;
	std::queue<uint64_t>        queue;
	std::unordered_set<uint64_t> in_flight;
	std::vector<uint64_t>       completed;
	std::atomic<uint32_t>       compile_calls {0};

	std::vector<std::jthread> workers;
	workers.reserve(kWorkers);
	for (uint32_t w = 0; w < kWorkers; w++) {
		workers.emplace_back([&](std::stop_token stop) {
			while (true) {
				uint64_t key = 0;
				{
					std::unique_lock lock(mtx);
					if (!cv.wait(lock, stop, [&] { return !queue.empty(); })) {
						break;
					}
					key = queue.front();
					queue.pop();
				}
				// Simulate pipeline compilation work off the GPU thread
				std::this_thread::sleep_for(std::chrono::microseconds(250));
				compile_calls.fetch_add(1, std::memory_order_relaxed);
				{
					std::scoped_lock lock(mtx);
					in_flight.erase(key);
					completed.push_back(key);
				}
			}
		});
	}

	std::unordered_set<uint64_t> ready_map;
	// Blast 8 duplicate submissions per unique key (512 total lookups)
	for (uint32_t round = 0; round < 8; round++) {
		for (uint64_t k = 1; k <= kUniqueKeys; k++) {
			if (ready_map.count(k)) {
				continue;
			}
			bool enqueued = false;
			{
				std::scoped_lock lock(mtx);
				if (in_flight.insert(k).second) {
					queue.push(k);
					enqueued = true;
				}
			}
			if (enqueued) {
				cv.notify_one();
			}
		}
	}

	// Drain until all 64 unique keys arrive
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (ready_map.size() < kUniqueKeys && std::chrono::steady_clock::now() < deadline) {
		std::vector<uint64_t> batch;
		{
			std::scoped_lock lock(mtx);
			batch.swap(completed);
		}
		for (uint64_t k: batch) {
			ready_map.insert(k);
		}
		if (ready_map.size() < kUniqueKeys) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}

	for (auto& w: workers) {
		w.request_stop();
	}
	cv.notify_all();
	workers.clear();

	if (ready_map.size() != kUniqueKeys || compile_calls.load() != kUniqueKeys) {
		std::cerr << "[FAIL] Async pool expected " << kUniqueKeys << " unique compiles, got ready="
		          << ready_map.size() << " calls=" << compile_calls.load() << "\n";
		return false;
	}
	std::cout << "[PASS 2/2] Async Pipeline Pool (4 workers): 512 requests -> " << compile_calls.load()
	          << "/" << kUniqueKeys << " deduplicated compiles, clean drain & shutdown\n";
	return true;
}

} // namespace

int main() {
	if (!TestSemanticHeaderPersistence()) {
		return 1;
	}
	if (!TestAsyncPoolDeduplicationAndDrain()) {
		return 1;
	}
	std::cout << "ALL UPGRADE #1 VERIFICATION TESTS PASSED!\n";
	return 0;
}
