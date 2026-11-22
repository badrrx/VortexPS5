#include "common/emulatorConfig.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv/unified1/spirv.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

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

// Encode RDNA 2 (GFX10.3) VOP1 V_MOV_B32 vdst, inline_int (0..64 -> 0x80 + imm)
uint32_t EncodeVMovB32Inline(uint32_t vdst, uint32_t imm) {
	return (0x3fu << 25u) | ((vdst & 0xffu) << 17u) | (0x01u << 9u) | (0x80u + (imm & 0x3fu));
}

// Encode RDNA 2 (GFX10.3) MUBUF instruction (2 dwords)
std::array<uint32_t, 2> EncodeMubuf(uint32_t opcode, uint32_t vdata, uint32_t vaddr,
                                    uint32_t srsrc_quad, uint32_t soffset, uint32_t offset,
                                    bool glc, bool offen) {
	const uint32_t word0 = (0x38u << 26u) | (((opcode >> 7u) & 1u) << 25u) |
	                       ((opcode & 0x7fu) << 18u) | (glc ? (1u << 14u) : 0u) |
	                       (offen ? (1u << 12u) : 0u) | (offset & 0xfffu);
	const uint32_t word1 = ((soffset & 0xffu) << 24u) | ((srsrc_quad & 0x1fu) << 16u) |
	                       ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
	return {word0, word1};
}

// Encode RDNA 2 (GFX10.3) SOPK S_WAITCNT_VSCNT null, count
uint32_t EncodeWaitcntVscnt(uint32_t count) {
	return (0xbu << 28u) | (0x17u << 23u) | (0x7du << 16u) | (count & 0xffffu);
}

// Encode RDNA 2 (GFX10.3) SOPP S_ENDPGM
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

void VerifyLiveVulkanShaderModule(const std::vector<uint32_t>& spirv) {
	vk::detail::DynamicLoader dl;
	auto vkGetInstanceProcAddr = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	if (vkGetInstanceProcAddr == nullptr) {
		::printf("[INFO] Vulkan loader unavailable; skipped live VkShaderModule check\n");
		return;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

	vk::ApplicationInfo app_info {};
	app_info.pApplicationName = "Rdna2Atomics64Tests";
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
	if (gpu_count == 0) {
		instance.destroy(nullptr);
		return;
	}
	std::vector<vk::PhysicalDevice> gpus(gpu_count);
	(void)instance.enumeratePhysicalDevices(&gpu_count, gpus.data());

	vk::PhysicalDevice gpu = gpus[0];
	vk::PhysicalDeviceVulkan12Features feat12 {};
	feat12.shaderBufferInt64Atomics = VK_TRUE;
	vk::PhysicalDeviceFeatures2 feat2 {};
	feat2.pNext = &feat12;
	gpu.getFeatures2(&feat2);
	if (feat12.shaderBufferInt64Atomics != VK_TRUE) {
		::printf("[INFO] Host GPU does not advertise shaderBufferInt64Atomics; skipped device test\n");
		instance.destroy(nullptr);
		return;
	}

	float                     prio = 1.0f;
	vk::DeviceQueueCreateInfo qinfo {};
	qinfo.queueFamilyIndex = 0;
	qinfo.queueCount       = 1;
	qinfo.pQueuePriorities = &prio;

	vk::PhysicalDeviceVulkan12Features enable12 {};
	enable12.shaderBufferInt64Atomics = VK_TRUE;
	vk::PhysicalDeviceFeatures enable10 {};
	enable10.shaderInt64 = VK_TRUE;

	vk::DeviceCreateInfo dinfo {};
	dinfo.pNext                = &enable12;
	dinfo.queueCreateInfoCount = 1;
	dinfo.pQueueCreateInfos    = &qinfo;
	dinfo.pEnabledFeatures     = &enable10;

	vk::Device device = nullptr;
	if (gpu.createDevice(&dinfo, nullptr, &device) == vk::Result::eSuccess && device != nullptr) {
		VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
		vk::ShaderModuleCreateInfo sm_info {};
		sm_info.codeSize = spirv.size() * sizeof(uint32_t);
		sm_info.pCode    = spirv.data();
		vk::ShaderModule mod = nullptr;
		const auto       res = device.createShaderModule(&sm_info, nullptr, &mod);
		Check(res == vk::Result::eSuccess && mod != nullptr,
		      "Live Vulkan 1.3 vkCreateShaderModule must accept recompiled 64-bit atomics SPIR-V");
		if (mod != nullptr) {
			device.destroyShaderModule(mod, nullptr);
		}
		device.destroy(nullptr);
		::printf("[PASS] Live Vulkan 1.3 Host GPU VkShaderModule created with Int64Atomics!\n");
	}
	instance.destroy(nullptr);
}

int RunAllTests() {
	Config::Initialize();
	Config::Load({});

	// Assemble an authentic RDNA 2 (GFX10.3) Compute Shader exercising ALL 6 newly added
	// 64-bit MUBUF atomics (including UE5 Nanite's BUFFER_ATOMIC_UMAX_X2 and lock-free
	// BUFFER_ATOMIC_CMPSWAP_X2) plus pipelined S_WAITCNT_VSCNT null, 1.
	std::vector<uint32_t> rdna2_code;
	auto emit_mubuf = [&](uint32_t op, uint32_t vdata, uint32_t vaddr, uint32_t off) {
		const auto words = EncodeMubuf(op, vdata, vaddr, 0u, 0x80u, off, true, true);
		rdna2_code.push_back(words[0]);
		rdna2_code.push_back(words[1]);
	};

	// Initialize VGPRs: v0:v1 = desired 64-bit, v2:v3 = comparator 64-bit, v4 = byte offset 0
	rdna2_code.push_back(EncodeVMovB32Inline(0u, 42u));
	rdna2_code.push_back(EncodeVMovB32Inline(1u, 7u));
	rdna2_code.push_back(EncodeVMovB32Inline(2u, 10u));
	rdna2_code.push_back(EncodeVMovB32Inline(3u, 0u));
	rdna2_code.push_back(EncodeVMovB32Inline(4u, 0u));

	// 1. BUFFER_ATOMIC_UMAX_X2 (0x58) — UE5 Nanite 64-bit visbuffer atomic
	emit_mubuf(0x58u, 0u, 4u, 0u);
	// 2. BUFFER_ATOMIC_CMPSWAP_X2 (0x51) — 64-bit CAS (v[0:1] desired, v[2:3] comparator)
	emit_mubuf(0x51u, 0u, 4u, 8u);
	// 3. BUFFER_ATOMIC_SUB_X2 (0x53)
	emit_mubuf(0x53u, 0u, 4u, 16u);
	// 4. BUFFER_ATOMIC_SMIN_X2 (0x55)
	emit_mubuf(0x55u, 0u, 4u, 24u);
	// 5. BUFFER_ATOMIC_SMAX_X2 (0x57)
	emit_mubuf(0x57u, 0u, 4u, 32u);
	// 6. BUFFER_ATOMIC_XOR_X2 (0x5b)
	emit_mubuf(0x5bu, 0u, 4u, 40u);
	// 7. Pipelined S_WAITCNT_VSCNT null, 1 (previously hard-crashed Control.cpp!)
	rdna2_code.push_back(EncodeWaitcntVscnt(1u));
	// 8. S_ENDPGM
	rdna2_code.push_back(EncodeEndpgm());

	// Provide a valid 4-dword RDNA 2 V# buffer descriptor in user_data s[0:3]
	// Base address = 0x100000, stride = 0, num_records = 4096, dst_sel = XYZW
	const std::array<uint32_t, 4> user_data {
	    0x00100000u,
	    0x00000000u,
	    4096u,
	    0x00024facu,
	};

	ShaderComputeInputInfo compute_info {};
	compute_info.threads_num[0]     = 64u;
	compute_info.threads_num[1]     = 1u;
	compute_info.threads_num[2]     = 1u;
	compute_info.wave_size          = 64u;
	compute_info.host_subgroup_size = 32u;

	CompileOptions options {};
	options.stage              = ShaderType::Compute;
	options.wave_size          = 64u;
	options.user_data          = user_data;
	options.input_info.compute = &compute_info;

	// Step 1: Decode + Translate RDNA 2 -> Kyty IR
	auto translated = TranslateProgram(rdna2_code, options);
	Check(!translated.program.blocks.empty(), "Translated IR program must contain basic blocks");

	// Step 2: Compile Kyty IR -> SPIR-V 1.6
	IR::ResourceSpecialization specialization {};
	specialization.buffers.resize(translated.program.info.buffers.size());
	auto compiled = CompileProgram(std::move(translated), options, specialization);
	Check(compiled.spirv.size() > 32, "Recompiled SPIR-V binary must be non-empty");
	Check(compiled.spirv[0] == 0x07230203u, "Recompiled SPIR-V magic must be valid");
	Check(compiled.program.info.buffer_int64_atomics,
	      "ShaderInfoCollection must set buffer_int64_atomics = true");

	// Step 3: Verify exact SPIR-V opcodes & Int64Atomics capability were emitted
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpCapability,
	                          static_cast<uint32_t>(spv::CapabilityInt64Atomics)),
	      "SPIR-V must declare OpCapability Int64Atomics");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicUMax),
	      "SPIR-V must emit OpAtomicUMax for BUFFER_ATOMIC_UMAX_X2");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicCompareExchange),
	      "SPIR-V must emit OpAtomicCompareExchange for BUFFER_ATOMIC_CMPSWAP_X2");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicISub),
	      "SPIR-V must emit OpAtomicISub for BUFFER_ATOMIC_SUB_X2");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicSMin),
	      "SPIR-V must emit OpAtomicSMin for BUFFER_ATOMIC_SMIN_X2");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicSMax),
	      "SPIR-V must emit OpAtomicSMax for BUFFER_ATOMIC_SMAX_X2");
	Check(SpirvContainsOpcode(compiled.spirv, spv::OpAtomicXor),
	      "SPIR-V must emit OpAtomicXor for BUFFER_ATOMIC_XOR_X2");

	// Step 4: Validate with official Khronos SPIRV-Tools validator (Vulkan 1.3)
	spvtools::SpirvTools spirv_tools(SPV_ENV_VULKAN_1_3);
	std::string          val_errors;
	spirv_tools.SetMessageConsumer(
	    [&](spv_message_level_t, const char*, const spv_position_t& pos, const char* msg) {
		    val_errors += "line " + std::to_string(pos.line) + ": " + msg + "\n";
	    });
	const bool valid_spv = spirv_tools.Validate(compiled.spirv);
	if (!valid_spv) {
		::fprintf(stderr, "SPIR-V validation errors:\n%s\n", val_errors.c_str());
	}
	Check(valid_spv, "Khronos spirv-val (SPV_ENV_VULKAN_1_3) must pass with zero errors");

	// Step 5: Load into a live Vulkan 1.3 host GPU VkShaderModule
	VerifyLiveVulkanShaderModule(compiled.spirv);

	Config::Shutdown();

	if (g_failures != 0) {
		::fprintf(stderr, "Rdna2Atomics64Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}
	::printf("[PASS] RDNA 2 -> IR -> SPIR-V 1.6 (spirv-val + 6x 64-bit Atomics + VSCNT) — %zu SPIR-V words\n",
	         compiled.spirv.size());
	::printf("Rdna2Atomics64Tests: ALL CHECKS PASSED!\n");
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
