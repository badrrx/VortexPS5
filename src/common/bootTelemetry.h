#ifndef KYTY_COMMON_BOOT_TELEMETRY_H_
#define KYTY_COMMON_BOOT_TELEMETRY_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace Common {

struct BootTelemetrySnapshot {
	std::string title_id;
	std::string elf_path;
	bool        elf_loaded            = false;
	bool        first_gpu_submit      = false;
	bool        first_frame_presented = false;
	uint64_t    elf_loaded_ms         = 0;
	uint64_t    first_gpu_submit_ms   = 0;
	uint64_t    first_frame_ms        = 0;
	uint64_t    shaders_compiled      = 0;
	uint64_t    shader_compile_us     = 0;
	uint64_t    pipelines_created     = 0;
	uint64_t    pipeline_cache_hits   = 0;
	uint64_t    missing_unique_nids   = 0;
	uint64_t    missing_nid_calls     = 0;
};

class BootTelemetry {
public:
	static BootTelemetry& Global();

	void BeginSession(std::string_view title_id, std::string_view elf_path = {});
	void MarkElfLoaded();
	void MarkFirstGpuSubmit();
	void MarkFirstFramePresented();
	void RecordShaderCompiled(uint64_t compile_us);
	void RecordPipelineCreated(bool cache_hit);
	void Reset();

	[[nodiscard]] bool                  HasActiveSession() const;
	[[nodiscard]] BootTelemetrySnapshot Snapshot() const;
	[[nodiscard]] bool ExportJson(const std::filesystem::path& out_path = {}) const;
	bool               FlushIfActive(const std::filesystem::path& out_path = {}) const;

private:
	uint64_t ElapsedMsLocked() const;

	mutable std::mutex                    m_mutex;
	std::atomic<bool>                     m_session_active {false};
	std::atomic<bool>                     m_elf_loaded_fast {false};
	std::atomic<bool>                     m_first_gpu_submit_fast {false};
	std::atomic<bool>                     m_first_frame_fast {false};
	std::chrono::steady_clock::time_point m_boot_start = std::chrono::steady_clock::now();
	BootTelemetrySnapshot                 m_data;
};

} // namespace Common

#endif // KYTY_COMMON_BOOT_TELEMETRY_H_
