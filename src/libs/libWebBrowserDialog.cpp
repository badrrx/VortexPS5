#include "common/abi.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstring>
#include <mutex>

namespace Libs {

LIB_VERSION("WebBrowserDialog", 1, "WebBrowserDialog", 1, 1);

namespace WebBrowserDialog {

constexpr int WEB_BROWSER_DIALOG_STATUS_NONE        = 0;
constexpr int WEB_BROWSER_DIALOG_STATUS_INITIALIZED = 1;
constexpr int WEB_BROWSER_DIALOG_STATUS_RUNNING     = 2;
constexpr int WEB_BROWSER_DIALOG_STATUS_FINISHED    = 3;

constexpr int WEB_BROWSER_DIALOG_RESULT_OK = 0;

constexpr int WEB_BROWSER_DIALOG_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80b80002u);
constexpr int WEB_BROWSER_DIALOG_ERROR_NOT_INITIALIZED     = static_cast<int>(0x80b80001u);
constexpr int WEB_BROWSER_DIALOG_ERROR_INVALID_PARAM       = static_cast<int>(0x80b80004u);

struct WebBrowserDialogResult {
	int32_t result;
	int32_t reserved[3];
};

static std::mutex g_web_dialog_mutex;
static int        g_web_dialog_status = WEB_BROWSER_DIALOG_STATUS_NONE;

int KYTY_SYSV_ABI WebBrowserDialogInitialize() {
	PRINT_NAME();

	std::scoped_lock lock(g_web_dialog_mutex);
	if (g_web_dialog_status != WEB_BROWSER_DIALOG_STATUS_NONE) {
		return WEB_BROWSER_DIALOG_ERROR_ALREADY_INITIALIZED;
	}
	g_web_dialog_status = WEB_BROWSER_DIALOG_STATUS_INITIALIZED;
	return OK;
}

int KYTY_SYSV_ABI WebBrowserDialogOpen(const void* param) {
	PRINT_NAME();

	std::scoped_lock lock(g_web_dialog_mutex);
	if (g_web_dialog_status == WEB_BROWSER_DIALOG_STATUS_NONE) {
		return WEB_BROWSER_DIALOG_ERROR_NOT_INITIALIZED;
	}
	(void)param;
	g_web_dialog_status = WEB_BROWSER_DIALOG_STATUS_RUNNING;
	return OK;
}

int KYTY_SYSV_ABI WebBrowserDialogGetStatus() {
	PRINT_NAME();

	std::scoped_lock lock(g_web_dialog_mutex);
	return g_web_dialog_status;
}

int KYTY_SYSV_ABI WebBrowserDialogUpdateStatus() {
	PRINT_NAME();

	std::scoped_lock lock(g_web_dialog_mutex);
	if (g_web_dialog_status == WEB_BROWSER_DIALOG_STATUS_RUNNING) {
		g_web_dialog_status = WEB_BROWSER_DIALOG_STATUS_FINISHED;
	}
	return g_web_dialog_status;
}

int KYTY_SYSV_ABI WebBrowserDialogGetResult(WebBrowserDialogResult* result) {
	PRINT_NAME();

	if (result == nullptr) {
		return WEB_BROWSER_DIALOG_ERROR_INVALID_PARAM;
	}
	std::memset(result, 0, sizeof(*result));
	result->result = WEB_BROWSER_DIALOG_RESULT_OK;
	return OK;
}

int KYTY_SYSV_ABI WebBrowserDialogClose() {
	PRINT_NAME();

	std::scoped_lock lock(g_web_dialog_mutex);
	if (g_web_dialog_status == WEB_BROWSER_DIALOG_STATUS_RUNNING) {
		g_web_dialog_status = WEB_BROWSER_DIALOG_STATUS_FINISHED;
	}
	return OK;
}

int KYTY_SYSV_ABI WebBrowserDialogTerminate() {
	PRINT_NAME();

	std::scoped_lock lock(g_web_dialog_mutex);
	g_web_dialog_status = WEB_BROWSER_DIALOG_STATUS_NONE;
	return OK;
}

int KYTY_SYSV_ABI WebBrowserDialogSetCookie(const void* param) {
	PRINT_NAME();
	if (param == nullptr) {
		return WEB_BROWSER_DIALOG_ERROR_INVALID_PARAM;
	}
	return OK;
}

} // namespace WebBrowserDialog

LIB_DEFINE(InitWebBrowserDialog_1) {
	LIB_FUNC("PSK+Eik919Q", WebBrowserDialog::WebBrowserDialogInitialize);
	LIB_FUNC("Wit4LJE4-hI", WebBrowserDialog::WebBrowserDialogOpen);
	LIB_FUNC("CFTG6a8TjOU", WebBrowserDialog::WebBrowserDialogGetStatus);
	LIB_FUNC("h1dR-t5ISgg", WebBrowserDialog::WebBrowserDialogUpdateStatus);
	LIB_FUNC("v2g2e6t799k", WebBrowserDialog::WebBrowserDialogGetResult);
	LIB_FUNC("y-c05h6B0wQ", WebBrowserDialog::WebBrowserDialogClose);
	LIB_FUNC("1-bL-r1w7oI", WebBrowserDialog::WebBrowserDialogTerminate);
	LIB_FUNC("T-j+t+m+0+w", WebBrowserDialog::WebBrowserDialogSetCookie);
}

} // namespace Libs
