#include "common/bootTelemetry.h"
#include "common/nidTriage.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
			std::exit(1);                                                                          \
		}                                                                                          \
	} while (0)

bool TestMultiThreadedLinkerAndSymbols() {
	// Construct RuntimeLinker on a non-main worker thread to prove the main-thread trap is gone.
	std::unique_ptr<Loader::RuntimeLinker> linker;
	std::thread                            init_thread([&] {
        linker = std::make_unique<Loader::RuntimeLinker>();
        CHECK(linker->GetEntry() == 0);
        CHECK(linker->GetProcParam() == 0);
        linker->RelocateAll();
    });
	init_thread.join();
	CHECK(linker != nullptr);

	auto* symdb = linker->Symbols();
	CHECK(symdb != nullptr);

	constexpr int            kWorkers       = 8;
	constexpr int            kSymsPerWorker = 64;
	std::atomic<int>         failures {0};
	std::vector<std::thread> threads;
	threads.reserve(kWorkers);

	for (int t = 0; t < kWorkers; ++t) {
		threads.emplace_back([&, t] {
			std::vector<const Loader::SymbolRecord*> saved_ptrs;
			saved_ptrs.reserve(kSymsPerWorker);

			for (int i = 0; i < kSymsPerWorker; ++i) {
				Loader::SymbolResolve sym {};
				sym.name                 = "NID_T" + std::to_string(t) + "_" + std::to_string(i);
				sym.library              = "libSceWorkerPrx";
				sym.library_version      = 1;
				sym.module               = "libSceWorkerPrx";
				sym.module_version_major = 1;
				sym.module_version_minor = 0;
				sym.type                 = Loader::SymbolType::Func;

				const uint64_t vaddr =
				    0x8000'0000ULL + static_cast<uint64_t>(t * 0x10000 + i * 16);
				symdb->Add(sym, vaddr, "worker_fn_" + std::to_string(i));

				const auto* rec = symdb->Find(sym);
				if (rec == nullptr || rec->vaddr != vaddr) {
					failures.fetch_add(1, std::memory_order_relaxed);
				} else {
					saved_ptrs.push_back(rec);
				}
			}

			// Verify pointer stability after concurrent insertions from all threads.
			for (size_t i = 0; i < saved_ptrs.size(); ++i) {
				const uint64_t expected =
				    0x8000'0000ULL + static_cast<uint64_t>(t * 0x10000 + static_cast<int>(i) * 16);
				if (saved_ptrs[i]->vaddr != expected) {
					failures.fetch_add(1, std::memory_order_relaxed);
				}
			}

			// Concurrent RuntimeLinker queries from worker threads must not trap.
			(void)linker->GetEntry();
			(void)linker->GetProcParam();
			linker->RelocateAll();
		});
	}

	for (auto& th: threads) {
		th.join();
	}
	CHECK(failures.load() == 0);

	// Verify lookup by NID from main thread after all workers completed.
	const auto* found = symdb->FindByNid("NID_T7_63", Loader::SymbolType::Func);
	CHECK(found != nullptr);
	CHECK(found->vaddr == 0x8000'0000ULL + static_cast<uint64_t>(7 * 0x10000 + 63 * 16));

	std::printf("[PASS] Suite 1: Multi-Threaded RuntimeLinker & SymbolDatabase Concurrent Resolution\n");
	return true;
}

bool TestPersistentPipelineCacheWiring() {
	const auto cache_path =
	    std::filesystem::temp_directory_path() / "kyty_bt18_cache" / "PPSA99018.vkcache";
	std::error_code ec;
	std::filesystem::remove_all(cache_path.parent_path(), ec);

	const std::array<uint8_t, 16> uuid = {
	    0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
	    0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F,
	};
	std::vector<uint8_t> payload(4096);
	for (size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xFFu);
	}

	CHECK(Libs::Graphics::SaveVkPipelineCacheBlob(cache_path, 0x1002u, 0x744Cu, uuid, payload));
	CHECK(std::filesystem::exists(cache_path));

	std::vector<uint8_t> loaded;
	CHECK(Libs::Graphics::LoadVkPipelineCacheBlob(cache_path, 0x1002u, 0x744Cu, uuid, loaded));
	CHECK(loaded == payload);

	// Reject mismatched GPU device ID.
	std::vector<uint8_t> rejected;
	CHECK(!Libs::Graphics::LoadVkPipelineCacheBlob(cache_path, 0x1002u, 0x9999u, uuid, rejected));

	// Reject corrupted byte in payload.
	{
		std::fstream fs(cache_path, std::ios::binary | std::ios::in | std::ios::out);
		CHECK(fs.is_open());
		fs.seekp(-1, std::ios::end);
		char last = 0;
		fs.read(&last, 1);
		fs.seekp(-1, std::ios::end);
		last ^= static_cast<char>(0xFF);
		fs.write(&last, 1);
	}
	CHECK(!Libs::Graphics::LoadVkPipelineCacheBlob(cache_path, 0x1002u, 0x744Cu, uuid, rejected));

	std::filesystem::remove_all(cache_path.parent_path(), ec);
	std::printf("[PASS] Suite 2: Persistent VkPipelineCache Disk Round-Trip & Invalidation\n");
	return true;
}

bool TestLiveBootTelemetryLifecycle() {
	auto& telemetry = Common::BootTelemetry::Global();
	telemetry.Reset();
	CHECK(!telemetry.HasActiveSession());

	const auto out_json =
	    std::filesystem::temp_directory_path() / "kyty_bt18_telemetry" / "boot_telemetry_PPSA99018.json";
	std::error_code ec;
	std::filesystem::remove_all(out_json.parent_path(), ec);

	// Inactive session must not emit stray files.
	CHECK(!telemetry.FlushIfActive(out_json));
	CHECK(!std::filesystem::exists(out_json));

	// Begin active live session and hammer hot-path milestones from multiple threads.
	Common::NidTriageTracker::Global().Clear();
	telemetry.BeginSession("PPSA99018", "/app0/eboot.bin");
	CHECK(telemetry.HasActiveSession());

	telemetry.MarkElfLoaded();

	std::vector<std::thread> submitters;
	for (int t = 0; t < 4; ++t) {
		submitters.emplace_back([&] {
			for (int i = 0; i < 250; ++i) {
				telemetry.MarkFirstGpuSubmit();
				telemetry.MarkFirstFramePresented();
			}
			telemetry.RecordPipelineCreated(true);
			telemetry.RecordPipelineCreated(false);
			telemetry.RecordShaderCompiled(125);
		});
	}
	for (auto& th: submitters) {
		th.join();
	}

	Common::NidTriageTracker::Global().RecordCall("FakeNidBt18#libSceTestPrx#libSceTestPrx",
	                                              "eboot.bin");

	CHECK(telemetry.FlushIfActive(out_json));
	CHECK(std::filesystem::exists(out_json));

	std::ifstream ifs(out_json);
	CHECK(ifs.is_open());
	std::stringstream ss;
	ss << ifs.rdbuf();
	const std::string json = ss.str();

	CHECK(json.find("\"title_id\": \"PPSA99018\"") != std::string::npos);
	CHECK(json.find("\"elf_loaded\": true") != std::string::npos);
	CHECK(json.find("\"first_gpu_submit\": true") != std::string::npos);
	CHECK(json.find("\"first_frame_presented\": true") != std::string::npos);
	CHECK(json.find("\"pipelines_created\": 8") != std::string::npos);
	CHECK(json.find("\"pipeline_cache_hits\": 4") != std::string::npos);
	CHECK(json.find("\"shaders_compiled\": 4") != std::string::npos);
	CHECK(json.find("\"shader_compile_us\": 500") != std::string::npos);
	CHECK(json.find("\"unique_count\": 1") != std::string::npos);
	CHECK(json.find("\"total_calls\": 1") != std::string::npos);

	telemetry.Reset();
	std::filesystem::remove_all(out_json.parent_path(), ec);
	std::filesystem::remove("missing_nids_report.json", ec);

	std::printf("[PASS] Suite 3: Live BootTelemetry Lifecycle, Lock-Free Milestones & JSON Flush\n");
	return true;
}

} // namespace

static int RunAllTests() {
	Common::InitializeThreads();

	CHECK(TestMultiThreadedLinkerAndSymbols());
	CHECK(TestPersistentPipelineCacheWiring());
	CHECK(TestLiveBootTelemetryLifecycle());

	std::printf("ALL CoreBreakthrough18Tests PASSED (3/3 suites)\n");
	return 0;
}

#if defined(_WIN32)
int wmain() {
	return RunAllTests();
}
#else
int main() {
	return RunAllTests();
}
#endif
