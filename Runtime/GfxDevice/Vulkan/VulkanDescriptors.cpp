#include <vector>
#include "VulkanApi.h"
#include "VulkanDescriptors.h"

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

VulkanDescriptorSet::VulkanDescriptorSet(TRefPtr<VulkanDevice> pDevice, TRefPtr<VulkanDescriptorPool> pool, TRefPtr<VulkanDescriptorSetLayout> descriptorSetLayout) :
	m_device(pDevice),
	m_descriptorPool(pool),
	m_descriptorSetLayout(descriptorSetLayout)
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

