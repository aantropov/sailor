#ifndef SAILOR_RENDER_TEXTURE_DESCRIPTOR_H
#define SAILOR_RENDER_TEXTURE_DESCRIPTOR_H

#include <RHI/Types.h>
#include <yaml-cpp/yaml.h>
#include <cstdint>
#include <string_view>

namespace Sailor
{
	struct RenderTextureDescriptor
	{
		uint32_t m_version = 1;
		uint32_t m_width = 512;
		uint32_t m_height = 512;
		RHI::ETextureFormat m_format = RHI::ETextureFormat::R8G8B8A8_UNORM;

		YAML::Node Serialize() const
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
			if (!IsSupportedFormat(m_format))
			{
				throw YAML::RepresentationException(YAML::Mark(), "RenderTextureDescriptor: unsupported format");
			}

			YAML::Node node;
			node["version"] = m_version;
			node["width"] = m_width;
			node["height"] = m_height;
			node["format"] = FormatToString(m_format);
			return node;
		}

		void Deserialize(const YAML::Node& node)
		{
			if (!node.IsMap())
			{
				throw YAML::RepresentationException(node.Mark(), "RenderTextureDescriptor: expected a map");
			}

			RenderTextureDescriptor candidate;

			if (!node["version"])
			{
				throw YAML::RepresentationException(node.Mark(), "RenderTextureDescriptor: missing 'version'");
			}
			candidate.m_version = node["version"].as<uint32_t>();
			if (candidate.m_version != 1)
			{
				throw YAML::RepresentationException(node["version"].Mark(), "RenderTextureDescriptor: unsupported version");
			}

			if (!node["width"])
			{
				throw YAML::RepresentationException(node.Mark(), "RenderTextureDescriptor: missing 'width'");
			}
			candidate.m_width = node["width"].as<uint32_t>();
			if (candidate.m_width < 1 || candidate.m_width > 16384)
			{
				throw YAML::RepresentationException(node["width"].Mark(), "RenderTextureDescriptor: width out of range");
			}

			if (!node["height"])
			{
				throw YAML::RepresentationException(node.Mark(), "RenderTextureDescriptor: missing 'height'");
			}
			candidate.m_height = node["height"].as<uint32_t>();
			if (candidate.m_height < 1 || candidate.m_height > 16384)
			{
				throw YAML::RepresentationException(node["height"].Mark(), "RenderTextureDescriptor: height out of range");
			}

			if (node["format"])
			{
				const YAML::Node& formatNode = node["format"];
				if (!formatNode.IsScalar())
				{
					throw YAML::RepresentationException(formatNode.Mark(), "RenderTextureDescriptor: format must be a scalar");
				}
				std::string_view fmt(formatNode.Scalar());
				if (!StringToFormat(fmt, candidate.m_format))
				{
					throw YAML::RepresentationException(formatNode.Mark(), "RenderTextureDescriptor: unsupported format string");
				}
			}
			else
			{
				candidate.m_format = RHI::ETextureFormat::R8G8B8A8_UNORM;
			}

			*this = candidate;
		}

	private:
		static bool IsSupportedFormat(RHI::ETextureFormat fmt)
		{
			if (fmt == RHI::ETextureFormat::R8G8B8A8_UNORM)
			{
				return true;
			}
			if (fmt == RHI::ETextureFormat::R8G8B8A8_SRGB)
			{
				return true;
			}
			if (fmt == RHI::ETextureFormat::R16G16B16A16_SFLOAT)
			{
				return true;
			}
			return false;
		}

		static const char* FormatToString(RHI::ETextureFormat fmt)
		{
			if (fmt == RHI::ETextureFormat::R8G8B8A8_UNORM)
			{
				return "R8G8B8A8_UNORM";
			}
			if (fmt == RHI::ETextureFormat::R8G8B8A8_SRGB)
			{
				return "R8G8B8A8_SRGB";
			}
			if (fmt == RHI::ETextureFormat::R16G16B16A16_SFLOAT)
			{
				return "R16G16B16A16_SFLOAT";
			}
			throw YAML::RepresentationException(YAML::Mark(), "RenderTextureDescriptor: cannot serialize unknown format");
		}

		static bool StringToFormat(std::string_view str, RHI::ETextureFormat& out)
		{
			if (str == "R8G8B8A8_UNORM")
			{
				out = RHI::ETextureFormat::R8G8B8A8_UNORM;
				return true;
			}
			if (str == "R8G8B8A8_SRGB")
			{
				out = RHI::ETextureFormat::R8G8B8A8_SRGB;
				return true;
			}
			if (str == "R16G16B16A16_SFLOAT")
			{
				out = RHI::ETextureFormat::R16G16B16A16_SFLOAT;
				return true;
			}
			return false;
		}
	};
}

#endif
