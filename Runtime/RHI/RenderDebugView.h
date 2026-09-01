#pragma once

#include <cstdint>

namespace Sailor::RHI
{
	enum class ESceneViewRenderMode : uint8_t
	{
		Lit = 0,
		AmbientOcclusion,
		Cascades,
		LightTiles,
		GlobalIlluminationOnly,
		GlobalIlluminationProbes,
		GlobalIlluminationBricks,
		GlobalIlluminationValidity,
		GlobalIlluminationVisibility,
		GlobalIlluminationResidency,
		GlobalIlluminationAssetIdentity,
		GlobalIlluminationFallback,
		GlobalIlluminationClipmapCascades
	};

	constexpr bool IsSceneViewDebugVisualization(
		ESceneViewRenderMode mode) noexcept
	{
		return mode != ESceneViewRenderMode::Lit;
	}

	constexpr const char* GetSceneViewRenderModeShaderDefine(
		ESceneViewRenderMode mode) noexcept
	{
		switch (mode)
		{
		case ESceneViewRenderMode::AmbientOcclusion:
			return "AO";
		case ESceneViewRenderMode::Cascades:
			return "CASCADES";
		case ESceneViewRenderMode::LightTiles:
			return "LIGHT_TILES";
		case ESceneViewRenderMode::Lit:
		default:
			return "";
		}
	}
}
