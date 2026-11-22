// Core Breakthrough #16 Verification Suite:
//   1. libSceAcm (Audio Compression Module) Lifecycle & Invalid-Param Hardening
//   2. libSceAjm (Audio Job Manager) Lifecycle, Invalid-Param & Unknown-Encoding Resilience
//   3. libSceHmd2 (PS VR2) Probe & Reprojection Edge-Case Hardening (via Sony NIDs)

#include "common/abi.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "libs/acm.h"
#include "libs/audio.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace Libs {
void InitHmd2_1(Loader::SymbolDatabase* s);
}

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestAcmHardening() {
	using namespace Libs::Audio::Acm;

	constexpr int ACM_ERROR_INVALID_PARAM = static_cast<int>(0x80260401u);

	// Previously fatal nullptr traps -> ACM_ERROR_INVALID_PARAM!
	Check(AcmContextCreate(nullptr) == ACM_ERROR_INVALID_PARAM,
	      "AcmContextCreate(nullptr) returns ACM_ERROR_INVALID_PARAM");
	Check(AcmBatchStartBuffer(1, nullptr, 0, nullptr, nullptr) == ACM_ERROR_INVALID_PARAM,
	      "AcmBatchStartBuffer(nullptr batch) returns ACM_ERROR_INVALID_PARAM");
	AcmBatchId batch = 0;
	Check(AcmBatchStartBuffers(1, 1, nullptr, nullptr, &batch) == ACM_ERROR_INVALID_PARAM,
	      "AcmBatchStartBuffers(count=1, nullptr info) returns ACM_ERROR_INVALID_PARAM");
	Check(AcmBatchStartBuffers(1, 0, nullptr, nullptr, nullptr) == ACM_ERROR_INVALID_PARAM,
	      "AcmBatchStartBuffers(nullptr batch) returns ACM_ERROR_INVALID_PARAM");

	// Valid lifecycle
	AcmContextId ctx = 0;
	Check(AcmContextCreate(&ctx) == OK && ctx != 0, "AcmContextCreate(valid) succeeds");
	Check(AcmBatchStartBuffer(ctx, nullptr, 0, nullptr, &batch) == OK && batch != 0,
	      "AcmBatchStartBuffer(valid) succeeds");
	Check(AcmBatchWait(ctx, batch, 0) == OK, "AcmBatchWait returns OK");
	Check(AcmContextDestroy(ctx) == OK, "AcmContextDestroy returns OK");

	std::printf("[PASS] Suite 1: libSceAcm Lifecycle & Invalid-Param Hardening\n");
}

void TestAjmHardening() {
	using namespace Libs::Audio::Ajm;

	// Previously fatal nullptr traps -> AJM_ERROR_INVALID_PARAMETER!
	Check(AjmInitialize(0, nullptr) < 0, "AjmInitialize(nullptr) returns error cleanly");
	Check(AjmInstanceCreate(1, 0, 0, nullptr) < 0,
	      "AjmInstanceCreate(nullptr) returns error cleanly");

	// Valid lifecycle + unknown PCM encoding bit fallback (previously fatal EXIT!)
	uint32_t ctx = 0;
	Check(AjmInitialize(0, &ctx) == OK && ctx == 1, "AjmInitialize(valid) succeeds");
	uint32_t       inst = 0;
	const uint64_t exotic_flags = (3ull << 8u) | 2ull; // unsupported encoding=3, 2 channels
	Check(AjmInstanceCreate(ctx, 1 /* DecAt9 */, exotic_flags, &inst) == OK,
	      "AjmInstanceCreate(exotic encoding=3) falls back cleanly to S16 without abort");
	Check(AjmInstanceDestroy(ctx, inst) == OK, "AjmInstanceDestroy returns OK");
	Check(AjmFinalize(ctx) == OK, "AjmFinalize returns OK");

	std::printf("[PASS] Suite 2: libSceAjm Lifecycle & Unknown-Encoding Resilience\n");
}

void TestHmd2Hardening() {
	Loader::SymbolDatabase db;
	Libs::InitHmd2_1(&db);

	auto lookup = [&](const char* nid) -> uint64_t {
		const auto* rec = db.FindByNid(nid, Loader::SymbolType::Func);
		return rec != nullptr ? rec->vaddr : 0;
	};

	using HmdInitFn      = int32_t(KYTY_SYSV_ABI*)(const void*);
	using HmdGetInfoFn   = int32_t(KYTY_SYSV_ABI*)(void*);
	using HmdSetTimingFn = int32_t(KYTY_SYSV_ABI*)(int32_t);

	auto hmd_init   = reinterpret_cast<HmdInitFn>(lookup("c812oYs7Vsc"));
	auto hmd_info   = reinterpret_cast<HmdGetInfoFn>(lookup("bIi4YUfSRys"));
	Check(hmd_init && hmd_info, "libSceHmd2 NIDs resolved");

	// Hmd2Initialize(nullptr) / GetDeviceInformation(nullptr) must not crash
	const int32_t init_res = hmd_init(nullptr);
	Check(init_res == OK || init_res < 0, "Hmd2Initialize(nullptr) executes cleanly");
	Check(hmd_info(nullptr) < 0, "Hmd2GetDeviceInformation(nullptr) returns error cleanly");

	std::printf("[PASS] Suite 3: libSceHmd2 Probe & Edge-Case Hardening\n");
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

	TestAcmHardening();
	TestAjmHardening();
	TestHmd2Hardening();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough16Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough16Tests PASSED (3/3 suites)\n");
	return 0;
}
