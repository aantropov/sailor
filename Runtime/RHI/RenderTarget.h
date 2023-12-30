#pragma once
#include "Memory/RefPtr.hpp"

#if defined(SAILOR_BUILD_WITH_VULKAN)
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#endif

#include "Texture.h"
#include "Types.h"

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{
	class RHIRenderTarget : public RHITexture
	{
	public:

		SAILOR_API RHIRenderTarget(ETextureFiltration filtration, ETextureClamping clamping, bool bShouldGenerateMips, EImageLayout defaultLayout = EImageLayout::ShaderReadOnlyOptimal, ESamplerReductionMode reduction = ESamplerReductionMode::Average) :
			RHITexture(filtration, clamping, bShouldGenerateMips, defaultLayout, reduction)
		{}

		// Only for formats that handles both depth & stencil
		RHITexturePtr GetDepthAspect() const { return m_depthAspect; }
		RHITexturePtr GetStencilAspect() const { return m_stencilAspect; }

		// Only for mip levels > 1
		RHITexturePtr GetMipLayer(uint32_t layer) const;
		uint32_t GetMipLevels() const { return (uint32_t)m_mipLayers.Num(); }

	protected:

		RHITexturePtr m_depthAspect;
		RHITexturePtr m_stencilAspect;

		TVector<RHITexturePtr> m_mipLayers;

#if defined(SAILOR_BUILD_WITH_VULKAN)
		friend class Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver;
#endif
	};
};
