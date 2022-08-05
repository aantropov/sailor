#include "Texture.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

glm::ivec2 RHITexture::GetExtent() const
{
	return glm::ivec2(m_vulkan.m_image->m_extent.width, m_vulkan.m_image->m_extent.height);
}

EFormat RHITexture::GetFormat() const
{
	return (EFormat)m_vulkan.m_image->m_format;
}