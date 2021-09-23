#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDescriptorSetLayout : public RHI::RHIResource
	{
	public:
		VulkanDescriptorSetLayout(TRefPtr<VulkanDevice> pDevice, std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings);

		VulkanDescriptorSetLayout() = default;
		virtual ~VulkanDescriptorSetLayout() override;

		/// VkDescriptorSetLayoutCreateInfo settings
		std::vector<struct VkDescriptorSetLayoutBinding> m_descriptorSetLayoutBindings;

		/// Vulkan VkDescriptorSetLayout handle
		operator VkDescriptorSetLayout() const { return m_descriptorSetLayout; }

		void Compile();
		void Release();

	protected:

		TRefPtr<VulkanDevice> m_device{};
		VkDescriptorSetLayout m_descriptorSetLayout{};
	};

	class VulkanDescriptorSet : public RHI::RHIResource
	{
	public:

	protected:
	};
}
