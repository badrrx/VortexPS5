#include "common/trophyTracker.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Common {

namespace {

constexpr float kSlideInDuration  = 0.30f;
constexpr float kSlideOutDuration = 0.35f;

float EaseOutCubic(float t) noexcept {
	const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
	return 1.0f - u * u * u;
}

TrophyGrade ParseGrade(int g) noexcept {
	switch (g) {
		case 1:  return TrophyGrade::Platinum;
		case 2:  return TrophyGrade::Gold;
		case 3:  return TrophyGrade::Silver;
		default: return TrophyGrade::Bronze;
	}
}

} // namespace

const char* TrophyGradeName(TrophyGrade grade) noexcept {
	switch (grade) {
		case TrophyGrade::Platinum: return "Platinum";
		case TrophyGrade::Gold:     return "Gold";
		case TrophyGrade::Silver:   return "Silver";
		default:                    return "Bronze";
	}
}

uint32_t GameTrophyStore::UnlockedCount() const noexcept {
	uint32_t count = 0;
	for (const auto& t: trophies) {
		if (t.unlocked) {
			++count;
		}
	}
	return count;
}

uint32_t GameTrophyStore::CompletionPercentage() const noexcept {
	const uint32_t unlocked = UnlockedCount();
	const uint32_t denom    = std::max(total_trophies, static_cast<uint32_t>(trophies.size()));
	if (denom == 0) {
		return 0;
	}
	return std::min<uint32_t>(100u, (unlocked * 100u) / denom);
}

std::filesystem::path TrophyStorePath(std::string_view             title_id,
                                      const std::filesystem::path& saves_root) {
	return saves_root / (std::string(title_id) + ".json");
}

GameTrophyStore LoadTrophyStore(std::string_view             title_id,
                                const std::filesystem::path& saves_root) {
	GameTrophyStore store {};
	store.title_id = std::string(title_id);
	if (title_id.empty()) {
		return store;
	}
	std::ifstream ifs(TrophyStorePath(title_id, saves_root), std::ios::binary);
	if (!ifs.is_open()) {
		return store;
	}
	const auto j = nlohmann::json::parse(ifs, nullptr, false);
	if (j.is_discarded() || !j.is_object()) {
		return store;
	}
	store.total_trophies = j.value("totalTrophies", 0u);
	if (auto it = j.find("trophies"); it != j.end() && it->is_array()) {
		for (const auto& item: *it) {
			if (!item.is_object()) continue;
			TrophyRecord rec {};
			rec.id          = item.value("id", 0);
			rec.title       = item.value("title", std::string {});
			rec.description = item.value("description", std::string {});
			rec.grade       = ParseGrade(item.value("grade", 4));
			rec.unlocked    = item.value("unlocked", true);
			rec.unlocked_at = item.value("unlockedAt", 0ull);
			store.trophies.push_back(std::move(rec));
		}
	}
	return store;
}

bool SaveTrophyStore(const GameTrophyStore& store, const std::filesystem::path& saves_root) {
	if (store.title_id.empty()) {
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(saves_root, ec);

	nlohmann::json j;
	j["titleId"]       = store.title_id;
	j["totalTrophies"] = std::max(store.total_trophies, static_cast<uint32_t>(store.trophies.size()));
	j["unlockedCount"] = store.UnlockedCount();
	j["percentage"]    = store.CompletionPercentage();

	auto arr = nlohmann::json::array();
	for (const auto& t: store.trophies) {
		nlohmann::json item;
		item["id"]          = t.id;
		item["title"]       = t.title;
		item["description"] = t.description;
		item["grade"]       = static_cast<int>(t.grade);
		item["gradeName"]   = TrophyGradeName(t.grade);
		item["unlocked"]    = t.unlocked;
		item["unlockedAt"]  = t.unlocked_at;
		arr.push_back(std::move(item));
	}
	j["trophies"] = std::move(arr);

	std::ofstream ofs(TrophyStorePath(store.title_id, saves_root),
	                  std::ios::binary | std::ios::trunc);
	if (!ofs.is_open()) {
		return false;
	}
	ofs << j.dump(2) << "\n";
	return ofs.good();
}

bool RecordTrophyUnlock(std::string_view title_id, int32_t trophy_id, std::string_view title,
                        std::string_view description, TrophyGrade grade, uint32_t total_trophies,
                        const std::filesystem::path& saves_root) {
	if (title_id.empty()) {
		return false;
	}
	auto store = LoadTrophyStore(title_id, saves_root);
	store.total_trophies = std::max(store.total_trophies, total_trophies);

	const uint64_t now_unix = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(
	        std::chrono::system_clock::now().time_since_epoch())
	        .count());

	for (auto& existing: store.trophies) {
		if (existing.id == trophy_id) {
			if (existing.unlocked) {
				return false; // Already unlocked
			}
			existing.unlocked    = true;
			existing.unlocked_at = now_unix;
			if (!title.empty()) existing.title = std::string(title);
			if (!description.empty()) existing.description = std::string(description);
			existing.grade = grade;
			(void)SaveTrophyStore(store, saves_root);
			return true;
		}
	}

	TrophyRecord rec {};
	rec.id          = trophy_id;
	rec.title       = std::string(title);
	rec.description = std::string(description);
	rec.grade       = grade;
	rec.unlocked    = true;
	rec.unlocked_at = now_unix;
	store.trophies.push_back(std::move(rec));
	(void)SaveTrophyStore(store, saves_root);
	return true;
}

void TrophyPopupQueue::Push(TrophyPopupEvent event) {
	std::scoped_lock lock(m_mutex);
	m_queue.push_back(std::move(event));
}

void TrophyPopupQueue::Tick(float dt_seconds, float hold_seconds) {
	std::scoped_lock lock(m_mutex);
	const float dt = std::max(dt_seconds, 0.0f);

	if (m_phase == TrophyPopupPhase::Hidden) {
		if (m_queue.empty()) {
			m_slide = 0.0f;
			m_alpha = 0.0f;
			return;
		}
		m_active     = std::move(m_queue.front());
		m_queue.pop_front();
		m_phase      = TrophyPopupPhase::SlideIn;
		m_phase_time = 0.0f;
	}

	m_phase_time += dt;

	if (m_phase == TrophyPopupPhase::SlideIn) {
		const float t = std::clamp(m_phase_time / kSlideInDuration, 0.0f, 1.0f);
		m_slide       = EaseOutCubic(t);
		m_alpha       = t;
		if (m_phase_time >= kSlideInDuration) {
			m_phase      = TrophyPopupPhase::Hold;
			m_phase_time = 0.0f;
			m_slide      = 1.0f;
			m_alpha      = 1.0f;
		}
	} else if (m_phase == TrophyPopupPhase::Hold) {
		m_slide = 1.0f;
		m_alpha = 1.0f;
		if (m_phase_time >= std::max(hold_seconds, 0.1f)) {
			m_phase      = TrophyPopupPhase::SlideOut;
			m_phase_time = 0.0f;
		}
	} else if (m_phase == TrophyPopupPhase::SlideOut) {
		const float t = std::clamp(m_phase_time / kSlideOutDuration, 0.0f, 1.0f);
		m_slide       = 1.0f - EaseOutCubic(t);
		m_alpha       = 1.0f - t;
		if (m_phase_time >= kSlideOutDuration) {
			m_active.reset();
			m_phase      = TrophyPopupPhase::Hidden;
			m_phase_time = 0.0f;
			m_slide      = 0.0f;
			m_alpha      = 0.0f;
		}
	}
}

TrophyPopupSnapshot TrophyPopupQueue::Snapshot() const {
	std::scoped_lock lock(m_mutex);
	TrophyPopupSnapshot snap {};
	snap.phase        = m_phase;
	snap.slide_factor = m_slide;
	snap.alpha        = m_alpha;
	if (m_active.has_value()) {
		snap.event = *m_active;
	}
	return snap;
}

size_t TrophyPopupQueue::PendingCount() const {
	std::scoped_lock lock(m_mutex);
	return m_queue.size();
}

void TrophyPopupQueue::Clear() {
	std::scoped_lock lock(m_mutex);
	m_queue.clear();
	m_active.reset();
	m_phase      = TrophyPopupPhase::Hidden;
	m_phase_time = 0.0f;
	m_slide      = 0.0f;
	m_alpha      = 0.0f;
}

} // namespace Common
