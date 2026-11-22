#include "graphics/presentation/homeDashboard.h"

#include "common/gameProfile.h"
#include "common/trophyTracker.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "kernel/memory.h"
#include "stb_image.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Libs::Graphics {

namespace {

struct Particle {
	float x;
	float y;
	float vx;
	float vy;
	float radius;
	float alpha;
	bool  gold;
};

struct LoadedTileMedia {
	SDL_Texture* icon_tex = nullptr;
	SDL_Texture* pic_tex  = nullptr;
	float        scale    = 1.0f;
};

void ParseParamJson(const std::filesystem::path& param_path, DashboardGameEntry& entry) {
	std::ifstream ifs(param_path, std::ios::binary);
	if (!ifs.is_open()) {
		return;
	}
	const auto j = nlohmann::json::parse(ifs, nullptr, false);
	if (j.is_discarded() || !j.is_object()) {
		return;
	}
	if (const auto it = j.find("titleId"); it != j.end() && it->is_string()) {
		entry.title_id = it->get<std::string>();
	}
	if (const auto it = j.find("contentVersion"); it != j.end() && it->is_string()) {
		entry.content_version = it->get<std::string>();
	}
	if (const auto it = j.find("titleName"); it != j.end() && it->is_string()) {
		entry.title_name = it->get<std::string>();
	}
	if (const auto loc = j.find("localizedParameters"); loc != j.end() && loc->is_object()) {
		std::string lang = "en-US";
		if (const auto dl = loc->find("defaultLanguage"); dl != loc->end() && dl->is_string()) {
			lang = dl->get<std::string>();
		}
		if (const auto lp = loc->find(lang); lp != loc->end() && lp->is_object()) {
			if (const auto tn = lp->find("titleName"); tn != lp->end() && tn->is_string()) {
				entry.title_name = tn->get<std::string>();
			}
		}
	}
}

SDL_Texture* LoadPngTexture(SDL_Renderer* renderer, const std::filesystem::path& path) {
	if (renderer == nullptr || path.empty()) {
		return nullptr;
	}
	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec)) {
		return nullptr;
	}
	int            w = 0, h = 0, comp = 0;
	const auto     u8path = path.u8string();
	const std::string spath(reinterpret_cast<const char*>(u8path.data()), u8path.size());
	unsigned char* pixels = stbi_load(spath.c_str(), &w, &h, &comp, 4);
	if (pixels == nullptr || w <= 0 || h <= 0) {
		if (pixels != nullptr) {
			stbi_image_free(pixels);
		}
		return nullptr;
	}
	SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
	                                     SDL_TEXTUREACCESS_STATIC, w, h);
	if (tex != nullptr) {
		SDL_UpdateTexture(tex, nullptr, pixels, w * 4);
		SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	}
	stbi_image_free(pixels);
	return tex;
}

void FreeMedia(std::vector<LoadedTileMedia>& media) {
	for (auto& m: media) {
		if (m.icon_tex != nullptr) {
			SDL_DestroyTexture(m.icon_tex);
			m.icon_tex = nullptr;
		}
		if (m.pic_tex != nullptr) {
			SDL_DestroyTexture(m.pic_tex);
			m.pic_tex = nullptr;
		}
	}
	media.clear();
}

void LoadMediaForLibrary(SDL_Renderer* renderer, const std::vector<DashboardGameEntry>& games,
                         std::vector<LoadedTileMedia>& media) {
	FreeMedia(media);
	media.resize(games.size());
	for (size_t i = 0; i < games.size(); i++) {
		if (!games[i].is_placeholder) {
			media[i].icon_tex = LoadPngTexture(renderer, games[i].icon_path);
			media[i].pic_tex  = LoadPngTexture(renderer, games[i].pic_path);
		}
	}
}

std::string MakeInitials(const std::string& title) {
	std::string out;
	bool        take = true;
	for (char c: title) {
		if (c == ' ' || c == '-' || c == '_') {
			take = true;
		} else if (take && std::isalnum(static_cast<unsigned char>(c)) != 0) {
			out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
			take = false;
			if (out.size() >= 3) {
				break;
			}
		}
	}
	return out.empty() ? "PS5" : out;
}

void DrawAmbientBackground(ImDrawList* dl, float w, float h, float time_s,
                           std::vector<Particle>& particles, float dt, SDL_Texture* bg_tex,
                           float bg_alpha) {
	// Deep PS5 midnight-blue vertical gradient
	dl->AddRectFilledMultiColor({0.0f, 0.0f}, {w, h}, IM_COL32(6, 12, 32, 255),
	                            IM_COL32(10, 22, 56, 255), IM_COL32(4, 9, 24, 255),
	                            IM_COL32(8, 18, 44, 255));

	// Optional selected-game pic0.png backdrop with dark scrim
	if (bg_tex != nullptr && bg_alpha > 0.01f) {
		const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(bg_alpha * 110.0f));
		dl->AddImage(reinterpret_cast<ImTextureID>(bg_tex), {0.0f, 0.0f}, {w, h}, {0.0f, 0.0f},
		             {1.0f, 1.0f}, tint);
		dl->AddRectFilledMultiColor({0.0f, 0.0f}, {w, h}, IM_COL32(5, 10, 24, 150),
		                            IM_COL32(5, 10, 24, 120), IM_COL32(4, 8, 20, 235),
		                            IM_COL32(4, 8, 20, 235));
	}

	// Soft radial ambient glow at top-center
	const float cx = w * 0.5f + std::sin(time_s * 0.35f) * 60.0f;
	const float cy = h * 0.28f;
	for (int ring = 4; ring >= 1; ring--) {
		const float r = static_cast<float>(ring) * 140.0f;
		const int   a = 22 - ring * 4;
		dl->AddCircleFilled({cx, cy}, r, IM_COL32(38, 125, 255, a), 48);
	}

	// Floating PS5 golden/cyan dust particles
	for (auto& p: particles) {
		p.x += p.vx * dt;
		p.y += p.vy * dt;
		if (p.x < -10.0f) p.x = w + 10.0f;
		if (p.x > w + 10.0f) p.x = -10.0f;
		if (p.y < -10.0f) p.y = h + 10.0f;
		if (p.y > h + 10.0f) p.y = -10.0f;
		const float pulse = 0.65f + 0.35f * std::sin(time_s * 1.8f + p.x * 0.02f);
		const int   alpha = static_cast<int>(p.alpha * pulse * 255.0f);
		const ImU32 col   = p.gold ? IM_COL32(255, 214, 130, alpha)
		                           : IM_COL32(110, 195, 255, alpha);
		dl->AddCircleFilled({p.x, p.y}, p.radius, col, 12);
	}
}

std::string CurrentClockString() {
	const auto now = std::chrono::system_clock::now();
	const auto tt  = std::chrono::system_clock::to_time_t(now);
	std::tm    lt {};
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	localtime_s(&lt, &tt);
#else
	localtime_r(&tt, &lt);
#endif
	char buf[16] {};
	std::snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
	return std::string(buf);
}

} // namespace

std::vector<DashboardGameEntry> ScanPs5GameLibrary(const std::filesystem::path& games_root) {
	std::vector<DashboardGameEntry> entries;
	std::error_code                 ec;

	if (std::filesystem::is_directory(games_root, ec)) {
		for (const auto& dir_entry: std::filesystem::directory_iterator(games_root, ec)) {
			if (!dir_entry.is_directory(ec)) {
				continue;
			}
			const auto& dir   = dir_entry.path();
			auto        eboot = dir / "eboot.bin";
			if (!std::filesystem::is_regular_file(eboot, ec)) {
				// Fallback: check for a single .elf inside the game folder
				eboot.clear();
				for (const auto& f: std::filesystem::directory_iterator(dir, ec)) {
					if (f.is_regular_file(ec) && f.path().extension() == ".elf") {
						eboot = f.path();
						break;
					}
				}
			}
			if (eboot.empty()) {
				continue;
			}

			DashboardGameEntry item {};
			item.title_name      = dir.filename().string();
			item.title_id        = "PPSA00000";
			item.content_version = "01.000.000";
			item.game_dir        = dir;
			item.eboot_path      = eboot;
			item.icon_path       = dir / "sce_sys" / "icon0.png";
			item.pic_path        = dir / "sce_sys" / "pic0.png";

			const auto param_json = dir / "sce_sys" / "param.json";
			if (std::filesystem::is_regular_file(param_json, ec)) {
				ParseParamJson(param_json, item);
			}
			const auto trophy_store = Common::LoadTrophyStore(item.title_id);
			item.trophy_unlocked    = trophy_store.UnlockedCount();
			item.trophy_total       = std::max(trophy_store.total_trophies,
			                                   static_cast<uint32_t>(trophy_store.trophies.size()));
			item.trophy_percent     = trophy_store.CompletionPercentage();
			entries.push_back(std::move(item));
		}
	}

	std::sort(entries.begin(), entries.end(),
	          [](const DashboardGameEntry& a, const DashboardGameEntry& b) {
		          return a.title_name < b.title_name;
	          });

	if (entries.empty()) {
		DashboardGameEntry placeholder {};
		placeholder.title_name      = "Add PS5 Games to /games/";
		placeholder.title_id        = "EMPTY LIBRARY";
		placeholder.content_version = "Drop <Title>/eboot.bin + sce_sys/param.json";
		placeholder.game_dir        = games_root;
		placeholder.is_placeholder  = true;
		entries.push_back(std::move(placeholder));
	}
	return entries;
}

DashboardLaunchResult RunHomeDashboard(const std::filesystem::path& default_games_dir,
                                       uint32_t                     demo_max_frames) {
	DashboardLaunchResult result {};
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		::printf("HomeDashboard: SDL_Init failed: %s\n", SDL_GetError());
		return result;
	}

	SDL_Window*   window   = nullptr;
	SDL_Renderer* renderer = nullptr;
	if (!SDL_CreateWindowAndRenderer("KYTY-PROSPERO — PS5 Home Dashboard", 1280, 720,
	                                 SDL_WINDOW_RESIZABLE, &window, &renderer)) {
		::printf("HomeDashboard: SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
		SDL_Quit();
		return result;
	}
	SDL_SetRenderVSync(renderer, 1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer3_Init(renderer);

	// Open first connected gamepad if present
	SDL_Gamepad* gamepad = nullptr;
	int          gp_count = 0;
	if (SDL_JoystickID* ids = SDL_GetGamepads(&gp_count); ids != nullptr) {
		if (gp_count > 0) {
			gamepad = SDL_OpenGamepad(ids[0]);
		}
		SDL_free(ids);
	}

	std::filesystem::path games_dir = default_games_dir;
	auto                  games     = ScanPs5GameLibrary(games_dir);
	std::vector<LoadedTileMedia> media;
	LoadMediaForLibrary(renderer, games, media);

	char games_dir_buf[512] {};
	{
		const auto u8s = games_dir.u8string();
		const size_t n = std::min(u8s.size(), sizeof(games_dir_buf) - 1);
		std::memcpy(games_dir_buf, u8s.data(), n);
	}

	// Deterministic ambient particle field
	std::vector<Particle> particles(72);
	uint32_t              seed = 0x50533530u;
	auto                  next_f = [&]() -> float {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFFu) / 65535.0f;
	};
	for (auto& p: particles) {
		p.x      = next_f() * 1280.0f;
		p.y      = next_f() * 720.0f;
		p.vx     = (next_f() - 0.5f) * 18.0f;
		p.vy     = -6.0f - next_f() * 14.0f;
		p.radius = 1.4f + next_f() * 2.4f;
		p.alpha  = 0.18f + next_f() * 0.45f;
		p.gold   = next_f() > 0.45f;
	}

	constexpr float kBootDurationS    = 2.0f;
	bool            in_boot_intro     = true;
	bool            show_settings     = false;
	bool            show_game_profile = false;
	int             selected_idx      = 0;
	int             mem_profile       = 0; // 0=Auto, 1=8192, 2=10240, 3=12800, 4=14016
	bool            shader_cache      = true;
	bool            perf_overlay      = false;
	int             global_upscaler   = 0; // 0=Off, 1=CAS, 2=FSR1
	float           global_sharpness  = 0.6f;
	int             global_res_scale  = 0; // 0=1.0x, 1=1.5x, 2=2.0x
	float           scroll_offset     = 0.0f;
	float           stick_cooldown    = 0.0f;
	uint32_t        frame_counter     = 0;

	Common::GameProfile     active_profile {};
	Common::GamePatchBundle active_patches {};
	bool                    has_active_patches = false;

	constexpr std::array<uint32_t, 5> kMemProfiles = {0u, 8192u, 10240u, 12800u, 14016u};
	constexpr std::array<const char*, 5> kMemLabels = {
	    "Auto (Host-Aware)", "8192 MiB (16 GB Safe)", "10240 MiB (24 GB)",
	    "12800 MiB (Full PS5)", "14016 MiB (PS5 Pro)"};
	constexpr std::array<const char*, 3> kUpscalerLabels = {
	    "Off (Native Blit)", "AMD FidelityFX CAS", "AMD FSR 1.0 EASU + RCAS"};
	constexpr std::array<float, 3>       kResScales      = {1.0f, 1.5f, 2.0f};
	constexpr std::array<const char*, 3> kResScaleLabels = {
	    "1.0x (Native)", "1.5x (1440p Scale)", "2.0x (4K UHD)"};
	constexpr std::array<uint32_t, 3>    kFpsCaps        = {0u, 60u, 120u};
	constexpr std::array<const char*, 3> kFpsCapLabels   = {
	    "Native / Game Default", "60 FPS", "120 FPS (Unlocked)"};

	auto OpenGameProfileModal = [&]() {
		if (games.empty() || selected_idx < 0 || selected_idx >= static_cast<int>(games.size())) {
			return;
		}
		const auto& g = games[static_cast<size_t>(selected_idx)];
		if (g.is_placeholder) {
			show_settings = true;
			return;
		}
		active_profile     = Common::LoadGameProfile(g.title_id);
		has_active_patches = Common::LoadPatchBundle(Common::PatchBundlePath(g.title_id),
		                                             active_patches);
		show_game_profile  = true;
	};

	const auto start_tp = std::chrono::steady_clock::now();
	auto       last_tp  = start_tp;
	bool       running  = true;

	auto TriggerLaunch = [&]() {
		if (games.empty() || selected_idx < 0 || selected_idx >= static_cast<int>(games.size())) {
			return;
		}
		const auto& g = games[static_cast<size_t>(selected_idx)];
		if (g.is_placeholder) {
			show_settings = true;
			return;
		}
		const auto prof             = Common::LoadGameProfile(g.title_id);
		result.launch_requested     = true;
		result.app0_dir             = g.game_dir;
		result.elf_path             = g.eboot_path;
		result.guest_memory_mb      = prof.guest_memory_mb != 0
		                                  ? prof.guest_memory_mb
		                                  : kMemProfiles[static_cast<size_t>(mem_profile)];
		result.shader_cache         = shader_cache;
		result.perf_overlay         = perf_overlay;
		result.present_upscaler     = prof.upscaler_mode != Config::PresentUpscalerMode::Off
		                                  ? prof.upscaler_mode
		                                  : static_cast<Config::PresentUpscalerMode>(global_upscaler);
		result.upscaler_sharpness   = prof.upscaler_mode != Config::PresentUpscalerMode::Off
		                                  ? prof.upscaler_sharpness
		                                  : global_sharpness;
		result.resolution_scale     = prof.resolution_scale > 1.01f
		                                  ? prof.resolution_scale
		                                  : kResScales[static_cast<size_t>(global_res_scale)];
		result.fps_cap              = prof.fps_cap;
		result.enabled_patches      = prof.enabled_patches;
		running                     = false;
	};

	while (running) {
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::clamp(
		    std::chrono::duration<float>(now - last_tp).count(), 0.001f, 0.05f);
		const float elapsed = std::chrono::duration<float>(now - start_tp).count();
		last_tp             = now;
		if (stick_cooldown > 0.0f) {
			stick_cooldown = std::max(0.0f, stick_cooldown - dt);
		}
		if (in_boot_intro && elapsed >= kBootDurationS) {
			in_boot_intro = false;
		}

		SDL_Event ev {};
		while (SDL_PollEvent(&ev)) {
			ImGui_ImplSDL3_ProcessEvent(&ev);
			if (ev.type == SDL_EVENT_QUIT) {
				running = false;
			} else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
				if (gamepad == nullptr) {
					gamepad = SDL_OpenGamepad(ev.gdevice.which);
				}
			} else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
				if (gamepad != nullptr && SDL_GetGamepadID(gamepad) == ev.gdevice.which) {
					SDL_CloseGamepad(gamepad);
					gamepad = nullptr;
				}
			} else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
				if (in_boot_intro) {
					in_boot_intro = false;
					continue;
				}
				if (ev.key.key == SDLK_ESCAPE) {
					if (show_game_profile) {
						show_game_profile = false;
					} else if (show_settings) {
						show_settings = false;
					} else {
						running = false;
					}
				} else if (!show_settings && !show_game_profile) {
					if (ev.key.key == SDLK_LEFT || ev.key.key == SDLK_A) {
						selected_idx = std::max(0, selected_idx - 1);
					} else if (ev.key.key == SDLK_RIGHT || ev.key.key == SDLK_D) {
						selected_idx = std::min(static_cast<int>(games.size()) - 1, selected_idx + 1);
					} else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
					           ev.key.key == SDLK_SPACE) {
						TriggerLaunch();
					} else if (ev.key.key == SDLK_S) {
						show_settings = true;
					} else if (ev.key.key == SDLK_G) {
						OpenGameProfileModal();
					}
				}
			} else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
				if (in_boot_intro) {
					in_boot_intro = false;
					continue;
				}
				if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
					if (show_game_profile) {
						show_game_profile = false;
					} else if (show_settings) {
						show_settings = false;
					} else {
						running = false;
					}
				} else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
					show_settings = !show_settings;
				} else if (!show_settings && !show_game_profile) {
					if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
						selected_idx = std::max(0, selected_idx - 1);
					} else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
						selected_idx = std::min(static_cast<int>(games.size()) - 1, selected_idx + 1);
					} else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
						TriggerLaunch();
					} else if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) {
						OpenGameProfileModal();
					}
				}
			}
		}

		// Left Analog Stick horizontal carousel navigation
		if (!in_boot_intro && !show_settings && gamepad != nullptr && stick_cooldown <= 0.0f) {
			const int16_t lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
			if (lx < -16000) {
				selected_idx   = std::max(0, selected_idx - 1);
				stick_cooldown = 0.18f;
			} else if (lx > 16000) {
				selected_idx   = std::min(static_cast<int>(games.size()) - 1, selected_idx + 1);
				stick_cooldown = 0.18f;
			}
		}

		int win_w = 1280, win_h = 720;
		SDL_GetWindowSize(window, &win_w, &win_h);
		const float fw = static_cast<float>(win_w);
		const float fh = static_cast<float>(win_h);

		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();

		ImDrawList* bg_dl = ImGui::GetBackgroundDrawList();
		SDL_Texture* active_pic =
		    (!in_boot_intro && selected_idx < static_cast<int>(media.size()))
		        ? media[static_cast<size_t>(selected_idx)].pic_tex
		        : nullptr;
		const float dash_alpha =
		    in_boot_intro ? 0.0f : std::clamp((elapsed - kBootDurationS) * 2.5f, 0.25f, 1.0f);
		DrawAmbientBackground(bg_dl, fw, fh, elapsed, particles, dt, active_pic, dash_alpha);

		if (in_boot_intro) {
			// Smooth PS5-style fade-in / fade-out boot intro
			const float fade_in  = std::clamp(elapsed / 0.65f, 0.0f, 1.0f);
			const float fade_out = std::clamp((kBootDurationS - elapsed) / 0.45f, 0.0f, 1.0f);
			const float alpha    = std::min(fade_in, fade_out);
			const int   a255     = static_cast<int>(alpha * 255.0f);

			ImGui::SetNextWindowPos({0.0f, 0.0f});
			ImGui::SetNextWindowSize({fw, fh});
			ImGui::Begin("##boot_intro", nullptr,
			             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
			                 ImGuiWindowFlags_NoInputs);
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const float cx = fw * 0.5f;
			const float cy = fh * 0.46f;
			const float bar_w = 240.0f * (0.4f + 0.6f * fade_in);
			dl->AddRectFilled({cx - bar_w, cy + 28.0f}, {cx + bar_w, cy + 31.0f},
			                  IM_COL32(72, 168, 255, a255), 2.0f);

			ImGui::PushFont(nullptr, 42.0f);
			const char* logo = "KYTY-PROSPERO";
			const auto  lsz  = ImGui::CalcTextSize(logo);
			ImGui::SetCursorPos({cx - lsz.x * 0.5f, cy - lsz.y * 0.5f - 10.0f});
			ImGui::TextColored({0.92f, 0.96f, 1.0f, alpha}, "%s", logo);
			ImGui::PopFont();

			ImGui::PushFont(nullptr, 16.0f);
			const char* sub = "PlayStation 5 Compatibility Engine  •  Vulkan 1.3";
			const auto  ssz = ImGui::CalcTextSize(sub);
			ImGui::SetCursorPos({cx - ssz.x * 0.5f, cy + 44.0f});
			ImGui::TextColored({0.58f, 0.78f, 1.0f, alpha * 0.85f}, "%s", sub);
			ImGui::PopFont();

			ImGui::SetCursorPos({cx - 110.0f, fh - 48.0f});
			ImGui::TextColored({0.65f, 0.72f, 0.82f, alpha * 0.7f}, "Press any key to skip");
			ImGui::End();
		} else {
			// Fullscreen transparent canvas for PS5 Home Dashboard
			ImGui::SetNextWindowPos({0.0f, 0.0f});
			ImGui::SetNextWindowSize({fw, fh});
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
			ImGui::Begin("##ps5_home", nullptr,
			             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
			                 ImGuiWindowFlags_NoBringToFrontOnFocus |
			                 ImGuiWindowFlags_NoScrollbar);
			ImDrawList* dl = ImGui::GetWindowDrawList();

			// --- Top Status Bar ---
			ImGui::SetCursorPos({48.0f, 26.0f});
			ImGui::PushFont(nullptr, 22.0f);
			ImGui::TextColored({1.0f, 1.0f, 1.0f, 1.0f}, "Games");
			ImGui::SameLine(0.0f, 28.0f);
			ImGui::TextColored({0.55f, 0.65f, 0.80f, 0.85f}, "KYTY-PROSPERO");
			ImGui::PopFont();

			const std::string clock_str = CurrentClockString();
			const char*       gp_str    = gamepad != nullptr ? "Gamepad Connected" : "Keyboard";
			ImGui::SetCursorPos({fw - 360.0f, 26.0f});
			ImGui::TextColored(gamepad != nullptr ? ImVec4(0.38f, 0.90f, 0.55f, 1.0f)
			                                      : ImVec4(0.65f, 0.75f, 0.90f, 0.9f),
			                   "%s", gp_str);
			ImGui::SameLine(0.0f, 18.0f);
			if (ImGui::Button("Settings [S]")) {
				show_settings = !show_settings;
			}
			ImGui::SameLine(0.0f, 18.0f);
			ImGui::TextColored({0.95f, 0.97f, 1.0f, 1.0f}, "%s", clock_str.c_str());

			// --- Horizontal Carousel of Rounded Game Tiles ---
			const float base_tile   = 152.0f;
			const float spacing     = 26.0f;
			const float target_off  = static_cast<float>(selected_idx) * (base_tile + spacing);
			scroll_offset += (target_off - scroll_offset) * std::min(1.0f, dt * 10.0f);
			const float carousel_y  = 112.0f;
			const float start_x     = 72.0f - scroll_offset;

			for (size_t i = 0; i < games.size(); i++) {
				const bool  is_sel = (static_cast<int>(i) == selected_idx);
				const float target_s = is_sel ? 1.18f : 0.94f;
				media[i].scale += (target_s - media[i].scale) * std::min(1.0f, dt * 12.0f);

				const float sz = base_tile * media[i].scale;
				const float cx = start_x + static_cast<float>(i) * (base_tile + spacing) + base_tile * 0.5f;
				const float cy = carousel_y + base_tile * 0.55f;
				const ImVec2 p0 {cx - sz * 0.5f, cy - sz * 0.5f};
				const ImVec2 p1 {cx + sz * 0.5f, cy + sz * 0.5f};

				if (p1.x < -40.0f || p0.x > fw + 40.0f) {
					continue;
				}

				if (media[i].icon_tex != nullptr) {
					dl->AddImageRounded(reinterpret_cast<ImTextureID>(media[i].icon_tex), p0, p1,
					                    {0.0f, 0.0f}, {1.0f, 1.0f}, IM_COL32(255, 255, 255, 255),
					                    16.0f);
				} else {
					const ImU32 c_top = games[i].is_placeholder ? IM_COL32(26, 44, 82, 235)
					                                            : IM_COL32(24, 78, 168, 245);
					const ImU32 c_bot = games[i].is_placeholder ? IM_COL32(14, 24, 48, 245)
					                                            : IM_COL32(10, 32, 84, 250);
					dl->AddRectFilledMultiColor(p0, p1, c_top, c_top, c_bot, c_bot);
					const std::string abbr = MakeInitials(games[i].title_name);
					ImGui::PushFont(nullptr, is_sel ? 34.0f : 26.0f);
					const auto tsz = ImGui::CalcTextSize(abbr.c_str());
					dl->AddText(ImGui::GetFont(), is_sel ? 34.0f : 26.0f,
					            {cx - tsz.x * 0.5f, cy - tsz.y * 0.5f - 8.0f},
					            IM_COL32(235, 245, 255, 245), abbr.c_str());
					ImGui::PopFont();
					dl->AddText({p0.x + 12.0f, p1.y - 26.0f}, IM_COL32(140, 195, 255, 210),
					            games[i].title_id.c_str());
				}

				if (is_sel) {
					dl->AddRect({p0.x - 4.0f, p0.y - 4.0f}, {p1.x + 4.0f, p1.y + 4.0f},
					            IM_COL32(255, 255, 255, 245), 18.0f, 0, 3.0f);
					dl->AddRect({p0.x - 8.0f, p0.y - 8.0f}, {p1.x + 8.0f, p1.y + 8.0f},
					            IM_COL32(64, 170, 255, 110), 22.0f, 0, 3.0f);
				} else {
					dl->AddRect(p0, p1, IM_COL32(120, 160, 220, 70), 16.0f, 0, 1.5f);
				}

				// Trophy completion pill badge on top-right of each non-placeholder game tile
				if (!games[i].is_placeholder) {
					char tbadge[24] {};
					std::snprintf(tbadge, sizeof(tbadge), "%u%%", games[i].trophy_percent);
					const ImVec2 bmin {p1.x - 52.0f, p0.y + 8.0f};
					const ImVec2 bmax {p1.x - 8.0f, p0.y + 28.0f};
					dl->AddRectFilled(bmin, bmax, IM_COL32(8, 16, 36, 210), 9.0f);
					dl->AddRect(bmin, bmax, IM_COL32(255, 212, 92, 190), 9.0f, 0, 1.2f);
					dl->AddText({bmin.x + 9.0f, bmin.y + 3.0f}, IM_COL32(255, 224, 128, 255), tbadge);
				}

				// Clickable invisible hit area on tile
				ImGui::SetCursorScreenPos(p0);
				char btn_id[32] {};
				std::snprintf(btn_id, sizeof(btn_id), "##tile_%zu", i);
				if (ImGui::InvisibleButton(btn_id, {sz, sz})) {
					if (is_sel) {
						TriggerLaunch();
					} else {
						selected_idx = static_cast<int>(i);
					}
				}
			}

			// --- Hero Details & Prominent Play Button ---
			const auto& sel = games[static_cast<size_t>(selected_idx)];
			const float hero_y = fh * 0.56f;
			ImGui::SetCursorPos({72.0f, hero_y});
			ImGui::PushFont(nullptr, 38.0f);
			ImGui::TextColored({1.0f, 1.0f, 1.0f, 1.0f}, "%s", sel.title_name.c_str());
			ImGui::PopFont();

			ImGui::SetCursorPos({74.0f, hero_y + 52.0f});
			if (!sel.is_placeholder) {
				ImGui::TextColored({0.62f, 0.80f, 1.0f, 0.95f},
				                   "%s   •   %s   •   Trophies: %u%% (%u/%u)",
				                   sel.title_id.c_str(), sel.content_version.c_str(),
				                   sel.trophy_percent, sel.trophy_unlocked, sel.trophy_total);
			} else {
				ImGui::TextColored({0.62f, 0.80f, 1.0f, 0.95f}, "%s   •   %s",
				                   sel.title_id.c_str(), sel.content_version.c_str());
			}

			ImGui::SetCursorPos({72.0f, hero_y + 92.0f});
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 24.0f);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.48f, 0.95f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.58f, 1.0f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.38f, 0.82f, 1.0f));
			ImGui::PushFont(nullptr, 20.0f);
			if (ImGui::Button(sel.is_placeholder ? "Configure Games Folder" : "Play", {220.0f, 52.0f})) {
				TriggerLaunch();
			}
			ImGui::PopFont();
			ImGui::PopStyleColor(3);
			if (!sel.is_placeholder) {
				ImGui::SameLine(0.0f, 16.0f);
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.20f, 0.36f, 0.92f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.32f, 0.54f, 0.98f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.09f, 0.16f, 0.30f, 1.0f));
				ImGui::PushFont(nullptr, 18.0f);
				if (ImGui::Button("Game Settings [G]", {210.0f, 52.0f})) {
					OpenGameProfileModal();
				}
				ImGui::PopFont();
				ImGui::PopStyleColor(3);
			}
			ImGui::PopStyleVar();

			// Bottom navigation legend
			ImGui::SetCursorPos({72.0f, fh - 40.0f});
			ImGui::TextColored({0.68f, 0.76f, 0.88f, 0.85f},
			                   "Arrows / D-Pad / Stick: Browse   |   Enter / Cross: Play   |   "
			                   "G / Square: Game Settings   |   S / Start: Settings   |   Esc / Circle: Exit");
			ImGui::End();
			ImGui::PopStyleVar();

			// --- Quick Settings Modal ---
			if (show_settings) {
				ImGui::SetNextWindowPos({fw * 0.5f, fh * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
				ImGui::SetNextWindowSize({560.0f, 380.0f});
				ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
				if (ImGui::Begin("KYTY-PROSPERO Settings", &show_settings,
				                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
					const auto host_mib =
					    LibKernel::Memory::QueryHostPhysicalMemoryBytes() / (1024ull * 1024ull);
					ImGui::Text("Detected Host RAM: %llu MiB",
					            static_cast<unsigned long long>(host_mib));
					ImGui::Combo("Guest Memory Profile", &mem_profile, kMemLabels.data(),
					             static_cast<int>(kMemLabels.size()));
					ImGui::Checkbox("Enable Disk Shader Cache (--shader-cache)", &shader_cache);
					ImGui::Checkbox("Show Diagnostic HUD on Boot (--perf-overlay)", &perf_overlay);
					ImGui::Separator();
					ImGui::TextColored({0.48f, 0.80f, 1.0f, 1.0f}, "Beyond-PS5 Presentation Upscaler");
					ImGui::Combo("Compute Upscaler", &global_upscaler, kUpscalerLabels.data(),
					             static_cast<int>(kUpscalerLabels.size()));
					ImGui::SliderFloat("CAS / FSR Sharpness", &global_sharpness, 0.0f, 1.0f, "%.2f");
					ImGui::Combo("Resolution Scale", &global_res_scale, kResScaleLabels.data(),
					             static_cast<int>(kResScaleLabels.size()));
					ImGui::Separator();
					ImGui::InputText("Games Directory", games_dir_buf, sizeof(games_dir_buf));
					if (ImGui::Button("Rescan Library")) {
						games_dir    = std::filesystem::path(games_dir_buf);
						games        = ScanPs5GameLibrary(games_dir);
						selected_idx = 0;
						LoadMediaForLibrary(renderer, games, media);
					}
					ImGui::SameLine();
					if (ImGui::Button("Close")) {
						show_settings = false;
					}
				}
				ImGui::End();
				ImGui::PopStyleVar();
			}

			// --- Per-Game Profile & Patches Modal (profiles/<TitleID>.json) ---
			if (show_game_profile) {
				ImGui::SetNextWindowPos({fw * 0.5f, fh * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
				ImGui::SetNextWindowSize({600.0f, 460.0f});
				ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f);
				if (ImGui::Begin("Per-Game Profile & Patches", &show_game_profile,
				                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
					ImGui::TextColored({0.55f, 0.85f, 1.0f, 1.0f}, "Title ID: %s",
					                   active_profile.title_id.c_str());
					ImGui::Separator();

					int upscaler_idx = static_cast<int>(active_profile.upscaler_mode);
					if (ImGui::Combo("Upscaler", &upscaler_idx, kUpscalerLabels.data(),
					                 static_cast<int>(kUpscalerLabels.size()))) {
						active_profile.upscaler_mode =
						    static_cast<Config::PresentUpscalerMode>(upscaler_idx);
					}
					ImGui::SliderFloat("Sharpness", &active_profile.upscaler_sharpness, 0.0f, 1.0f,
					                   "%.2f");

					int scale_idx = active_profile.resolution_scale >= 1.9f
					                    ? 2
					                    : (active_profile.resolution_scale >= 1.4f ? 1 : 0);
					if (ImGui::Combo("Resolution Scale", &scale_idx, kResScaleLabels.data(),
					                 static_cast<int>(kResScaleLabels.size()))) {
						active_profile.resolution_scale =
						    kResScales[static_cast<size_t>(scale_idx)];
					}

					int fps_idx = active_profile.fps_cap >= 120u
					                  ? 2
					                  : (active_profile.fps_cap >= 60u ? 1 : 0);
					if (ImGui::Combo("FPS Cap / VBlank", &fps_idx, kFpsCapLabels.data(),
					                 static_cast<int>(kFpsCapLabels.size()))) {
						active_profile.fps_cap = kFpsCaps[static_cast<size_t>(fps_idx)];
					}

					int gmem_idx = 0;
					for (size_t k = 0; k < kMemProfiles.size(); ++k) {
						if (kMemProfiles[k] == active_profile.guest_memory_mb) {
							gmem_idx = static_cast<int>(k);
							break;
						}
					}
					if (ImGui::Combo("Guest Memory Budget", &gmem_idx, kMemLabels.data(),
					                 static_cast<int>(kMemLabels.size()))) {
						active_profile.guest_memory_mb =
						    kMemProfiles[static_cast<size_t>(gmem_idx)];
					}

					ImGui::Separator();
					ImGui::TextColored({1.0f, 0.85f, 0.45f, 1.0f}, "Game Patches (patches/%s.json)",
					                   active_profile.title_id.c_str());
					if (!has_active_patches || active_patches.patches.empty()) {
						ImGui::TextDisabled("No patches found for this TitleID.");
					} else {
						for (const auto& patch: active_patches.patches) {
							auto it = std::find(active_profile.enabled_patches.begin(),
							                    active_profile.enabled_patches.end(), patch.id);
							bool enabled = (it != active_profile.enabled_patches.end());
							std::string label = "[" + patch.category + "] " + patch.name;
							if (ImGui::Checkbox(label.c_str(), &enabled)) {
								if (enabled && it == active_profile.enabled_patches.end()) {
									active_profile.enabled_patches.push_back(patch.id);
								} else if (!enabled && it != active_profile.enabled_patches.end()) {
									active_profile.enabled_patches.erase(it);
								}
							}
						}
					}

					ImGui::Separator();
					if (ImGui::Button("Save Profile")) {
						(void)Common::SaveGameProfile(active_profile);
						show_game_profile = false;
					}
					ImGui::SameLine();
					if (ImGui::Button("Cancel")) {
						show_game_profile = false;
					}
				}
				ImGui::End();
				ImGui::PopStyleVar();
			}
		}

		ImGui::Render();
		SDL_SetRenderDrawColor(renderer, 6, 12, 32, 255);
		SDL_RenderClear(renderer);
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
		SDL_RenderPresent(renderer);

		frame_counter++;
		if (demo_max_frames > 0 && frame_counter >= demo_max_frames) {
			running = false;
		}
	}

	FreeMedia(media);
	if (gamepad != nullptr) {
		SDL_CloseGamepad(gamepad);
	}
	ImGui_ImplSDLRenderer3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return result;
}

} // namespace Libs::Graphics
