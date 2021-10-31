#pragma once
#include "Core/RefPtr.hpp"
#include "Texture.h"
#include "Fence.h"
#include "CommandList.h"
#include "Mesh.h"
#include "Types.h"
#include "Platform/Win32/Window.h"

namespace Sailor::RHI
{
	class IGfxDevice
	{
	public:

		virtual SAILOR_API void Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug) = 0;
		virtual SAILOR_API ~IGfxDevice() = default;

		virtual SAILOR_API bool ShouldFixLostDevice(const Win32::Window* pViewport) = 0;
		virtual SAILOR_API void FixLostDevice(const Win32::Window* pViewport) = 0;

		virtual SAILOR_API bool PresentFrame(const class FrameState& state,
			const std::vector<TRefPtr<RHI::CommandList>>* primaryCommandBuffers = nullptr,
			const std::vector<TRefPtr<RHI::CommandList>>* secondaryCommandBuffers = nullptr) const = 0;

		virtual void SAILOR_API WaitIdle() = 0;

		virtual SAILOR_API TRefPtr<class RHI::Buffer> CreateBuffer(size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API TRefPtr<class RHI::CommandList> CreateBuffer(TRefPtr<Buffer>& outbuffer, const void* pData, size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API TRefPtr<RHI::Mesh> CreateMesh(const std::vector<RHI::Vertex>& vertices, const std::vector<uint32_t>& indices);

		virtual SAILOR_API void SubmitCommandList(TRefPtr<RHI::CommandList> commandList, TRefPtr<RHI::Fence> fence) = 0;

		//Immediate context
		virtual SAILOR_API TRefPtr<Buffer> CreateBuffer_Immediate(const void* pData, size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API void CopyBuffer_Immediate(TRefPtr<Buffer> src, TRefPtr<Buffer> dst, size_t size) = 0;

		virtual SAILOR_API TRefPtr<Texture> CreateImage_Immediate(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			ETextureType type = ETextureType::Texture2D,
			ETextureFormat format = ETextureFormat::R8G8B8A8_SRGB,
			ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit) = 0;
		//Immediate context

		void TraceFences();

	protected:

		std::vector<TRefPtr<RHI::Fence>> m_trackedFences;
	};
};
