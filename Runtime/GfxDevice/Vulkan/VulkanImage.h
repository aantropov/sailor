#pragma once
#include "VulkanApi.h"
#include "Core/RefPtr.hpp"
#include "RHI/RHIResource.h"

using namespace Sailor;
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;
	class VulkanDeviceMemory;

	class VulkanImage : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:

		VulkanImage(TRefPtr<VulkanDevice> device);
		VulkanImage(VkImage image, TRefPtr<VulkanDevice> device);

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

		VkResult Bind(TRefPtr<VulkanDeviceMemory> deviceMemory, VkDeviceSize memoryOffset);
		VkMemoryRequirements GetMemoryRequirements() const;

		TRefPtr<VulkanDeviceMemory> GetMemoryDevice() { return m_deviceMemory; }
		TRefPtr<VulkanDevice> GetDevice() const { return m_device; }

	protected:

		virtual ~VulkanImage() override;

		VkImage m_image = nullptr;
		TRefPtr<VulkanDevice> m_device;

		TRefPtr<VulkanDeviceMemory> m_deviceMemory;
		VkDeviceSize m_memoryOffset = 0;
		VkDeviceSize m_size = 0;
	};
}
