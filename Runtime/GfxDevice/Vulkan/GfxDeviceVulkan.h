#pragma once
#include "Defines.h"
#include "Core/RefPtr.hpp"
#include "RHI/GfxDevice.h"
#include "RHI/Texture.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/Types.h"
#include "Platform/Win32/Window.h"

#ifdef VULKAN

namespace Sailor::GfxDevice::Vulkan
{
	class GfxDeviceVulkan : public RHI::IGfxDevice
	{
	public:

		virtual SAILOR_API void Initialize(const Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug);
		virtual SAILOR_API ~GfxDeviceVulkan();

		virtual SAILOR_API bool ShouldFixLostDevice(const Win32::Window* pViewport);
		virtual SAILOR_API void FixLostDevice(const Win32::Window* pViewport);

		virtual SAILOR_API bool PresentFrame(const class FrameState& state,
			const std::vector<TRefPtr<RHI::CommandList>>* primaryCommandBuffers = nullptr,
			const std::vector<TRefPtr<RHI::CommandList>>* secondaryCommandBuffers = nullptr) const;

		virtual void SAILOR_API WaitIdle();

		virtual SAILOR_API TRefPtr<class RHI::Buffer> CreateBuffer(size_t size, RHI::EBufferUsageFlags usage);
		virtual SAILOR_API TRefPtr<class RHI::CommandList> CreateBuffer(TRefPtr<RHI::Buffer>& outbuffer, const void* pData, size_t size, RHI::EBufferUsageFlags usage);

		virtual SAILOR_API void SubmitCommandList(TRefPtr<RHI::CommandList> commandList, TRefPtr<RHI::Fence> fence);

		//Immediate context
		virtual SAILOR_API TRefPtr<RHI::Buffer> CreateBuffer_Immediate(const void* pData, size_t size, RHI::EBufferUsageFlags usage);
		virtual SAILOR_API void CopyBuffer_Immediate(TRefPtr<RHI::Buffer> src, TRefPtr<RHI::Buffer> dst, size_t size);

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

		GfxDevice::Vulkan::VulkanApi* m_vkInstance;
	};
};

#endif
