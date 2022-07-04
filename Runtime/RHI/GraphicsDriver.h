#pragma once
#include "AssetRegistry/UID.h"
#include "Types.h"
#include "Engine/Object.h"

#define SAILOR_ENQUEUE_JOB_RENDER_THREAD_CMD(Name, Lambda) \
{ \
auto lambda = Lambda; \
auto submit = [lambda]() \
{ \
	Sailor::RHI::RHICommandListPtr cmdList = Sailor::RHI::Renderer::GetDriver()->CreateCommandList(false, false); \
	Sailor::RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true); \
	lambda(cmdList); \
	Sailor::RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList); \
	Sailor::RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, Sailor::RHI::RHIFencePtr::Make()); \
}; \
Sailor::App::GetSubmodule<Tasks::Scheduler>()->Run(Sailor::Tasks::Scheduler::CreateTask(Name, submit, Sailor::Tasks::EThreadType::Render)); \
}\

#define SAILOR_ENQUEUE_JOB_RENDER_THREAD_TRANSFER_CMD(Name, Lambda) \
{ \
auto lambda = Lambda; \
auto submit = [lambda]() \
{ \
	Sailor::RHI::RHICommandListPtr cmdList = Sailor::RHI::Renderer::GetDriver()->CreateCommandList(false, true); \
	Sailor::RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList, true); \
	lambda(cmdList); \
	Sailor::RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList); \
	Sailor::RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, Sailor::RHI::RHIFencePtr::Make()); \
}; \
Sailor::App::GetSubmodule<Tasks::Scheduler>()->Run(Sailor::Tasks::Scheduler::CreateTask(Name, submit, Sailor::Tasks::EThreadType::Render)); \
}\

namespace Sailor
{
	class FrameState;
	class ShaderSet;

	using ShaderSetPtr = TObjectPtr<class ShaderSet>;
}

namespace Sailor::Win32
{
	class Window;
}

namespace Sailor::RHI
{
	class IGraphicsDriver
	{
	public:

		SAILOR_API virtual void Initialize(const Sailor::Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug) = 0;
		SAILOR_API virtual ~IGraphicsDriver() = default;

		SAILOR_API virtual bool ShouldFixLostDevice(const Sailor::Win32::Window* pViewport) = 0;
		SAILOR_API virtual void FixLostDevice(const Sailor::Win32::Window* pViewport) = 0;

		SAILOR_API virtual bool PresentFrame(const Sailor::FrameState& state,
			const TVector<RHICommandListPtr>* primaryCommandBuffers = nullptr,
			const TVector<RHICommandListPtr>* secondaryCommandBuffers = nullptr,
			TVector<RHISemaphorePtr> waitSemaphores = {}) const = 0;

		SAILOR_API virtual void WaitIdle() = 0;

		SAILOR_API virtual RHISemaphorePtr CreateWaitSemaphore() = 0;
		SAILOR_API virtual RHICommandListPtr CreateCommandList(bool bIsSecondary = false, bool bOnlyTransferQueue = false) = 0;
		SAILOR_API virtual RHIBufferPtr CreateBuffer(size_t size, EBufferUsageFlags usage) = 0;
		SAILOR_API virtual RHIBufferPtr CreateBuffer(RHICommandListPtr& cmdBuffer, const void* pData, size_t size, EBufferUsageFlags usage) = 0;
		SAILOR_API virtual RHIMeshPtr CreateMesh();
		SAILOR_API virtual void UpdateMesh(RHI::RHIMeshPtr mesh, const TVector<VertexP3N3UV2C4>& vertices, const TVector<uint32_t>& indices);
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
			ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit) = 0;

		SAILOR_API virtual RHI::RHITexturePtr CreateRenderTarget(
			glm::ivec3 extent,
			uint32_t mipMapLevel = 1,
			RHI::ETextureType type = RHI::ETextureType::Texture2D,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit) = 0;

		SAILOR_API virtual RHIMaterialPtr CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader) = 0;
		SAILOR_API virtual RHIMaterialPtr CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader, const RHI::RHIShaderBindingSetPtr& shaderBindigs) = 0;

		SAILOR_API virtual void SubmitCommandList(RHICommandListPtr commandList, RHIFencePtr fence = nullptr, RHISemaphorePtr signalSemaphore = nullptr, RHISemaphorePtr waitSemaphore = nullptr) = 0;

		// Shader binding set
		SAILOR_API virtual RHIShaderBindingSetPtr CreateShaderBindings() = 0;
		SAILOR_API virtual void AddBufferToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding, RHI::EShaderBindingType bufferType) = 0;
		SAILOR_API virtual void AddSamplerToShaderBindings(RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::RHITexturePtr texture, uint32_t shaderBinding) = 0;

		// Used for full binding update
		SAILOR_API virtual void UpdateShaderBinding(RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, RHITexturePtr value) = 0;
		SAILOR_API virtual void UpdateShaderBinding_Immediate(RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, const void* value, size_t size) = 0;

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

		//Immediate context
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

		SAILOR_API void TrackResources_ThreadSafe();
		SAILOR_API void TrackDelayedInitialization(IDelayedInitialization* pResource, RHIFencePtr handle);

	protected:

		SAILOR_API void TrackPendingCommandList_ThreadSafe(RHIFencePtr handle);

		SpinLock m_lockTrackedFences;
		TVector<RHIFencePtr> m_trackedFences;

		TConcurrentMap<VertexAttributeBits, RHIVertexDescriptionPtr> m_cachedVertexDescriptions;
	};

	class IGraphicsDriverCommands
	{
	public:

		SAILOR_API virtual bool FitsViewport(RHI::RHICommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2 scissorExtent, float minDepth, float maxDepth) = 0;
		SAILOR_API virtual bool FitsDefaultViewport(RHI::RHICommandListPtr cmd) = 0;

		SAILOR_API virtual void BeginCommandList(RHICommandListPtr cmd, bool bOneTimeSubmit = false) = 0;
		SAILOR_API virtual void EndCommandList(RHICommandListPtr cmd) = 0;

		SAILOR_API virtual void UpdateShaderBindingVariable(RHICommandListPtr cmd, RHI::RHIShaderBindingPtr binding, const std::string& variable, const void* value, size_t size, uint32_t indexInArray);
		SAILOR_API virtual void UpdateShaderBindingVariable(RHICommandListPtr cmd, RHI::RHIShaderBindingPtr binding, const std::string& variable, const void* value, size_t size) = 0;
		SAILOR_API virtual void UpdateShaderBinding(RHICommandListPtr cmd, RHI::RHIShaderBindingPtr binding, const void* data, size_t size, size_t variableOffset = 0) = 0;
		SAILOR_API virtual void UpdateBuffer(RHICommandListPtr cmd, RHI::RHIBufferPtr buffer, const void* data, size_t size, size_t offset = 0) = 0;
		SAILOR_API virtual void SetMaterialParameter(RHICommandListPtr cmd, RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, const std::string& variable, const void* value, size_t size) = 0;

		SAILOR_API virtual void BindMaterial(RHICommandListPtr cmd, RHI::RHIMaterialPtr material) = 0;
		SAILOR_API virtual void BindVertexBuffers(RHICommandListPtr cmd, RHI::RHIBufferPtr vertexBuffer) = 0;
		SAILOR_API virtual void BindIndexBuffer(RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer) = 0;
		SAILOR_API virtual void SetViewport(RHICommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2 scissorExtent, float minDepth, float maxDepth) = 0;
		SAILOR_API virtual void SetDefaultViewport(RHICommandListPtr cmd) = 0;
		SAILOR_API virtual void BindShaderBindings(RHICommandListPtr cmd, RHI::RHIMaterialPtr, const TVector<RHI::RHIShaderBindingSetPtr>& bindings) = 0;
		SAILOR_API virtual void DrawIndexed(RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer, uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, uint32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;

		// Used for variables inside uniform buffer 
		// 'customData.color' would be parsed as 'customData' buffer with 'color' variable
		template<typename TDataType>
		void SetMaterialParameter(RHICommandListPtr cmd, RHI::RHIShaderBindingSetPtr bindings, const std::string& parameter, const TDataType& value)
		{
			std::string outBinding;
			std::string outVariable;

			RHIShaderBindingSet::ParseParameter(parameter, outBinding, outVariable);
			SetMaterialParameter(cmd, bindings, outBinding, outVariable, &value, sizeof(value));
		}

		virtual ~IGraphicsDriverCommands() = default;
	};
};
