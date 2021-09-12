#pragma once
#include "VulkanApi.h"
#include <vulkan/vulkan.h>

namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDeviceMemory;

	class VulkanBuffer final : public TRefBase
	{

	public:

		const VkBuffer* GetHandle() const { return &m_buffer; }
		operator VkBuffer() const { return m_buffer; }

		VulkanBuffer(VkDeviceSize in_size, VkBufferUsageFlags in_usage, VkSharingMode in_sharingMode);
		VulkanBuffer(TRefPtr<VulkanDevice> device, VkDeviceSize in_size, VkBufferUsageFlags in_usage, VkSharingMode in_sharingMode);

		/// VkBufferCreateInfo settings
		VkBufferCreateFlags m_flags = 0;
		VkDeviceSize m_size;
		VkBufferUsageFlags m_usage;
		VkSharingMode m_sharingMode;

		bool Compile(TRefPtr<VulkanDevice> device);
		VkResult Bind(TRefPtr<VulkanDeviceMemory> deviceMemory, VkDeviceSize memoryOffset);
		VkMemoryRequirements GetMemoryRequirements() const;

		virtual ~VulkanBuffer() override;

	protected:

		TRefPtr<VulkanDevice> m_device;
		VkBuffer m_buffer;
		TRefPtr<VulkanDeviceMemory> m_deviceMemory;
		VkDeviceSize m_memoryOffset = 0;
	};
}