#pragma once
#include "VulkanApi.h"
#include "Core/SpinLock.h"
#include "Memory/RefPtr.hpp"
#include "VulkanDevice.h"
#include "RHI/Types.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanSampler;
	class VulkanImageView;
	class VulkanDescriptorPoolPage;

	using VulkanDescriptorPoolPagePtr = TRefPtr<VulkanDescriptorPoolPage>;

	class VulkanDescriptorSetLayout : public RHI::RHIResource, public RHI::IExplicitInitialization
	{
	public:
		SAILOR_API VulkanDescriptorSetLayout(VulkanDevicePtr pDevice, TVector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings, int32_t variableDescriptorBinding = -1);

		VulkanDescriptorSetLayout() = delete;

		SAILOR_API virtual ~VulkanDescriptorSetLayout() override;

		/// VkDescriptorSetLayoutCreateInfo settings
		TVector<struct VkDescriptorSetLayoutBinding> m_descriptorSetLayoutBindings;

		/// Vulkan VkDescriptorSetLayout handle
		SAILOR_API operator VkDescriptorSetLayout() const { return m_descriptorSetLayout; }

		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

		SAILOR_API size_t GetHash() const;
		SAILOR_API bool operator==(const VulkanDescriptorSetLayout& rhs) const;
		SAILOR_API bool HasVariableDescriptorBinding() const { return m_variableDescriptorBinding != -1; }
		SAILOR_API int32_t GetVariableDescriptorBinding() const { return m_variableDescriptorBinding; }

	protected:

		VulkanDevicePtr m_device{};
		VkDescriptorSetLayout m_descriptorSetLayout{};
		int32_t m_variableDescriptorBinding = -1;
	};

	class VulkanDescriptorPoolPage final : public RHI::RHIResource
	{
	public:
		SAILOR_API VulkanDescriptorPoolPage(VulkanDevicePtr pDevice, VkDescriptorPool descriptorPool);

		SAILOR_API operator VkDescriptorPool() const { return m_descriptorPool; }

	protected:
		SAILOR_API virtual ~VulkanDescriptorPoolPage() override;

		VulkanDevicePtr m_device;
		VkDescriptorPool m_descriptorPool{ VK_NULL_HANDLE };
	};

	class VulkanDescriptorPool : public RHI::RHIResource
	{
	public:
		SAILOR_API VulkanDescriptorPool(VulkanDevicePtr pDevice, uint32_t maxSets, const TVector<VkDescriptorPoolSize>& descriptorPoolSizes);

		SAILOR_API operator VkDescriptorPool() const { return m_currentPage ? static_cast<VkDescriptorPool>(*m_currentPage) : VK_NULL_HANDLE; }
		SAILOR_API VkResult AllocateDescriptorSet(VkDescriptorSetAllocateInfo allocateInfo,
			const TVector<VkDescriptorPoolSize>& descriptorRequirements,
			VkDescriptorSet& outDescriptorSet,
			VulkanDescriptorPoolPagePtr& outPoolPage);

	protected:
		SAILOR_API virtual ~VulkanDescriptorPool();
		SAILOR_API VkResult CreatePool(const TVector<VkDescriptorPoolSize>* descriptorRequirements = nullptr);

		VulkanDescriptorPoolPagePtr m_currentPage;
		VulkanDevicePtr m_device;
		uint32_t m_maxSets = 0;
		TVector<VkDescriptorPoolSize> m_descriptorPoolSizes;
		SpinLock m_lock;
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
		SAILOR_API VulkanImageViewPtr GetImageView() const { return m_imageView; }

	protected:

		VulkanSamplerPtr m_sampler;
		VulkanImageViewPtr m_imageView;
		VkImageLayout m_imageLayout;
		VkDescriptorImageInfo m_imageInfo{};
	};

	class VulkanDescriptorStorageImage : public VulkanDescriptor
	{
	public:
		SAILOR_API VulkanDescriptorStorageImage(uint32_t dstBinding,
			uint32_t dstArrayElement,
			VulkanImageViewPtr imageView,
			VkImageLayout imageLayout = VkImageLayout::VK_IMAGE_LAYOUT_GENERAL);

		SAILOR_API void SetImageView(VulkanImageViewPtr imageView);

		SAILOR_API virtual void Apply(VkWriteDescriptorSet& writeDescriptorSet) const override;
		SAILOR_API VulkanImageViewPtr GetImageView() const { return m_imageView; }

	protected:

		VulkanImageViewPtr m_imageView;
		VkImageLayout m_imageLayout;
		VkDescriptorImageInfo m_imageInfo{};
	};

	class VulkanDescriptorSet : public RHI::RHIResource, public RHI::IExplicitInitialization
	{
	public:
		VulkanDescriptorSet() = delete;

		SAILOR_API VulkanDescriptorSet(VulkanDevicePtr pDevice,
			VulkanDescriptorPoolPtr pool,
			VulkanDescriptorSetLayoutPtr descriptorSetLayout,
			TVector<VulkanDescriptorPtr> descriptors,
			uint32_t variableDescriptorCount = 0);

		/// VkDescriptorSetAllocateInfo settings
		VulkanDescriptorSetLayoutPtr m_setLayout;
		TVector<VulkanDescriptorPtr> m_descriptors;

		SAILOR_API bool UpdateDescriptor(VulkanDescriptorPtr descriptor);
		SAILOR_API bool TryCompile();
		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

		SAILOR_API VkDescriptorSet* GetHandle() { return &m_descriptorSet; }
		SAILOR_API operator VkDescriptorSet() const { return m_descriptorSet; }
		SAILOR_API bool IsCompiled() const { return m_descriptorSet != VK_NULL_HANDLE; }
		SAILOR_API uint32_t GetVariableDescriptorCount() const { return m_variableDescriptorCount; }
		SAILOR_API bool ReferencesImageView(uint32_t binding, uint32_t arrayElement, const VulkanImageViewPtr& imageView) const;

		SAILOR_API const VulkanDescriptorSetLayoutPtr& GetDescriptorSetLayout() const { return m_descriptorSetLayout; }
		SAILOR_API bool LikelyContains(VkDescriptorSetLayoutBinding layout) const;

	protected:
		SAILOR_API ~VulkanDescriptorSet() override;

		static const VkDescriptorSetLayoutBinding* FindLayoutBinding(const VulkanDescriptorSetLayoutPtr& layout, uint32_t binding, VkDescriptorType descriptorType);
		static uint32_t GetEffectiveDescriptorCount(const VulkanDescriptorSetLayoutPtr& layout, const VkDescriptorSetLayoutBinding& layoutBinding, uint32_t variableDescriptorCount);
		static bool ValidateDescriptorWrite(const VulkanDescriptorSetLayoutPtr& layout, const VkWriteDescriptorSet& write, uint32_t variableDescriptorCount, const char* context);

		SAILOR_API void RecalculateCompatibility();

		VulkanDevicePtr m_device{};
		VkDescriptorSet m_descriptorSet{ VK_NULL_HANDLE };
		VulkanDescriptorPoolPtr m_descriptorPool{};
		VulkanDescriptorPoolPagePtr m_descriptorPoolPage{};
		VulkanDescriptorSetLayoutPtr m_descriptorSetLayout{};

		size_t m_compatibilityHashCode = 0;
		uint32_t m_variableDescriptorCount = 0;
	};
}
