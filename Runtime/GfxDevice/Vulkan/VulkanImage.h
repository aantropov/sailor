#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "RHI/Types.h"
#include "VulkanMemory.h"
#include "Memory/Memory.h"

using namespace Sailor;
using namespace Sailor::Memory;
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;
	class VulkanDeviceMemory;

	class VulkanImage : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:

		VulkanImage(VulkanDevicePtr device);
		VulkanImage(VkImage image, VulkanDevicePtr device);

		/// VkImageCreateInfo settings
		VkImageCreateFlags m_flags = 0;
		VkImageType m_imageType = VK_IMAGE_TYPE_2D;
		VkFormat m_format = VK_FORMAT_R8G8B8A8_SRGB;
		VkExtent3D m_extent = { 0, 0, 0 };
		uint32_t m_mipLevels = 0;
		uint32_t m_arrayLayers = 0;
		VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
		VkImageTiling m_tiling = VK_IMAGE_TILING_OPTIMAL;
		VkImageUsageFlags m_usage = 0;
		VkSharingMode m_sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		std::vector<uint32_t> m_queueFamilyIndices;
		VkImageLayout m_initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		operator VkImage() const { return m_image; }

		virtual void Compile() override;
		virtual void Release() override;

		VkResult Bind(TMemoryPtr<VulkanMemoryPtr> ptr);
		VkResult Bind(VulkanDeviceMemoryPtr deviceMemory, VkDeviceSize memoryOffset);
		VkMemoryRequirements GetMemoryRequirements() const;

		VulkanDeviceMemoryPtr GetMemoryDevice() { return m_deviceMemory; }
		VulkanDevicePtr GetDevice() const { return m_device; }

	protected:

		virtual ~VulkanImage() override;

		VkImage m_image = nullptr;
		VulkanDevicePtr m_device;

		VulkanDeviceMemoryPtr m_deviceMemory;
		VkDeviceSize m_memoryOffset = 0;
		VkDeviceSize m_size = 0;
		TMemoryPtr<VulkanMemoryPtr> m_ptr{};
	};
}
