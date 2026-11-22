// Core Breakthrough #19 Verification Suite (Phase B):
//   1. Extended Compute SH Registers, VRS Shading-Rate Clamping & User SGPR Bounds
//   2. Indirect Command/Register Buffers & Null-Address Indirect Draw/Dispatch Resilience
//   3. Unknown SDK 9.x-10.x EventWrite Resilience & Exact PM4 Stream Dword Alignment

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/pm4.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
			std::exit(1);                                                                          \
		}                                                                                          \
	} while (0)

bool TestComputeShVrsAndSgprHardening() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Pm4;

	auto*            dummy_renderer = reinterpret_cast<RenderContext*>(static_cast<uintptr_t>(0));
	CommandProcessor cp(*dummy_renderer, 0);

	std::vector<uint32_t> stream;

	// 1. VRS DB_SHADING_RATE_ENCODING with extended value 3 (previously EXIT_NOT_IMPLEMENTED > 2u)
	stream.push_back(KYTY_PM4(3u, IT_SET_CONTEXT_REG, 0u));
	stream.push_back(DB_SHADING_RATE_ENCODING);
	stream.push_back(3u << DB_SHADING_RATE_ENCODING_SHADING_RATE_ENCODING_SHIFT);

	// 2. Valid COMPUTE_NUM_THREAD_X = 64
	stream.push_back(KYTY_PM4(3u, IT_SET_SH_REG, 0u));
	stream.push_back(COMPUTE_NUM_THREAD_X);
	stream.push_back(64u);

	// 3. Out-of-range SET_SH_REG offset >= SH_NUM (previously EXIT_NOT_IMPLEMENTED)
	stream.push_back(KYTY_PM4(4u, IT_SET_SH_REG, 0u));
	stream.push_back(SH_NUM + 0x10u);
	stream.push_back(0x11111111u);
	stream.push_back(0x22222222u);

	// 4. CS User SGPR overflow: slot 14 with 4 dwords (exceeds 16 CS user SGPRs)
	stream.push_back(KYTY_PM4(6u, IT_SET_SH_REG, 0u));
	stream.push_back(COMPUTE_USER_DATA_0 + 14u);
	stream.push_back(0xAAAA0014u);
	stream.push_back(0xAAAA0015u);
	stream.push_back(0xDEAD0016u);
	stream.push_back(0xDEAD0017u);

	// 5. User accumulator register with extended bits > 0x7f
	stream.push_back(KYTY_PM4(3u, IT_SET_SH_REG, 0u));
	stream.push_back(SPI_SHADER_USER_ACCUM_PS_0);
	stream.push_back(0x0000FF80u);

	// 6. Sentinel UCONFIG register to prove exact dword alignment after all packets
	stream.push_back(KYTY_PM4(3u, IT_SET_UCONFIG_REG, 0u));
	stream.push_back(VGT_OBJECT_ID);
	stream.push_back(0xB1900001u);

	Pm4Execution exec {};
	const auto   res = cp.Process(exec, stream);
	CHECK(res == Pm4ProcessResult::Complete);
	CHECK(cp.GetCtx().GetDepthRenderTarget().shading_rate_encoding == 2u);
	CHECK(cp.GetShCtx().GetCs().cs_regs.num_thread_x == 64u);
	CHECK(cp.GetUcfg().GetObjectId() == 0xB1900001u);

	std::printf("[PASS] Suite 1: Extended Compute SH, VRS Clamping & User SGPR Bounds\n");
	return true;
}

bool TestIndirectBuffersAndNullDraws() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Pm4;

	auto*            dummy_renderer = reinterpret_cast<RenderContext*>(static_cast<uintptr_t>(0));
	CommandProcessor cp(*dummy_renderer, 0);

	// Real guest indirect register buffers containing both unknown/extended and valid pairs.
	alignas(16) std::array<uint32_t, 6> cx_pairs = {
	    0x000003feu, 0x12345678u,                                                        // unknown CX reg -> warn & skip
	    DB_SHADING_RATE_ENCODING, 3u << DB_SHADING_RATE_ENCODING_SHADING_RATE_ENCODING_SHIFT, // extended VRS -> clamp to 2
	    CX_NOP, 0u,                                                                      // NOP
	};
	alignas(16) std::array<uint32_t, 6> sh_pairs = {
	    SH_NUM + 4u, 0xCAFEBABEu,               // extended SH offset -> warn & skip
	    COMPUTE_NUM_THREAD_Y, 16u,              // valid CS reg -> applied
	    SH_NOP, 0u,                             // NOP
	};
	alignas(16) std::array<uint32_t, 6> uc_pairs = {
	    UC_NUM + 8u, 0xDEADBEEFu,               // extended UC offset -> warn & skip
	    VGT_OBJECT_ID, 0xB1900002u,             // valid UC reg -> applied
	    UC_NOP, 0u,                             // NOP
	};

	const auto push_addr = [](std::vector<uint32_t>& s, const void* ptr) {
		const auto addr = reinterpret_cast<uintptr_t>(ptr);
		s.push_back(static_cast<uint32_t>(addr & 0xFFFFFFFFu));
		s.push_back(static_cast<uint32_t>(static_cast<uint64_t>(addr) >> 32u));
	};

	std::vector<uint32_t> stream;

	// 1. Null indirect command buffer (non-zero num_dw) -> warn & skip
	stream.push_back(KYTY_PM4(4u, IT_INDIRECT_BUFFER, 0u));
	stream.push_back(0u);
	stream.push_back(0u);
	stream.push_back(16u);

	// 2. Null indirect CX/SH/UC register buffers -> warn & skip
	for (const uint32_t op:
	     {IT_SET_CONTEXT_REG_INDIRECT, IT_SET_SH_REG_INDIRECT, IT_SET_UCONFIG_REG_INDIRECT}) {
		stream.push_back(KYTY_PM4(5u, op, 0u));
		stream.push_back(0u);
		stream.push_back(0u);
		stream.push_back(0u);
		stream.push_back(4u);
	}

	// 3. Real indirect CX/SH/UC register buffers with mixed unknown & valid pairs
	stream.push_back(KYTY_PM4(5u, IT_SET_CONTEXT_REG_INDIRECT, 0u));
	push_addr(stream, cx_pairs.data());
	stream.push_back(0u);
	stream.push_back(3u);

	stream.push_back(KYTY_PM4(5u, IT_SET_SH_REG_INDIRECT, 0u));
	push_addr(stream, sh_pairs.data());
	stream.push_back(0u);
	stream.push_back(3u);

	stream.push_back(KYTY_PM4(5u, IT_SET_UCONFIG_REG_INDIRECT, 0u));
	push_addr(stream, uc_pairs.data());
	stream.push_back(0u);
	stream.push_back(3u);

	Pm4Execution exec {};
	const auto   res = cp.Process(exec, stream);
	CHECK(res == Pm4ProcessResult::Complete);
	CHECK(cp.GetCtx().GetDepthRenderTarget().shading_rate_encoding == 2u);
	CHECK(cp.GetShCtx().GetCs().cs_regs.num_thread_y == 16u);
	CHECK(cp.GetUcfg().GetObjectId() == 0xB1900002u);

	// 4. Null-base DrawIndirectMulti & null-address DispatchIndirect must skip cleanly
	IndirectDrawRegisters regs {};
	cp.DrawIndirectMulti(0, 4, nullptr, 16, regs, 0u, true);
	cp.DrawIndirectMulti(0, 4, nullptr, 4, regs, 2u, false); // invalid stride
	cp.DispatchIndirect(0, 0);
	cp.DispatchIndirect(3, 0); // unaligned

	std::printf("[PASS] Suite 2: Indirect Command/Reg Buffers & Null Draw/Dispatch Guards\n");
	return true;
}

bool TestUnknownEventWriteAndAlignment() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::Pm4;

	auto*            dummy_renderer = reinterpret_cast<RenderContext*>(static_cast<uintptr_t>(0));
	CommandProcessor cp(*dummy_renderer, 0);

	// Direct CommandProcessor::TriggerEvent with unknown SDK 10.x event type & cache index
	cp.TriggerEvent(0x0000007Fu, 0x00000000u, 0);
	cp.TriggerEvent(0x00000016u, 0x00000009u, 0); // unexpected cache event index

	// Verify PM4 stream with IT_EVENT_WRITE + trailing UCONFIG sentinel
	std::vector<uint32_t> stream;
	stream.push_back(KYTY_PM4(2u, IT_EVENT_WRITE, 0u));
	stream.push_back(0x0000007Fu);
	stream.push_back(KYTY_PM4(3u, IT_SET_UCONFIG_REG, 0u));
	stream.push_back(VGT_OBJECT_ID);
	stream.push_back(0x1919BEEFu);

	Pm4Execution exec {};
	const auto   res = cp.Process(exec, stream);
	CHECK(res == Pm4ProcessResult::Complete);
	CHECK(cp.GetUcfg().GetObjectId() == 0x1919BEEFu);

	std::printf("[PASS] Suite 3: Unknown EventWrite Resilience & Exact PM4 Stream Alignment\n");
	return true;
}

int RunAllTests() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});

	CHECK(TestComputeShVrsAndSgprHardening());
	CHECK(TestIndirectBuffersAndNullDraws());
	CHECK(TestUnknownEventWriteAndAlignment());

	std::printf("ALL CoreBreakthrough19Tests PASSED (3/3 suites)\n");
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
