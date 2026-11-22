#include "graphics/presentation/performanceOverlay.h"

#include <iostream>
#include <thread>
#include <vector>

using namespace Libs::Graphics;

namespace {

bool TestPaneToggleTransitions() {
	// Start hidden
	ToggleDiagnosticPane(DiagnosticPane::None);
	if (PerformanceOverlayEnabled() || ActiveDiagnosticPane() != DiagnosticPane::None) {
		std::cerr << "[FAIL] Expected HUD hidden after None toggle\n";
		return false;
	}
	const uint64_t rev0 = PerformanceOverlayRevision();

	// Press F1 -> Performance pane active
	ToggleDiagnosticPane(DiagnosticPane::Performance);
	if (!PerformanceOverlayEnabled() || ActiveDiagnosticPane() != DiagnosticPane::Performance ||
	    PerformanceOverlayRevision() <= rev0) {
		std::cerr << "[FAIL] F1 did not activate Performance pane\n";
		return false;
	}

	// Press F2 -> switch directly to Shaders pane (stays enabled)
	ToggleDiagnosticPane(DiagnosticPane::Shaders);
	if (!PerformanceOverlayEnabled() || ActiveDiagnosticPane() != DiagnosticPane::Shaders) {
		std::cerr << "[FAIL] F2 did not switch to Shaders pane\n";
		return false;
	}

	// Press F3 -> switch directly to Memory pane
	ToggleDiagnosticPane(DiagnosticPane::Memory);
	if (!PerformanceOverlayEnabled() || ActiveDiagnosticPane() != DiagnosticPane::Memory) {
		std::cerr << "[FAIL] F3 did not switch to Memory pane\n";
		return false;
	}

	// Press F3 again -> hides HUD
	ToggleDiagnosticPane(DiagnosticPane::Memory);
	if (PerformanceOverlayEnabled() || ActiveDiagnosticPane() != DiagnosticPane::None) {
		std::cerr << "[FAIL] Second F3 press did not hide HUD\n";
		return false;
	}

	std::cout << "[PASS 1/3] F1/F2/F3 Pane Toggle State Machine: None -> F1 -> F2 -> F3 -> None\n";
	return true;
}

bool TestZeroOverheadWhenHiddenAndLockFreeTelemetry() {
	// Ensure hidden: hits/misses must be no-op!
	ToggleDiagnosticPane(DiagnosticPane::None);
	const auto before = GetDiagnosticTelemetrySnapshot();
	HudRecordPipelineHit();
	HudRecordPipelineMiss();
	const auto after_hidden = GetDiagnosticTelemetrySnapshot();
	if (after_hidden.pipeline_hits != before.pipeline_hits ||
	    after_hidden.pipeline_misses != before.pipeline_misses) {
		std::cerr << "[FAIL] Pipeline hit/miss counters incremented while HUD was hidden!\n";
		return false;
	}

	// Enable HUD -> multi-threaded lock-free telemetry updates
	ToggleDiagnosticPane(DiagnosticPane::Shaders);
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; t++) {
		threads.emplace_back([t] {
			for (int i = 0; i < 250; i++) {
				HudRecordPipelineHit();
				if (i % 5 == 0) {
					HudRecordPipelineMiss();
				}
				HudRecordPipelineCompileUs(static_cast<uint64_t>((t + 1) * 1000 + i));
			}
		});
	}
	for (auto& th: threads) {
		th.join();
	}
	HudUpdateAsyncPipelineState(7u, 2u, 4u);
	HudSetActiveGuestBudgetBytes(8192ull * 1024ull * 1024ull);

	const auto snap = GetDiagnosticTelemetrySnapshot();
	if (snap.pipeline_hits != before.pipeline_hits + 1000 ||
	    snap.pipeline_misses != before.pipeline_misses + 200 ||
	    snap.async_queue_depth != 7u || snap.async_active_workers != 2u ||
	    snap.async_total_workers != 4u || snap.slowest_pipeline_us < 4249u ||
	    snap.active_guest_budget_mib != 8192u) {
		std::cerr << "[FAIL] Telemetry snapshot mismatch: hits=" << snap.pipeline_hits
		          << " misses=" << snap.pipeline_misses << " q=" << snap.async_queue_depth
		          << " workers=" << snap.async_active_workers << "/" << snap.async_total_workers
		          << " slowest=" << snap.slowest_pipeline_us
		          << " budget=" << snap.active_guest_budget_mib << "\n";
		return false;
	}

	std::cout << "[PASS 2/3] Lock-Free Multi-Thread Telemetry: hits=" << snap.pipeline_hits
	          << " misses=" << snap.pipeline_misses << " queue=" << snap.async_queue_depth
	          << " workers=" << snap.async_active_workers << "/" << snap.async_total_workers
	          << " slowest=" << snap.slowest_pipeline_us << " us budget="
	          << snap.active_guest_budget_mib << " MiB\n";
	return true;
}

bool TestOneShotActionButtons() {
	if (ConsumeHudPipelineCacheFlush() || ConsumeHudForceGc()) {
		std::cerr << "[FAIL] Expected action flags initially false\n";
		return false;
	}
	RequestHudPipelineCacheFlush();
	RequestHudForceGc();
	if (!ConsumeHudPipelineCacheFlush() || ConsumeHudPipelineCacheFlush()) {
		std::cerr << "[FAIL] Flush Pipeline Cache one-shot consume semantics failed\n";
		return false;
	}
	if (!ConsumeHudForceGc() || ConsumeHudForceGc()) {
		std::cerr << "[FAIL] Run GC Now one-shot consume semantics failed\n";
		return false;
	}
	ToggleDiagnosticPane(DiagnosticPane::None);
	std::cout << "[PASS 3/3] One-Shot GPU Thread Actions: Flush Pipeline Cache & Run GC Now verified\n";
	return true;
}

} // namespace

int main() {
	if (!TestPaneToggleTransitions()) {
		return 1;
	}
	if (!TestZeroOverheadWhenHiddenAndLockFreeTelemetry()) {
		return 1;
	}
	if (!TestOneShotActionButtons()) {
		return 1;
	}
	std::cout << "ALL UPGRADE #3 VERIFICATION TESTS PASSED!\n";
	return 0;
}
