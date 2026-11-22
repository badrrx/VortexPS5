#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PERFORMANCEOVERLAY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PERFORMANCEOVERLAY_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

namespace Libs::Graphics {

struct GraphicContext;

enum class DiagnosticPane : uint8_t {
	None        = 0,
	Performance = 1, // F1: FPS, frame graph, 1% lows, >33ms / >50ms hitches
	Shaders     = 2, // F2: Pipeline cache hits/misses, async queue/workers, slowest compile, Flush button
	Memory      = 3, // F3: VMA pressure tier, per-heap usage/budget bars, guest budget, GC button
};

struct DiagnosticTelemetrySnapshot {
	uint64_t pipeline_hits            = 0;
	uint64_t pipeline_misses          = 0;
	uint32_t async_queue_depth        = 0;
	uint32_t async_active_workers     = 0;
	uint32_t async_total_workers      = 0;
	uint64_t slowest_pipeline_us      = 0;
	uint64_t hitches_over_33ms        = 0;
	uint64_t hitches_over_50ms        = 0;
	uint64_t active_guest_budget_mib  = 0;
};

// Called for each main-bus guest flip, with the guest's video-out size.
void PerformanceOverlayRecordFlip(uint32_t guest_width, uint32_t guest_height) noexcept;

[[nodiscard]] bool           PerformanceOverlayEnabled() noexcept;
[[nodiscard]] DiagnosticPane ActiveDiagnosticPane() noexcept;
void                         TogglePerformanceOverlay() noexcept;
void                         ToggleDiagnosticPane(DiagnosticPane pane) noexcept;
// Switches between the compact and the detailed panel, showing the panel if it was hidden.
void TogglePerformanceOverlayDetails() noexcept;

// Lock-free telemetry updates from PipelineCache / RenderContext (no-op when HUD is hidden).
void HudRecordPipelineHit() noexcept;
void HudRecordPipelineMiss() noexcept;
void HudUpdateAsyncPipelineState(uint32_t queue_depth, uint32_t active_workers,
                                 uint32_t total_workers) noexcept;
void HudRecordPipelineCompileUs(uint64_t compile_us) noexcept;
void HudSetActiveGuestBudgetBytes(uint64_t bytes) noexcept;
[[nodiscard]] DiagnosticTelemetrySnapshot GetDiagnosticTelemetrySnapshot() noexcept;

// Lock-free one-shot UI action requests consumed on the GPU thread.
void               RequestHudPipelineCacheFlush() noexcept;
[[nodiscard]] bool ConsumeHudPipelineCacheFlush() noexcept;
void               RequestHudForceGc() noexcept;
[[nodiscard]] bool ConsumeHudForceGc() noexcept;

// Changes when the panel needs a repaint without a guest flip (toggled, or the guest stalled).
[[nodiscard]] uint64_t PerformanceOverlayRevision() noexcept;

// Must run between ImGui::NewFrame() and ImGui::Render(), on the present thread while the
// graphics subsystem is alive. Draws in the top-right corner and returns the panel's bottom
// edge, or 0 when hidden.
float DrawPerformanceOverlay(const GraphicContext& graphics, vk::Extent2D extent);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PERFORMANCEOVERLAY_H_
