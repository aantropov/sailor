#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

using namespace Sailor;
namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanDevice;
	class VulkanImage;

	class VulkanImageView : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:
		SAILOR_API VulkanImageView(VulkanDevicePtr device, VulkanImagePtr image);
		SAILOR_API VulkanImageView(VulkanDevicePtr device, VulkanImagePtr image, VkImageAspectFlags aspectFlags);

		/// VkImageViewCreateInfo settings
		VkImageViewCreateFlags m_flags = 0;
		VulkanImagePtr m_image;
		VkImageViewType m_viewType = VK_IMAGE_VIEW_TYPE_2D;
		VkFormat m_format = VK_FORMAT_UNDEFINED;
		VkComponentMapping m_components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
		VkImageSubresourceRange m_subresourceRange;

		/// Vulkan VkImageView handle
		SAILOR_API operator VkImageView() const { return m_imageView; }

		SAILOR_API virtual void Compile() override;
		SAILOR_API virtual void Release() override;

		SAILOR_API virtual ~VulkanImageView();

	protected:

		VkImageView m_imageView = VK_NULL_HANDLE;
		VulkanDevicePtr m_device;
	};
}
