#include "FrameGraphParser.h"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace Sailor;

uint32_t FrameGraphAsset::RenderTarget::ParseUintValue(std::string_view str)
{
	const std::string_view value = Utils::TrimView(str);
	uint32_t size = 0;
	if (!value.empty())
	{
		const char* begin = value.data();
		if (value.front() == '+') ++begin;
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

	const glm::ivec2 viewportExtent = App::GetMainWindow()->GetRenderArea();
	const auto renderExtent = Settings::ResolveRenderDimensions(
		static_cast<uint32_t>((std::max)(viewportExtent.x, 1)),
		static_cast<uint32_t>((std::max)(viewportExtent.y, 1)),
		App::GetActiveGraphicsSettings().m_resolutionFactor);
	if (variable == "RenderWidth") size = renderExtent.m_width;
	else if (variable == "RenderHeight") size = renderExtent.m_height;
	else if (variable == "ViewportWidth") size = static_cast<uint32_t>((std::max)(viewportExtent.x, 1));
	else size = static_cast<uint32_t>((std::max)(viewportExtent.y, 1));

	const double scaled = size / divisor;
	if (scaled > (std::numeric_limits<uint32_t>::max)())
	{
		throw YAML::RepresentationException(YAML::Mark::null_mark(), "Frame graph dimension is too large: " + std::string(str));
	}
	return (std::max)(1u, static_cast<uint32_t>(scaled));
}

void FrameGraphAsset::Deserialize(const YAML::Node& inData)
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
		auto targets = inData["renderTargets"].as<TVector<RenderTarget>>();

		for (auto& target : targets)
		{
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
