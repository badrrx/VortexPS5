// Core Breakthrough #23 Verification Suite (Phase F):
//   1. libSceNgs2 Custom Rack & Null/Edge-Case Hardening
//   2. libSceAgcDriver Indirect-Register Patcher Null-Arg Hardening
//   3. libSceAjm Batch Null-Arg Sweep & pthread Attribute/Rwlock Hardening

#include "common/abi.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/agc.h"
#include "libs/errno.h"
#include "libs/ngs2.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Libs::Audio::Ajm {
struct AjmBatchInfo;
struct AjmBatchError;
int KYTY_SYSV_ABI AjmBatchStart(uint32_t context, const AjmBatchInfo* info, int priority,
                                AjmBatchError* error, uint32_t* batch);
int KYTY_SYSV_ABI AjmBatchJobInitialize(AjmBatchInfo* info, uint32_t instance,
                                        const void* codec_parameters, size_t codec_parameters_size,
                                        void* result);
int KYTY_SYSV_ABI AjmBatchJobDecode(AjmBatchInfo* info, uint32_t instance,
                                    const void* bitstream_input, size_t bitstream_input_size,
                                    void* pcm_output, size_t pcm_output_size, void* result);
int KYTY_SYSV_ABI AjmBatchJobGetInfo(AjmBatchInfo* info, uint32_t instance, void* result);
int KYTY_SYSV_ABI AjmBatchJobRun(AjmBatchInfo* info, uint32_t instance, uint64_t flags,
                                 const void* data_input, size_t data_input_size, void* data_output,
                                 size_t data_output_size, void* sideband_output,
                                 size_t sideband_output_size);
} // namespace Libs::Audio::Ajm

namespace {

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
			std::exit(1);                                                                          \
		}                                                                                          \
	} while (0)

struct TestNgs2ContextBufferInfo {
	void*  host_buffer      = nullptr;
	size_t host_buffer_size = 0;
	size_t reserved[5]      = {};
};

bool TestNgs2Hardening() {
	using namespace Libs::Audio::Ngs2;
	constexpr int32_t kInvalidOut = static_cast<int32_t>(0x804a8010u);

	CHECK(Ngs2SystemResetOption(nullptr) == kInvalidOut);
	CHECK(Ngs2SystemQueryBufferSize(nullptr, nullptr) == kInvalidOut);
	CHECK(Ngs2SystemCreate(nullptr, nullptr, nullptr) == kInvalidOut);
	CHECK(Ngs2SystemCreateWithAllocator(nullptr, nullptr, nullptr) == kInvalidOut);
	CHECK(Ngs2RackQueryBufferSize(0x1000u, nullptr, nullptr) == kInvalidOut);

	// Exotic SDK 10.x custom rack ID (0x4099) must fall back to passthrough defaults, not EXIT!
	TestNgs2ContextBufferInfo buf_info {};
	CHECK(Ngs2RackQueryBufferSize(0x4099u, nullptr,
	                              reinterpret_cast<Ngs2ContextBufferInfo*>(&buf_info)) == OK);
	CHECK(buf_info.host_buffer_size > 0);

	std::printf("[PASS] Suite 1: libSceNgs2 Custom Rack & Null/Edge-Case Hardening\n");
	return true;
}

bool TestAgcHardening() {
	using namespace Libs::Graphics::Gen5;

	CHECK(AgcSetCxRegIndirectPatchSetAddress(nullptr, nullptr) != OK);
	CHECK(AgcSetShRegIndirectPatchSetAddress(nullptr, nullptr) != OK);
	CHECK(AgcSetUcRegIndirectPatchSetAddress(nullptr, nullptr) != OK);
	CHECK(AgcSetCxRegIndirectPatchSetNumRegisters(nullptr, 4) != OK);
	CHECK(AgcSetShRegIndirectPatchSetNumRegisters(nullptr, 4) != OK);
	CHECK(AgcSetUcRegIndirectPatchSetNumRegisters(nullptr, 4) != OK);
	CHECK(AgcSetCxRegIndirectPatchAddRegisters(nullptr, 2) != OK);
	CHECK(AgcSetShRegIndirectPatchAddRegisters(nullptr, 2) != OK);
	CHECK(AgcSetUcRegIndirectPatchAddRegisters(nullptr, 2) != OK);

	std::printf("[PASS] Suite 2: libSceAgcDriver Indirect-Register Patcher Hardening\n");
	return true;
}

bool TestAjmAndPthreadHardening() {
	using namespace Libs::Audio::Ajm;
	constexpr int32_t kAjmInvalidParam = static_cast<int32_t>(0x80930005u);

	CHECK(AjmBatchStart(0, nullptr, 0, nullptr, nullptr) == kAjmInvalidParam);
	CHECK(AjmBatchJobInitialize(nullptr, 0, nullptr, 0, nullptr) == kAjmInvalidParam);
	CHECK(AjmBatchJobDecode(nullptr, 0, nullptr, 0, nullptr, 0, nullptr) == kAjmInvalidParam);
	CHECK(AjmBatchJobGetInfo(nullptr, 0, nullptr) == kAjmInvalidParam);
	CHECK(AjmBatchJobRun(nullptr, 0, 0, nullptr, 0, nullptr, 0, nullptr, 0) == kAjmInvalidParam);

	// Pthread attribute invalid-enum & null-rwlock edge cases
	using namespace Libs::LibKernel;
	Libs::LibKernel::Initialize();
	PthreadInitSelfForMainThread();

	PthreadAttr attr = nullptr;
	CHECK(PthreadAttrInit(&attr) == OK);
	CHECK(PthreadAttrSetdetachstate(&attr, 99) == KERNEL_ERROR_EINVAL);
	CHECK(PthreadAttrSetinheritsched(&attr, 99) == KERNEL_ERROR_EINVAL);
	CHECK(PthreadAttrDestroy(&attr) == OK);

	PthreadRwlock null_rw = nullptr;
	CHECK(PthreadRwlockTrywrlock(&null_rw) == KERNEL_ERROR_EINVAL);

	std::printf("[PASS] Suite 3: libSceAjm Null-Arg Sweep & pthread Attr/Rwlock Hardening\n");
	return true;
}

int RunAllTests() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});

	CHECK(TestNgs2Hardening());
	CHECK(TestAgcHardening());
	CHECK(TestAjmAndPthreadHardening());

	std::printf("ALL CoreBreakthrough23Tests PASSED (3/3 suites)\n");
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
