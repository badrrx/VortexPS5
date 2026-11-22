#include "loader/symbolDatabase.h"

#include "common/file.h"

#include <fmt/format.h>
#include <magic_enum.hpp>
#include <mutex>

namespace Loader {

constexpr char LIB_PREFIX[] = "libSce";

static std::string UpdateName(const std::string& str) {
	return str.starts_with(LIB_PREFIX) ? Common::RemoveFirst(str, 6) : str;
}

std::string SymbolDatabase::GenerateName(const SymbolResolve& s) {
	auto library = UpdateName(s.library);
	auto module  = UpdateName(s.module);
	return fmt::format("{}[{}_v{}][{}_v{}.{}][{}]", s.name.c_str(), library.c_str(),
	                   s.library_version, module.c_str(), s.module_version_major,
	                   s.module_version_minor, magic_enum::enum_name(s.type));
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr) {
	SymbolRecord r {};
	r.name  = GenerateName(s);
	r.vaddr = vaddr;
	std::unique_lock lock(m_mutex);
	m_map.insert_or_assign(r.name, m_symbols.size());
	m_symbols.push_back(std::move(r));
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr, const std::string& dbg_name) {
	SymbolRecord r {};
	r.name     = GenerateName(s);
	r.vaddr    = vaddr;
	r.dbg_name = dbg_name;
	std::unique_lock lock(m_mutex);
	m_map.insert_or_assign(r.name, m_symbols.size());
	m_symbols.push_back(std::move(r));
}

void SymbolDatabase::DbgDump(const std::string& folder, const std::string& file_name) {
	auto folder_str = Common::FixDirectorySlash(folder);

	Common::File::CreateDirectories(folder_str);

	Common::File f;
	f.Create(folder_str + file_name);

	std::shared_lock lock(m_mutex);
	for (const auto& sym: m_symbols) {
		f.Printf("%" PRIx64 " %s\n", sym.vaddr, sym.name.c_str());
	}

	f.Close();
}

const SymbolRecord* SymbolDatabase::Find(const SymbolResolve& s) const {
	const auto key = GenerateName(s);
	std::shared_lock lock(m_mutex);
	auto it = m_map.find(key);
	if (it == m_map.end() && s.module_version_major == 0) {
		// Allow PS5 SYS_V0 (*_v0.1) imports to fall back cleanly to *_v1.1 HLE implementations.
		SymbolResolve v1        = s;
		v1.module_version_major = 1;
		it                      = m_map.find(GenerateName(v1));
	}
	if (it == m_map.end()) {
		return nullptr;
	}
	auto index = it->second;
	if (index >= m_symbols.size()) {
		return nullptr;
	}
	return &m_symbols[index];
}

const SymbolRecord* SymbolDatabase::FindByNid(const std::string& nid, SymbolType type,
                                                uint64_t vaddr) const {
	auto prefix = nid + "[";
	auto suffix = fmt::format("[{}]", magic_enum::enum_name(type));

	std::shared_lock lock(m_mutex);
	for (const auto& symbol: m_symbols) {
		if ((vaddr == 0 || symbol.vaddr == vaddr) && symbol.name.starts_with(prefix) &&
		    symbol.name.ends_with(suffix)) {
			return &symbol;
		}
	}

	return nullptr;
}

const SymbolRecord* SymbolDatabase::FindByName(const std::string& name, SymbolType type) const {
	auto prefix = name + "[";
	auto suffix = fmt::format("[{}]", magic_enum::enum_name(type));

	std::shared_lock lock(m_mutex);
	for (const auto& symbol: m_symbols) {
		if (symbol.name.starts_with(prefix) && symbol.name.ends_with(suffix)) {
			return &symbol;
		}
	}

	return nullptr;
}

} // namespace Loader
