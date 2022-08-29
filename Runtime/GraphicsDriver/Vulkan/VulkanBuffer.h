#pragma once
#include "VulkanApi.h"
#include <vulkan/vulkan.h>
#include "RHI/Types.h"
#include "VulkanMemory.h"
#include "Memory/Memory.h"

using namespace Sailor::Memory;
namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanDeviceMemory;

	class VulkanBuffer final : public RHI::RHIResource, public RHI::IExplicitInitialization
	{

	public:

		SAILOR_API const VkBuffer* GetHandle() const { return &m_buffer; }
		SAILOR_API operator VkBuffer() const { return m_buffer; }

		SAILOR_API VulkanBuffer(VulkanDevicePtr device, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode);

		/// VkBufferCreateInfo settings
		VkBufferCreateFlags m_flags = 0;
		VkDeviceSize m_size;
		VkBufferUsageFlags m_usage;
		VkSharingMode m_sharingMode;

		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

		SAILOR_API VulkanBufferMemoryPtr GetBufferMemoryPtr();

		SAILOR_API VkResult Bind(TMemoryPtr<VulkanMemoryPtr> ptr);
		SAILOR_API VkResult Bind(VulkanDeviceMemoryPtr deviceMemory, VkDeviceSize memoryOffset);
		SAILOR_API VkMemoryRequirements GetMemoryRequirements() const;

		SAILOR_API TMemoryPtr<VulkanMemoryPtr> GetMemoryPtr() { return m_ptr; }
		SAILOR_API VulkanDeviceMemoryPtr GetMemoryDevice() { return m_deviceMemory; }
		SAILOR_API virtual ~VulkanBuffer() override;

	protected:

		VulkanDevicePtr m_device;
		VkBuffer m_buffer;
		VulkanDeviceMemoryPtr m_deviceMemory;
		VkDeviceSize m_memoryOffset = 0;
		TMemoryPtr<VulkanMemoryPtr> m_ptr{};
	};
}
