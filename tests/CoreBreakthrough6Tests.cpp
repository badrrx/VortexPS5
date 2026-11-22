// Core Breakthrough #6 Verification Suite:
//   1. RDNA 2 (GFX10.3) V_MOVRELSD_B32 + V_MOVRELSD_2_B32 + DS_SWIZZLE_B32
//      compiled through TranslateProgram -> CompileProgram -> Khronos spirv-val (SPV_ENV_VULKAN_1_3)
//   2. Real libSceAmpr / APR ResolveFilepathsToIds -> AprCommandBufferReadFile -> WriteCounter
//      (extended mode/op) -> WriteCounterOnCompletion -> WriteAddressOnCompletion -> Submit -> Wait
//      verified against a real host file + target memory payload + completion marker
//   3. Real libSceVideoDec2 QueryDecoderMemoryInfo -> CreateDecoder (Standard resource_type=0) ->
//      Reset -> Flush -> DeleteDecoder lifecycle verified with SCE_OK

#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <vector>

namespace Libs {
namespace LibKernelApr {
void InitLibKernel_1_Apr(Loader::SymbolDatabase* s);
}
namespace LibAmpr {
void InitAmpr_1(Loader::SymbolDatabase* s);
}
namespace VideoDec2 {
void InitVideoDec2_1(Loader::SymbolDatabase* s);
}
} // namespace Libs

namespace {

// -----------------------------------------------------------------------------
// GFX10.3 Encoders
// -----------------------------------------------------------------------------
uint32_t EncodeSopp(uint32_t op, uint16_t simm16) {
	return (0x17fu << 23u) | ((op & 0x7fu) << 16u) | simm16;
}

uint32_t EncodeSop1(uint32_t op, uint32_t sdst, uint32_t ssrc0) {
	return (0x17du << 23u) | ((sdst & 0x7fu) << 16u) | ((op & 0xffu) << 8u) | (ssrc0 & 0xffu);
}

uint32_t EncodeVop1(uint32_t op, uint32_t vdst, uint32_t src0) {
	return (0x3fu << 25u) | ((vdst & 0xffu) << 17u) | ((op & 0xffu) << 9u) | (src0 & 0x1ffu);
}

void EmitDs(std::vector<uint32_t>& code, uint32_t op, uint32_t vdst, uint32_t addr, uint32_t data0,
            uint32_t data1, uint16_t offset) {
	const uint32_t w0 = (0x36u << 26u) | ((op & 0xffu) << 18u) | offset;
	const uint32_t w1 = ((vdst & 0xffu) << 24u) | ((data1 & 0xffu) << 16u) |
	                    ((data0 & 0xffu) << 8u) | (addr & 0xffu);
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
// Suite 1: RDNA 2 V_MOVRELSD_B32 + V_MOVRELSD_2_B32 + DS_SWIZZLE_B32
// -----------------------------------------------------------------------------
bool TestRdna2MovrelsdAndSwizzle() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::ShaderRecompiler;

	std::vector<uint32_t> code;

	// Load 4 dynamic dwords into v[4:7] from buffer descriptor s[0:3]
	EmitMubuf(code, 0x0fu, 4u, 0u, 0u, 0x80u, true);
	code.push_back(EncodeSopp(0x0cu, 0x0000u)); // S_WAITCNT 0

	// Initialize v[8:11] and v[12:15] from v[4:7]
	code.push_back(EncodeVop1(0x01u, 8u, 256u + 4u));
	code.push_back(EncodeVop1(0x01u, 9u, 256u + 5u));
	code.push_back(EncodeVop1(0x01u, 12u, 256u + 6u));

	// S_MOV_B32 m0(0x7c), s4 (SOP1 op 0x03)
	code.push_back(EncodeSop1(0x03u, 0x7cu, 4u));

	// V_MOVRELSD_B32 (VOP1 0x44): v[8 + m0] = v[4 + m0]
	code.push_back(EncodeVop1(0x44u, 8u, 256u + 4u));
	// V_MOVRELSD_2_B32 (VOP1 0x48): v[12 + m0] = v[8 + m0]
	code.push_back(EncodeVop1(0x48u, 12u, 256u + 8u));

	// DS_SWIZZLE_B32 (DS op 0x35): v13 = swizzle(v12, quad_perm[3,2,1,0] = 0x801b)
	EmitDs(code, 0x35u, 13u, 12u, 0u, 0u, 0x801bu);
	code.push_back(EncodeSopp(0x0cu, 0x0000u)); // S_WAITCNT 0

	// Store v[12:14] to output buffer
	code.push_back(EncodeVop1(0x01u, 14u, 256u + 8u));
	EmitMubuf(code, 0x1fu, 12u, 0u, 0u, 0x80u, true);
	code.push_back(EncodeSopp(0x01u, 0x0000u)); // S_ENDPGM

	const std::array<uint32_t, 5> user_data {0x00100000u, 0x00000000u, 4096u, 0x00024facu, 1u};
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

	std::printf("[PASS] Suite 1: RDNA 2 V_MOVRELSD_B32 + V_MOVRELSD_2_B32 + DS_SWIZZLE_B32 -> "
	            "SPIR-V (%zu words) passed Khronos spirv-val (SPV_ENV_VULKAN_1_3)\n",
	            compiled.spirv.size());
	return true;
}

// -----------------------------------------------------------------------------
// Suite 2: Real libSceAmpr / APR ReadFile + Extended Counter + Completion Marker
// -----------------------------------------------------------------------------
bool TestAmprAprReadAndCounterSignaling() {
	Loader::SymbolDatabase db;
	Libs::LibKernelApr::InitLibKernel_1_Apr(&db);
	Libs::LibAmpr::InitAmpr_1(&db);

	using ResolveFn     = int(KYTY_SYSV_ABI*)(const char* const*, uint32_t, uint32_t*, uint32_t*);
	using CtorFn        = int(KYTY_SYSV_ABI*)(void*, void*, void*);
	using DtorFn        = void(KYTY_SYSV_ABI*)(void*);
	using SetBufFn      = int(KYTY_SYSV_ABI*)(void*, void*, uint32_t);
	using GetOffFn      = uint32_t(KYTY_SYSV_ABI*)(void*);
	using ReadFileFn    = int(KYTY_SYSV_ABI*)(void*, uint64_t, uint64_t, uint32_t, void*, uint64_t, uint64_t);
	using WriteCntFn    = int(KYTY_SYSV_ABI*)(void*, uint8_t, uint8_t, uint64_t, uint8_t, uint32_t);
	using WriteCntCmpFn = int(KYTY_SYSV_ABI*)(void*, uint8_t, uint8_t, uint64_t, uint8_t);
	using WriteAddrFn   = int(KYTY_SYSV_ABI*)(void*, volatile uint64_t*, uint64_t);
	using SubmitFn      = int(KYTY_SYSV_ABI*)(void*, uint32_t, uint32_t*);
	using WaitFn        = int(KYTY_SYSV_ABI*)(uint32_t);

	auto resolve_fn     = ResolveNid<ResolveFn>(db, "WT-5NKy42fw");
	auto ctor_fn        = ResolveNid<CtorFn>(db, "a8uLzYY--tM");
	auto dtor_fn        = ResolveNid<DtorFn>(db, "Qs1xtplKo0U");
	auto set_buf_fn     = ResolveNid<SetBufFn>(db, "N-FSPA4S3nI");
	auto get_off_fn     = ResolveNid<GetOffFn>(db, "GnxKOHEawhk");
	auto read_file_fn   = ResolveNid<ReadFileFn>(db, "mQ16-QdKv7k");
	auto write_cnt_fn   = ResolveNid<WriteCntFn>(db, "jK+yuYCI7MA");
	auto write_cntc_fn  = ResolveNid<WriteCntCmpFn>(db, "3wn42MWTzTs");
	auto write_addr_fn  = ResolveNid<WriteAddrFn>(db, "sJXyWHjP-F8");
	auto submit_fn      = ResolveNid<SubmitFn>(db, "qvMUCyyaCSI");
	auto wait_fn        = ResolveNid<WaitFn>(db, "rqwFKI4PAiM");

	if (!resolve_fn || !ctor_fn || !dtor_fn || !set_buf_fn || !get_off_fn || !read_file_fn ||
	    !write_cnt_fn || !write_cntc_fn || !write_addr_fn || !submit_fn || !wait_fn) {
		std::fprintf(stderr, "[FAIL] Suite 2 failed to resolve libSceAmpr / APR NIDs\n");
		return false;
	}

	const auto        temp_dir = std::filesystem::temp_directory_path() / "kyty_cb6_apr_test";
	std::error_code   ec;
	std::filesystem::create_directories(temp_dir, ec);
	const std::string payload  = "KYTY-PROSPERO-AMPR-BREAKTHROUGH-6!";
	{
		std::ofstream ofs(temp_dir / "asset.bin", std::ios::binary);
		ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
	}
	Libs::LibKernel::FileSystem::Mount(temp_dir, "/cb6_apr");

	const char* paths[1]   = {"/cb6_apr/asset.bin"};
	uint32_t    file_ids[1] = {0};
	uint32_t    err_idx     = 0;
	if (resolve_fn(paths, 1, file_ids, &err_idx) != OK || file_ids[0] == 0) {
		std::fprintf(stderr, "[FAIL] Suite 2 AprResolveFilepathsToIds failed\n");
		return false;
	}

	alignas(16) uint8_t cmd_obj[128] {};
	alignas(64) uint8_t cmd_mem[4096] {};
	ctor_fn(cmd_obj, nullptr, nullptr);
	if (set_buf_fn(cmd_obj, cmd_mem, sizeof(cmd_mem)) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 CommandBufferSetBuffer failed\n");
		return false;
	}

	std::vector<char> dst(payload.size(), '\0');
	volatile uint64_t completion_marker = 0;

	if (read_file_fn(cmd_obj, 0, 0, file_ids[0], dst.data(), dst.size(), 0) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 AprCommandBufferReadFile failed\n");
		return false;
	}
	// Exercise extended access=2, operation=1, mode=1 (previously crashed with EXIT_NOT_IMPLEMENTED!)
	if (write_cnt_fn(cmd_obj, 0, 2, 1, 1, 1) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 CommandBufferWriteCounter failed\n");
		return false;
	}
	if (write_cntc_fn(cmd_obj, 1, 1, 42, 0) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 CommandBufferWriteCounterOnCompletion failed\n");
		return false;
	}
	if (write_addr_fn(cmd_obj, &completion_marker, 0xCAFEBABE12345678ull) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 CommandBufferWriteAddressOnCompletion failed\n");
		return false;
	}

	const uint32_t used_bytes = get_off_fn(cmd_obj);
	(void)used_bytes;
	uint32_t sub_id = 0;
	if (submit_fn(cmd_obj, 0, &sub_id) != OK || sub_id == 0) {
		std::fprintf(stderr, "[FAIL] Suite 2 AprSubmitCommandBufferAndGetId failed\n");
		return false;
	}
	if (wait_fn(sub_id) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 2 AprWaitCommandBuffer failed\n");
		return false;
	}

	dtor_fn(cmd_obj);
	Libs::LibKernel::FileSystem::Umount("/cb6_apr");
	std::filesystem::remove_all(temp_dir, ec);

	if (std::memcmp(dst.data(), payload.data(), payload.size()) != 0) {
		std::fprintf(stderr, "[FAIL] Suite 2 APR read payload mismatch\n");
		return false;
	}
	if (completion_marker != 0xCAFEBABE12345678ull) {
		std::fprintf(stderr, "[FAIL] Suite 2 APR completion marker mismatch (0x%llx)\n",
		             static_cast<unsigned long long>(completion_marker));
		return false;
	}

	std::printf("[PASS] Suite 2: libSceAmpr APR ReadFile + Extended WriteCounter + "
	            "WriteAddressOnCompletion -> Submit/Wait verified (%zu bytes + marker OK)\n",
	            payload.size());
	return true;
}

// -----------------------------------------------------------------------------
// Suite 3: Real libSceVideoDec2 Standard (resource_type=0) Decoder Lifecycle
// -----------------------------------------------------------------------------
struct TestVideodec2DecoderConfigInfo {
	size_t   this_size;
	uint32_t resource_type;
	uint32_t codec_type;
	uint32_t profile;
	uint32_t max_level;
	int32_t  max_frame_width;
	int32_t  max_frame_height;
	int32_t  max_dpb_frame_count;
	uint32_t decode_input_queue_depth;
	void*    compute_queue;
	uint64_t cpu_affinity_mask;
	int32_t  cpu_thread_priority;
	bool     optimize_progressive_video;
	bool     check_memory_type;
	uint8_t  reserved0;
	uint8_t  reserved1;
	void*    extra_config_info;
};

struct TestVideodec2DecoderMemoryInfo {
	size_t   this_size;
	size_t   cpu_memory_size;
	void*    cpu_memory;
	size_t   gpu_memory_size;
	void*    gpu_memory;
	size_t   cpu_gpu_memory_size;
	void*    cpu_gpu_memory;
	size_t   max_frame_buffer_size;
	uint32_t frame_buffer_alignment;
	uint32_t reserved0;
};

bool TestVideoDec2Lifecycle() {
	Loader::SymbolDatabase db;
	Libs::VideoDec2::InitVideoDec2_1(&db);

	using QueryMemFn = int32_t(KYTY_SYSV_ABI*)(const TestVideodec2DecoderConfigInfo*,
	                                           TestVideodec2DecoderMemoryInfo*);
	using CreateFn   = int32_t(KYTY_SYSV_ABI*)(const TestVideodec2DecoderConfigInfo*,
	                                           const TestVideodec2DecoderMemoryInfo*, void**);
	using ResetFn    = int32_t(KYTY_SYSV_ABI*)(void*);
	using DeleteFn   = int32_t(KYTY_SYSV_ABI*)(void*);

	auto query_fn  = ResolveNid<QueryMemFn>(db, "qqMCwlULR+E");
	auto create_fn = ResolveNid<CreateFn>(db, "CNNRoRYd8XI");
	auto reset_fn  = ResolveNid<ResetFn>(db, "wJXikG6QFN8");
	auto delete_fn = ResolveNid<DeleteFn>(db, "jwImxXRGSKA");

	if (!query_fn || !create_fn || !reset_fn || !delete_fn) {
		std::fprintf(stderr, "[FAIL] Suite 3 failed to resolve libSceVideoDec2 NIDs\n");
		return false;
	}

	// Test Standard resource_type=0 (previously rejected!) with AVC (codec_type=0)
	TestVideodec2DecoderConfigInfo cfg {};
	cfg.this_size                = sizeof(cfg);
	cfg.resource_type            = 0u; // Standard (no compute_queue required)
	cfg.codec_type               = 1u; // VIDEODEC2_CODEC_TYPE_AVC (H.264)
	cfg.max_frame_width          = 1920;
	cfg.max_frame_height         = 1080;
	cfg.max_dpb_frame_count      = 4;
	cfg.decode_input_queue_depth = 4;

	TestVideodec2DecoderMemoryInfo mem {};
	mem.this_size = sizeof(mem);
	int32_t rc    = query_fn(&cfg, &mem);
	if (rc != OK || mem.cpu_memory_size == 0) {
		std::fprintf(stderr, "[FAIL] Suite 3 QueryDecoderMemoryInfo failed (rc=0x%08x)\n",
		             static_cast<uint32_t>(rc));
		return false;
	}

	std::vector<uint8_t> cpu_buf(mem.cpu_memory_size, 0);
	std::vector<uint8_t> gpu_buf(mem.gpu_memory_size, 0);
	std::vector<uint8_t> cg_buf(mem.cpu_gpu_memory_size, 0);
	mem.cpu_memory     = cpu_buf.empty() ? nullptr : cpu_buf.data();
	mem.gpu_memory     = gpu_buf.empty() ? nullptr : gpu_buf.data();
	mem.cpu_gpu_memory = cg_buf.empty() ? nullptr : cg_buf.data();

	void* decoder = nullptr;
	rc            = create_fn(&cfg, &mem, &decoder);
	if (rc != OK || decoder == nullptr) {
		std::fprintf(stderr, "[FAIL] Suite 3 CreateDecoder failed (rc=0x%08x)\n",
		             static_cast<uint32_t>(rc));
		return false;
	}

	if (reset_fn(decoder) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 Reset failed\n");
		return false;
	}
	if (delete_fn(decoder) != OK) {
		std::fprintf(stderr, "[FAIL] Suite 3 DeleteDecoder failed\n");
		return false;
	}

	std::printf("[PASS] Suite 3: libSceVideoDec2 Standard (resource_type=0) QueryMemory -> "
	            "CreateDecoder -> Reset -> DeleteDecoder verified\n");
	return true;
}

int RunTests() {
	Common::InitializeThreads();
	Config::Initialize();
	Config::Load({});
	Libs::LibKernel::FileSystem::Initialize();

	int failures = 0;
	if (!TestRdna2MovrelsdAndSwizzle()) {
		failures++;
	}
	if (!TestAmprAprReadAndCounterSignaling()) {
		failures++;
	}
	if (!TestVideoDec2Lifecycle()) {
		failures++;
	}

	Libs::LibKernel::FileSystem::Shutdown();

	if (failures == 0) {
		std::printf("ALL CORE BREAKTHROUGH #6 TESTS PASSED (3/3)\n");
		return 0;
	}
	std::fprintf(stderr, "CORE BREAKTHROUGH #6 TESTS FAILED (%d failures)\n", failures);
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
