#ifndef EMULATOR_SRC_LIBS_LIBFIOS2_H_
#define EMULATOR_SRC_LIBS_LIBFIOS2_H_

#include "common/abi.h"

#include <cstdint>

namespace Loader {
class SymbolDatabase;
} // namespace Loader

namespace Libs::LibFios2 {

using SceFiosOp        = int32_t;
using SceFiosFH        = int32_t;
using SceFiosOverlayID = int32_t;
using SceFiosOffset    = int64_t;
using SceFiosSize      = int64_t;

inline constexpr SceFiosOp kFiosOpInvalid = 0;
inline constexpr SceFiosFH kFiosFhInvalid = 0;

inline constexpr int32_t SCE_FIOS_OK                  = 0;
inline constexpr int32_t SCE_FIOS_ERROR_UNINITIALIZED = static_cast<int32_t>(0x80810001u);
inline constexpr int32_t SCE_FIOS_ERROR_BAD_PATH      = static_cast<int32_t>(0x80810004u);
inline constexpr int32_t SCE_FIOS_ERROR_BAD_PTR       = static_cast<int32_t>(0x80810005u);
inline constexpr int32_t SCE_FIOS_ERROR_BAD_FH        = static_cast<int32_t>(0x80810008u);
inline constexpr int32_t SCE_FIOS_ERROR_BAD_OP        = static_cast<int32_t>(0x80810009u);
inline constexpr int32_t SCE_FIOS_ERROR_CANT_OPEN     = static_cast<int32_t>(0x8081000bu);
inline constexpr int32_t SCE_FIOS_ERROR_READ          = static_cast<int32_t>(0x8081000du);
inline constexpr int32_t SCE_FIOS_ERROR_ACCESS        = static_cast<int32_t>(0x80810012u);

enum class SceFiosWhence : int32_t {
	Set = 0,
	Cur = 1,
	End = 2,
};

struct SceFiosParams {
	uint32_t initialized     : 1;
	uint32_t paramsSize      : 15;
	uint32_t pathMax         : 16;
	uint32_t profiling;
	uint32_t ioThreadCount;
	uint32_t threadsPerScheduler;
	uint32_t extraFlag1      : 1;
	uint32_t extraFlags      : 31;
	uint32_t maxChunk;
	uint8_t  maxDecompressorThreadCount;
	uint8_t  reserved1;
	uint8_t  reserved2;
	uint8_t  reserved3;
	intptr_t reserved4;
	intptr_t reserved5;
};

struct SceFiosOpenParams {
	uint16_t openFlags;
	uint16_t opFlags;
	uint32_t reserved;
	void*    buffer;
	size_t   bufferSize;
};

struct SceFiosStat {
	SceFiosOffset fileSize;
	uint32_t      statFlags;
	uint32_t      nameLength;
	uint64_t      creationDate;
	uint64_t      modificationDate;
	uint64_t      accessDate;
	char          name[256];
};

struct SceFiosOverlay {
	uint8_t  type;
	uint8_t  order;
	uint16_t dstLen;
	uint16_t srcLen;
	uint16_t reserved2;
	int32_t  pid;
	int32_t  reserved4;
	char     dst[1024];
	char     src[1024];
};

// Registers libSceFios2 NIDs into the runtime linker symbol database.
void InitFios2_1(Loader::SymbolDatabase* s);

// Direct C++ entry points (matching the guest SysV ABI functions) for unit tests and HLE bridging.
int32_t       KYTY_SYSV_ABI FiosInitialize(const SceFiosParams* params);
void          KYTY_SYSV_ABI FiosTerminate();
int32_t       KYTY_SYSV_ABI FiosIsInitialized();
SceFiosOp     KYTY_SYSV_ABI FiosFHOpen(const void* attr, SceFiosFH* out_fh, const char* path,
                                       const SceFiosOpenParams* open_params);
int32_t       KYTY_SYSV_ABI FiosFHOpenSync(const void* attr, SceFiosFH* out_fh, const char* path,
                                           const SceFiosOpenParams* open_params);
SceFiosOp     KYTY_SYSV_ABI FiosFHRead(const void* attr, SceFiosFH fh, void* buf,
                                       SceFiosSize length);
SceFiosSize   KYTY_SYSV_ABI FiosFHReadSync(const void* attr, SceFiosFH fh, void* buf,
                                           SceFiosSize length);
SceFiosOp     KYTY_SYSV_ABI FiosFHPread(const void* attr, SceFiosFH fh, void* buf,
                                        SceFiosSize length, SceFiosOffset offset);
SceFiosSize   KYTY_SYSV_ABI FiosFHPreadSync(const void* attr, SceFiosFH fh, void* buf,
                                            SceFiosSize length, SceFiosOffset offset);
SceFiosOffset KYTY_SYSV_ABI FiosFHSeek(SceFiosFH fh, SceFiosOffset offset, SceFiosWhence whence);
SceFiosOffset KYTY_SYSV_ABI FiosFHTell(SceFiosFH fh);
SceFiosSize   KYTY_SYSV_ABI FiosFHGetSize(SceFiosFH fh);
SceFiosOp     KYTY_SYSV_ABI FiosFHClose(const void* attr, SceFiosFH fh);
int32_t       KYTY_SYSV_ABI FiosFHCloseSync(const void* attr, SceFiosFH fh);
SceFiosOp     KYTY_SYSV_ABI FiosStat(const void* attr, const char* path, SceFiosStat* out_stat);
int32_t       KYTY_SYSV_ABI FiosStatSync(const void* attr, const char* path, SceFiosStat* out_stat);
int32_t       KYTY_SYSV_ABI FiosFileExistsSync(const void* attr, const char* path);
SceFiosSize   KYTY_SYSV_ABI FiosFileGetSizeSync(const void* attr, const char* path);
int32_t       KYTY_SYSV_ABI FiosOpIsDone(SceFiosOp op);
int32_t       KYTY_SYSV_ABI FiosOpWait(SceFiosOp op);
int32_t       KYTY_SYSV_ABI FiosOpWaitForDone(SceFiosOp op, int64_t timeout_ns);
int32_t       KYTY_SYSV_ABI FiosOpGetError(SceFiosOp op);
SceFiosSize   KYTY_SYSV_ABI FiosOpGetActualCount(SceFiosOp op);
void          KYTY_SYSV_ABI FiosOpDelete(SceFiosOp op);
int32_t       KYTY_SYSV_ABI FiosOverlayAdd(const SceFiosOverlay* overlay, SceFiosOverlayID* out_id);
int32_t       KYTY_SYSV_ABI FiosOverlayRemove(SceFiosOverlayID id);
int32_t       KYTY_SYSV_ABI FiosOverlayResolveSync(int32_t process_id, int32_t resolve_flag,
                                                   const char* in_path, char* out_path,
                                                   size_t max_path);

} // namespace Libs::LibFios2

#endif // EMULATOR_SRC_LIBS_LIBFIOS2_H_
