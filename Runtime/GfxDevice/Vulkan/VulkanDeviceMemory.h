#pragma once
#include <map>
#include <vulkan/vulkan.h>
#include "Core/RefPtr.hpp"
#include "RHI/RHIResource.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanBuffer;

	class VulkanDeviceMemory final : public RHI::Resource
	{
	public:
		VulkanDeviceMemory(TRefPtr<class VulkanDevice> device, const VkMemoryRequirements& memRequirements, VkMemoryPropertyFlags properties, void* pNextAllocInfo = nullptr);

		void Copy(VkDeviceSize offset, VkDeviceSize size, const void* src_data);

		/// Wrapper of vkMapMemory
		VkResult Map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** ppData);
		void Unmap();

		operator VkDeviceMemory() const { return m_deviceMemory; }

		const VkMemoryRequirements& GetMemoryRequirements() const { return m_memoryRequirements; }
		const VkMemoryPropertyFlags& GetMemoryPropertyFlags() const { return m_properties; }

		TRefPtr<VulkanDevice> GetDevice() { return m_device; }

	protected:

		virtual ~VulkanDeviceMemory() override;

		VkDeviceMemory m_deviceMemory;
		VkMemoryRequirements m_memoryRequirements;
		VkMemoryPropertyFlags m_properties;
		TRefPtr<VulkanDevice> m_device;
	};
}
