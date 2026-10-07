#pragma once

#include "RHI/Types.h"
#include "Engine/Types.h"

#include <limits>

namespace Sailor::RHI
{
	struct RHISceneViewProxy;
	struct RHIInstancedMeshGroup;
	struct RHIShadowCasterProxy;

	// Material-owned values copied into immutable scene resources on publication.
	struct RHIMaterialMetadata
	{
		size_t m_renderQueueTag = 0u;
		glm::vec4 m_baseColorFactor{ 1.0f };
		float m_alphaCutoff = 0.5f;
		uint32_t m_baseColorSampler = 0u;
		bool m_bRequiresCustomDepthShader = false;
		ShaderSetPtr m_shader{};
#if defined(__APPLE__)
		TVector<uint32_t> m_textureSamplers{};
#endif

		SAILOR_API void AppendTo(RHISceneViewProxy& proxy, RHIMaterialPtr material) const;
		SAILOR_API void AppendTo(RHIInstancedMeshGroup& group, RHIMaterialPtr material) const;
		SAILOR_API void AppendShadowMesh(RHIShadowCasterProxy& caster, const RHIMeshPtr& mesh,
			const glm::mat4& matrix, const RHIMaterialPtr& material,
			float maxCameraDistance = (std::numeric_limits<float>::max)()) const;
	};
}
