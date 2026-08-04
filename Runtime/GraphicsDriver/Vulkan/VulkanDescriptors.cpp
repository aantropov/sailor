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
	return m_variableDescriptorBinding == rhs.m_variableDescriptorBinding &&
		m_descriptorSetLayoutBindings == rhs.m_descriptorSetLayoutBindings;
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

VulkanDescriptorPoolPage::VulkanDescriptorPoolPage(VulkanDevicePtr pDevice, VkDescriptorPool descriptorPool) :
	m_device(pDevice),
	m_descriptorPool(descriptorPool)
{
}

VulkanDescriptorPoolPage::~VulkanDescriptorPoolPage()
{
	if (m_descriptorPool)
	{
		vkDestroyDescriptorPool(*m_device, m_descriptorPool, nullptr);
		m_descriptorPool = VK_NULL_HANDLE;
	}
}

VulkanDescriptorPool::VulkanDescriptorPool(VulkanDevicePtr pDevice, uint32_t maxSets,
	const TVector<VkDescriptorPoolSize>& descriptorPoolSizes) :
	m_device(pDevice),
	m_maxSets(maxSets),
	m_descriptorPoolSizes(descriptorPoolSizes)
{
	VK_CHECK(CreatePool());
}

VkResult VulkanDescriptorPool::CreatePool(const TVector<VkDescriptorPoolSize>* descriptorRequirements)
{
	if (descriptorRequirements != nullptr)
	{
		for (const VkDescriptorPoolSize& requirement : *descriptorRequirements)
		{
			const uint32_t blockCapacity = std::max(requirement.descriptorCount, m_maxSets);
			const size_t index = m_descriptorPoolSizes.FindIf(
				[&requirement](const VkDescriptorPoolSize& poolSize)
				{
					return poolSize.type == requirement.type;
				});

			if (index == static_cast<size_t>(-1))
			{
				m_descriptorPoolSizes.Add(VkDescriptorPoolSize{
					requirement.type,
					blockCapacity });
			}
			else
			{
				m_descriptorPoolSizes[index].descriptorCount = std::max(
					m_descriptorPoolSizes[index].descriptorCount,
					blockCapacity);
			}
		}
	}

	const TVector<VkDescriptorPoolSize> descriptorPoolSizes = m_descriptorPoolSizes;

	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(descriptorPoolSizes.Num());
	poolInfo.pPoolSizes = descriptorPoolSizes.GetData();
	poolInfo.maxSets = m_maxSets;
	poolInfo.flags = 0;
	if (m_device->IsDescriptorUpdateAfterBindSupported())
	{
		poolInfo.flags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	}
	poolInfo.pNext = nullptr;

	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	const VkResult result = vkCreateDescriptorPool(*m_device, &poolInfo, nullptr, &descriptorPool);
	if (result == VK_SUCCESS)
	{
		m_currentPage = VulkanDescriptorPoolPagePtr::Make(m_device, descriptorPool);
	}

	return result;
}

VkResult VulkanDescriptorPool::AllocateDescriptorSet(VkDescriptorSetAllocateInfo allocateInfo,
	const TVector<VkDescriptorPoolSize>& descriptorRequirements,
	VkDescriptorSet& outDescriptorSet,
	VulkanDescriptorPoolPagePtr& outPoolPage)
{
	m_lock.Lock();

	outDescriptorSet = VK_NULL_HANDLE;
	outPoolPage.Clear();
	VkResult result = m_currentPage ? VK_SUCCESS : CreatePool();
	if (result == VK_SUCCESS)
	{
		allocateInfo.descriptorPool = *m_currentPage;
		result = vkAllocateDescriptorSets(*m_device, &allocateInfo, &outDescriptorSet);
		if (result == VK_SUCCESS)
		{
			outPoolPage = m_currentPage;
		}
	}

	if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL)
	{
		outDescriptorSet = VK_NULL_HANDLE;
		outPoolPage.Clear();
		result = CreatePool(&descriptorRequirements);
		if (result == VK_SUCCESS)
		{
			allocateInfo.descriptorPool = *m_currentPage;
			result = vkAllocateDescriptorSets(*m_device, &allocateInfo, &outDescriptorSet);
			if (result == VK_SUCCESS)
			{
				outPoolPage = m_currentPage;
			}
		}
	}

	m_lock.Unlock();
	return result;
}

VulkanDescriptorPool::~VulkanDescriptorPool()
{
	m_currentPage.Clear();
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

bool VulkanDescriptorSet::ReferencesImageView(uint32_t binding, uint32_t arrayElement, const VulkanImageViewPtr& imageView) const
{
	auto referencesImageView = [&imageView](const VulkanDescriptorPtr& descriptor)
		{
			if (const auto* combinedImage = dynamic_cast<const VulkanDescriptorCombinedImage*>(descriptor.GetRawPtr()))
			{
				return combinedImage->GetImageView() == imageView;
			}

			if (const auto* storageImage = dynamic_cast<const VulkanDescriptorStorageImage*>(descriptor.GetRawPtr()))
			{
				return storageImage->GetImageView() == imageView;
			}

			return false;
		};

	if (arrayElement < m_descriptors.Num())
	{
		const VulkanDescriptorPtr& candidate = m_descriptors[arrayElement];
		if (candidate &&
			candidate->GetBinding() == binding &&
			candidate->GetArrayElement() == arrayElement)
		{
			return referencesImageView(candidate);
		}
	}

	for (const VulkanDescriptorPtr& descriptor : m_descriptors)
	{
		if (!descriptor ||
			descriptor->GetBinding() != binding ||
			descriptor->GetArrayElement() != arrayElement)
		{
			continue;
		}

		return referencesImageView(descriptor);
	}

	return false;
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

bool VulkanDescriptorSet::UpdateDescriptor(VulkanDescriptorPtr descriptor)
{
	if (!m_descriptorSet || !descriptor)
	{
		return false;
	}

	VkWriteDescriptorSet descriptorWrite{};
	descriptor->Apply(descriptorWrite);
	descriptorWrite.dstSet = m_descriptorSet;

	if (!ValidateDescriptorWrite(
		m_descriptorSetLayout,
		descriptorWrite,
		(std::max)(1u, m_variableDescriptorCount),
		"VulkanDescriptorSet::UpdateDescriptor"))
	{
		return false;
	}

	size_t descriptorIndex = static_cast<size_t>(-1);
	if (descriptor->GetArrayElement() < m_descriptors.Num())
	{
		const VulkanDescriptorPtr& candidate = m_descriptors[descriptor->GetArrayElement()];
		if (candidate &&
			candidate->GetBinding() == descriptor->GetBinding() &&
			candidate->GetArrayElement() == descriptor->GetArrayElement() &&
			candidate->GetType() == descriptor->GetType())
		{
			descriptorIndex = descriptor->GetArrayElement();
		}
	}

	if (descriptorIndex == static_cast<size_t>(-1) &&
		!m_descriptors.IsEmpty() &&
		descriptor->GetArrayElement() == m_descriptors.Num() &&
		m_descriptors[m_descriptors.Num() - 1]->GetBinding() == descriptor->GetBinding() &&
		m_descriptors[m_descriptors.Num() - 1]->GetArrayElement() + 1 == descriptor->GetArrayElement() &&
		m_descriptors[m_descriptors.Num() - 1]->GetType() == descriptor->GetType())
	{
		descriptorIndex = m_descriptors.Num();
	}

	if (descriptorIndex == static_cast<size_t>(-1))
	{
		descriptorIndex = m_descriptors.FindIf(
			[&descriptor](const VulkanDescriptorPtr& candidate)
			{
				return candidate &&
					candidate->GetBinding() == descriptor->GetBinding() &&
					candidate->GetArrayElement() == descriptor->GetArrayElement() &&
					candidate->GetType() == descriptor->GetType();
			});
	}

	// UPDATE_UNUSED_WHILE_PENDING only permits changing a slot that pending
	// command buffers do not use. Replacing an existing descriptor requires an
	// immutable replacement set (handled by the caller's fallback path).
	if (descriptorIndex != static_cast<size_t>(-1) && descriptorIndex != m_descriptors.Num())
	{
		return false;
	}

	vkUpdateDescriptorSets(*m_device, 1, &descriptorWrite, 0, nullptr);
	m_descriptors.Emplace(std::move(descriptor));

	return true;
}

void VulkanDescriptorSet::Compile()
{
	TryCompile();
}

bool VulkanDescriptorSet::TryCompile()
{
	uint32_t variableDescriptorCount = std::max(1u, m_variableDescriptorCount);

	if (!m_descriptorSet)
	{
		m_descriptorSetLayout->Compile();

		VkDescriptorSetLayout vkdescriptorSetLayout = *m_descriptorSetLayout;
		if (!vkdescriptorSetLayout || !m_descriptorPool || !m_device)
		{
			SAILOR_LOG_ERROR("VulkanDescriptorSet::TryCompile: descriptor layout, pool, or device is unavailable.");
			return false;
		}

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

		TVector<VkDescriptorPoolSize> descriptorRequirements;
		for (const VkDescriptorSetLayoutBinding& layoutBinding : m_descriptorSetLayout->m_descriptorSetLayoutBindings)
		{
			const uint32_t descriptorCount = GetEffectiveDescriptorCount(
				m_descriptorSetLayout,
				layoutBinding,
				variableDescriptorCount);
			const size_t requirementIndex = descriptorRequirements.FindIf(
				[&layoutBinding](const VkDescriptorPoolSize& requirement)
				{
					return requirement.type == layoutBinding.descriptorType;
				});

			if (requirementIndex == static_cast<size_t>(-1))
			{
				descriptorRequirements.Add(VkDescriptorPoolSize{
					layoutBinding.descriptorType,
					descriptorCount });
			}
			else
			{
				descriptorRequirements[requirementIndex].descriptorCount += descriptorCount;
			}
		}

		VkDescriptorSetAllocateInfo descriptSetAllocateInfo = {};
		descriptSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptSetAllocateInfo.descriptorPool = VK_NULL_HANDLE;
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

		VulkanDescriptorPoolPagePtr descriptorPoolPage;
		const VkResult allocationResult = m_descriptorPool->AllocateDescriptorSet(
			descriptSetAllocateInfo,
			descriptorRequirements,
			m_descriptorSet,
			descriptorPoolPage);
		if (allocationResult != VK_SUCCESS || !m_descriptorSet)
		{
			SAILOR_LOG_ERROR("VulkanDescriptorSet::TryCompile: vkAllocateDescriptorSets failed. result=%d, descriptors=%zu, variableCount=%u",
				static_cast<int32_t>(allocationResult),
				m_descriptors.Num(),
				variableDescriptorCount);
			m_descriptorSet = VK_NULL_HANDLE;
			return false;
		}

		// Keep the exact pool page alive for as long as this descriptor set can be
		// referenced by a recorded or submitted command buffer.
		m_descriptorPoolPage = std::move(descriptorPoolPage);
		m_descriptorPool.Clear();

		if (m_descriptorSetLayout->HasVariableDescriptorBinding())
		{
			// Store the count actually used by vkAllocateDescriptorSets. The
			// requested count may have been clamped to the layout's upper bound.
			m_variableDescriptorCount = variableDescriptorCount;
		}
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

	return true;
}

void VulkanDescriptorSet::Release()
{
	m_descriptorSet = VK_NULL_HANDLE;
	m_descriptors.Clear();
	m_descriptorPoolPage.Clear();
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
	if (bufferType == RHI::EShaderBindingType::StorageBuffer)
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
