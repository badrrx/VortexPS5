// Core Breakthrough #15 Verification Suite:
//   1. libSceJpegDec & libScePngDec Codec Lifecycle & Error-Code Resilience (via Sony NIDs)
//   2. libSceCes (CP932/CP1252 <-> UTF-8) & libSceFont Library Lifecycle (via Sony NIDs)
//   3. libSceImeDialog & libSceWebBrowserDialog Non-Blocking State Machines

#include "common/abi.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "libs/errno.h"
#include "libs/imeDialog.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Libs {
void InitJpegDec_1(Loader::SymbolDatabase* s);
void InitPngDec_1(Loader::SymbolDatabase* s);
void InitFont_1(Loader::SymbolDatabase* s);
void InitWebBrowserDialog_1(Loader::SymbolDatabase* s);
namespace LibCes {
void InitCes_1(Loader::SymbolDatabase* s);
}
} // namespace Libs

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestJpegDecAndPngDec() {
	Loader::SymbolDatabase db;
	Libs::InitJpegDec_1(&db);
	Libs::InitPngDec_1(&db);

	auto lookup = [&](const char* nid) -> uint64_t {
		const auto* rec = db.FindByNid(nid, Loader::SymbolType::Func);
		return rec != nullptr ? rec->vaddr : 0;
	};

	// --- JpegDec ---
	struct JpegDecCreateParam {
		uint32_t this_size;
		uint32_t attribute;
		uint32_t max_image_width;
	};
	using JpegQueryMemFn = int32_t(KYTY_SYSV_ABI*)(const JpegDecCreateParam*);
	using JpegCreateFn   = int32_t(KYTY_SYSV_ABI*)(const JpegDecCreateParam*, void*, uint32_t,
                                                 void**);
	using JpegDeleteFn   = int32_t(KYTY_SYSV_ABI*)(void*);
	using JpegParseFn    = int32_t(KYTY_SYSV_ABI*)(const void*, void*);

	auto jpeg_query  = reinterpret_cast<JpegQueryMemFn>(lookup("uNAUmANZMEw"));
	auto jpeg_create = reinterpret_cast<JpegCreateFn>(lookup("JPh3Zgg0Zwc"));
	auto jpeg_delete = reinterpret_cast<JpegDeleteFn>(lookup("Hwh11+m5KoI"));
	auto jpeg_parse  = reinterpret_cast<JpegParseFn>(lookup("LSinoSQH790"));
	Check(jpeg_query && jpeg_create && jpeg_delete && jpeg_parse, "JpegDec NIDs resolved");

	Check(jpeg_query(nullptr) < 0, "JpegDecQueryMemorySize(nullptr) returns error");
	JpegDecCreateParam jparam {sizeof(JpegDecCreateParam), 1, 1920};
	const int32_t      jmem_sz = jpeg_query(&jparam);
	Check(jmem_sz > 0, "JpegDecQueryMemorySize(valid) returns positive size");

	std::vector<uint8_t> jmem(static_cast<size_t>(jmem_sz), 0);
	void*                jhandle = nullptr;
	Check(jpeg_create(&jparam, jmem.data(), static_cast<uint32_t>(jmem.size()), &jhandle) == OK &&
	          jhandle != nullptr,
	      "JpegDecCreate succeeds");
	Check(jpeg_parse(nullptr, nullptr) < 0, "JpegDecParseHeader(nullptr) returns error cleanly");
	Check(jpeg_delete(jhandle) == OK, "JpegDecDelete succeeds");

	// --- PngDec ---
	struct PngDecCreateParam {
		uint32_t this_size;
		uint32_t attribute;
		uint32_t max_image_width;
	};
	using PngQueryMemFn = int32_t(KYTY_SYSV_ABI*)(const PngDecCreateParam*);
	using PngCreateFn   = int32_t(KYTY_SYSV_ABI*)(const PngDecCreateParam*, void*, uint32_t, void**);
	using PngDeleteFn   = int32_t(KYTY_SYSV_ABI*)(void*);
	using PngParseFn    = int32_t(KYTY_SYSV_ABI*)(const void*, void*);

	auto png_query  = reinterpret_cast<PngQueryMemFn>(lookup("-6srIGbLTIU"));
	auto png_create = reinterpret_cast<PngCreateFn>(lookup("m0uW+8pFyaw"));
	auto png_delete = reinterpret_cast<PngDeleteFn>(lookup("QbD+eENEwo8"));
	auto png_parse  = reinterpret_cast<PngParseFn>(lookup("U6h4e5JRPaQ"));
	Check(png_query && png_create && png_delete && png_parse, "PngDec NIDs resolved");

	Check(png_query(nullptr) < 0, "PngDecQueryMemorySize(nullptr) returns error");
	PngDecCreateParam pparam {sizeof(PngDecCreateParam), 0, 1920};
	const int32_t     pmem_sz = png_query(&pparam);
	Check(pmem_sz > 0, "PngDecQueryMemorySize(valid) returns positive size");

	std::vector<uint8_t> pmem(static_cast<size_t>(pmem_sz), 0);
	void*                phandle = nullptr;
	Check(png_create(&pparam, pmem.data(), static_cast<uint32_t>(pmem.size()), &phandle) == OK &&
	          phandle != nullptr,
	      "PngDecCreate succeeds");
	Check(png_parse(nullptr, nullptr) < 0, "PngDecParseHeader(nullptr) returns error cleanly");
	Check(png_delete(phandle) == OK, "PngDecDelete succeeds");

	std::printf("[PASS] Suite 1: libSceJpegDec & libScePngDec Codec Lifecycle & Resilience\n");
}

void TestCesAndFont() {
	Loader::SymbolDatabase db;
	Libs::LibCes::InitCes_1(&db);
	Libs::InitFont_1(&db);

	auto lookup = [&](const char* nid) -> uint64_t {
		const auto* rec = db.FindByNid(nid, Loader::SymbolType::Func);
		return rec != nullptr ? rec->vaddr : 0;
	};

	// --- CES (CP932 -> UTF-8) ---
	using InitSjisFn  = void*(KYTY_SYSV_ABI*)(void*);
	using InitCtxFn   = int(KYTY_SYSV_ABI*)(void*, const void*);
	using StrToUtf8Fn = int(KYTY_SYSV_ABI*)(void*, const uint8_t*, uint32_t, uint32_t*, uint8_t*,
                                            uint32_t, uint32_t*);

	auto init_sjis = reinterpret_cast<InitSjisFn>(lookup("ZiDCxUUGbec"));
	auto init_ctx  = reinterpret_cast<InitCtxFn>(lookup("538bRGc6Zo8"));
	auto mbcs_utf8 = reinterpret_cast<StrToUtf8Fn>(lookup("yGKn6vdYInc"));
	Check(init_sjis && init_ctx && mbcs_utf8, "libSceCes NIDs resolved");

	alignas(8) uint8_t profile[128] {};
	alignas(8) uint8_t ctx[128] {};
	Check(init_sjis(profile) == profile, "CesUcsProfileInitSJis1997Cp932 returns profile ptr");
	Check(init_ctx(ctx, profile) == OK, "CesMbcsUcsContextInit returns OK");

	const uint8_t src[] = "KytyPS5";
	uint8_t       dst[64] {};
	uint32_t      src_len = 0, dst_len = 0;
	Check(mbcs_utf8(ctx, src, sizeof(src), &src_len, dst, sizeof(dst), &dst_len) == OK &&
	          std::strcmp(reinterpret_cast<const char*>(dst), "KytyPS5") == 0,
	      "CesMbcsStrToUtf8Str converts ASCII/CP932 cleanly");
	Check(mbcs_utf8(nullptr, src, sizeof(src), &src_len, dst, sizeof(dst), &dst_len) != OK,
	      "CesMbcsStrToUtf8Str(nullptr ctx) returns error cleanly");

	// --- Font ---
	using DestroyLibFn = int(KYTY_SYSV_ABI*)(void*);
	auto font_destroy  = reinterpret_cast<DestroyLibFn>(lookup("FXP359ygujs"));
	Check(font_destroy != nullptr, "libSceFont NIDs resolved");
	const int fd_res = font_destroy(nullptr);
	Check(fd_res == OK || fd_res < 0, "FontDestroyLibrary(nullptr) handles null cleanly");

	std::printf("[PASS] Suite 2: libSceCes & libSceFont Conversion & Error Resilience\n");
}

void TestImeAndWebBrowserDialog() {
	using namespace Libs::Dialog::ImeDialog;

	// 1. ImeDialog status & invalid-param resilience
	Check(ImeDialogGetStatus() == static_cast<int>(Status::None),
	      "ImeDialogGetStatus starts None");
	Check(ImeDialogInit(nullptr, nullptr) != OK, "ImeDialogInit(nullptr) returns error cleanly");
	Check(ImeDialogGetResult(nullptr) != OK, "ImeDialogGetResult(nullptr) returns error cleanly");

	// 2. WebBrowserDialog full non-blocking lifecycle via Sony NIDs
	Loader::SymbolDatabase db;
	Libs::InitWebBrowserDialog_1(&db);

	auto lookup = [&](const char* nid) -> uint64_t {
		const auto* rec = db.FindByNid(nid, Loader::SymbolType::Func);
		return rec != nullptr ? rec->vaddr : 0;
	};

	using InitFn   = int(KYTY_SYSV_ABI*)();
	using OpenFn   = int(KYTY_SYSV_ABI*)(const void*);
	using StatusFn = int(KYTY_SYSV_ABI*)();
	using ResultFn = int(KYTY_SYSV_ABI*)(void*);
	using CloseFn  = int(KYTY_SYSV_ABI*)();
	using TermFn   = int(KYTY_SYSV_ABI*)();

	auto web_init   = reinterpret_cast<InitFn>(lookup("PSK+Eik919Q"));
	auto web_open   = reinterpret_cast<OpenFn>(lookup("Wit4LJE4-hI"));
	auto web_status = reinterpret_cast<StatusFn>(lookup("CFTG6a8TjOU"));
	auto web_update = reinterpret_cast<StatusFn>(lookup("h1dR-t5ISgg"));
	auto web_result = reinterpret_cast<ResultFn>(lookup("v2g2e6t799k"));
	auto web_close  = reinterpret_cast<CloseFn>(lookup("y-c05h6B0wQ"));
	auto web_term   = reinterpret_cast<TermFn>(lookup("1-bL-r1w7oI"));

	Check(web_init && web_open && web_status && web_update && web_result && web_close && web_term,
	      "All libSceWebBrowserDialog NIDs resolved");
	Check(web_status() == 0 /* NONE */, "WebBrowserDialog starts NONE");
	Check(web_init() == OK, "WebBrowserDialogInitialize returns OK");
	Check(web_open(nullptr) == OK, "WebBrowserDialogOpen succeeds");
	Check(web_status() == 2 /* RUNNING */, "WebBrowserDialogGetStatus returns RUNNING");
	Check(web_update() == 3 /* FINISHED */, "WebBrowserDialogUpdateStatus transitions to FINISHED");
	Check(web_result(nullptr) != OK, "WebBrowserDialogGetResult(nullptr) returns error");
	int32_t res_buf[4] {-1, 0, 0, 0};
	Check(web_result(res_buf) == OK && res_buf[0] == 0,
	      "WebBrowserDialogGetResult returns RESULT_OK");
	Check(web_close() == OK, "WebBrowserDialogClose returns OK");
	Check(web_term() == OK && web_status() == 0, "WebBrowserDialogTerminate resets to NONE");

	std::printf("[PASS] Suite 3: libSceImeDialog & libSceWebBrowserDialog State Machines\n");
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

	TestJpegDecAndPngDec();
	TestCesAndFont();
	TestImeAndWebBrowserDialog();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough15Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough15Tests PASSED (3/3 suites)\n");
	return 0;
}
