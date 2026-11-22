#include "graphics/presentation/presentUpscaler.h"

#include "common/logging/log.h"
#include "gpu_blit_shaders/gpu_present_fsr_cas_spv.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics {

std::span<const uint32_t> GetPresentUpscalerSpirv() noexcept {
	return std::span<const uint32_t>(GPU_PRESENT_FSR_CAS_SPV,
	                                 sizeof(GPU_PRESENT_FSR_CAS_SPV) / sizeof(uint32_t));
}

bool CreatePresentUpscalerComputePipeline(vk::Device device, bool use_push_descriptors,
                                          vk::DescriptorSetLayout* out_desc_layout,
                                          vk::PipelineLayout*      out_pipeline_layout,
                                          vk::Pipeline*            out_pipeline) {
	if (device == nullptr || out_desc_layout == nullptr || out_pipeline_layout == nullptr ||
	    out_pipeline == nullptr) {
		return false;
	}

	const std::array<vk::DescriptorSetLayoutBinding, 2> bindings {{
	    {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute},
	    {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
	}};

	vk::DescriptorSetLayoutCreateInfo desc_info {};
	if (use_push_descriptors) {
		desc_info.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	}
	desc_info.bindingCount = static_cast<uint32_t>(bindings.size());
	desc_info.pBindings    = bindings.data();

	vk::DescriptorSetLayout desc_layout = nullptr;
	if (device.createDescriptorSetLayout(&desc_info, nullptr, &desc_layout) != vk::Result::eSuccess) {
		return false;
	}

	const vk::PushConstantRange pc_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                      sizeof(PresentUpscalerPushConstants)};
	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &desc_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &pc_range;

	vk::PipelineLayout pipeline_layout = nullptr;
	if (device.createPipelineLayout(&layout_info, nullptr, &pipeline_layout) != vk::Result::eSuccess) {
		device.destroyDescriptorSetLayout(desc_layout, nullptr);
		return false;
	}

	const auto spv = GetPresentUpscalerSpirv();
	vk::ShaderModuleCreateInfo shader_info {};
	shader_info.codeSize = spv.size_bytes();
	shader_info.pCode    = spv.data();

	vk::ShaderModule shader_module = nullptr;
	if (device.createShaderModule(&shader_info, nullptr, &shader_module) != vk::Result::eSuccess) {
		device.destroyPipelineLayout(pipeline_layout, nullptr);
		device.destroyDescriptorSetLayout(desc_layout, nullptr);
		return false;
	}

	vk::ComputePipelineCreateInfo pipe_info {};
	pipe_info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
	pipe_info.stage.module = shader_module;
	pipe_info.stage.pName  = "main";
	pipe_info.layout       = pipeline_layout;

	vk::Pipeline pipeline = nullptr;
	const auto   res = device.createComputePipelines(nullptr, 1, &pipe_info, nullptr, &pipeline);
	device.destroyShaderModule(shader_module, nullptr);

	if (res != vk::Result::eSuccess) {
		device.destroyPipelineLayout(pipeline_layout, nullptr);
		device.destroyDescriptorSetLayout(desc_layout, nullptr);
		return false;
	}

	*out_desc_layout     = desc_layout;
	*out_pipeline_layout = pipeline_layout;
	*out_pipeline        = pipeline;
	return true;
}

PresentUpscaler::PresentUpscaler(GraphicContext& graphics): m_graphics(graphics) {}

PresentUpscaler::~PresentUpscaler() {
	Release();
}

bool PresentUpscaler::IsActive() noexcept {
	return Config::GetPresentUpscalerMode() != Config::PresentUpscalerMode::Off ||
	       Config::GetResolutionScale() > 1.01f;
}

void PresentUpscaler::Release() {
	auto device = m_graphics.device;
	if (device == nullptr) {
		return;
	}
	if (m_output_view != nullptr) {
		device.destroyImageView(m_output_view, nullptr);
		m_output_view = nullptr;
	}
	if (m_output_image.image != nullptr) {
		m_graphics.DeleteImage(m_output_image);
		m_output_image.image = nullptr;
	}
	if (m_pipeline != nullptr) {
		device.destroyPipeline(m_pipeline, nullptr);
		m_pipeline = nullptr;
	}
	if (m_pipeline_layout != nullptr) {
		device.destroyPipelineLayout(m_pipeline_layout, nullptr);
		m_pipeline_layout = nullptr;
	}
	if (m_desc_layout != nullptr) {
		device.destroyDescriptorSetLayout(m_desc_layout, nullptr);
		m_desc_layout = nullptr;
	}
	if (m_sampler != nullptr) {
		device.destroySampler(m_sampler, nullptr);
		m_sampler = nullptr;
	}
	m_output_extent = {};
}

bool PresentUpscaler::EnsurePipeline() {
	if (m_pipeline != nullptr) {
		return true;
	}
	auto device = m_graphics.device;
	vk::SamplerCreateInfo sampler {};
	sampler.magFilter    = vk::Filter::eLinear;
	sampler.minFilter    = vk::Filter::eLinear;
	sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	if (device.createSampler(&sampler, nullptr, &m_sampler) != vk::Result::eSuccess) {
		return false;
	}
	if (!CreatePresentUpscalerComputePipeline(device, true, &m_desc_layout, &m_pipeline_layout,
	                                          &m_pipeline)) {
		device.destroySampler(m_sampler, nullptr);
		m_sampler = nullptr;
		return false;
	}
	return true;
}

bool PresentUpscaler::EnsureOutputImage(vk::Extent2D extent) {
	if (extent.width == 0 || extent.height == 0) {
		return false;
	}
	if (m_output_image.image != nullptr && m_output_extent == extent) {
		return true;
	}
	auto device = m_graphics.device;
	if (m_output_view != nullptr) {
		device.destroyImageView(m_output_view, nullptr);
		m_output_view = nullptr;
	}
	if (m_output_image.image != nullptr) {
		m_graphics.DeleteImage(m_output_image);
		m_output_image.image = nullptr;
	}

	vk::ImageCreateInfo create {};
	create.imageType     = vk::ImageType::e2D;
	create.extent        = {extent.width, extent.height, 1};
	create.mipLevels     = 1;
	create.arrayLayers   = 1;
	create.format        = vk::Format::eR8G8B8A8Unorm;
	create.tiling        = vk::ImageTiling::eOptimal;
	create.initialLayout = vk::ImageLayout::eUndefined;
	create.usage         = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc;
	create.sharingMode   = vk::SharingMode::eExclusive;
	create.samples       = vk::SampleCountFlagBits::e1;
	if (!m_graphics.CreateImage(create, m_output_image)) {
		return false;
	}

	vk::ImageViewCreateInfo view_info {};
	view_info.image            = m_output_image.image;
	view_info.viewType         = vk::ImageViewType::e2D;
	view_info.format           = vk::Format::eR8G8B8A8Unorm;
	view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	if (device.createImageView(&view_info, nullptr, &m_output_view) != vk::Result::eSuccess) {
		m_graphics.DeleteImage(m_output_image);
		m_output_image.image = nullptr;
		return false;
	}
	m_output_extent = extent;
	return true;
}

vk::Image PresentUpscaler::Dispatch(vk::CommandBuffer command, vk::ImageView src_view,
                                    vk::Extent2D src_extent, vk::Extent2D target_extent,
                                    Config::PresentUpscalerMode mode, float sharpness,
                                    float resolution_scale) {
	if (!EnsurePipeline()) {
		return nullptr;
	}
	const float clamped_scale = std::clamp(resolution_scale, 1.0f, 2.0f);
	const vk::Extent2D scaled_extent {
	    std::max<uint32_t>(
	        1u, std::min<uint32_t>(3840u, static_cast<uint32_t>(
	                                          std::max<float>(static_cast<float>(target_extent.width),
	                                                          static_cast<float>(src_extent.width) *
	                                                              clamped_scale)))),
	    std::max<uint32_t>(
	        1u, std::min<uint32_t>(2160u, static_cast<uint32_t>(
	                                          std::max<float>(static_cast<float>(target_extent.height),
	                                                          static_cast<float>(src_extent.height) *
	                                                              clamped_scale))))};

	if (!EnsureOutputImage(scaled_extent)) {
		return nullptr;
	}

	// Transition output image to GENERAL for compute imageStore.
	vk::ImageMemoryBarrier2 to_general {};
	to_general.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	to_general.srcAccessMask       = vk::AccessFlagBits2::eNone;
	to_general.dstStageMask        = vk::PipelineStageFlagBits2::eComputeShader;
	to_general.dstAccessMask       = vk::AccessFlagBits2::eShaderStorageWrite;
	to_general.oldLayout           = vk::ImageLayout::eUndefined;
	to_general.newLayout           = vk::ImageLayout::eGeneral;
	to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_general.image               = m_output_image.image;
	to_general.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

	vk::DependencyInfo dep_in {};
	dep_in.imageMemoryBarrierCount = 1;
	dep_in.pImageMemoryBarriers    = &to_general;
	command.pipelineBarrier2(dep_in);

	const vk::DescriptorImageInfo src_info {m_sampler, src_view,
	                                        vk::ImageLayout::eShaderReadOnlyOptimal};
	const vk::DescriptorImageInfo dst_info {nullptr, m_output_view, vk::ImageLayout::eGeneral};

	std::array<vk::WriteDescriptorSet, 2> writes {};
	writes[0].dstBinding      = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType  = vk::DescriptorType::eCombinedImageSampler;
	writes[0].pImageInfo      = &src_info;
	writes[1].dstBinding      = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType  = vk::DescriptorType::eStorageImage;
	writes[1].pImageInfo      = &dst_info;

	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0,
	                             static_cast<uint32_t>(writes.size()), writes.data());

	const PresentUpscalerPushConstants pc {
	    src_extent.width,
	    src_extent.height,
	    m_output_extent.width,
	    m_output_extent.height,
	    static_cast<uint32_t>(mode),
	    std::clamp(sharpness, 0.0f, 1.0f),
	    clamped_scale,
	    0u,
	};
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.dispatch((m_output_extent.width + 7u) / 8u, (m_output_extent.height + 7u) / 8u, 1u);

	// Transition output image to TRANSFER_SRC_OPTIMAL for swapchain blit.
	vk::ImageMemoryBarrier2 to_src {};
	to_src.srcStageMask        = vk::PipelineStageFlagBits2::eComputeShader;
	to_src.srcAccessMask       = vk::AccessFlagBits2::eShaderStorageWrite;
	to_src.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	to_src.dstAccessMask       = vk::AccessFlagBits2::eTransferRead;
	to_src.oldLayout           = vk::ImageLayout::eGeneral;
	to_src.newLayout           = vk::ImageLayout::eTransferSrcOptimal;
	to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_src.image               = m_output_image.image;
	to_src.subresourceRange    = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

	vk::DependencyInfo dep_out {};
	dep_out.imageMemoryBarrierCount = 1;
	dep_out.pImageMemoryBarriers    = &to_src;
	command.pipelineBarrier2(dep_out);

	return m_output_image.image;
}

} // namespace Libs::Graphics
