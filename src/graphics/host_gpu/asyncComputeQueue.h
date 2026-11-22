#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCCOMPUTEQUEUE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCCOMPUTEQUEUE_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <span>

namespace Libs::Graphics {

struct AsyncComputeQueueSelection {
	uint32_t graphics_family      = UINT32_MAX;
	uint32_t async_compute_family = UINT32_MAX;
	uint32_t async_compute_index  = 0;
	bool     dedicated_family     = false; // True when a compute-only (non-graphics) family exists
	bool     distinct_queue       = false; // True when async compute uses a separate VkQueue handle
};

// Pure queue-family selector:
// 1. Prefers a dedicated compute-only queue family (eCompute && !eGraphics).
// 2. Otherwise uses a second queue index on `graphics_family` if queueCount >= 2.
// 3. Falls back cleanly to `(graphics_family, index=0)` on single-queue GPUs.
[[nodiscard]] AsyncComputeQueueSelection SelectAsyncComputeQueueFamily(
    std::span<const vk::QueueFamilyProperties> families, uint32_t graphics_family) noexcept;

// Vulkan 1.3 Timeline Semaphore bridge synchronizing Graphics <-> Async Compute submissions.
class AsyncComputeTimelineSync final {
public:
	AsyncComputeTimelineSync() = default;
	~AsyncComputeTimelineSync();
	KYTY_CLASS_NO_COPY(AsyncComputeTimelineSync);

	[[nodiscard]] bool Initialize(vk::Device device, uint64_t initial_value = 0);
	void               Destroy();

	[[nodiscard]] vk::Semaphore Handle() const noexcept { return m_timeline; }
	[[nodiscard]] uint64_t      NextValue() noexcept {
        return m_next_value.fetch_add(1, std::memory_order_acq_rel);
	}
	[[nodiscard]] uint64_t CurrentValue() const noexcept {
		return m_next_value.load(std::memory_order_acquire) - 1;
	}

	// Queries the GPU-completed timeline counter via vkGetSemaphoreCounterValue.
	[[nodiscard]] uint64_t QueryCompletedValue() const;

	// Signals the timeline semaphore from the CPU (useful for host-driven handoff & tests).
	[[nodiscard]] bool SignalCpu(uint64_t value);

	// Waits on the CPU until the timeline reaches `value`.
	[[nodiscard]] bool WaitCpu(uint64_t value, uint64_t timeout_ns = UINT64_MAX) const;

	// Builds a Vulkan 1.3 VkSemaphoreSubmitInfo for waiting on Graphics -> Compute or Compute -> Graphics.
	[[nodiscard]] vk::SemaphoreSubmitInfo MakeWaitInfo(
	    uint64_t wait_value, vk::PipelineStageFlags2 stage_mask) const noexcept;
	[[nodiscard]] vk::SemaphoreSubmitInfo MakeSignalInfo(
	    uint64_t signal_value, vk::PipelineStageFlags2 stage_mask) const noexcept;

private:
	vk::Device            m_device   = nullptr;
	vk::Semaphore         m_timeline = nullptr;
	std::atomic<uint64_t> m_next_value {1};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_ASYNCCOMPUTEQUEUE_H_
