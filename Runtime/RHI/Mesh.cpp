#include "Mesh.h"
#include "Types.h"
#include "Buffer.h"
#include "VertexDescription.h"
#include "Fence.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

bool RHIMesh::IsReady() const
{
	return m_vertexBuffer && m_indexBuffer && 
		m_vertexBuffer->GetSize() > 0 && m_indexBuffer->GetSize() > 0 
		&& IDelayedInitialization::IsReady();
}

uint32_t RHIMesh::GetIndexCount() const
{
	return (uint32_t)m_indexBuffer->GetSize() / sizeof(uint32_t);
}

uint32_t RHIMesh::GetFirstIndex() const
{
	return (uint32_t)m_indexBuffer->GetOffset() / sizeof(uint32_t);
}

uint32_t RHIMesh::GetVertexOffset() const
{
	return m_vertexBuffer->GetOffset() / (uint32_t)m_vertexDescription->GetVertexStride();
}

size_t RHIMesh::ResolveMaterialIndex(
	size_t meshIndex,
	size_t numMaterials) const
{
	if (numMaterials == 0)
	{
		return (std::numeric_limits<size_t>::max)();
	}

	return m_materialIndex < numMaterials ?
		static_cast<size_t>(m_materialIndex) :
		(std::min)(meshIndex, numMaterials - 1);
}
