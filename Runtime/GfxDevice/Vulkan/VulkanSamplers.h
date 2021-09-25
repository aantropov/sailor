#pragma once
#include "VulkanSemaphore.h"
#include "RHI/RHIResource.h"
#include "Core/RefPtr.hpp"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;

	class VulkanSampler : public RHI::RHIResource
	{
	public:

		VulkanSampler(TRefPtr<VulkanDevice> pDevice, VkFilter filter, VkSamplerAddressMode addressMode, bool bAnisotropyEnabled = true, float maxAnisotropy = 8);
		~VulkanSampler();

		operator VkSampler() const { return m_textureSampler; }

	private:

		VkSampler m_textureSampler;
		TRefPtr<VulkanDevice> m_device;
	};
}