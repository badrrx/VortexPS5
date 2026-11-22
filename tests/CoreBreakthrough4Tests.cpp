// Core Breakthrough #4 Verification Suite:
//   1. RDNA 2 (GFX10.3) IEEE-754 Division (V_DIV_SCALE/FMAS/FIXUP F32 & F64) + Extended S_GETREG_B32
//      compiled through TranslateProgram -> CompileProgram -> Khronos spirv-val (SPV_ENV_VULKAN_1_3)
//   2. Real libSceNgs2 System / Mastering+Reverb+Sampler Rack / VoiceState / SystemRender lifecycle
//   3. Real libSceAudio3d Port / Non-blocking Push / Object Reserve+Attributes / Close+Terminate lifecycle

#include "common/emulatorConfig.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "libs/audio3d.h"
#include "libs/errno.h"
#include "libs/ngs2.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
// GFX10.3 Instruction Encoders
// -----------------------------------------------------------------------------
uint32_t EncodeSopp(uint32_t op, uint16_t simm16) {
	return (0x17fu << 23u) | ((op & 0x7fu) << 16u) | simm16;
}

uint32_t EncodeSopk(uint32_t op, uint32_t sdst, uint16_t simm16) {
	return (0xbu << 28u) | ((op & 0x1fu) << 23u) | ((sdst & 0x7fu) << 16u) | simm16;
}

uint32_t EncodeVop1(uint32_t op, uint32_t vdst, uint32_t src0) {
	return (0x3fu << 25u) | ((vdst & 0xffu) << 17u) | ((op & 0xffu) << 9u) | (src0 & 0x1ffu);
}

void EmitVop3A(std::vector<uint32_t>& code, uint32_t op, uint32_t vdst, uint32_t src0,
               uint32_t src1, uint32_t src2) {
	const uint32_t w0 = (0x35u << 26u) | ((op & 0x3ffu) << 16u) | (vdst & 0xffu);
	const uint32_t w1 = ((src2 & 0x1ffu) << 18u) | ((src1 & 0x1ffu) << 9u) | (src0 & 0x1ffu);
	code.push_back(w0);
	code.push_back(w1);
}

void EmitVop3B(std::vector<uint32_t>& code, uint32_t op, uint32_t vdst, uint32_t sdst,
               uint32_t src0, uint32_t src1, uint32_t src2) {
	const uint32_t w0 = (0x35u << 26u) | ((op & 0x3ffu) << 16u) | ((sdst & 0x7fu) << 8u) |
	                    (vdst & 0xffu);
	const uint32_t w1 = ((src2 & 0x1ffu) << 18u) | ((src1 & 0x1ffu) << 9u) | (src0 & 0x1ffu);
	code.push_back(w0);
	code.push_back(w1);
}

void EmitMubuf(std::vector<uint32_t>& code, uint32_t op, uint32_t vdata, uint32_t vaddr,
               uint32_t srsrc_quad, uint32_t soffset, bool idxen) {
	const uint32_t w0 = (0x38u << 26u) | ((op & 0x7fu) << 18u) | (idxen ? (1u << 13u) : 0u);
	const uint32_t w1 = ((soffset & 0xffu) << 24u) | ((srsrc_quad & 0x1fu) << 16u) |
	                    ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
	code.push_back(w0);
	code.push_back(w1);
}

uint16_t EncodeHwRegSelector(uint32_t hwreg_id, uint32_t offset, uint32_t size) {
	return static_cast<uint16_t>(((size - 1u) << 11u) | ((offset & 0x1fu) << 6u) | (hwreg_id & 0x3fu));
}

bool ValidateSpirv(const std::vector<uint32_t>& spirv, std::string& out_error) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& pos,
	                             const char* msg) {
		out_error += "line " + std::to_string(pos.line) + ": " + msg + "\n";
	});
	return tools.Validate(spirv.data(), spirv.size());
}

// -----------------------------------------------------------------------------
// Suite 1: RDNA 2 IEEE-754 Division (F32 & F64) + Extended S_GETREG_B32
// -----------------------------------------------------------------------------
bool TestRdna2IeeeDivAndHwReg() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::ShaderRecompiler;

	std::vector<uint32_t> code;

	// Load 4 dynamic dwords into v[4:7] from buffer descriptor s[0:3], vaddr = v0 (idxen=true)
	// BUFFER_LOAD_DWORDX4 = 0x0f
	EmitMubuf(code, 0x0fu, 4u, 0u, 0u, 0x80u, true);
	// S_WAITCNT vmcnt(0)
	code.push_back(EncodeSopp(0x0cu, 0x0000u));

	// --- F32 IEEE-754 Division sequence using dynamic v4 (numer) / v5 (denom) ---
	// V_DIV_SCALE_F32 (0x16d): v8, vcc(106) = div_scale_f32(v5, v5, v4)
	EmitVop3B(code, 0x16du, 8u, 106u, 256u + 5u, 256u + 5u, 256u + 4u);
	// V_DIV_SCALE_F32 (0x16d): v9, vcc(106) = div_scale_f32(v4, v5, v4)
	EmitVop3B(code, 0x16du, 9u, 106u, 256u + 4u, 256u + 5u, 256u + 4u);
	// V_RCP_F32 (VOP1 op 0x2a): v10 = rcp_f32(v8)
	code.push_back(EncodeVop1(0x2au, 10u, 256u + 8u));
	// V_DIV_FMAS_F32 (0x16f): v11 = div_fmas_f32(v9, v10, v8)
	EmitVop3A(code, 0x16fu, 11u, 256u + 9u, 256u + 10u, 256u + 8u);
	// V_DIV_FIXUP_F32 (0x15f): v12 = div_fixup_f32(v11, v5, v4)
	EmitVop3A(code, 0x15fu, 12u, 256u + 11u, 256u + 5u, 256u + 4u);

	// --- F64 IEEE-754 Division sequence using dynamic v[4:5] (numer) / v[6:7] (denom) ---
	// V_DIV_SCALE_F64 (0x16e): v[14:15], vcc(106) = div_scale_f64(v[6:7], v[6:7], v[4:5])
	EmitVop3B(code, 0x16eu, 14u, 106u, 256u + 6u, 256u + 6u, 256u + 4u);
	// V_RCP_F64 (VOP1 op 0x2f): v[16:17] = rcp_f64(v[14:15])
	code.push_back(EncodeVop1(0x2fu, 16u, 256u + 14u));
	// V_DIV_FMAS_F64 (0x170): v[18:19] = div_fmas_f64(v[4:5], v[16:17], v[14:15])
	EmitVop3A(code, 0x170u, 18u, 256u + 4u, 256u + 16u, 256u + 14u);
	// V_DIV_FIXUP_F64 (0x160): v[20:21] = div_fixup_f64(v[18:19], v[6:7], v[4:5])
	EmitVop3A(code, 0x160u, 20u, 256u + 18u, 256u + 6u, 256u + 4u);
	// V_CVT_F32_F64 (VOP1 op 0x0f): v13 = cvt_f32_f64(v[20:21])
	code.push_back(EncodeVop1(0x0fu, 13u, 256u + 20u));

	// --- Extended S_GETREG_B32 (SOPK op 0x12) hardware register coverage ---
	// MODE (id=1, off=0, size=32), STATUS (id=2), HW_ID1 (id=4), SHADER_CYCLES (id=15), HW_ID2 (id=23), unknown (id=31)
	code.push_back(EncodeSopk(0x12u, 4u, EncodeHwRegSelector(1u, 0u, 32u)));
	code.push_back(EncodeSopk(0x12u, 5u, EncodeHwRegSelector(2u, 0u, 32u)));
	code.push_back(EncodeSopk(0x12u, 6u, EncodeHwRegSelector(4u, 0u, 16u)));
	code.push_back(EncodeSopk(0x12u, 7u, EncodeHwRegSelector(15u, 0u, 20u)));
	code.push_back(EncodeSopk(0x12u, 8u, EncodeHwRegSelector(23u, 0u, 4u)));
	code.push_back(EncodeSopk(0x12u, 9u, EncodeHwRegSelector(31u, 0u, 8u)));

	// Move s4 (MODE) and s7 (SHADER_CYCLES) into v14, v15 so they are live outputs
	// V_MOV_B32 (VOP1 op 0x01)
	code.push_back(EncodeVop1(0x01u, 14u, 4u));
	code.push_back(EncodeVop1(0x01u, 15u, 7u));

	// Store v[12:15] to buffer descriptor s[0:3]
	// BUFFER_STORE_DWORDX4 = 0x1f
	EmitMubuf(code, 0x1fu, 12u, 0u, 0u, 0x80u, true);
	// S_ENDPGM
	code.push_back(EncodeSopp(0x01u, 0x0000u));

	const std::array<uint32_t, 4> user_data {0x00100000u, 0x00000000u, 4096u, 0x00024facu};
	ShaderComputeInputInfo        cs_info {};
	cs_info.threads_num[0]     = 64u;
	cs_info.threads_num[1]     = 1u;
	cs_info.threads_num[2]     = 1u;
	cs_info.wave_size          = 64u;
	cs_info.host_subgroup_size = 32u;

	CompileOptions opts {};
	opts.stage              = ShaderType::Compute;
	opts.wave_size          = 64u;
	opts.user_data          = user_data;
	opts.input_info.compute = &cs_info;

	auto translated = TranslateProgram(code, opts);
	IR::ResourceSpecialization spec {};
	spec.buffers.resize(translated.program.info.buffers.size());
	auto compiled = CompileProgram(std::move(translated), opts, spec);

	if (compiled.spirv.empty()) {
		std::fprintf(stderr, "[FAIL] Suite 1 CompileProgram produced empty SPIR-V\n");
		return false;
	}

	std::string val_error;
	if (!ValidateSpirv(compiled.spirv, val_error)) {
		std::fprintf(stderr, "[FAIL] Suite 1 spirv-val failed:\n%s\n", val_error.c_str());
		return false;
	}

	std::printf("[PASS] Suite 1: RDNA 2 V_DIV_SCALE/FMAS/FIXUP (F32+F64) + Extended S_GETREG_B32 "
	            "-> SPIR-V (%zu words) passed Khronos spirv-val (SPV_ENV_VULKAN_1_3)\n",
	            compiled.spirv.size());
	return true;
}

// -----------------------------------------------------------------------------
// Suite 2: Real libSceNgs2 System, Racks (Mastering/Reverb/Sampler), VoiceState & Render
// -----------------------------------------------------------------------------
struct TestNgs2SystemOption {
	size_t    size                     = 0;
	char      name[64]                 = {};
	uintptr_t job_scheduler_options[4] = {};
	uint32_t  flags                    = 0;
	uint32_t  max_grain_samples        = 0;
	uint32_t  num_grain_samples        = 0;
	uint32_t  sample_rate              = 0;
	uint32_t  max_voice_channels       = 0;
	uint32_t  reserved[5]              = {};
};

struct TestNgs2ContextBufferInfo {
	void*     host_buffer      = nullptr;
	size_t    host_buffer_size = 0;
	uintptr_t reserved[5]      = {};
	uintptr_t user_data        = 0;
};

struct TestNgs2RenderBufferInfo {
	void*    buffer        = nullptr;
	size_t   buffer_size   = 0;
	uint32_t waveform_type = 0;
	uint32_t num_channels  = 0;
};

struct TestNgs2VoiceState {
	uint32_t state_flags = 0;
	uint32_t pad[15]     = {};
};

bool TestNgs2VoiceAndMasteringLifecycle() {
	using namespace Libs::Audio::Ngs2;

	TestNgs2SystemOption sys_opt {};
	if (Ngs2SystemResetOption(reinterpret_cast<Ngs2SystemOption*>(&sys_opt)) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 Ngs2SystemResetOption failed\n");
		return false;
	}

	TestNgs2ContextBufferInfo sys_buf {};
	if (Ngs2SystemQueryBufferSize(reinterpret_cast<const Ngs2SystemOption*>(&sys_opt),
	                              reinterpret_cast<Ngs2ContextBufferInfo*>(&sys_buf)) != OK ||
	    sys_buf.host_buffer_size == 0) {
		std::fprintf(stderr, "[FAIL] Suite 2 Ngs2SystemQueryBufferSize failed\n");
		return false;
	}

	std::vector<uint8_t> sys_mem(sys_buf.host_buffer_size + 64, 0);
	sys_buf.host_buffer = sys_mem.data();

	uintptr_t sys_handle = 0;
	if (Ngs2SystemCreate(reinterpret_cast<const Ngs2SystemOption*>(&sys_opt),
	                     reinterpret_cast<const Ngs2ContextBufferInfo*>(&sys_buf),
	                     &sys_handle) != OK ||
	    sys_handle == 0) {
		std::fprintf(stderr, "[FAIL] Suite 2 Ngs2SystemCreate failed\n");
		return false;
	}

	// Create Mastering (0x3000), Reverb (0x2001), and Sampler (0x1000) racks with default options
	const uint32_t              rack_ids[3] = {0x3000u, 0x2001u, 0x1000u};
	uintptr_t                   racks[3]    = {};
	std::vector<uint8_t>        rack_mems[3];

	for (int i = 0; i < 3; ++i) {
		TestNgs2ContextBufferInfo rack_buf {};
		if (Ngs2RackQueryBufferSize(rack_ids[i], nullptr,
		                            reinterpret_cast<Ngs2ContextBufferInfo*>(&rack_buf)) != OK ||
		    rack_buf.host_buffer_size == 0) {
			std::fprintf(stderr, "[FAIL] Suite 2 Ngs2RackQueryBufferSize(0x%x) failed\n",
			             rack_ids[i]);
			return false;
		}
		rack_mems[i].assign(rack_buf.host_buffer_size + 64, 0);
		rack_buf.host_buffer = rack_mems[i].data();

		if (Ngs2RackCreate(sys_handle, rack_ids[i], nullptr,
		                   reinterpret_cast<const Ngs2ContextBufferInfo*>(&rack_buf),
		                   &racks[i]) != OK ||
		    racks[i] == 0) {
			std::fprintf(stderr, "[FAIL] Suite 2 Ngs2RackCreate(0x%x) failed\n", rack_ids[i]);
			return false;
		}

		uintptr_t voice_handle = 0;
		if (Ngs2RackGetVoiceHandle(racks[i], 0, &voice_handle) != OK || voice_handle == 0) {
			std::fprintf(stderr, "[FAIL] Suite 2 Ngs2RackGetVoiceHandle(0x%x) failed\n",
			             rack_ids[i]);
			return false;
		}

		// Previously crashed with EXIT("unknown type") on Mastering and Reverb voices!
		TestNgs2VoiceState state {};
		if (Ngs2VoiceGetState(voice_handle, reinterpret_cast<Ngs2VoiceState*>(&state),
		                      sizeof(state)) != OK) {
			std::fprintf(stderr, "[FAIL] Suite 2 Ngs2VoiceGetState(0x%x) failed\n", rack_ids[i]);
			return false;
		}
	}

	// Render one grain into a stereo float output buffer
	std::vector<float>       pcm_out(sys_opt.num_grain_samples * 2, 1.0f);
	TestNgs2RenderBufferInfo render_buf {};
	render_buf.buffer        = pcm_out.data();
	render_buf.buffer_size   = pcm_out.size() * sizeof(float);
	render_buf.waveform_type = 0x18; // float32
	render_buf.num_channels  = 2;

	if (Ngs2SystemRender(sys_handle, reinterpret_cast<const Ngs2RenderBufferInfo*>(&render_buf),
	                     1) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 Ngs2SystemRender failed\n");
		return false;
	}

	// Verify graceful error return on invalid voice handle (must NOT abort)
	TestNgs2VoiceState bad_state {};
	if (Ngs2VoiceGetState(0, reinterpret_cast<Ngs2VoiceState*>(&bad_state), sizeof(bad_state)) ==
	    OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 expected error on null voice handle\n");
		return false;
	}

	for (int i = 2; i >= 0; --i) {
		if (Ngs2RackDestroy(racks[i], nullptr) != OK) {
			std::fprintf(stderr, "[FAIL] Suite 2 Ngs2RackDestroy(0x%x) failed\n", rack_ids[i]);
			return false;
		}
	}
	if (Ngs2SystemDestroy(sys_handle, nullptr) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 Ngs2SystemDestroy failed\n");
		return false;
	}

	std::printf("[PASS] Suite 2: libSceNgs2 System + Mastering/Reverb/Sampler Racks + VoiceState + "
	            "Render + Teardown verified\n");
	return true;
}

// -----------------------------------------------------------------------------
// Suite 3: Real libSceAudio3d Port / Non-blocking Push / Object Reserve / Close
// -----------------------------------------------------------------------------
struct TestAudio3dOpenParameters {
	size_t   size        = 0x20;
	uint32_t granularity = 256;
	uint32_t rate        = 0;
	uint32_t max_objects = 512;
	uint32_t queue_depth = 2;
	uint32_t buffer_mode = 2;
	uint32_t pad         = 0;
};

bool TestAudio3dPortAndObjectLifecycle() {
	using namespace Libs::Audio::Audio3d;

	if (Audio3dInitialize(0) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dInitialize failed\n");
		return false;
	}

	TestAudio3dOpenParameters params {};
	Audio3dGetDefaultOpenParameters(reinterpret_cast<Audio3dOpenParameters*>(&params));
	// Exercise user_id=1000 (default active user) and buffer_mode=1 (both previously crashed!)
	params.buffer_mode = 1;

	uint32_t port_id = 0xffffffffu;
	if (Audio3dPortOpen(1000, reinterpret_cast<const Audio3dOpenParameters*>(&params), &port_id) !=
	        OK ||
	    port_id >= 4u) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dPortOpen failed\n");
		return false;
	}

	// Set known attribute + extended PS5 Tempest attribute (must not abort)
	float reverb_lvl = 0.25f;
	if (Audio3dPortSetAttribute(port_id, 0x10001u, &reverb_lvl, sizeof(reverb_lvl)) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dPortSetAttribute(0x10001) failed\n");
		return false;
	}
	uint32_t ext_attr = 1u;
	if (Audio3dPortSetAttribute(port_id, 0x10008u, &ext_attr, sizeof(ext_attr)) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dPortSetAttribute(0x10008) failed\n");
		return false;
	}

	// Object reserve -> set attributes -> unreserve
	uint32_t obj_id = 0;
	if (Audio3dObjectReserve(port_id, &obj_id) != OK || obj_id == 0) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dObjectReserve failed\n");
		return false;
	}
	uint32_t dummy_attr[4] = {1, 2, 3, 4};
	if (Audio3dObjectSetAttributes(port_id, obj_id, 1, dummy_attr) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dObjectSetAttributes failed\n");
		return false;
	}
	if (Audio3dObjectUnreserve(port_id, obj_id) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dObjectUnreserve failed\n");
		return false;
	}

	// Advance + Non-blocking Push (blocking=0 previously crashed!) + QueueLevel query
	if (Audio3dPortAdvance(port_id) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dPortAdvance failed\n");
		return false;
	}
	if (Audio3dPortPush(port_id, 0) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dPortPush(blocking=0) failed\n");
		return false;
	}
	uint32_t q_level = 0;
	uint32_t q_avail = 0;
	if (Audio3dPortGetQueueLevel(port_id, &q_level, &q_avail) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dPortGetQueueLevel failed\n");
		return false;
	}

	if (Audio3dPortClose(port_id) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dPortClose failed\n");
		return false;
	}
	if (Audio3dTerminate() != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Audio3dTerminate failed\n");
		return false;
	}

	std::printf("[PASS] Suite 3: libSceAudio3d Port (user_id=1000, buffer_mode=1) + Object "
	            "Reserve/Attributes + Non-blocking Push + Close/Terminate verified\n");
	return true;
}

int RunTests() {
	Config::Initialize();
	Config::Load({});

	int failures = 0;
	if (!TestRdna2IeeeDivAndHwReg()) {
		failures++;
	}
	if (!TestNgs2VoiceAndMasteringLifecycle()) {
		failures++;
	}
	if (!TestAudio3dPortAndObjectLifecycle()) {
		failures++;
	}

	if (failures == 0) {
		std::printf("ALL CORE BREAKTHROUGH #4 TESTS PASSED (3/3)\n");
		return 0;
	}
	std::fprintf(stderr, "CORE BREAKTHROUGH #4 TESTS FAILED (%d failures)\n", failures);
	return 1;
}

} // namespace

#if defined(_WIN32)
int wmain() {
	return RunTests();
}
#else
int main() {
	return RunTests();
}
#endif

// Minimal kernel/posix symbols required by kernel/semaphore.cpp in this standalone test binary.
namespace Libs::Posix {
int* KYTY_SYSV_ABI GetErrorAddr() {
	static thread_local int err = 0;
	return &err;
}
} // namespace Libs::Posix

namespace Libs::LibKernel {
struct KernelTimespec;
int PthreadGetCurrentPriorityForKernel() {
	return 700;
}
void KernelDispatchPendingSignalForCurrentThread() {}
int KYTY_SYSV_ABI KernelClockGettime(int, KernelTimespec*) {
	return 0;
}
} // namespace Libs::LibKernel
