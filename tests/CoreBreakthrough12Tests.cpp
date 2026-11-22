// Core Breakthrough #12 Verification Suite:
//   1. PS5 System Dialogs (CommonDialog / MsgDialog / SaveDataDialog) Lifecycle & Resilience
//   2. libScePlayGo Chunk Locus, Progress, Speed & ETA Queries
//   3. libSceNpTrophy2 & libSceAppContent Lifecycle & Invalid-Param Hardening

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/dialog.h"
#include "libs/errno.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace Libs::PlayGo {
struct PlayGoInitParams {
	const void* buf_addr;
	uint32_t    buf_size;
	uint32_t    reserved;
};
struct PlayGoProgress {
	uint64_t progress_size;
	uint64_t total_size;
};
int KYTY_SYSV_ABI PlayGoInitialize(const PlayGoInitParams* init);
int KYTY_SYSV_ABI PlayGoTerminate();
int KYTY_SYSV_ABI PlayGoOpen(int* out_handle, const void* param);
int KYTY_SYSV_ABI PlayGoClose(int handle);
int KYTY_SYSV_ABI PlayGoGetLocus(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries,
                                 int8_t* out_loci);
int KYTY_SYSV_ABI PlayGoGetInstallSpeed(int handle, int32_t* out_speed);
int KYTY_SYSV_ABI PlayGoGetEta(int handle, const uint16_t* chunk_ids, uint32_t number_of_entries,
                               int64_t* out_eta);
int KYTY_SYSV_ABI PlayGoGetProgress(int handle, const uint16_t* chunk_ids,
                                    uint32_t number_of_entries, PlayGoProgress* out_progress);
} // namespace Libs::PlayGo

namespace Libs::LibNpTrophy2 {
int KYTY_SYSV_ABI NpTrophy2CreateHandle(int* handle);
int KYTY_SYSV_ABI NpTrophy2CreateContext(int* context, int user_id, uint32_t service_label,
                                         uint64_t options);
int KYTY_SYSV_ABI NpTrophy2RegisterContext(int context, int handle, uint64_t options);
int KYTY_SYSV_ABI NpTrophy2DestroyHandle(int handle);
int KYTY_SYSV_ABI NpTrophy2DestroyContext(int context);
} // namespace Libs::LibNpTrophy2

namespace Libs::AppContent {
struct AppContentInitParam;
struct AppContentBootParam;
struct AppContentMountPoint {
	char data[16];
};
int KYTY_SYSV_ABI AppContentInitialize(const AppContentInitParam* init_param,
                                       AppContentBootParam*       boot_param);
int KYTY_SYSV_ABI AppContentAppParamGetInt(uint32_t param_id, int32_t* value);
int KYTY_SYSV_ABI AppContentAddcontUnmount(const AppContentMountPoint* mount_point);
int KYTY_SYSV_ABI AppContentDownloadDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point,
                                                            size_t* available_space_kb);
} // namespace Libs::AppContent

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
	if (!cond) {
		std::fprintf(stderr, "[FAIL] %s\n", msg);
		++g_failures;
	}
}

void TestSystemDialogs() {
	using namespace Libs::Dialog::CommonDialog;
	using namespace Libs::Dialog::MsgDialog;
	using namespace Libs::Dialog::SaveDataDialog;

	// 1. CommonDialog
	Check(CommonDialogInitialize() == OK, "CommonDialogInitialize returns OK");
	Check(!CommonDialogIsUsed(), "CommonDialogIsUsed starts false");

	// 2. MsgDialog full lifecycle + nullptr resilience
	Check(MsgDialogInitialize() == OK, "MsgDialogInitialize returns OK");
	Check(MsgDialogOpen(nullptr) == OK, "MsgDialogOpen(nullptr) succeeds cleanly");
	Check(MsgDialogUpdateStatus() == 3 /* FINISHED */, "MsgDialogUpdateStatus returns FINISHED");
	Check(MsgDialogGetStatus() == 3 /* FINISHED */, "MsgDialogGetStatus returns FINISHED");
	Check(MsgDialogGetResult(nullptr) == OK, "MsgDialogGetResult(nullptr) returns OK cleanly");
	alignas(8) uint8_t msg_res[32] {};
	Check(MsgDialogGetResult(msg_res) == OK, "MsgDialogGetResult(valid) returns OK");
	Check(MsgDialogClose() == OK, "MsgDialogClose returns OK");
	Check(MsgDialogTerminate() == OK, "MsgDialogTerminate returns OK");

	// 3. SaveDataDialog full lifecycle + nullptr resilience (RUNNING -> FINISHED state machine)
	Check(SaveDataDialogInitialize() == OK, "SaveDataDialogInitialize returns OK");
	Check(SaveDataDialogOpen(nullptr) == OK, "SaveDataDialogOpen(nullptr) succeeds cleanly");
	Check(SaveDataDialogUpdateStatus() == 2 /* RUNNING */,
	      "SaveDataDialogUpdateStatus (1st poll) returns RUNNING");
	Check(SaveDataDialogUpdateStatus() == 3 /* FINISHED */,
	      "SaveDataDialogUpdateStatus (2nd poll) transitions to FINISHED");
	Check(SaveDataDialogGetResult(nullptr) == OK,
	      "SaveDataDialogGetResult(nullptr) returns OK cleanly");
	alignas(8) uint8_t save_res[64] {};
	Check(SaveDataDialogGetResult(save_res) == OK, "SaveDataDialogGetResult(valid) returns OK");
	Check(SaveDataDialogClose(nullptr) == OK, "SaveDataDialogClose(nullptr) returns OK");
	Check(SaveDataDialogTerminate() == OK, "SaveDataDialogTerminate returns OK");

	std::printf("[PASS] Suite 1: PS5 System Dialogs (Common/Msg/SaveData) Lifecycle & Resilience\n");
}

void TestPlayGoStreaming() {
	using namespace Libs::PlayGo;

	// Invalid init checks
	Check(PlayGoInitialize(nullptr) == PLAYGO_ERROR_BAD_POINTER,
	      "PlayGoInitialize(nullptr) returns BAD_POINTER");

	std::vector<uint8_t> work_buf(2u * 1024u * 1024u, 0);
	PlayGoInitParams     init {work_buf.data(), static_cast<uint32_t>(work_buf.size()), 0};
	Check(PlayGoInitialize(&init) == OK, "PlayGoInitialize(2 MiB) returns OK");

	int handle = 0;
	Check(PlayGoOpen(&handle, nullptr) == OK && handle > 0, "PlayGoOpen returns valid handle");

	const uint16_t chunk0 = 0;
	int8_t         locus  = -1;
	Check(PlayGoGetLocus(handle, &chunk0, 1, &locus) == OK && locus == 3 /* LOCAL_FAST */,
	      "PlayGoGetLocus(chunk 0) reports LOCAL_FAST (3)");

	int32_t speed = -1;
	Check(PlayGoGetInstallSpeed(handle, &speed) == OK && speed == 2 /* FULL */,
	      "PlayGoGetInstallSpeed reports FULL (2)");

	int64_t eta = -1;
	Check(PlayGoGetEta(handle, &chunk0, 1, &eta) == OK && eta == 0,
	      "PlayGoGetEta(chunk 0) reports 0 ETA");

	PlayGoProgress prog {};
	Check(PlayGoGetProgress(handle, &chunk0, 1, &prog) == OK && prog.progress_size == 1 &&
	          prog.total_size == 1,
	      "PlayGoGetProgress(chunk 0) reports 100% (1/1)");

	Check(PlayGoClose(handle) == OK, "PlayGoClose returns OK");
	Check(PlayGoClose(999) == PLAYGO_ERROR_BAD_HANDLE, "PlayGoClose(invalid) returns BAD_HANDLE");
	Check(PlayGoTerminate() == OK, "PlayGoTerminate returns OK");

	std::printf("[PASS] Suite 2: libScePlayGo Chunk Locus, Progress, Speed & ETA\n");
}

void TestTrophy2AndAppContent() {
	using namespace Libs::LibNpTrophy2;
	using namespace Libs::AppContent;

	constexpr int NP_TROPHY2_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80553904u);
	constexpr int NP_TROPHY2_ERROR_TITLE_CONF       = static_cast<int>(0x8055391eu);
	constexpr int APP_CONTENT_ERROR_PARAMETER       = static_cast<int>(0x80D90002u);

	// 1. NpTrophy2 lifecycle + error resilience
	Check(NpTrophy2CreateHandle(nullptr) == NP_TROPHY2_ERROR_INVALID_ARGUMENT,
	      "NpTrophy2CreateHandle(nullptr) returns INVALID_ARGUMENT");
	int trophy_handle = 0;
	Check(NpTrophy2CreateHandle(&trophy_handle) == OK && trophy_handle > 0,
	      "NpTrophy2CreateHandle succeeds");

	int trophy_ctx = 0;
	Check(NpTrophy2CreateContext(nullptr, 1, 0, 0) == NP_TROPHY2_ERROR_INVALID_ARGUMENT,
	      "NpTrophy2CreateContext(nullptr) returns INVALID_ARGUMENT");
	Check(NpTrophy2CreateContext(&trophy_ctx, 1, 0, 0) == OK && trophy_ctx > 0,
	      "NpTrophy2CreateContext succeeds");

	const int reg_res = NpTrophy2RegisterContext(trophy_ctx, trophy_handle, 0);
	Check(reg_res == OK || reg_res == NP_TROPHY2_ERROR_TITLE_CONF,
	      "NpTrophy2RegisterContext returns OK or TITLE_CONF_NOT_INSTALLED cleanly");
	Check(NpTrophy2DestroyContext(trophy_ctx) == OK, "NpTrophy2DestroyContext returns OK");
	Check(NpTrophy2DestroyHandle(trophy_handle) == OK, "NpTrophy2DestroyHandle returns OK");

	// 2. AppContent lifecycle + previously fatal nullptr / unknown param_id traps
	Check(AppContentInitialize(nullptr, nullptr) == OK, "AppContentInitialize returns OK");

	int32_t sku_flag = -1;
	Check(AppContentAppParamGetInt(0, &sku_flag) == OK && sku_flag == 3 /* FULL */,
	      "AppContentAppParamGetInt(0) returns SKU_FLAG_FULL (3)");
	Check(AppContentAppParamGetInt(0, nullptr) == APP_CONTENT_ERROR_PARAMETER,
	      "AppContentAppParamGetInt(nullptr) returns APP_CONTENT_ERROR_PARAMETER (was fatal!)");
	Check(AppContentAppParamGetInt(99, &sku_flag) == APP_CONTENT_ERROR_PARAMETER,
	      "AppContentAppParamGetInt(unknown param_id=99) returns APP_CONTENT_ERROR_PARAMETER (was fatal!)");

	size_t avail_kb = 0;
	Check(AppContentDownloadDataGetAvailableSpaceKb(nullptr, &avail_kb) == OK && avail_kb > 0,
	      "AppContentDownloadDataGetAvailableSpaceKb reports positive available KB");
	Check(AppContentAddcontUnmount(nullptr) == APP_CONTENT_ERROR_PARAMETER,
	      "AppContentAddcontUnmount(nullptr) returns APP_CONTENT_ERROR_PARAMETER");

	std::printf("[PASS] Suite 3: libSceNpTrophy2 & libSceAppContent Lifecycle & Hardening\n");
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
	Libs::LibKernel::FileSystem::Initialize();

	TestSystemDialogs();
	TestPlayGoStreaming();
	TestTrophy2AndAppContent();

	Libs::LibKernel::FileSystem::Shutdown();

	if (g_failures != 0) {
		std::fprintf(stderr, "CoreBreakthrough12Tests FAILED (%d failure(s))\n", g_failures);
		return 1;
	}

	std::printf("ALL CoreBreakthrough12Tests PASSED (3/3 suites)\n");
	return 0;
}
