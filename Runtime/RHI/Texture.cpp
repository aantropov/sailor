#include "Texture.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

glm::ivec2 RHITexture::GetExtent() const
{
	if (!m_vulkan.m_image || !m_vulkan.m_imageView)
	{
		return glm::ivec2(0, 0);
	}

	const uint32_t baseMipLevel = m_vulkan.m_imageView->m_subresourceRange.baseMipLevel;
	return glm::vec2(m_vulkan.m_image->m_extent.width, m_vulkan.m_image->m_extent.height) / powf(2, static_cast<float>(baseMipLevel));
}

EFormat RHITexture::GetFormat() const
{
	return m_vulkan.m_image ? static_cast<EFormat>(m_vulkan.m_image->m_format) : EFormat::UNDEFINED;
}

size_t RHITexture::GetSize() const
{
	return m_vulkan.m_image ? m_vulkan.m_image->GetMemoryRequirements().size : 0;
}

#if defined(SAILOR_BUILD_WITH_VULKAN)
uintptr_t RHITexture::GetNativeDeviceHandle() const
{
	auto image = m_vulkan.m_image;
	auto device = image ? image->GetDevice() : VulkanDevicePtr{};
	return device ? reinterpret_cast<uintptr_t>(static_cast<VkDevice>(*device)) : 0;
}

uintptr_t RHITexture::GetNativeImageHandle() const
{
	auto image = m_vulkan.m_image;
	return image ? reinterpret_cast<uintptr_t>(static_cast<VkImage>(*image)) : 0;
}

uintptr_t RHITexture::GetNativeImageViewHandle() const
{
	auto imageView = m_vulkan.m_imageView;
	return imageView ? reinterpret_cast<uintptr_t>(static_cast<VkImageView>(*imageView)) : 0;
}
#endif