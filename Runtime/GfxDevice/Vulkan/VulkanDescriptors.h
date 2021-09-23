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

	class VulkanDescriptorPool : public RHI::RHIResource
	{
	public:
		VulkanDescriptorPool(TRefPtr<VulkanDevice> pDevice, uint32_t maxSets, const std::vector<VkDescriptorPoolSize>& descriptorPoolSizes);

		operator VkDescriptorPool() const { return m_descriptorPool; }
	
	protected:
		virtual ~VulkanDescriptorPool();

		VkDescriptorPool m_descriptorPool;
		TRefPtr<VulkanDevice> m_device;
	};
}
