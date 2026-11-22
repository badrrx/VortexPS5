#include "common/nidTriage.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Common {

void ParseSonyImportSymbol(std::string_view raw_symbol, std::string& out_nid,
                           std::string& out_library) {
	const auto first_hash = raw_symbol.find('#');
	if (first_hash == std::string_view::npos) {
		out_nid     = std::string(raw_symbol);
		out_library = "unknown";
		return;
	}
	out_nid = std::string(raw_symbol.substr(0, first_hash));
	const auto rest        = raw_symbol.substr(first_hash + 1);
	const auto second_hash = rest.find('#');
	if (second_hash == std::string_view::npos) {
		out_library = rest.empty() ? "unknown" : std::string(rest);
	} else {
		const auto lib = rest.substr(0, second_hash);
		out_library    = lib.empty() ? "unknown" : std::string(lib);
	}
}

NidTriageTracker& NidTriageTracker::Global() {
	static NidTriageTracker instance;
	return instance;
}

void NidTriageTracker::RecordImport(std::string_view raw_symbol, std::string_view program_name) {
	std::string nid;
	std::string library;
	ParseSonyImportSymbol(raw_symbol, nid, library);

	std::scoped_lock lock(m_mutex);
	Key              key {library, nid};
	auto&            entry = m_entries[key];
	if (entry.nid.empty()) {
		entry.nid     = std::move(nid);
		entry.library = std::move(library);
		entry.symbol  = std::string(raw_symbol);
		entry.program = std::string(program_name);
	}
}

void NidTriageTracker::RecordCall(std::string_view raw_symbol, std::string_view program_name) {
	std::string nid;
	std::string library;
	ParseSonyImportSymbol(raw_symbol, nid, library);

	std::scoped_lock lock(m_mutex);
	Key              key {library, nid};
	auto&            entry = m_entries[key];
	if (entry.nid.empty()) {
		entry.nid     = std::move(nid);
		entry.library = std::move(library);
		entry.symbol  = std::string(raw_symbol);
		entry.program = std::string(program_name);
	}
	entry.call_count++;
}

NidTriageReport NidTriageTracker::BuildReport() const {
	std::scoped_lock lock(m_mutex);
	NidTriageReport  report {};

	std::map<std::string, MissingLibraryGroup> grouped;
	for (const auto& [key, entry]: m_entries) {
		auto& group = grouped[entry.library];
		if (group.library.empty()) {
			group.library = entry.library;
		}
		group.total_calls += entry.call_count;
		group.nids.push_back(entry);
		report.total_unique_nids++;
		report.total_calls += entry.call_count;
	}

	report.libraries.reserve(grouped.size());
	for (auto& [name, group]: grouped) {
		std::sort(group.nids.begin(), group.nids.end(),
		          [](const MissingNidEntry& a, const MissingNidEntry& b) {
			          if (a.call_count != b.call_count) return a.call_count > b.call_count;
			          return a.nid < b.nid;
		          });
		report.libraries.push_back(std::move(group));
	}

	std::sort(report.libraries.begin(), report.libraries.end(),
	          [](const MissingLibraryGroup& a, const MissingLibraryGroup& b) {
		          if (a.total_calls != b.total_calls) return a.total_calls > b.total_calls;
		          if (a.nids.size() != b.nids.size()) return a.nids.size() > b.nids.size();
		          return a.library < b.library;
	          });

	return report;
}

std::string NidTriageTracker::FormatConsoleSummary(size_t max_libs,
                                                   size_t max_nids_per_lib) const {
	const auto report = BuildReport();
	if (report.total_unique_nids == 0) {
		return "=== Unresolved-NID Triage: 0 missing symbols ===\n";
	}

	std::string out;
	char        buf[256] {};
	std::snprintf(buf, sizeof(buf),
	              "=== Unresolved-NID Triage: %llu unique missing NIDs across %zu libraries (%llu calls) ===\n",
	              static_cast<unsigned long long>(report.total_unique_nids),
	              report.libraries.size(),
	              static_cast<unsigned long long>(report.total_calls));
	out += buf;

	const size_t lib_limit = std::min(report.libraries.size(), max_libs);
	for (size_t i = 0; i < lib_limit; ++i) {
		const auto& lib = report.libraries[i];
		std::snprintf(buf, sizeof(buf), "  [%s] unique=%zu calls=%llu\n", lib.library.c_str(),
		              lib.nids.size(), static_cast<unsigned long long>(lib.total_calls));
		out += buf;

		const size_t nid_limit = std::min(lib.nids.size(), max_nids_per_lib);
		for (size_t j = 0; j < nid_limit; ++j) {
			const auto& n = lib.nids[j];
			std::snprintf(buf, sizeof(buf), "    - %-16s calls=%-8llu (%s)\n", n.nid.c_str(),
			              static_cast<unsigned long long>(n.call_count), n.symbol.c_str());
			out += buf;
		}
	}
	return out;
}

bool NidTriageTracker::ExportJsonReport(const std::filesystem::path& path) const {
	const auto report = BuildReport();
	if (path.has_parent_path()) {
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
	}

	nlohmann::json root;
	root["totalUniqueMissingNids"] = report.total_unique_nids;
	root["totalUnresolvedCalls"]   = report.total_calls;

	auto libs_json = nlohmann::json::array();
	for (const auto& lib: report.libraries) {
		nlohmann::json lib_obj;
		lib_obj["library"]    = lib.library;
		lib_obj["uniqueNids"] = lib.nids.size();
		lib_obj["totalCalls"] = lib.total_calls;
		auto nids_json        = nlohmann::json::array();
		for (const auto& n: lib.nids) {
			nlohmann::json nid_obj;
			nid_obj["nid"]     = n.nid;
			nid_obj["symbol"]  = n.symbol;
			nid_obj["calls"]   = n.call_count;
			nid_obj["program"] = n.program;
			nids_json.push_back(std::move(nid_obj));
		}
		lib_obj["nids"] = std::move(nids_json);
		libs_json.push_back(std::move(lib_obj));
	}
	root["libraries"] = std::move(libs_json);

	std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
	if (!ofs.is_open()) {
		return false;
	}
	ofs << root.dump(2) << "\n";
	return ofs.good();
}

void NidTriageTracker::Clear() {
	std::scoped_lock lock(m_mutex);
	m_entries.clear();
}

} // namespace Common
