#ifndef KYTY_COMMON_NID_TRIAGE_H_
#define KYTY_COMMON_NID_TRIAGE_H_

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

struct MissingNidEntry {
	std::string nid;
	std::string symbol;
	std::string library;
	std::string program;
	uint64_t    call_count = 0;
};

struct MissingLibraryGroup {
	std::string                  library;
	uint64_t                     total_calls = 0;
	std::vector<MissingNidEntry> nids;
};

struct NidTriageReport {
	uint64_t                         total_unique_nids = 0;
	uint64_t                         total_calls       = 0;
	std::vector<MissingLibraryGroup> libraries;
};

// Parses Sony dynamic symbol format "<NID>#<Library>#<Module>" (or plain symbol name).
void ParseSonyImportSymbol(std::string_view raw_symbol, std::string& out_nid,
                           std::string& out_library);

// Thread-safe unresolved NID triage aggregator.
class NidTriageTracker {
public:
	static NidTriageTracker& Global();

	void RecordImport(std::string_view raw_symbol, std::string_view program_name = {});
	void RecordCall(std::string_view raw_symbol, std::string_view program_name = {});

	[[nodiscard]] NidTriageReport BuildReport() const;
	[[nodiscard]] std::string     FormatConsoleSummary(size_t max_libs = 16,
	                                                   size_t max_nids_per_lib = 8) const;
	[[nodiscard]] bool            ExportJsonReport(
	               const std::filesystem::path& path = "missing_nids_report.json") const;
	void Clear();

private:
	struct Key {
		std::string library;
		std::string nid;

		bool operator<(const Key& other) const {
			if (library != other.library) return library < other.library;
			return nid < other.nid;
		}
	};

	mutable std::mutex             m_mutex;
	std::map<Key, MissingNidEntry> m_entries;
};

} // namespace Common

#endif // KYTY_COMMON_NID_TRIAGE_H_
