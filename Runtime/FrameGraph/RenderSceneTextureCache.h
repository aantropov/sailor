#pragma once
#include "Core/Defines.h"
#include "Containers/Vector.h"

namespace Sailor::Framegraph::Details
{
	SAILOR_API bool CanReuseRenderSceneTextureBindings(
		uint64_t cachedSourceRevision,
		uint64_t currentSourceRevision,
		bool bHasCachedBindings,
		const TVector<uint64_t>* cachedSlotRevisions = nullptr,
		const TVector<uint64_t>* currentSlotRevisions = nullptr);
}
