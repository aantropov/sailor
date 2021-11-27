#pragma once
#include <array>
#include "VulkanApi.h"
#include "RHI/Types.h"
#include "Memory/RefPtr.hpp"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;

	class VulkanSampler : public RHI::Resource
	{
	public:

		VulkanSampler(VulkanDevicePtr pDevice, VkFilter filter, VkSamplerAddressMode addressMode, bool bUseMips = true, bool bIsAnisotropyEnabled = true, float maxAnisotropy = 8);
		~VulkanSampler();

		operator VkSampler() const { return m_textureSampler; }

	private:

		VkSampler m_textureSampler;
		VulkanDevicePtr m_device;
	};

	struct VulkanSamplerCache
	{
		VulkanSamplerCache(VulkanDevicePtr pDevice);

		VulkanSamplerPtr GetSampler(RHI::ETextureFiltration filtration, RHI::ETextureClamping clampingMode, bool bHasMipMaps) const;

	private:

		std::array<VulkanSamplerPtr, 8> m_samplers;
	};
}