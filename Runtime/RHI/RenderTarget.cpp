#include "RenderTarget.h"
#include "Texture.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

RHITexturePtr RHIRenderTarget::GetMipLayer(uint32_t layer) const
{
	check(layer < m_mipLayers.Num());
	return m_mipLayers[layer];
}