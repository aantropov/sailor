#pragma once
#include <map>
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanBuffer;

	class VulkanDeviceMemory final : public RHI::RHIResource
	{
	public:
		SAILOR_API VulkanDeviceMemory(TRefPtr<class VulkanDevice> device, const VkMemoryRequirements& memRequirements, VkMemoryPropertyFlags properties, void* pNextAllocInfo = nullptr);

		SAILOR_API void Copy(VkDeviceSize offset, VkDeviceSize size, const void* src_data);

		SAILOR_API operator VkDeviceMemory() const { return m_deviceMemory; }

		SAILOR_API const VkMemoryRequirements& GetMemoryRequirements() const { return m_memoryRequirements; }
		SAILOR_API const VkMemoryPropertyFlags& GetMemoryPropertyFlags() const { return m_properties; }

		SAILOR_API VulkanDevicePtr GetDevice() { return m_device; }

		SAILOR_API void* GetPointer() { return pData; }

	protected:

		/// Wrapper of vkMapMemory
		SAILOR_API VkResult Map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** ppData);
		SAILOR_API void Unmap();

		SAILOR_API virtual ~VulkanDeviceMemory() override;

		VkDeviceMemory m_deviceMemory;
		VkMemoryRequirements m_memoryRequirements;
		VkMemoryPropertyFlags m_properties;
		VulkanDevicePtr m_device;

		// We keep mapped memory from constructor till destructor
		void* pData = nullptr;

	};
}
