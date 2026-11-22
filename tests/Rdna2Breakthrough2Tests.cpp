#include "common/emulatorConfig.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv/unified1/spirv.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler;

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

uint32_t EncodeVMovB32Inline(uint32_t vdst, uint32_t imm) {
	return (0x3fu << 25u) | ((vdst & 0xffu) << 17u) | (0x01u << 9u) | (0x80u + (imm & 0x3fu));
}

// RDNA 2 FLAT/GLOBAL encoding (bits [31:26] = 0x37)
std::array<uint32_t, 2> EncodeGlobalAtomic(uint32_t opcode, uint32_t vdst, uint32_t vaddr,
                                           uint32_t vdata, uint32_t saddr, uint32_t offset,
                                           bool glc) {
	const uint32_t word0 = (0x37u << 26u) | ((opcode & 0x7fu) << 18u) |
	                       (glc ? (1u << 16u) : 0u) | (2u << 14u) | (offset & 0xfffu);
	const uint32_t word1 = ((vdst & 0xffu) << 24u) | ((saddr & 0x7fu) << 16u) |
	                       ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
	return {word0, word1};
}

// RDNA 2 VOP3P encoding (bits [31:23] = 0x198 -> 0xcc << 24)
std::array<uint32_t, 2> EncodeVop3p(uint32_t opcode, uint32_t vdst, uint32_t src0, uint32_t src1,
                                    uint32_t src2) {
	const uint32_t word0 = (0x198u << 23u) | ((opcode & 0x7fu) << 16u) | (vdst & 0xffu);
	const uint32_t word1 = ((src2 & 0x1ffu) << 18u) | ((src1 & 0x1ffu) << 9u) | (src0 & 0x1ffu);
	return {word0, word1};
}

// RDNA 2 VOP3B encoding (bits [31:26] = 0x35)
std::array<uint32_t, 2> EncodeVop3b(uint32_t opcode, uint32_t vdst, uint32_t sdst, uint32_t src0,
                                    uint32_t src1, uint32_t src2) {
	const uint32_t word0 = (0x35u << 26u) | ((opcode & 0x3ffu) << 16u) | ((sdst & 0x7fu) << 8u) |
	                       (vdst & 0xffu);
	const uint32_t word1 = ((src2 & 0x1ffu) << 18u) | ((src1 & 0x1ffu) << 9u) | (src0 & 0x1ffu);
	return {word0, word1};
}

// RDNA 2 MUBUF BUFFER_LOAD_DWORDX4 (0x0e)
std::array<uint32_t, 2> EncodeBufferLoadX4(uint32_t vdata, uint32_t vaddr, uint32_t srsrc_quad,
                                           uint32_t offset) {
	const uint32_t word0 = (0x38u << 26u) | (0x0eu << 18u) | (1u << 12u) | (offset & 0xfffu);
	const uint32_t word1 = (0x80u << 24u) | ((srsrc_quad & 0x1fu) << 16u) |
	                       ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
	return {word0, word1};
}

// RDNA 2 MUBUF BUFFER_STORE_DWORDX2 (0x1d)
std::array<uint32_t, 2> EncodeBufferStoreX2(uint32_t vdata, uint32_t vaddr, uint32_t srsrc_quad,
                                            uint32_t offset) {
	const uint32_t word0 = (0x38u << 26u) | (0x1du << 18u) | (1u << 12u) | (offset & 0xfffu);
	const uint32_t word1 = (0x80u << 24u) | ((srsrc_quad & 0x1fu) << 16u) |
	                       ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
	return {word0, word1};
}

// RDNA 2 SOP1: (0x17d << 23) | (sdst << 16) | (op << 8) | ssrc0
uint32_t EncodeSop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
	return (0x17du << 23u) | ((sdst & 0x7fu) << 16u) | ((op & 0xffu) << 8u) | (ssrc0 & 0xffu);
}

// RDNA 2 EXP pos0 (target = 12, en = 0xf, done = 1): bits [31:26] = 0x3e
std::array<uint32_t, 2> EncodeExpPos0(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3) {
	const uint32_t word0 = (0x3eu << 26u) | (1u << 11u) | (12u << 4u) | 0xfu;
	const uint32_t word1 = ((v3 & 0xffu) << 24u) | ((v2 & 0xffu) << 16u) | ((v1 & 0xffu) << 8u) |
	                       (v0 & 0xffu);
	return {word0, word1};
}

uint32_t EncodeEndpgm() {
	return 0xbf810000u;
}

bool SpirvContainsOpcode(const std::vector<uint32_t>& spirv, spv::Op target_op,
                         uint32_t required_word1 = UINT32_MAX) {
	for (size_t i = 5; i < spirv.size();) {
		const uint32_t word_count = spirv[i] >> 16u;
		const uint32_t op         = spirv[i] & 0xffffu;
		if (word_count == 0u || i + word_count > spirv.size()) {
			return false;
		}
		if (op == static_cast<uint32_t>(target_op)) {
			if (required_word1 == UINT32_MAX || (word_count > 1u && spirv[i + 1u] == required_word1)) {
				return true;
			}
		}
		i += word_count;
	}
	return false;
}

bool ValidateSpirv(const std::vector<uint32_t>& spirv, const char* label) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          errors;
	tools.SetMessageConsumer(
	    [&](spv_message_level_t, const char*, const spv_position_t& pos, const char* msg) {
		    errors += "line " + std::to_string(pos.line) + ": " + msg + "\n";
	    });
	const bool ok = tools.Validate(spirv);
	if (!ok) {
		::fprintf(stderr, "[%s] spirv-val errors:\n%s\n", label, errors.c_str());
	}
	return ok;
}

void TestGlobalFlatBdaAtomics() {
	std::vector<uint32_t> code;
	auto push2 = [&](const std::array<uint32_t, 2>& w) {
		code.push_back(w[0]);
		code.push_back(w[1]);
	};

	// v0:v1 = desired, v2:v3 = comparator, v4 = byte offset 0
	code.push_back(EncodeVMovB32Inline(0u, 11u));
	code.push_back(EncodeVMovB32Inline(1u, 22u));
	code.push_back(EncodeVMovB32Inline(2u, 33u));
	code.push_back(EncodeVMovB32Inline(3u, 0u));
	code.push_back(EncodeVMovB32Inline(4u, 0u));

	// 32-bit GLOBAL_ATOMIC_ADD (0x32) & GLOBAL_ATOMIC_CMPSWAP (0x31) via s[0:1] + v4
	push2(EncodeGlobalAtomic(0x32u, 5u, 4u, 0u, 0u, 0u, true));
	push2(EncodeGlobalAtomic(0x31u, 6u, 4u, 0u, 0u, 4u, true));
	// 64-bit GLOBAL_ATOMIC_UMAX_X2 (0x58) & GLOBAL_ATOMIC_CMPSWAP_X2 (0x51) via s[0:1] + v4
	push2(EncodeGlobalAtomic(0x58u, 8u, 4u, 0u, 0u, 8u, true));
	push2(EncodeGlobalAtomic(0x51u, 10u, 4u, 0u, 0u, 16u, true));
	code.push_back(EncodeEndpgm());

	const std::array<uint32_t, 2> user_data {0x00200000u, 0x00000000u};
	ShaderComputeInputInfo        cinfo {};
	cinfo.threads_num[0]     = 64u;
	cinfo.threads_num[1]     = 1u;
	cinfo.threads_num[2]     = 1u;
	cinfo.wave_size          = 64u;
	cinfo.host_subgroup_size = 32u;

	CompileOptions opts {};
	opts.stage              = ShaderType::Compute;
	opts.wave_size          = 64u;
	opts.user_data          = user_data;
	opts.input_info.compute = &cinfo;

	auto translated = TranslateProgram(code, opts);
	IR::ResourceSpecialization spec {};
	spec.buffers.resize(translated.program.info.buffers.size());
	auto compiled = CompileProgram(std::move(translated), opts, spec);

	Check(compiled.program.info.buffer_int64_atomics,
	      "GLOBAL_ATOMIC_*_X2 must enable buffer_int64_atomics");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpCapability,
	                          static_cast<uint32_t>(spv::CapabilityPhysicalStorageBufferAddresses)),
	      "GLOBAL_ATOMIC_* must declare CapabilityPhysicalStorageBufferAddresses");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpCapability,
	                          static_cast<uint32_t>(spv::CapabilityInt64Atomics)),
	      "GLOBAL_ATOMIC_*_X2 must declare CapabilityInt64Atomics");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicIAdd),
	      "GLOBAL_ATOMIC_ADD must emit OpAtomicIAdd");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicUMax),
	      "GLOBAL_ATOMIC_UMAX_X2 must emit OpAtomicUMax");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicCompareExchange),
	      "GLOBAL_ATOMIC_CMPSWAP(_X2) must emit OpAtomicCompareExchange");
	Check(ValidateSpirv(compiled.spirv, "GlobalFlatBdaAtomics"),
	      "GLOBAL_ATOMIC_* SPIR-V must pass spirv-val");
	::printf("[PASS] Suite 1: 32/64-bit GLOBAL_ATOMIC_* (BDA) -> SPIR-V 1.6 (%zu words, spirv-val OK)\n",
	         compiled.spirv.size());
}

void TestDp4aAndMadI64() {
	std::vector<uint32_t> code;
	auto push2 = [&](const std::array<uint32_t, 2>& w) {
		code.push_back(w[0]);
		code.push_back(w[1]);
	};

	// Load dynamic inputs v[0:3] from buffer s[0:3] so ConstantPropagation cannot fold away DP4a/MAD64
	code.push_back(EncodeVMovB32Inline(4u, 0u));
	push2(EncodeBufferLoadX4(0u, 4u, 0u, 0u));

	// V_DOT4_I32_I8 v5, v0, v1, v2 (VOP3P 0x16, VGPR encoding = 256 + v)
	push2(EncodeVop3p(0x16u, 5u, 256u + 0u, 256u + 1u, 256u + 2u));
	// V_DOT4_U32_U8 v6, v0, v1, v5 (VOP3P 0x17)
	push2(EncodeVop3p(0x17u, 6u, 256u + 0u, 256u + 1u, 256u + 5u));
	// V_MAD_I64_I32 v[8:9], null(0x7d), v5, v6, v[2:3] (VOP3B 0x177)
	push2(EncodeVop3b(0x177u, 8u, 0x7du, 256u + 5u, 256u + 6u, 256u + 2u));
	// Store v[8:9] to output buffer s[0:3] so DCE retains all computations
	push2(EncodeBufferStoreX2(8u, 4u, 0u, 0u));
	code.push_back(EncodeEndpgm());

	const std::array<uint32_t, 4> user_data {0x00100000u, 0x00000000u, 4096u, 0x00024facu};
	ShaderComputeInputInfo        cinfo {};
	cinfo.threads_num[0]     = 64u;
	cinfo.threads_num[1]     = 1u;
	cinfo.threads_num[2]     = 1u;
	cinfo.wave_size          = 64u;
	cinfo.host_subgroup_size = 32u;

	CompileOptions opts {};
	opts.stage              = ShaderType::Compute;
	opts.wave_size          = 64u;
	opts.user_data          = user_data;
	opts.input_info.compute = &cinfo;

	auto translated = TranslateProgram(code, opts);
	IR::ResourceSpecialization spec {};
	spec.buffers.resize(translated.program.info.buffers.size());
	auto compiled = CompileProgram(std::move(translated), opts, spec);

	Check(SpirvContainsOpcode(compiled.spirv, spv::OpBitFieldSExtract),
	      "V_DOT4_I32_I8 must emit OpBitFieldSExtract");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpBitFieldUExtract),
	      "V_DOT4_U32_U8 must emit OpBitFieldUExtract");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpSMulExtended),
	      "V_MAD_I64_I32 must emit OpSMulExtended");
	Check(ValidateSpirv(compiled.spirv, "Dp4aAndMadI64"),
	      "DP4a + V_MAD_I64_I32 SPIR-V must pass spirv-val");
	::printf("[PASS] Suite 2: V_DOT4_I32_I8 + V_DOT4_U32_U8 + V_MAD_I64_I32 -> SPIR-V 1.6 (%zu words, spirv-val OK)\n",
	         compiled.spirv.size());
}

void TestNonComputeSwappcCall() {
	// Leaf subroutine in simulated guest memory at address 0x0000000040001000:
	//   v_mov_b32 v0, 1
	//   s_setpc_b64 s[0:1]   (SOP1 0x20, sdst=0, ssrc0=0)
	const std::vector<uint32_t> leaf_fn {
	    EncodeVMovB32Inline(0u, 1u),
	    EncodeSop1(0x20u, 0u, 0u),
	};
	constexpr uint64_t kLeafAddr = 0x0000000040001000ull;

	// Vertex shader main:
	//   s_swappc_b64 s[0:1], s[0:1]   (SOP1 0x21, sdst=0, ssrc0=0)
	//   exp pos0, v0, v0, v0, v0 done
	//   s_endpgm
	std::vector<uint32_t> vs_code;
	vs_code.push_back(EncodeSop1(0x21u, 0u, 0u));
	const auto exp_words = EncodeExpPos0(0u, 0u, 0u, 0u);
	vs_code.push_back(exp_words[0]);
	vs_code.push_back(exp_words[1]);
	vs_code.push_back(EncodeEndpgm());

	const std::array<uint32_t, 2> user_data {
	    static_cast<uint32_t>(kLeafAddr & 0xffffffffu),
	    static_cast<uint32_t>(kLeafAddr >> 32u),
	};

	ShaderVertexInputInfo vinfo {};
	vinfo.wave_size = 64u;

	CompileOptions opts {};
	opts.stage             = ShaderType::Vertex;
	opts.wave_size         = 64u;
	opts.user_data         = user_data;
	opts.input_info.vertex = &vinfo;

	// Previously PrepareShaderSource -> PrepareCallTargets crashed with EXIT_NOT_IMPLEMENTED(stage != Compute)!
	auto source = PrepareShaderSource(vs_code, opts);
	Check(source.calls.size() == 1u, "Vertex shader must discover 1 S_SWAPPC_B64 call site");

	IR::SrtRuntime runtime {};
	runtime.user_data   = user_data;
	runtime.shader_base = reinterpret_cast<uint64_t>(vs_code.data());
	runtime.userdata    = const_cast<std::vector<uint32_t>*>(&leaf_fn);
	runtime.read_specialization_memory = [](void* ud, uint64_t addr,
	                                        std::span<uint32_t> dst) -> bool {
		const auto* fn = static_cast<const std::vector<uint32_t>*>(ud);
		if (addr < kLeafAddr) return false;
		const uint64_t byte_off = addr - kLeafAddr;
		if ((byte_off & 3u) != 0u) return false;
		const size_t word_off = static_cast<size_t>(byte_off / 4u);
		if (word_off + dst.size() > fn->size()) return false;
		std::memcpy(dst.data(), fn->data() + word_off, dst.size_bytes());
		return true;
	};

	const auto& linked_decoded = RefreshShaderSource(source, runtime);
	Check(source.linked.has_value(), "Vertex shader leaf subroutine must link cleanly");

	auto translated = TranslateProgram(linked_decoded, opts);
	IR::ResourceSpecialization spec {};
	spec.buffers.resize(translated.program.info.buffers.size());
	auto compiled = CompileProgram(std::move(translated), opts, spec);

	Check(ValidateSpirv(compiled.spirv, "NonComputeSwappcVertex"),
	      "Vertex shader with S_SWAPPC_B64 call must pass spirv-val");
	::printf("[PASS] Suite 3: Vertex Shader S_SWAPPC_B64 Subroutine Linking -> SPIR-V 1.6 (%zu words, spirv-val OK)\n",
	         compiled.spirv.size());
}

int RunAllTests() {
	Config::Initialize();
	Config::Load({});

	TestGlobalFlatBdaAtomics();
	TestDp4aAndMadI64();
	TestNonComputeSwappcCall();

	Config::Shutdown();

	if (g_failures != 0) {
		::fprintf(stderr, "Rdna2Breakthrough2Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}
	::printf("Rdna2Breakthrough2Tests: ALL 3/3 SUITES PASSED!\n");
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
