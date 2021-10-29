#pragma once
#include "Core/RefPtr.hpp"
#include "Texture.h"
#include "Fence.h"
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

		virtual SAILOR_API bool PresentFrame(const class FrameState& state,
			const std::vector<class CommandBuffer>* primaryCommandBuffers = nullptr,
			const std::vector<CommandBuffer>* secondaryCommandBuffers = nullptr) = 0;

		virtual void SAILOR_API WaitIdle() = 0;

		virtual SAILOR_API TRefPtr<class Buffer> CreateBuffer(size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API TRefPtr<class CommandBuffer> CreateBuffer(TRefPtr<Buffer>& outbuffer, const void* pData, size_t size, EBufferUsageFlags usage) = 0;

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
			ETextureUsageFlags usage = ETextureUsageBit::TransferSrc_Bit | ETextureUsageBit::TransferDst_Bit | ETextureUsageBit::Sampled_Bit) = 0;
		//Immediate context
	};
};
