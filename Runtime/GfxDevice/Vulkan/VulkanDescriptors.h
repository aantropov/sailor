#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "VulkanDevice.h"
#include "RHI/Types.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanSampler;
	class VulkanImageView;

	class VulkanDescriptorSetLayout : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:
		VulkanDescriptorSetLayout(VulkanDevicePtr pDevice, TVector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings);

		VulkanDescriptorSetLayout() = default;
		virtual ~VulkanDescriptorSetLayout() override;

		/// VkDescriptorSetLayoutCreateInfo settings
		TVector<struct VkDescriptorSetLayoutBinding> m_descriptorSetLayoutBindings;

		/// Vulkan VkDescriptorSetLayout handle
		operator VkDescriptorSetLayout() const { return m_descriptorSetLayout; }

		virtual void Compile() override;
		virtual void Release() override;

	protected:

		VulkanDevicePtr m_device{};
		VkDescriptorSetLayout m_descriptorSetLayout{};
	};

	class VulkanDescriptorPool : public RHI::Resource
	{
	public:
		VulkanDescriptorPool(VulkanDevicePtr pDevice, uint32_t maxSets, const TVector<VkDescriptorPoolSize>& descriptorPoolSizes);

		operator VkDescriptorPool() const { return m_descriptorPool; }

	protected:
		virtual ~VulkanDescriptorPool();

		VkDescriptorPool m_descriptorPool;
		VulkanDevicePtr m_device;
	};

	class VulkanDescriptor : public RHI::Resource, public RHI::IStateModifier<VkWriteDescriptorSet>
	{
	public:

		VulkanDescriptor(uint32_t dstBinding, uint32_t dstArrayElement, VkDescriptorType descriptorType);
		virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;

		uint32_t GetBinding() const { return m_dstBinding; }
		uint32_t GetArrayElement() const { return m_dstArrayElement; }

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
			VulkanBufferPtr buffer,
			VkDeviceSize offset = 0,
			VkDeviceSize range = VK_WHOLE_SIZE,
			RHI::EShaderBindingType bufferType = RHI::EShaderBindingType::UniformBuffer);

		virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;

	protected:

		VulkanBufferPtr m_buffer;
		VkDeviceSize m_offset;
		VkDeviceSize m_range;
		VkDescriptorBufferInfo m_bufferInfo;
	};

	class VulkanDescriptorImage : public VulkanDescriptor
	{
	public:
		VulkanDescriptorImage(uint32_t dstBinding,
			uint32_t dstArrayElement,
			VulkanSamplerPtr sampler,
			VulkanImageViewPtr imageView,
			VkImageLayout imageLayout = VkImageLayout::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		void SetImageView(VulkanImageViewPtr imageView);

		virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;

	protected:

		VulkanSamplerPtr m_sampler;
		VulkanImageViewPtr m_imageView;
		VkImageLayout m_imageLayout;
		VkDescriptorImageInfo m_imageInfo{};
	};

	class VulkanDescriptorSet : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:
		VulkanDescriptorSet() = default;
		VulkanDescriptorSet(VulkanDevicePtr pDevice,
			VulkanDescriptorPoolPtr pool,
			VulkanDescriptorSetLayoutPtr descriptorSetLayout,
			TVector<VulkanDescriptorPtr> descriptors);

		/// VkDescriptorSetAllocateInfo settings
		VulkanDescriptorSetLayoutPtr setLayout;
		TVector<VulkanDescriptorPtr> m_descriptors;

		virtual void Compile() override;
		virtual void Release() override;

		VkDescriptorSet* GetHandle() { return &m_descriptorSet; }
		operator VkDescriptorSet() const { return m_descriptorSet; }

	protected:
		~VulkanDescriptorSet() override;

		VulkanDevicePtr m_device{};
		VkDescriptorSet m_descriptorSet{};
		VulkanDescriptorPoolPtr m_descriptorPool{};
		VulkanDescriptorSetLayoutPtr m_descriptorSetLayout{};

		DWORD m_currentThreadId = 0;
	};
}
