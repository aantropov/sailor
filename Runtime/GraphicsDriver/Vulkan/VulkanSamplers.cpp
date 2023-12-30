#include <algorithm>
#include "VulkanSamplers.h"
#include "VulkanDevice.h"
#include "Memory/RefPtr.hpp"
#include "Math/Math.h"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanSampler::VulkanSampler(VulkanDevicePtr pDevice,
	VkFilter filter,
	VkSamplerAddressMode addressMode,
	bool bUseMips,
	bool bIsAnisotropyEnabled,
	float maxAnisotropy,
	VkSamplerReductionMode reduction) :
	m_device(pDevice)
{
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = filter;
	samplerInfo.minFilter = filter;

	samplerInfo.addressModeU = addressMode;
	samplerInfo.addressModeV = addressMode;
	samplerInfo.addressModeW = addressMode;

	samplerInfo.anisotropyEnable = bIsAnisotropyEnabled;
	samplerInfo.maxAnisotropy = std::min(maxAnisotropy, m_device->GetMaxAllowedAnisotropy());

	samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;

	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;

	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = bUseMips ? 64.0f : 0.0f;

	VkSamplerReductionModeCreateInfoEXT createInfoReduction = {};

	createInfoReduction.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT;
	createInfoReduction.reductionMode = reduction;
	samplerInfo.pNext = &createInfoReduction;

	VK_CHECK(vkCreateSampler(*m_device, &samplerInfo, nullptr, &m_textureSampler));
}

VulkanSampler::~VulkanSampler()
{
	vkDestroySampler(*m_device, m_textureSampler, nullptr);
}

VulkanSamplerCache::VulkanSamplerCache(VulkanDevicePtr pDevice)
{
	for (uint32_t reduction = 0; reduction < 3; reduction++)
	{
		for (uint32_t i = 0; i < 8; i++)
		{
			bool bUseMips = (i & 1);
			VkSamplerAddressMode addressing = (i >> 1) & 1 ? VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_REPEAT : VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			VkFilter filter = (i >> 2) & 1 ? VkFilter::VK_FILTER_NEAREST : VkFilter::VK_FILTER_LINEAR;

			m_samplers[i + reduction * 8] = VulkanSamplerPtr::Make(pDevice,
				filter,
				addressing,
				bUseMips,
				true,
				8.0f,
				(VkSamplerReductionMode)reduction);
		}
	}
}

VulkanSamplerPtr VulkanSamplerCache::GetSampler(RHI::ETextureFiltration filtration, RHI::ETextureClamping clampingMode, bool bHasMipMaps, RHI::ESamplerReductionMode reduction) const
{
	uint8_t index = bHasMipMaps ? 1 : 0;
	index += clampingMode == RHI::ETextureClamping::Repeat ? 2 : 0;
	index += filtration == RHI::ETextureFiltration::Nearest ? 4 : 0;
	index += ((uint32_t)reduction) * 8;

	return m_samplers[index];
}
