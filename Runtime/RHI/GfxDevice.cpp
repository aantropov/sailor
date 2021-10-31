#include "GfxDevice.h"

using namespace Sailor;
using namespace Sailor::RHI;

TRefPtr<RHI::Mesh> IGfxDevice::CreateMesh(const std::vector<RHI::Vertex>& vertices, const std::vector<uint32_t>& indices)
{
	TRefPtr<RHI::Mesh> res = TRefPtr<RHI::Mesh>::Make();

	const VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();
	const VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.size();

	auto updateVerticesCmd = CreateBuffer(res->m_vertexBuffer,
		reinterpret_cast<void const*>(&vertices[0]),
		bufferSize,
		EBufferUsageBit::VertexBuffer_Bit);

	auto updateIndexCmd = CreateBuffer(res->m_indexBuffer,
		reinterpret_cast<void const*>(&indices[0]),
		indexBufferSize,
		EBufferUsageBit::IndexBuffer_Bit);

	TRefPtr<RHI::Fence> fenceUpdateVertices = TRefPtr<RHI::Fence>::Make();
	SubmitCommandList(updateVerticesCmd, fenceUpdateVertices);

	TRefPtr<RHI::Fence> fenceUpdateIndex= TRefPtr<RHI::Fence>::Make();
	SubmitCommandList(updateIndexCmd, fenceUpdateIndex);

	fenceUpdateVertices->AddObservable(res);
	fenceUpdateIndex->AddObservable(res);

	res->AddVisitor(fenceUpdateVertices);
	res->AddVisitor(fenceUpdateIndex);

	{
		std::unique_lock<std::mutex>(m_mutexTrackedFences);
		m_trackedFences.push_back(fenceUpdateVertices);
		m_trackedFences.push_back(fenceUpdateIndex);
	}

	return res;
}

void IGfxDevice::TraceFences()
{
	std::unique_lock<std::mutex>(m_mutexTrackedFences);

	for (int32_t index = 0; index < m_trackedFences.size(); index++)
	{
		TRefPtr<Fence> cmd = m_trackedFences[index];

		if (cmd->IsFinished())
		{
			cmd->TraceDependencies();
			
			std::iter_swap(m_trackedFences.begin() + index, m_trackedFences.end() - 1);
			m_trackedFences.pop_back();

			index--;
		}
	}
}

