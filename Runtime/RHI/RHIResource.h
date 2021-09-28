#pragma once
#include "Core/RefPtr.hpp"
#include <glm/glm/glm.hpp>

namespace Sailor::RHI
{
	enum class ETextureFiltration : uint8_t
	{
		Nearest = 0,
		Linear,
		Bicubic
	};

	enum class ETextureClamping : uint8_t
	{
		Clamp = 0,
		Repeat
	};

	class RHIVertex
	{
	public:
		glm::vec3 m_position;
		glm::vec2 m_texcoord;
		glm::vec4 m_color;
	};

	struct UBOTransform
	{
		alignas(16) glm::mat4 m_model;
		alignas(16) glm::mat4 m_view;
		alignas(16) glm::mat4 m_projection;
	};

	class RHIResource : public TRefBase
	{
	public:

	protected:

		RHIResource() = default;
		virtual ~RHIResource() = default;

	private:

		RHIResource(RHIResource& copy) = delete;
		RHIResource& operator =(RHIResource& rhs) = delete;
	};

	class IRHIExplicitInit
	{
	public:

		virtual void Compile() = 0;
		virtual void Release() = 0;
	};

	template<typename TState>
	class IRHIStateModifier
	{
	public:
		virtual void Apply(TState& State) const = 0;
		virtual ~IRHIStateModifier() = default;
	};
};