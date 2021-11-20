#pragma once
#include "VulkanApi.h"
#include <vulkan/vulkan.h>
#include "RHI/Types.h"
#include "VulkanMemory.h"
#include "Memory/Memory.h"

using namespace Sailor::Memory;
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDeviceMemory;

	class VulkanBuffer final : public RHI::Resource, public RHI::IExplicitInitialization
	{

	public:

		const VkBuffer* GetHandle() const { return &m_buffer; }
		operator VkBuffer() const { return m_buffer; }

		VulkanBuffer(VulkanDevicePtr device, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode);

		/// VkBufferCreateInfo settings
		VkBufferCreateFlags m_flags = 0;
		VkDeviceSize m_size;
		VkBufferUsageFlags m_usage;
		VkSharingMode m_sharingMode;

		virtual void Compile() override;
		virtual void Release() override;

		VkResult Bind(TMemoryPtr<VulkanMemoryPtr> ptr);
		VkResult Bind(VulkanDeviceMemoryPtr deviceMemory, VkDeviceSize memoryOffset);
		VkMemoryRequirements GetMemoryRequirements() const;

		TMemoryPtr<VulkanMemoryPtr> GetMemoryPtr() { return m_ptr; }
		VulkanDeviceMemoryPtr GetMemoryDevice() { return m_deviceMemory; }
		virtual ~VulkanBuffer() override;

	protected:

		VulkanDevicePtr m_device;
		VkBuffer m_buffer;
		VulkanDeviceMemoryPtr m_deviceMemory;
		VkDeviceSize m_memoryOffset = 0;
		TMemoryPtr<VulkanMemoryPtr> m_ptr{};
	};
}
