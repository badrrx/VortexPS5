#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTUPSCALER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTUPSCALER_H_

#include "common/common.h"
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <span>

namespace Libs::Graphics {

struct PresentUpscalerPushConstants {
	uint32_t input_width      = 0;
	uint32_t input_height     = 0;
	uint32_t output_width     = 0;
	uint32_t output_height    = 0;
	uint32_t mode             = 0; // 0 = Off/Copy, 1 = CAS, 2 = FSR 1.0 EASU + RCAS
	float    sharpness        = 0.6f;
	float    resolution_scale = 1.0f;
	uint32_t reserved         = 0;
};
static_assert(sizeof(PresentUpscalerPushConstants) == 32);

// Returns the compiled Vulkan 1.3 SPIR-V words for gpu_present_fsr_cas.comp.
[[nodiscard]] std::span<const uint32_t> GetPresentUpscalerSpirv() noexcept;

// Standalone helper to build the Vulkan 1.3 compute pipeline (used by both PresentUpscaler and tests).
// When `use_push_descriptors` is false, creates a standard descriptor set layout so tests can run
// without requiring VK_KHR_push_descriptor.
[[nodiscard]] bool CreatePresentUpscalerComputePipeline(
    vk::Device device, bool use_push_descriptors, vk::DescriptorSetLayout* out_desc_layout,
    vk::PipelineLayout* out_pipeline_layout, vk::Pipeline* out_pipeline);

// Zero-overhead-when-disabled Vulkan 1.3 compute post-processing pass for CAS / FSR 1.0 EASU.
class PresentUpscaler final {
public:
	explicit PresentUpscaler(GraphicContext& graphics);
	~PresentUpscaler();
	KYTY_CLASS_NO_COPY(PresentUpscaler);

	// Returns true if the active config requests CAS, FSR 1.0, or resolution scaling > 1.0x.
	[[nodiscard]] static bool IsActive() noexcept;

	// Dispatches the Vulkan 1.3 CAS/FSR compute pass from `src_view` into an internal storage
	// image sized to `target_extent`, transitioned to eTransferSrcOptimal for final swapchain blit.
	// Returns the output VulkanImage handle (or nullptr if allocation/dispatch failed).
	[[nodiscard]] vk::Image Dispatch(vk::CommandBuffer command, vk::ImageView src_view,
	                                 vk::Extent2D src_extent, vk::Extent2D target_extent,
	                                 Config::PresentUpscalerMode mode, float sharpness,
	                                 float resolution_scale);

	[[nodiscard]] vk::Extent2D OutputExtent() const noexcept { return m_output_extent; }
	void                       Release();

private:
	bool EnsurePipeline();
	bool EnsureOutputImage(vk::Extent2D extent);

	GraphicContext&         m_graphics;
	vk::Sampler             m_sampler         = nullptr;
	vk::DescriptorSetLayout m_desc_layout     = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
	VulkanImage             m_output_image {};
	vk::ImageView           m_output_view     = nullptr;
	vk::Extent2D            m_output_extent {};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTUPSCALER_H_
