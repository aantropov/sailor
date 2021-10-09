#pragma once
#include <map>
#include "Core/RefPtr.hpp"
#include "VulkanDevice.h"
#include "Memory/Memory.h"

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanBuffer;

	class VulkanDeviceMemory final : public RHI::Resource
	{
	public:
		VulkanDeviceMemory(TRefPtr<VulkanDevice> device, const VkMemoryRequirements& memRequirements, VkMemoryPropertyFlags properties, void* pNextAllocInfo = nullptr);

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

namespace Sailor::Memory
{
	using VulkanDeviceMemoryPtr = TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory>;

	class VulkanStagingAllocator
	{
	protected:

		const VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	public:

		SAILOR_API VulkanDeviceMemoryPtr Allocate(size_t size)
		{
			auto device = Sailor::GfxDevice::Vulkan::VulkanApi::GetInstance()->GetMainDevice();
			auto memoryRequirements = device->GetMemoryRequirements_StagingBuffer();
			memoryRequirements.size = size;

			VulkanDeviceMemoryPtr deviceMemory = (VulkanDeviceMemoryPtr)TRefPtr<Sailor::GfxDevice::Vulkan::VulkanDeviceMemory>::Make(device,
				memoryRequirements,
				memoryProperties);

			return deviceMemory;
		}

		SAILOR_API void Free(VulkanDeviceMemoryPtr pData, size_t size)
		{
			pData.Clear();
		}
	};

	template<typename TDataType, typename TPtrType = VulkanDeviceMemoryPtr>
	void GetAddress(TDataType& pData, VulkanDeviceMemoryPtr& outPtr)
	{
		outPtr = pData.m_ptr;
	}

	template<typename TPtrType = VulkanDeviceMemoryPtr>
	size_t Size(VulkanDeviceMemoryPtr from)
	{
		return from->GetMemoryRequirements().size;
	}

	template<typename TDataType, typename TPtrType = VulkanDeviceMemoryPtr, typename TAllocator = VulkanStagingAllocator>
	TDataType Allocate(size_t size, VulkanStagingAllocator* allocator)
	{
		TDataType newObj{};
		newObj.m_ptr = allocator->Allocate(size);
		return newObj;
	}

	template<typename TDataType, typename TPtrType = VulkanDeviceMemoryPtr, typename TAllocator = VulkanStagingAllocator>
	void Free(TDataType& ptr, VulkanStagingAllocator* allocator)
	{
		allocator->Free(ptr.m_ptr, ptr.m_size);
		ptr.Clear();
	}
}
