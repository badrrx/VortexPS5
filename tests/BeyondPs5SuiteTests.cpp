#include "common/gameProfile.h"
#include "common/trophyTracker.h"
#include "graphics/presentation/presentUpscaler.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

// Minimal GraphicContext VMA stubs for standalone unit test linkage (only CreatePresentUpscalerComputePipeline is exercised).
namespace Libs::Graphics {
bool GraphicContext::CreateImage(const vk::ImageCreateInfo&, VulkanImage&) { return false; }
void GraphicContext::DeleteImage(VulkanImage&) {}
} // namespace Libs::Graphics

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestVulkanCasFsrComputePipeline() {
	const auto spv = Libs::Graphics::GetPresentUpscalerSpirv();
	Check(spv.size() > 16, "SPIR-V binary for gpu_present_fsr_cas.comp must be non-empty");
	Check(spv[0] == 0x07230203u, "SPIR-V binary must start with valid SPIR-V magic 0x07230203");

	vk::detail::DynamicLoader dl;
	auto vkGetInstanceProcAddr = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	if (vkGetInstanceProcAddr == nullptr) {
		::printf("[INFO] Vulkan loader not available in environment; verified SPIR-V header (%zu words)\n",
		         spv.size());
		return;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

	vk::ApplicationInfo app_info {};
	app_info.pApplicationName = "BeyondPs5SuiteTests";
	app_info.apiVersion       = VK_API_VERSION_1_3;

	vk::InstanceCreateInfo inst_info {};
	inst_info.pApplicationInfo = &app_info;

	vk::Instance instance = nullptr;
	if (vk::createInstance(&inst_info, nullptr, &instance) != vk::Result::eSuccess ||
	    instance == nullptr) {
		::printf("[INFO] Headless Vulkan instance unavailable; verified SPIR-V header (%zu words)\n",
		         spv.size());
		return;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

	uint32_t gpu_count = 0;
	(void)instance.enumeratePhysicalDevices(&gpu_count, nullptr);
	if (gpu_count == 0) {
		instance.destroy(nullptr);
		::printf("[INFO] No Vulkan physical device; verified SPIR-V header (%zu words)\n", spv.size());
		return;
	}

	std::vector<vk::PhysicalDevice> gpus(gpu_count);
	(void)instance.enumeratePhysicalDevices(&gpu_count, gpus.data());

	vk::PhysicalDevice chosen_gpu   = nullptr;
	uint32_t           compute_qfam = UINT32_MAX;
	for (const auto& gpu: gpus) {
		uint32_t qcount = 0;
		gpu.getQueueFamilyProperties(&qcount, nullptr);
		std::vector<vk::QueueFamilyProperties> qprops(qcount);
		gpu.getQueueFamilyProperties(&qcount, qprops.data());
		for (uint32_t i = 0; i < qcount; ++i) {
			if ((qprops[i].queueFlags & vk::QueueFlagBits::eCompute) != vk::QueueFlags {}) {
				chosen_gpu   = gpu;
				compute_qfam = i;
				break;
			}
		}
		if (chosen_gpu != nullptr) break;
	}
	Check(chosen_gpu != nullptr, "Expected a Vulkan device with a compute queue family");

	float                     priority = 1.0f;
	vk::DeviceQueueCreateInfo qinfo {};
	qinfo.queueFamilyIndex = compute_qfam;
	qinfo.queueCount       = 1;
	qinfo.pQueuePriorities = &priority;

	vk::DeviceCreateInfo dev_info {};
	dev_info.queueCreateInfoCount = 1;
	dev_info.pQueueCreateInfos    = &qinfo;

	vk::Device device = nullptr;
	const auto dev_res = chosen_gpu.createDevice(&dev_info, nullptr, &device);
	Check(dev_res == vk::Result::eSuccess && device != nullptr,
	      "Failed to create headless Vulkan 1.3 device for compute pipeline test");

	if (device != nullptr) {
		VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

		vk::DescriptorSetLayout desc_layout = nullptr;
		vk::PipelineLayout      pipe_layout = nullptr;
		vk::Pipeline            pipeline    = nullptr;
		const bool              ok = Libs::Graphics::CreatePresentUpscalerComputePipeline(
            device, false, &desc_layout, &pipe_layout, &pipeline);
		Check(ok && pipeline != nullptr,
		      "CreatePresentUpscalerComputePipeline must compile CAS/FSR1 SPIR-V into a valid VkPipeline");

		if (pipeline != nullptr) device.destroyPipeline(pipeline, nullptr);
		if (pipe_layout != nullptr) device.destroyPipelineLayout(pipe_layout, nullptr);
		if (desc_layout != nullptr) device.destroyDescriptorSetLayout(desc_layout, nullptr);
		device.destroy(nullptr);
	}
	instance.destroy(nullptr);

	::printf("[PASS] Vulkan 1.3 CAS/FSR 1.0 Compute Shader SPIR-V (%zu words) & Pipeline Creation\n",
	         spv.size());
}

void TestPatchEngineAndGameProfiles() {
	const auto tmp_dir = std::filesystem::temp_directory_path() / "kyty_prospero_upgrade5_test";
	std::error_code ec;
	std::filesystem::remove_all(tmp_dir, ec);
	std::filesystem::create_directories(tmp_dir / "patches", ec);
	std::filesystem::create_directories(tmp_dir / "profiles", ec);

	const auto patch_path = Common::PatchBundlePath("PPSA99999", tmp_dir / "patches");
	{
		std::ofstream ofs(patch_path, std::ios::binary | std::ios::trunc);
		ofs << R"({
  "titleId": "PPSA99999",
  "version": "01.000.000",
  "patches": [
    {
      "id": "unlock_120fps",
      "name": "120 FPS Unlock",
      "category": "FPS",
      "enabled": true,
      "fpsCap": 120,
      "writes": [
        { "offset": "0x100", "original": "74 05 89 C0", "patched": "90 90 89 C0" }
      ]
    },
    {
      "id": "no_motion_blur",
      "name": "Disable Motion Blur",
      "category": "Visual",
      "enabled": false,
      "writes": [
        { "offset": "0x200", "original": "0F 85 10 00", "patched": "90 E9 10 00" }
      ]
    }
  ]
})";
	}

	Common::GamePatchBundle bundle;
	std::string             err;
	Check(Common::LoadPatchBundle(patch_path, bundle, &err), "LoadPatchBundle should succeed");
	Check(bundle.title_id == "PPSA99999", "Parsed TitleID must match PPSA99999");
	Check(bundle.patches.size() == 2, "Expected 2 patches in bundle");

	// Prepare synthetic guest executable buffer
	std::vector<uint8_t> guest_mem(4096, 0xCC);
	guest_mem[0x100] = 0x74;
	guest_mem[0x101] = 0x05;
	guest_mem[0x102] = 0x89;
	guest_mem[0x103] = 0xC0;
	guest_mem[0x200] = 0x0F;
	guest_mem[0x201] = 0x85;
	guest_mem[0x202] = 0x10;
	guest_mem[0x203] = 0x00;
	const auto original_snapshot = guest_mem;

	std::vector<Common::AppliedPatchBackup> backups;
	uint32_t                                fps_cap = 0;
	const std::vector<std::string>          enable_both {"unlock_120fps", "no_motion_blur"};
	Check(Common::ApplyPatchBundleToBuffer(bundle, enable_both, guest_mem, backups, &fps_cap, &err),
	      "ApplyPatchBundleToBuffer should succeed");
	Check(fps_cap == 120u, "FPS cap override should be 120");
	Check(guest_mem[0x100] == 0x90 && guest_mem[0x101] == 0x90,
	      "120 FPS patch bytes must be written");
	Check(guest_mem[0x200] == 0x90 && guest_mem[0x201] == 0xE9,
	      "Motion blur patch bytes must be written");

	// Re-applying over already-patched bytes must fail safely without corrupting memory
	std::vector<Common::AppliedPatchBackup> dummy_backups;
	Check(!Common::ApplyPatchBundleToBuffer(bundle, enable_both, guest_mem, dummy_backups, nullptr,
	                                        &err),
	      "Re-applying over mismatched original bytes must fail safely");

	// Clean rollback must restore exact original buffer
	Common::RollbackPatchBackups(guest_mem, backups);
	Check(guest_mem == original_snapshot, "RollbackPatchBackups must restore exact original bytes");

	// Per-game profile save/load round-trip
	Common::GameProfile prof {};
	prof.title_id           = "PPSA99999";
	prof.resolution_scale   = 2.0f;
	prof.upscaler_mode      = Config::PresentUpscalerMode::FSR1;
	prof.upscaler_sharpness = 0.85f;
	prof.fps_cap            = 120;
	prof.guest_memory_mb    = 8192;
	prof.enabled_patches    = enable_both;
	Check(Common::SaveGameProfile(prof, tmp_dir / "profiles"), "SaveGameProfile must succeed");

	const auto loaded = Common::LoadGameProfile("PPSA99999", tmp_dir / "profiles");
	Check(loaded.title_id == "PPSA99999", "Loaded profile titleId match");
	Check(std::fabs(loaded.resolution_scale - 2.0f) < 0.001f, "Loaded resolution_scale == 2.0");
	Check(loaded.upscaler_mode == Config::PresentUpscalerMode::FSR1, "Loaded upscaler == FSR1");
	Check(std::fabs(loaded.upscaler_sharpness - 0.85f) < 0.001f, "Loaded sharpness == 0.85");
	Check(loaded.fps_cap == 120u && loaded.guest_memory_mb == 8192u, "Loaded fps/memory match");
	Check(loaded.enabled_patches.size() == 2, "Loaded enabled_patches match");

	std::filesystem::remove_all(tmp_dir, ec);
	::printf("[PASS] Per-Game JSON Patch Engine, Memory Rollback & Profile Round-Trip\n");
}

void TestTrophyPersistenceAndPopupQueue() {
	const auto tmp_dir = std::filesystem::temp_directory_path() / "kyty_prospero_trophies_test";
	std::error_code ec;
	std::filesystem::remove_all(tmp_dir, ec);

	Check(Common::RecordTrophyUnlock("PPSA99999", 1, "First Blood", "Defeat the tutorial boss.",
	                                 Common::TrophyGrade::Bronze, 10, tmp_dir),
	      "First unlock should return true");
	Check(!Common::RecordTrophyUnlock("PPSA99999", 1, "First Blood", "Defeat the tutorial boss.",
	                                  Common::TrophyGrade::Bronze, 10, tmp_dir),
	      "Duplicate unlock should be idempotent and return false");
	Check(Common::RecordTrophyUnlock("PPSA99999", 2, "Silver Sentinel", "Clear Chapter 3.",
	                                 Common::TrophyGrade::Silver, 10, tmp_dir),
	      "Silver unlock should return true");
	Check(Common::RecordTrophyUnlock("PPSA99999", 3, "Golden Legend", "Finish on Hard mode.",
	                                 Common::TrophyGrade::Gold, 10, tmp_dir),
	      "Gold unlock should return true");
	Check(Common::RecordTrophyUnlock("PPSA99999", 4, "Master of Prospero", "Earn all trophies.",
	                                 Common::TrophyGrade::Platinum, 10, tmp_dir),
	      "Platinum unlock should return true");

	const auto reloaded = Common::LoadTrophyStore("PPSA99999", tmp_dir);
	Check(reloaded.UnlockedCount() == 4u, "Reloaded store must have 4 unlocked trophies");
	Check(reloaded.CompletionPercentage() == 40u, "CompletionPercentage must be 40% (4/10)");

	// Test deterministic PS5 trophy popup state machine
	Common::TrophyPopupQueue queue;
	Check(queue.Snapshot().phase == Common::TrophyPopupPhase::Hidden, "Initial phase Hidden");

	queue.Push({"First Blood", "Defeat the tutorial boss.", Common::TrophyGrade::Bronze});
	queue.Push({"Master of Prospero", "Earn all trophies.", Common::TrophyGrade::Platinum});
	Check(queue.PendingCount() == 2u, "2 trophies queued");

	queue.Tick(0.15f, 2.0f);
	auto s = queue.Snapshot();
	Check(s.phase == Common::TrophyPopupPhase::SlideIn && s.slide_factor > 0.0f &&
	          s.slide_factor < 1.0f,
	      "Mid-SlideIn should have partial slide factor");
	Check(s.event.title == "First Blood", "Active popup should be First Blood");

	queue.Tick(0.20f, 2.0f);
	s = queue.Snapshot();
	Check(s.phase == Common::TrophyPopupPhase::Hold && s.slide_factor == 1.0f && s.alpha == 1.0f,
	      "After SlideIn duration should be in Hold at full visibility");

	queue.Tick(2.05f, 2.0f);
	s = queue.Snapshot();
	Check(s.phase == Common::TrophyPopupPhase::SlideOut, "After Hold should enter SlideOut");

	queue.Tick(0.40f, 2.0f);
	s = queue.Snapshot();
	Check(s.phase == Common::TrophyPopupPhase::Hidden, "After SlideOut should return to Hidden");

	// Next tick should immediately pop the second queued trophy
	queue.Tick(0.10f, 2.0f);
	s = queue.Snapshot();
	Check(s.phase == Common::TrophyPopupPhase::SlideIn && s.event.title == "Master of Prospero" &&
	          s.event.grade == Common::TrophyGrade::Platinum,
	      "Second queued Platinum trophy should slide in cleanly");

	std::filesystem::remove_all(tmp_dir, ec);
	::printf("[PASS] Trophy JSON Persistence & Animated Popup State Machine\n");
}

} // namespace

int RunAllTests() {
	TestVulkanCasFsrComputePipeline();
	TestPatchEngineAndGameProfiles();
	TestTrophyPersistenceAndPopupQueue();

	if (g_failures != 0) {
		::fprintf(stderr, "BeyondPs5SuiteTests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}
	::printf("BeyondPs5SuiteTests: ALL 3/3 SUITES PASSED!\n");
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
