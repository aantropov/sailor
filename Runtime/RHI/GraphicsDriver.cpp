#include "GraphicsDriver.h"
#include "Renderer.h"
#include "CommandList.h"
#include "Buffer.h"
#include "RenderTarget.h"
#include "Fence.h"
#include "Mesh.h"
#include "VertexDescription.h"
#include "Tasks/Scheduler.h"

using namespace Sailor;
using namespace Sailor::RHI;

void IGraphicsDriver::UpdateMesh(RHI::RHIMeshPtr mesh, const void* pVertices, size_t vertexBuffer, const void* pIndices, size_t indexBuffer)
{
	const VkDeviceSize bufferSize = vertexBuffer;
	const VkDeviceSize indexBufferSize = indexBuffer;

	RHI::RHICommandListPtr cmdList = CreateCommandList(false, RHI::ECommandListQueue::Transfer);
	RHI::Renderer::GetDriver()->SetDebugName(cmdList, "UpdateMesh");
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true);

	// Handle both buffers in memory closier each other
	const EBufferUsageFlags flags = EBufferUsageBit::VertexBuffer_Bit | EBufferUsageBit::IndexBuffer_Bit;

	mesh->m_vertexBuffer = CreateBuffer(cmdList,
		pVertices,
		bufferSize,
		flags);

	mesh->m_indexBuffer = CreateBuffer(cmdList,
		pIndices,
		indexBufferSize,
		flags);

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

RHI::RHIRenderTargetPtr IGraphicsDriver::GetOrAddTemporaryRenderTarget(RHI::EFormat textureFormat, glm::ivec2 extent, uint32_t mipLevels)
{
	size_t hash = (size_t)textureFormat;
	Sailor::HashCombine(hash, extent, mipLevels);

	auto& cachedVector = m_temporaryRenderTargets.At_Lock(hash);

	if (cachedVector.Num() > 0)
	{
		RHI::RHIRenderTargetPtr res = *cachedVector.Last();
		cachedVector.RemoveAt(cachedVector.Num() - 1);
		m_temporaryRenderTargets.Unlock(hash);
		return res;
	}

	m_temporaryRenderTargets.Unlock(hash);

	RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit;

	if (RHI::IsDepthFormat(textureFormat))
	{
		usage |= RHI::ETextureUsageBit::DepthStencilAttachment_Bit;
	}
	else
	{
		usage |= RHI::ETextureUsageBit::ColorAttachment_Bit;
	}

	auto rt = CreateRenderTarget(extent, mipLevels, textureFormat, RHI::ETextureFiltration::Linear, RHI::ETextureClamping::Clamp, usage);
	SetDebugName(rt, "Temporary render target");

	return rt;
}

void IGraphicsDriver::ReleaseTemporaryRenderTarget(RHI::RHIRenderTargetPtr renderTarget)
{
	check(renderTarget && renderTarget.IsValid());

	size_t hash = (size_t)renderTarget->GetFormat();
	const uint32_t mipLevels = renderTarget->GetMipLevels();

	Sailor::HashCombine(hash, renderTarget->GetExtent(), mipLevels);

	auto& cachedVector = m_temporaryRenderTargets.At_Lock(hash);
	cachedVector.Emplace(std::move(renderTarget));
	m_temporaryRenderTargets.Unlock(hash);
}

void IGraphicsDriverCommands::UpdateShaderBindingVariable(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingPtr shaderBinding, const std::string& variable, const void* value, size_t size, uint32_t indexInArray)
{
	SAILOR_PROFILE_FUNCTION();

	// All uniform buffers should be bound
	check(shaderBinding->IsBind());

	RHI::ShaderLayoutBindingMember bindingLayout;
	check(shaderBinding->FindVariableInUniformBuffer(variable, bindingLayout));
	check(bindingLayout.IsArray() || indexInArray == 0);

	UpdateShaderBinding(cmd, shaderBinding, value, size, bindingLayout.m_absoluteOffset + bindingLayout.m_arrayStride * indexInArray);
}