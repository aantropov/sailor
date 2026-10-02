#include "FrameGraphParser.h"

using namespace Sailor;

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
