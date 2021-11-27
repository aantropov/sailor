#pragma once
#include "VulkanApi.h"
#include "Memory/RefPtr.hpp"
#include "RHI/Types.h"

using namespace Sailor;
namespace Sailor::GfxDevice::Vulkan
{
	class VulkanDevice;
	class VulkanImage;

	class VulkanImageView : public RHI::Resource, public RHI::IExplicitInitialization
	{
	public:
		VulkanImageView(VulkanDevicePtr device, VulkanImagePtr image);
		VulkanImageView(VulkanDevicePtr device, VulkanImagePtr image, VkImageAspectFlags aspectFlags);

		/// VkImageViewCreateInfo settings
		VkImageViewCreateFlags m_flags = 0;
		VulkanImagePtr m_image;
		VkImageViewType m_viewType = VK_IMAGE_VIEW_TYPE_2D;
		VkFormat m_format = VK_FORMAT_UNDEFINED;
		VkComponentMapping m_components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
		VkImageSubresourceRange m_subresourceRange;

		/// Vulkan VkImageView handle
		operator VkImageView() const { return m_imageView; }

		virtual void Compile() override;
		virtual void Release() override;

		virtual ~VulkanImageView();

	protected:

		VkImageView m_imageView = VK_NULL_HANDLE;
		VulkanDevicePtr m_device;
	};
}
