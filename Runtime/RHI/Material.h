#pragma once
#include "Core/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanBufferMemory.h"

namespace Sailor::RHI
{
	class Material : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanPipelinePtr m_pipeline;
			Sailor::GfxDevice::Vulkan::VulkanDescriptorSetPtr m_descriptorSet;
		} m_vulkan;
#endif
		SAILOR_API Material(RenderState renderState, ShaderPtr vertexShader, ShaderPtr fragmentShader) :
			m_renderState(std::move(renderState)),
			m_vertexShader(vertexShader),
			m_fragmentShader(fragmentShader)
		{}

		SAILOR_API void SetLayoutShaderBindings(std::vector<RHI::ShaderLayoutBinding> layoutBindings);

		SAILOR_API const RHI::RenderState& GetRenderState() const { return m_renderState; }
		SAILOR_API const std::vector<RHI::ShaderLayoutBinding>& GetLayoutBindings() const { return m_layoutBindings; }

		SAILOR_API RHI::ShaderBindingPtr& GetOrCreateShaderBinding(const std::string& parameter);
		
		SAILOR_API const std::unordered_map<std::string, RHI::ShaderBindingPtr>& GetShaderBindings() const { return m_shaderBindings; }

		SAILOR_API ShaderPtr GetVertexShader() const { return m_vertexShader; }
		SAILOR_API ShaderPtr GetFragmentShader() const { return m_fragmentShader; }

		SAILOR_API bool HasParameter(const std::string& parameter) const;

	protected:

		RHI::RenderState m_renderState;

		ShaderPtr m_vertexShader;
		ShaderPtr m_fragmentShader;

		std::vector<RHI::ShaderLayoutBinding> m_layoutBindings;
		std::unordered_map<std::string, RHI::ShaderBindingPtr> m_shaderBindings;

		friend class IGfxDevice;
	};
};
