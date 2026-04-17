#include "Containers/Vector.h"
#include "VulkanApi.h"
#include "VulkanBuffer.h"
#include "VulkanDescriptors.h"
#include "VulkanImageView.h"
#include "VulkanSamplers.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

const VkDescriptorSetLayoutBinding* VulkanDescriptorSet::FindLayoutBinding(const VulkanDescriptorSetLayoutPtr& layout, uint32_t binding, VkDescriptorType descriptorType)
{
	for (const auto& layoutBinding : layout->m_descriptorSetLayoutBindings)
	{
		if (layoutBinding.binding == binding && layoutBinding.descriptorType == descriptorType)
		{
			return &layoutBinding;
		}
	}

	return nullptr;
}

uint32_t VulkanDescriptorSet::GetEffectiveDescriptorCount(const VulkanDescriptorSetLayoutPtr& layout, const VkDescriptorSetLayoutBinding& layoutBinding, uint32_t variableDescriptorCount)
{
	const bool bIsVariableDescriptorBinding = layout->HasVariableDescriptorBinding() &&
		layout->GetVariableDescriptorBinding() == static_cast<int32_t>(layoutBinding.binding);

	if (!bIsVariableDescriptorBinding)
	{
		return layoutBinding.descriptorCount;
	}

	return std::min(layoutBinding.descriptorCount, (std::max)(1u, variableDescriptorCount));
}

bool VulkanDescriptorSet::ValidateDescriptorWrite(const VulkanDescriptorSetLayoutPtr& layout, const VkWriteDescriptorSet& write, uint32_t variableDescriptorCount, const char* context)
{
	const auto* layoutBinding = FindLayoutBinding(layout, write.dstBinding, write.descriptorType);
	if (!layoutBinding)
	{
		SAILOR_LOG_ERROR("%s: missing layout binding for descriptor write. binding=%u, type=%u, count=%u, arrayElement=%u",
			context,
			write.dstBinding,
			static_cast<uint32_t>(write.descriptorType),
			write.descriptorCount,
			write.dstArrayElement);
		return false;
	}

	const uint32_t allowedDescriptorCount = GetEffectiveDescriptorCount(layout, *layoutBinding, variableDescriptorCount);
	if (write.dstArrayElement >= allowedDescriptorCount ||
		write.descriptorCount > allowedDescriptorCount ||
		write.dstArrayElement + write.descriptorCount > allowedDescriptorCount)
	{
		SAILOR_LOG_ERROR("%s: descriptor write exceeds layout. binding=%u, type=%u, count=%u, arrayElement=%u, allowed=%u, layoutCount=%u, variableCount=%u",
			context,
			write.dstBinding,
			static_cast<uint32_t>(write.descriptorType),
			write.descriptorCount,
			write.dstArrayElement,
			allowedDescriptorCount,
			layoutBinding->descriptorCount,
			variableDescriptorCount);
		return false;
	}

	return true;
}

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanDevicePtr pDevice, TVector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings, int32_t variableDescriptorBinding) :
	m_descriptorSetLayoutBindings(std::move(descriptorSetLayoutBindings)),
	m_device(pDevice),
	m_variableDescriptorBinding(variableDescriptorBinding)
{
}

VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout()
{
	VulkanDescriptorSetLayout::Release();
}

bool VulkanDescriptorSetLayout::operator==(const VulkanDescriptorSetLayout& rhs) const
{
	return this->m_descriptorSetLayoutBindings == rhs.m_descriptorSetLayoutBindings;
}

size_t VulkanDescriptorSetLayout::GetHash() const
{
	size_t hash = 0;
	for (const auto& binding : m_descriptorSetLayoutBindings)
	{
		HashCombine(hash, binding.binding, binding.descriptorType);
	}

	return hash;
}

void VulkanDescriptorSetLayout::Compile()
{
	if (m_descriptorSetLayout)
	{
		return;
	}

	VkDescriptorSetLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = (uint32_t)m_descriptorSetLayoutBindings.Num();
	layoutInfo.pBindings = m_descriptorSetLayoutBindings.GetData();

	const bool bHasVariableDescriptorBinding = m_variableDescriptorBinding != -1;
	TVector<VkDescriptorBindingFlags> bindingFlagsStorage;
	VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bindingFlags{};
	const bool bUseUpdateAfterBind = m_device->IsDescriptorUpdateAfterBindSupported();

	if (layoutInfo.bindingCount > 0 && (bUseUpdateAfterBind || bHasVariableDescriptorBinding))
	{
		if (bUseUpdateAfterBind)
		{
			layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
		}

		bindingFlagsStorage.Resize(layoutInfo.bindingCount);
		for (uint32_t i = 0; i < bindingFlagsStorage.Num(); i++)
		{
			VkDescriptorBindingFlags flag =
				VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
				VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

			if (bUseUpdateAfterBind)
			{
				flag |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
			}

			if ((int32_t)m_descriptorSetLayoutBindings[i].binding == m_variableDescriptorBinding)
			{
				flag |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
			}

			bindingFlagsStorage[i] = flag;
		}

		bindingFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT;
		bindingFlags.bindingCount = layoutInfo.bindingCount;
		bindingFlags.pBindingFlags = bindingFlagsStorage.GetData();
		layoutInfo.pNext = &bindingFlags;
	}
	else
	{
		layoutInfo.flags = 0;
		layoutInfo.pNext = nullptr;
	}

	VK_CHECK(vkCreateDescriptorSetLayout(*m_device, &layoutInfo, nullptr, &m_descriptorSetLayout));
}

void VulkanDescriptorSetLayout::Release()
{
	if (m_descriptorSetLayout)
	{
		vkDestroyDescriptorSetLayout(*m_device, m_descriptorSetLayout, nullptr);
		m_descriptorSetLayout = 0;
	}
}

VulkanDescriptorPool::VulkanDescriptorPool(VulkanDevicePtr pDevice, uint32_t maxSets,
	const TVector<VkDescriptorPoolSize>& descriptorPoolSizes) :
	m_device(pDevice)
{
	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(descriptorPoolSizes.Num());
	poolInfo.pPoolSizes = descriptorPoolSizes.GetData();
	poolInfo.maxSets = maxSets;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	if (m_device->IsDescriptorUpdateAfterBindSupported())
	{
		poolInfo.flags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	}
	poolInfo.pNext = nullptr;

	VK_CHECK(vkCreateDescriptorPool(*m_device, &poolInfo, nullptr, &m_descriptorPool));
}

VulkanDescriptorPool::~VulkanDescriptorPool()
{
	if (m_descriptorPool)
	{
		vkDestroyDescriptorPool(*m_device, m_descriptorPool, nullptr);
	}
}

VulkanDescriptorSet::VulkanDescriptorSet(VulkanDevicePtr pDevice,
	VulkanDescriptorPoolPtr pool,
	VulkanDescriptorSetLayoutPtr descriptorSetLayout,
	TVector<VulkanDescriptorPtr> descriptors,
	uint32_t variableDescriptorCount) :
	m_descriptors(std::move(descriptors)),
	m_device(pDevice),
	m_descriptorPool(pool),
	m_descriptorSetLayout(descriptorSetLayout),
	m_variableDescriptorCount(variableDescriptorCount)
{
	RecalculateCompatibility();
}

bool VulkanDescriptorSet::LikelyContains(VkDescriptorSetLayoutBinding layout) const
{
	const auto& hashCode = GetHash(layout.binding >> 4 | layout.descriptorType);
	return (m_compatibilityHashCode & hashCode) == hashCode;
}

void VulkanDescriptorSet::RecalculateCompatibility()
{
	m_compatibilityHashCode = 0;

	for (uint32_t i = 0; i < m_descriptors.Num(); i++)
	{
		const auto& descriptor = m_descriptors[i];
		const auto& hash = GetHash(i >> 4 | descriptor->GetType());
		m_compatibilityHashCode |= hash;
	}
}

void VulkanDescriptorSet::UpdateDescriptor(uint32_t index)
{
	VkWriteDescriptorSet descriptorWrite{};

	m_descriptors[index]->Apply(descriptorWrite);
	descriptorWrite.dstSet = m_descriptorSet;

	const auto* variableLayoutBinding = m_descriptorSetLayout->HasVariableDescriptorBinding() ?
		FindLayoutBinding(m_descriptorSetLayout, static_cast<uint32_t>(m_descriptorSetLayout->GetVariableDescriptorBinding()), descriptorWrite.descriptorType) :
		nullptr;
	const uint32_t variableDescriptorCount = variableLayoutBinding ?
		(std::min)(variableLayoutBinding->descriptorCount, (std::max)(1u, m_variableDescriptorCount)) :
		(std::max)(1u, m_variableDescriptorCount);

#ifdef _DEBUG
	bool bValid = true;
	switch (descriptorWrite.descriptorType)
	{
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		bValid = (descriptorWrite.pImageInfo != nullptr) && (descriptorWrite.pImageInfo->imageView != VK_NULL_HANDLE) && (descriptorWrite.pImageInfo->sampler != VK_NULL_HANDLE);
		break;
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		bValid = (descriptorWrite.pImageInfo != nullptr) && (descriptorWrite.pImageInfo->imageView != VK_NULL_HANDLE);
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		bValid = (descriptorWrite.pBufferInfo != nullptr) && (descriptorWrite.pBufferInfo->buffer != VK_NULL_HANDLE);
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		bValid = (descriptorWrite.pTexelBufferView != nullptr) && (*descriptorWrite.pTexelBufferView != VK_NULL_HANDLE);
		break;
	default:
		break;
	}

	bValid = bValid && ValidateDescriptorWrite(m_descriptorSetLayout, descriptorWrite, variableDescriptorCount, "VulkanDescriptorSet::UpdateDescriptor");
	check(bValid);
#endif

	vkUpdateDescriptorSets(*m_device, 1, &descriptorWrite, 0, nullptr);
}

void VulkanDescriptorSet::Compile()
{
	uint32_t variableDescriptorCount = std::max(1u, m_variableDescriptorCount);

	if (!m_descriptorSet)
	{
		m_descriptorSetLayout->Compile();

		VkDescriptorSetLayout vkdescriptorSetLayout = *m_descriptorSetLayout;
		VkDescriptorSetVariableDescriptorCountAllocateInfo variableDescriptorCountInfo{};
		if (m_descriptorSetLayout->HasVariableDescriptorBinding())
		{
			const int32_t variableBinding = m_descriptorSetLayout->GetVariableDescriptorBinding();
			const auto* layoutBinding = FindLayoutBinding(m_descriptorSetLayout, static_cast<uint32_t>(variableBinding), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			if (!layoutBinding)
			{
				for (const auto& candidate : m_descriptorSetLayout->m_descriptorSetLayoutBindings)
				{
					if (candidate.binding == static_cast<uint32_t>(variableBinding))
					{
						layoutBinding = &candidate;
						break;
					}
				}
			}

			if (layoutBinding && variableDescriptorCount > layoutBinding->descriptorCount)
			{
				SAILOR_LOG_ERROR("VulkanDescriptorSet::Compile: variable descriptor count exceeds layout. requested=%u, layout=%u, binding=%u",
					variableDescriptorCount,
					layoutBinding->descriptorCount,
					layoutBinding->binding);
				variableDescriptorCount = layoutBinding->descriptorCount;
			}
		}

		VkDescriptorSetAllocateInfo descriptSetAllocateInfo = {};
		descriptSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptSetAllocateInfo.descriptorPool = *m_descriptorPool;
		descriptSetAllocateInfo.descriptorSetCount = 1;
		descriptSetAllocateInfo.pSetLayouts = &vkdescriptorSetLayout;
		descriptSetAllocateInfo.pNext = nullptr;

		if (m_descriptorSetLayout->HasVariableDescriptorBinding())
		{
			variableDescriptorCountInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
			variableDescriptorCountInfo.descriptorSetCount = 1;
			variableDescriptorCountInfo.pDescriptorCounts = &variableDescriptorCount;
			descriptSetAllocateInfo.pNext = &variableDescriptorCountInfo;
		}

		VK_CHECK(vkAllocateDescriptorSets(*m_device, &descriptSetAllocateInfo, &m_descriptorSet));

		m_currentThreadId = GetCurrentThreadId();
	}

	TVector<VkWriteDescriptorSet> descriptorsWrite(m_descriptors.Num());

	for (uint32_t i = 0; i < m_descriptors.Num(); i++)
	{
		m_descriptors[i]->Apply(descriptorsWrite[i]);
		descriptorsWrite[i].dstSet = m_descriptorSet;
	}

	TVector<VkWriteDescriptorSet> validWrites;
	validWrites.Reserve(descriptorsWrite.Num());

	for (auto& write : descriptorsWrite)
	{
		bool bValid = true;

		switch (write.descriptorType)
		{
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			bValid = (write.pImageInfo != nullptr) && (write.pImageInfo->imageView != VK_NULL_HANDLE) && (write.pImageInfo->sampler != VK_NULL_HANDLE);
			break;
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			bValid = (write.pImageInfo != nullptr) && (write.pImageInfo->imageView != VK_NULL_HANDLE);
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
			bValid = (write.pBufferInfo != nullptr) && (write.pBufferInfo->buffer != VK_NULL_HANDLE);
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			bValid = (write.pTexelBufferView != nullptr) && (*write.pTexelBufferView != VK_NULL_HANDLE);
			break;
		default:
			break;
		}

		if (bValid)
		{
			bValid = ValidateDescriptorWrite(m_descriptorSetLayout, write, variableDescriptorCount, "VulkanDescriptorSet::Compile");
			if (bValid)
			{
				validWrites.Add(write);
			}
		}
#ifdef _DEBUG
		if (!bValid)
		{
			check(false);
		}
#endif
	}

	RecalculateCompatibility();
	if (validWrites.Num() > 0)
	{
		vkUpdateDescriptorSets(*m_device, static_cast<uint32_t>(validWrites.Num()), validWrites.GetData(), 0, nullptr);
	}
}

void VulkanDescriptorSet::Release()
{
	DWORD currentThreadId = GetCurrentThreadId();

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	if (m_currentThreadId == currentThreadId || !scheduler || !scheduler->HasThread(m_currentThreadId))
	{
		vkFreeDescriptorSets(*m_device, *m_descriptorPool, 1, &m_descriptorSet);
	}
	else
	{
		check(m_descriptorPool.IsValid());
		check(m_device.IsValid());

		auto pReleaseResource = Tasks::CreateTask("Release descriptor set",
			[
				duplicatedPool = std::move(m_descriptorPool),
					duplicatedSet = std::move(m_descriptorSet),
					duplicatedDevice = std::move(m_device)
			]() mutable
			{
				if (duplicatedSet && duplicatedDevice && duplicatedPool)
				{
					vkFreeDescriptorSets(*duplicatedDevice, *duplicatedPool, 1, &duplicatedSet);
				}
			});

				scheduler->Run(pReleaseResource, m_currentThreadId);
	}
}

VulkanDescriptorSet::~VulkanDescriptorSet()
{
	VulkanDescriptorSet::Release();
}

VulkanDescriptor::VulkanDescriptor(uint32_t dstBinding, uint32_t dstArrayElement, VkDescriptorType descriptorType) :
	m_dstBinding(dstBinding),
	m_dstArrayElement(dstArrayElement),
	m_descriptorType(descriptorType)
{}

void VulkanDescriptor::Apply(VkWriteDescriptorSet& writeDescriptorSet) const
{
	memset(&writeDescriptorSet, 0, sizeof(writeDescriptorSet));

	writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writeDescriptorSet.dstBinding = m_dstBinding;
	writeDescriptorSet.dstArrayElement = m_dstArrayElement;
	writeDescriptorSet.descriptorType = m_descriptorType;
}

VulkanDescriptorBuffer::VulkanDescriptorBuffer(uint32_t dstBinding,
	uint32_t dstArrayElement,
	VulkanBufferPtr buffer,
	VkDeviceSize offset,
	VkDeviceSize range,
	RHI::EShaderBindingType bufferType) :
	VulkanDescriptor(dstBinding, dstArrayElement, (VkDescriptorType)bufferType),
	m_buffer(buffer),
	m_offset(offset),
	m_range(range)
{
	// If we're using storage buffer then we can operate with the whole range
	if (const bool bIsStorageBuffer = bufferType == RHI::EShaderBindingType::StorageBuffer)
	{
		m_range = VK_WHOLE_SIZE;
	}

	m_bufferInfo.buffer = *buffer;
	m_bufferInfo.offset = m_offset;
	m_bufferInfo.range = m_range;
}

void VulkanDescriptorBuffer::Apply(VkWriteDescriptorSet& writeDescriptorSet) const
{
	VulkanDescriptor::Apply(writeDescriptorSet);

	writeDescriptorSet.descriptorCount = 1;
	writeDescriptorSet.pBufferInfo = &m_bufferInfo;
	writeDescriptorSet.pImageInfo = nullptr; // Optional
	writeDescriptorSet.pTexelBufferView = nullptr; // Optional
}

VulkanDescriptorCombinedImage::VulkanDescriptorCombinedImage(uint32_t dstBinding,
	uint32_t dstArrayElement,
	VulkanSamplerPtr sampler,
	VulkanImageViewPtr imageView,
	VkImageLayout imageLayout) :
	VulkanDescriptor(dstBinding, dstArrayElement, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
	m_sampler(sampler),
	m_imageView(imageView),
	m_imageLayout(imageLayout)
{
	m_imageInfo.imageLayout = m_imageLayout;
	m_imageInfo.imageView = *m_imageView;
	m_imageInfo.sampler = *m_sampler;
}

void VulkanDescriptorCombinedImage::SetImageView(VulkanImageViewPtr imageView)
{
	m_imageView = imageView;
	m_imageInfo.imageView = m_imageView ? *m_imageView : VK_NULL_HANDLE;
}

void VulkanDescriptorCombinedImage::Apply(VkWriteDescriptorSet& writeDescriptorSet) const
{
	VulkanDescriptor::Apply(writeDescriptorSet);

	writeDescriptorSet.descriptorCount = 1;
	writeDescriptorSet.pImageInfo = &m_imageInfo;
}

VulkanDescriptorStorageImage::VulkanDescriptorStorageImage(uint32_t dstBinding,
	uint32_t dstArrayElement,
	VulkanImageViewPtr imageView,
	VkImageLayout imageLayout) :
	VulkanDescriptor(dstBinding, dstArrayElement, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
	m_imageView(imageView),
	m_imageLayout(imageLayout)
{
	m_imageInfo.imageLayout = m_imageLayout;
	m_imageInfo.imageView = *m_imageView;
	m_imageInfo.sampler = VK_NULL_HANDLE;
}

void VulkanDescriptorStorageImage::SetImageView(VulkanImageViewPtr imageView)
{
	m_imageView = imageView;
	m_imageInfo.imageView = m_imageView ? *m_imageView : VK_NULL_HANDLE;
}

void VulkanDescriptorStorageImage::Apply(VkWriteDescriptorSet& writeDescriptorSet) const
{
	VulkanDescriptor::Apply(writeDescriptorSet);

	writeDescriptorSet.descriptorCount = 1;
	writeDescriptorSet.pImageInfo = &m_imageInfo;
}
