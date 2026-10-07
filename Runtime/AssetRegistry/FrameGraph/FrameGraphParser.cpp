#include "FrameGraphParser.h"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>

using namespace Sailor;

namespace
{
	void ValidateRasterAttachments(const FrameGraphAsset& graph, const FrameGraphAsset::Node& node, YAML::Mark mark)
	{
		const auto type = HashString(node.m_name);
		const bool bScene = type == "RenderScene"_h.GetHash();
		const bool bHasDepth = bScene || type == "ExperimentalParticles"_h.GetHash() ||
			type == "DebugDraw"_h.GetHash() || type == "RenderImGui"_h.GetHash() || type == "DepthPrepass"_h.GetHash();
		const bool bHasColor = (bHasDepth && type != "DepthPrepass"_h.GetHash()) ||
			type == "PostProcess"_h.GetHash() || type == "Sky"_h.GetHash() || type == "AtmosphericFog"_h.GetHash();
		if (!bHasDepth && !bHasColor) return;

		const auto& label = node.m_tag.empty() ? node.m_name : node.m_tag;
		const auto attachment = [&](const std::string& binding, bool bDepth, const std::string& fallback = {}) -> const FrameGraphAsset::RenderTarget*
		{
			const std::string* name = nullptr;
			if (!node.m_renderTargets.Find(binding, name)) name = &fallback;
			if (name->empty()) return nullptr;
			const FrameGraphAsset::RenderTarget* target = nullptr;
			if (graph.m_renderTargets.Find(*name, target))
			{
				if (RHI::IsDepthFormat(target->m_format) != bDepth)
				{
					throw YAML::RepresentationException(mark, std::format(
						"Frame graph pass '{}' attachment '{}' uses '{}' with a non-{} format",
						label, binding, *name, bDepth ? "depth" : "color"));
				}
				return target;
			}
			if (graph.m_samplers.ContainsKey(*name))
			{
				throw YAML::RepresentationException(mark, std::format(
					"Frame graph pass '{}' attachment '{}' uses sampled-only resource '{}'", label, binding, *name));
			}
			// A runtime producer may publish this resource after the static graph is built.
			return nullptr;
		};

		const auto color = bHasColor ? attachment("color", false,
			type == "PostProcess"_h.GetHash() ? "BackBuffer" : "") : nullptr;
		const auto depth = bHasDepth ? attachment("depthStencil", true,
			type == "RenderImGui"_h.GetHash() ? "" : "DepthBuffer") : nullptr;
		const auto motion = bScene ? attachment("motionVectors", false) : nullptr;
		if (!color) return;
		for (const auto target : { depth, motion })
		{
			if (target && (target->m_width < color->m_width || target->m_height < color->m_height))
			{
				throw YAML::RepresentationException(mark, std::format(
					"Frame graph pass '{}' attachment '{}' does not cover the render area of '{}'",
					label, target->m_name, color->m_name));
			}
		}
	}
}

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
			ValidateRasterAttachments(*this, node, el.Mark());
			m_nodes.Add(std::move(node));
		}
	}
}
