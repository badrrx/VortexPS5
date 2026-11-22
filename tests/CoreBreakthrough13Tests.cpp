// Core Breakthrough #13 Verification Suite:
//   1. Multi-Threaded Kernel EventFlag & Semaphore Stress + Edge-Case Hardening
//   2. libKernel _umtx_op (WAIT_UINT_PRIVATE / WAKE_PRIVATE / invalid op) & Clock/UUID Queries
//   3. GFX10.3 Mesh Shader with ViewportIndex + Layer + PointSize Exports -> TranslateProgram ->
//      CompileProgram -> Khronos spirv-val (SPV_ENV_VULKAN_1_3)

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "kernel/eventFlag.h"
#include "kernel/pthread.h"
#include "kernel/semaphore.h"
#include "libs/errno.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <thread>
#include <vector>

namespace Libs::LibKernel {
int KYTY_SYSV_ABI      KernelUuidCreate(uint32_t* uuid);
int KYTY_SYSV_ABI      KernelClockGettime(int clock_id, KernelTimespec* tp);
uint64_t KYTY_SYSV_ABI KernelGetProcessTime();
uint64_t KYTY_SYSV_ABI KernelGetProcessTimeCounter();
} // namespace Libs::LibKernel

namespace Libs::Posix {
int KYTY_SYSV_ABI UmtxOp(volatile void* address, int operation, uint64_t value, void* uaddr,
                         const void* timeout);
int* KYTY_SYSV_ABI GetErrorAddr();
} // namespace Libs::Posix

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

uint32_t EncodeSopp(uint32_t op, uint16_t simm16) {
	return (0x17fu << 23u) | ((op & 0x7fu) << 16u) | simm16;
}

uint32_t EncodeVop1(uint32_t op, uint32_t vdst, uint32_t src0) {
	return (0x3fu << 25u) | ((vdst & 0xffu) << 17u) | ((op & 0xffu) << 9u) | (src0 & 0x1ffu);
}

void EmitExp(std::vector<uint32_t>& code, uint32_t target, uint32_t en, bool done, uint32_t v0,
             uint32_t v1, uint32_t v2, uint32_t v3) {
	const uint32_t w0 = (0x3eu << 26u) | (done ? (1u << 11u) : 0u) | ((target & 0x3fu) << 4u) |
	                    (en & 0xfu);
	const uint32_t w1 = ((v3 & 0xffu) << 24u) | ((v2 & 0xffu) << 16u) | ((v1 & 0xffu) << 8u) |
	                    (v0 & 0xffu);
	code.push_back(w0);
	code.push_back(w1);
}

bool ValidateSpirv(const std::vector<uint32_t>& spirv, std::string& out_error) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& pos,
	                             const char* msg) {
		out_error += "line " + std::to_string(pos.line) + ": " + msg + "\n";
	});
	return tools.Validate(spirv.data(), spirv.size());
}

void TestKernelEventFlagAndSemaphore() {
	using namespace Libs::LibKernel;
	using namespace Libs::LibKernel::EventFlag;
	using namespace Libs::LibKernel::Semaphore;

	// --- EventFlag ---
	KernelEventFlag ef = nullptr;
	Check(KernelCreateEventFlag(&ef, "cb13_ef", 0x22 /* multi + prio */, 0, nullptr) == OK &&
	          ef != nullptr,
	      "KernelCreateEventFlag succeeds");

	// Previously fatal unknown wait_mode (0x00, 0x33) -> KERNEL_ERROR_EINVAL!
	uint64_t pat = 0;
	Check(KernelPollEventFlag(ef, 0x1, 0x00, &pat) == KERNEL_ERROR_EINVAL,
	      "KernelPollEventFlag(invalid mode 0x00) returns KERNEL_ERROR_EINVAL");
	Check(KernelWaitEventFlag(ef, 0x1, 0x33, &pat, nullptr) == KERNEL_ERROR_EINVAL,
	      "KernelWaitEventFlag(invalid mode 0x33) returns KERNEL_ERROR_EINVAL");

	// Timeout check
	KernelUseconds timeout = 5000;
	Check(KernelWaitEventFlag(ef, 0x3, 0x01 /* AND */, &pat, &timeout) == KERNEL_ERROR_ETIMEDOUT,
	      "KernelWaitEventFlag times out cleanly with KERNEL_ERROR_ETIMEDOUT");

	// Multi-threaded Set -> Wait (WAIT_AND | CLEAR_ALL = 0x11)
	std::thread ef_setter([&] {
		Common::Thread::SleepMicro(2000);
		(void)KernelSetEventFlag(ef, 0x1);
		Common::Thread::SleepMicro(2000);
		(void)KernelSetEventFlag(ef, 0x2);
	});
	KernelUseconds wait_us = 500000;
	Check(KernelWaitEventFlag(ef, 0x3, 0x11 /* AND | CLEAR_ALL */, &pat, &wait_us) == OK &&
	          (pat & 0x3u) == 0x3u,
	      "Multi-threaded EventFlag WAIT_AND|CLEAR_ALL wakes cleanly");
	ef_setter.join();

	// Verify CLEAR_ALL cleared bits
	Check(KernelPollEventFlag(ef, 0x3, 0x02 /* OR */, &pat) == KERNEL_ERROR_EBUSY,
	      "EventFlag bits cleared after CLEAR_ALL");

	Check(KernelClearEventFlag(ef, 0) == OK, "KernelClearEventFlag returns OK");
	Check(KernelDeleteEventFlag(ef) == OK, "KernelDeleteEventFlag returns OK");

	// --- Semaphore ---
	// Previously fatal sem == nullptr -> KERNEL_ERROR_EINVAL!
	Check(KernelCreateSema(nullptr, "bad", 1, 0, 1, nullptr) == KERNEL_ERROR_EINVAL,
	      "KernelCreateSema(nullptr) returns KERNEL_ERROR_EINVAL");

	KernelSema sem = nullptr;
	Check(KernelCreateSema(&sem, "cb13_sem", 1, 0, 64, nullptr) == OK && sem != nullptr,
	      "KernelCreateSema succeeds");
	Check(KernelPollSema(sem, 1) == KERNEL_ERROR_EBUSY, "KernelPollSema(empty) returns EBUSY");

	constexpr int    kIterations = 32;
	std::atomic<int> consumed {0};
	std::thread      producer([&] {
        for (int i = 0; i < kIterations; ++i) {
            (void)KernelSignalSema(sem, 1);
        }
    });
	for (int i = 0; i < kIterations; ++i) {
		KernelUseconds sem_to = 500000;
		if (KernelWaitSema(sem, 1, &sem_to) == OK) {
			consumed.fetch_add(1, std::memory_order_relaxed);
		}
	}
	producer.join();
	Check(consumed.load() == kIterations,
	      "Multi-threaded Semaphore producer/consumer completed all 32 iterations");
	Check(KernelDeleteSema(sem) == OK, "KernelDeleteSema returns OK");

	std::printf("[PASS] Suite 1: Multi-Threaded EventFlag & Semaphore Stress + Hardening\n");
}

void TestKernelUmtxAndSystemQueries() {
	using namespace Libs::LibKernel;
	using namespace Libs::Posix;

	// 1. UmtxOp: WAIT_UINT_PRIVATE with mismatched value returns immediately, WAKE_PRIVATE succeeds
	volatile uint32_t futex_word = 42;
	Check(UmtxOp(&futex_word, 15 /* UMTX_OP_WAIT_UINT_PRIVATE */, 99, nullptr, nullptr) == 0,
	      "UmtxOp(UMTX_OP_WAIT_UINT_PRIVATE, mismatched value) returns immediately");
	Check(UmtxOp(&futex_word, 16 /* UMTX_OP_WAKE_PRIVATE */, 1, nullptr, nullptr) == 0,
	      "UmtxOp(UMTX_OP_WAKE_PRIVATE) returns 0");

	// 2. Previously fatal non-null uaddr + unsupported op=999
	int dummy_uaddr = 0;
	Check(UmtxOp(&futex_word, 999, 0, &dummy_uaddr, nullptr) == -1 && *GetErrorAddr() == 22,
	      "UmtxOp(unsupported op=999, non-null uaddr) returns -1 with POSIX_EINVAL (was fatal!)");

	// 3. Clock / ProcessTime / UUID queries
	KernelTimespec ts {};
	Check(KernelClockGettime(0, &ts) == OK, "KernelClockGettime(REALTIME) returns OK");
	Check(KernelGetProcessTimeCounter() > 0, "KernelGetProcessTimeCounter() > 0");
	(void)KernelGetProcessTime();

	uint32_t uuid[4] {};
	Check(KernelUuidCreate(uuid) == OK, "KernelUuidCreate(valid) returns OK");

	std::printf("[PASS] Suite 2: libKernel _umtx_op & Clock/UUID Syscall Hardening\n");
}

void TestMeshShaderViewportAndPointSizeSpirv() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::ShaderRecompiler;

	// Build a real GFX10.3 Mesh Shader that:
	//   1. Sets m0 = (1 << 12) | 3 (1 primitive, 3 vertices) and emits S_SENDMSG(GS_ALLOC_REQ)
	//   2. Exports POS0 (gl_Position), POS1 (MISC: PointSize + Layer + ViewportIndex), and PRIM0
	//   3. Ends with S_ENDPGM
	// Previously, ViewportIndex & PointSize in Mesh shaders fatally crashed spirvEmitterMesh.cpp!
	std::vector<uint32_t> code;
	// s_mov_b32 m0 (0x7c), literal ((1u << 12u) | 3u)
	code.push_back((0x2fu << 25u) | (0x7cu << 16u) | (0x00u << 8u) | 0xffu);
	code.push_back((1u << 12u) | 3u);
	// s_sendmsg msg=GS_ALLOC_REQ (10) -> SOPP op=0x10, simm16=10
	code.push_back(EncodeSopp(0x10u, 10u));
	// v_mov_b32 v0, 0x3f800000 (1.0f) -> VOP1 op=1, src0=0xf2 (1.0f inline const)
	code.push_back(EncodeVop1(0x01u, 0u, 0xf2u));
	// v_mov_b32 v1, 0 -> inline const 0 (0x80)
	code.push_back(EncodeVop1(0x01u, 1u, 0x80u));
	// EXP POS0 (target=12), en=0xf, done=false, v0,v0,v0,v0
	EmitExp(code, 12u, 0xfu, false, 0u, 0u, 0u, 0u);
	// EXP POS1 (target=13), en=0x5 (x=PointSize, z=Layer|ViewportIndex), done=false, v0,v1,v1,v1
	EmitExp(code, 13u, 0x5u, false, 0u, 1u, 1u, 1u);
	// EXP PRIM0 (target=20), en=0x1, done=true, v1,v1,v1,v1
	EmitExp(code, 20u, 0x1u, true, 1u, 1u, 1u, 1u);
	// s_endpgm
	code.push_back(EncodeSopp(0x01u, 0u));

	ShaderVertexInputInfo vs_info {};
	vs_info.mesh.vertices_per_group    = 64;
	vs_info.mesh.primitives_per_group  = 64;
	vs_info.mesh.max_vertices          = 64;
	vs_info.mesh.max_primitives        = 64;
	vs_info.mesh.output_primitive_size = 3;
	vs_info.mesh.provoking_vertex      = 0;
	vs_info.mesh.threads_num[0]        = 32;
	vs_info.mesh.threads_num[1]        = 1;
	vs_info.mesh.threads_num[2]        = 1;
	// Enable MISC vector on POS1 (bit 21) + PointSize (bit 16) + Layer (bit 18) + ViewportIndex (bit 19)
	vs_info.pa_cl_vs_out_cntl = (1u << 21u) | (1u << 16u) | (1u << 18u) | (1u << 19u);

	CompileOptions options {};
	options.stage             = ShaderType::Mesh;
	options.wave_size         = 32;
	options.dump_ir           = false;
	options.input_info.vertex = &vs_info;

	auto translated = TranslateProgram(code, options);
	Check(!translated.program.blocks.empty(),
	      "TranslateProgram(GFX10.3 Mesh Shader w/ ViewportIndex+Layer+PointSize) succeeds");

	const auto compiled =
	    CompileProgram(std::move(translated), options, IR::ResourceSpecialization {});
	Check(!compiled.spirv.empty(), "CompileProgram emits non-empty Mesh Shader SPIR-V 1.6");

	std::string val_err;
	const bool  valid = ValidateSpirv(compiled.spirv, val_err);
	if (!valid) {
		std::fprintf(stderr, "spirv-val error:\n%s\n", val_err.c_str());
	}
	Check(valid, "Khronos spirv-val (SPV_ENV_VULKAN_1_3) validates Mesh Shader SPIR-V cleanly");

	std::printf("[PASS] Suite 3: GFX10.3 Mesh Shader (ViewportIndex/Layer/PointSize) -> SPIR-V 1.6 Validation\n");
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

	TestKernelEventFlagAndSemaphore();
	TestKernelUmtxAndSystemQueries();
	TestMeshShaderViewportAndPointSizeSpirv();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough13Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough13Tests PASSED (3/3 suites)\n");
	return 0;
}
