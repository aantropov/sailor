#pragma once

#include "RHI/RenderSubmission.h"
#include "RHI/Material.h"
#include "RHI/Lighting.h"
#include "RHI/GlobalIllumination.h"

#include <algorithm>
#include <limits>

namespace Sailor::RHI
{
	constexpr uint64_t InvalidContentHash = (std::numeric_limits<uint64_t>::max)();

	class RHIViewSubmissionResources final : public RHIFrameGraphSubmissionResource
	{
	public:
		void ResetForSubmission() override {}
		void InvalidateSubmission() override
		{
			m_shadowMatricesHash = InvalidContentHash;
			m_shadowIndicesHash = InvalidContentHash;
			m_shadowAtlasTilesHash = InvalidContentHash;
		}

		RHIShaderBindingSetPtr m_lightsBindings{};
		RHIShaderBindingSetPtr m_lightsTemplate{};
		RHIShaderBindingSetPtr m_sharedLightsStorage{};
		RHIShaderBindingSetPtr m_sharedGlobalIlluminationStorage{};
		RHIShaderBindingSetPtr m_frameBindings{};
		size_t m_shadowMatrixCapacity = 0u;
		size_t m_shadowIndexCapacity = 0u;
		size_t m_shadowAtlasTileCapacity = 0u;
		uint64_t m_lightsTemplateRevision = 0ull;
		size_t m_frameGraphSamplerHash = 0u;
		uint64_t m_shadowMatricesHash = InvalidContentHash;
		uint64_t m_shadowIndicesHash = InvalidContentHash;
		uint64_t m_shadowAtlasTilesHash = InvalidContentHash;
	};

	class RHISharedViewSubmissionResources final : public RHIFrameGraphSubmissionResource
	{
	public:
		void ResetForSubmission() override {}
		void InvalidateSubmission() override
		{
			m_uploadedLightingRevision = InvalidContentHash;
			m_uploadedAnimationRevision = InvalidContentHash;
			m_uploadedGlobalIlluminationLayout = InvalidContentHash;
			m_uploadedGlobalIlluminationCoefficients = InvalidContentHash;
			m_uploadedGlobalIlluminationStates = InvalidContentHash;
			m_uploadedGlobalIlluminationHeader = InvalidContentHash;
			m_lightsSource.Clear();
			m_bonesSource.Clear();
		}

		RHIShaderBindingSetPtr m_lightsStorage{};
		RHIShaderBindingSetPtr m_boneBindings{};
		RHIShaderBindingSetPtr m_globalIlluminationStorage{};
		TSharedPtr<const TVector<RHILightShaderData>> m_lightsSource{};
		TSharedPtr<const TVector<glm::mat4>> m_bonesSource{};
		size_t m_lightCapacity = 0u;
		size_t m_boneCapacity = 0u;
		size_t m_globalIlluminationNodeCapacity = 0u;
		size_t m_globalIlluminationBrickCapacity = 0u;
		size_t m_globalIlluminationProbeCapacity = 0u;
		size_t m_globalIlluminationCoefficientCapacity = 0u;
		size_t m_globalIlluminationStateCapacity = 0u;
		uint64_t m_uploadedLightingRevision = InvalidContentHash;
		uint64_t m_uploadedAnimationRevision = InvalidContentHash;
		uint64_t m_uploadedGlobalIlluminationLayout = InvalidContentHash;
		uint64_t m_uploadedGlobalIlluminationCoefficients = InvalidContentHash;
		uint64_t m_uploadedGlobalIlluminationStates = InvalidContentHash;
		uint64_t m_uploadedGlobalIlluminationHeader = InvalidContentHash;
	};

	inline size_t GrowSubmissionCapacity(size_t currentCapacity, size_t requiredCapacity)
	{
		size_t result = (std::max)(size_t{ 1u }, currentCapacity);
		while (result < requiredCapacity)
		{
			const size_t next = result * 2u;
			if (next <= result)
			{
				return requiredCapacity;
			}
			result = next;
		}
		return result;
	}

	void UploadSharedLighting(RHICommandListPtr commandList,
		const RHISceneViewSnapshot& snapshot, RHISharedViewSubmissionResources& resources);
	RHIGlobalIlluminationRenderStats UploadGlobalIllumination(RHICommandListPtr commandList,
		const RHISceneViewSnapshot& snapshot, RHISharedViewSubmissionResources& resources);
	void UploadSharedBones(RHICommandListPtr commandList,
		const RHISceneViewSnapshot& snapshot, RHISharedViewSubmissionResources& resources);
}
