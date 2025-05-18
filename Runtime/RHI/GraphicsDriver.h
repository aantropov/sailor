#pragma once
#include "AssetRegistry/FileId.h"
#include "Types.h"
#include "Engine/Object.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

#define SAILOR_ENQUEUE_TASK_RENDER_THREAD_CMD(Name, Lambda) \
{ \
auto lambda = Lambda; \
auto submit = [lambda]() \
{ \
	Sailor::RHI::RHICommandListPtr cmdList = Sailor::RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics); \
	Sailor::RHI::Renderer::GetDriver()->SetDebugName(cmdList, Name); \
	Sailor::RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true); \
	lambda(cmdList); \
	Sailor::RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList); \
	auto fence = Sailor::RHI::RHIFencePtr::Make(); \
	Sailor::RHI::Renderer::GetDriver()->SetDebugName(fence, Name); \
	Sailor::RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, fence); \
}; \
Sailor::App::GetSubmodule<Tasks::Scheduler>()->Run(Sailor::Tasks::CreateTask(Name, submit, Sailor::EThreadType::Render)); \
}\

#define SAILOR_ENQUEUE_TASK_RENDER_THREAD_TRANSFER_CMD(Name, Lambda) \
{ \
auto lambda = Lambda; \
auto submit = [lambda]() \
{ \
	Sailor::RHI::RHICommandListPtr cmdList = Sailor::RHI::Renderer::GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Transfer); \
	Sailor::RHI::Renderer::GetDriver()->SetDebugName(cmdList, Name); \
	Sailor::RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true); \
	lambda(cmdList); \
	Sailor::RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList); \
	auto fence = Sailor::RHI::RHIFencePtr::Make(); \
	Sailor::RHI::Renderer::GetDriver()->SetDebugName(fence, Name); \
	Sailor::RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, fence); \
}; \
Sailor::App::GetSubmodule<Tasks::Scheduler>()->Run(Sailor::Tasks::CreateTask(Name, submit, Sailor::EThreadType::Render)); \
}\

namespace Sailor
{
	class FrameState;
	class ShaderSet;

	using ShaderSetPtr = TObjectPtr<class ShaderSet>;
}

#include "Platform/Window.h"

namespace Sailor::RHI
{
	class IGraphicsDriver
	{
	public:

               SAILOR_API virtual void Initialize(Platform::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug) = 0;
		SAILOR_API virtual ~IGraphicsDriver() = default;

		SAILOR_API virtual void BeginConditionalDestroy() = 0;

		SAILOR_API virtual void StartGpuTracking() = 0;
		SAILOR_API virtual RHI::GpuStats FinishGpuTracking() = 0;

		SAILOR_API virtual uint32_t GetNumSubmittedCommandBuffers() const = 0;

               SAILOR_API virtual bool ShouldFixLostDevice(const Platform::Window* pViewport) = 0;
               SAILOR_API virtual bool FixLostDevice(Platform::Window* pViewport) = 0;

		SAILOR_API virtual bool AcquireNextImage() = 0;
		SAILOR_API virtual bool PresentFrame(const Sailor::FrameState& state,
			const TVector<RHICommandListPtr>& primaryCommandBuffers = {},
			const TVector<RHISemaphorePtr>& waitSemaphores = {}) const = 0;

		SAILOR_API virtual void SetDebugName(RHIResourcePtr resource, const std::string& name) = 0;
		SAILOR_API virtual void WaitIdle() = 0;

		SAILOR_API virtual RHIRenderTargetPtr GetBackBuffer() const = 0;
		SAILOR_API virtual RHIRenderTargetPtr GetDepthBuffer() const = 0;

		SAILOR_API virtual RHISemaphorePtr CreateWaitSemaphore() = 0;
		SAILOR_API virtual RHICommandListPtr CreateCommandList(bool bIsSecondary = false, RHI::ECommandListQueue queue = RHI::ECommandListQueue::Graphics) = 0;
		SAILOR_API virtual RHIBufferPtr CreateBuffer(size_t size, EBufferUsageFlags usage, RHI::EMemoryPropertyFlags properties = RHI::EMemoryPropertyBit::DeviceLocal) = 0;
		SAILOR_API virtual RHIBufferPtr CreateBuffer(RHICommandListPtr& cmdBuffer, const void* pData, size_t size, EBufferUsageFlags usage, RHI::EMemoryPropertyFlags properties = RHI::EMemoryPropertyBit::DeviceLocal) = 0;
		SAILOR_API virtual RHIBufferPtr CreateIndirectBuffer(size_t size) = 0;

		SAILOR_API virtual RHIMeshPtr CreateMesh();

		SAILOR_API virtual void UpdateMesh(RHI::RHIMeshPtr mesh, const void* pVertices, size_t vertexBuffer, const void* pIndices, size_t indexBuffer);

		SAILOR_API virtual RHIShaderPtr CreateShader(EShaderStage shaderStage, const ShaderByteCode& shaderSpirv) = 0;
		SAILOR_API virtual RHITexturePtr CreateTexture(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			ETextureType type = ETextureType::Texture2D,
			ETextureFormat format = ETextureFormat::R8G8B8A8_SRGB,
			ETextureFiltration filtration = ETextureFiltration::Linear,
			ETextureClamping clamping = ETextureClamping::Clamp,
			ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit,
			RHI::ESamplerReductionMode reduction = RHI::ESamplerReductionMode::Average) = 0;

		SAILOR_API virtual RHI::RHIRenderTargetPtr CreateRenderTarget(
			glm::ivec2 extent,
			uint32_t mipMapLevel = 1,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit,
			RHI::ESamplerReductionMode reduction = RHI::ESamplerReductionMode::Average) = 0;

		SAILOR_API virtual RHI::RHIRenderTargetPtr CreateRenderTarget(
			RHI::RHICommandListPtr cmdList,
			glm::ivec2 extent,
			uint32_t mipMapLevel = 1,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit,
			RHI::ESamplerReductionMode reduction = RHI::ESamplerReductionMode::Average) = 0;

		SAILOR_API virtual RHI::RHISurfacePtr CreateSurface(
			glm::ivec2 extent,
			uint32_t mipMapLevel = 1,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit) = 0;

		SAILOR_API virtual RHI::RHICubemapPtr CreateCubemap(
			glm::ivec2 extent,
			uint32_t mipMapLevel = 1,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit,
			RHI::ESamplerReductionMode reduction = RHI::ESamplerReductionMode::Average) = 0;

		SAILOR_API virtual RHIMaterialPtr CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader) = 0;
		SAILOR_API virtual RHIMaterialPtr CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader, const RHI::RHIShaderBindingSetPtr& shaderBindigs) = 0;

		SAILOR_API virtual void SubmitCommandList(RHICommandListPtr commandList, RHIFencePtr fence = nullptr, RHISemaphorePtr signalSemaphore = nullptr, RHISemaphorePtr waitSemaphore = nullptr) = 0;

		// Shader binding set
		SAILOR_API virtual RHIShaderBindingSetPtr CreateShaderBindings() = 0;
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddBufferToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, RHIBufferPtr buffer, const std::string& name, uint32_t shaderBinding) = 0;
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddSsboToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t elementSize, size_t numElements, uint32_t shaderBinding, bool bBindSsboWithOffset = false) = 0;
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddBufferToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding, RHI::EShaderBindingType bufferType) = 0;
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddSamplerToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::RHITexturePtr texture, uint32_t shaderBinding) = 0;
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddSamplerToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, const TVector<RHI::RHITexturePtr>& array, uint32_t shaderBinding) = 0;
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddStorageImageToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::RHITexturePtr texture, uint32_t shaderBinding) = 0;
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddStorageImageToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, const TVector<RHI::RHITexturePtr>& array, uint32_t shaderBinding) = 0;
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddShaderBinding(RHI::RHIShaderBindingSetPtr& pShaderBindings, const RHI::RHIShaderBindingPtr& binding, const std::string& name, uint32_t shaderBinding) = 0;
		SAILOR_API virtual bool FillShadersLayout(RHI::RHIShaderBindingSetPtr& pShaderBindings, const TVector<RHIShaderPtr>& shaders, uint32_t setNum) = 0;

		// Used for full binding update
		SAILOR_API virtual void UpdateShaderBinding(RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, RHITexturePtr value, uint32_t index = 0) = 0;

		// Used only for static vertex types
		template<typename TVertex>
		SAILOR_API RHIVertexDescriptionPtr& GetOrAddVertexDescription()
		{
			const size_t vertexTypeHash = TVertex::GetVertexAttributeBits();

			auto& pDescription = m_cachedVertexDescriptions.At_Lock(vertexTypeHash);
			if (!pDescription)
			{
				pDescription = RHIVertexDescriptionPtr::Make();
			}
			m_cachedVertexDescriptions.Unlock(vertexTypeHash);

			return pDescription;
		}

		SAILOR_API RHIVertexDescriptionPtr& GetOrAddVertexDescription(VertexAttributeBits bits);

		RHI::RHITexturePtr GetDefaultTexture() { return m_defaultTexture; }

		SAILOR_API virtual RHI::RHIRenderTargetPtr GetOrAddTemporaryRenderTarget(RHI::EFormat textureFormat, glm::ivec2 extent, uint32_t mipLevels);
		SAILOR_API virtual void ReleaseTemporaryRenderTarget(RHI::RHIRenderTargetPtr renderTarget);

		//Immediate context
		SAILOR_API virtual void UpdateShaderBinding_Immediate(RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, const void* value, size_t size) = 0;
		SAILOR_API virtual RHIBufferPtr CreateBuffer_Immediate(const void* pData, size_t size, EBufferUsageFlags usage) = 0;
		SAILOR_API virtual void CopyBuffer_Immediate(RHIBufferPtr src, RHIBufferPtr dst, size_t size) = 0;
		SAILOR_API virtual void SubmitCommandList_Immediate(RHICommandListPtr commandList);

		SAILOR_API virtual RHITexturePtr CreateImage_Immediate(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			ETextureType type = ETextureType::Texture2D,
			ETextureFormat format = ETextureFormat::R8G8B8A8_SRGB,
			ETextureFiltration filtration = ETextureFiltration::Linear,
			ETextureClamping clamping = ETextureClamping::Clamp,
			ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit) = 0;
		//Immediate context

		SAILOR_API virtual void CollectGarbage_RenderThread() = 0;
		SAILOR_API void TrackResources_ThreadSafe();
		SAILOR_API void TrackDelayedInitialization(IDelayedInitialization* pResource, RHIFencePtr handle);

		SAILOR_API virtual RHI::RHITexturePtr GetOrAddMsaaFramebufferRenderTarget(RHI::EFormat textureFormat, glm::ivec2 extent) = 0;

	protected:

		RHI::RHITexturePtr m_defaultTexture{};

		SAILOR_API void TrackPendingCommandList_ThreadSafe(RHIFencePtr handle);

		SpinLock m_lockTrackedFences;
		TVector<RHIFencePtr> m_trackedFences{};

		TConcurrentMap<VertexAttributeBits, RHIVertexDescriptionPtr> m_cachedVertexDescriptions{};
		TConcurrentMap<size_t, TVector<RHI::RHIRenderTargetPtr>> m_temporaryRenderTargets{};
	};

	class IGraphicsDriverCommands
	{
	protected:

		// Copy from RHIShaderBindingSet::ParseParameter to increase compile time
		SAILOR_API void ParseParameter(const std::string& parameter, std::string& outBinding, std::string& outVariable);

	public:

		SAILOR_API virtual bool FitsViewport(RHI::RHICommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2 scissorExtent, float minDepth, float maxDepth) = 0;
		SAILOR_API virtual bool FitsDefaultViewport(RHI::RHICommandListPtr cmd) = 0;

		SAILOR_API virtual void BeginDebugRegion(RHI::RHICommandListPtr cmdList, const std::string& title, const glm::vec4& color) = 0;
		SAILOR_API virtual void EndDebugRegion(RHI::RHICommandListPtr cmdList) = 0;

		SAILOR_API virtual void RenderSecondaryCommandBuffers(RHI::RHICommandListPtr cmd,
			TVector<RHI::RHICommandListPtr> secondaryCmds,
			const TVector<RHI::RHITexturePtr>& colorAttachments,
			RHI::RHITexturePtr depthStencilAttachment,
			glm::ivec4 renderArea,
			glm::ivec2 offset,
			bool bClearRenderTargets,
			glm::vec4 clearColor,
			float clearDepth,
			bool bSupportMultisampling = true,
			bool bStoreDepth = true) = 0;

		SAILOR_API virtual void RenderSecondaryCommandBuffers(RHI::RHICommandListPtr cmd,
			TVector<RHI::RHICommandListPtr> secondaryCmds,
			const TVector<RHI::RHISurfacePtr>& colorAttachments,
			RHI::RHITexturePtr depthStencilAttachment,
			glm::ivec4 renderArea,
			glm::ivec2 offset,
			bool bClearRenderTargets,
			glm::vec4 clearColor,
			float clearDepth,
			bool bStoreDepth = true) = 0;

		SAILOR_API virtual void BeginRenderPass(RHI::RHICommandListPtr cmd,
			const TVector<RHI::RHITexturePtr>& colorAttachments,
			RHI::RHITexturePtr depthStencilAttachment,
			glm::ivec4 renderArea,
			glm::ivec2 offset,
			bool bClearRenderTargets,
			glm::vec4 clearColor,
			float clearDepth,
			bool bSupportMultisampling = true,
			bool bStoreDepth = true) = 0;

		SAILOR_API virtual void BeginRenderPass(RHI::RHICommandListPtr cmd,
			const TVector<RHI::RHISurfacePtr>& colorAttachments,
			RHI::RHITexturePtr depthStencilAttachment,
			glm::ivec4 renderArea,
			glm::ivec2 offset,
			bool bClearRenderTargets,
			glm::vec4 clearColor,
			float clearDepth,
			bool bStoreDepth) = 0;

		SAILOR_API virtual void EndRenderPass(RHI::RHICommandListPtr cmd) = 0;

		SAILOR_API virtual void RestoreImageBarriers(RHI::RHICommandListPtr cmd) = 0;

		SAILOR_API virtual void MemoryBarrier(RHI::RHICommandListPtr cmd, RHI::EAccessFlags srcBit, RHI::EAccessFlags dstBit) = 0;
		SAILOR_API virtual void ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EImageLayout newLayout) = 0;
		SAILOR_API virtual void ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EFormat format, RHI::EImageLayout layout, bool bAllowToWriteFromComputeShader) = 0;
		SAILOR_API virtual void ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EFormat format, RHI::EImageLayout oldLayout, RHI::EImageLayout newLayout) = 0;
		SAILOR_API virtual bool BlitImage(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr src, RHI::RHITexturePtr dst, glm::ivec4 srcRegionRect, glm::ivec4 dstRegionRect, RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear) = 0;
		SAILOR_API virtual void ClearImage(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr dst, const glm::vec4& clearColor) = 0;
		SAILOR_API virtual void ClearDepthStencil(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr dst, float depth = 0.0f, uint32_t stencil = 0) = 0;

		SAILOR_API virtual void BeginSecondaryCommandList(RHICommandListPtr cmd, bool bOneTimeSubmit = false, bool bSupportMultisampling = true, RHI::EFormat colorAttachment = RHI::EFormat::R16G16B16A16_SFLOAT) = 0;
		SAILOR_API virtual void BeginCommandList(RHICommandListPtr cmd, bool bOneTimeSubmit) = 0;
		SAILOR_API virtual void EndCommandList(RHICommandListPtr cmd) = 0;

		SAILOR_API virtual void UpdateShaderBindingVariable(RHICommandListPtr cmd, RHI::RHIShaderBindingPtr binding, const std::string& variable, const void* value, size_t size, uint32_t indexInArray);
		SAILOR_API virtual void UpdateShaderBindingVariable(RHICommandListPtr cmd, RHI::RHIShaderBindingPtr binding, const std::string& variable, const void* value, size_t size) = 0;
		SAILOR_API virtual void UpdateShaderBinding(RHICommandListPtr cmd, RHI::RHIShaderBindingPtr binding, const void* data, size_t size, size_t variableOffset = 0) = 0;
		SAILOR_API virtual void UpdateBuffer(RHICommandListPtr cmd, RHI::RHIBufferPtr buffer, const void* data, size_t size, size_t offset = 0) = 0;
		SAILOR_API virtual void SetMaterialParameter(RHICommandListPtr cmd, RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, const std::string& variable, const void* value, size_t size) = 0;

		SAILOR_API virtual void ExecuteSecondaryCommandList(RHI::RHICommandListPtr cmd, RHI::RHICommandListPtr cmdSecondary) = 0;
		SAILOR_API virtual void BindMaterial(RHICommandListPtr cmd, RHI::RHIMaterialPtr material) = 0;

		SAILOR_API virtual void Dispatch(RHICommandListPtr cmd, RHI::RHIShaderPtr computeShader,
			uint32_t groupSizeX, uint32_t groupSizeY, uint32_t groupSizeZ,
			const TVector<RHI::RHIShaderBindingSetPtr>& bindings,
			const void* pPushConstantsData = nullptr,
			uint32_t sizePushConstantsData = 0) = 0;

		SAILOR_API virtual void BindVertexBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr vertexBuffer, uint32_t offset) = 0;
		SAILOR_API virtual void BindIndexBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer, uint32_t offset, bool bUint16InsteadOfUint32 = false) = 0;

		SAILOR_API virtual void SetViewport(RHICommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2 scissorExtent, float minDepth, float maxDepth) = 0;
		SAILOR_API virtual void SetDefaultViewport(RHICommandListPtr cmd) = 0;
		SAILOR_API virtual void BindShaderBindings(RHICommandListPtr cmd, RHI::RHIMaterialPtr, const TVector<RHI::RHIShaderBindingSetPtr>& bindings) = 0;
		SAILOR_API virtual void DrawIndexed(RHICommandListPtr cmd, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, uint32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;
		SAILOR_API virtual void DrawIndexedIndirect(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr buffer, size_t offset, uint32_t drawCount, uint32_t stride) = 0;
		SAILOR_API virtual void PushConstants(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material, size_t size, const void* ptr) = 0;

		SAILOR_API virtual void GenerateMipMaps(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr target) = 0;
		SAILOR_API virtual void ConvertEquirect2Cubemap(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr equirect, RHI::RHICubemapPtr cubemap) = 0;

		SAILOR_API virtual void CopyBufferToImage(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr src, RHI::RHITexturePtr dst) = 0;
		SAILOR_API virtual void CopyImageToBuffer(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr src, RHI::RHIBufferPtr dst) = 0;

		// Used for variables inside uniform buffer 
		// 'customData.color' would be parsed as 'customData' buffer with 'color' variable
		template<typename TDataType>
		void SetMaterialParameter(RHICommandListPtr cmd, RHI::RHIShaderBindingSetPtr bindings, const std::string& parameter, const TDataType& value)
		{
			std::string outBinding;
			std::string outVariable;

			ParseParameter(parameter, outBinding, outVariable);
			SetMaterialParameter(cmd, bindings, outBinding, outVariable, &value, sizeof(value));
		}

		virtual ~IGraphicsDriverCommands() = default;
	};
};
