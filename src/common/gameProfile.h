#ifndef KYTY_COMMON_GAME_PROFILE_H_
#define KYTY_COMMON_GAME_PROFILE_H_

#include "common/emulatorConfig.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

struct MemoryPatchOp {
	uint64_t             offset = 0;
	std::vector<uint8_t> original;
	std::vector<uint8_t> patched;
};

struct GamePatchEntry {
	std::string                id;
	std::string                name;
	std::string                category; // e.g. "FPS", "Visual", "QoL"
	bool                       enabled_by_default = false;
	uint32_t                   fps_cap            = 0; // Optional FPS/vblank override when enabled
	std::vector<MemoryPatchOp> writes;
};

struct GamePatchBundle {
	std::string                 title_id;
	std::string                 version;
	std::vector<GamePatchEntry> patches;
};

struct AppliedPatchBackup {
	uint64_t             offset = 0;
	std::vector<uint8_t> saved_original;
};

struct GameProfile {
	std::string                 title_id;
	float                       resolution_scale   = 1.0f; // 1.0, 1.5, 2.0
	Config::PresentUpscalerMode upscaler_mode      = Config::PresentUpscalerMode::Off;
	float                       upscaler_sharpness = 0.6f; // 0.0 - 1.0
	uint32_t                    fps_cap            = 0;    // 0 = native/auto, 60, 120
	uint32_t                    guest_memory_mb    = 0;    // 0 = auto host-aware
	std::vector<std::string>    enabled_patches;
};

// Parses a hex string like "90 90 EB 05" or "9090EB05" into bytes.
[[nodiscard]] bool ParseHexBytes(std::string_view hex, std::vector<uint8_t>& out);

// Loads patches/<TitleID>.json (or any explicit patch JSON path).
[[nodiscard]] bool LoadPatchBundle(const std::filesystem::path& path, GamePatchBundle& out,
                                   std::string* error = nullptr);

// Convenience: looks up <patches_dir>/<title_id>.json.
[[nodiscard]] std::filesystem::path PatchBundlePath(std::string_view             title_id,
                                                    const std::filesystem::path& patches_dir = "patches");

// Applies all enabled patches in `bundle` (selected by `enabled_ids`, or `enabled_by_default` if
// `enabled_ids` is empty) to `memory_region`. Verifies `original` bytes before writing and records
// rollback backups in `out_backups`. On any mismatch or out-of-bounds write, rolls back all writes
// atomically and returns false.
[[nodiscard]] bool ApplyPatchBundleToBuffer(const GamePatchBundle&           bundle,
                                            std::span<const std::string>     enabled_ids,
                                            std::span<uint8_t>               memory_region,
                                            std::vector<AppliedPatchBackup>& out_backups,
                                            uint32_t*                        out_fps_cap = nullptr,
                                            std::string*                     error       = nullptr);

// Restores all bytes recorded in `backups` in reverse order.
void RollbackPatchBackups(std::span<uint8_t>                     memory_region,
                          const std::vector<AppliedPatchBackup>& backups);

// Loads/saves per-game profile JSON at <profiles_dir>/<title_id>.json.
[[nodiscard]] std::filesystem::path GameProfilePath(
    std::string_view title_id, const std::filesystem::path& profiles_dir = "profiles");
[[nodiscard]] GameProfile LoadGameProfile(std::string_view             title_id,
                                          const std::filesystem::path& profiles_dir = "profiles");
[[nodiscard]] bool SaveGameProfile(const GameProfile&           profile,
                                   const std::filesystem::path& profiles_dir = "profiles");

} // namespace Common

#endif // KYTY_COMMON_GAME_PROFILE_H_
