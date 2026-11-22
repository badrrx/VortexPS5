#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "kernel/module.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <mutex>
#include <unordered_map>

namespace Libs {

LIB_VERSION("Sysmodule", 1, "Sysmodule", 1, 1);

namespace LibKernel {
struct ModuleInfoForUnwind;
int KYTY_SYSV_ABI KernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info);
} // namespace LibKernel

namespace Sysmodule {

constexpr int      SYSMODULE_ERROR_UNLOADED = static_cast<int>(0x805a1001u);
constexpr int      SYSMODULE_ERROR_FATAL    = static_cast<int>(0x805a10ffu);

static constexpr struct NativeModule {
	uint16_t    id;
	const char* path;
} g_native_modules[] = {{0x0115, "/app0/sce_module/libSceNpCppWebApi.prx"}};

struct Module {
	uint32_t                references = 0;
	LibKernel::KernelModule handle     = -1;
};

static std::mutex                          g_module_mutex;
static std::unordered_map<uint16_t, Module> g_modules;

static int LoadModule(uint16_t id) {
	std::scoped_lock lock(g_module_mutex);
	if (const auto module = g_modules.find(id); module != g_modules.end()) {
		++module->second.references;
		return 0;
	}

	LibKernel::KernelModule handle = -1;
	for (const auto& module: g_native_modules) {
		if (module.id != id) continue;
		handle = LibKernel::KernelLoadStartModule(module.path, 0, nullptr, 0, nullptr, nullptr);
		if (handle < 0) {
			LOGF_COLOR(Log::Color::Yellow,
			           "\t Sysmodule: optional native module 0x%04x (%s) unavailable (err=0x%08x)"
			           " — falling back to HLE stub\n",
			           static_cast<unsigned>(id), module.path, static_cast<uint32_t>(handle));
			handle = -1;
		}
		break;
	}
	g_modules.emplace(id, Module {1, handle});
	return 0;
}

static KYTY_SYSV_ABI int SysmoduleGetModuleInfoForUnwind(uint64_t addr, int flags,
                                                         LibKernel::ModuleInfoForUnwind* info) {
	return LibKernel::KernelGetModuleInfoForUnwind(addr, flags, info);
}

KYTY_SYSV_ABI int SysmoduleLoadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = 0x%04x\n", static_cast<unsigned>(id));

	return LoadModule(id);
}

KYTY_SYSV_ABI int SysmoduleUnloadModule(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = 0x%04x\n", static_cast<unsigned>(id));

	std::scoped_lock lock(g_module_mutex);
	const auto       module = g_modules.find(id);
	if (module == g_modules.end()) {
		return SYSMODULE_ERROR_UNLOADED;
	}
	if (module->second.references == 1) {
		if (module->second.handle >= 0 &&
		    LibKernel::KernelStopUnloadModule(module->second.handle, 0, nullptr, 0, nullptr,
		                                     nullptr) < 0) {
			return SYSMODULE_ERROR_FATAL;
		}
		g_modules.erase(id);
	} else {
		--module->second.references;
	}
	return 0;
}

KYTY_SYSV_ABI int SysmoduleLoadModuleInternalWithArg(uint16_t id, int arg1, int arg2, int arg3,
                                                     int* ret) {
	PRINT_NAME();

	LOGF("\t id = 0x%04x, arg1 = %d, arg2 = %d, arg3 = %d\n", static_cast<unsigned>(id), arg1, arg2,
	     arg3);

	if (arg1 != 0 || arg2 != 0 || arg3 != 0) {
		LOGF_COLOR(Log::Color::Yellow,
		           "\t SysmoduleLoadModuleInternalWithArg: accepting non-zero args for id=0x%04x\n",
		           static_cast<unsigned>(id));
	}

	const int result = LoadModule(id);
	if (ret != nullptr) {
		*ret = result;
	}
	return result;
}

KYTY_SYSV_ABI int SysmoduleIsLoaded(uint16_t id) {
	PRINT_NAME();

	LOGF("\t id = 0x%04x\n", static_cast<unsigned>(id));

	std::scoped_lock lock(g_module_mutex);
	return g_modules.contains(id) ? 0 : SYSMODULE_ERROR_UNLOADED;
}

} // namespace Sysmodule

LIB_DEFINE(InitSysmodule_1) {
	LIB_FUNC("4fU5yvOkVG4", Sysmodule::SysmoduleGetModuleInfoForUnwind);
	LIB_FUNC("eR2bZFAAU0Q", Sysmodule::SysmoduleUnloadModule);
	LIB_FUNC("hHrGoGoNf+s", Sysmodule::SysmoduleLoadModuleInternalWithArg);
	LIB_FUNC("g8cM39EUZ6o", Sysmodule::SysmoduleLoadModule);
	LIB_FUNC("fMP5NHUOaMk", Sysmodule::SysmoduleIsLoaded);
}

} // namespace Libs
