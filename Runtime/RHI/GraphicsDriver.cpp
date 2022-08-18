#include "GraphicsDriver.h"
#include "Renderer.h"
#include "CommandList.h"
#include "Buffer.h"
#include "Fence.h"
#include "Mesh.h"
#include "VertexDescription.h"
#include "Tasks/Scheduler.h"

using namespace Sailor;
using namespace Sailor::RHI;

void IGraphicsDriver::UpdateMesh(RHI::RHIMeshPtr mesh, const TVector<VertexP3N3UV2C4>& vertices, const TVector<uint32_t>& indices)
{
	const VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.Num();
	const VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.Num();

	RHI::RHICommandListPtr cmdList = CreateCommandList(false, true);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	mesh->m_vertexBuffer = CreateBuffer(cmdList,
		&vertices[0],
		bufferSize,
		EBufferUsageBit::VertexBuffer_Bit);

	mesh->m_indexBuffer = CreateBuffer(cmdList,
		&indices[0],
		indexBufferSize,
		EBufferUsageBit::IndexBuffer_Bit);

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);

	// Create fences to track the state of mesh creation
	RHI::RHIFencePtr fence = RHI::RHIFencePtr::Make();
	TrackDelayedInitialization(mesh.GetRawPtr(), fence);

	// Submit cmd lists
	SAILOR_ENQUEUE_TASK_RENDER_THREAD("Create mesh",
		([this, cmdList, fence]()
	{
		SubmitCommandList(cmdList, fence);
	}));
}

TRefPtr<RHI::RHIMesh> IGraphicsDriver::CreateMesh()
{
	TRefPtr<RHI::RHIMesh> res = TRefPtr<RHI::RHIMesh>::Make();

	return res;
}

void IGraphicsDriver::TrackResources_ThreadSafe()
{
	m_lockTrackedFences.Lock();

	for (int32_t index = 0; index < m_trackedFences.Num(); index++)
	{
		RHIFencePtr fence = m_trackedFences[index];

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
	m_lockTrackedFences.Unlock();
}

void IGraphicsDriver::TrackDelayedInitialization(IDelayedInitialization* pResource, RHIFencePtr handle)
{
	auto resource = dynamic_cast<RHIResource*>(pResource);

	// Fence should notify res, when cmd list is finished
	handle->AddObservable(resource);

	// Res should hold fences to track dependencies
	pResource->AddDependency(handle);
}

void IGraphicsDriver::TrackPendingCommandList_ThreadSafe(RHIFencePtr handle)
{
	m_lockTrackedFences.Lock();

	// We should track fences to handle pending command list
	m_trackedFences.Add(handle);

	m_lockTrackedFences.Unlock();
}

void IGraphicsDriver::SubmitCommandList_Immediate(RHICommandListPtr commandList)
{
	RHIFencePtr fence = RHIFencePtr::Make();
	SubmitCommandList(commandList, fence);
	fence->Wait();
}

RHIVertexDescriptionPtr& IGraphicsDriver::GetOrAddVertexDescription(VertexAttributeBits bits)
{
	auto& pDescription = m_cachedVertexDescriptions.At_Lock(bits);
	if (!pDescription)
	{
		pDescription = RHIVertexDescriptionPtr::Make();
	}
	m_cachedVertexDescriptions.Unlock(bits);

	return pDescription;
}

void IGraphicsDriverCommands::UpdateShaderBindingVariable(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingPtr shaderBinding, const std::string& variable, const void* value, size_t size, uint32_t indexInArray)
{
	SAILOR_PROFILE_FUNCTION();

	// All uniform buffers should be bound
	assert(shaderBinding->IsBind());

	RHI::ShaderLayoutBindingMember bindingLayout;
	assert(shaderBinding->FindVariableInUniformBuffer(variable, bindingLayout));
	assert(bindingLayout.IsArray() || indexInArray == 0);

	UpdateShaderBinding(cmd, shaderBinding, value, size, bindingLayout.m_absoluteOffset + bindingLayout.m_arrayStride * indexInArray);
}
