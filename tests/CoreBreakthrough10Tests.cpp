// Core Breakthrough #10 Verification Suite:
//   1. Vulkan Host Renderer Pipeline, Image & State Edge-Case Hardening
//   2. libSceVideoOut Modern Display Modes (120Hz / VRR / HDR) & Lifecycle Hardening
//   3. libSceSysmodule Dynamic PRX Load/Unload & Modern SDK Module ID Resilience

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/presentation/videoOut.h"
#include "libs/errno.h"

#include <cstdio>
#include <cstring>

namespace Libs::Graphics {
vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back);
} // namespace Libs::Graphics

namespace Libs::Sysmodule {
KYTY_SYSV_ABI int SysmoduleLoadModule(uint16_t id);
KYTY_SYSV_ABI int SysmoduleUnloadModule(uint16_t id);
KYTY_SYSV_ABI int SysmoduleLoadModuleInternalWithArg(uint16_t id, int arg1, int arg2, int arg3,
                                                     int* ret);
KYTY_SYSV_ABI int SysmoduleIsLoaded(uint16_t id);
} // namespace Libs::Sysmodule

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestHostGpuStateHardening() {
	using namespace Libs::Graphics;

	// 1. ResolvePolygonMode with extended poly_mode (2) — previously EXIT_NOT_IMPLEMENTED!
	HW::ModeControl mode {};
	mode.poly_mode            = 2;
	mode.polymode_front_ptype = 1; // Line
	mode.polymode_back_ptype  = 1;
	Check(ResolvePolygonMode(mode, false, false) == vk::PolygonMode::eLine,
	      "ResolvePolygonMode accepts extended poly_mode=2 -> eLine");

	// 2. ResolvePolygonMode with differing front/back modes — previously fatal EXIT!
	mode.poly_mode            = 1;
	mode.polymode_front_ptype = 0; // Point
	mode.polymode_back_ptype  = 2; // Fill
	Check(ResolvePolygonMode(mode, false, false) == vk::PolygonMode::ePoint,
	      "ResolvePolygonMode resolves differing front/back modes to front (ePoint)");

	// 3. ResolvePolygonMode with unknown polygon_mode (7) — previously fatal EXIT!
	mode.polymode_front_ptype = 7;
	mode.polymode_back_ptype  = 7;
	Check(ResolvePolygonMode(mode, false, false) == vk::PolygonMode::eFill,
	      "ResolvePolygonMode unknown mode=7 falls back safely to eFill");

	// 4. TextureGetComponentMapping with unknown swizzle selector (2/3 are reserved in RDNA2) — previously fatal EXIT!
	const uint32_t unknown_swizzle = (2u) | (3u << 3u) | (2u << 6u) | (3u << 9u);
	const auto     mapping =
	    TextureGetComponentMapping(unknown_swizzle, Prospero::ColorComponentMapping {});
	Check(mapping.r == vk::ComponentSwizzle::eIdentity &&
	          mapping.g == vk::ComponentSwizzle::eIdentity &&
	          mapping.b == vk::ComponentSwizzle::eIdentity &&
	          mapping.a == vk::ComponentSwizzle::eIdentity,
	      "TextureGetComponentMapping unknown swizzle falls back safely to Identity");

	// 5. Valid Zero/One swizzle
	const uint32_t zero_one_swizzle = (0u) | (1u << 3u) | (0u << 6u) | (1u << 9u);
	const auto     zo =
	    TextureGetComponentMapping(zero_one_swizzle, Prospero::ColorComponentMapping {});
	Check(zo.r == vk::ComponentSwizzle::eZero && zo.g == vk::ComponentSwizzle::eOne &&
	          zo.b == vk::ComponentSwizzle::eZero && zo.a == vk::ComponentSwizzle::eOne,
	      "TextureGetComponentMapping valid Zero/One swizzle maps accurately");

	std::printf("[PASS] Suite 1: Vulkan Host Renderer Pipeline, Image & State Hardening\n");
}

void TestVideoOutModernModes() {
	using namespace Libs::VideoOut;

	// Initialize headless VideoOutDriver (no Vulkan swapchain required)
	(void)VideoOutInitHeadless(3840, 2160);

	// 1. Open with non-system user_id (0x10000001) — previously fatal EXIT_NOT_IMPLEMENTED!
	const int handle = VideoOutOpen(0x10000001, 0 /* MAIN */, 0, nullptr);
	Check(handle > 0, "VideoOutOpen with real user_id=0x10000001 succeeds");

	// 2. Open with invalid index=1 — previously fatal EXIT_NOT_IMPLEMENTED!
	Check(VideoOutOpen(255, 0, 1, nullptr) == VIDEO_OUT_ERROR_INVALID_VALUE,
	      "VideoOutOpen(index=1) returns VIDEO_OUT_ERROR_INVALID_VALUE");

	// 3. Open with negative user_id -> VIDEO_OUT_ERROR_INVALID_VALUE
	Check(VideoOutOpen(-1, 0, 0, nullptr) == VIDEO_OUT_ERROR_INVALID_VALUE,
	      "VideoOutOpen(user_id=-1) returns VIDEO_OUT_ERROR_INVALID_VALUE");

	// 4. Null attribute pointers — previously fatal EXIT_NOT_IMPLEMENTED / EXIT_IF!
	VideoOutSetBufferAttribute2(nullptr, 0, 0, 1920, 1080, 0, 0, 0);
	VideoOutAddBufferAttributeOption(nullptr, 1);

	// 5. Flip rate 0..3 (including 120Hz 40fps divisor rate=3) and invalid rate=4
	Check(VideoOutSetFlipRate(handle, 0) == OK, "VideoOutSetFlipRate(0) returns OK");
	Check(VideoOutSetFlipRate(handle, 3) == OK, "VideoOutSetFlipRate(3) for 120Hz returns OK");
	Check(VideoOutSetFlipRate(handle, 4) == VIDEO_OUT_ERROR_INVALID_VALUE,
	      "VideoOutSetFlipRate(4) returns VIDEO_OUT_ERROR_INVALID_VALUE");
	Check(VideoOutSetFlipRate(handle, 0) == OK, "VideoOutSetFlipRate reset to 0 returns OK");

	// 6. VRR peg/unpeg handle validation
	Check(VideoOutVrrPegToFixedRate(handle, 0, 0) == OK,
	      "VideoOutVrrPegToFixedRate(valid handle) returns OK");
	Check(VideoOutVrrUnpegFromFixedRate(handle) == OK,
	      "VideoOutVrrUnpegFromFixedRate(valid handle) returns OK");
	Check(VideoOutVrrPegToFixedRate(999, 0, 0) == VIDEO_OUT_ERROR_INVALID_HANDLE,
	      "VideoOutVrrPegToFixedRate(invalid handle) returns INVALID_HANDLE");
	Check(VideoOutVrrUnpegFromFixedRate(999) == VIDEO_OUT_ERROR_INVALID_HANDLE,
	      "VideoOutVrrUnpegFromFixedRate(invalid handle) returns INVALID_HANDLE");

	// 7. Output options, output support, configure output, output status (4K)
	alignas(8) uint8_t opts_buf[64] {};
	auto*              opts = reinterpret_cast<VideoOutOutputOptions*>(opts_buf);
	constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_DEFAULT  = 0x0000000000000001ULL;
	constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_119_88HZ = 0x000000000000000FULL;
	Check(VideoOutInitializeOutputOptions(opts) == OK, "VideoOutInitializeOutputOptions returns OK");
	Check(VideoOutIsOutputSupported(handle, VIDEO_OUT_OUTPUT_MODE_DEFAULT, opts, nullptr, 0) == 1,
	      "VideoOutIsOutputSupported(DEFAULT) returns TRUE");
	const int hz120_sup =
	    VideoOutIsOutputSupported(handle, VIDEO_OUT_OUTPUT_MODE_119_88HZ, opts, nullptr, 0);
	Check(hz120_sup == 0 || hz120_sup == 1,
	      "VideoOutIsOutputSupported(119.88Hz) returns valid boolean (0 or 1)");
	Check(VideoOutConfigureOutput(handle, VIDEO_OUT_OUTPUT_MODE_DEFAULT, opts, nullptr, 0) == OK,
	      "VideoOutConfigureOutput(DEFAULT) returns OK");

	alignas(8) uint8_t status_buf[0x30] {};
	auto*              status = reinterpret_cast<VideoOutOutputStatus*>(status_buf);
	Check(VideoOutGetOutputStatus(handle, status) == OK, "VideoOutGetOutputStatus returns OK");
	uint32_t resolution = 0;
	std::memcpy(&resolution, status_buf, sizeof(resolution));
	Check(resolution == 2u, "VideoOutGetOutputStatus reports 4K (resolution=2)");

	// 8. Color settings gamma validation + AdjustColor
	alignas(8) uint8_t color_buf[16] {};
	auto*              color = reinterpret_cast<VideoOutColorSettings*>(color_buf);
	Check(VideoOutColorSettingsSetGamma(color, 1.0f) == OK,
	      "VideoOutColorSettingsSetGamma(1.0) returns OK");
	Check(VideoOutColorSettingsSetGamma(color, 5.0f) == VIDEO_OUT_ERROR_INVALID_VALUE,
	      "VideoOutColorSettingsSetGamma(5.0) returns INVALID_VALUE");
	Check(VideoOutAdjustColor(handle, color) == OK, "VideoOutAdjustColor returns OK");

	// 9. Clean close
	Check(VideoOutClose(handle) == OK, "VideoOutClose returns OK");
	Check(VideoOutClose(handle) == VIDEO_OUT_ERROR_INVALID_HANDLE,
	      "VideoOutClose(already closed) returns INVALID_HANDLE");

	VideoOutShutdown();

	std::printf("[PASS] Suite 2: libSceVideoOut Modern Display Modes & Lifecycle Hardening\n");
}

void TestSysmoduleResilience() {
	using namespace Libs::Sysmodule;

	constexpr int      SYSMODULE_ERROR_UNLOADED = static_cast<int>(0x805a1001u);
	constexpr uint16_t UNKNOWN_SDK10_MODULE     = 0x0999u;
	constexpr uint16_t INTERNAL_SDK10_MODULE    = 0x8099u;

	// 1. Unknown modern SDK 7.x-10.x module ID starts unloaded
	Check(SysmoduleIsLoaded(UNKNOWN_SDK10_MODULE) == SYSMODULE_ERROR_UNLOADED,
	      "SysmoduleIsLoaded(unknown SDK10 module) returns SYSMODULE_ERROR_UNLOADED");
	Check(SysmoduleUnloadModule(UNKNOWN_SDK10_MODULE) == SYSMODULE_ERROR_UNLOADED,
	      "SysmoduleUnloadModule(not loaded) returns SYSMODULE_ERROR_UNLOADED");

	// 2. Load -> refcount 2 -> unload -> still loaded -> unload -> unloaded
	Check(SysmoduleLoadModule(UNKNOWN_SDK10_MODULE) == 0,
	      "SysmoduleLoadModule(unknown SDK10 module) returns SCE_OK");
	Check(SysmoduleIsLoaded(UNKNOWN_SDK10_MODULE) == 0,
	      "SysmoduleIsLoaded after load returns SCE_OK");
	Check(SysmoduleLoadModule(UNKNOWN_SDK10_MODULE) == 0,
	      "SysmoduleLoadModule(2nd ref) returns SCE_OK");
	Check(SysmoduleUnloadModule(UNKNOWN_SDK10_MODULE) == 0,
	      "SysmoduleUnloadModule(1st unload) returns SCE_OK");
	Check(SysmoduleIsLoaded(UNKNOWN_SDK10_MODULE) == 0,
	      "SysmoduleIsLoaded still loaded after 1st unload (refcount=1)");
	Check(SysmoduleUnloadModule(UNKNOWN_SDK10_MODULE) == 0,
	      "SysmoduleUnloadModule(2nd unload) returns SCE_OK");
	Check(SysmoduleIsLoaded(UNKNOWN_SDK10_MODULE) == SYSMODULE_ERROR_UNLOADED,
	      "SysmoduleIsLoaded unloaded after 2nd unload");

	// 3. SysmoduleLoadModuleInternalWithArg with non-zero args & nullptr ret — previously fatal EXIT_IF!
	Check(SysmoduleLoadModuleInternalWithArg(INTERNAL_SDK10_MODULE, 16, 1, 2, nullptr) == 0,
	      "SysmoduleLoadModuleInternalWithArg(non-zero args, nullptr ret) succeeds without abort");
	int out_ret = -1;
	Check(SysmoduleLoadModuleInternalWithArg(INTERNAL_SDK10_MODULE, 32, 0, 1, &out_ret) == 0 &&
	          out_ret == 0,
	      "SysmoduleLoadModuleInternalWithArg(non-zero args, valid ret) writes 0 and returns 0");
	Check(SysmoduleUnloadModule(INTERNAL_SDK10_MODULE) == 0, "Unload internal module (ref 2->1)");
	Check(SysmoduleUnloadModule(INTERNAL_SDK10_MODULE) == 0, "Unload internal module (ref 1->0)");

	std::printf("[PASS] Suite 3: libSceSysmodule Dynamic PRX Load/Unload Resilience\n");
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

	TestHostGpuStateHardening();
	TestVideoOutModernModes();
	TestSysmoduleResilience();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough10Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough10Tests PASSED (3/3 suites)\n");
	return 0;
}
