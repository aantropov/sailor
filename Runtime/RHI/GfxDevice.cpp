#include "GfxDevice.h"
#include "Renderer.h"
#include "CommandList.h"
#include "Buffer.h"
#include "Fence.h"
#include "Mesh.h"
#include "JobSystem/JobSystem.h"

using namespace Sailor;
using namespace Sailor::RHI;

void IGfxDevice::UpdateMesh(RHI::MeshPtr mesh, const TVector<Vertex>& vertices, const TVector<uint32_t>& indices)
{
	const VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.Num();
	const VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.Num();

	auto updateVerticesCmd = CreateBuffer(mesh->m_vertexBuffer,
		&vertices[0],
		bufferSize,
		EBufferUsageBit::VertexBuffer_Bit);

	auto updateIndexCmd = CreateBuffer(mesh->m_indexBuffer,
		&indices[0],
		indexBufferSize,
		EBufferUsageBit::IndexBuffer_Bit);

	// Create fences to track the state of mesh creation
	RHI::FencePtr fenceUpdateVertices = RHI::FencePtr::Make();
	RHI::FencePtr fenceUpdateIndex = RHI::FencePtr::Make();

	TrackDelayedInitialization(mesh.GetRawPtr(), fenceUpdateVertices);
	TrackDelayedInitialization(mesh.GetRawPtr(), fenceUpdateIndex);

	// Submit cmd lists
	SAILOR_ENQUEUE_JOB_RENDER_THREAD("Create mesh",
		([this, updateVerticesCmd, fenceUpdateVertices, updateIndexCmd, fenceUpdateIndex]()
			{
				SubmitCommandList(updateVerticesCmd, fenceUpdateVertices);
				SubmitCommandList(updateIndexCmd, fenceUpdateIndex);
			}));
}

TRefPtr<RHI::Mesh> IGfxDevice::CreateMesh()
{
	TRefPtr<RHI::Mesh> res = TRefPtr<RHI::Mesh>::Make();

	return res;
}

void IGfxDevice::TrackResources_ThreadSafe()
{
	std::scoped_lock<std::mutex> guard(m_mutexTrackedFences);

	for (int32_t index = 0; index < m_trackedFences.Num(); index++)
	{
		FencePtr fence = m_trackedFences[index];

		if (fence->IsFinished())
		{
			fence->TraceObservables();
			fence->ClearDependencies();
			fence->ClearObservables();

			std::iter_swap(m_trackedFences.begin() + index, m_trackedFences.end() - 1);
			m_trackedFences.RemoveLast();

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
}

void IGfxDevice::TrackPendingCommandList_ThreadSafe(FencePtr handle)
{
	std::scoped_lock<std::mutex> guard(m_mutexTrackedFences);

	// We should track fences to handle pending command list
	m_trackedFences.Add(handle);
}

void IGfxDevice::SubmitCommandList_Immediate(CommandListPtr commandList)
{
	FencePtr fence = FencePtr::Make();
	SubmitCommandList(commandList, fence);
	fence->Wait();
}

void IGfxDeviceCommands::UpdateShaderBingingVariable(RHI::CommandListPtr cmd, RHI::ShaderBindingPtr shaderBinding, const std::string& variable, const void* value, size_t size, uint32_t indexInArray)
{
	SAILOR_PROFILE_FUNCTION();

	// All uniform buffers should be bound
	assert(shaderBinding->IsBind());

	RHI::ShaderLayoutBindingMember bindingLayout;
	assert(shaderBinding->FindVariableInUniformBuffer(variable, bindingLayout));
	assert(bindingLayout.IsArray() || indexInArray == 0);

	UpdateShaderBinding(cmd, shaderBinding, value, size, bindingLayout.m_absoluteOffset + bindingLayout.m_arrayStride * indexInArray);
}