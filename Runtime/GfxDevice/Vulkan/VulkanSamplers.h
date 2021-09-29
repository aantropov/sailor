#pragma once
#include <array>
#include "VulkanSemaphore.h"
#include "RHI/RHIResource.h"
#include "Core/RefPtr.hpp"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;

	class VulkanSampler : public RHI::RHIResource
	{
	public:

		VulkanSampler(TRefPtr<VulkanDevice> pDevice, VkFilter filter, VkSamplerAddressMode addressMode, bool bUseMips = true, bool bIsAnisotropyEnabled = true, float maxAnisotropy = 8);
		~VulkanSampler();

		operator VkSampler() const { return m_textureSampler; }

	private:

		VkSampler m_textureSampler;
		TRefPtr<VulkanDevice> m_device;
	};

	struct VulkanSamplers
	{
		void Initialize(TRefPtr<VulkanDevice> pDevice);

		TRefPtr<VulkanSampler> GetSampler(RHI::ETextureFiltration filtration, RHI::ETextureClamping clampingMode, bool bHasMipMaps) const;

	private:

		std::array<TRefPtr<VulkanSampler>, 8> m_samplers;
	};
}