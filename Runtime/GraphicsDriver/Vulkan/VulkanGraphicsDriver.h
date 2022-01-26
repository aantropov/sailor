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

		virtual SAILOR_API void Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		virtual SAILOR_API ~VulkanGraphicsDriver() override;

		virtual SAILOR_API bool ShouldFixLostDevice(const Win32::Window* pViewport);
		virtual SAILOR_API void FixLostDevice(const Win32::Window* pViewport);

		virtual SAILOR_API bool PresentFrame(const class FrameState& state,
			const TVector<RHI::CommandListPtr>* primaryCommandBuffers = nullptr,
			const TVector<RHI::CommandListPtr>* secondaryCommandBuffers = nullptr,
			TVector<RHI::SemaphorePtr> waitSemaphores = {}) const;

		virtual void SAILOR_API WaitIdle();

		virtual SAILOR_API RHI::SemaphorePtr CreateWaitSemaphore();
		virtual SAILOR_API RHI::CommandListPtr CreateCommandList(bool bIsSecondary = false, bool bOnlyTransferQueue = false);
		virtual SAILOR_API RHI::BufferPtr CreateBuffer(size_t size, RHI::EBufferUsageFlags usage);
		virtual SAILOR_API RHI::CommandListPtr CreateBuffer(RHI::BufferPtr& outbuffer, const void* pData, size_t size, RHI::EBufferUsageFlags usage);
		virtual SAILOR_API RHI::ShaderPtr CreateShader(RHI::EShaderStage shaderStage, const RHI::ShaderByteCode& shaderSpirv);
		virtual SAILOR_API RHI::TexturePtr CreateImage(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			RHI::ETextureType type = RHI::ETextureType::Texture2D,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Clamp,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit);

		virtual SAILOR_API RHI::MaterialPtr CreateMaterial(const RHI::RenderState& renderState, const Sailor::ShaderSetPtr& shader);

		virtual SAILOR_API void SubmitCommandList(RHI::CommandListPtr commandList, RHI::FencePtr fence = nullptr, RHI::SemaphorePtr signalSemaphore = nullptr, RHI::SemaphorePtr waitSemaphore = nullptr);

		// Shader binding set
		virtual SAILOR_API RHI::ShaderBindingSetPtr CreateShaderBindings();
		virtual SAILOR_API void AddBufferToShaderBindings(RHI::ShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding, RHI::EShaderBindingType bufferType);
		virtual SAILOR_API void AddSamplerToShaderBindings(RHI::ShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::TexturePtr texture, uint32_t shaderBinding);

		// Used for full binding update
		virtual SAILOR_API void UpdateShaderBinding(RHI::ShaderBindingSetPtr bindings, const std::string& binding, RHI::TexturePtr value);
		virtual SAILOR_API void UpdateShaderBinding_Immediate(RHI::ShaderBindingSetPtr bindings, const std::string& binding, const void* value, size_t size);

		// Begin Immediate context
		virtual SAILOR_API RHI::BufferPtr CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage);
		virtual SAILOR_API void CopyBuffer_Immediate(RHI::BufferPtr src, RHI::BufferPtr dst, size_t size);

		virtual SAILOR_API TRefPtr<RHI::Texture> CreateImage_Immediate(
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
		virtual SAILOR_API void BeginCommandList(RHI::CommandListPtr cmd);
		virtual SAILOR_API void EndCommandList(RHI::CommandListPtr cmd);

		virtual SAILOR_API void UpdateShaderBindingVariable(RHI::CommandListPtr cmd, RHI::ShaderBindingPtr binding, const std::string& variable, const void* value, size_t size);
		virtual SAILOR_API void UpdateShaderBinding(RHI::CommandListPtr cmd, RHI::ShaderBindingPtr binding, const void* data, size_t size, size_t offset = 0);
		virtual SAILOR_API void SetMaterialParameter(RHI::CommandListPtr cmd, RHI::MaterialPtr material, const std::string& binding, const std::string& variable, const void* value, size_t size);
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