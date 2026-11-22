// Core Breakthrough #5 Verification Suite:
//   1. RDNA 2 (GFX10.3) V_TRIG_PREOP_F64 + V_QSAD_PK_U16_U8 + V_MQSAD_PK_U16_U8 + V_MQSAD_U32_U8
//      + S_SENDMSG compiled through TranslateProgram -> CompileProgram -> Khronos spirv-val (Vulkan 1.3)
//   2. Real libSceSaveData (via Sony NIDs) Initialize3 -> Mount3 -> SetParam/GetParam -> SaveIcon ->
//      GetMountInfo -> Umount2 -> Terminate against real host directory
//   3. Real libSceNet / libSceNpManager / libSceNpEntitlementAccess init, MAC, offline profile/state,
//      full-game SKU entitlement, and null-argument resilience (no EXIT_NOT_IMPLEMENTED aborts)

#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <vector>

namespace Libs::LibSaveDataNative {
void InitSaveDataNative_1(Loader::SymbolDatabase* s);
}
namespace Libs::LibNpEntitlementAccess {
void InitNet_1_NpEntitlementAccess(Loader::SymbolDatabase* s);
}

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

// -----------------------------------------------------------------------------
// Suite 1: RDNA 2 V_TRIG_PREOP_F64 + V_QSAD/V_MQSAD + S_SENDMSG
// -----------------------------------------------------------------------------
bool TestRdna2MathAndSadOps() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::ShaderRecompiler;

	std::vector<uint32_t> code;

	// Load 4 dynamic dwords into v[4:7] from buffer descriptor s[0:3]
	EmitMubuf(code, 0x0fu, 4u, 0u, 0u, 0x80u, true);
	code.push_back(EncodeSopp(0x0cu, 0x0000u)); // S_WAITCNT 0

	// V_TRIG_PREOP_F64 (0x174): v[8:9] = trig_preop_f64(v[4:5], v6)
	EmitVop3A(code, 0x174u, 8u, 256u + 4u, 256u + 6u, 0u);
	// V_CVT_F32_F64 (VOP1 0x0f): v16 = cvt_f32_f64(v[8:9])
	code.push_back(EncodeVop1(0x0fu, 16u, 256u + 8u));

	// V_QSAD_PK_U16_U8 (0x172): v[10:11] = qsad_pk_u16_u8(v[4:5], v6, v[6:7])
	EmitVop3A(code, 0x172u, 10u, 256u + 4u, 256u + 6u, 256u + 6u);
	// V_MQSAD_PK_U16_U8 (0x173): v[12:13] = mqsad_pk_u16_u8(v[4:5], v7, v[10:11])
	EmitVop3A(code, 0x173u, 12u, 256u + 4u, 256u + 7u, 256u + 10u);
	// V_MQSAD_U32_U8 (0x175): v[20:23] = mqsad_u32_u8(v[12:13], v6, v[4:7])
	EmitVop3A(code, 0x175u, 20u, 256u + 12u, 256u + 6u, 256u + 4u);

	// S_SENDMSG (SOPP 0x10) with MSG_INTERRUPT (1)
	code.push_back(EncodeSopp(0x10u, 0x0001u));

	// Combine v16 + v20..v22 into v[16:19] and store to output buffer
	code.push_back(EncodeVop1(0x01u, 17u, 256u + 20u));
	code.push_back(EncodeVop1(0x01u, 18u, 256u + 21u));
	code.push_back(EncodeVop1(0x01u, 19u, 256u + 22u));
	EmitMubuf(code, 0x1fu, 16u, 0u, 0u, 0x80u, true);
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

	std::printf("[PASS] Suite 1: RDNA 2 V_TRIG_PREOP_F64 + V_QSAD/V_MQSAD + S_SENDMSG -> SPIR-V "
	            "(%zu words) passed Khronos spirv-val (SPV_ENV_VULKAN_1_3)\n",
	            compiled.spirv.size());
	return true;
}

// -----------------------------------------------------------------------------
// Suite 2: Real libSceSaveData Mount -> SetParam/GetParam -> SaveIcon -> Umount
// -----------------------------------------------------------------------------
struct TestSaveDataDirName {
	char data[32];
};
struct TestSaveDataMountPoint {
	char data[16];
};
struct TestSaveDataMount3 {
	int                        user_id;
	int                        pad;
	const TestSaveDataDirName* dir_name;
	uint64_t                   blocks;
	uint64_t                   system_blocks;
	uint32_t                   mount_mode;
	int                        pad2;
	int32_t                    resource;
	uint8_t                    reserved[32];
};
struct TestSaveDataMountResult {
	TestSaveDataMountPoint mount_point;
	uint64_t               required_blocks;
	uint32_t               unused;
	uint32_t               mount_status;
	uint8_t                reserved[28];
	int                    pad;
};
struct TestSaveDataParam {
	char     title[128];
	char     sub_title[128];
	char     detail[1024];
	uint32_t user_param;
	int      pad;
	int64_t  mtime;
	uint8_t  reserved[32];
};
struct TestSaveDataMountInfo {
	uint64_t blocks;
	uint64_t free_blocks;
	uint8_t  reserved[32];
};
struct TestSaveDataIcon {
	void*   buf;
	size_t  buf_size;
	size_t  data_size;
	uint8_t reserved[32];
};

template <typename Fn>
Fn ResolveNid(Loader::SymbolDatabase& db, const char* nid) {
	const auto* rec = db.FindByNid(nid, Loader::SymbolType::Func);
	return rec != nullptr ? reinterpret_cast<Fn>(rec->vaddr) : nullptr;
}

bool TestSaveDataFullLifecycle() {
	Loader::SymbolDatabase db;
	Libs::LibSaveDataNative::InitSaveDataNative_1(&db);

	using InitFn      = int(KYTY_SYSV_ABI*)(const void*);
	using Mount3Fn    = int(KYTY_SYSV_ABI*)(const TestSaveDataMount3*, TestSaveDataMountResult*);
	using SetParamFn  = int(KYTY_SYSV_ABI*)(const TestSaveDataMountPoint*, uint32_t, const void*, size_t);
	using GetParamFn  = int(KYTY_SYSV_ABI*)(const TestSaveDataMountPoint*, uint32_t, void*, size_t, size_t*);
	using GetInfoFn   = int(KYTY_SYSV_ABI*)(const TestSaveDataMountPoint*, TestSaveDataMountInfo*);
	using SaveIconFn  = int(KYTY_SYSV_ABI*)(const TestSaveDataMountPoint*, const TestSaveDataIcon*);
	using Umount2Fn   = int(KYTY_SYSV_ABI*)(uint32_t, const TestSaveDataMountPoint*);
	using DeleteFn    = int(KYTY_SYSV_ABI*)(const void*);
	using TerminateFn = int(KYTY_SYSV_ABI*)();

	auto init_fn      = ResolveNid<InitFn>(db, "TywrFKCoLGY");
	auto mount3_fn    = ResolveNid<Mount3Fn>(db, "ZP4e7rlzOUk");
	auto set_param_fn = ResolveNid<SetParamFn>(db, "85zul--eGXs");
	auto get_param_fn = ResolveNid<GetParamFn>(db, "XgvSuIdnMlw");
	auto get_info_fn  = ResolveNid<GetInfoFn>(db, "65VH0Qaaz6s");
	auto save_icon_fn = ResolveNid<SaveIconFn>(db, "c88Yy54Mx0w");
	auto umount2_fn   = ResolveNid<Umount2Fn>(db, "uW4vfTwMQVo");
	auto term_fn      = ResolveNid<TerminateFn>(db, "yKDy8S5yLA0");
	(void)ResolveNid<DeleteFn>(db, "S1GkePI17zQ");

	if (!init_fn || !mount3_fn || !set_param_fn || !get_param_fn || !get_info_fn || !save_icon_fn ||
	    !umount2_fn || !term_fn) {
		std::fprintf(stderr, "[FAIL] Suite 2 failed to resolve libSceSaveData NIDs\n");
		return false;
	}

	if (init_fn(nullptr) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataInitialize3 failed\n");
		return false;
	}

	TestSaveDataDirName dir {};
	std::snprintf(dir.data, sizeof(dir.data), "CB5_TEST_SLOT");

	TestSaveDataMount3 mount {};
	mount.user_id    = Config::GetUserId();
	mount.dir_name   = &dir;
	mount.blocks     = 64;
	mount.mount_mode = 2u | 32u; // RDWR | CREATE2

	TestSaveDataMountResult mount_res {};
	int rc = mount3_fn(&mount, &mount_res);
	if (rc != OK || mount_res.mount_point.data[0] != '/') {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataMount3 failed (rc=0x%08x)\n",
		             static_cast<uint32_t>(rc));
		return false;
	}

	TestSaveDataParam param {};
	std::snprintf(param.title, sizeof(param.title), "Core Breakthrough 5 Save");
	std::snprintf(param.sub_title, sizeof(param.sub_title), "Slot 0");
	std::snprintf(param.detail, sizeof(param.detail), "Verified host VFS save roundtrip");
	param.user_param = 0x12345678u;

	if (set_param_fn(&mount_res.mount_point, 0u, &param, sizeof(param)) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataSetParam failed\n");
		return false;
	}

	TestSaveDataParam read_param {};
	size_t            got_size = 0;
	if (get_param_fn(&mount_res.mount_point, 0u, &read_param, sizeof(read_param), &got_size) != OK ||
	    std::strcmp(read_param.title, "Core Breakthrough 5 Save") != 0 ||
	    read_param.user_param != 0x12345678u) {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataGetParam mismatch\n");
		return false;
	}

	TestSaveDataMountInfo info {};
	if (get_info_fn(&mount_res.mount_point, &info) != OK || info.blocks != 64) {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataGetMountInfo failed\n");
		return false;
	}

	uint8_t          fake_png[16] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	TestSaveDataIcon icon {};
	icon.buf       = fake_png;
	icon.buf_size  = sizeof(fake_png);
	icon.data_size = sizeof(fake_png);
	if (save_icon_fn(&mount_res.mount_point, &icon) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataSaveIcon failed\n");
		return false;
	}
	// Null icon must return error without aborting
	if (save_icon_fn(nullptr, nullptr) == OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataSaveIcon(nullptr) expected error\n");
		return false;
	}

	if (umount2_fn(0u, &mount_res.mount_point) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataUmount2 failed\n");
		return false;
	}
	if (term_fn() != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 SaveDataTerminate failed\n");
		return false;
	}

	// Clean up test save folder
	std::error_code ec;
	std::filesystem::remove_all("_SaveData", ec);

	std::printf("[PASS] Suite 2: libSceSaveData Initialize3 -> Mount3 -> SetParam/GetParam -> "
	            "SaveIcon -> GetMountInfo -> Umount2 -> Terminate verified\n");
	return true;
}

// -----------------------------------------------------------------------------
// Suite 3: Real libSceNet / libSceNpManager / libSceNpEntitlementAccess
// -----------------------------------------------------------------------------
struct TestNetEtherAddr {
	uint8_t data[6];
};
struct TestNpEntitlementInitParam {
	char reserved[32];
};
struct TestNpEntitlementBootParam {
	char reserved[32];
};

bool TestNpAndNetOfflineLifecycle() {
	using namespace Libs::Network;

	if (Net::NetInit() != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NetInit failed\n");
		return false;
	}

	const int pool_id = Net::NetPoolCreate("cb5_pool", 16384, 0);
	if (pool_id < 0) {
		std::fprintf(stderr, "[FAIL] Suite 3 NetPoolCreate failed (%d)\n", pool_id);
		return false;
	}

	TestNetEtherAddr mac {};
	if (Net::NetGetMacAddress(reinterpret_cast<Net::NetEtherAddr*>(&mac), 0) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NetGetMacAddress failed\n");
		return false;
	}
	bool non_zero_mac = false;
	for (uint8_t b: mac.data) {
		if (b != 0) {
			non_zero_mac = true;
		}
	}
	if (!non_zero_mac) {
		std::fprintf(stderr, "[FAIL] Suite 3 NetGetMacAddress returned all-zero MAC\n");
		return false;
	}
	// Null argument resilience (must return error, NOT abort)
	if (Net::NetGetMacAddress(nullptr, 0) == OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NetGetMacAddress(nullptr) expected error\n");
		return false;
	}
	if (Net::NetPoolDestroy(pool_id) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NetPoolDestroy failed\n");
		return false;
	}

	if (NetCtl::NetCtlInit() != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NetCtlInit failed\n");
		return false;
	}
	if (NetCtl::NetCtlGetNatInfo(nullptr) == OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NetCtlGetNatInfo(nullptr) expected error\n");
		return false;
	}

	// NP Manager offline state & null-argument resilience
	uint32_t np_state = 0xffffffffu;
	if (NpManager::NpGetState(Config::GetUserId(), &np_state) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NpGetState failed\n");
		return false;
	}
	if (NpManager::NpGetState(Config::GetUserId(), nullptr) == OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NpGetState(nullptr) expected error\n");
		return false;
	}
	uint32_t reachability = 0xffffffffu;
	if (NpManager::NpGetNpReachabilityState(Config::GetUserId(), &reachability) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 NpGetNpReachabilityState failed\n");
		return false;
	}

	// NP Entitlement Access (Full-Game SKU verification via Sony NIDs)
	Loader::SymbolDatabase db;
	Libs::LibNpEntitlementAccess::InitNet_1_NpEntitlementAccess(&db);
	using EntInitFn = int(KYTY_SYSV_ABI*)(const TestNpEntitlementInitParam*, TestNpEntitlementBootParam*);
	using EntSkuFn  = int(KYTY_SYSV_ABI*)(uint32_t*);

	auto ent_init = ResolveNid<EntInitFn>(db, "jO8DM8oyego");
	auto ent_sku  = ResolveNid<EntSkuFn>(db, "lPDO62PpJIA");
	if (!ent_init || !ent_sku) {
		std::fprintf(stderr, "[FAIL] Suite 3 failed to resolve NpEntitlementAccess NIDs\n");
		return false;
	}
	TestNpEntitlementInitParam init_p {};
	TestNpEntitlementBootParam boot_p {};
	uint32_t                   sku_flag = 0;
	if (ent_init(&init_p, &boot_p) != OK || ent_sku(&sku_flag) != OK || sku_flag != 3u) {
		std::fprintf(stderr, "[FAIL] Suite 3 NpEntitlementAccess SKU check failed (sku=%u)\n", sku_flag);
		return false;
	}

	std::printf("[PASS] Suite 3: libSceNet (non-zero MAC + Pool) + libSceNetCtl + libSceNpManager "
	            "(state=%u, reach=%u) + libSceNpEntitlementAccess (SKU_FULL=%u) verified\n",
	            np_state, reachability, sku_flag);
	return true;
}

int RunTests() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});
	Libs::LibKernel::FileSystem::Initialize();
	Libs::Network::Initialize();

	int failures = 0;
	if (!TestRdna2MathAndSadOps()) {
		failures++;
	}
	if (!TestSaveDataFullLifecycle()) {
		failures++;
	}
	if (!TestNpAndNetOfflineLifecycle()) {
		failures++;
	}

	Libs::Network::Shutdown();
	Libs::LibKernel::FileSystem::Shutdown();

	if (failures == 0) {
		std::printf("ALL CORE BREAKTHROUGH #5 TESTS PASSED (3/3)\n");
		return 0;
	}
	std::fprintf(stderr, "CORE BREAKTHROUGH #5 TESTS FAILED (%d failures)\n", failures);
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
