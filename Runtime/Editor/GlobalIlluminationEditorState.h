#pragma once

#include "ECS/GlobalIlluminationECS.h"

#include <cstdint>
#include <string>

namespace Sailor
{
	struct SAILOR_SHARED_API EditorGlobalIlluminationState final
	{
		uint32_t m_maxProbeStatesPerSnapshot = 0u;
		TVector<GlobalIlluminationProbeState> m_probes{};
		std::string m_diagnostic{};
		uint64_t m_compositionCount = 0u;
		uint64_t m_rejectedCompositionCount = 0u;
	};
}
