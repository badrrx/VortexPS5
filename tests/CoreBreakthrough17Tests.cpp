// Core Breakthrough #17 Verification Suite (Phase 2):
//   1. Persistent Vulkan VkPipelineCache Disk Save/Load + AsyncPipelinePrewarmer Worker Pool
//   2. Deep-Swept libSceNgs2 & libSceAgcDriver SDK 9.x-10.x Edge-Case Hardening
//   3. Per-TitleID Live Boot Telemetry & Auto-Triage JSON Export

#include "common/bootTelemetry.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/nidTriage.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "libs/agc.h"
#include "libs/errno.h"
#include "libs/ngs2.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestPipelineDiskCacheAndAsyncPrewarmer() {
	using namespace Libs::Graphics;

	const auto cache_path =
	    std::filesystem::temp_directory_path() / "kyty_cb17_cache" / "PPSA01234.vkcache";
	std::error_code ec;
	std::filesystem::remove_all(cache_path.parent_path(), ec);

	const std::array<uint8_t, 16> uuid = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
	                                      0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01};
	const std::array<uint8_t, 16> bad_uuid = {0xff, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
	                                          0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01};
	std::vector<uint8_t>          blob(1024);
	for (size_t i = 0; i < blob.size(); ++i) {
		blob[i] = static_cast<uint8_t>((i * 37u + 13u) & 0xffu);
	}

	// 1. Atomic disk save & verified reload
	Check(SaveVkPipelineCacheBlob(cache_path, 0x10de, 0x2684, uuid, blob),
	      "SaveVkPipelineCacheBlob writes cache atomically");
	std::vector<uint8_t> loaded;
	Check(LoadVkPipelineCacheBlob(cache_path, 0x10de, 0x2684, uuid, loaded) && loaded == blob,
	      "LoadVkPipelineCacheBlob roundtrips exact 1024-byte pipeline cache blob");

	// 2. Wrong driver UUID must be rejected cleanly
	std::vector<uint8_t> rejected;
	Check(!LoadVkPipelineCacheBlob(cache_path, 0x10de, 0x2684, bad_uuid, rejected) &&
	          rejected.empty(),
	      "LoadVkPipelineCacheBlob rejects mismatched driver UUID");

	// 3. AsyncPipelinePrewarmer concurrent job execution
	AsyncPipelinePrewarmer prewarmer(2);
	std::atomic<uint32_t>  sum {0};
	for (uint32_t i = 1; i <= 32; ++i) {
		prewarmer.Enqueue([i, &sum] { sum.fetch_add(i, std::memory_order_relaxed); });
	}
	prewarmer.WaitIdle();
	Check(prewarmer.CompletedCount() == 32 && sum.load() == (32u * 33u) / 2u,
	      "AsyncPipelinePrewarmer executed all 32 concurrent jobs accurately");

	std::filesystem::remove_all(cache_path.parent_path(), ec);

	std::printf("[PASS] Suite 1: Persistent VkPipelineCache & AsyncPipelinePrewarmer\n");
}

void TestNgs2AndAgcDeepHardening() {
	using namespace Libs::Audio::Ngs2;
	using namespace Libs::Graphics::Gen5;

	// 1. NGS2 previously fatal null-handle / null-info traps -> NGS2_ERROR_*
	uint8_t info_buf[32] {};
	Check(Ngs2VoiceRunCommands(0, nullptr, 1) < 0,
	      "Ngs2VoiceRunCommands(handle=0) returns error cleanly (was fatal!)");
	Check(Ngs2VoiceQueryInfo(0, 0x4001, info_buf, sizeof(info_buf)) < 0,
	      "Ngs2VoiceQueryInfo(handle=0) returns error cleanly (was fatal!)");
	Check(Ngs2VoiceGetPortInfo(0, 0, info_buf, sizeof(info_buf)) < 0,
	      "Ngs2VoiceGetPortInfo(handle=0) returns error cleanly (was fatal!)");

	// 2. AGC previously fatal null-pointer & bad-header traps -> error code
	Check(AgcCreateShader(nullptr, nullptr, nullptr) < 0,
	      "AgcCreateShader(nullptr) returns error cleanly (was fatal!)");
	Check(AgcCreateInterpolantMapping(nullptr, nullptr, nullptr) < 0,
	      "AgcCreateInterpolantMapping(nullptr) returns error cleanly (was fatal!)");
	Check(AgcCreateInterpolantMapping2(nullptr, nullptr, nullptr) < 0,
	      "AgcCreateInterpolantMapping2(nullptr) returns error cleanly (was fatal!)");

	std::printf("[PASS] Suite 2: Deep-Swept libSceNgs2 & libSceAgcDriver Edge-Case Hardening\n");
}

void TestBootTelemetryHarness() {
	auto& telemetry = Common::BootTelemetry::Global();
	telemetry.Reset();
	telemetry.BeginSession("PPSA01234", "/app0/eboot.bin");
	telemetry.MarkElfLoaded();
	telemetry.MarkFirstGpuSubmit();
	telemetry.RecordShaderCompiled(4200);
	telemetry.RecordShaderCompiled(1800);
	telemetry.RecordPipelineCreated(false);
	telemetry.RecordPipelineCreated(true);
	telemetry.MarkFirstFramePresented();

	auto& nids = Common::NidTriageTracker::Global();
	nids.Clear();
	nids.RecordCall("ABC12345678#libSceTest#libSceTest", "eboot.bin");

	const auto snap = telemetry.Snapshot();
	Check(snap.title_id == "PPSA01234" && snap.elf_loaded && snap.first_gpu_submit &&
	          snap.first_frame_presented,
	      "BootTelemetry records all boot milestones");
	Check(snap.shaders_compiled == 2 && snap.shader_compile_us == 6000 &&
	          snap.pipelines_created == 2 && snap.pipeline_cache_hits == 1,
	      "BootTelemetry records shader & pipeline cache statistics");
	Check(snap.missing_unique_nids == 1 && snap.missing_nid_calls == 1,
	      "BootTelemetry integrates live NidTriageTracker counts");

	const auto out_path =
	    std::filesystem::temp_directory_path() / "kyty_cb17_boot_telemetry_PPSA01234.json";
	Check(telemetry.ExportJson(out_path), "BootTelemetry::ExportJson succeeds");

	std::ifstream ifs(out_path);
	std::string   json((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
	ifs.close();
	Check(json.find("\"title_id\": \"PPSA01234\"") != std::string::npos &&
	          json.find("\"first_frame_presented\": true") != std::string::npos &&
	          json.find("\"pipeline_cache_hits\": 1") != std::string::npos,
	      "Exported boot_telemetry JSON contains expected milestones & stats");

	std::error_code ec;
	std::filesystem::remove(out_path, ec);
	nids.Clear();

	std::printf("[PASS] Suite 3: Per-TitleID Live Boot Telemetry & Auto-Triage JSON Export\n");
}

} // namespace

#if defined(_WIN32)
int wmain() {
#else
int main() {
#endif
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});

	TestPipelineDiskCacheAndAsyncPrewarmer();
	TestNgs2AndAgcDeepHardening();
	TestBootTelemetryHarness();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough17Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough17Tests PASSED (3/3 suites)\n");
	return 0;
}
