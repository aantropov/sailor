#include "Mesh.h"
#include "Types.h"
#include "Buffer.h"
#include "Fence.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

bool RHIMesh::IsReady() const
{
	return m_vertexBuffer && m_indexBuffer && 
		m_vertexBuffer->GetSize() > 0 && m_indexBuffer->GetSize() > 0 
		&& m_dependencies.Num() == 0;
}
