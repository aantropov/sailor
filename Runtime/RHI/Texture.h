#pragma once
#include "Memory/RefPtr.hpp"
#include "GraphicsDriver/Vulkan/VulkanBuffer.h"
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

		SAILOR_API RHITexture(ETextureFiltration filtration, ETextureClamping clamping, bool bShouldGenerateMips, EImageLayout defaultLayout = EImageLayout::ShaderReadOnlyOptimal) :
			m_filtration(filtration),
			m_clamping(clamping),
			m_bShouldGenerateMips(bShouldGenerateMips),
			m_defaultLayout(defaultLayout)
		{}

		SAILOR_API ETextureFiltration GetFiltration() const { return m_filtration; }
		SAILOR_API ETextureClamping GetClamping() const { return m_clamping; }
		SAILOR_API bool ShouldGenerateMips() const { return m_bShouldGenerateMips; }
		SAILOR_API EFormat GetFormat() const;
		SAILOR_API glm::ivec2 GetExtent() const;
		SAILOR_API EImageLayout GetDefaultLayout() const { return m_defaultLayout; }

	private:

		ETextureFiltration m_filtration = RHI::ETextureFiltration::Linear;
		ETextureClamping m_clamping = RHI::ETextureClamping::Repeat;
		EImageLayout m_defaultLayout = EImageLayout::ColorAttachmentOptimal;
		bool m_bShouldGenerateMips = false;
	};
};
