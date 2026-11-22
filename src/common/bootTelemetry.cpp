#include "common/bootTelemetry.h"

#include "common/nidTriage.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace Common {

namespace {

std::string EscapeJson(std::string_view input) {
	std::string out;
	out.reserve(input.size() + 8);
	for (const char c: input) {
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: out.push_back(c); break;
		}
	}
	return out;
}

} // namespace

BootTelemetry& BootTelemetry::Global() {
	static BootTelemetry instance;
	return instance;
}

uint64_t BootTelemetry::ElapsedMsLocked() const {
	const auto now = std::chrono::steady_clock::now();
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(now - m_boot_start).count());
}

void BootTelemetry::BeginSession(std::string_view title_id, std::string_view elf_path) {
	std::scoped_lock lock(m_mutex);
	m_boot_start     = std::chrono::steady_clock::now();
	m_data           = BootTelemetrySnapshot {};
	m_data.title_id  = title_id.empty() ? "UNKNOWN" : std::string(title_id);
	m_data.elf_path  = std::string(elf_path);
	m_elf_loaded_fast.store(false, std::memory_order_relaxed);
	m_first_gpu_submit_fast.store(false, std::memory_order_relaxed);
	m_first_frame_fast.store(false, std::memory_order_relaxed);
	m_session_active.store(true, std::memory_order_release);
}

void BootTelemetry::MarkElfLoaded() {
	if (m_elf_loaded_fast.load(std::memory_order_relaxed)) {
		return;
	}
	bool changed = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_data.elf_loaded) {
			m_data.elf_loaded    = true;
			m_data.elf_loaded_ms = ElapsedMsLocked();
			m_elf_loaded_fast.store(true, std::memory_order_release);
			changed = true;
		}
	}
	if (changed) {
		(void)FlushIfActive();
	}
}

void BootTelemetry::MarkFirstGpuSubmit() {
	if (m_first_gpu_submit_fast.load(std::memory_order_relaxed)) {
		return;
	}
	bool changed = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_data.first_gpu_submit) {
			m_data.first_gpu_submit    = true;
			m_data.first_gpu_submit_ms = ElapsedMsLocked();
			m_first_gpu_submit_fast.store(true, std::memory_order_release);
			changed = true;
		}
	}
	if (changed) {
		(void)FlushIfActive();
	}
}

void BootTelemetry::MarkFirstFramePresented() {
	if (m_first_frame_fast.load(std::memory_order_relaxed)) {
		return;
	}
	bool changed = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_data.first_frame_presented) {
			m_data.first_frame_presented = true;
			m_data.first_frame_ms        = ElapsedMsLocked();
			m_first_frame_fast.store(true, std::memory_order_release);
			changed = true;
		}
	}
	if (changed) {
		(void)FlushIfActive();
	}
}

void BootTelemetry::RecordShaderCompiled(uint64_t compile_us) {
	std::scoped_lock lock(m_mutex);
	++m_data.shaders_compiled;
	m_data.shader_compile_us += compile_us;
}

void BootTelemetry::RecordPipelineCreated(bool cache_hit) {
	std::scoped_lock lock(m_mutex);
	++m_data.pipelines_created;
	if (cache_hit) {
		++m_data.pipeline_cache_hits;
	}
}

void BootTelemetry::Reset() {
	std::scoped_lock lock(m_mutex);
	m_boot_start = std::chrono::steady_clock::now();
	m_data       = BootTelemetrySnapshot {};
	m_elf_loaded_fast.store(false, std::memory_order_relaxed);
	m_first_gpu_submit_fast.store(false, std::memory_order_relaxed);
	m_first_frame_fast.store(false, std::memory_order_relaxed);
	m_session_active.store(false, std::memory_order_release);
}

bool BootTelemetry::HasActiveSession() const {
	return m_session_active.load(std::memory_order_acquire);
}

bool BootTelemetry::FlushIfActive(const std::filesystem::path& out_path) const {
	if (!HasActiveSession()) {
		return false;
	}
	const bool ok = ExportJson(out_path);
	if (NidTriageTracker::Global().BuildReport().total_unique_nids > 0) {
		(void)NidTriageTracker::Global().ExportJsonReport("missing_nids_report.json");
	}
	return ok;
}

BootTelemetrySnapshot BootTelemetry::Snapshot() const {
	std::scoped_lock lock(m_mutex);
	auto             snap       = m_data;
	const auto       nid_report = NidTriageTracker::Global().BuildReport();
	snap.missing_unique_nids    = nid_report.total_unique_nids;
	snap.missing_nid_calls      = nid_report.total_calls;
	return snap;
}

bool BootTelemetry::ExportJson(const std::filesystem::path& out_path) const {
	const auto snap = Snapshot();
	const auto target =
	    out_path.empty() ? std::filesystem::path("boot_telemetry_" + snap.title_id + ".json")
	                     : out_path;

	std::error_code ec;
	if (target.has_parent_path()) {
		std::filesystem::create_directories(target.parent_path(), ec);
	}

	const auto tmp_path = target.string() + ".tmp";
	{
		std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
		if (!ofs.is_open()) {
			return false;
		}
		ofs << "{\n"
		    << "  \"title_id\": \"" << EscapeJson(snap.title_id) << "\",\n"
		    << "  \"elf_path\": \"" << EscapeJson(snap.elf_path) << "\",\n"
		    << "  \"milestones\": {\n"
		    << "    \"elf_loaded\": " << (snap.elf_loaded ? "true" : "false") << ",\n"
		    << "    \"elf_loaded_ms\": " << snap.elf_loaded_ms << ",\n"
		    << "    \"first_gpu_submit\": " << (snap.first_gpu_submit ? "true" : "false") << ",\n"
		    << "    \"first_gpu_submit_ms\": " << snap.first_gpu_submit_ms << ",\n"
		    << "    \"first_frame_presented\": " << (snap.first_frame_presented ? "true" : "false")
		    << ",\n"
		    << "    \"first_frame_ms\": " << snap.first_frame_ms << "\n"
		    << "  },\n"
		    << "  \"gpu_stats\": {\n"
		    << "    \"shaders_compiled\": " << snap.shaders_compiled << ",\n"
		    << "    \"shader_compile_us\": " << snap.shader_compile_us << ",\n"
		    << "    \"pipelines_created\": " << snap.pipelines_created << ",\n"
		    << "    \"pipeline_cache_hits\": " << snap.pipeline_cache_hits << "\n"
		    << "  },\n"
		    << "  \"missing_nids\": {\n"
		    << "    \"unique_count\": " << snap.missing_unique_nids << ",\n"
		    << "    \"total_calls\": " << snap.missing_nid_calls << "\n"
		    << "  }\n"
		    << "}\n";
		if (!ofs.good()) {
			return false;
		}
	}

	std::filesystem::rename(tmp_path, target, ec);
	if (ec) {
		std::filesystem::remove(target, ec);
		ec.clear();
		std::filesystem::rename(tmp_path, target, ec);
	}
	return !ec;
}

} // namespace Common
