#pragma once

#include "ECS/LightingECS.h"

namespace Sailor::LightingECSInternal
{
	TSharedPtr<TVector<LightingECS::LightShaderData>> AcquireLightsSnapshot(
		TVector<TSharedPtr<TVector<LightingECS::LightShaderData>>>& pool,
		const TVector<LightingECS::LightShaderData>& source);

	void ResolveShadowCasterUpdatePolicy(const TVector<RHI::RHIVisibleShadowCaster>& casters,
		bool& outContainsDynamicCasters,
		bool& outContainsAnimatedCasters);

	RHI::RHISubmissionCompletionTokenPtr AcquireShadowPayloadToken(
		const RHI::RHISubmissionCompletionTokenPtr& cachedToken);

	float CalculateCsmShadowMapMemoryMb(const RHI::RHIRenderTargetPtr& shadowMap);
}
