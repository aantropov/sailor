#pragma once

#include "AssetRegistry/FileId.h"
#include "Containers/Map.h"
#include "Engine/InstanceId.h"
#include "Engine/Types.h"
#include "Memory/ObjectPtr.hpp"

namespace Sailor
{
	enum class EPrefabInstanceIdPolicy : uint8_t
	{
		PreserveAvailable,
		GenerateNew,
		RequireExact
	};

	// The source asset remains authoritative on the live root's FileId.
	struct PrefabInstanceLink final
	{
		InstanceId m_rootInstanceId{};
		TMap<InstanceId, InstanceId> m_sourceToInstanceIds{};
		PrefabPtr m_effectiveBaseline{};
	};

	struct WorldPrefabLinks final
	{
		TMap<InstanceId, PrefabInstanceLink> m_instances;
		TMap<InstanceId, InstanceId> m_rootsByObject;
	};
}
