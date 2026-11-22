#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_HOMEDASHBOARD_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_HOMEDASHBOARD_H_

#include "common/emulatorConfig.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Libs::Graphics {

struct DashboardGameEntry {
	std::string           title_name;
	std::string           title_id;
	std::string           content_version;
	std::filesystem::path game_dir;
	std::filesystem::path eboot_path;
	std::filesystem::path icon_path;
	std::filesystem::path pic_path;
	uint32_t              trophy_unlocked = 0;
	uint32_t              trophy_total    = 0;
	uint32_t              trophy_percent  = 0;
	bool                  is_placeholder  = false;
};

struct DashboardLaunchResult {
	bool                        launch_requested   = false;
	std::filesystem::path       app0_dir;
	std::filesystem::path       elf_path;
	uint32_t                    guest_memory_mb    = 0;
	bool                        shader_cache       = true;
	bool                        perf_overlay       = false;
	Config::PresentUpscalerMode present_upscaler   = Config::PresentUpscalerMode::Off;
	float                       upscaler_sharpness = 0.6f;
	float                       resolution_scale   = 1.0f;
	uint32_t                    fps_cap            = 0;
	std::vector<std::string>    enabled_patches;
};

// Scans `games_root` (e.g. "./games") for PS5 titles containing eboot.bin (or .elf) + optional
// sce_sys/param.json, icon0.png, and pic0.png. Returns a placeholder card if none are found.
[[nodiscard]] std::vector<DashboardGameEntry> ScanPs5GameLibrary(
    const std::filesystem::path& games_root);

// Launches the native SDL3 + ImGui PS5-style Boot Intro + Home Dashboard.
// When `demo_max_frames > 0`, runs for that many rendered frames and exits cleanly for testing.
[[nodiscard]] DashboardLaunchResult RunHomeDashboard(
    const std::filesystem::path& default_games_dir = "games",
    uint32_t                     demo_max_frames   = 0);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_HOMEDASHBOARD_H_
