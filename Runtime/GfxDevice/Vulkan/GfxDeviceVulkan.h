#pragma once
#include "Defines.h"
#include "Core/RefPtr.hpp"
#include "Memory/MemoryPoolAllocator.hpp"
#include "RHI/Types.h"
#include "RHI/GfxDevice.h"
#include "RHI/Texture.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/Shader.h"
#include "RHI/Material.h"
#include "VulkanMemory.h"
#include "VulkanBufferMemory.h"
#include "Platform/Win32/Window.h"

#ifdef VULKAN

namespace Sailor::GfxDevice::Vulkan
{
	using VulkanUniformBufferAllocator = TBlockAllocator<Sailor::Memory::GlobalVulkanBufferAllocator, VulkanBufferMemoryPtr>;

	class GfxDeviceVulkan : public RHI::IGfxDevice
	{
	public:

		virtual SAILOR_API void Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		virtual SAILOR_API ~GfxDeviceVulkan() override;

		virtual SAILOR_API bool ShouldFixLostDevice(const Win32::Window* pViewport);
		virtual SAILOR_API void FixLostDevice(const Win32::Window* pViewport);

		virtual SAILOR_API bool PresentFrame(const class FrameState& state,
			const std::vector<RHI::CommandListPtr>* primaryCommandBuffers = nullptr,
			const std::vector<RHI::CommandListPtr>* secondaryCommandBuffers = nullptr) const;

		virtual void SAILOR_API WaitIdle();

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
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit);
		
		virtual SAILOR_API RHI::MaterialPtr CreateMaterial(const RHI::RenderState& renderState, RHI::ShaderPtr vertexShader, RHI::ShaderPtr fragmentShader);

		virtual SAILOR_API void SubmitCommandList(RHI::CommandListPtr commandList, TRefPtr<RHI::Fence> fence);

		//Immediate context
		virtual SAILOR_API RHI::BufferPtr CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage);
		virtual SAILOR_API void CopyBuffer_Immediate(RHI::BufferPtr src, RHI::BufferPtr dst, size_t size);

		virtual SAILOR_API TRefPtr<RHI::Texture> CreateImage_Immediate(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			RHI::ETextureType type = RHI::ETextureType::Texture2D,
			RHI::ETextureFormat format = RHI::ETextureFormat::R8G8B8A8_SRGB,
			RHI::ETextureUsageFlags usage = RHI::ETextureUsageBit::TextureTransferSrc_Bit | RHI::ETextureUsageBit::TextureTransferDst_Bit | RHI::ETextureUsageBit::Sampled_Bit);
		//Immediate context

	protected:

		VulkanUniformBufferAllocator& GetUniformBufferAllocator(const std::string& uniformTypeId);

		// Uniform buffers to store uniforms
		std::unordered_map<std::string, VulkanUniformBufferAllocator> m_uniformBuffers;
		
		GfxDevice::Vulkan::VulkanApi* m_vkInstance;
	};
};

#endif
