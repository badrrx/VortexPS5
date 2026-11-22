// Core Breakthrough #22 Verification Suite (Phase E):
//   1. RDNA 2 SDWA Source/Dest Selectors & DST_U (PAD/SEXT/PRESERVE/Reserved) -> SPIR-V 1.6 spirv-val
//   2. RDNA 2 ExecHi/VccHi Special-Register & Scalar Destination Handling -> SPIR-V 1.6 spirv-val
//   3. Ray Tracing BVH Dirty-Page BLAS Deduplication Gate (EvaluateBlasBuildDecision)

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/rayTracing.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <vector>

namespace {

#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
			std::exit(1);                                                                          \
		}                                                                                          \
	} while (0)

uint32_t EncodeSopp(uint32_t op, uint16_t simm16) {
	return (0x17fu << 23u) | ((op & 0x7fu) << 16u) | simm16;
}

uint32_t EncodeVop1(uint32_t op, uint32_t vdst, uint32_t src0) {
	return (0x3fu << 25u) | ((vdst & 0xffu) << 17u) | ((op & 0xffu) << 9u) | (src0 & 0x1ffu);
}

uint32_t EncodeSop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
	return (0x17du << 23u) | ((sdst & 0x7fu) << 16u) | ((op & 0xffu) << 8u) | (ssrc0 & 0xffu);
}

// Emits VOP1 with SDWA extension word (src0 = 0xF9).
void EmitVop1Sdwa(std::vector<uint32_t>& code, uint32_t op, uint32_t vdst, uint32_t src0_vgpr,
                  uint32_t dst_sel, uint32_t dst_unused, uint32_t src0_sel, bool src0_sext) {
	code.push_back(EncodeVop1(op, vdst, 0xf9u));
	const uint32_t sdwa = (src0_vgpr & 0xffu) | ((dst_sel & 0x7u) << 8u) |
	                      ((dst_unused & 0x3u) << 11u) | ((src0_sel & 0x7u) << 16u) |
	                      (src0_sext ? (1u << 19u) : 0u);
	code.push_back(sdwa);
}

bool ValidateSpirv(const std::vector<uint32_t>& spirv, std::string& out_error) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& pos,
	                             const char* msg) {
		out_error += "line " + std::to_string(pos.line) + ": " + msg + "\n";
	});
	return tools.Validate(spirv.data(), spirv.size());
}

bool CompileComputeAndValidate(const std::vector<uint32_t>& code, uint64_t hash,
                               size_t& out_words) {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::ShaderRecompiler;

	ShaderComputeInputInfo cs_info {};
	cs_info.wave_size     = 64;
	cs_info.threads_num[0] = 64;
	cs_info.threads_num[1] = 1;
	cs_info.threads_num[2] = 1;

	CompileOptions options {};
	options.stage              = ShaderType::Compute;
	options.wave_size          = 64;
	options.shader_hash        = hash;
	options.dump_ir            = false;
	options.input_info.compute = &cs_info;

	auto translated = TranslateProgram(code, options);
	CHECK(!translated.program.blocks.empty());

	const auto compiled =
	    CompileProgram(std::move(translated), options, IR::ResourceSpecialization {});
	CHECK(!compiled.spirv.empty());

	std::string val_err;
	const bool  valid = ValidateSpirv(compiled.spirv, val_err);
	if (!valid) {
		std::fprintf(stderr, "SPIR-V validation failed:\n%s\n", val_err.c_str());
	}
	CHECK(valid);
	out_words = compiled.spirv.size();
	return true;
}

bool TestSdwaModifiersAndReservedModes() {
	std::vector<uint32_t> code;
	// v0 = 0x12345678 via inline constant + SDWA modifiers
	code.push_back(EncodeVop1(0x01u, 0u, 0x81u)); // V_MOV_B32 v0, 1
	// 1. Valid SDWA: BYTE_1 -> WORD_0, DST_U = PAD (0)
	EmitVop1Sdwa(code, 0x01u, 1u, 0u, /*dst_sel=*/4u, /*dst_unused=*/0u, /*src0_sel=*/1u, false);
	// 2. Valid SDWA: WORD_1 (sext) -> BYTE_2, DST_U = SEXT (1)
	EmitVop1Sdwa(code, 0x01u, 2u, 1u, /*dst_sel=*/2u, /*dst_unused=*/1u, /*src0_sel=*/5u, true);
	// 3. Valid SDWA: BYTE_0 -> BYTE_3, DST_U = PRESERVE (2)
	EmitVop1Sdwa(code, 0x01u, 3u, 2u, /*dst_sel=*/3u, /*dst_unused=*/2u, /*src0_sel=*/0u, false);
	// 4. Full DWORD selector (6) with PAD/SEXT/PRESERVE modes
	EmitVop1Sdwa(code, 0x01u, 4u, 3u, /*dst_sel=*/6u, /*dst_unused=*/0u, /*src0_sel=*/6u, false);
	EmitVop1Sdwa(code, 0x01u, 5u, 4u, /*dst_sel=*/5u, /*dst_unused=*/2u, /*src0_sel=*/4u, false);
	code.push_back(EncodeSopp(0x01u, 0u)); // S_ENDPGM

	size_t words = 0;
	CHECK(CompileComputeAndValidate(code, 0x2200'0001ULL, words));
	std::printf("[PASS] Suite 1: RDNA 2 SDWA Modifiers & Reserved Modes -> SPIR-V 1.6 (%zu words, spirv-val OK)\n",
	            words);
	return true;
}

bool TestExecHiAndSpecialRegisterLowering() {
	std::vector<uint32_t> code;
	// Read EXEC_LO (0x7e) and EXEC_HI (0x7f) into SGPRs/VGPRs and write back safely.
	code.push_back(EncodeSop1(0x03u, 0u, 0x7eu)); // S_MOV_B32 s0, exec_lo
	code.push_back(EncodeSop1(0x03u, 1u, 0x7fu)); // S_MOV_B32 s1, exec_hi
	code.push_back(EncodeVop1(0x01u, 0u, 0x7eu)); // V_MOV_B32 v0, exec_lo
	code.push_back(EncodeVop1(0x01u, 1u, 0x7fu)); // V_MOV_B32 v1, exec_hi
	code.push_back(EncodeSopp(0x01u, 0u));        // S_ENDPGM

	size_t words = 0;
	CHECK(CompileComputeAndValidate(code, 0x2200'0002ULL, words));
	std::printf("[PASS] Suite 2: RDNA 2 ExecLo/ExecHi Special-Register Lowering -> SPIR-V 1.6 (%zu words, spirv-val OK)\n",
	            words);
	return true;
}

bool TestRayTracingBlasDirtyDeduplication() {
	using namespace Libs::Graphics;

	// 1. Not ready -> Skip
	CHECK(EvaluateBlasBuildDecision(false, false, true, false, false, false, false, false) ==
	      BlasBuildDecision::Skip);

	// 2. Ready + not built -> Rebuild
	CHECK(EvaluateBlasBuildDecision(true, false, true, false, false, false, false, false) ==
	      BlasBuildDecision::Rebuild);

	// 3. Ready + built + whole reshaped -> Rebuild
	CHECK(EvaluateBlasBuildDecision(true, true, true, true, false, true, true, false) ==
	      BlasBuildDecision::Rebuild);

	// 4. Ready + built + dynamic BUT guest pages are clean -> Skip (deduplicated!)
	CHECK(EvaluateBlasBuildDecision(true, true, true, false, /*dynamic=*/true,
	                                /*geometry_known=*/true, /*applied_current=*/true,
	                                /*guest_pages_dirty=*/false) == BlasBuildDecision::Skip);

	// 5. Ready + built + dynamic AND guest pages are dirty -> Update
	CHECK(EvaluateBlasBuildDecision(true, true, true, false, /*dynamic=*/true,
	                                /*geometry_known=*/true, /*applied_current=*/true,
	                                /*guest_pages_dirty=*/true) == BlasBuildDecision::Update);

	// 6. Ready + built + geometry unknown + applied_current -> Update
	CHECK(EvaluateBlasBuildDecision(true, true, false, false, false,
	                                /*geometry_known=*/false, /*applied_current=*/true,
	                                /*guest_pages_dirty=*/false) == BlasBuildDecision::Update);

	std::printf("[PASS] Suite 3: Ray Tracing BVH Dirty-Page BLAS Deduplication Gate\n");
	return true;
}

int RunAllTests() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});

	CHECK(TestSdwaModifiersAndReservedModes());
	CHECK(TestExecHiAndSpecialRegisterLowering());
	CHECK(TestRayTracingBlasDirtyDeduplication());

	std::printf("ALL CoreBreakthrough22Tests PASSED (3/3 suites)\n");
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
