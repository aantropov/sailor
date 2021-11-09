#pragma once
#include <mutex>
#include "Types.h"

namespace Sailor
{
	class FrameState;
}

namespace Sailor::Win32
{
	class Window;
}

namespace Sailor::RHI
{
	typedef TRefPtr<class Buffer> BufferPtr;
	typedef TRefPtr<class CommandList> CommandListPtr;
	typedef TRefPtr<class Fence> FencePtr;
	typedef TRefPtr<class Mesh> MeshPtr;
	typedef TRefPtr<class Texture> TexturePtr;
	typedef TRefPtr<class Shader> ShaderPtr;

	class IGfxDevice
	{
	public:

		virtual SAILOR_API void Initialize(const Sailor::Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug) = 0;
		virtual SAILOR_API ~IGfxDevice() = default;

		virtual SAILOR_API bool ShouldFixLostDevice(const Sailor::Win32::Window* pViewport) = 0;
		virtual SAILOR_API void FixLostDevice(const Sailor::Win32::Window* pViewport) = 0;

		virtual SAILOR_API bool PresentFrame(const Sailor::FrameState& state,
			const std::vector<CommandListPtr>* primaryCommandBuffers = nullptr,
			const std::vector<CommandListPtr>* secondaryCommandBuffers = nullptr) const = 0;

		virtual void SAILOR_API WaitIdle() = 0;

		virtual SAILOR_API BufferPtr CreateBuffer(size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API CommandListPtr CreateBuffer(BufferPtr& outbuffer, const void* pData, size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API MeshPtr CreateMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
		virtual SAILOR_API ShaderPtr CreateShader(EShaderStage shaderStage, const ShaderByteCode& shaderSpirv) = 0;
		virtual SAILOR_API TexturePtr CreateImage(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			ETextureType type = ETextureType::Texture2D,
			ETextureFormat format = ETextureFormat::R8G8B8A8_SRGB,
			ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit) = 0;
		
		virtual SAILOR_API void SubmitCommandList(CommandListPtr commandList, FencePtr fence) = 0;

		//Immediate context
		virtual SAILOR_API BufferPtr CreateBuffer_Immediate(const void* pData, size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API void CopyBuffer_Immediate(BufferPtr src, BufferPtr dst, size_t size) = 0;

		virtual SAILOR_API TexturePtr CreateImage_Immediate(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			ETextureType type = ETextureType::Texture2D,
			ETextureFormat format = ETextureFormat::R8G8B8A8_SRGB,
			ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit) = 0;
		//Immediate context
				
		void TrackResources();

	protected:

		void TrackDelayedInitialization(IDelayedInitialization* pResource, FencePtr handle);
		
		std::mutex m_mutexTrackedFences;
		std::vector<FencePtr> m_trackedFences;
	};
};
