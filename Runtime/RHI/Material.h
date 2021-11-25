#pragma once
#include "Core/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"
#include "GfxDevice/Vulkan/VulkanBufferMemory.h"

namespace Sailor::RHI
{
	class ShaderBindingSet : public Resource
	{
	public:

#if defined(VULKAN)
		struct
		{
			GfxDevice::Vulkan::VulkanDescriptorSetPtr m_descriptorSet;
		} m_vulkan;
#endif

		SAILOR_API void AddLayoutShaderBinding(ShaderLayoutBinding layout);
		SAILOR_API void SetLayoutShaderBindings(std::vector<RHI::ShaderLayoutBinding> layoutBindings);
		SAILOR_API const std::vector<RHI::ShaderLayoutBinding>& GetLayoutBindings() const { return m_layoutBindings; }
		SAILOR_API RHI::ShaderBindingPtr& GetOrCreateShaderBinding(const std::string& binding);
		SAILOR_API const std::unordered_map<std::string, RHI::ShaderBindingPtr>& GetShaderBindings() const { return m_shaderBindings; }

		static SAILOR_API void ParseParameter(const std::string& parameter, std::string& outBinding, std::string& outVariable);

		SAILOR_API bool HasBinding(const std::string& binding) const;
		SAILOR_API bool HasParameter(const std::string& parameter) const;

		SAILOR_API bool NeedsStorageBuffer() const { return m_bNeedsStorageBuffer; }

	protected:

		SAILOR_API bool PerInstanceDataStoredInSSBO() const;

		std::vector<RHI::ShaderLayoutBinding> m_layoutBindings;
		std::unordered_map<std::string, RHI::ShaderBindingPtr> m_shaderBindings;
		bool m_bNeedsStorageBuffer = false;
	};

	class Material : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanPipelinePtr m_pipeline;
		} m_vulkan;
#endif
		SAILOR_API Material(RenderState renderState, ShaderPtr vertexShader, ShaderPtr fragmentShader) :
			m_renderState(std::move(renderState)),
			m_vertexShader(vertexShader),
			m_fragmentShader(fragmentShader)
		{}

		SAILOR_API const RHI::RenderState& GetRenderState() const { return m_renderState; }

		SAILOR_API ShaderPtr GetVertexShader() const { return m_vertexShader; }
		SAILOR_API ShaderPtr GetFragmentShader() const { return m_fragmentShader; }

		SAILOR_API ShaderBindingSetPtr GetBindings() const { return m_bindings; }
		SAILOR_API void SetBindings(ShaderBindingSetPtr bindings) { m_bindings = bindings; }

	protected:

		RHI::RenderState m_renderState;

		ShaderPtr m_vertexShader;
		ShaderPtr m_fragmentShader;

		ShaderBindingSetPtr m_bindings;

		friend class IGfxDevice;
	};
};
