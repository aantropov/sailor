#pragma once

#include <cstdint>

namespace Sailor::RHI
{
	enum class ESceneViewRenderMode : uint8_t
	{
		Lit = 0,
		AmbientOcclusion,
		Cascades,
		LightTiles
	};

	constexpr bool IsValidSceneViewRenderMode(ESceneViewRenderMode mode) noexcept
	{
		return mode == ESceneViewRenderMode::Lit ||
			mode == ESceneViewRenderMode::AmbientOcclusion ||
			mode == ESceneViewRenderMode::Cascades ||
			mode == ESceneViewRenderMode::LightTiles;
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
