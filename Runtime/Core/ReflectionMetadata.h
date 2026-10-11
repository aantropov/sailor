#pragma once

#include "Containers/Containers.h"
#include <yaml-cpp/yaml.h>

namespace Sailor
{
	struct ReflectedTypeCatalog
	{
		YAML::Node m_metadata;
		TMap<std::string, YAML::Node> m_types;
		TMap<std::string, YAML::Node> m_defaults;
		TMap<std::string, YAML::Node> m_enums;
		TSet<std::string> m_registeredTypes;
	};
}
