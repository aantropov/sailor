#pragma once

#include "RHI/Types.h"
#include "Math/Math.h"

#include <array>
#include <cstddef>
#include <limits>

namespace Sailor::RHI
{
	inline constexpr uint32_t MaxReflectionProbes = 32u;
	inline constexpr uint32_t MaxReflectionTextures = MaxReflectionProbes * 2u;
	inline constexpr uint32_t InvalidReflectionProbeIndex =
		(std::numeric_limits<uint32_t>::max)();

	struct alignas(16) RHIReflectionProbeShaderData
	{
		glm::vec4 m_positionRadius{};
		glm::vec4 m_capturePosition{};
		glm::uvec4 m_textureIndices{};
	};

	static_assert(sizeof(RHIReflectionProbeShaderData) == 48u);
	static_assert(offsetof(RHIReflectionProbeShaderData, m_textureIndices) == 32u);

	class RHISceneReflectionsSnapshot final : public RHIResource
	{
	public:
		std::array<RHIReflectionProbeShaderData, MaxReflectionProbes> m_probes{};
		std::array<RHITexturePtr, MaxReflectionTextures> m_textures{};
		uint64_t m_revision = 0u;
	};

	using RHISceneReflectionsSnapshotPtr = TRefPtr<const RHISceneReflectionsSnapshot>;
}
