#include <vector>
#include "VulkanApi.h"
#include "VulkanBuffer.h"
#include "VulkanDescriptors.h"
#include "VulkanImageView.h"
#include "VulkanSamplers.h"
#include "AssetRegistry/ShaderCompiler.h"

using namespace Sailor;
using namespace Sailor::GfxDevice::Vulkan;

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(TRefPtr<VulkanDevice> pDevice, std::vector<VkDescriptorSetLayoutBinding> descriptorSetLayoutBindings) :
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
	layoutInfo.bindingCount = (uint32_t)m_descriptorSetLayoutBindings.size();
	layoutInfo.pBindings = m_descriptorSetLayoutBindings.data();

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

VulkanDescriptorPool::VulkanDescriptorPool(TRefPtr<VulkanDevice> pDevice, uint32_t maxSets,
	const std::vector<VkDescriptorPoolSize>& descriptorPoolSizes) :
	m_device(pDevice)
{
	VkDescriptorPoolCreateInfo poolInfo = {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.poolSizeCount = static_cast<uint32_t>(descriptorPoolSizes.size());
	poolInfo.pPoolSizes = descriptorPoolSizes.data();
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

VulkanDescriptorSet::VulkanDescriptorSet(TRefPtr<VulkanDevice> pDevice,
	TRefPtr<VulkanDescriptorPool> pool,
	TRefPtr<VulkanDescriptorSetLayout> descriptorSetLayout,
	std::vector<TRefPtr<VulkanDescriptor>> descriptors) :
	m_device(pDevice),
	m_descriptorPool(pool),
	m_descriptorSetLayout(descriptorSetLayout),
	m_descriptors(std::move(descriptors))
{
}

void VulkanDescriptorSet::Compile()
{
	if (m_descriptorSet)
	{
		return;
	}

	m_descriptorSetLayout->Compile();

	VkDescriptorSetLayout vkdescriptorSetLayout = *m_descriptorSetLayout;

	VkDescriptorSetAllocateInfo descriptSetAllocateInfo = {};
	descriptSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptSetAllocateInfo.descriptorPool = *m_descriptorPool;
	descriptSetAllocateInfo.descriptorSetCount = 1;
	descriptSetAllocateInfo.pSetLayouts = &vkdescriptorSetLayout;

	VK_CHECK(vkAllocateDescriptorSets(*m_device, &descriptSetAllocateInfo, &m_descriptorSet));

	VkWriteDescriptorSet* descriptorsWrite = reinterpret_cast<VkWriteDescriptorSet*>(_malloca(m_descriptors.size() * sizeof(VkWriteDescriptorSet)));

	for (uint32_t i = 0; i < m_descriptors.size(); i++)
	{
		m_descriptors[i]->Apply(descriptorsWrite[i]);
		descriptorsWrite[i].dstSet = m_descriptorSet;
	}

	vkUpdateDescriptorSets(*m_device, static_cast<uint32_t>(m_descriptors.size()), descriptorsWrite, 0, nullptr);

	_freea(descriptorsWrite);
}

void VulkanDescriptorSet::Release()
{
	if (m_descriptorSet)
	{
		vkFreeDescriptorSets(*m_device, *m_descriptorPool, 1, &m_descriptorSet);
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
	TRefPtr<VulkanBuffer> buffer,
	VkDeviceSize offset,
	VkDeviceSize range) :
	m_buffer(buffer),
	m_offset(offset),
	m_range(range),
	VulkanDescriptor(dstBinding, dstArrayElement, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
{
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
	TRefPtr<VulkanSampler> sampler,
	TRefPtr<VulkanImageView> imageView,
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

void VulkanDescriptorImage::Apply(VkWriteDescriptorSet& writeDescriptorSet) const
{
	VulkanDescriptor::Apply(writeDescriptorSet);

	writeDescriptorSet.descriptorCount = 1;
	writeDescriptorSet.pImageInfo = &m_imageInfo;
}