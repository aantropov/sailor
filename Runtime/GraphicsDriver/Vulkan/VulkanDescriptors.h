#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "VulkanDevice.h"
#include "RHI/Types.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanSampler;
	class VulkanImageView;

	class VulkanDescriptorSetLayout : public RHI::RHIResource, public RHI::IExplicitInitialization
	{
	public:
		SAILOR_API VulkanDescriptorSetLayout(VulkanDevicePtr pDevice, TVector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings);

		SAILOR_API VulkanDescriptorSetLayout() = default;
		SAILOR_API virtual ~VulkanDescriptorSetLayout() override;

		/// VkDescriptorSetLayoutCreateInfo settings
		TVector<struct VkDescriptorSetLayoutBinding> m_descriptorSetLayoutBindings;

		/// Vulkan VkDescriptorSetLayout handle
		SAILOR_API operator VkDescriptorSetLayout() const { return m_descriptorSetLayout; }

		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

		SAILOR_API size_t GetHash() const;
		SAILOR_API bool operator==(const VulkanDescriptorSetLayout& rhs) const;

	protected:

		VulkanDevicePtr m_device{};
		VkDescriptorSetLayout m_descriptorSetLayout{};
	};

	class VulkanDescriptorPool : public RHI::RHIResource
	{
	public:
		SAILOR_API VulkanDescriptorPool(VulkanDevicePtr pDevice, uint32_t maxSets, const TVector<VkDescriptorPoolSize>& descriptorPoolSizes);

		SAILOR_API operator VkDescriptorPool() const { return m_descriptorPool; }

	protected:
		SAILOR_API virtual ~VulkanDescriptorPool();

		VkDescriptorPool m_descriptorPool;
		VulkanDevicePtr m_device;
	};

	class VulkanDescriptor : public RHI::RHIResource, public RHI::IStateModifier<VkWriteDescriptorSet>
	{
	public:

		SAILOR_API VulkanDescriptor(uint32_t dstBinding, uint32_t dstArrayElement, VkDescriptorType descriptorType);
		SAILOR_API virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;

		SAILOR_API uint32_t GetBinding() const { return m_dstBinding; }
		SAILOR_API uint32_t GetArrayElement() const { return m_dstArrayElement; }
		SAILOR_API VkDescriptorType GetType() const { return m_descriptorType; }

	protected:
		uint32_t m_dstBinding;
		uint32_t m_dstArrayElement;
		VkDescriptorType m_descriptorType;
	};

	class VulkanDescriptorBuffer : public VulkanDescriptor
	{
	public:
		SAILOR_API VulkanDescriptorBuffer(uint32_t dstBinding,
			uint32_t dstArrayElement,
			VulkanBufferPtr buffer,
			VkDeviceSize offset = 0,
			VkDeviceSize range = VK_WHOLE_SIZE,
			RHI::EShaderBindingType bufferType = RHI::EShaderBindingType::UniformBuffer);

		SAILOR_API virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;

	protected:

		VulkanBufferPtr m_buffer;
		VkDeviceSize m_offset;
		VkDeviceSize m_range;
		VkDescriptorBufferInfo m_bufferInfo;
	};

	class VulkanDescriptorCombinedImage : public VulkanDescriptor
	{
	public:
		SAILOR_API VulkanDescriptorCombinedImage(uint32_t dstBinding,
			uint32_t dstArrayElement,
			VulkanSamplerPtr sampler,
			VulkanImageViewPtr imageView,
			VkImageLayout imageLayout = VkImageLayout::VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		SAILOR_API void SetImageView(VulkanImageViewPtr imageView);

		SAILOR_API virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;

	protected:

		VulkanSamplerPtr m_sampler;
		VulkanImageViewPtr m_imageView;
		VkImageLayout m_imageLayout;
		VkDescriptorImageInfo m_imageInfo{};
	};

	class VulkanDescriptorSet : public RHI::RHIResource, public RHI::IExplicitInitialization
	{
	public:
		SAILOR_API VulkanDescriptorSet() = default;
		SAILOR_API VulkanDescriptorSet(VulkanDevicePtr pDevice,
			VulkanDescriptorPoolPtr pool,
			VulkanDescriptorSetLayoutPtr descriptorSetLayout,
			TVector<VulkanDescriptorPtr> descriptors);

		/// VkDescriptorSetAllocateInfo settings
		VulkanDescriptorSetLayoutPtr setLayout;
		TVector<VulkanDescriptorPtr> m_descriptors;

		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

		SAILOR_API VkDescriptorSet* GetHandle() { return &m_descriptorSet; }
		SAILOR_API operator VkDescriptorSet() const { return m_descriptorSet; }

	protected:
		SAILOR_API ~VulkanDescriptorSet() override;

		VulkanDevicePtr m_device{};
		VkDescriptorSet m_descriptorSet{};
		VulkanDescriptorPoolPtr m_descriptorPool{};
		VulkanDescriptorSetLayoutPtr m_descriptorSetLayout{};

		DWORD m_currentThreadId = 0;
	};
}
