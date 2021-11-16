#include "GfxDevice.h"
#include "Renderer.h"
#include "CommandList.h"
#include "Buffer.h"
#include "Fence.h"
#include "Mesh.h"
#include "JobSystem/JobSystem.h"

using namespace Sailor;
using namespace Sailor::RHI;

TRefPtr<RHI::Mesh> IGfxDevice::CreateMesh(const std::vector<RHI::Vertex>& vertices, const std::vector<uint32_t>& indices)
{
	TRefPtr<RHI::Mesh> res = TRefPtr<RHI::Mesh>::Make();

	const VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();
	const VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.size();

	auto updateVerticesCmd = CreateBuffer(res->m_vertexBuffer,
		&vertices[0],
		bufferSize,
		EBufferUsageBit::VertexBuffer_Bit);

	auto updateIndexCmd = CreateBuffer(res->m_indexBuffer,
		&indices[0],
		indexBufferSize,
		EBufferUsageBit::IndexBuffer_Bit);

	// Create fences to track the state of mesh creation
	TRefPtr<RHI::Fence> fenceUpdateVertices = TRefPtr<RHI::Fence>::Make();
	TRefPtr<RHI::Fence> fenceUpdateIndex = TRefPtr<RHI::Fence>::Make();

	// Submit cmd lists
	SAILOR_ENQUEUE_JOB_RENDER_THREAD("Create mesh",
		([this, updateVerticesCmd, fenceUpdateVertices, updateIndexCmd, fenceUpdateIndex]()
			{
				SubmitCommandList(updateVerticesCmd, fenceUpdateVertices);
				SubmitCommandList(updateIndexCmd, fenceUpdateIndex);
			}));

	TrackDelayedInitialization(res.GetRawPtr(), fenceUpdateVertices);
	TrackDelayedInitialization(res.GetRawPtr(), fenceUpdateIndex);

	return res;
}

void IGfxDevice::TrackResources()
{
	std::unique_lock<std::mutex>(m_mutexTrackedFences);

	for (int32_t index = 0; index < m_trackedFences.size(); index++)
	{
		FencePtr fence = m_trackedFences[index];

		if (fence->IsFinished())
		{
			fence->TraceObservables();
			fence->ClearDependencies();
			fence->ClearObservables();

			std::iter_swap(m_trackedFences.begin() + index, m_trackedFences.end() - 1);
			m_trackedFences.pop_back();

			index--;
		}
	}
}

void IGfxDevice::TrackDelayedInitialization(IDelayedInitialization* pResource, FencePtr handle)
{
	auto resource = dynamic_cast<Resource*>(pResource);

	// Fence should notify res, when cmd list is finished
	handle->AddObservable(resource);

	// Res should hold fences to track dependencies
	pResource->AddDependency(handle);

	{
		std::unique_lock<std::mutex>(m_mutexTrackedFences);

		// We should track fences to notity the res that cmd lists are finished
		m_trackedFences.push_back(handle);
	}
}

void IGfxDevice::SubmitCommandList_Immediate(CommandListPtr commandList)
{
	FencePtr fence = FencePtr::Make();
	SubmitCommandList(commandList, fence);
	fence->Wait();
}
