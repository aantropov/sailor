#include "Texture.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

glm::ivec2 RHITexture::GetExtent() const
{
	uint32_t baseMipLevel = m_vulkan.m_imageView->m_subresourceRange.baseMipLevel;

	return glm::vec2(m_vulkan.m_image->m_extent.width, m_vulkan.m_image->m_extent.height) / powf(2, (float)baseMipLevel);
}

EFormat RHITexture::GetFormat() const
{
	return (EFormat)m_vulkan.m_image->m_format;
}

size_t RHITexture::GetSize() const
{
	return m_vulkan.m_image->GetMemoryRequirements().size;
}