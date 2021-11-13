#pragma once
#include "Core/RefPtr.hpp"
#include "Renderer.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

namespace Sailor::RHI
{
	class Material : public Resource
	{
	public:
#if defined(VULKAN)
		struct
		{
			Sailor::GfxDevice::Vulkan::VulkanPipelinePtr m_pipeline;
		} m_vulkan;
#endif
		Material(RenderState renderState) : m_renderState(std::move(renderState)) {}

		bool IsTransparent() const { return m_bIsTransparent; }
		const std::string& GetRenderQueue() const { return m_renderQueue; }
		const RHI::RenderState& GetRenderState() const { return m_renderState; }
		const std::vector<RHI::ShaderBinding>& GetBindings() const { return m_bindings; }

	protected:

		RHI::RenderState m_renderState;

		std::string m_renderQueue = "Opaque";
		bool m_bIsTransparent = false;

		ShaderPtr m_vertexShader;
		ShaderPtr m_fragmentShader;

		std::vector<RHI::ShaderBinding> m_bindings;
	};

	class MaterialDynamic : public Material
	{
	public:

		void SetParameter(const std::string& parameter, TexturePtr value) {}
		void SetParameter(const std::string& parameter, const glm::vec4 value) {}
		void SetParameter(const std::string& parameter, const glm::mat4x4& value) {}

	protected:

	};
};
