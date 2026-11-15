#pragma once

#include "Core/YamlSerializable.h"
#include "RHI/Types.h"

namespace Sailor
{
	struct RenderTextureDescriptor : IYamlSerializable
	{
		uint32_t m_version = 1;
		uint32_t m_width = 512;
		uint32_t m_height = 512;
		RHI::ETextureFormat m_format = RHI::ETextureFormat::R8G8B8A8_UNORM;

		YAML::Node Serialize() const override;
		void Deserialize(const YAML::Node& node) override;

	private:
		void Validate() const
		{
			if (m_version != 1)
			{
				throw YAML::RepresentationException(YAML::Mark(), "RenderTextureDescriptor: unsupported version");
			}
			if (m_width < 1 || m_width > 16384)
			{
				throw YAML::RepresentationException(YAML::Mark(), "RenderTextureDescriptor: width out of range");
			}
			if (m_height < 1 || m_height > 16384)
			{
				throw YAML::RepresentationException(YAML::Mark(), "RenderTextureDescriptor: height out of range");
			}
			if (m_format != RHI::ETextureFormat::R8G8B8A8_UNORM &&
				m_format != RHI::ETextureFormat::R8G8B8A8_SRGB &&
				m_format != RHI::ETextureFormat::R16G16B16A16_SFLOAT)
			{
				throw YAML::RepresentationException(YAML::Mark(), "RenderTextureDescriptor: unsupported format");
			}
		}
	};
}

REFL_AUTO(
	type(Sailor::RenderTextureDescriptor),
	field(m_version),
	field(m_width),
	field(m_height),
	field(m_format, Sailor::Attributes::YamlOptional{})
)

inline YAML::Node Sailor::RenderTextureDescriptor::Serialize() const
{
	Validate();
	return SerializeReflected(*this);
}

inline void Sailor::RenderTextureDescriptor::Deserialize(const YAML::Node& node)
{
	RenderTextureDescriptor candidate;
	DeserializeReflected(node, candidate);
	candidate.Validate();
	*this = candidate;
}
