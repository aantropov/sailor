#include "GlobalIllumination/GISettings.h"

#include "Core/YamlSerializable.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <cmath>

using namespace Sailor;

YAML::Node GlobalIlluminationProbeBinding::Serialize() const
{
	YAML::Node result(YAML::NodeType::Map);
	YAML::Node asset(YAML::NodeType::Map);
	::Serialize(asset, "fileId", m_asset);
	result["asset"] = asset;
	SERIALIZE_PROPERTY(result, m_mode);
	SERIALIZE_PROPERTY(result, m_initialWeight);
	if (m_bPreload)
	{
		::Serialize(result, "preload", true);
	}
	return result;
}

bool GlobalIlluminationProbeBinding::Deserialize(
	const YAML::Node& inData,
	const std::string& name,
	std::string& outDiagnostic)
{
	*this = {};
	const YAML::Node asset = inData["asset"];
	if (!asset || !::Deserialize(asset, "fileId", m_asset))
	{
		outDiagnostic = "global-illumination probe binding '" + name +
			"' must reference asset.fileId";
		return false;
	}
	if (!inData["mode"])
	{
		outDiagnostic = "global-illumination probe binding '" + name +
			"' mode is required";
		return false;
	}
	if (!DESERIALIZE_PROPERTY(inData, m_mode))
	{
		outDiagnostic = "global-illumination probe binding '" + name +
			"' mode must be Blend or Additive";
		return false;
	}
	DESERIALIZE_PROPERTY(inData, m_initialWeight);
	::Deserialize(inData, "preload", m_bPreload);
	return true;
}

bool GISettings::Validate(
	std::string& outDiagnostic) const noexcept
{
	outDiagnostic.clear();
	if (!m_runtimeProbes.Validate(outDiagnostic))
	{
		return false;
	}
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
	SERIALIZE_PROPERTY(globalIllumination, m_mode);
	globalIllumination["runtimeProbes"] = m_runtimeProbes.Serialize();

	struct SortedBinding final
	{
		std::string m_name;
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

	YAML::Node probes(YAML::NodeType::Map);
	for (const SortedBinding& binding : sortedBindings)
	{
		probes[binding.m_name] = binding.m_binding->Serialize();
	}
	globalIllumination["probes"] = probes;
	return globalIllumination;
}

bool GISettings::Deserialize(
	const YAML::Node& worldRoot,
	std::string& outDiagnostic) noexcept
{
	GISettings parsedSettings;
	outDiagnostic.clear();

	auto deserialize = [&]() -> bool
	{
		const YAML::Node globalIllumination = worldRoot["globalIllumination"];
		if (!globalIllumination)
		{
			return true;
		}

		if (!globalIllumination["mode"])
		{
			outDiagnostic = "globalIllumination.mode is required";
			return false;
		}
		if (!::Deserialize(globalIllumination, "mode", parsedSettings.m_mode))
		{
			outDiagnostic = "globalIllumination.mode must be NoGI, Runtime, or Baked";
			return false;
		}

		const YAML::Node runtimeProbes = globalIllumination["runtimeProbes"];
		if (!runtimeProbes || !parsedSettings.m_runtimeProbes.Deserialize(runtimeProbes))
		{
			outDiagnostic = "globalIllumination.runtimeProbes is incomplete";
			return false;
		}

		const YAML::Node probes = globalIllumination["probes"];
		if (probes)
		{
			for (const auto& entry : probes)
			{
				const std::string name = entry.first.as<std::string>();
				GlobalIlluminationProbeBinding binding;
				if (!binding.Deserialize(entry.second, name, outDiagnostic))
				{
					return false;
				}
				if (!parsedSettings.m_probes.Insert(name, std::move(binding)))
				{
					outDiagnostic = "global-illumination probe binding names must be unique";
					return false;
				}
			}
		}

		return parsedSettings.Validate(outDiagnostic);
	};

	bool bResult = false;
	std::string yamlDiagnostic;
	if (!External::TryInvokeYaml(deserialize, bResult, yamlDiagnostic))
	{
		outDiagnostic = "cannot parse globalIllumination settings: " + yamlDiagnostic;
		return false;
	}
	if (!bResult)
	{
		return false;
	}

	*this = std::move(parsedSettings);
	return true;
}
