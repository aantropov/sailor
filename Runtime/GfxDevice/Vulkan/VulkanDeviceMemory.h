#pragma once
#include <map>
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanBuffer;

	class VulkanDeviceMemory final : public RHI::Resource
	{
	public:
		VulkanDeviceMemory(TRefPtr<class VulkanDevice> device, const VkMemoryRequirements& memRequirements, VkMemoryPropertyFlags properties, void* pNextAllocInfo = nullptr);

		void Copy(VkDeviceSize offset, VkDeviceSize size, const void* src_data);

		operator VkDeviceMemory() const { return m_deviceMemory; }

		const VkMemoryRequirements& GetMemoryRequirements() const { return m_memoryRequirements; }
		const VkMemoryPropertyFlags& GetMemoryPropertyFlags() const { return m_properties; }

		VulkanDevicePtr GetDevice() { return m_device; }

	protected:

		/// Wrapper of vkMapMemory
		VkResult Map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** ppData);
		void Unmap();

		virtual ~VulkanDeviceMemory() override;

		VkDeviceMemory m_deviceMemory;
		VkMemoryRequirements m_memoryRequirements;
		VkMemoryPropertyFlags m_properties;
		VulkanDevicePtr m_device;

		// We keep mapped memory from constructor till destructor
		void* pData = nullptr;

	};
}
