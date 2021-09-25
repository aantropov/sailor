#include "VulkanSamplers.h"
#include "VulkanDevice.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanSampler::VulkanSampler(TRefPtr<VulkanDevice> pDevice,
	VkFilter filter,
	VkSamplerAddressMode addressMode,
	bool bIsAnisotropyEnabled,
	float maxAnisotropy) :
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

	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;

	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;

	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;

	VK_CHECK(vkCreateSampler(*m_device, &samplerInfo, nullptr, &m_textureSampler));
}

VulkanSampler::~VulkanSampler()
{
	vkDestroySampler(*m_device, m_textureSampler, nullptr);
}
