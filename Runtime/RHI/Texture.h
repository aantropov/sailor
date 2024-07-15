#pragma once
#include "Memory/RefPtr.hpp"

#if defined(SAILOR_BUILD_WITH_VULKAN)
#include "GraphicsDriver/Vulkan/VulkanBuffer.h"
#endif

#include "Types.h"

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{
	class RHITexture : public RHIResource, public IDelayedInitialization
	{
	public:
#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			VulkanImagePtr m_image{};
			VulkanImageViewPtr m_imageView{};
		} m_vulkan;
#endif

		SAILOR_API RHITexture(ETextureFiltration filtration, ETextureClamping clamping, bool bShouldGenerateMips, EImageLayout defaultLayout = EImageLayout::ShaderReadOnlyOptimal, ESamplerReductionMode reduction = ESamplerReductionMode::Average) :
			m_reduction(reduction),
			m_filtration(filtration),
			m_clamping(clamping),
			m_defaultLayout(defaultLayout),
			m_bHasMipMaps(bShouldGenerateMips)
		{}

		SAILOR_API ESamplerReductionMode GetSamplerReduction() const { return m_reduction; }
		SAILOR_API ETextureFiltration GetFiltration() const { return m_filtration; }
		SAILOR_API ETextureClamping GetClamping() const { return m_clamping; }
		SAILOR_API bool HasMipMaps() const { return m_bHasMipMaps; }
		SAILOR_API EFormat GetFormat() const;
		SAILOR_API glm::ivec2 GetExtent() const;
		SAILOR_API EImageLayout GetDefaultLayout() const { return m_defaultLayout; }

		// The render resource must already be in the specified layout.
		SAILOR_API void ForceSetDefaultLayout(EImageLayout newLayout) { m_defaultLayout = newLayout; }
		SAILOR_API size_t GetSize() const;

	protected:

		ESamplerReductionMode m_reduction = RHI::ESamplerReductionMode::Average;
		ETextureFiltration m_filtration = RHI::ETextureFiltration::Linear;
		ETextureClamping m_clamping = RHI::ETextureClamping::Repeat;
		EImageLayout m_defaultLayout = EImageLayout::ColorAttachmentOptimal;
		bool m_bHasMipMaps = false;
	};
};
