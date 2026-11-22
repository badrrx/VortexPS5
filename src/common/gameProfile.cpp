#include "common/gameProfile.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Common {

namespace {

int HexNibble(char c) noexcept {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

bool ParseHexUint64(std::string_view text, uint64_t& out) noexcept {
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
		text.remove_prefix(1);
	}
	if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		text.remove_prefix(2);
	}
	if (text.empty()) {
		return false;
	}
	uint64_t value = 0;
	auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
	if (ec != std::errc {} || end != text.data() + text.size()) {
		return false;
	}
	out = value;
	return true;
}

const char* UpscalerModeToString(Config::PresentUpscalerMode mode) noexcept {
	switch (mode) {
		case Config::PresentUpscalerMode::CAS:  return "CAS";
		case Config::PresentUpscalerMode::FSR1: return "FSR1";
		default:                                return "Off";
	}
}

Config::PresentUpscalerMode UpscalerModeFromString(std::string_view s) noexcept {
	if (s == "CAS" || s == "cas") return Config::PresentUpscalerMode::CAS;
	if (s == "FSR1" || s == "fsr1" || s == "FSR") return Config::PresentUpscalerMode::FSR1;
	return Config::PresentUpscalerMode::Off;
}

} // namespace

bool ParseHexBytes(std::string_view hex, std::vector<uint8_t>& out) {
	out.clear();
	std::string compact;
	compact.reserve(hex.size());
	for (char c: hex) {
		if (!std::isspace(static_cast<unsigned char>(c))) {
			compact.push_back(c);
		}
	}
	if (compact.empty() || (compact.size() % 2) != 0) {
		return false;
	}
	out.resize(compact.size() / 2);
	for (size_t i = 0; i < out.size(); ++i) {
		const int hi = HexNibble(compact[i * 2]);
		const int lo = HexNibble(compact[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			out.clear();
			return false;
		}
		out[i] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return true;
}

std::filesystem::path PatchBundlePath(std::string_view             title_id,
                                      const std::filesystem::path& patches_dir) {
	return patches_dir / (std::string(title_id) + ".json");
}

bool LoadPatchBundle(const std::filesystem::path& path, GamePatchBundle& out, std::string* error) {
	out = {};
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs.is_open()) {
		if (error != nullptr) *error = "Could not open patch file";
		return false;
	}
	const auto root = nlohmann::json::parse(ifs, nullptr, false);
	if (root.is_discarded() || !root.is_object()) {
		if (error != nullptr) *error = "Invalid JSON in patch file";
		return false;
	}
	if (auto it = root.find("titleId"); it != root.end() && it->is_string()) {
		out.title_id = it->get<std::string>();
	} else if (auto it2 = root.find("id"); it2 != root.end() && it2->is_string()) {
		out.title_id = it2->get<std::string>();
	}
	if (auto it = root.find("version"); it != root.end() && it->is_string()) {
		out.version = it->get<std::string>();
	}

	const auto patches_it = root.find("patches");
	if (patches_it == root.end() || !patches_it->is_array()) {
		if (error != nullptr) *error = "Patch JSON missing 'patches' array";
		return false;
	}

	for (const auto& p: *patches_it) {
		if (!p.is_object()) continue;
		GamePatchEntry entry {};
		entry.id                 = p.value("id", std::string {});
		entry.name               = p.value("name", entry.id);
		entry.category           = p.value("category", std::string("General"));
		entry.enabled_by_default = p.value("enabled", false);
		entry.fps_cap            = p.value("fpsCap", 0u);
		if (entry.id.empty()) {
			entry.id = entry.name;
		}

		if (auto writes_it = p.find("writes"); writes_it != p.end() && writes_it->is_array()) {
			for (const auto& w: *writes_it) {
				if (!w.is_object()) continue;
				MemoryPatchOp op {};
				if (auto off_it = w.find("offset"); off_it != w.end()) {
					if (off_it->is_string()) {
						if (!ParseHexUint64(off_it->get<std::string>(), op.offset)) {
							if (error != nullptr) *error = "Invalid hex offset in patch write";
							return false;
						}
					} else if (off_it->is_number_unsigned()) {
						op.offset = off_it->get<uint64_t>();
					} else {
						if (error != nullptr) *error = "Unsupported offset type in patch write";
						return false;
					}
				}
				const auto orig_str  = w.value("original", std::string {});
				const auto patch_str = w.value("patched", std::string {});
				if (!ParseHexBytes(orig_str, op.original) || !ParseHexBytes(patch_str, op.patched) ||
				    op.original.size() != op.patched.size()) {
					if (error != nullptr) *error = "Invalid or mismatched original/patched hex bytes";
					return false;
				}
				entry.writes.push_back(std::move(op));
			}
		}
		out.patches.push_back(std::move(entry));
	}
	return true;
}

bool ApplyPatchBundleToBuffer(const GamePatchBundle&           bundle,
                              std::span<const std::string>     enabled_ids,
                              std::span<uint8_t>               memory_region,
                              std::vector<AppliedPatchBackup>& out_backups,
                              uint32_t*                        out_fps_cap,
                              std::string*                     error) {
	out_backups.clear();
	const bool use_explicit_list = !enabled_ids.empty();

	for (const auto& patch: bundle.patches) {
		const bool active =
		    use_explicit_list
		        ? (std::find(enabled_ids.begin(), enabled_ids.end(), patch.id) != enabled_ids.end())
		        : patch.enabled_by_default;
		if (!active) {
			continue;
		}
		if (out_fps_cap != nullptr && patch.fps_cap > 0) {
			*out_fps_cap = patch.fps_cap;
		}
		for (const auto& op: patch.writes) {
			if (op.offset > memory_region.size() ||
			    op.patched.size() > memory_region.size() - op.offset) {
				RollbackPatchBackups(memory_region, out_backups);
				out_backups.clear();
				if (error != nullptr) *error = "Patch offset out of memory region bounds";
				return false;
			}
			uint8_t* dst = memory_region.data() + op.offset;
			if (!op.original.empty() &&
			    std::memcmp(dst, op.original.data(), op.original.size()) != 0) {
				RollbackPatchBackups(memory_region, out_backups);
				out_backups.clear();
				if (error != nullptr) *error = "Original bytes mismatch for patch " + patch.id;
				return false;
			}
			AppliedPatchBackup backup {};
			backup.offset = op.offset;
			backup.saved_original.assign(dst, dst + op.patched.size());
			std::memcpy(dst, op.patched.data(), op.patched.size());
			out_backups.push_back(std::move(backup));
		}
	}
	return true;
}

void RollbackPatchBackups(std::span<uint8_t>                     memory_region,
                          const std::vector<AppliedPatchBackup>& backups) {
	for (auto it = backups.rbegin(); it != backups.rend(); ++it) {
		if (it->offset <= memory_region.size() &&
		    it->saved_original.size() <= memory_region.size() - it->offset) {
			std::memcpy(memory_region.data() + it->offset, it->saved_original.data(),
			            it->saved_original.size());
		}
	}
}

std::filesystem::path GameProfilePath(std::string_view             title_id,
                                      const std::filesystem::path& profiles_dir) {
	return profiles_dir / (std::string(title_id) + ".json");
}

GameProfile LoadGameProfile(std::string_view             title_id,
                            const std::filesystem::path& profiles_dir) {
	GameProfile profile {};
	profile.title_id = std::string(title_id);
	if (title_id.empty()) {
		return profile;
	}
	std::ifstream ifs(GameProfilePath(title_id, profiles_dir), std::ios::binary);
	if (!ifs.is_open()) {
		return profile;
	}
	const auto j = nlohmann::json::parse(ifs, nullptr, false);
	if (j.is_discarded() || !j.is_object()) {
		return profile;
	}
	profile.resolution_scale =
	    std::clamp(j.value("resolutionScale", 1.0f), 1.0f, 2.0f);
	profile.upscaler_mode =
	    UpscalerModeFromString(j.value("upscalerMode", std::string("Off")));
	profile.upscaler_sharpness =
	    std::clamp(j.value("upscalerSharpness", 0.6f), 0.0f, 1.0f);
	profile.fps_cap         = j.value("fpsCap", 0u);
	profile.guest_memory_mb = j.value("guestMemoryMb", 0u);
	if (auto it = j.find("enabledPatches"); it != j.end() && it->is_array()) {
		for (const auto& pid: *it) {
			if (pid.is_string()) {
				profile.enabled_patches.push_back(pid.get<std::string>());
			}
		}
	}
	return profile;
}

bool SaveGameProfile(const GameProfile& profile, const std::filesystem::path& profiles_dir) {
	if (profile.title_id.empty()) {
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(profiles_dir, ec);
	const auto path = GameProfilePath(profile.title_id, profiles_dir);

	nlohmann::json j;
	j["titleId"]           = profile.title_id;
	j["resolutionScale"]   = std::clamp(profile.resolution_scale, 1.0f, 2.0f);
	j["upscalerMode"]      = UpscalerModeToString(profile.upscaler_mode);
	j["upscalerSharpness"] = std::clamp(profile.upscaler_sharpness, 0.0f, 1.0f);
	j["fpsCap"]            = profile.fps_cap;
	j["guestMemoryMb"]     = profile.guest_memory_mb;
	j["enabledPatches"]    = profile.enabled_patches;

	std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
	if (!ofs.is_open()) {
		return false;
	}
	ofs << j.dump(2) << "\n";
	return ofs.good();
}

} // namespace Common
