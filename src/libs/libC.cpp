#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/hostMemory.h"
#include "kernel/fileSystem.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/guestPrintf.h"
#include "libs/libs.h"
#include "libs/vaContext.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fmt/format.h>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <malloc.h>
#endif

namespace Libs {

namespace LibKernel {
void KernelDispatchPendingSignalForCurrentThread();
} // namespace LibKernel

namespace LibC {

LIB_VERSION("libc", 1, "libc", 1, 1);

static uint32_t g_need_flag = 1;

using cxa_destructor_func_t = KYTY_SYSV_ABI void (*)(void*);
using atexit_func_t         = KYTY_SYSV_ABI void (*)();

struct CxaDestructor {
	cxa_destructor_func_t destructor_func;
	void*                 destructor_object;
	void*                 module_id;
};

struct CContext {
	std::list<CxaDestructor> cxa;
	std::list<atexit_func_t> atexit;
};

using InitEnvParams = Loader::EntryParams;

static int                g_argc = 0;
static const char* const* g_argv = nullptr;
static const char* const* g_envp = nullptr;

int GetArgc() {
	return g_argc;
}

const char** GetArgv() {
	return const_cast<const char**>(g_argv);
}

static KYTY_SYSV_ABI void exit(int code) {
	PRINT_NAME();

	::exit(code);
}

static void PrintAbortStringCandidate(const char* name, uint64_t addr) {
	if (!Graphics::HostMemoryIsReadable(addr)) {
		return;
	}

	const auto* ptr = reinterpret_cast<const char*>(static_cast<uintptr_t>(addr));
	char        buf[257] {};
	size_t      len = 0;

	for (; len < sizeof(buf) - 1 && Graphics::HostMemoryIsReadable(addr + len); len++) {
		const auto c = static_cast<unsigned char>(ptr[len]);
		if (c == 0) {
			break;
		}
		if (!std::isprint(c) && c != '\n' && c != '\r' && c != '\t') {
			return;
		}
		buf[len] = static_cast<char>(c);
	}

	if (len >= 4) {
		LOGF("\t %s string = \"%s\"\n", name, buf);
	}
}

static void PrintAbortWideStringCandidate(const char* name, uint64_t addr) {
	if (!Graphics::HostMemoryIsReadable(addr)) {
		return;
	}

	const auto* ptr = reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(addr));
	char        buf[257] {};
	size_t      len = 0;

	for (;
	     len < sizeof(buf) - 1 && Graphics::HostMemoryIsReadable(addr + len * sizeof(uint16_t) + 1);
	     len++) {
		const auto c = ptr[len];
		if (c == 0) {
			break;
		}
		if (c > 0x7f ||
		    (!std::isprint(static_cast<unsigned char>(c)) && c != '\n' && c != '\r' && c != '\t')) {
			return;
		}
		buf[len] = static_cast<char>(c);
	}

	if (len >= 4) {
		LOGF("\t %s u16 = \"%s\"\n", name, buf);
	}
}

static void PrintAbortBytesCandidate(const char* name, uint64_t addr) {
	if (!Graphics::HostMemoryIsReadable(addr)) {
		return;
	}

	uint8_t bytes[64] {};
	size_t  len = 0;

	for (; len < sizeof(bytes) && Graphics::HostMemoryIsReadable(addr + len); len++) {
		bytes[len] = *reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(addr + len));
	}

	if (len == 0) {
		return;
	}

	LOGF("\t %s bytes =", name);
	for (size_t i = 0; i < len; i++) {
		LOGF(" %02" PRIx8, bytes[i]);
	}
	LOGF("\n");
}

static void PrintAbortPointerCandidate(const char* name, uint64_t addr) {
	PrintAbortStringCandidate(name, addr);
	PrintAbortWideStringCandidate(name, addr);
	PrintAbortBytesCandidate(name, addr);
}

static bool LooksLikeAbortPointer(uint64_t addr) {
	return Graphics::HostMemoryIsReadable(addr) && addr >= 0x10000;
}

static void PrintAbortPointerArrayCandidate(const char* name, uint64_t addr) {
	if (!LooksLikeAbortPointer(addr)) {
		return;
	}

	for (int i = 0; i < 8; i++) {
		const auto slot_addr = addr + static_cast<uint64_t>(i) * sizeof(uint64_t);
		if (!Graphics::HostMemoryIsReadable(slot_addr)) {
			break;
		}

		const auto value = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(slot_addr));
		if (!LooksLikeAbortPointer(value)) {
			continue;
		}

		LOGF("\t %s[%d] = 0x%016" PRIx64 "\n", name, i, value);
		const auto child_name = fmt::format("{}[{}]", name, i);
		PrintAbortPointerCandidate(child_name.c_str(), value);
	}
}

[[noreturn]] static KYTY_SYSV_ABI void abort(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                             uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	uint64_t rbp = 0;
	uint64_t rsp = 0;
	// This assembly is x86-64-specific, not Windows-specific.
#if defined(__x86_64__) || defined(_M_X64)
	asm volatile("movq %%rbp, %0" : "=r"(rbp));
	asm volatile("movq %%rsp, %0" : "=r"(rsp));
#endif

	const auto ret = reinterpret_cast<uint64_t>(__builtin_return_address(0));

	LOGF("Guest abort diagnostics:\n"
	     "\t return = 0x%016" PRIx64 "\n"
	     "\t rbp    = 0x%016" PRIx64 "\n"
	     "\t rsp    = 0x%016" PRIx64 "\n"
	     "\t arg0   = 0x%016" PRIx64 "\n"
	     "\t arg1   = 0x%016" PRIx64 "\n"
	     "\t arg2   = 0x%016" PRIx64 "\n"
	     "\t arg3   = 0x%016" PRIx64 "\n"
	     "\t arg4   = 0x%016" PRIx64 "\n"
	     "\t arg5   = 0x%016" PRIx64 "\n",
	     ret, rbp, rsp, arg0, arg1, arg2, arg3, arg4, arg5);

	PrintAbortPointerCandidate("arg0", arg0);
	PrintAbortPointerCandidate("arg1", arg1);
	PrintAbortPointerCandidate("arg2", arg2);
	PrintAbortPointerCandidate("arg3", arg3);
	PrintAbortPointerCandidate("arg4", arg4);
	PrintAbortPointerCandidate("arg5", arg5);
	PrintAbortPointerArrayCandidate("arg0", arg0);
	PrintAbortPointerArrayCandidate("arg1", arg1);
	PrintAbortPointerArrayCandidate("arg2", arg2);
	PrintAbortPointerArrayCandidate("arg3", arg3);
	PrintAbortPointerArrayCandidate("arg4", arg4);
	PrintAbortPointerArrayCandidate("arg5", arg5);

	if (rbp != 0) {
		Common::Singleton<Loader::RuntimeLinker>::Instance()->StackTrace(rbp, rsp);
	}

	if (rsp != 0) {
		LOGF("Guest abort stack words:\n");
		auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
		for (int i = 0; i < 16; i++) {
			const auto addr = rsp + static_cast<uint64_t>(i) * sizeof(uint64_t);
			if (!Graphics::HostMemoryIsReadable(addr)) {
				break;
			}
			const auto value   = *reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(addr));
			auto*      program = linker->FindProgramByAddr(value);
			if (program != nullptr) {
				auto module_name = Common::PathToString(program->file_name.filename());
				LOGF("\t [%02d] 0x%016" PRIx64 " %s+0x%016" PRIx64 "\n", i, value,
				     module_name.c_str(), value - program->base_vaddr);
			} else {
				LOGF("\t [%02d] 0x%016" PRIx64 "\n", i, value);
			}
			PrintAbortPointerCandidate(fmt::format("stack[{:02d}]", i).c_str(), value);
			PrintAbortPointerArrayCandidate(fmt::format("stack[{:02d}]", i).c_str(), value);
		}
	}

	EXIT("Guest abort()\n");
	std::abort();
}

static KYTY_SYSV_ABI int* libc_error() {
	PRINT_NAME();

	return Posix::GetErrorAddr();
}

static KYTY_SYSV_ABI void init_env(const InitEnvParams* params) {
	PRINT_NAME();

	if (params == nullptr || params->argc < 0) {
		g_argc = 0;
		g_argv = nullptr;
		g_envp = nullptr;
		return;
	}

	g_argc = params->argc;
	g_argv = params->argv;
	g_envp = params->argv + params->argc + 1;

	LOGF("\t argc = %d\n"
	     "\t argv = 0x%016" PRIx64 "\n"
	     "\t envp = 0x%016" PRIx64 "\n",
	     g_argc, reinterpret_cast<uint64_t>(g_argv), reinterpret_cast<uint64_t>(g_envp));

	for (int i = 0; i < g_argc; i++) {
		LOGF("\t argv[%d] = %s\n", i, g_argv[i] != nullptr ? g_argv[i] : "<null>");
	}
}

static KYTY_SYSV_ABI int atexit(atexit_func_t func) {
	PRINT_NAME();

	if (func != nullptr) {
		Common::Singleton<CContext>::Instance()->atexit.push_front(func);
	}

	return 0;
}

static KYTY_SYSV_ABI int libc_printf(VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	PRINT_NAME();

	return GetGuestPrintfCtxFunc()(&ctx);
}

static KYTY_SYSV_ABI int puts(const char* s) {
	PRINT_NAME();

	return GetGuestPrintfStdFunc()("%s\n", s);
}

static std::mutex                                   g_env_mutex;
static std::unordered_map<std::string, std::string> g_guest_env;

static KYTY_SYSV_ABI char* getenv(const char* name) {
	PRINT_NAME();

	if (name == nullptr || name[0] == '\0') {
		return nullptr;
	}

	std::scoped_lock lock(g_env_mutex);
	auto             it = g_guest_env.find(name);
	if (it == g_guest_env.end()) {
		return nullptr;
	}
	return it->second.data();
}

static KYTY_SYSV_ABI int unsetenv(const char* name) {
	PRINT_NAME();

	if (name == nullptr || name[0] == '\0' || std::strchr(name, '=') != nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	std::scoped_lock lock(g_env_mutex);
	g_guest_env.erase(name);
	return 0;
}

static KYTY_SYSV_ABI int setenv(const char* name, const char* value, int overwrite) {
	PRINT_NAME();

	LOGF("\t name      = %s\n"
	     "\t value     = %s\n"
	     "\t overwrite = %d\n",
	     (name != nullptr ? name : "<null>"), (value != nullptr ? value : "<null>"), overwrite);

	if (name == nullptr || value == nullptr || name[0] == '\0' ||
	    std::strchr(name, '=') != nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}

	// Store in isolated guest environment map (never leak into host process environment).
	std::scoped_lock lock(g_env_mutex);
	if (overwrite != 0 || !g_guest_env.contains(name)) {
		g_guest_env[name] = value;
	}
	return 0;
}

static KYTY_SYSV_ABI int64_t libc_time(int64_t* timer) {
	const auto now = static_cast<int64_t>(std::time(nullptr));
	if (timer != nullptr) {
		*timer = now;
	}
	return now;
}

static KYTY_SYSV_ABI double libc_difftime(int64_t time1, int64_t time0) {
	return std::difftime(static_cast<std::time_t>(time1), static_cast<std::time_t>(time0));
}

struct GuestTm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

static_assert(sizeof(GuestTm) == 36);

static GuestTm ToGuestTm(const std::tm& time) {
	return {time.tm_sec,  time.tm_min,  time.tm_hour, time.tm_mday, time.tm_mon,
	        time.tm_year, time.tm_wday, time.tm_yday, time.tm_isdst};
}

static std::tm ToHostTm(const GuestTm& time) {
	std::tm result {};
	result.tm_sec   = time.tm_sec;
	result.tm_min   = time.tm_min;
	result.tm_hour  = time.tm_hour;
	result.tm_mday  = time.tm_mday;
	result.tm_mon   = time.tm_mon;
	result.tm_year  = time.tm_year;
	result.tm_wday  = time.tm_wday;
	result.tm_yday  = time.tm_yday;
	result.tm_isdst = time.tm_isdst;
	return result;
}

static KYTY_SYSV_ABI GuestTm* libc_gmtime(const int64_t* timer) {
	if (timer == nullptr) {
		return nullptr;
	}

	thread_local GuestTm result {};
	std::tm              host_result {};
	const auto           t = static_cast<std::time_t>(*timer);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (_gmtime64_s(&host_result, &t) != 0) {
		return nullptr;
	}
#else
	if (gmtime_r(&t, &host_result) == nullptr) {
		return nullptr;
	}
#endif

	result = ToGuestTm(host_result);
	return &result;
}

static KYTY_SYSV_ABI GuestTm* libc_localtime(const int64_t* timer) {
	if (timer == nullptr) {
		return nullptr;
	}

	thread_local GuestTm result {};
	std::tm              host_result {};
	const auto           t = static_cast<std::time_t>(*timer);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (_localtime64_s(&host_result, &t) != 0) {
		return nullptr;
	}
#else
	if (localtime_r(&t, &host_result) == nullptr) {
		return nullptr;
	}
#endif

	result = ToGuestTm(host_result);
	return &result;
}

static KYTY_SYSV_ABI int64_t libc_mktime(GuestTm* timeptr) {
	if (timeptr == nullptr) {
		return -1;
	}

	auto host_time = ToHostTm(*timeptr);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	const auto result = static_cast<int64_t>(_mktime64(&host_time));
#else
	const auto result = static_cast<int64_t>(std::mktime(&host_time));
#endif

	*timeptr = ToGuestTm(host_time);
	return result;
}

static KYTY_SYSV_ABI size_t libc_strftime(char* str, size_t count, const char* format,
                                          const GuestTm* timeptr) {
	if (str == nullptr || format == nullptr || timeptr == nullptr) {
		return 0;
	}

	const auto host_time = ToHostTm(*timeptr);
	return std::strftime(str, count, format, &host_time);
}

static KYTY_SYSV_ABI void catchReturnFromMain(int status) {
	PRINT_NAME();

	uint64_t ret_addr = reinterpret_cast<uint64_t>(__builtin_return_address(0));
	uint64_t rsp      = 0;
	uint64_t rbp      = 0;
#if defined(__x86_64__) || defined(_M_X64)
	asm volatile("movq %%rsp, %0\n\t"
	             "movq %%rbp, %1\n\t"
	             : "=r"(rsp), "=r"(rbp));
#endif

	LOGF("\t status     = %d\n"
	     "\t ret_addr   = 0x%016" PRIx64 "\n"
	     "\t rsp        = 0x%016" PRIx64 "\n"
	     "\t rbp        = 0x%016" PRIx64 "\n",
	     status, ret_addr, rsp, rbp);
	if (rsp != 0) {
		auto* stack = reinterpret_cast<const uint64_t*>(rsp);
		for (uint32_t i = 0; i < 12; i++) {
			LOGF("\t stack[%02" PRIu32 "] = 0x%016" PRIx64 "\n", i, stack[i]);
		}
	}

	::printf("return from main = %d\n", status);
}

using execute_once_func_t = KYTY_SYSV_ABI int (*)(void*, void*, void**);

static std::mutex              g_execute_once_mutex;
static std::condition_variable g_execute_once_cv;

static KYTY_SYSV_ABI int std_execute_once(int* flag, execute_once_func_t func, void* arg) {
	PRINT_NAME();

	if (flag == nullptr || func == nullptr) {
		return 0;
	}

	constexpr int once_init    = 0;
	constexpr int once_done    = 1;
	constexpr int once_running = 2;

	{
		std::unique_lock lock(g_execute_once_mutex);
		while (*flag == once_running) {
			g_execute_once_cv.wait_for(lock, std::chrono::microseconds(10000));
			if (*flag == once_running) {
				lock.unlock();
				LibKernel::KernelDispatchPendingSignalForCurrentThread();
				lock.lock();
			}
		}
		if (*flag == once_done) {
			return 1;
		}
		*flag = once_running;
	}

	void* callback_context = nullptr;
	int   result           = func(nullptr, arg, &callback_context);

	{
		std::lock_guard lock(g_execute_once_mutex);
		*flag = (result != 0 ? once_done : once_init);
	}
	g_execute_once_cv.notify_all();

	return result;
}

static KYTY_SYSV_ABI int cxa_atexit(cxa_destructor_func_t func, void* arg, void* d) {
	PRINT_NAME();

	auto* cc = Common::Singleton<CContext>::Instance();

	CxaDestructor c {};
	c.destructor_func   = func;
	c.destructor_object = arg;
	c.module_id         = d;

	cc->cxa.push_back(c);

	return 0;
}

void KYTY_SYSV_ABI cxa_finalize(void* d) {
	PRINT_NAME();

	auto* cc = Common::Singleton<CContext>::Instance();

	for (auto i = cc->cxa.rbegin(); i != cc->cxa.rend(); ++i) {
		auto& c = *i;
		if ((d == nullptr || c.module_id == d) && c.destructor_func != nullptr) {
			auto* func        = c.destructor_func;
			auto* object      = c.destructor_object;
			c.destructor_func = nullptr;
			func(object);
		}
	}
}

} // namespace LibC

namespace LibcInternalExt {

LIB_VERSION("LibcInternalExt", 1, "LibcInternal", 1, 1);

static uint64_t g_mspace_atomic_id_mask = 0;
static uint64_t g_mstate_table[64]      = {0};

using thread_atexit_destructor_t = KYTY_SYSV_ABI void (*)(void*);

struct ThreadAtexitDestructor {
	thread_atexit_destructor_t destructor;
	void*                      object;
};

static thread_local std::vector<ThreadAtexitDestructor> g_thread_atexit_destructors;

struct Info {
	uint64_t  size;
	uint32_t  unknown1;
	uint32_t  unknown2;
	uint64_t* mspace_atomic_id_mask;
	uint64_t* mstate_table;
};

void KYTY_SYSV_ABI LibcHeapGetTraceInfo(Info* info) {
	PRINT_NAME();

	if (info == nullptr || info->size < 32) {
		LOGF_COLOR(Log::Color::Yellow,
		           "\t LibcHeapGetTraceInfo: ignoring invalid/undersized info (size=%" PRIu64 ")\n",
		           info != nullptr ? info->size : 0ull);
		return;
	}

	info->mspace_atomic_id_mask = &g_mspace_atomic_id_mask;
	info->mstate_table          = g_mstate_table;
}

int KYTY_SYSV_ABI LibcInternalExtCxaThreadAtexit(thread_atexit_destructor_t destructor,
                                                 void* object, void* /*module_id*/) {
	PRINT_NAME();

	g_thread_atexit_destructors.push_back({destructor, object});

	return 0;
}

void RunThreadAtexitDestructors() {
	while (!g_thread_atexit_destructors.empty()) {
		auto destructor = g_thread_atexit_destructors.back();
		g_thread_atexit_destructors.pop_back();

		if (destructor.destructor != nullptr) {
			destructor.destructor(destructor.object);
		}
	}
}

LIB_DEFINE(InitLibcInternalExt_1) {
	LIB_FUNC("NWtTN10cJzE", LibcInternalExt::LibcHeapGetTraceInfo);
	LIB_FUNC("qBS714-Jr3g", LibcInternalExt::LibcInternalExtCxaThreadAtexit);
}

} // namespace LibcInternalExt

namespace LibcInternal {

LIB_VERSION("LibcInternal", 1, "LibcInternal", 1, 1);

static uint32_t g_need_flag = 1;

int KYTY_SYSV_ABI vprintf(const char* str, VaList* c) {
	PRINT_NAME();

	return GetGuestVprintfFunc()(str, c);
}

static KYTY_SYSV_ABI int snprintf(VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	PRINT_NAME();

	return GetGuestSnprintfCtxFunc()(&ctx);
}

int KYTY_SYSV_ABI fflush(FILE* stream) {
	PRINT_NAME();

	if (stream == nullptr || stream == stdout || stream == stderr) {
		return ::fflush(stream);
	}

	return 0;
}

void* KYTY_SYSV_ABI memset(void* s, int c, size_t n) {
	PRINT_NAME();

	return ::memset(s, c, n);
}

void* KYTY_SYSV_ABI memcpy(void* dest, const void* src, size_t n) {
	return ::memcpy(dest, src, n);
}

void* KYTY_SYSV_ABI memmove(void* dest, const void* src, size_t n) {
	return ::memmove(dest, src, n);
}

int KYTY_SYSV_ABI memcmp(const void* s1, const void* s2, size_t n) {
	return ::memcmp(s1, s2, n);
}

int KYTY_SYSV_ABI strcmp(const char* s1, const char* s2) {
	if (s1 == nullptr || s2 == nullptr) {
		return (s1 == s2) ? 0 : (s1 == nullptr ? -1 : 1);
	}
	return ::strcmp(s1, s2);
}

int KYTY_SYSV_ABI strncmp(const char* s1, const char* s2, size_t n) {
	if (n == 0u) {
		return 0;
	}
	if (s1 == nullptr || s2 == nullptr) {
		return (s1 == s2) ? 0 : (s1 == nullptr ? -1 : 1);
	}
	return ::strncmp(s1, s2, n);
}

size_t KYTY_SYSV_ABI strlen(const char* s) {
	return s != nullptr ? ::strlen(s) : 0u;
}

char* KYTY_SYSV_ABI strcpy(char* dest, const char* src) {
	return ::strcpy(dest, src);
}

char* KYTY_SYSV_ABI strncpy(char* dest, const char* src, size_t count) {
	return ::strncpy(dest, src, count);
}

char* KYTY_SYSV_ABI strcat(char* dest, const char* src) {
	return ::strcat(dest, src);
}

const char* KYTY_SYSV_ABI strchr(const char* s, int c) {
	return s != nullptr ? ::strchr(s, c) : nullptr;
}

char* KYTY_SYSV_ABI strrchr(const char* s, int c) {
	return s != nullptr ? const_cast<char*>(::strrchr(s, c)) : nullptr;
}

char* KYTY_SYSV_ABI strstr(const char* haystack, const char* needle) {
	static int log_count = 0;
	if (log_count++ < 8) {
		LOGF("LibcInternal::strstr(\"%s\", \"%s\")\n", (haystack != nullptr ? haystack : "<null>"),
		     (needle != nullptr ? needle : "<null>"));
	}
	if (haystack == nullptr || needle == nullptr) {
		return nullptr;
	}
	return const_cast<char*>(::strstr(haystack, needle));
}

long KYTY_SYSV_ABI strtol(const char* str, char** endptr, int base) {
	static int log_count = 0;
	if (log_count++ < 16) {
		LOGF("LibcInternal::strtol(\"%s\", base=%d)\n", (str != nullptr ? str : "<null>"), base);
	}
	return ::strtol(str, endptr, base);
}

unsigned long KYTY_SYSV_ABI strtoul(const char* str, char** endptr, int base) {
	static int log_count = 0;
	if (log_count++ < 16) {
		LOGF("LibcInternal::strtoul(\"%s\", base=%d)\n", (str != nullptr ? str : "<null>"), base);
	}
	return ::strtoul(str, endptr, base);
}

int KYTY_SYSV_ABI atoi(const char* str) {
	return ::atoi(str);
}

float KYTY_SYSV_ABI sinf(float x) {
	return std::sin(x);
}

float KYTY_SYSV_ABI cosf(float x) {
	return std::cos(x);
}

void KYTY_SYSV_ABI sincosf(float x, float* sinp, float* cosp) {
	if (sinp != nullptr) {
		*sinp = std::sin(x);
	}
	if (cosp != nullptr) {
		*cosp = std::cos(x);
	}
}

double KYTY_SYSV_ABI sin(double x) {
	return std::sin(x);
}

double KYTY_SYSV_ABI cos(double x) {
	return std::cos(x);
}

void KYTY_SYSV_ABI sincos(double x, double* sinp, double* cosp) {
	if (sinp != nullptr) {
		*sinp = std::sin(x);
	}
	if (cosp != nullptr) {
		*cosp = std::cos(x);
	}
}

int KYTY_SYSV_ABI LibcHeapErrorReportForGame(uint64_t msp, uint64_t ptr, uint64_t error,
                                             uint64_t arg3, uint64_t arg4, uint64_t arg5) {
	PRINT_NAME();

	LOGF("\t temporary: heap error report ignored, msp=0x%016" PRIx64 ", ptr=0x%016" PRIx64
	     ", error=0x%016" PRIx64 ", args=(0x%016" PRIx64 ",0x%016" PRIx64 ",0x%016" PRIx64 ")\n",
	     msp, ptr, error, arg3, arg4, arg5);

	return 0;
}

struct GuestDirent {
	uint32_t d_fileno;
	uint16_t d_reclen;
	uint8_t  d_type;
	uint8_t  d_namlen;
	char     d_name[256];
};

struct GuestDirStream {
	std::vector<std::pair<std::string, uint8_t>> entries;
	size_t                                       index = 0;
	GuestDirent                                  current {};
};

static KYTY_SYSV_ABI int gethostname(char* name, size_t namelen) {
	PRINT_NAME();
	if (name == nullptr || namelen == 0) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return -1;
	}
	static constexpr const char* kHostName = "ps5-vortex";
	std::strncpy(name, kHostName, namelen - 1);
	name[namelen - 1] = '\0';
	return 0;
}

static KYTY_SYSV_ABI char* realpath(const char* path, char* resolved_path) {
	PRINT_NAME();
	if (path == nullptr) {
		*Posix::GetErrorAddr() = Posix::POSIX_EINVAL;
		return nullptr;
	}
	char* out = resolved_path;
	if (out == nullptr) {
		out = static_cast<char*>(std::malloc(1024));
		if (out == nullptr) {
			return nullptr;
		}
	}
	std::strncpy(out, path, 1023);
	out[1023] = '\0';
	return out;
}

static KYTY_SYSV_ABI void* opendir(const char* name) {
	PRINT_NAME();
	if (name == nullptr || name[0] == '\0') {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOENT;
		return nullptr;
	}
	const auto real_path = LibKernel::FileSystem::GetRealFilename(name);
	std::error_code ec;
	if (!std::filesystem::exists(real_path, ec) || !std::filesystem::is_directory(real_path, ec)) {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOENT;
		return nullptr;
	}
	auto* dir = new GuestDirStream();
	for (const auto& entry: std::filesystem::directory_iterator(real_path, ec)) {
		if (ec) {
			break;
		}
		const auto fname = entry.path().filename().string();
		const uint8_t dtype = entry.is_directory(ec) ? 4u : 8u; // DT_DIR=4, DT_REG=8
		dir->entries.emplace_back(fname, dtype);
	}
	return dir;
}

static KYTY_SYSV_ABI GuestDirent* readdir(void* dirp) {
	PRINT_NAME();
	if (dirp == nullptr) {
		return nullptr;
	}
	auto* dir = static_cast<GuestDirStream*>(dirp);
	if (dir->index >= dir->entries.size()) {
		return nullptr;
	}
	const auto& [name, dtype] = dir->entries[dir->index++];
	dir->current              = {};
	dir->current.d_fileno     = static_cast<uint32_t>(dir->index);
	dir->current.d_reclen     = sizeof(GuestDirent);
	dir->current.d_type       = dtype;
	const size_t copy_len     = std::min<size_t>(name.size(), sizeof(dir->current.d_name) - 1);
	dir->current.d_namlen     = static_cast<uint8_t>(copy_len);
	std::memcpy(dir->current.d_name, name.data(), copy_len);
	dir->current.d_name[copy_len] = '\0';
	return &dir->current;
}

[[maybe_unused]] static KYTY_SYSV_ABI int closedir(void* dirp) {
	PRINT_NAME();
	if (dirp != nullptr) {
		delete static_cast<GuestDirStream*>(dirp);
	}
	return 0;
}

static KYTY_SYSV_ABI int getifaddrs(void** ifap) {
	PRINT_NAME();
	if (ifap != nullptr) {
		*ifap = nullptr;
	}
	return 0;
}

// HLE Heap Allocator (used when homebrew bundles a stubbed libc.prx or omits libc.prx)
static std::mutex                 g_aligned_alloc_mutex;
static std::unordered_set<void*>  g_aligned_allocs;

static KYTY_SYSV_ABI void* hle_malloc(size_t size) {
	return std::malloc(size == 0 ? 1u : size);
}

static KYTY_SYSV_ABI void* hle_calloc(size_t nmemb, size_t size) {
	return std::calloc(nmemb == 0 ? 1u : nmemb, size == 0 ? 1u : size);
}

static KYTY_SYSV_ABI void hle_free(void* ptr) {
	if (ptr == nullptr) {
		return;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	{
		std::scoped_lock lock(g_aligned_alloc_mutex);
		if (g_aligned_allocs.erase(ptr) > 0) {
			_aligned_free(ptr);
			return;
		}
	}
#endif
	std::free(ptr);
}

static KYTY_SYSV_ABI void* hle_realloc(void* ptr, size_t size) {
	if (ptr == nullptr) {
		return hle_malloc(size);
	}
	if (size == 0) {
		hle_free(ptr);
		return nullptr;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	{
		std::scoped_lock lock(g_aligned_alloc_mutex);
		if (g_aligned_allocs.contains(ptr)) {
			void* next = _aligned_realloc(ptr, size, 16u);
			if (next != nullptr && next != ptr) {
				g_aligned_allocs.erase(ptr);
				g_aligned_allocs.insert(next);
			}
			return next;
		}
	}
#endif
	return std::realloc(ptr, size);
}

static KYTY_SYSV_ABI void* hle_aligned_alloc(size_t alignment, size_t size) {
	const size_t align = std::max<size_t>(alignment, sizeof(void*));
	const size_t bytes = size == 0 ? align : size;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	void* ptr = _aligned_malloc(bytes, align);
	if (ptr != nullptr) {
		std::scoped_lock lock(g_aligned_alloc_mutex);
		g_aligned_allocs.insert(ptr);
	}
	return ptr;
#else
	void* ptr = nullptr;
	return ::posix_memalign(&ptr, align, bytes) == 0 ? ptr : nullptr;
#endif
}

static KYTY_SYSV_ABI int hle_posix_memalign(void** memptr, size_t alignment, size_t size) {
	if (memptr == nullptr || alignment < sizeof(void*) || (alignment & (alignment - 1u)) != 0) {
		return Posix::POSIX_EINVAL;
	}
	void* ptr = hle_aligned_alloc(alignment, size);
	if (ptr == nullptr) {
		return Posix::POSIX_ENOMEM;
	}
	*memptr = ptr;
	return 0;
}

static KYTY_SYSV_ABI char* hle_strdup(const char* s) {
	if (s == nullptr) {
		return nullptr;
	}
	const size_t len = std::strlen(s) + 1u;
	auto*        out = static_cast<char*>(std::malloc(len));
	if (out != nullptr) {
		std::memcpy(out, s, len);
	}
	return out;
}

static KYTY_SYSV_ABI char* hle_strndup(const char* s, size_t n) {
	if (s == nullptr) {
		return nullptr;
	}
	const size_t len = ::strnlen(s, n);
	auto*        out = static_cast<char*>(std::malloc(len + 1u));
	if (out != nullptr) {
		std::memcpy(out, s, len);
		out[len] = '\0';
	}
	return out;
}

static KYTY_SYSV_ABI const void* hle_memchr(const void* s, int c, size_t n) {
	return std::memchr(s, c, n);
}

static KYTY_SYSV_ABI size_t hle_strcspn(const char* s, const char* reject) {
	return std::strcspn(s, reject);
}

static KYTY_SYSV_ABI size_t hle_strspn(const char* s, const char* accept) {
	return std::strspn(s, accept);
}

static KYTY_SYSV_ABI const char* hle_strpbrk(const char* s, const char* accept) {
	return std::strpbrk(s, accept);
}

static KYTY_SYSV_ABI char* hle_strtok(char* str, const char* delim) {
	return std::strtok(str, delim);
}

static KYTY_SYSV_ABI char* hle_strtok_r(char* str, const char* delim, char** saveptr) {
	if (saveptr == nullptr) {
		return nullptr;
	}
	char* s = (str != nullptr) ? str : *saveptr;
	if (s == nullptr) {
		return nullptr;
	}
	s += std::strspn(s, delim);
	if (*s == '\0') {
		*saveptr = nullptr;
		return nullptr;
	}
	char* end = s + std::strcspn(s, delim);
	if (*end != '\0') {
		*end     = '\0';
		*saveptr = end + 1;
	} else {
		*saveptr = nullptr;
	}
	return s;
}

static KYTY_SYSV_ABI int hle_tolower(int c) {
	return std::tolower(c);
}

static KYTY_SYSV_ABI int hle_toupper(int c) {
	return std::toupper(c);
}

static KYTY_SYSV_ABI double hle_strtod(const char* nptr, char** endptr) {
	return std::strtod(nptr, endptr);
}

static KYTY_SYSV_ABI float hle_strtof(const char* nptr, char** endptr) {
	return std::strtof(nptr, endptr);
}

static KYTY_SYSV_ABI long long hle_strtoll(const char* nptr, char** endptr, int base) {
	return std::strtoll(nptr, endptr, base);
}

static KYTY_SYSV_ABI unsigned long long hle_strtoull(const char* nptr, char** endptr, int base) {
	return std::strtoull(nptr, endptr, base);
}

using guest_cmp_func_t = KYTY_SYSV_ABI int (*)(const void*, const void*);

static KYTY_SYSV_ABI void hle_qsort(void* base, size_t nmemb, size_t size, guest_cmp_func_t cmp) {
	if (base == nullptr || nmemb <= 1 || size == 0 || cmp == nullptr) {
		return;
	}
	auto*               bytes = static_cast<uint8_t*>(base);
	std::vector<size_t> order(nmemb);
	for (size_t i = 0; i < nmemb; ++i) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		return cmp(bytes + a * size, bytes + b * size) < 0;
	});
	std::vector<uint8_t> tmp(nmemb * size);
	for (size_t i = 0; i < nmemb; ++i) {
		std::memcpy(tmp.data() + i * size, bytes + order[i] * size, size);
	}
	std::memcpy(bytes, tmp.data(), nmemb * size);
}

static KYTY_SYSV_ABI void* hle_bsearch(const void* key, const void* base, size_t nmemb, size_t size,
                                       guest_cmp_func_t cmp) {
	if (key == nullptr || base == nullptr || nmemb == 0 || size == 0 || cmp == nullptr) {
		return nullptr;
	}
	const auto* bytes = static_cast<const uint8_t*>(base);
	size_t      lo    = 0;
	size_t      hi    = nmemb;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2u;
		const auto*  elm = bytes + mid * size;
		const int    c   = cmp(key, elm);
		if (c == 0) {
			return const_cast<uint8_t*>(elm);
		}
		if (c < 0) {
			hi = mid;
		} else {
			lo = mid + 1u;
		}
	}
	return nullptr;
}

static KYTY_SYSV_ABI char* hle_setlocale(int category, const char* locale) {
	return std::setlocale(category, locale);
}

static KYTY_SYSV_ABI std::lconv* hle_localeconv() {
	return std::localeconv();
}

static KYTY_SYSV_ABI std::FILE* hle_fopen(const char* path, const char* mode) {
	if (path == nullptr || mode == nullptr) {
		return nullptr;
	}
	const auto real = LibKernel::FileSystem::GetRealFilename(path);
	return std::fopen(Common::PathToString(real).c_str(), mode);
}

static KYTY_SYSV_ABI int hle_fclose(std::FILE* f) {
	return f != nullptr ? std::fclose(f) : 0;
}

static KYTY_SYSV_ABI size_t hle_fread(void* ptr, size_t size, size_t nmemb, std::FILE* f) {
	return f != nullptr ? std::fread(ptr, size, nmemb, f) : 0u;
}

static KYTY_SYSV_ABI size_t hle_fwrite(const void* ptr, size_t size, size_t nmemb, std::FILE* f) {
	return f != nullptr ? std::fwrite(ptr, size, nmemb, f) : 0u;
}

static KYTY_SYSV_ABI int hle_fseek(std::FILE* f, long offset, int whence) {
	return f != nullptr ? std::fseek(f, offset, whence) : -1;
}

static KYTY_SYSV_ABI long hle_ftell(std::FILE* f) {
	return f != nullptr ? std::ftell(f) : -1L;
}

static KYTY_SYSV_ABI int hle_ferror(std::FILE* f) {
	return f != nullptr ? std::ferror(f) : 0;
}

static KYTY_SYSV_ABI int hle_feof(std::FILE* f) {
	return f != nullptr ? std::feof(f) : 1;
}

static KYTY_SYSV_ABI char* hle_fgets(char* s, int size, std::FILE* f) {
	return f != nullptr ? std::fgets(s, size, f) : nullptr;
}

static KYTY_SYSV_ABI int hle_fputs(const char* s, std::FILE* f) {
	return (f != nullptr && s != nullptr) ? std::fputs(s, f) : -1;
}

static KYTY_SYSV_ABI int hle_putchar(int c) {
	return std::putchar(c);
}

static KYTY_SYSV_ABI long hle_lroundf(float x) {
	return std::lround(x);
}

static KYTY_SYSV_ABI float hle_expf(float x) {
	return std::exp(x);
}

static KYTY_SYSV_ABI float hle_powf(float x, float y) {
	return std::pow(x, y);
}

static KYTY_SYSV_ABI int hle_vsnprintf(char* s, size_t n, const char* format, VaList* c) {
	return GetGuestVsnprintfFunc()(s, n, format, c);
}

static KYTY_SYSV_ABI int hle_libc_stub_zero() {
	return 0;
}

static KYTY_SYSV_ABI int hle_mbtowc(wchar_t* pwc, const char* s, size_t n) {
	return std::mbtowc(pwc, s, n);
}

static KYTY_SYSV_ABI size_t hle_strlcpy(char* dst, const char* src, size_t size) {
	if (src == nullptr) {
		return 0u;
	}
	const size_t srclen = std::strlen(src);
	if (dst != nullptr && size > 0u) {
		const size_t copy = std::min<size_t>(srclen, size - 1u);
		std::memcpy(dst, src, copy);
		dst[copy] = '\0';
	}
	return srclen;
}

static KYTY_SYSV_ABI size_t hle_strlcat(char* dst, const char* src, size_t size) {
	if (dst == nullptr || src == nullptr) {
		return 0u;
	}
	const size_t dlen = ::strnlen(dst, size);
	const size_t slen = std::strlen(src);
	if (dlen == size) {
		return size + slen;
	}
	const size_t copy = std::min<size_t>(slen, size - dlen - 1u);
	std::memcpy(dst + dlen, src, copy);
	dst[dlen + copy] = '\0';
	return dlen + slen;
}

static KYTY_SYSV_ABI int hle_strcasecmp(const char* s1, const char* s2) {
	if (s1 == nullptr || s2 == nullptr) {
		return (s1 == s2) ? 0 : (s1 == nullptr ? -1 : 1);
	}
	while (*s1 != '\0' && *s2 != '\0') {
		const int a = std::tolower(static_cast<unsigned char>(*s1++));
		const int b = std::tolower(static_cast<unsigned char>(*s2++));
		if (a != b) {
			return a - b;
		}
	}
	return std::tolower(static_cast<unsigned char>(*s1)) -
	       std::tolower(static_cast<unsigned char>(*s2));
}

static KYTY_SYSV_ABI int hle_remove(const char* path) {
	if (path == nullptr || path[0] == '\0') {
		*Posix::GetErrorAddr() = Posix::POSIX_ENOENT;
		return -1;
	}
	const auto      real = LibKernel::FileSystem::GetRealFilename(path);
	std::error_code ec;
	if (std::filesystem::remove(real, ec)) {
		return 0;
	}
	*Posix::GetErrorAddr() = Posix::POSIX_ENOENT;
	return -1;
}

static KYTY_SYSV_ABI int hle_fprintf(VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
	auto*       stream = VaArg_ptr<std::FILE>(&ctx.va_list);
	const char* format = VaArg_ptr<const char>(&ctx.va_list);
	if (format == nullptr) {
		return -1;
	}
	char      buf[4096];
	const int len = GetGuestVsnprintfFunc()(buf, sizeof(buf), format, &ctx.va_list);
	if (len <= 0) {
		return len;
	}
	const size_t to_write = std::min<size_t>(static_cast<size_t>(len), sizeof(buf) - 1u);
	if (stream != nullptr && stream != stdout && stream != stderr) {
		(void)std::fwrite(buf, 1u, to_write, stream);
	} else {
		LOGF("%.*s", static_cast<int>(to_write), buf);
	}
	return len;
}

LIB_DEFINE(InitLibcInternal_1) {
	LibcInternalExt::InitLibcInternalExt_1(s);

	LIB_OBJECT("ZT4ODD2Ts9o", &LibcInternal::g_need_flag);
	LIB_OBJECT("2sWzhYqFH4E", stdout);

	LIB_FUNC("GMpvxPFW924", LibcInternal::vprintf);
	LIB_FUNC("MUjC4lbHrK4", LibcInternal::fflush);
	LIB_FUNC("8zTFvBIAIN8", LibcInternal::memset);
	LIB_FUNC("Q3VBxCXhUHs", LibcInternal::memcpy);
	LIB_FUNC("+P6FRGH4LfA", LibcInternal::memmove);
	LIB_FUNC("DfivPArhucg", LibcInternal::memcmp);
	LIB_FUNC("Ovb2dSJOAuE", LibcInternal::strcmp);
	LIB_FUNC("aesyjrHVWy4", LibcInternal::strncmp);
	LIB_FUNC("j4ViWNHEgww", LibcInternal::strlen);
	LIB_FUNC("kiZSXIWd9vg", LibcInternal::strcpy);
	LIB_FUNC("6sJWiWSRuqk", LibcInternal::strncpy);
	LIB_FUNC("Ls4tzzhimqQ", LibcInternal::strcat);
	LIB_FUNC("ob5xAW4ln-0", LibcInternal::strchr);
	LIB_FUNC("9yDWMxEFdJU", LibcInternal::strrchr);
	LIB_FUNC("viiwFMaNamA", LibcInternal::strstr);
	LIB_FUNC("mXlxhmLNMPg", LibcInternal::strtol);
	LIB_FUNC("QxmSHBCuKTk", LibcInternal::strtoul);
	LIB_FUNC("zlfEH8FmyUA", LibcInternal::strtoul);
	LIB_FUNC("fPxypibz2MY", LibcInternal::atoi);
	LIB_FUNC("Q4rRL34CEeE", LibcInternal::sinf);
	LIB_FUNC("-P6FNMzk2Kc", LibcInternal::cosf);
	LIB_FUNC("pztV4AF18iI", LibcInternal::sincosf);
	LIB_FUNC("H8ya2H00jbI", LibcInternal::sin);
	LIB_FUNC("2WE3BTYVwKM", LibcInternal::cos);
	LIB_FUNC("jMB7EFyu30Y", LibcInternal::sincos);
	LIB_FUNC("eLdDw6l0-bU", LibcInternal::snprintf);

	LIB_FUNC("smbQukfxYJM", LibC::getenv);
	LIB_FUNC("CRJcH8CnPSI", LibC::unsetenv);
	LIB_FUNC("-nvxBWa0iDs", LibcInternal::gethostname);
	LIB_FUNC("vhtcIgZG-Lk", LibcInternal::realpath);
	LIB_FUNC("ay3uROQAc5A", LibcInternal::opendir);
	LIB_FUNC("lybyyKtP54c", LibcInternal::readdir);
	LIB_FUNC("j-gWL6wDros", LibcInternal::getifaddrs);

	// HLE heap allocator & CRT (active when homebrew bundles a stubbed libc.prx)
	LIB_FUNC("gQX+4GDQjpM", LibcInternal::hle_malloc);
	LIB_FUNC("tIhsqj0qsFE", LibcInternal::hle_free);
	LIB_FUNC("Y7aJ1uydPMo", LibcInternal::hle_realloc);
	LIB_FUNC("2X5agFjKxMc", LibcInternal::hle_calloc);
	LIB_FUNC("cVSk9y8URbc", LibcInternal::hle_posix_memalign);
	LIB_FUNC("2Btkg8k24Zg", LibcInternal::hle_aligned_alloc);
	LIB_FUNC("Ujf3KzMvRmI", LibcInternal::hle_aligned_alloc);
	LIB_FUNC("g7zzzLDYGw0", LibcInternal::hle_strdup);
	LIB_FUNC("XGnuIBmEmyk", LibcInternal::hle_strndup);
	LIB_FUNC("8u8lPzUEq+U", LibcInternal::hle_memchr);
	LIB_FUNC("q0F6yS-rCms", LibcInternal::hle_strcspn);
	LIB_FUNC("-kU6bB4M-+k", LibcInternal::hle_strspn);
	LIB_FUNC("kDZvoVssCgQ", LibcInternal::hle_strpbrk);
	LIB_FUNC("oVkZ8W8-Q8A", LibcInternal::hle_strtok);
	LIB_FUNC("enqPGLfmVNU", LibcInternal::hle_strtok_r);
	LIB_FUNC("PqF+kHW-2WQ", LibcInternal::hle_tolower);
	LIB_FUNC("TYE4irxSmko", LibcInternal::hle_toupper);
	LIB_FUNC("2vDqwBlpF-o", LibcInternal::hle_strtod);
	LIB_FUNC("xENtRue8dpI", LibcInternal::hle_strtof);
	LIB_FUNC("VOBg+iNwB-4", LibcInternal::hle_strtoll);
	LIB_FUNC("5OqszGpy7Mg", LibcInternal::hle_strtoull);
	LIB_FUNC("AEJdIVZTEmo", LibcInternal::hle_qsort);
	LIB_FUNC("NesIgTmfF0Q", LibcInternal::hle_bsearch);
	LIB_FUNC("PtsB1Q9wsFA", LibcInternal::hle_setlocale);
	LIB_FUNC("0hlfW1O4Aa4", LibcInternal::hle_localeconv);
	LIB_FUNC("xeYO4u7uyJ0", LibcInternal::hle_fopen);
	LIB_FUNC("uodLYyUip20", LibcInternal::hle_fclose);
	LIB_FUNC("lbB+UlZqVG0", LibcInternal::hle_fread);
	LIB_FUNC("MpxhMh8QFro", LibcInternal::hle_fwrite);
	LIB_FUNC("rQFVBXp-Cxg", LibcInternal::hle_fseek);
	LIB_FUNC("Qazy8LmXTvw", LibcInternal::hle_ftell);
	LIB_FUNC("AHxyhN96dy4", LibcInternal::hle_ferror);
	LIB_FUNC("LxcEU+ICu8U", LibcInternal::hle_feof);
	LIB_FUNC("KdP-nULpuGw", LibcInternal::hle_fgets);
	LIB_FUNC("QrZZdJ8XsX0", LibcInternal::hle_fputs);
	LIB_FUNC("m5wN+SwZOR4", LibcInternal::hle_putchar);
	LIB_FUNC("C6gWCWJKM+U", LibcInternal::hle_lroundf);
	LIB_FUNC("8zsu04XNsZ4", LibcInternal::hle_expf);
	LIB_FUNC("1D0H2KNjshE", LibcInternal::hle_powf);
	LIB_FUNC("Q2V+iqvjgC0", LibcInternal::hle_vsnprintf);
	LIB_FUNC("QMFyLoqNxIg", LibcInternal::hle_libc_stub_zero);
	LIB_FUNC("6eU9xX9oEdQ", LibcInternal::hle_mbtowc);
	LIB_FUNC("SfQIZcqvvms", LibcInternal::hle_strlcpy);
	LIB_FUNC("ByfjUZsWiyg", LibcInternal::hle_strlcat);
	LIB_FUNC("AV6ipCNa4Rw", LibcInternal::hle_strcasecmp);
	LIB_FUNC("MZO7FXyAPU8", LibcInternal::hle_remove);
	LIB_FUNC("fffwELXNVFA", LibcInternal::hle_fprintf);
	LIB_FUNC("7Psx1DlAyE4", LibcInternal::hle_libc_stub_zero);

	LIB_FUNC("L1SBTkC+Cvw", LibC::abort);
	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);
	LIB_FUNC("DiGVep5yB5w", LibC::std_execute_once);

	LIB_FUNC("al3JzFI9MQ0", LibcInternal::LibcHeapErrorReportForGame);
}

} // namespace LibcInternal

namespace LibC {

LIB_DEFINE(InitLibC_1) {
	LibcInternal::InitLibcInternal_1(s);

	LIB_OBJECT("P330P3dFF68", &LibC::g_need_flag);

	LIB_FUNC("uMei1W9uyNo", LibC::exit);
	LIB_FUNC("L1SBTkC+Cvw", LibC::abort);
	LIB_FUNC("9BcDykPmo1I", LibC::libc_error);
	LIB_FUNC("bzQExy189ZI", LibC::init_env);
	LIB_FUNC("8G2LB+A3rzg", LibC::atexit);
	LIB_FUNC("hcuQgD53UxM", LibC::libc_printf);
	LIB_FUNC("YQ0navp+YIc", LibC::puts);
	LIB_FUNC("smbQukfxYJM", LibC::getenv);
	LIB_FUNC("M4YYbSFfJ8g", LibC::setenv);
	LIB_FUNC("CRJcH8CnPSI", LibC::unsetenv);
	LIB_FUNC("wLlFkwG9UcQ", LibC::libc_time);
	LIB_FUNC("-VVn74ZyhEs", LibC::libc_difftime);
	LIB_FUNC("1mecP7RgI2A", LibC::libc_gmtime);
	LIB_FUNC("efhK-YSUYYQ", LibC::libc_localtime);
	LIB_FUNC("n7AepwR0s34", LibC::libc_mktime);
	LIB_FUNC("Av3zjWi64Kw", LibC::libc_strftime);
	LIB_FUNC("XKRegsFpEpk", LibC::catchReturnFromMain);
	LIB_FUNC("tsvEmnenz48", LibC::cxa_atexit);
	LIB_FUNC("H2e8t5ScQGc", LibC::cxa_finalize);
	LIB_FUNC("DiGVep5yB5w", LibC::std_execute_once);
}

} // namespace LibC

} // namespace Libs
