#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Memory/MemoryPoolAllocator.hpp"
#include "RHI/Types.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/Texture.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/Shader.h"
#include "RHI/Material.h"
#include "GraphicsDriver/Vulkan/VulkanMemory.h"
#include "GraphicsDriver/Vulkan/VulkanBufferMemory.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "Platform/Win32/Window.h"
#include "Containers/ConcurrentMap.h"

#ifdef SAILOR_BUILD_WITH_VULKAN

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanGraphicsDriver : public RHI::IGraphicsDriver, public RHI::IGraphicsDriverCommands
	{
	public:

		SAILOR_API virtual void Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		SAILOR_API virtual ~VulkanGraphicsDriver() override;

		SAILOR_API virtual uint32_t GetNumSubmittedCommandBuffers() const;

		SAILOR_API virtual bool ShouldFixLostDevice(const Win32::Window* pViewport);
		SAILOR_API virtual bool FixLostDevice(const Win32::Window* pViewport);

		SAILOR_API virtual bool AcquireNextImage();
		SAILOR_API virtual bool PresentFrame(const class FrameState& state,	const TVector<RHI::RHICommandListPtr>& primaryCommandBuffers, const TVector<RHI::RHISemaphorePtr>& waitSemaphores) const;

		SAILOR_API virtual void WaitIdle();
		SAILOR_API virtual RHI::RHITexturePtr GetBackBuffer() const;
		SAILOR_API virtual RHI::RHITexturePtr GetDepthBuffer() const;

		SAILOR_API virtual RHI::RHISemaphorePtr CreateWaitSemaphore();
		SAILOR_API virtual RHI::RHICommandListPtr CreateCommandList(bool bIsSecondary = false, bool bOnlyTransferQueue = false);
		SAILOR_API virtual RHI::RHIBufferPtr CreateBuffer(size_t size, RHI::EBufferUsageFlags usage);
		SAILOR_API virtual RHI::RHIBufferPtr CreateBuffer(RHI::RHICommandListPtr& cmdBuffer, const void* pData, size_t size, RHI::EBufferUsageFlags usage);
		SAILOR_API virtual RHI::RHIShaderPtr CreateShader(RHI::EShaderStage shaderStage, const RHI::ShaderByteCode& shaderSpirv);
		SAILOR_API virtual RHI::RHITexturePtr CreateTexture(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			RHI::ETextureType type = RHI::ETextureType::Texture2D,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit);

		SAILOR_API virtual RHI::RHITexturePtr CreateRenderTarget(
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			RHI::ETextureType type = RHI::ETextureType::Texture2D,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit);

		SAILOR_API virtual RHI::RHISurfacePtr CreateSurface(
			glm::ivec3 extent,
			uint32_t mipMapLevel = 1,
			RHI::ETextureType type = RHI::ETextureType::Texture2D,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::ColorAttachment_Bit | RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit);

		SAILOR_API virtual RHI::RHIMaterialPtr CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader);
		SAILOR_API virtual RHI::RHIMaterialPtr CreateMaterial(const RHI::RHIVertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader, const RHI::RHIShaderBindingSetPtr& shaderBindigs);
		
		SAILOR_API virtual void UpdateMesh(RHI::RHIMeshPtr mesh, const TVector<RHI::VertexP3N3UV2C4>& vertices, const TVector<uint32_t>& indices) override;

		SAILOR_API virtual void SubmitCommandList(RHI::RHICommandListPtr commandList, RHI::RHIFencePtr fence = nullptr, RHI::RHISemaphorePtr signalSemaphore = nullptr, RHI::RHISemaphorePtr waitSemaphore = nullptr);

		// Shader binding set
		SAILOR_API virtual RHI::RHIShaderBindingSetPtr CreateShaderBindings();
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddSsboToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t elementSize, size_t numElements, uint32_t shaderBinding);
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddBufferToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding, RHI::EShaderBindingType bufferType);
		SAILOR_API virtual RHI::RHIShaderBindingPtr AddSamplerToShaderBindings(RHI::RHIShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::RHITexturePtr texture, uint32_t shaderBinding);

		// Used for full binding update
		SAILOR_API virtual void UpdateShaderBinding(RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, RHI::RHITexturePtr value);
		SAILOR_API virtual void UpdateShaderBinding_Immediate(RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, const void* value, size_t size);		

		// Begin Immediate context
		SAILOR_API virtual RHI::RHIBufferPtr CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage);
		SAILOR_API virtual void CopyBuffer_Immediate(RHI::RHIBufferPtr src, RHI::RHIBufferPtr dst, size_t size);

		SAILOR_API virtual TRefPtr<RHI::RHITexture> CreateImage_Immediate(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			RHI::ETextureType type = RHI::ETextureType::Texture2D,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit);
		//End Immediate context

		//Begin IGraphicsDriverCommands
		SAILOR_API virtual void RenderSecondaryCommandBuffers(RHI::RHICommandListPtr cmd,
			TVector<RHI::RHICommandListPtr> secondaryCmds,
			const TVector<RHI::RHITexturePtr>& colorAttachments,
			RHI::RHITexturePtr depthStencilAttachment,
			glm::ivec4 renderArea,
			glm::ivec2 offset,
			bool bClearRenderTargets,
			glm::vec4 clearColor,
			bool bSupportMultisampling = true);

		SAILOR_API virtual void BeginRenderPass(RHI::RHICommandListPtr cmd,
			const TVector<RHI::RHISurfacePtr>& colorAttachments,
			RHI::RHISurfacePtr depthStencilAttachment,
			glm::ivec4 renderArea,
			glm::ivec2 offset,
			bool bClearRenderTargets,
			glm::vec4 clearColor);

		SAILOR_API virtual void BeginRenderPass(RHI::RHICommandListPtr cmd, const TVector<RHI::RHITexturePtr>& colorAttachments,
			RHI::RHITexturePtr depthStencilAttachment,
			glm::ivec4 renderArea,
			glm::ivec2 offset,
			bool bClearRenderTargets,
			glm::vec4 clearColor,
			bool bSupportMultisampling = true);

		SAILOR_API virtual void EndRenderPass(RHI::RHICommandListPtr cmd);
		SAILOR_API virtual void ImageMemoryBarrier(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr image, RHI::EFormat format, RHI::EImageLayout oldLayout, RHI::EImageLayout newLayout);
		SAILOR_API virtual bool FitsViewport(RHI::RHICommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2 scissorExtent, float minDepth, float maxDepth);
		SAILOR_API virtual bool FitsDefaultViewport(RHI::RHICommandListPtr cmd);

		SAILOR_API virtual void BeginSecondaryCommandList(RHI::RHICommandListPtr cmd, bool bOneTimeSubmit = false, bool bSupportMultisampling = true);
		SAILOR_API virtual void BeginCommandList(RHI::RHICommandListPtr cmd, bool bOneTimeSubmit);
		SAILOR_API virtual void EndCommandList(RHI::RHICommandListPtr cmd);

		SAILOR_API virtual void BlitImage(RHI::RHICommandListPtr cmd, RHI::RHITexturePtr src, RHI::RHITexturePtr dst, glm::ivec4 srcRegionRect, glm::ivec4 dstRegionRect, RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear);
		SAILOR_API virtual void UpdateShaderBindingVariable(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingPtr binding, const std::string& variable, const void* value, size_t size);
		SAILOR_API virtual void UpdateShaderBinding(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingPtr binding, const void* data, size_t size, size_t offset = 0);
		SAILOR_API virtual void UpdateBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr buffer, const void* data, size_t size, size_t offset = 0);
		SAILOR_API virtual void SetMaterialParameter(RHI::RHICommandListPtr cmd, RHI::RHIShaderBindingSetPtr bindings, const std::string& binding, const std::string& variable, const void* value, size_t size);
		SAILOR_API virtual void BindMaterial(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material);
		SAILOR_API virtual void BindVertexBuffers(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr vertexBuffer);
		SAILOR_API virtual void BindIndexBuffer(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer);
		
		SAILOR_API virtual void BindVertexBufferEx(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr vertexBuffer, uint32_t offset);
		SAILOR_API virtual void BindIndexBufferEx(RHI::RHICommandListPtr cmd, RHI::RHIBufferPtr indexBuffer, uint32_t offset);

		SAILOR_API virtual void SetViewport(RHI::RHICommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2 scissorExtent, float minDepth, float maxDepth);
		SAILOR_API virtual void SetDefaultViewport(RHI::RHICommandListPtr cmd);
		SAILOR_API virtual void BindShaderBindings(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr, const TVector<RHI::RHIShaderBindingSetPtr>& bindings);
		SAILOR_API virtual void DrawIndexed(RHI::RHICommandListPtr cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance);
		SAILOR_API virtual void ExecuteSecondaryCommandList(RHI::RHICommandListPtr cmd, RHI::RHICommandListPtr cmdSecondary);
		SAILOR_API virtual void PushConstants(RHI::RHICommandListPtr cmd, RHI::RHIMaterialPtr material, size_t size, const void* ptr);
		//End IGraphicsDriverCommands

		// Vulkan specific
		SAILOR_API void Update(RHI::RHICommandListPtr cmd, VulkanBufferMemoryPtr dstBuffer, const void* data, size_t size, size_t offset = 0);
		SAILOR_API RHI::RHITexturePtr GetOrCreateMsaaRenderTarget(RHI::EFormat textureFormat, glm::ivec2 extent);

	protected:

		SAILOR_API void UpdateDescriptorSet(RHI::RHIShaderBindingSetPtr bindings);
		SAILOR_API TSharedPtr<VulkanBufferAllocator>& GetUniformBufferAllocator(const std::string& uniformTypeId);
		SAILOR_API TSharedPtr<VulkanBufferAllocator>& GetMaterialSsboAllocator();
		SAILOR_API TSharedPtr<VulkanBufferAllocator>& GetMeshSsboAllocator();

		// Uniform buffers to store uniforms
		TConcurrentMap<std::string, TSharedPtr<VulkanBufferAllocator>> m_uniformBuffers;

		// Storage buffers to store everything
		TSharedPtr<VulkanBufferAllocator> m_materialSsboAllocator;
		TSharedPtr<VulkanBufferAllocator> m_meshSsboAllocator;

		// Cached MSAA render targets to support MSAA
		TConcurrentMap<size_t, RHI::RHITexturePtr> m_cachedMsaaRenderTargets{};

		GraphicsDriver::Vulkan::VulkanApi* m_vkInstance;

		RHI::RHITexturePtr m_backBuffer;
		RHI::RHITexturePtr m_depthStencilBuffer;
	};
};

#endif //SAILOR_BUILD_WITH_VULKAN
