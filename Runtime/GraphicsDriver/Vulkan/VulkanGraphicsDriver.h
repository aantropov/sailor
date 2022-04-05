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

#ifdef VULKAN

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanGraphicsDriver : public RHI::IGraphicsDriver, public RHI::IGraphicsDriverCommands
	{
	public:

		SAILOR_API virtual void Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		SAILOR_API virtual ~VulkanGraphicsDriver() override;

		SAILOR_API virtual bool ShouldFixLostDevice(const Win32::Window* pViewport);
		SAILOR_API virtual void FixLostDevice(const Win32::Window* pViewport);

		SAILOR_API virtual bool PresentFrame(const class FrameState& state,
			const TVector<RHI::CommandListPtr>* primaryCommandBuffers = nullptr,
			const TVector<RHI::CommandListPtr>* secondaryCommandBuffers = nullptr,
			TVector<RHI::SemaphorePtr> waitSemaphores = {}) const;

		SAILOR_API virtual void WaitIdle();

		SAILOR_API virtual RHI::SemaphorePtr CreateWaitSemaphore();
		SAILOR_API virtual RHI::CommandListPtr CreateCommandList(bool bIsSecondary = false, bool bOnlyTransferQueue = false);
		SAILOR_API virtual RHI::BufferPtr CreateBuffer(size_t size, RHI::EBufferUsageFlags usage);
		SAILOR_API virtual RHI::BufferPtr CreateBuffer(RHI::CommandListPtr& cmdBuffer, const void* pData, size_t size, RHI::EBufferUsageFlags usage);
		SAILOR_API virtual RHI::ShaderPtr CreateShader(RHI::EShaderStage shaderStage, const RHI::ShaderByteCode& shaderSpirv);
		SAILOR_API virtual RHI::TexturePtr CreateImage(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			RHI::ETextureType type = RHI::ETextureType::Texture2D,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit);

		SAILOR_API virtual RHI::MaterialPtr CreateMaterial(const RHI::VertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader);
		SAILOR_API virtual RHI::MaterialPtr CreateMaterial(const RHI::VertexDescriptionPtr& vertexDescription, RHI::EPrimitiveTopology topology, const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader, const RHI::ShaderBindingSetPtr& shaderBindigs);

		SAILOR_API virtual void SubmitCommandList(RHI::CommandListPtr commandList, RHI::FencePtr fence = nullptr, RHI::SemaphorePtr signalSemaphore = nullptr, RHI::SemaphorePtr waitSemaphore = nullptr);

		// Shader binding set
		SAILOR_API virtual RHI::ShaderBindingSetPtr CreateShaderBindings();
		SAILOR_API virtual void AddBufferToShaderBindings(RHI::ShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding, RHI::EShaderBindingType bufferType);
		SAILOR_API virtual void AddSamplerToShaderBindings(RHI::ShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::TexturePtr texture, uint32_t shaderBinding);

		// Used for full binding update
		SAILOR_API virtual void UpdateShaderBinding(RHI::ShaderBindingSetPtr bindings, const std::string& binding, RHI::TexturePtr value);
		SAILOR_API virtual void UpdateShaderBinding_Immediate(RHI::ShaderBindingSetPtr bindings, const std::string& binding, const void* value, size_t size);

		// Begin Immediate context
		SAILOR_API virtual RHI::BufferPtr CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage);
		SAILOR_API virtual void CopyBuffer_Immediate(RHI::BufferPtr src, RHI::BufferPtr dst, size_t size);

		SAILOR_API virtual TRefPtr<RHI::Texture> CreateImage_Immediate(
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
		SAILOR_API virtual void BeginCommandList(RHI::CommandListPtr cmd);
		SAILOR_API virtual void EndCommandList(RHI::CommandListPtr cmd);

		SAILOR_API virtual void UpdateShaderBindingVariable(RHI::CommandListPtr cmd, RHI::ShaderBindingPtr binding, const std::string& variable, const void* value, size_t size);
		SAILOR_API virtual void UpdateShaderBinding(RHI::CommandListPtr cmd, RHI::ShaderBindingPtr binding, const void* data, size_t size, size_t offset = 0);
		SAILOR_API virtual void SetMaterialParameter(RHI::CommandListPtr cmd, RHI::ShaderBindingSetPtr bindings, const std::string& binding, const std::string& variable, const void* value, size_t size);

		SAILOR_API virtual void BindMaterial(RHI::CommandListPtr cmd, RHI::MaterialPtr material);
		SAILOR_API virtual void BindVertexBuffers(RHI::CommandListPtr cmd, RHI::BufferPtr vertexBuffer);
		SAILOR_API virtual void BindIndexBuffer(RHI::CommandListPtr cmd, RHI::BufferPtr indexBuffer);
		SAILOR_API virtual void SetViewport(RHI::CommandListPtr cmd, float x, float y, float width, float height, glm::vec2 scissorOffset, glm::vec2 scissorExtent, float minDepth, float maxDepth);
		SAILOR_API virtual void SetDefaultViewport(RHI::CommandListPtr cmd);
		SAILOR_API virtual void BindShaderBindings(RHI::CommandListPtr cmd, RHI::MaterialPtr, const TVector<RHI::ShaderBindingSetPtr>& bindings);
		SAILOR_API virtual void DrawIndexed(RHI::CommandListPtr cmd, RHI::BufferPtr indexBuffer, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance);
		//End IGraphicsDriverCommands

	protected:

		SAILOR_API void UpdateDescriptorSet(RHI::ShaderBindingSetPtr bindings);
		SAILOR_API TSharedPtr<VulkanBufferAllocator>& GetUniformBufferAllocator(const std::string& uniformTypeId);
		SAILOR_API TSharedPtr<VulkanBufferAllocator>& GetStorageBufferAllocator();

		// Uniform buffers to store uniforms
		TConcurrentMap<std::string, TSharedPtr<VulkanBufferAllocator>> m_uniformBuffers;

		// Storage buffers to store everything
		TSharedPtr<VulkanBufferAllocator> m_storageBuffer;

		GraphicsDriver::Vulkan::VulkanApi* m_vkInstance;
	};
};

#endif
