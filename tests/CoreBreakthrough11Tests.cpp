// Core Breakthrough #11 Verification Suite:
//   1. Dynamic ELF/PRX Loader, TLS & Relocation Hardening (runtimeLinker.cpp)
//   2. libSceAudioOut Multi-Port & Invalid-Param Hardening (audio.cpp)
//   3. libSceAudioOut2 Multi-Port, 3D Object Port & Spatializer Hardening (libAudio2.cpp)

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "libs/audio.h"
#include "libs/errno.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace Loader {
void EvaluateRelocationForTest(Elf64_Rela* r, Program* program, uint64_t* out_value,
                               bool* out_resolved);
} // namespace Loader

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestLoaderRelocationAndTlsHardening() {
	using namespace Loader;

	RuntimeLinker rt;
	Program       prog {};
	prog.rt           = &rt;
	prog.base_vaddr   = 0x0000000100000000ULL;
	prog.file_name    = "cb11_test.prx";
	prog.dynamic_info = std::make_unique<DynamicInfo>();

	// Construct a small real Elf64_Sym table + string table
	static char strtab[] = "\0local_tls_var\0unknown_sym\0";
	Elf64_Sym         syms[5] {};
	// sym[1]: STB_LOCAL | STT_TLS, st_value = 0x120
	syms[1].st_name  = 1;
	syms[1].st_info  = static_cast<uint8_t>((STB_LOCAL << 4u) | STT_TLS);
	syms[1].st_value = 0x120;
	// sym[2]: STB_LOCAL | STT_GNU_IFUNC, st_value = 0x400
	syms[2].st_name  = 1;
	syms[2].st_info  = static_cast<uint8_t>((STB_LOCAL << 4u) | STT_GNU_IFUNC);
	syms[2].st_value = 0x400;
	// sym[3]: STB_LOCAL | unknown type (15), st_value = 0x800
	syms[3].st_name  = 1;
	syms[3].st_info  = static_cast<uint8_t>((STB_LOCAL << 4u) | 15u);
	syms[3].st_value = 0x800;
	// sym[4]: unknown bind (14) | STT_COMMON, st_name = "unknown_sym"
	syms[4].st_name  = 15;
	syms[4].st_info  = static_cast<uint8_t>((14u << 4u) | STT_COMMON);
	syms[4].st_value = 0;

	prog.dynamic_info->symbol_table = syms;
	prog.dynamic_info->str_table    = strtab;

	auto make_rela = [](uint64_t offset, uint32_t sym_idx, uint32_t type, int64_t addend) {
		Elf64_Rela r {};
		r.r_offset = offset;
		r.r_info   = (static_cast<uint64_t>(sym_idx) << 32u) | type;
		r.r_addend = addend;
		return r;
	};

	uint64_t val      = 0;
	bool     resolved = false;

	// 1. R_X86_64_NONE — previously fatal EXIT("unknown type: 0")!
	auto r_none = make_rela(0x10, 0, R_X86_64_NONE, 0);
	EvaluateRelocationForTest(&r_none, &prog, &val, &resolved);
	Check(resolved && val == 0, "R_X86_64_NONE resolves cleanly to 0");

	// 2. R_X86_64_RELATIVE
	auto r_rel = make_rela(0x20, 0, R_X86_64_RELATIVE, 0x555);
	EvaluateRelocationForTest(&r_rel, &prog, &val, &resolved);
	Check(resolved && val == prog.base_vaddr + 0x555, "R_X86_64_RELATIVE computes base + addend");

	// 3. R_X86_64_DTPMOD64
	auto r_dtpmod = make_rela(0x30, 1, R_X86_64_DTPMOD64, 0);
	EvaluateRelocationForTest(&r_dtpmod, &prog, &val, &resolved);
	Check(resolved && val == reinterpret_cast<uint64_t>(&prog),
	      "R_X86_64_DTPMOD64 resolves to program handle");

	// 4. R_X86_64_DTPOFF64 & R_X86_64_TPOFF64 — previously fatal EXIT!
	auto r_dtpoff = make_rela(0x38, 1, R_X86_64_DTPOFF64, 0x10);
	EvaluateRelocationForTest(&r_dtpoff, &prog, &val, &resolved);
	Check(resolved && val == 0x130, "R_X86_64_DTPOFF64 computes sym.st_value + addend (0x130)");

	auto r_tpoff = make_rela(0x40, 1, R_X86_64_TPOFF64, 0x20);
	EvaluateRelocationForTest(&r_tpoff, &prog, &val, &resolved);
	Check(resolved && val == 0x140, "R_X86_64_TPOFF64 computes sym.st_value + addend (0x140)");

	// 5. STT_GNU_IFUNC local symbol — previously fatal EXIT("unknown symbol type")!
	auto r_ifunc = make_rela(0x48, 2, R_X86_64_64, 0x8);
	EvaluateRelocationForTest(&r_ifunc, &prog, &val, &resolved);
	Check(resolved && val == prog.base_vaddr + 0x408,
	      "R_X86_64_64 with STT_GNU_IFUNC resolves cleanly");

	// 6. Unknown symbol type (15) — previously fatal EXIT!
	auto r_unk_sym = make_rela(0x50, 3, R_X86_64_64, 0);
	EvaluateRelocationForTest(&r_unk_sym, &prog, &val, &resolved);
	Check(resolved && val == prog.base_vaddr + 0x800,
	      "R_X86_64_64 with unknown symbol type 15 resolves cleanly without abort");

	// 7. Unknown bind (14) + STT_COMMON — previously fatal EXIT!
	auto r_unk_bind = make_rela(0x58, 4, R_X86_64_GLOB_DAT, 0);
	EvaluateRelocationForTest(&r_unk_bind, &prog, &val, &resolved);
	Check(!resolved && val == 0,
	      "R_X86_64_GLOB_DAT with unknown bind 14 & STT_COMMON does not abort");

	// 8. Unsupported relocation type (99) — previously fatal EXIT("unknown type: 99")!
	auto r_unk_type = make_rela(0x60, 0, 99u, 0);
	EvaluateRelocationForTest(&r_unk_type, &prog, &val, &resolved);
	Check(!resolved && val == 0, "Unsupported relocation type 99 returns unresolved cleanly");

	std::printf("[PASS] Suite 1: Dynamic ELF/PRX Loader, TLS & Relocation Hardening\n");
}

void TestAudioOutHardening() {
	using namespace Libs::Audio;
	using namespace Libs::Audio::AudioOut;

	Check(AudioOutInit() == OK, "AudioOutInit returns OK");

	// 1. Previously fatal invalid index=1 & unknown format=15
	Check(AudioOutOpen(0, 0 /* MAIN */, 1, 256, 48000, 1) == AUDIO_OUT_ERROR_INVALID_PARAM,
	      "AudioOutOpen(index=1) returns AUDIO_OUT_ERROR_INVALID_PARAM");
	Check(AudioOutOpen(0, 0 /* MAIN */, 0, 256, 48000, 15) == AUDIO_OUT_ERROR_INVALID_FORMAT,
	      "AudioOutOpen(unknown format=15) returns AUDIO_OUT_ERROR_INVALID_FORMAT");

	// 2. Multi-port open (MAIN + BGM + AUDIO3D)
	const int h_main = AudioOutOpen(0, 0 /* MAIN */, 0, 256, 48000, 4 /* FloatStereo */);
	const int h_bgm  = AudioOutOpen(0, 1 /* BGM */, 0, 256, 48000, 1 /* S16Stereo */);
	const int h_3d   = AudioOutOpen(0, 126 /* AUDIO3D */, 0, 256, 48000, 4 /* FloatStereo */);
	Check(h_main > 0 && h_bgm > 0 && h_3d > 0, "Multi-port AudioOutOpen (MAIN/BGM/AUDIO3D) succeeds");

	// 3. Previously fatal nullptr checks on GetPortState / SetVolume / Outputs
	Check(AudioOutGetPortState(h_main, nullptr) == AUDIO_OUT_ERROR_INVALID_POINTER,
	      "AudioOutGetPortState(nullptr) returns AUDIO_OUT_ERROR_INVALID_POINTER");
	Check(AudioOutSetVolume(h_main, 0x3, nullptr) == AUDIO_OUT_ERROR_INVALID_POINTER,
	      "AudioOutSetVolume(nullptr) returns AUDIO_OUT_ERROR_INVALID_POINTER");
	Check(AudioOutOutputs(nullptr, 1) == AUDIO_OUT_ERROR_INVALID_POINTER,
	      "AudioOutOutputs(nullptr) returns AUDIO_OUT_ERROR_INVALID_POINTER");

	// 4. Valid GetPortState + SetVolume + Close
	alignas(8) uint8_t state_buf[32] {};
	Check(AudioOutGetPortState(h_main, reinterpret_cast<AudioOutPortState*>(state_buf)) == OK,
	      "AudioOutGetPortState(valid) returns OK");
	int vols[8] = {32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768};
	Check(AudioOutSetVolume(h_main, 0x3, vols) == OK, "AudioOutSetVolume(valid) returns OK");

	Check(AudioOutClose(h_main) == OK, "AudioOutClose(h_main) returns OK");
	Check(AudioOutClose(h_bgm) == OK, "AudioOutClose(h_bgm) returns OK");
	Check(AudioOutClose(h_3d) == OK, "AudioOutClose(h_3d) returns OK");
	Check(AudioOutClose(h_main) == AUDIO_OUT_ERROR_INVALID_PORT,
	      "AudioOutClose(already closed) returns AUDIO_OUT_ERROR_INVALID_PORT");

	std::printf("[PASS] Suite 2: libSceAudioOut Multi-Port & Invalid-Param Hardening\n");
}

void TestAudioOut2Hardening() {
	using namespace Libs::Audio;
	using namespace Libs::Audio::AudioOut2;

	constexpr int AUDIO_OUT2_ERROR_INVALID_PARAM = static_cast<int>(0x80268001u);

	Check(AudioOut2Initialize() == OK, "AudioOut2Initialize returns OK");

	// 1. Previously fatal nullptr traps across AudioOut2 APIs
	Check(AudioOut2ContextResetParam(nullptr) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2ContextResetParam(nullptr) returns INVALID_PARAM");
	size_t mem_size = 0;
	Check(AudioOut2ContextQueryMemory(nullptr, &mem_size) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2ContextQueryMemory(nullptr, ...) returns INVALID_PARAM");
	AudioOut2ContextHandle ctx = 0;
	Check(AudioOut2ContextCreate(nullptr, nullptr, 0, &ctx) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2ContextCreate(nullptr, ...) returns INVALID_PARAM");
	Check(AudioOut2ContextSetAttributes(0, nullptr, 1) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2ContextSetAttributes(nullptr, num=1) returns INVALID_PARAM");
	Check(AudioOut2PortCreate(0, nullptr, nullptr) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2PortCreate(nullptr, nullptr) returns INVALID_PARAM");
	Check(AudioOut2PortGetState(0, nullptr) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2PortGetState(nullptr) returns INVALID_PARAM");
	Check(AudioOut2GetSystemState(nullptr) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2GetSystemState(nullptr) returns INVALID_PARAM");
	Check(AudioOut2SpeakerArrayCreate(nullptr, nullptr, nullptr) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2SpeakerArrayCreate(nullptr) returns INVALID_PARAM");
	Check(AudioOut2GetSpeakerArrayAmbisonicsCoefficients(nullptr, 0, nullptr, 4) ==
	          AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2GetSpeakerArrayAmbisonicsCoefficients(nullptr, 4) returns INVALID_PARAM");
	Check(AudioOut2GetSpeakerInfo(nullptr, 0) == AUDIO_OUT2_ERROR_INVALID_PARAM,
	      "AudioOut2GetSpeakerInfo(nullptr) returns INVALID_PARAM");

	// 2. Full real AudioOut2 Context + Channel Port + 3D Object Port + SpeakerArray lifecycle
	struct LocalContextParam {
		uint32_t max_ports;
		uint32_t max_object_ports;
		uint32_t guarantee_object_ports;
		uint32_t queue_depth;
		uint32_t num_grains;
		uint32_t flags;
		uint32_t reserved[10];
	} ctx_param {};

	auto* ctx_param_ptr = reinterpret_cast<AudioOut2ContextParam*>(&ctx_param);
	Check(AudioOut2ContextResetParam(ctx_param_ptr) == OK, "AudioOut2ContextResetParam returns OK");
	ctx_param.max_ports        = 8;
	ctx_param.max_object_ports = 4;
	ctx_param.queue_depth      = 2;
	ctx_param.num_grains       = 256;

	Check(AudioOut2ContextQueryMemory(ctx_param_ptr, &mem_size) == OK && mem_size > 0,
	      "AudioOut2ContextQueryMemory returns valid memory size");
	std::vector<uint8_t> ctx_mem(mem_size, 0);
	Check(AudioOut2ContextCreate(ctx_param_ptr, ctx_mem.data(), ctx_mem.size(), &ctx) == OK &&
	          ctx != 0,
	      "AudioOut2ContextCreate succeeds with channel + object ports");

	AudioOut2UserHandle user = 0;
	Check(AudioOut2UserCreate(0x10000001u, &user) == OK && user != 0, "AudioOut2UserCreate succeeds");

	struct LocalPortParam {
		uint16_t            port_type;
		uint16_t            pad;
		uint32_t            data_format;
		uint32_t            sampling_freq;
		uint32_t            flags;
		AudioOut2UserHandle user_handle;
		uint32_t            reserved[10];
	};

	LocalPortParam ch_param {};
	ch_param.port_type     = 0;       // MAIN
	ch_param.data_format   = 0x0200u; // FloatStereo (2ch, type 0)
	ch_param.sampling_freq = 48000;
	ch_param.user_handle   = user;
	AudioOut2PortHandle ch_port = 0;
	Check(AudioOut2PortCreate(ctx, reinterpret_cast<const AudioOut2PortParam*>(&ch_param),
	                          &ch_port) == OK &&
	          ch_port != 0,
	      "AudioOut2PortCreate(MAIN stereo) succeeds");

	LocalPortParam obj_param {};
	obj_param.port_type     = 0x100;   // 3D Object port
	obj_param.data_format   = 0x0100u; // FloatMono (1ch, type 0)
	obj_param.sampling_freq = 48000;
	obj_param.user_handle   = user;
	AudioOut2PortHandle obj_port = 0;
	Check(AudioOut2PortCreate(ctx, reinterpret_cast<const AudioOut2PortParam*>(&obj_param),
	                          &obj_port) == OK &&
	          obj_port != 0,
	      "AudioOut2PortCreate(3D Object 0x100 FloatMono) succeeds");

	alignas(8) uint8_t port_state[64] {};
	Check(AudioOut2PortGetState(ch_port, reinterpret_cast<AudioOut2PortState*>(port_state)) == OK,
	      "AudioOut2PortGetState(ch_port) returns OK");
	alignas(8) uint8_t sys_state[64] {};
	Check(AudioOut2GetSystemState(reinterpret_cast<AudioOut2SystemState*>(sys_state)) == OK,
	      "AudioOut2GetSystemState returns OK");

	AudioOut2SpeakerArrayHandle sa = nullptr;
	Check(AudioOut2SpeakerArrayCreate(&sa, nullptr, nullptr) == OK && sa != nullptr,
	      "AudioOut2SpeakerArrayCreate succeeds");
	float coeffs[4] {};
	Check(AudioOut2GetSpeakerArrayAmbisonicsCoefficients(sa, 0, coeffs, 4) == OK && coeffs[0] > 0.5f,
	      "AudioOut2GetSpeakerArrayAmbisonicsCoefficients fills valid coefficients");
	alignas(8) uint8_t spk_info[128] {};
	Check(AudioOut2GetSpeakerInfo(reinterpret_cast<AudioOut2SpeakerInfo*>(spk_info), 0) == OK,
	      "AudioOut2GetSpeakerInfo returns OK");
	Check(AudioOut2SpeakerArrayDestroy(sa) == OK, "AudioOut2SpeakerArrayDestroy returns OK");

	Check(AudioOut2PortDestroy(obj_port) == OK, "AudioOut2PortDestroy(obj_port) returns OK");
	Check(AudioOut2PortDestroy(ch_port) == OK, "AudioOut2PortDestroy(ch_port) returns OK");
	Check(AudioOut2UserDestroy(user) == OK, "AudioOut2UserDestroy returns OK");
	Check(AudioOut2ContextDestroy(ctx) == OK, "AudioOut2ContextDestroy returns OK");

	std::printf("[PASS] Suite 3: libSceAudioOut2 Multi-Port, 3D Object & Spatializer Hardening\n");
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
	Libs::Audio::Initialize();

	TestLoaderRelocationAndTlsHardening();
	TestAudioOutHardening();
	TestAudioOut2Hardening();

	Libs::Audio::Shutdown();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough11Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough11Tests PASSED (3/3 suites)\n");
	return 0;
}
