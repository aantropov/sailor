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

		SAILOR_API RHIRenderTarget(ETextureFiltration filtration, ETextureClamping clamping, bool bShouldGenerateMips, EImageLayout defaultLayout = EImageLayout::ShaderReadOnlyOptimal) :
			RHITexture(filtration, clamping, bShouldGenerateMips, defaultLayout)
		{}

		RHITexturePtr GetMipLayer(uint32_t layer) const;

	protected:

		TVector<RHITexturePtr> m_mipLayers;

#if defined(SAILOR_BUILD_WITH_VULKAN)
		friend class Sailor::GraphicsDriver::Vulkan::VulkanGraphicsDriver;
#endif
	};
};
