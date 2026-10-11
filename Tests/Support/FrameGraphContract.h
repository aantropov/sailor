#pragma once

#include <string>
#include <string_view>
#include <yaml-cpp/yaml.h>

namespace Sailor::Tests
{
	inline std::string GetSequenceMapping(const YAML::Node& sequence, std::string_view key)
	{
		if (sequence && sequence.IsSequence())
		{
			for (const auto& entry : sequence)
			{
				if (!entry.IsMap()) continue;
				const auto value = entry[key];
				if (value && value.IsScalar()) return value.Scalar();
			}
		}
		return {};
	}
}
