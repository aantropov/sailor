#include "Core/Vector.h"
#include "VulkanApi.h"
#include "VulkanBuffer.h"
#include "VulkanDescriptors.h"
#include "VulkanImageView.h"
#include "VulkanSamplers.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(VulkanDevicePtr pDevice, TVector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings) :
	m_descriptorSetLayoutBindings(std::move(descriptorSetLayoutBindings)),
	m_device(pDevice)
{
}

VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout()
{
	Release();
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
	layoutInfo.pBindings = m_descriptorSetLayoutBindings.Data();

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
	poolInfo.pPoolSizes = descriptorPoolSizes.Data();
	poolInfo.maxSets = maxSets;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
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
	TVector<VulkanDescriptorPtr> descriptors) :
	m_device(pDevice),
	m_descriptorPool(pool),
	m_descriptorSetLayout(descriptorSetLayout),
	m_descriptors(std::move(descriptors))
{
}

void VulkanDescriptorSet::Compile()
{
	if (!m_descriptorSet)
	{
		m_descriptorSetLayout->Compile();

		VkDescriptorSetLayout vkdescriptorSetLayout = *m_descriptorSetLayout;

		VkDescriptorSetAllocateInfo descriptSetAllocateInfo = {};
		descriptSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptSetAllocateInfo.descriptorPool = *m_descriptorPool;
		descriptSetAllocateInfo.descriptorSetCount = 1;
		descriptSetAllocateInfo.pSetLayouts = &vkdescriptorSetLayout;

		VK_CHECK(vkAllocateDescriptorSets(*m_device, &descriptSetAllocateInfo, &m_descriptorSet));

		m_currentThreadId = GetCurrentThreadId();
	}

	VkWriteDescriptorSet* descriptorsWrite = reinterpret_cast<VkWriteDescriptorSet*>(_malloca(m_descriptors.Num() * sizeof(VkWriteDescriptorSet)));

	for (uint32_t i = 0; i < m_descriptors.Num(); i++)
	{
		m_descriptors[i]->Apply(descriptorsWrite[i]);
		descriptorsWrite[i].dstSet = m_descriptorSet;
	}

	vkUpdateDescriptorSets(*m_device, static_cast<uint32_t>(m_descriptors.Num()), descriptorsWrite, 0, nullptr);

	_freea(descriptorsWrite);
}

void VulkanDescriptorSet::Release()
{
	DWORD currentThreadId = GetCurrentThreadId();

	auto duplicatedDevice = m_device;
	auto duplicatedPool = m_descriptorPool;
	auto duplicatedSet = m_descriptorSet;

	auto pReleaseResource = JobSystem::Scheduler::CreateTask("Release descriptor set", [=]()
		{
			if (m_descriptorSet)
			{
				vkFreeDescriptorSets(*duplicatedDevice, *duplicatedPool, 1, &duplicatedSet);
			}
		});

	if (m_currentThreadId == currentThreadId)
	{
		pReleaseResource->Execute();
		m_device.Clear();
	}
	else
	{
		App::GetSubmodule<JobSystem::Scheduler>()->Run(pReleaseResource, m_currentThreadId);
	}
}

VulkanDescriptorSet::~VulkanDescriptorSet()
{
	Release();
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
	m_buffer(buffer),
	m_offset(offset),
	m_range(range),
	VulkanDescriptor(dstBinding, dstArrayElement, (VkDescriptorType)bufferType)
{
	// If we're using storage buffer then we bind buffer once and operate with instance id
	if (const bool bIsStorageBuffer = bufferType == RHI::EShaderBindingType::StorageBuffer)
	{
		m_offset = 0;
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

VulkanDescriptorImage::VulkanDescriptorImage(uint32_t dstBinding,
	uint32_t dstArrayElement,
	VulkanSamplerPtr sampler,
	VulkanImageViewPtr imageView,
	VkImageLayout imageLayout) :
	m_sampler(sampler),
	m_imageView(imageView),
	m_imageLayout(imageLayout),
	VulkanDescriptor(dstBinding, dstArrayElement, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
{
	m_imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	m_imageInfo.imageView = *m_imageView;
	m_imageInfo.sampler = *m_sampler;
}

void VulkanDescriptorImage::SetImageView(VulkanImageViewPtr imageView)
{
	m_imageView = imageView;
}

void VulkanDescriptorImage::Apply(VkWriteDescriptorSet& writeDescriptorSet) const
{
	VulkanDescriptor::Apply(writeDescriptorSet);

	writeDescriptorSet.descriptorCount = 1;
	writeDescriptorSet.pImageInfo = &m_imageInfo;
}