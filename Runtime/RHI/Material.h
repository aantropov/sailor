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

		bool IsDepthTestEnabled() const { return m_bEnableDepthTest; }
		bool IsEnabledZWrite() const { return m_bEnableZWrite; }
		bool IsTransparent() const { return m_bIsTransparent; }
		ECullMode GetCullMode() const { return m_cullMode; }
		EBlendMode GetBlendMode() const { return m_blendMode; }
		float GetDepthBias() const { return m_depthBias; }
		const std::string& GetRenderQueue() const { return m_renderQueue; }

	protected:

		bool m_bEnableDepthTest = true;
		bool m_bEnableZWrite = true;
		float m_depthBias = 0.0f;
		ECullMode m_cullMode = ECullMode::Back;
		std::string m_renderQueue = "Opaque";
		bool m_bIsTransparent = false;
		EBlendMode m_blendMode = EBlendMode::None;

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
