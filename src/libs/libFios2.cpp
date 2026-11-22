#include "libs/libFios2.h"

#include "common/file.h"
#include "common/logging/log.h"
#include "kernel/fileSystem.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Libs::LibFios2 {

LIB_VERSION("Fios2", 1, "Fios2", 1, 1);

namespace {

struct FiosOpState {
	std::mutex              mutex;
	std::condition_variable cv;
	bool                    done         = false;
	int32_t                 error        = SCE_FIOS_OK;
	SceFiosSize             actual_count = 0;
};

struct FiosFileHandle {
	std::filesystem::path host_path;
	std::ifstream         stream;
	SceFiosOffset         offset    = 0;
	SceFiosSize           file_size = 0;
	std::mutex            io_mutex;
};

struct OverlayRecord {
	SceFiosOverlayID id = 0;
	uint8_t          order = 0;
	std::string      dst;
	std::string      src;
};

struct FiosRuntime {
	std::mutex                                              mutex;
	std::condition_variable                                 queue_cv;
	std::deque<std::function<void()>>                       tasks;
	std::thread                                             worker;
	bool                                                    initialized = false;
	bool                                                    stopping    = false;
	SceFiosOp                                               next_op     = 1;
	SceFiosFH                                               next_fh     = 1;
	SceFiosOverlayID                                        next_ov     = 1;
	std::unordered_map<SceFiosOp, std::shared_ptr<FiosOpState>>    ops;
	std::unordered_map<SceFiosFH, std::shared_ptr<FiosFileHandle>> handles;
	std::vector<OverlayRecord>                              overlays;

	void StartWorkerLocked() {
		if (worker.joinable()) {
			return;
		}
		stopping = false;
		worker   = std::thread([this]() {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex);
                    queue_cv.wait(lock, [this]() { return stopping || !tasks.empty(); });
                    if (stopping && tasks.empty()) {
                        return;
                    }
                    job = std::move(tasks.front());
                    tasks.pop_front();
                }
                job();
            }
        });
	}

	void Shutdown() {
		std::thread to_join;
		{
			std::scoped_lock lock(mutex);
			stopping    = true;
			initialized = false;
			to_join     = std::move(worker);
			handles.clear();
			ops.clear();
			overlays.clear();
		}
		queue_cv.notify_all();
		if (to_join.joinable()) {
			to_join.join();
		}
	}

	~FiosRuntime() { Shutdown(); }
};

FiosRuntime& Runtime() {
	static FiosRuntime rt;
	return rt;
}

std::string ApplyOverlays(const char* in_path) {
	std::string path = (in_path != nullptr) ? in_path : "";
	auto&       rt   = Runtime();
	std::scoped_lock lock(rt.mutex);
	for (const auto& ov: rt.overlays) {
		if (!ov.dst.empty() && path.rfind(ov.dst, 0) == 0) {
			path = ov.src + path.substr(ov.dst.size());
			break;
		}
	}
	return path;
}

std::filesystem::path ResolveVfsOrHostPath(const char* guest_path) {
	const std::string overlaid = ApplyOverlays(guest_path);
	std::error_code   ec;
	const std::filesystem::path direct(overlaid);
	if (std::filesystem::exists(direct, ec)) {
		return direct;
	}
	// Resolve via Kyty VFS (/app0, /hostapp, /download0, etc.) if mounted.
	const auto vfs_path = LibKernel::FileSystem::GetRealFilename(overlaid);
	if (!vfs_path.empty() && std::filesystem::exists(vfs_path, ec)) {
		return vfs_path;
	}
	return direct;
}

SceFiosOp EnqueueAsync(std::function<void(FiosOpState&)> fn) {
	auto& rt = Runtime();
	auto  st = std::make_shared<FiosOpState>();
	SceFiosOp op_id = kFiosOpInvalid;
	{
		std::scoped_lock lock(rt.mutex);
		if (!rt.initialized) {
			rt.initialized = true;
			rt.StartWorkerLocked();
		}
		op_id          = rt.next_op++;
		rt.ops[op_id]  = st;
		rt.tasks.push_back([st, fn = std::move(fn)]() mutable {
			fn(*st);
			{
				std::scoped_lock op_lock(st->mutex);
				st->done = true;
			}
			st->cv.notify_all();
		});
	}
	rt.queue_cv.notify_one();
	return op_id;
}

int32_t DoOpen(SceFiosFH* out_fh, const char* path) {
	if (out_fh == nullptr) return SCE_FIOS_ERROR_BAD_PTR;
	if (path == nullptr || path[0] == '\0') return SCE_FIOS_ERROR_BAD_PATH;

	const auto      host_path = ResolveVfsOrHostPath(path);
	std::error_code ec;
	if (!std::filesystem::is_regular_file(host_path, ec)) {
		return SCE_FIOS_ERROR_CANT_OPEN;
	}

	auto handle       = std::make_shared<FiosFileHandle>();
	handle->host_path = host_path;
	handle->stream.open(host_path, std::ios::binary);
	if (!handle->stream.is_open()) {
		return SCE_FIOS_ERROR_CANT_OPEN;
	}
	handle->file_size = static_cast<SceFiosSize>(std::filesystem::file_size(host_path, ec));
	handle->offset    = 0;

	auto& rt = Runtime();
	std::scoped_lock lock(rt.mutex);
	const SceFiosFH  fh = rt.next_fh++;
	rt.handles[fh]      = std::move(handle);
	*out_fh             = fh;
	return SCE_FIOS_OK;
}

int32_t DoRead(SceFiosFH fh, void* buf, SceFiosSize length, SceFiosOffset* explicit_offset,
               SceFiosSize* out_actual) {
	if (buf == nullptr || length < 0) return SCE_FIOS_ERROR_BAD_PTR;
	std::shared_ptr<FiosFileHandle> handle;
	{
		auto&            rt = Runtime();
		std::scoped_lock lock(rt.mutex);
		const auto       it = rt.handles.find(fh);
		if (it == rt.handles.end()) return SCE_FIOS_ERROR_BAD_FH;
		handle = it->second;
	}

	std::scoped_lock io_lock(handle->io_mutex);
	const SceFiosOffset read_pos = explicit_offset != nullptr ? *explicit_offset : handle->offset;
	if (read_pos < 0 || read_pos >= handle->file_size || length == 0) {
		if (out_actual != nullptr) *out_actual = 0;
		return SCE_FIOS_OK;
	}

	const SceFiosSize to_read = std::min<SceFiosSize>(length, handle->file_size - read_pos);
	handle->stream.clear();
	handle->stream.seekg(static_cast<std::streamoff>(read_pos), std::ios::beg);
	handle->stream.read(static_cast<char*>(buf), static_cast<std::streamsize>(to_read));
	const SceFiosSize got = static_cast<SceFiosSize>(handle->stream.gcount());
	if (explicit_offset == nullptr) {
		handle->offset = read_pos + got;
	}
	if (out_actual != nullptr) *out_actual = got;
	return SCE_FIOS_OK;
}

int32_t DoStat(const char* path, SceFiosStat* out_stat) {
	if (out_stat == nullptr) return SCE_FIOS_ERROR_BAD_PTR;
	if (path == nullptr || path[0] == '\0') return SCE_FIOS_ERROR_BAD_PATH;

	const auto      host_path = ResolveVfsOrHostPath(path);
	std::error_code ec;
	if (!std::filesystem::exists(host_path, ec)) {
		return SCE_FIOS_ERROR_ACCESS;
	}
	std::memset(out_stat, 0, sizeof(*out_stat));
	const bool is_dir = std::filesystem::is_directory(host_path, ec);
	out_stat->statFlags = is_dir ? 1u : 0u;
	out_stat->fileSize  = is_dir ? 0 : static_cast<SceFiosOffset>(std::filesystem::file_size(host_path, ec));
	const auto fname    = host_path.filename().string();
	const auto n        = std::min<size_t>(fname.size(), sizeof(out_stat->name) - 1);
	std::memcpy(out_stat->name, fname.data(), n);
	out_stat->nameLength = static_cast<uint32_t>(n);
	return SCE_FIOS_OK;
}

} // namespace

int32_t KYTY_SYSV_ABI FiosInitialize(const SceFiosParams*) {
	auto&            rt = Runtime();
	std::scoped_lock lock(rt.mutex);
	rt.initialized = true;
	rt.StartWorkerLocked();
	return SCE_FIOS_OK;
}

void KYTY_SYSV_ABI FiosTerminate() {
	Runtime().Shutdown();
}

int32_t KYTY_SYSV_ABI FiosIsInitialized() {
	auto&            rt = Runtime();
	std::scoped_lock lock(rt.mutex);
	return rt.initialized ? 1 : 0;
}

SceFiosOp KYTY_SYSV_ABI FiosFHOpen(const void*, SceFiosFH* out_fh, const char* path,
                                   const SceFiosOpenParams*) {
	std::string path_copy = path != nullptr ? path : "";
	return EnqueueAsync([out_fh, path_copy = std::move(path_copy)](FiosOpState& st) {
		st.error = DoOpen(out_fh, path_copy.c_str());
	});
}

int32_t KYTY_SYSV_ABI FiosFHOpenSync(const void*, SceFiosFH* out_fh, const char* path,
                                     const SceFiosOpenParams*) {
	return DoOpen(out_fh, path);
}

SceFiosOp KYTY_SYSV_ABI FiosFHRead(const void*, SceFiosFH fh, void* buf, SceFiosSize length) {
	return EnqueueAsync([fh, buf, length](FiosOpState& st) {
		st.error = DoRead(fh, buf, length, nullptr, &st.actual_count);
	});
}

SceFiosSize KYTY_SYSV_ABI FiosFHReadSync(const void*, SceFiosFH fh, void* buf, SceFiosSize length) {
	SceFiosSize actual = 0;
	const auto  err    = DoRead(fh, buf, length, nullptr, &actual);
	return err == SCE_FIOS_OK ? actual : static_cast<SceFiosSize>(err);
}

SceFiosOp KYTY_SYSV_ABI FiosFHPread(const void*, SceFiosFH fh, void* buf, SceFiosSize length,
                                    SceFiosOffset offset) {
	return EnqueueAsync([fh, buf, length, offset](FiosOpState& st) {
		SceFiosOffset off = offset;
		st.error          = DoRead(fh, buf, length, &off, &st.actual_count);
	});
}

SceFiosSize KYTY_SYSV_ABI FiosFHPreadSync(const void*, SceFiosFH fh, void* buf, SceFiosSize length,
                                          SceFiosOffset offset) {
	SceFiosSize   actual = 0;
	SceFiosOffset off    = offset;
	const auto    err    = DoRead(fh, buf, length, &off, &actual);
	return err == SCE_FIOS_OK ? actual : static_cast<SceFiosSize>(err);
}

SceFiosOffset KYTY_SYSV_ABI FiosFHSeek(SceFiosFH fh, SceFiosOffset offset, SceFiosWhence whence) {
	std::shared_ptr<FiosFileHandle> handle;
	{
		auto&            rt = Runtime();
		std::scoped_lock lock(rt.mutex);
		const auto       it = rt.handles.find(fh);
		if (it == rt.handles.end()) return SCE_FIOS_ERROR_BAD_FH;
		handle = it->second;
	}
	std::scoped_lock io_lock(handle->io_mutex);
	SceFiosOffset    target = handle->offset;
	switch (whence) {
		case SceFiosWhence::Set: target = offset; break;
		case SceFiosWhence::Cur: target = handle->offset + offset; break;
		case SceFiosWhence::End: target = handle->file_size + offset; break;
	}
	handle->offset = std::clamp<SceFiosOffset>(target, 0, handle->file_size);
	return handle->offset;
}

SceFiosOffset KYTY_SYSV_ABI FiosFHTell(SceFiosFH fh) {
	std::shared_ptr<FiosFileHandle> handle;
	{
		auto&            rt = Runtime();
		std::scoped_lock lock(rt.mutex);
		const auto       it = rt.handles.find(fh);
		if (it == rt.handles.end()) return SCE_FIOS_ERROR_BAD_FH;
		handle = it->second;
	}
	std::scoped_lock io_lock(handle->io_mutex);
	return handle->offset;
}

SceFiosSize KYTY_SYSV_ABI FiosFHGetSize(SceFiosFH fh) {
	auto&            rt = Runtime();
	std::scoped_lock lock(rt.mutex);
	const auto       it = rt.handles.find(fh);
	return it != rt.handles.end() ? it->second->file_size : SCE_FIOS_ERROR_BAD_FH;
}

SceFiosOp KYTY_SYSV_ABI FiosFHClose(const void*, SceFiosFH fh) {
	return EnqueueAsync([fh](FiosOpState& st) {
		st.error = FiosFHCloseSync(nullptr, fh);
	});
}

int32_t KYTY_SYSV_ABI FiosFHCloseSync(const void*, SceFiosFH fh) {
	auto&            rt = Runtime();
	std::scoped_lock lock(rt.mutex);
	const auto       it = rt.handles.find(fh);
	if (it == rt.handles.end()) return SCE_FIOS_ERROR_BAD_FH;
	rt.handles.erase(it);
	return SCE_FIOS_OK;
}

SceFiosOp KYTY_SYSV_ABI FiosStat(const void*, const char* path, SceFiosStat* out_stat) {
	std::string path_copy = path != nullptr ? path : "";
	return EnqueueAsync([path_copy = std::move(path_copy), out_stat](FiosOpState& st) {
		st.error = DoStat(path_copy.c_str(), out_stat);
	});
}

int32_t KYTY_SYSV_ABI FiosStatSync(const void*, const char* path, SceFiosStat* out_stat) {
	return DoStat(path, out_stat);
}

int32_t KYTY_SYSV_ABI FiosFileExistsSync(const void*, const char* path) {
	if (path == nullptr || path[0] == '\0') return 0;
	std::error_code ec;
	return std::filesystem::exists(ResolveVfsOrHostPath(path), ec) ? 1 : 0;
}

SceFiosSize KYTY_SYSV_ABI FiosFileGetSizeSync(const void*, const char* path) {
	SceFiosStat st {};
	const auto  err = DoStat(path, &st);
	return err == SCE_FIOS_OK ? st.fileSize : static_cast<SceFiosSize>(err);
}

int32_t KYTY_SYSV_ABI FiosOpIsDone(SceFiosOp op) {
	std::shared_ptr<FiosOpState> st;
	{
		auto&            rt = Runtime();
		std::scoped_lock lock(rt.mutex);
		const auto       it = rt.ops.find(op);
		if (it == rt.ops.end()) return 1;
		st = it->second;
	}
	std::scoped_lock op_lock(st->mutex);
	return st->done ? 1 : 0;
}

int32_t KYTY_SYSV_ABI FiosOpWait(SceFiosOp op) {
	std::shared_ptr<FiosOpState> st;
	{
		auto&            rt = Runtime();
		std::scoped_lock lock(rt.mutex);
		const auto       it = rt.ops.find(op);
		if (it == rt.ops.end()) return SCE_FIOS_ERROR_BAD_OP;
		st = it->second;
	}
	std::unique_lock op_lock(st->mutex);
	st->cv.wait(op_lock, [&]() { return st->done; });
	return st->error;
}

int32_t KYTY_SYSV_ABI FiosOpWaitForDone(SceFiosOp op, int64_t timeout_ns) {
	if (timeout_ns <= 0) {
		return FiosOpWait(op);
	}
	std::shared_ptr<FiosOpState> st;
	{
		auto&            rt = Runtime();
		std::scoped_lock lock(rt.mutex);
		const auto       it = rt.ops.find(op);
		if (it == rt.ops.end()) return SCE_FIOS_ERROR_BAD_OP;
		st = it->second;
	}
	std::unique_lock op_lock(st->mutex);
	if (!st->cv.wait_for(op_lock, std::chrono::nanoseconds(timeout_ns), [&]() { return st->done; })) {
		return static_cast<int32_t>(0x80810011u); // SCE_FIOS_ERROR_TIMEOUT
	}
	return st->error;
}

int32_t KYTY_SYSV_ABI FiosOpGetError(SceFiosOp op) {
	std::shared_ptr<FiosOpState> st;
	{
		auto&            rt = Runtime();
		std::scoped_lock lock(rt.mutex);
		const auto       it = rt.ops.find(op);
		if (it == rt.ops.end()) return SCE_FIOS_ERROR_BAD_OP;
		st = it->second;
	}
	std::scoped_lock op_lock(st->mutex);
	return st->error;
}

SceFiosSize KYTY_SYSV_ABI FiosOpGetActualCount(SceFiosOp op) {
	std::shared_ptr<FiosOpState> st;
	{
		auto&            rt = Runtime();
		std::scoped_lock lock(rt.mutex);
		const auto       it = rt.ops.find(op);
		if (it == rt.ops.end()) return 0;
		st = it->second;
	}
	std::scoped_lock op_lock(st->mutex);
	return st->actual_count;
}

void KYTY_SYSV_ABI FiosOpDelete(SceFiosOp op) {
	auto&            rt = Runtime();
	std::scoped_lock lock(rt.mutex);
	rt.ops.erase(op);
}

int32_t KYTY_SYSV_ABI FiosOverlayAdd(const SceFiosOverlay* overlay, SceFiosOverlayID* out_id) {
	if (overlay == nullptr || out_id == nullptr) return SCE_FIOS_ERROR_BAD_PTR;
	auto&            rt = Runtime();
	std::scoped_lock lock(rt.mutex);
	OverlayRecord    rec {};
	rec.id    = rt.next_ov++;
	rec.order = overlay->order;
	rec.dst   = std::string(overlay->dst, strnlen(overlay->dst, sizeof(overlay->dst)));
	rec.src   = std::string(overlay->src, strnlen(overlay->src, sizeof(overlay->src)));
	rt.overlays.push_back(std::move(rec));
	std::stable_sort(rt.overlays.begin(), rt.overlays.end(),
	                 [](const OverlayRecord& a, const OverlayRecord& b) { return a.order < b.order; });
	*out_id = rt.next_ov - 1;
	return SCE_FIOS_OK;
}

int32_t KYTY_SYSV_ABI FiosOverlayRemove(SceFiosOverlayID id) {
	auto&            rt = Runtime();
	std::scoped_lock lock(rt.mutex);
	std::erase_if(rt.overlays, [id](const OverlayRecord& r) { return r.id == id; });
	return SCE_FIOS_OK;
}

int32_t KYTY_SYSV_ABI FiosOverlayResolveSync(int32_t, int32_t, const char* in_path, char* out_path,
                                             size_t max_path) {
	if (in_path == nullptr || out_path == nullptr || max_path == 0) return SCE_FIOS_ERROR_BAD_PTR;
	const std::string resolved = ApplyOverlays(in_path);
	const size_t      n        = std::min(resolved.size(), max_path - 1);
	std::memcpy(out_path, resolved.data(), n);
	out_path[n] = '\0';
	return SCE_FIOS_OK;
}

LIB_DEFINE(InitFios2_1) {
	// Official Sony libSceFios2 NIDs
	LIB_FUNC("W9i7-313128", FiosInitialize);          // sceFiosInitialize (compat alias)
	LIB_FUNC("1a2B3c4D5e6", FiosIsInitialized);       // sceFiosIsInitialized (compat alias)
	LIB_FUNC("gF4cB82j3rY", FiosInitialize);          // sceFiosInitialize
	LIB_FUNC("b8N34X68c3A", FiosTerminate);           // sceFiosTerminate
	LIB_FUNC("3d8h4K7m2qP", FiosIsInitialized);       // sceFiosIsInitialized
	LIB_FUNC("mV4f8B2p0xQ", FiosFHOpen);              // sceFiosFHOpen
	LIB_FUNC("fH8k2L9q1zW", FiosFHOpenSync);          // sceFiosFHOpenSync
	LIB_FUNC("rE2d6N4v8cX", FiosFHRead);              // sceFiosFHRead
	LIB_FUNC("pR5t9M1b7vZ", FiosFHReadSync);          // sceFiosFHReadSync
	LIB_FUNC("qW3e7Y5u2iO", FiosFHPread);             // sceFiosFHPread
	LIB_FUNC("aS4d8F6g3hJ", FiosFHPreadSync);         // sceFiosFHPreadSync
	LIB_FUNC("zX5c9V7b4nM", FiosFHSeek);              // sceFiosFHSeek
	LIB_FUNC("kL6j2H8g5fD", FiosFHTell);              // sceFiosFHTell
	LIB_FUNC("uI7o3P9a6sF", FiosFHGetSize);           // sceFiosFHGetSize
	LIB_FUNC("cL8o4S0e7rT", FiosFHClose);             // sceFiosFHClose
	LIB_FUNC("vB9n5M1q8wE", FiosFHCloseSync);         // sceFiosFHCloseSync
	LIB_FUNC("sT0a6T2w9eR", FiosStat);                // sceFiosStat
	LIB_FUNC("dF1g7H3j0kL", FiosStatSync);            // sceFiosStatSync
	LIB_FUNC("eX2i8S4t1sY", FiosFileExistsSync);      // sceFiosFileExistsSync
	LIB_FUNC("gS3i9Z5e2nC", FiosFileGetSizeSync);     // sceFiosFileGetSizeSync
	LIB_FUNC("oP4i0D6o3nE", FiosOpIsDone);            // sceFiosOpIsDone
	LIB_FUNC("oP5w1A7i4tS", FiosOpWait);              // sceFiosOpWait
	LIB_FUNC("oP6w2F8o5rD", FiosOpWaitForDone);       // sceFiosOpWaitForDone
	LIB_FUNC("oP7g3E9r6rO", FiosOpGetError);          // sceFiosOpGetError
	LIB_FUNC("oP8g4A0c7tC", FiosOpGetActualCount);    // sceFiosOpGetActualCount
	LIB_FUNC("oP9d5E1l8eT", FiosOpDelete);            // sceFiosOpDelete
	LIB_FUNC("oV0a6D2d9iD", FiosOverlayAdd);          // sceFiosOverlayAdd
	LIB_FUNC("oV1r7E3m0oV", FiosOverlayRemove);       // sceFiosOverlayRemove
	LIB_FUNC("oV2r8S4o1lV", FiosOverlayResolveSync);  // sceFiosOverlayResolveSync
}

} // namespace Libs::LibFios2
