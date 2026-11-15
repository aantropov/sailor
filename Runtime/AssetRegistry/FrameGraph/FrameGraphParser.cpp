#include "FrameGraphParser.h"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <algorithm>

using namespace Sailor;

namespace
{
	Settings::GraphicsExtent GetDefaultOutputExtent()
	{
		Platform::Window* pWindow = App::GetMainWindowPlatform();
		if (pWindow)
		{
			const glm::ivec2 extent = pWindow->GetRenderArea();
			return Settings::GraphicsExtent
			{
				(static_cast<uint32_t>((std::max)(extent.x, 1))),
				(static_cast<uint32_t>((std::max)(extent.y, 1)))
			};
		}
		return Settings::GraphicsExtent{ 1, 1 };
	}

	float GetDefaultResolutionFactor()
	{
		return App::GetActiveGraphicsSettings().m_resolutionFactor;
	}
}

uint32_t FrameGraphAsset::RenderTarget::ParseUintValue(std::string_view str)
{
	return ParseUintValue(str, GetDefaultOutputExtent(), GetDefaultResolutionFactor());
}

uint32_t FrameGraphAsset::RenderTarget::ParseUintValue(std::string_view str, const Settings::GraphicsExtent& outputExtent, float resolutionFactor)
{
	const std::string_view value = Utils::TrimView(str);
	uint32_t size = 0;
	if (!value.empty())
	{
		const char* begin = value.data();
		if (value.front() == '+')
		{
			++begin;
		}
		const auto parsed = std::from_chars(begin, value.data() + value.size(), size);
		if (parsed.ec == std::errc() && parsed.ptr == value.data() + value.size() && size > 0)
		{
			return size;
		}
	}

	const auto slash = value.find('/');
	const std::string_view variable = Utils::TrimView(value.substr(0, slash));
	if (variable != "RenderWidth" && variable != "RenderHeight" &&
		variable != "ViewportWidth" && variable != "ViewportHeight")
	{
		throw YAML::RepresentationException(YAML::Mark::null_mark(), "Invalid frame graph dimension: " + std::string(str));
	}

	double divisor = 1.0;
	if (slash != std::string_view::npos)
	{
		const std::string denominator(Utils::TrimView(value.substr(slash + 1)));
		char* end = nullptr;
		divisor = std::strtod(denominator.c_str(), &end);
		if (end != denominator.c_str() + denominator.size() || !std::isfinite(divisor) || divisor <= 0.0)
		{
			throw YAML::RepresentationException(YAML::Mark::null_mark(), "Invalid frame graph dimension divisor: " + std::string(str));
		}
	}

	const uint32_t viewportWidth = (std::max)(outputExtent.m_width, 1u);
	const uint32_t viewportHeight = (std::max)(outputExtent.m_height, 1u);
	const auto renderExtent = Settings::ResolveRenderDimensions(viewportWidth, viewportHeight, resolutionFactor);

	if (variable == "RenderWidth")
	{
		size = renderExtent.m_width;
	}
	else if (variable == "RenderHeight")
	{
		size = renderExtent.m_height;
	}
	else if (variable == "ViewportWidth")
	{
		size = viewportWidth;
	}
	else
	{
		size = viewportHeight;
	}
	return (std::max)(1u, static_cast<uint32_t>(size / divisor));
}

void FrameGraphAsset::RenderTarget::Deserialize(const YAML::Node& inData)
{
	Deserialize(inData, GetDefaultOutputExtent(), GetDefaultResolutionFactor());
}

void FrameGraphAsset::RenderTarget::Deserialize(const YAML::Node& inData, const Settings::GraphicsExtent& outputExtent, float resolutionFactor)
{
	m_name = inData["name"].as<std::string>();

	if (inData["width"])
	{
		m_width = ParseUintValue(inData["width"].as<std::string_view>(), outputExtent, resolutionFactor);
	}

	if (inData["height"])
	{
		m_height = ParseUintValue(inData["height"].as<std::string_view>(), outputExtent, resolutionFactor);
	}

	if (inData["format"])
	{
		DeserializeEnum<RHI::ETextureFormat>(inData["format"], m_format);
	}

	if (inData["filtration"])
	{
		DeserializeEnum<RHI::ETextureFiltration>(inData["filtration"], m_filtration);
	}

	if (inData["reduction"])
	{
		DeserializeEnum<RHI::ESamplerReductionMode>(inData["reduction"], m_reduction);
	}

	if (inData["clamping"])
	{
		DeserializeEnum<RHI::ETextureClamping>(inData["clamping"], m_clamping);
	}

	if (inData["bIsSurface"])
	{
		m_bIsSurface = inData["bIsSurface"].as<bool>();
	}

	if (inData["bGenerateMips"])
	{
		m_bGenerateMips = inData["bGenerateMips"].as<bool>();
	}

	if (inData["maxMipLevel"])
	{
		const int32_t limit = inData["maxMipLevel"].as<int32_t>();
		if (limit <= 0)
		{
			throw YAML::RepresentationException(inData["maxMipLevel"].Mark(), "Frame graph mip limit must be positive: " + m_name);
		}
		m_maxMipLevel = static_cast<uint32_t>(limit);
	}

	if (inData["bIsCompatibleWithComputeShaders"])
	{
		m_bIsCompatibleWithComputeShaders = inData["bIsCompatibleWithComputeShaders"].as<bool>();
	}
}

void FrameGraphAsset::Deserialize(const YAML::Node& inData)
{
	Deserialize(inData, GetDefaultOutputExtent(), GetDefaultResolutionFactor());
}

void FrameGraphAsset::Deserialize(const YAML::Node& inData, const Settings::GraphicsExtent& outputExtent, float resolutionFactor)
{
	m_samplers.Clear();
	m_values.Clear();
	m_renderTargets.Clear();
	m_nodes.Clear();

	if (inData["samplers"])
	{
		auto samplers = inData["samplers"].as<TVector<FrameGraphAsset::Resource>>();

		for (auto& sampler : samplers)
		{
			if (m_samplers.ContainsKey(sampler.m_name))
			{
				throw YAML::RepresentationException(inData["samplers"].Mark(), "Duplicate frame graph resource: " + sampler.m_name);
			}
			m_samplers[sampler.m_name] = std::move(sampler);
		}
	}

	if (inData["float"])
	{
		for (const auto& p : inData["float"])
		{
			for (const auto& keyValue : p)
			{
				const std::string key = keyValue.first.as<std::string>();
				m_values[key] = Value(keyValue.second.as<float>());
			}
		}
	}

	if (inData["vec4"])
	{
		for (const auto& p : inData["vec4"])
		{
			for (const auto& keyValue : p)
			{
				const std::string key = keyValue.first.as<std::string>();
				m_values[key] = Value(keyValue.second.as<glm::vec4>());
			}
		}
	}

	if (inData["renderTargets"])
	{
		for (const auto& node : inData["renderTargets"])
		{
			RenderTarget target;
			target.Deserialize(node, outputExtent, resolutionFactor);

			if (m_renderTargets.ContainsKey(target.m_name) || m_samplers.ContainsKey(target.m_name))
			{
				throw YAML::RepresentationException(inData["renderTargets"].Mark(), "Duplicate frame graph resource: " + target.m_name);
			}
			m_renderTargets[target.m_name] = std::move(target);
		}
	}

	if (inData["frame"])
	{
		for (const auto& el : inData["frame"])
		{
			FrameGraphAsset::Node node;
			node.Deserialize(el);
			m_nodes.Add(std::move(node));
		}
	}
}
