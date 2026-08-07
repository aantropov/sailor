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
	return m_indexCount != (std::numeric_limits<uint32_t>::max)() ?
		m_indexCount :
		(uint32_t)m_indexBuffer->GetSize() / sizeof(uint32_t);
}

uint32_t RHIMesh::GetFirstIndex() const
{
	return m_firstIndex != (std::numeric_limits<uint32_t>::max)() ?
		(uint32_t)m_indexBuffer->GetOffset() / sizeof(uint32_t) + m_firstIndex :
		(uint32_t)m_indexBuffer->GetOffset() / sizeof(uint32_t);
}

uint32_t RHIMesh::GetVertexOffset() const
{
	return m_vertexOffset != (std::numeric_limits<uint32_t>::max)() ?
		m_vertexBuffer->GetOffset() / (uint32_t)m_vertexDescription->GetVertexStride() + m_vertexOffset :
		m_vertexBuffer->GetOffset() / (uint32_t)m_vertexDescription->GetVertexStride();
}

uint32_t RHIMesh::GetNumLods() const
{
	return 1u + static_cast<uint32_t>(m_lods.Num());
}

RHIMeshPtr RHIMesh::GetLod(uint32_t lod) const
{
	if (lod == 0 || m_lods.IsEmpty())
	{
		return {};
	}

	const size_t lodIndex = (std::min)(
		static_cast<size_t>(lod - 1u),
		m_lods.Num() - 1u);
	return m_lods[lodIndex];
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
