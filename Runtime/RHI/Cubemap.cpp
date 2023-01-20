#include "Cubemap.h"
#include "Texture.h"
#include "RenderTarget.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

RHIRenderTargetPtr RHICubemap::GetFace(uint32_t face) const
{
	check(face < m_faces.Num());
	return m_faces[face];
}