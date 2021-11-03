#include "GfxDevice.h"
#include "Renderer.h"
#include "CommandList.h"
#include "Buffer.h"
#include "Fence.h"
#include "Mesh.h"

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

	// Create fences to track the state of mesh creation
	TRefPtr<RHI::Fence> fenceUpdateVertices = TRefPtr<RHI::Fence>::Make();
	TRefPtr<RHI::Fence> fenceUpdateIndex = TRefPtr<RHI::Fence>::Make();

	// Submit cmd lists
	auto pJob = JobSystem::Scheduler::GetInstance()->CreateJob("Create mesh",
		[this, updateVerticesCmd, fenceUpdateVertices, updateIndexCmd, fenceUpdateIndex]()
	{
		SubmitCommandList(updateVerticesCmd, fenceUpdateVertices);
		SubmitCommandList(updateIndexCmd, fenceUpdateIndex);
	}, JobSystem::EThreadType::Rendering);

	// Fence should notify mesh, when cmd list is finished
	fenceUpdateVertices->AddObservable(res);
	fenceUpdateIndex->AddObservable(res);

	// Fence should hold cmd list, during it is executed
	fenceUpdateVertices->AddDependency(updateVerticesCmd);
	fenceUpdateIndex->AddDependency(updateIndexCmd);

	// Mesh should hold fences, during cmd list is executed
	res->AddDependency(fenceUpdateVertices);
	res->AddDependency(fenceUpdateIndex);

	{
		std::unique_lock<std::mutex>(m_mutexTrackedFences);

		// We should track fences to notity the mesh that cmd lists are finished
		m_trackedFences.push_back(fenceUpdateVertices);
		m_trackedFences.push_back(fenceUpdateIndex);
	}


	JobSystem::Scheduler::GetInstance()->Run(pJob);
	return res;
}

void IGfxDevice::TrackResources()
{
	std::unique_lock<std::mutex>(m_mutexTrackedFences);

	for (int32_t index = 0; index < m_trackedFences.size(); index++)
	{
		FencePtr cmd = m_trackedFences[index];

		if (cmd->IsFinished())
		{
			cmd->TraceObservables();
			cmd->ClearDependencies();
			cmd->ClearObservables();

			std::iter_swap(m_trackedFences.begin() + index, m_trackedFences.end() - 1);
			m_trackedFences.pop_back();

			index--;
		}
	}
}

