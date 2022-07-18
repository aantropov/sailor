#pragma once
#include "Memory/RefPtr.hpp"
#include "Types.h"
#include "Containers/Map.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanBufferMemory.h"

namespace Sailor::RHI
{
	class RHIShaderBindingSet : public RHIResource, public IDelayedInitialization
	{
	public:

#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			GraphicsDriver::Vulkan::VulkanDescriptorSetPtr m_descriptorSet;
		} m_vulkan;
#endif

		SAILOR_API void AddLayoutShaderBinding(ShaderLayoutBinding layout);
		SAILOR_API void SetLayoutShaderBindings(TVector<RHI::ShaderLayoutBinding> layoutBindings);
		SAILOR_API const TVector<RHI::ShaderLayoutBinding>& GetLayoutBindings() const { return m_layoutBindings; }
		SAILOR_API RHI::RHIShaderBindingPtr& GetOrCreateShaderBinding(const std::string& binding);
		SAILOR_API void RemoveShaderBinding(const std::string& binding);
		SAILOR_API const TConcurrentMap<std::string, RHI::RHIShaderBindingPtr>& GetShaderBindings() const { return m_shaderBindings; }

		static SAILOR_API void ParseParameter(const std::string& parameter, std::string& outBinding, std::string& outVariable);

		SAILOR_API bool HasBinding(const std::string& binding) const;
		SAILOR_API bool HasParameter(const std::string& parameter) const;

		SAILOR_API bool NeedsStorageBuffer() const { return m_bNeedsStorageBuffer; }

	protected:

		SAILOR_API bool PerInstanceDataStoredInSsbo() const;

		TVector<RHI::ShaderLayoutBinding> m_layoutBindings;
		TConcurrentMap<std::string, RHI::RHIShaderBindingPtr> m_shaderBindings;
		bool m_bNeedsStorageBuffer = false;
	};

	class RHIMaterial : public RHIResource
	{
	public:
#if defined(SAILOR_BUILD_WITH_VULKAN)
		struct
		{
			Sailor::GraphicsDriver::Vulkan::VulkanPipelinePtr m_pipeline;
		} m_vulkan;
#endif
		SAILOR_API RHIMaterial(RenderState renderState, RHIShaderPtr vertexShader, RHIShaderPtr fragmentShader) :
			m_renderState(std::move(renderState)),
			m_vertexShader(vertexShader),
			m_fragmentShader(fragmentShader)
		{}

		SAILOR_API const RHI::RenderState& GetRenderState() const { return m_renderState; }

		SAILOR_API RHIShaderPtr GetVertexShader() const { return m_vertexShader; }
		SAILOR_API RHIShaderPtr GetFragmentShader() const { return m_fragmentShader; }

		SAILOR_API RHIShaderBindingSetPtr GetBindings() const { return m_bindings; }
		SAILOR_API void SetBindings(RHIShaderBindingSetPtr bindings) { m_bindings = bindings; }

	protected:

		RHI::RenderState m_renderState;

		RHIShaderPtr m_vertexShader;
		RHIShaderPtr m_fragmentShader;

		RHIShaderBindingSetPtr m_bindings;

		friend class IGraphicsDriver;
	};
};
