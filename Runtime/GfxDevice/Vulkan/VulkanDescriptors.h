#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"
#include "RHI/RHIResource.h"

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

	class VulkanDescriptor : public RHI::RHIResource, public RHI::IRHIStateModifier<VkWriteDescriptorSet>
	{
	public:

		VulkanDescriptor(uint32_t dstBinding, uint32_t dstArrayElement, VkDescriptorType descriptorType);
		virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;

	protected:
		uint32_t m_dstBinding;
		uint32_t m_dstArrayElement;
		VkDescriptorType m_descriptorType;
	};

	class VulkanDescriptorBuffer : public VulkanDescriptor
	{
	public:
		VulkanDescriptorBuffer(uint32_t dstBinding,
			uint32_t dstArrayElement,
			TRefPtr<VulkanBuffer> buffer,
			VkDeviceSize offset = 0,
			VkDeviceSize range = VK_WHOLE_SIZE);

		virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;

	protected:

		TRefPtr<VulkanBuffer> m_buffer;
		VkDeviceSize m_offset;
		VkDeviceSize m_range;
		VkDescriptorBufferInfo m_bufferInfo;
	};

	class VulkanDescriptorSet : public RHI::RHIResource
	{
	public:
		VulkanDescriptorSet() = default;
		VulkanDescriptorSet(TRefPtr<VulkanDevice> pDevice,
			TRefPtr<VulkanDescriptorPool> pool,
			TRefPtr<VulkanDescriptorSetLayout> descriptorSetLayout,
			std::vector<TRefPtr<VulkanDescriptor>> descriptors);

		/// VkDescriptorSetAllocateInfo settings
		TRefPtr<VulkanDescriptorSetLayout> setLayout;
		std::vector<TRefPtr<VulkanDescriptor>> m_descriptors;

		void Compile();
		void Release();

		VkDescriptorSet* GetHandle() { return &m_descriptorSet; }
		operator VkDescriptorSet() const { return m_descriptorSet; }

	protected:
		~VulkanDescriptorSet() override;

		TRefPtr<VulkanDevice> m_device{};
		VkDescriptorSet m_descriptorSet{};
		TRefPtr<VulkanDescriptorPool> m_descriptorPool{};
		TRefPtr<VulkanDescriptorSetLayout> m_descriptorSetLayout{};
	};
}
