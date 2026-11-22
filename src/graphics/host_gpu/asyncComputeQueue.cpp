#include "graphics/host_gpu/asyncComputeQueue.h"

namespace Libs::Graphics {

AsyncComputeQueueSelection SelectAsyncComputeQueueFamily(
    std::span<const vk::QueueFamilyProperties> families, uint32_t graphics_family) noexcept {
	AsyncComputeQueueSelection sel {};
	sel.graphics_family      = graphics_family;
	sel.async_compute_family = graphics_family;
	sel.async_compute_index  = 0;
	sel.dedicated_family     = false;
	sel.distinct_queue       = false;

	if (families.empty() || graphics_family >= families.size()) {
		return sel;
	}

	// 1. Prefer a dedicated hardware Async Compute Engine (eCompute without eGraphics).
	for (uint32_t i = 0; i < static_cast<uint32_t>(families.size()); ++i) {
		const auto flags = families[i].queueFlags;
		if (families[i].queueCount > 0 &&
		    (flags & vk::QueueFlagBits::eCompute) == vk::QueueFlagBits::eCompute &&
		    (flags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags {}) {
			sel.async_compute_family = i;
			sel.async_compute_index  = 0;
			sel.dedicated_family     = true;
			sel.distinct_queue       = true;
			return sel;
		}
	}

	// 2. Secondary preference: a second queue on the universal graphics+compute family.
	if (families[graphics_family].queueCount >= 2u) {
		sel.async_compute_family = graphics_family;
		sel.async_compute_index  = 1u;
		sel.dedicated_family     = false;
		sel.distinct_queue       = true;
		return sel;
	}

	// 3. Safe fallback on single-queue GPUs: share the universal queue.
	return sel;
}

AsyncComputeTimelineSync::~AsyncComputeTimelineSync() {
	Destroy();
}

bool AsyncComputeTimelineSync::Initialize(vk::Device device, uint64_t initial_value) {
	if (device == nullptr) {
		return false;
	}
	Destroy();
	m_device = device;
	m_next_value.store(initial_value + 1, std::memory_order_release);

	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = initial_value;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	if (m_device.createSemaphore(&create_info, nullptr, &m_timeline) != vk::Result::eSuccess) {
		m_timeline = nullptr;
		m_device   = nullptr;
		return false;
	}
	return true;
}

void AsyncComputeTimelineSync::Destroy() {
	if (m_device != nullptr && m_timeline != nullptr) {
		m_device.destroySemaphore(m_timeline, nullptr);
	}
	m_timeline = nullptr;
	m_device   = nullptr;
}

uint64_t AsyncComputeTimelineSync::QueryCompletedValue() const {
	if (m_device == nullptr || m_timeline == nullptr) {
		return 0;
	}
	uint64_t value = 0;
	if (m_device.getSemaphoreCounterValue(m_timeline, &value) != vk::Result::eSuccess) {
		return 0;
	}
	return value;
}

bool AsyncComputeTimelineSync::SignalCpu(uint64_t value) {
	if (m_device == nullptr || m_timeline == nullptr) {
		return false;
	}
	vk::SemaphoreSignalInfo info {};
	info.semaphore = m_timeline;
	info.value     = value;
	return m_device.signalSemaphore(&info) == vk::Result::eSuccess;
}

bool AsyncComputeTimelineSync::WaitCpu(uint64_t value, uint64_t timeout_ns) const {
	if (m_device == nullptr || m_timeline == nullptr) {
		return false;
	}
	vk::SemaphoreWaitInfo info {};
	info.semaphoreCount = 1;
	info.pSemaphores    = &m_timeline;
	info.pValues        = &value;
	return m_device.waitSemaphores(&info, timeout_ns) == vk::Result::eSuccess;
}

vk::SemaphoreSubmitInfo AsyncComputeTimelineSync::MakeWaitInfo(
    uint64_t wait_value, vk::PipelineStageFlags2 stage_mask) const noexcept {
	vk::SemaphoreSubmitInfo info {};
	info.semaphore = m_timeline;
	info.value     = wait_value;
	info.stageMask = stage_mask;
	return info;
}

vk::SemaphoreSubmitInfo AsyncComputeTimelineSync::MakeSignalInfo(
    uint64_t signal_value, vk::PipelineStageFlags2 stage_mask) const noexcept {
	vk::SemaphoreSubmitInfo info {};
	info.semaphore = m_timeline;
	info.value     = signal_value;
	info.stageMask = stage_mask;
	return info;
}

} // namespace Libs::Graphics
