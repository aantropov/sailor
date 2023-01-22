#include "Cubemap.h"
#include "Texture.h"
#include "RenderTarget.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

RHITexturePtr RHICubemap::GetFace(uint32_t face, uint32_t mipLevel) const
{
	if (!HasMipMaps() && mipLevel > 0)
	{
		return nullptr;
	}

	uint32_t index = mipLevel * 6 + face;

	check(index < m_faces.Num());
	return m_faces[index];
}