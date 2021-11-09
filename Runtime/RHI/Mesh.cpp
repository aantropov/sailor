#include "Mesh.h"
#include "Types.h"
#include "Buffer.h"
#include "Fence.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

bool Mesh::IsReady() const
{
	return m_vertexBuffer->GetSize() > 0 && m_indexBuffer->GetSize() > 0 && m_dependencies.size() == 0;
}
