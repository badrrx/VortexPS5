// Core Breakthrough #7 Verification Suite:
//   1. RDNA 2 (GFX10.3) V_FREXP_EXP_I32_F64 + V_FREXP_MANT_F64 + V_LDEXP_F64 + V_CMP_CLASS_F64
//      compiled through TranslateProgram -> CompileProgram -> Khronos spirv-val (SPV_ENV_VULKAN_1_3)
//   2. Real libScePad + libSceUserService + libSceSystemService gameplay polling loops & null/unknown
//      argument resilience (zero EXIT_NOT_IMPLEMENTED aborts)

#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "libs/controller.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <vector>

namespace Libs {
void InitUserService_1(Loader::SymbolDatabase* s);
void InitSystemService_1(Loader::SymbolDatabase* s);
} // namespace Libs

namespace {

// -----------------------------------------------------------------------------
// GFX10.3 Encoders
// -----------------------------------------------------------------------------
uint32_t EncodeSopp(uint32_t op, uint16_t simm16) {
	return (0x17fu << 23u) | ((op & 0x7fu) << 16u) | simm16;
}

uint32_t EncodeVop1(uint32_t op, uint32_t vdst, uint32_t src0) {
	return (0x3fu << 25u) | ((vdst & 0xffu) << 17u) | ((op & 0xffu) << 9u) | (src0 & 0x1ffu);
}

uint32_t EncodeVop2(uint32_t op, uint32_t vdst, uint32_t vsrc1, uint32_t src0) {
	return ((op & 0x3fu) << 25u) | ((vdst & 0xffu) << 17u) | ((vsrc1 & 0xffu) << 9u) |
	       (src0 & 0x1ffu);
}

uint32_t EncodeVopc(uint32_t op, uint32_t vsrc1, uint32_t src0) {
	return (0x3eu << 25u) | ((op & 0xffu) << 17u) | ((vsrc1 & 0xffu) << 9u) | (src0 & 0x1ffu);
}

void EmitVop3A(std::vector<uint32_t>& code, uint32_t op, uint32_t vdst, uint32_t src0,
               uint32_t src1, uint32_t src2) {
	const uint32_t w0 = (0x35u << 26u) | ((op & 0x3ffu) << 16u) | (vdst & 0xffu);
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

bool ValidateSpirv(const std::vector<uint32_t>& spirv, std::string& out_error) {
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& pos,
	                             const char* msg) {
		out_error += "line " + std::to_string(pos.line) + ": " + msg + "\n";
	});
	return tools.Validate(spirv.data(), spirv.size());
}

template <typename Fn>
Fn ResolveNid(Loader::SymbolDatabase& db, const char* nid) {
	const auto* rec = db.FindByNid(nid, Loader::SymbolType::Func);
	return rec != nullptr ? reinterpret_cast<Fn>(rec->vaddr) : nullptr;
}

// -----------------------------------------------------------------------------
// Suite 1: RDNA 2 V_FREXP_EXP_I32_F64 + V_FREXP_MANT_F64 + V_LDEXP_F64 + V_CMP_CLASS_F64
// -----------------------------------------------------------------------------
bool TestRdna2F64Completeness() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::ShaderRecompiler;

	std::vector<uint32_t> code;

	// Load 4 dynamic dwords into v[4:7] from buffer descriptor s[0:3]
	EmitMubuf(code, 0x0fu, 4u, 0u, 0u, 0x80u, true);
	code.push_back(EncodeSopp(0x0cu, 0x0000u)); // S_WAITCNT 0

	// V_FREXP_EXP_I32_F64 (VOP1 0x3c): v12 = frexp_exp(v[4:5])
	code.push_back(EncodeVop1(0x3cu, 12u, 256u + 4u));
	// V_FREXP_MANT_F64 (VOP1 0x3d): v[8:9] = frexp_mant(v[4:5])
	code.push_back(EncodeVop1(0x3du, 8u, 256u + 4u));
	// V_LDEXP_F64 (VOP3 0x168): v[10:11] = ldexp(v[8:9], v12)
	EmitVop3A(code, 0x168u, 10u, 256u + 8u, 256u + 12u, 0u);
	// V_CMP_CLASS_F64 (VOPC 0xa8): vcc = cmp_class_f64(v[10:11], v6)
	code.push_back(EncodeVopc(0xa8u, 6u, 256u + 10u));
	// V_CNDMASK_B32 (VOP2 0x01): v13 = vcc ? v10 : v11
	code.push_back(EncodeVop2(0x01u, 13u, 10u, 256u + 11u));
	// V_CVT_F32_F64 (VOP1 0x0f): v14 = cvt_f32_f64(v[10:11])
	code.push_back(EncodeVop1(0x0fu, 14u, 256u + 10u));

	// Store v[12:14] to output buffer
	EmitMubuf(code, 0x1fu, 12u, 0u, 0u, 0x80u, true);
	code.push_back(EncodeSopp(0x01u, 0x0000u)); // S_ENDPGM

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

	std::printf("[PASS] Suite 1: RDNA 2 V_FREXP_EXP_I32_F64 + V_FREXP_MANT_F64 + V_LDEXP_F64 + "
	            "V_CMP_CLASS_F64 -> SPIR-V (%zu words) passed Khronos spirv-val (SPV_ENV_VULKAN_1_3)\n",
	            compiled.spirv.size());
	return true;
}

// -----------------------------------------------------------------------------
// Suite 2: Real libScePad + libSceUserService + libSceSystemService Polling Loops
// -----------------------------------------------------------------------------
struct TestUserServiceLoginUserIdList {
	int user_id[4];
};
struct TestUserServiceEvent {
	uint32_t event_type;
	int      user_id;
};
struct TestSystemServiceStatus {
	int32_t event_num;
	bool    is_system_ui_overlaid;
	bool    is_in_background_execution;
	bool    is_vr_play_area_overlaid;
	uint8_t reserved[127];
};
struct TestSystemServiceEvent {
	int32_t event_type;
	uint8_t data[8192];
};

bool TestPadUserAndSystemServicePolling() {
	// --- 1. libScePad ---
	using namespace Libs::Controller;
	if (PadInit() != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 PadInit failed\n");
		return false;
	}
	const int pad_handle = PadOpen(Config::GetUserId(), 0, 0, nullptr);
	if (pad_handle < 0) {
		std::fprintf(stderr, "[FAIL] Suite 2 PadOpen failed (%d)\n", pad_handle);
		return false;
	}
	alignas(16) uint8_t pad_data_buf[256] {};
	if (PadReadState(pad_handle, reinterpret_cast<PadData*>(pad_data_buf)) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 PadReadState failed\n");
		return false;
	}
	if (PadRead(pad_handle, reinterpret_cast<PadData*>(pad_data_buf), 1) < 0) {
		std::fprintf(stderr, "[FAIL] Suite 2 PadRead(num=1) failed\n");
		return false;
	}
	// Out-of-range num=0 & num=65 previously crashed with EXIT_NOT_IMPLEMENTED!
	if (PadRead(pad_handle, reinterpret_cast<PadData*>(pad_data_buf), 0) == OK ||
	    PadRead(pad_handle, reinterpret_cast<PadData*>(pad_data_buf), 65) == OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 PadRead out-of-range expected error\n");
		return false;
	}
	if (PadClose(pad_handle) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 PadClose failed\n");
		return false;
	}

	// --- 2. libSceUserService ---
	Loader::SymbolDatabase db;
	Libs::InitUserService_1(&db);
	Libs::InitSystemService_1(&db);

	using GetInitialUserFn = int(KYTY_SYSV_ABI*)(int*);
	using GetUserEventFn   = int(KYTY_SYSV_ABI*)(TestUserServiceEvent*);
	using GetLoginListFn   = int(KYTY_SYSV_ABI*)(TestUserServiceLoginUserIdList*);

	auto get_init_user = ResolveNid<GetInitialUserFn>(db, "CdWp0oHWGr0");
	auto get_user_evt  = ResolveNid<GetUserEventFn>(db, "yH17Q6NWtVg");
	auto get_login_lst = ResolveNid<GetLoginListFn>(db, "fPhymKNvK-A");
	if (!get_init_user || !get_user_evt || !get_login_lst) {
		std::fprintf(stderr, "[FAIL] Suite 2 failed to resolve libSceUserService NIDs\n");
		return false;
	}
	int uid = -1;
	if (get_init_user(&uid) != OK || uid != Config::GetUserId()) {
		std::fprintf(stderr, "[FAIL] Suite 2 UserServiceGetInitialUser failed\n");
		return false;
	}
	TestUserServiceLoginUserIdList login_list {};
	if (get_login_lst(&login_list) != OK || login_list.user_id[0] != Config::GetUserId()) {
		std::fprintf(stderr, "[FAIL] Suite 2 UserServiceGetLoginUserIdList failed\n");
		return false;
	}
	// Null pointer resilience (all 3 previously crashed with EXIT_NOT_IMPLEMENTED!)
	if (get_init_user(nullptr) == OK || get_user_evt(nullptr) == OK ||
	    get_login_lst(nullptr) == OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 UserService null-arg resilience failed\n");
		return false;
	}

	// --- 3. libSceSystemService ---
	using ParamGetIntFn    = int(KYTY_SYSV_ABI*)(int, int*);
	using ParamGetStrFn    = int(KYTY_SYSV_ABI*)(int, char*, size_t);
	using ReceiveEventFn   = int(KYTY_SYSV_ABI*)(TestSystemServiceEvent*);
	using GetStatusFn      = int(KYTY_SYSV_ABI*)(TestSystemServiceStatus*);

	auto param_int_fn  = ResolveNid<ParamGetIntFn>(db, "fZo48un7LK4");
	auto param_str_fn  = ResolveNid<ParamGetStrFn>(db, "SsC-m-S9JTA");
	auto recv_evt_fn   = ResolveNid<ReceiveEventFn>(db, "656LMQSrg6U");
	auto get_status_fn = ResolveNid<GetStatusFn>(db, "rPo6tV8D9bM");
	if (!param_int_fn || !param_str_fn || !recv_evt_fn || !get_status_fn) {
		std::fprintf(stderr, "[FAIL] Suite 2 failed to resolve libSceSystemService NIDs\n");
		return false;
	}

	char sys_name[64] {};
	if (param_str_fn(6, sys_name, sizeof(sys_name)) != OK || std::strcmp(sys_name, "Kyty") != 0) {
		std::fprintf(stderr, "[FAIL] Suite 2 SystemServiceParamGetString(SYSTEM_NAME) failed\n");
		return false;
	}
	// Unknown string param_id=999 previously crashed with EXIT("unknown string param_id")!
	char unknown_str[64] {"sentinel"};
	if (param_str_fn(999, unknown_str, sizeof(unknown_str)) != OK || unknown_str[0] != '\0') {
		std::fprintf(stderr, "[FAIL] Suite 2 SystemServiceParamGetString(unknown) failed\n");
		return false;
	}
	TestSystemServiceStatus status {};
	if (get_status_fn(&status) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 SystemServiceGetStatus failed\n");
		return false;
	}
	TestSystemServiceEvent evt {};
	(void)recv_evt_fn(&evt); // OK or NO_EVENT — must not crash

	std::printf("[PASS] Suite 2: libScePad (Open/ReadState/Read/RangeCheck/Close) + "
	            "libSceUserService + libSceSystemService (ParamGetString/Status/Event) verified\n");
	return true;
}

int RunTests() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});
	Libs::Controller::Initialize();

	int failures = 0;
	if (!TestRdna2F64Completeness()) {
		failures++;
	}
	if (!TestPadUserAndSystemServicePolling()) {
		failures++;
	}

	Libs::Controller::Shutdown();

	if (failures == 0) {
		std::printf("ALL CORE BREAKTHROUGH #7 TESTS PASSED (2/2)\n");
		return 0;
	}
	std::fprintf(stderr, "CORE BREAKTHROUGH #7 TESTS FAILED (%d failures)\n", failures);
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
