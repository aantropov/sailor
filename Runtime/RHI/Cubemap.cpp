#include "Cubemap.h"
#include "Texture.h"
#include "RenderTarget.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

RHITexturePtr RHICubemap::GetFace(uint32_t face, uint32_t mipLevel) const
{
	if (!HasMipMaps() && mipLevel > 0)
	{
		return nullptr;
	}

	if (mipLevel == 0)
	{
		return m_faces[face];
	}

	return m_mipLevels[mipLevel - 1]->m_faces[face];
}

RHICubemapPtr RHICubemap::GetMipLevel(uint32_t mipLevel) const
{
	if (!HasMipMaps() && mipLevel > 0)
	{
		return nullptr;
	}

	return m_mipLevels[mipLevel - 1];
}