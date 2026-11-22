#ifndef KYTY_COMMON_TROPHY_TRACKER_H_
#define KYTY_COMMON_TROPHY_TRACKER_H_

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

enum class TrophyGrade : uint8_t {
	Platinum = 1,
	Gold     = 2,
	Silver   = 3,
	Bronze   = 4,
};

struct TrophyRecord {
	int32_t     id          = 0;
	std::string title;
	std::string description;
	TrophyGrade grade       = TrophyGrade::Bronze;
	bool        unlocked    = false;
	uint64_t    unlocked_at = 0; // Unix epoch seconds
};

struct GameTrophyStore {
	std::string               title_id;
	uint32_t                  total_trophies = 0;
	std::vector<TrophyRecord> trophies;

	[[nodiscard]] uint32_t UnlockedCount() const noexcept;
	[[nodiscard]] uint32_t CompletionPercentage() const noexcept;
};

// Path: <saves_root>/<TitleID>.json (default: saves/trophies/<TitleID>.json)
[[nodiscard]] std::filesystem::path TrophyStorePath(
    std::string_view title_id, const std::filesystem::path& saves_root = "saves/trophies");

[[nodiscard]] GameTrophyStore LoadTrophyStore(
    std::string_view title_id, const std::filesystem::path& saves_root = "saves/trophies");

[[nodiscard]] bool SaveTrophyStore(const GameTrophyStore&       store,
                                   const std::filesystem::path& saves_root = "saves/trophies");

// Records a newly unlocked trophy in saves/trophies/<TitleID>.json (idempotent).
// Returns true if this trophy was newly unlocked.
bool RecordTrophyUnlock(std::string_view title_id, int32_t trophy_id, std::string_view title,
                        std::string_view description, TrophyGrade grade, uint32_t total_trophies,
                        const std::filesystem::path& saves_root = "saves/trophies");

// Animated PS5-style Trophy Popup state machine (deterministic for both UI & unit tests).
enum class TrophyPopupPhase : uint8_t {
	Hidden   = 0,
	SlideIn  = 1,
	Hold     = 2,
	SlideOut = 3,
};

struct TrophyPopupEvent {
	std::string title;
	std::string description;
	TrophyGrade grade = TrophyGrade::Bronze;
};

struct TrophyPopupSnapshot {
	TrophyPopupPhase phase         = TrophyPopupPhase::Hidden;
	float            slide_factor  = 0.0f; // 0.0 = offscreen, 1.0 = fully visible
	float            alpha         = 0.0f;
	TrophyPopupEvent event;
};

class TrophyPopupQueue {
public:
	void Push(TrophyPopupEvent event);
	void Tick(float dt_seconds, float hold_seconds = 5.0f);
	[[nodiscard]] TrophyPopupSnapshot Snapshot() const;
	[[nodiscard]] size_t              PendingCount() const;
	void                              Clear();

private:
	mutable std::mutex           m_mutex;
	std::deque<TrophyPopupEvent> m_queue;
	std::optional<TrophyPopupEvent> m_active;
	TrophyPopupPhase             m_phase      = TrophyPopupPhase::Hidden;
	float                        m_phase_time = 0.0f;
	float                        m_slide      = 0.0f;
	float                        m_alpha      = 0.0f;
};

[[nodiscard]] const char* TrophyGradeName(TrophyGrade grade) noexcept;

} // namespace Common

#endif // KYTY_COMMON_TROPHY_TRACKER_H_
