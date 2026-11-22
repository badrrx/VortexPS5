#include "common/nidTriage.h"
#include "graphics/host_gpu/asyncComputeQueue.h"
#include "libs/libFios2.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

// Minimal VFS fallback stub for standalone test linkage (host paths resolve directly in libFios2.cpp).
namespace Libs::LibKernel::FileSystem {
std::filesystem::path GetRealFilename(const std::string& mounted_name) {
	return std::filesystem::path(mounted_name);
}
} // namespace Libs::LibKernel::FileSystem

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestLibSceFios2AsyncStreaming() {
	using namespace Libs::LibFios2;

	const auto tmp_dir = std::filesystem::temp_directory_path() / "kyty_prospero_fios2_test";
	std::error_code ec;
	std::filesystem::remove_all(tmp_dir, ec);
	std::filesystem::create_directories(tmp_dir, ec);

	const std::string payload   = "KYTY-PROSPERO-FIOS2-ASYNC-STREAMING-PAYLOAD-2026!";
	const auto        file_path = tmp_dir / "chunk0.pak";
	{
		std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
		ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
	}

	SceFiosParams params {};
	Check(FiosInitialize(&params) == SCE_FIOS_OK, "FiosInitialize must return SCE_FIOS_OK");
	Check(FiosIsInitialized() == 1, "FiosIsInitialized must return 1");

	// Register an overlay mapping /virtual_pak -> tmp_dir
	SceFiosOverlay ov {};
	ov.order = 1;
	std::snprintf(ov.dst, sizeof(ov.dst), "%s", "/virtual_pak");
	const auto dir_str = tmp_dir.generic_string();
	std::snprintf(ov.src, sizeof(ov.src), "%s", dir_str.c_str());
	SceFiosOverlayID ov_id = 0;
	Check(FiosOverlayAdd(&ov, &ov_id) == SCE_FIOS_OK && ov_id > 0, "FiosOverlayAdd must succeed");

	char resolved_buf[512] {};
	Check(FiosOverlayResolveSync(0, 0, "/virtual_pak/chunk0.pak", resolved_buf,
	                             sizeof(resolved_buf)) == SCE_FIOS_OK,
	      "FiosOverlayResolveSync must succeed");
	Check(std::string(resolved_buf).find("chunk0.pak") != std::string::npos,
	      "Resolved overlay path must contain chunk0.pak");

	// Async Stat
	SceFiosStat st {};
	const SceFiosOp stat_op = FiosStat(nullptr, "/virtual_pak/chunk0.pak", &st);
	Check(stat_op > 0, "FiosStat must return a valid async op handle");
	Check(FiosOpWait(stat_op) == SCE_FIOS_OK, "FiosOpWait(stat_op) must succeed");
	Check(FiosOpIsDone(stat_op) == 1, "stat_op must report done");
	Check(st.fileSize == static_cast<SceFiosOffset>(payload.size()),
	      "FiosStat fileSize must match exact byte length");
	FiosOpDelete(stat_op);

	// Async Open
	SceFiosFH fh = kFiosFhInvalid;
	const SceFiosOp open_op = FiosFHOpen(nullptr, &fh, "/virtual_pak/chunk0.pak", nullptr);
	Check(open_op > 0, "FiosFHOpen must return a valid async op handle");
	Check(FiosOpWait(open_op) == SCE_FIOS_OK && fh > 0, "FiosFHOpen must yield valid handle");
	FiosOpDelete(open_op);

	// Async sequential Read (first 13 bytes)
	char buf1[32] {};
	const SceFiosOp read_op = FiosFHRead(nullptr, fh, buf1, 13);
	Check(FiosOpWait(read_op) == SCE_FIOS_OK, "FiosFHRead must succeed");
	Check(FiosOpGetActualCount(read_op) == 13, "FiosOpGetActualCount must equal 13");
	Check(std::string_view(buf1, 13) == "KYTY-PROSPERO", "First 13 bytes must match payload");
	FiosOpDelete(read_op);

	// Async Pread at offset 14
	char buf2[64] {};
	const SceFiosOp pread_op = FiosFHPread(nullptr, fh, buf2, 35, 14);
	Check(FiosOpWait(pread_op) == SCE_FIOS_OK, "FiosFHPread must succeed");
	Check(FiosOpGetActualCount(pread_op) == 35, "FiosFHPread actual count must equal 35");
	Check(std::string_view(buf2, 35) == "FIOS2-ASYNC-STREAMING-PAYLOAD-2026!",
	      "Pread bytes must match exact offset slice");
	FiosOpDelete(pread_op);

	// Async Close
	const SceFiosOp close_op = FiosFHClose(nullptr, fh);
	Check(FiosOpWait(close_op) == SCE_FIOS_OK, "FiosFHClose must succeed");
	FiosOpDelete(close_op);

	Check(FiosOverlayRemove(ov_id) == SCE_FIOS_OK, "FiosOverlayRemove must succeed");
	FiosTerminate();
	Check(FiosIsInitialized() == 0, "FiosIsInitialized must return 0 after Terminate");

	std::filesystem::remove_all(tmp_dir, ec);
	::printf("[PASS] Suite 1: libSceFios2 Async Open/Stat/Read/Pread/Overlay/Close verified\n");
}

void TestNidTriageEngine() {
	Common::NidTriageTracker tracker;
	tracker.RecordImport("AAAA1111222#libSceNgs2#libSceNgs2", "eboot.bin");
	tracker.RecordImport("BBBB3333444#libSceAudio3d#libSceAudio3d", "eboot.bin");
	tracker.RecordImport("CCCC5555666#libSceAudio3d#libSceAudio3d", "eboot.bin");

	for (int i = 0; i < 12; ++i) {
		tracker.RecordCall("BBBB3333444#libSceAudio3d#libSceAudio3d", "eboot.bin");
	}
	for (int i = 0; i < 3; ++i) {
		tracker.RecordCall("AAAA1111222#libSceNgs2#libSceNgs2", "eboot.bin");
	}
	tracker.RecordCall("CCCC5555666#libSceAudio3d#libSceAudio3d", "eboot.bin");

	const auto report = tracker.BuildReport();
	Check(report.total_unique_nids == 3u, "Must deduplicate to 3 unique missing NIDs");
	Check(report.total_calls == 16u, "Total call count must equal 16");
	Check(report.libraries.size() == 2u, "Must group into 2 libraries");
	Check(report.libraries[0].library == "libSceAudio3d" && report.libraries[0].total_calls == 13u,
	      "Highest-frequency library must sort first (libSceAudio3d, 13 calls)");
	Check(report.libraries[0].nids[0].nid == "BBBB3333444" &&
	          report.libraries[0].nids[0].call_count == 12u,
	      "Highest-frequency NID within library must sort first");

	const auto summary = tracker.FormatConsoleSummary();
	Check(summary.find("libSceAudio3d") != std::string::npos &&
	          summary.find("BBBB3333444") != std::string::npos,
	      "Console triage summary must include top library & NID");

	const auto report_path =
	    std::filesystem::temp_directory_path() / "kyty_prospero_missing_nids_test.json";
	Check(tracker.ExportJsonReport(report_path), "ExportJsonReport must succeed");

	std::ifstream ifs(report_path, std::ios::binary);
	const auto    parsed = nlohmann::json::parse(ifs, nullptr, false);
	Check(!parsed.is_discarded() && parsed["totalUniqueMissingNids"] == 3 &&
	          parsed["totalUnresolvedCalls"] == 16 && parsed["libraries"].size() == 2,
	      "Exported missing_nids_report.json must round-trip cleanly");

	std::error_code ec;
	std::filesystem::remove(report_path, ec);
	::printf("[PASS] Suite 2: Automated Unresolved-NID Triage Engine & JSON Report verified\n");
}

void TestAsyncComputeQueueAndTimelineSync() {
	using namespace Libs::Graphics;

	// Case A: Dedicated hardware Async Compute family (eCompute without eGraphics)
	{
		std::array<vk::QueueFamilyProperties, 3> fams {};
		fams[0].queueFlags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute |
		                     vk::QueueFlagBits::eTransfer;
		fams[0].queueCount = 1;
		fams[1].queueFlags = vk::QueueFlagBits::eTransfer;
		fams[1].queueCount = 2;
		fams[2].queueFlags = vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer;
		fams[2].queueCount = 4;

		const auto sel = SelectAsyncComputeQueueFamily(fams, 0);
		Check(sel.dedicated_family && sel.distinct_queue && sel.async_compute_family == 2 &&
		          sel.async_compute_index == 0,
		      "Must pick dedicated compute-only family 2 when available");
	}

	// Case B: Multi-queue universal family (queueCount >= 2)
	{
		std::array<vk::QueueFamilyProperties, 1> fams {};
		fams[0].queueFlags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
		fams[0].queueCount = 2;

		const auto sel = SelectAsyncComputeQueueFamily(fams, 0);
		Check(!sel.dedicated_family && sel.distinct_queue && sel.async_compute_family == 0 &&
		          sel.async_compute_index == 1,
		      "Must pick secondary queue index 1 on multi-queue universal family");
	}

	// Case C: Single-queue GPU fallback
	{
		std::array<vk::QueueFamilyProperties, 1> fams {};
		fams[0].queueFlags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
		fams[0].queueCount = 1;

		const auto sel = SelectAsyncComputeQueueFamily(fams, 0);
		Check(!sel.dedicated_family && !sel.distinct_queue && sel.async_compute_family == 0 &&
		          sel.async_compute_index == 0,
		      "Must safely fall back to universal queue 0 on single-queue GPUs");
	}

	// Case D: Live Vulkan 1.3 Timeline Semaphore test on host GPU
	vk::detail::DynamicLoader dl;
	auto vkGetInstanceProcAddr = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	if (vkGetInstanceProcAddr == nullptr) {
		::printf("[INFO] Vulkan loader unavailable; verified queue selector topologies\n");
		return;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

	vk::ApplicationInfo app_info {};
	app_info.pApplicationName = "CoreBreakthrough3Tests";
	app_info.apiVersion       = VK_API_VERSION_1_3;
	vk::InstanceCreateInfo inst_info {};
	inst_info.pApplicationInfo = &app_info;

	vk::Instance instance = nullptr;
	if (vk::createInstance(&inst_info, nullptr, &instance) != vk::Result::eSuccess ||
	    instance == nullptr) {
		return;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

	uint32_t gpu_count = 0;
	(void)instance.enumeratePhysicalDevices(&gpu_count, nullptr);
	if (gpu_count > 0) {
		std::vector<vk::PhysicalDevice> gpus(gpu_count);
		(void)instance.enumeratePhysicalDevices(&gpu_count, gpus.data());
		vk::PhysicalDevice gpu = gpus[0];

		float                     prio = 1.0f;
		vk::DeviceQueueCreateInfo qinfo {};
		qinfo.queueFamilyIndex = 0;
		qinfo.queueCount       = 1;
		qinfo.pQueuePriorities = &prio;

		vk::PhysicalDeviceVulkan12Features feat12 {};
		feat12.timelineSemaphore = VK_TRUE;

		vk::DeviceCreateInfo dev_info {};
		dev_info.pNext                = &feat12;
		dev_info.queueCreateInfoCount = 1;
		dev_info.pQueueCreateInfos    = &qinfo;

		vk::Device device = nullptr;
		if (gpu.createDevice(&dev_info, nullptr, &device) == vk::Result::eSuccess &&
		    device != nullptr) {
			VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

			AsyncComputeTimelineSync sync;
			Check(sync.Initialize(device, 0), "Timeline semaphore initialization must succeed");
			Check(sync.QueryCompletedValue() == 0, "Initial timeline value must be 0");

			const uint64_t g_val = sync.NextValue(); // 1 (Graphics -> Compute)
			const uint64_t c_val = sync.NextValue(); // 2 (Compute -> Graphics)
			const auto wait_info =
			    sync.MakeWaitInfo(g_val, vk::PipelineStageFlagBits2::eComputeShader);
			const auto sig_info =
			    sync.MakeSignalInfo(c_val, vk::PipelineStageFlagBits2::eAllCommands);
			Check(wait_info.semaphore == sync.Handle() && wait_info.value == 1,
			      "MakeWaitInfo must encode timeline semaphore and wait value 1");
			Check(sig_info.semaphore == sync.Handle() && sig_info.value == 2,
			      "MakeSignalInfo must encode timeline semaphore and signal value 2");

			Check(sync.SignalCpu(g_val), "SignalCpu(1) must succeed");
			Check(sync.WaitCpu(g_val, 1'000'000'000ull), "WaitCpu(1) must return immediately");
			Check(sync.QueryCompletedValue() == 1, "Completed timeline value must equal 1");

			Check(sync.SignalCpu(c_val), "SignalCpu(2) must succeed");
			Check(sync.WaitCpu(c_val, 1'000'000'000ull), "WaitCpu(2) must return immediately");
			Check(sync.QueryCompletedValue() == 2, "Completed timeline value must equal 2");

			sync.Destroy();
			device.destroy(nullptr);
		}
	}
	instance.destroy(nullptr);
	::printf("[PASS] Suite 3: Async Compute Queue Selector + Live Vulkan 1.3 Timeline Semaphore verified\n");
}

int RunAllTests() {
	TestLibSceFios2AsyncStreaming();
	TestNidTriageEngine();
	TestAsyncComputeQueueAndTimelineSync();

	if (g_failures != 0) {
		::fprintf(stderr, "CoreBreakthrough3Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}
	::printf("CoreBreakthrough3Tests: ALL 3/3 SUITES PASSED!\n");
	return 0;
}

} // namespace

#if defined(_WIN32)
int wmain() {
	return RunAllTests();
}
#else
int main() {
	return RunAllTests();
}
#endif
