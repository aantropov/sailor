#include "GlobalIllumination/GISettings.h"
#include "Core/YamlUtils.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

bool GISettings::Validate(
	std::string& outDiagnostic) const noexcept
{
	outDiagnostic.clear();
	for (const auto& entry : m_probes)
	{
		const std::string& name = entry.m_first;
		const GlobalIlluminationProbeBinding& binding = *entry.m_second;
		if (name.empty())
		{
			outDiagnostic = "a global-illumination probe binding has an empty name";
			return false;
		}
		if (!binding.m_asset)
		{
			outDiagnostic = "global-illumination probe binding '" + name +
				"' has no .probes asset";
			return false;
		}
		if (!std::isfinite(binding.m_initialWeight) ||
			binding.m_initialWeight < 0.0f)
		{
			outDiagnostic = "global-illumination probe binding '" + name +
				"' has an invalid initial weight";
			return false;
		}
	}
	return true;
}

YAML::Node GISettings::Serialize() const
{
	YAML::Node globalIllumination(YAML::NodeType::Map);
	YAML::Node probes(YAML::NodeType::Map);
	globalIllumination["mode"] = std::string(magic_enum::enum_name(m_mode));

	struct SortedBinding final
	{
		std::string m_name{};
		const GlobalIlluminationProbeBinding* m_binding = nullptr;
	};
	TVector<SortedBinding> sortedBindings;
	sortedBindings.Reserve(m_probes.Num());
	for (const auto& entry : m_probes)
	{
		sortedBindings.Add({ entry.m_first, entry.m_second });
	}
	std::sort(
		sortedBindings.begin(),
		sortedBindings.end(),
		[](const SortedBinding& lhs, const SortedBinding& rhs)
		{
			return lhs.m_name < rhs.m_name;
		});

	for (const SortedBinding& sorted : sortedBindings)
	{
		const std::string& name = sorted.m_name;
		const GlobalIlluminationProbeBinding& binding = *sorted.m_binding;
		YAML::Node bindingNode(YAML::NodeType::Map);
		YAML::Node assetNode(YAML::NodeType::Map);
		assetNode["fileId"] = binding.m_asset.Serialize();
		bindingNode["asset"] = assetNode;
		bindingNode["mode"] = std::string(magic_enum::enum_name(binding.m_mode));
		bindingNode["initialWeight"] = binding.m_initialWeight;
		if (binding.m_bPreload)
		{
			bindingNode["preload"] = true;
		}
		probes[name] = bindingNode;
	}

	globalIllumination["probes"] = probes;
	return globalIllumination;
}

bool GISettings::Deserialize(
	const YAML::Node& worldRoot,
	std::string& outDiagnostic) noexcept
{
	m_mode = EGlobalIlluminationMode::RealtimeAndBaked;
	m_probes.Clear();
	outDiagnostic.clear();

	auto deserialize = [&]() -> bool
	{
		const YAML::Node giNode = Utils::FindYamlMapField(
			worldRoot,
			"globalIllumination");
		if (!giNode)
		{
			return true;
		}
		if (!giNode.IsMap())
		{
			outDiagnostic = "globalIllumination must be a map";
			return false;
		}
		const YAML::Node globalModeNode = Utils::FindYamlMapField(giNode, "mode");
		if (!globalModeNode || !globalModeNode.IsScalar())
		{
			outDiagnostic = "globalIllumination.mode is required";
			return false;
		}
		const std::string globalMode = globalModeNode.as<std::string>();
		const auto parsedGlobalMode =
			magic_enum::enum_cast<EGlobalIlluminationMode>(globalMode);
		if (!parsedGlobalMode)
		{
			outDiagnostic = "globalIllumination.mode must be Realtime, RealtimeAndBaked, or BakedOnly";
			return false;
		}
		m_mode = *parsedGlobalMode;
		const YAML::Node probesNode = Utils::FindYamlMapField(giNode, "probes");
		if (!probesNode)
		{
			return true;
		}
		if (!probesNode.IsMap())
		{
			outDiagnostic = "globalIllumination.probes must be a name-to-binding map";
			return false;
		}

		for (const auto& yamlEntry : probesNode)
		{
			const std::string name = yamlEntry.first.as<std::string>();
			const YAML::Node bindingNode = yamlEntry.second;
			if (name.empty() || !bindingNode.IsMap())
			{
				outDiagnostic = "a globalIllumination.probes entry has an empty name or invalid binding";
				return false;
			}
			const YAML::Node assetNode = Utils::FindYamlMapField(bindingNode, "asset");
			const YAML::Node fileIdNode = assetNode && assetNode.IsMap()
				? Utils::FindYamlMapField(assetNode, "fileId")
				: YAML::Node();
			if (!fileIdNode || !fileIdNode.IsScalar())
			{
				outDiagnostic = "global-illumination probe binding '" + name +
					"' must reference asset.fileId";
				return false;
			}

			GlobalIlluminationProbeBinding binding;
			binding.m_asset.Deserialize(fileIdNode);
			const YAML::Node modeNode = Utils::FindYamlMapField(bindingNode, "mode");
			if (!modeNode || !modeNode.IsScalar())
			{
				outDiagnostic = "global-illumination probe binding '" + name +
					"' mode is required";
				return false;
			}
			const std::string mode = modeNode.as<std::string>();
			const auto parsedProbeMode =
				magic_enum::enum_cast<EGlobalIlluminationProbeMode>(mode);
			if (!parsedProbeMode)
			{
				outDiagnostic = "global-illumination probe binding '" + name +
					"' mode must be Blend or Additive";
				return false;
			}
			binding.m_mode = *parsedProbeMode;
			const YAML::Node initialWeightNode = Utils::FindYamlMapField(
				bindingNode,
				"initialWeight");
			binding.m_initialWeight = initialWeightNode
				? initialWeightNode.as<float>()
				: 0.0f;
			const YAML::Node preloadNode = Utils::FindYamlMapField(bindingNode, "preload");
			binding.m_bPreload = preloadNode
				? preloadNode.as<bool>()
				: false;
			if (!m_probes.Insert(name, std::move(binding)))
			{
				outDiagnostic = "global-illumination probe binding names must be unique";
				return false;
			}
		}
		return Validate(outDiagnostic);
	};

	bool bResult = false;
	std::string yamlDiagnostic;
	if (!External::TryInvokeYaml(deserialize, bResult, yamlDiagnostic))
	{
		m_mode = EGlobalIlluminationMode::RealtimeAndBaked;
		m_probes.Clear();
		outDiagnostic = std::string("cannot parse globalIllumination settings: ") +
			yamlDiagnostic;
		return false;
	}
	return bResult;
}
