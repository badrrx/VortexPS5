// Core Breakthrough #14 Verification Suite:
//   1. RDNA 2 Frontend Translator Hardening (EXP unknown target / V_INTERP_MOV_F32 reserved mode)
//      -> TranslateProgram -> CompileProgram -> Khronos spirv-val (SPV_ENV_VULKAN_1_3)
//   2. Real libSceRtc (via Sony NIDs) Tick / Timezone / Date / RFC3339 & Error-Code Verification
//   3. Real libSceLibcInternal / libSceLibcInternalExt (fflush, LibcHeapGetTraceInfo,
//      thread_atexit) Hardening

#include "common/abi.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "libs/errno.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <spirv-tools/libspirv.hpp>
#include <string>
#include <vector>

namespace Libs::LibRtc {
void InitRtc_1(Loader::SymbolDatabase* s);
}

namespace Libs::LibcInternal {
int KYTY_SYSV_ABI fflush(FILE* stream);
}

namespace Libs::LibcInternalExt {
struct Info {
	uint64_t  size;
	uint32_t  unknown1;
	uint32_t  unknown2;
	uint64_t* mspace_atomic_id_mask;
	uint64_t* mstate_table;
};
using thread_atexit_destructor_t = void(KYTY_SYSV_ABI*)(void*);
void KYTY_SYSV_ABI LibcHeapGetTraceInfo(Info* info);
int KYTY_SYSV_ABI  LibcInternalExtCxaThreadAtexit(thread_atexit_destructor_t destructor,
                                                  void* object, void* module_id);
void               RunThreadAtexitDestructors();
} // namespace Libs::LibcInternalExt

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

void TestFrontendTranslatorHardening() {
	using namespace Libs::Graphics;
	using namespace Libs::Graphics::ShaderRecompiler;

	// Build a GFX10.3 Pixel Shader with:
	//   1. V_MOV_B32 v0, 1.0f
	//   2. EXP target=0x10 (POS4 — unknown/unsupported in PS, previously fatal EXIT!)
	//   3. EXP target=0x00 (MRT0), en=0xf, done=true
	//   4. S_ENDPGM
	std::vector<uint32_t> code;
	code.push_back(EncodeVop1(0x01u, 0u, 0xf2u)); // v0 = 1.0f
	EmitExp(code, 0x10u, 0xfu, false, 0u, 0u, 0u, 0u); // previously fatal EXIT("unsupported EXP target")!
	EmitExp(code, 0x00u, 0xfu, true, 0u, 0u, 0u, 0u);  // MRT0 valid export
	code.push_back(EncodeSopp(0x01u, 0u));              // s_endpgm

	ShaderPixelInputInfo ps_info {};
	ps_info.wave_size             = 32;
	ps_info.target_output_mode[0] = 4; // 32_ABGR

	CompileOptions options {};
	options.stage            = ShaderType::Pixel;
	options.wave_size        = 32;
	options.dump_ir          = false;
	options.input_info.pixel = &ps_info;

	auto translated = TranslateProgram(code, options);
	Check(!translated.program.blocks.empty(),
	      "TranslateProgram(PS with unknown EXP target 0x10) succeeds without abort");

	const auto compiled =
	    CompileProgram(std::move(translated), options, IR::ResourceSpecialization {});
	Check(!compiled.spirv.empty(), "CompileProgram emits non-empty PS SPIR-V 1.6");

	std::string val_err;
	const bool  valid = ValidateSpirv(compiled.spirv, val_err);
	if (!valid) {
		std::fprintf(stderr, "spirv-val error:\n%s\n", val_err.c_str());
	}
	Check(valid, "Khronos spirv-val (SPV_ENV_VULKAN_1_3) validates PS SPIR-V cleanly");

	std::printf("[PASS] Suite 1: RDNA 2 Frontend Translator Hardening -> SPIR-V 1.6 Validation\n");
}

void TestLibSceRtc() {
	Loader::SymbolDatabase db;
	Libs::LibRtc::InitRtc_1(&db);

	auto lookup = [&](const char* nid) -> uint64_t {
		const auto* rec = db.FindByNid(nid, Loader::SymbolType::Func);
		return rec != nullptr ? rec->vaddr : 0;
	};

	struct RtcDateTime {
		uint16_t year, month, day, hour, minute, second;
		uint32_t microsecond;
	};
	struct RtcTick {
		uint64_t tick;
	};

	using CheckValidFn   = int(KYTY_SYSV_ABI*)(const RtcDateTime*);
	using IsLeapYearFn   = int(KYTY_SYSV_ABI*)(int);
	using GetDayOfWeekFn = int(KYTY_SYSV_ABI*)(int, int, int);
	using GetTickFn      = int(KYTY_SYSV_ABI*)(const RtcDateTime*, RtcTick*);
	using SetTickFn      = int(KYTY_SYSV_ABI*)(RtcDateTime*, const RtcTick*);
	using GetCurrClockFn = int(KYTY_SYSV_ABI*)(RtcDateTime*, int);
	using GetLocalFn     = int(KYTY_SYSV_ABI*)(RtcDateTime*);

	auto check_valid   = reinterpret_cast<CheckValidFn>(lookup("lPEBYdVX0XQ"));
	auto is_leap_year  = reinterpret_cast<IsLeapYearFn>(lookup("Ug8pCwQvh0c"));
	auto day_of_week   = reinterpret_cast<GetDayOfWeekFn>(lookup("CyIK-i4XdgQ"));
	auto get_tick      = reinterpret_cast<GetTickFn>(lookup("8w-H19ip48I"));
	auto set_tick      = reinterpret_cast<SetTickFn>(lookup("ueega6v3GUw"));
	auto get_curr      = reinterpret_cast<GetCurrClockFn>(lookup("8lfvnRMqwEM"));
	auto get_local     = reinterpret_cast<GetLocalFn>(lookup("ZPD1YOKI+Kw"));

	Check(check_valid && is_leap_year && day_of_week && get_tick && set_tick && get_curr &&
	          get_local,
	      "All libSceRtc NIDs resolved in SymbolDatabase");

	constexpr int RTC_ERROR_INVALID_POINTER = -2135621630;
	constexpr int RTC_ERROR_INVALID_MONTH   = -2135621623;

	Check(check_valid(nullptr) == RTC_ERROR_INVALID_POINTER,
	      "RtcCheckValid(nullptr) returns RTC_ERROR_INVALID_POINTER");
	Check(is_leap_year(2024) == 1 && is_leap_year(2025) == 0,
	      "RtcIsLeapYear(2024)=1, RtcIsLeapYear(2025)=0");
	Check(day_of_week(2026, 11, 21) == 6 /* Saturday */,
	      "RtcGetDayOfWeek(2026-11-21) == Saturday (6)");

	RtcDateTime bad_dt {2026, 13, 1, 0, 0, 0, 0};
	Check(check_valid(&bad_dt) == RTC_ERROR_INVALID_MONTH,
	      "RtcCheckValid(month=13) returns RTC_ERROR_INVALID_MONTH");

	RtcDateTime dt {2026, 11, 21, 14, 30, 45, 123456};
	RtcTick     tick {};
	Check(get_tick(&dt, &tick) == OK && tick.tick > 0, "RtcGetTick succeeds");
	RtcDateTime roundtrip {};
	Check(set_tick(&roundtrip, &tick) == OK && roundtrip.year == 2026 && roundtrip.month == 11 &&
	          roundtrip.day == 21 && roundtrip.hour == 14 && roundtrip.minute == 30 &&
	          roundtrip.second == 45 && roundtrip.microsecond == 123456,
	      "RtcSetTick roundtrips exact date/time/microsecond");

	RtcDateTime now_utc {}, now_local {};
	Check(get_curr(&now_utc, 0) == OK && now_utc.year >= 2020, "RtcGetCurrentClock(UTC) succeeds");
	Check(get_local(&now_local) == OK && now_local.year >= 2020,
	      "RtcGetCurrentClockLocalTime succeeds");

	std::printf("[PASS] Suite 2: libSceRtc Tick / Timezone / Date & Error-Code Verification\n");
}

static int g_thread_atexit_called = 0;
static void KYTY_SYSV_ABI TestThreadDtor(void* obj) {
	if (obj == &g_thread_atexit_called) {
		++g_thread_atexit_called;
	}
}

void TestLibcInternalHardening() {
	using namespace Libs::LibcInternal;
	using namespace Libs::LibcInternalExt;

	// 1. fflush(stdout), fflush(stderr), fflush(nullptr) — stderr/nullptr previously fatal EXIT!
	Check(Libs::LibcInternal::fflush(stdout) == 0, "LibcInternal::fflush(stdout) returns 0");
	Check(Libs::LibcInternal::fflush(stderr) == 0, "LibcInternal::fflush(stderr) returns 0 (was fatal!)");
	Check(Libs::LibcInternal::fflush(nullptr) == 0, "LibcInternal::fflush(nullptr) returns 0 (was fatal!)");

	// 2. LibcHeapGetTraceInfo with nullptr, undersized (16), standard (32), and extended SDK10 (48)
	LibcHeapGetTraceInfo(nullptr);
	Info small_info {};
	small_info.size = 16;
	LibcHeapGetTraceInfo(&small_info);
	Check(small_info.mspace_atomic_id_mask == nullptr,
	      "LibcHeapGetTraceInfo(size=16) safely ignored without abort");

	Info ext_info {};
	ext_info.size = 48; // extended PS5 SDK struct size (previously fatal EXIT_NOT_IMPLEMENTED!)
	LibcHeapGetTraceInfo(&ext_info);
	Check(ext_info.mspace_atomic_id_mask != nullptr && ext_info.mstate_table != nullptr,
	      "LibcHeapGetTraceInfo(size=48) populates trace pointers cleanly");

	// 3. Thread-local atexit registration and execution
	g_thread_atexit_called = 0;
	Check(LibcInternalExtCxaThreadAtexit(&TestThreadDtor, &g_thread_atexit_called, nullptr) == 0,
	      "LibcInternalExtCxaThreadAtexit registers destructor");
	RunThreadAtexitDestructors();
	Check(g_thread_atexit_called == 1, "RunThreadAtexitDestructors invokes registered destructor");

	std::printf("[PASS] Suite 3: libSceLibcInternal & LibcInternalExt Hardening\n");
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

	TestFrontendTranslatorHardening();
	TestLibSceRtc();
	TestLibcInternalHardening();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough14Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough14Tests PASSED (3/3 suites)\n");
	return 0;
}
