#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"

using namespace Sailor;
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;
	class VulkanImage : public TRefBase
	{
	public:

		VulkanImage();
		VulkanImage(VkImage image, VkDevice device);

		/// VkImageCreateInfo settings
		VkImageCreateFlags m_flags = 0;
		VkImageType m_imageType = VK_IMAGE_TYPE_2D;
		VkFormat m_format = VK_FORMAT_UNDEFINED;
		VkExtent3D m_extent = { 0, 0, 0 };
		uint32_t m_mipLevels = 0;
		uint32_t m_arrayLayers = 0;
		VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
		VkImageTiling m_tiling = VK_IMAGE_TILING_OPTIMAL;
		VkImageUsageFlags m_usage = 0;
		VkSharingMode m_sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		std::vector<uint32_t> m_queueFamilyIndices;
		VkImageLayout m_initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		operator VkImage() const { return m_vulkanData.m_image; }

		void Compile();
		VkResult Bind(VkDeviceMemory deviceMemory, VkDeviceSize memoryOffset);

	protected:

		virtual ~VulkanImage();

		struct VulkanData
		{
			VkImage m_image = VK_NULL_HANDLE;
			VkDevice m_device;

			VkDeviceMemory m_deviceMemory = 0;
			VkDeviceSize m_memoryOffset = 0;
			VkDeviceSize m_size = 0;

			void Clear();
		};

		VulkanData m_vulkanData;
	};
}
