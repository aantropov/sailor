#pragma once
#include "Core/RefPtr.hpp"
#include <glm/glm/glm.hpp>
#include <glm/glm/gtx/hash.hpp>

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

	enum class EMSAASamples : uint8_t
	{
		Samples_1 = 1,
		Samples_2 = 2,
		Samples_4 = 4,
		Samples_8 = 8,
		Samples_16 = 16,
		Samples_32 = 32,
		Samples_64 = 64
	};

	class RHIVertex
	{
	public:
		glm::vec3 m_position;
		glm::vec2 m_texcoord;
		glm::vec4 m_color;

		bool operator==(const RHIVertex& other) const 
		{
			return m_position == other.m_position && 
				m_color == other.m_color && 
				m_texcoord == other.m_texcoord;
		}
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

namespace std {
	template<> struct hash<Sailor::RHI::RHIVertex> 
	{
		size_t operator()(Sailor::RHI::RHIVertex const& vertex) const 
		{
			return ((hash<glm::vec3>()(vertex.m_position) ^
				(hash<glm::vec3>()(vertex.m_color) << 1)) >> 1) ^
				(hash<glm::vec2>()(vertex.m_texcoord) << 1);
		}
	};
}