#include "RHIFrameGraph.h"
#include "Containers/Hash.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/GlobalIllumination.h"
#include "RHI/Shader.h"
#include "RHI/VertexDescription.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "RHI/Cubemap.h"
#include "RHI/CommandList.h"
#include "FrameGraph/LightCullingNode.h"
#include "FrameGraph/EnvironmentNode.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Settings/GraphicsSettings.h"
#include "Tasks/Tasks.h"
#include "Core/LogMacros.h"

#include <atomic>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	constexpr uint64_t InvalidContentHash = (std::numeric_limits<uint64_t>::max)();

	class RHISubmissionProgress final
	{
	public:
		void SetLastSuccessfulSemaphore(RHISemaphorePtr semaphore)
		{
			m_lock.Lock();
			m_lastSuccessfulSemaphore = std::move(semaphore);
			m_lock.Unlock();
		}

		RHISemaphorePtr GetLastSuccessfulSemaphore() const
		{
			m_lock.Lock();
			auto result = m_lastSuccessfulSemaphore;
			m_lock.Unlock();
			return result;
		}

	private:
		mutable SpinLock m_lock;
		RHISemaphorePtr m_lastSuccessfulSemaphore{};
	};

	struct GlobalIlluminationRenderStatsStorage final
	{
		SpinLock m_lock;
		const RHIFrameGraph* m_owner = nullptr;
		RHIGlobalIlluminationRenderStats m_stats{};
	};

	GlobalIlluminationRenderStatsStorage& GetGlobalIlluminationRenderStatsStorage()
	{
		static GlobalIlluminationRenderStatsStorage storage;
		return storage;
	}

	void PublishGlobalIlluminationRenderStats(
		const RHIFrameGraph* owner,
		const RHIGlobalIlluminationRenderStats& stats)
	{
		auto& storage = GetGlobalIlluminationRenderStatsStorage();
		storage.m_lock.Lock();
		storage.m_owner = owner;
		storage.m_stats = stats;
		storage.m_lock.Unlock();
	}

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
		size_t m_previousBoneCapacity = 0u;
		RHIShaderBindingSetPtr m_lightCullingBindings{};
		RHITexturePtr m_lightCullingDepth{};
		glm::ivec2 m_lightCullingViewportSize{};
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
		TSharedPtr<TVector<RHILightShaderData>> m_lightsSource{};
		TSharedPtr<TVector<glm::mat4>> m_bonesSource{};
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

	size_t GrowSubmissionCapacity(size_t currentCapacity, size_t requiredCapacity)
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

	template<typename T>
	uint64_t HashSubmissionValues(const TVector<T>& values)
	{
		const size_t numBytes = values.Num() * sizeof(T);
		uint64_t result = HashBytes(values.GetData(), numBytes);
		HashValue(result, values.Num());
		return result;
	}

	template<typename T>
	uint64_t HashSubmissionValue(const T& value)
	{
		return HashBytes(&value, sizeof(T));
	}

	EGlobalIlluminationDebugVisualization ResolveGlobalIlluminationDebug(
		ESceneViewRenderMode renderMode) noexcept
	{
		switch (renderMode)
		{
		case ESceneViewRenderMode::GlobalIlluminationOnly:
			return EGlobalIlluminationDebugVisualization::IndirectOnly;
		case ESceneViewRenderMode::GlobalIlluminationProbes:
			return EGlobalIlluminationDebugVisualization::Probes;
		case ESceneViewRenderMode::GlobalIlluminationBricks:
			return EGlobalIlluminationDebugVisualization::Bricks;
		case ESceneViewRenderMode::GlobalIlluminationValidity:
			return EGlobalIlluminationDebugVisualization::Validity;
		case ESceneViewRenderMode::GlobalIlluminationVisibility:
			return EGlobalIlluminationDebugVisualization::Visibility;
		case ESceneViewRenderMode::GlobalIlluminationResidency:
			return EGlobalIlluminationDebugVisualization::Residency;
		case ESceneViewRenderMode::GlobalIlluminationAssetIdentity:
			return EGlobalIlluminationDebugVisualization::AssetIdentity;
		case ESceneViewRenderMode::GlobalIlluminationFallback:
			return EGlobalIlluminationDebugVisualization::Fallback;
		case ESceneViewRenderMode::GlobalIlluminationSubdivisions:
			return EGlobalIlluminationDebugVisualization::Subdivisions;
		default:
			return EGlobalIlluminationDebugVisualization::Lit;
		}
	}

	void CloneTextureBindings(
		const RHIShaderBindingSetPtr& source,
		RHIShaderBindingSetPtr& destination)
	{
		if (!source || !destination)
		{
			return;
		}

		auto& driver = Renderer::GetDriver();
		for (const auto& entry : source->GetShaderBindings())
		{
			const auto& binding = entry.m_second;
			if (!binding || binding->GetTextureBindings().IsEmpty())
			{
				continue;
			}

			const auto& layout = binding->GetLayout();
			if (layout.m_type != EShaderBindingType::CombinedImageSampler)
			{
				continue;
			}

			driver->AddSamplerToShaderBindings(
				destination,
				entry.m_first,
				binding->GetTextureBindings(),
				layout.m_binding,
				layout.m_bVariableDescriptorCount,
				layout.m_arrayCount);
		}
	}

	void PrepareViewSubmissionResources(
		RHIFrameGraph* owner,
		RHICommandListPtr transferCommandList,
		RHISceneViewSnapshot& snapshot,
		bool bUploadSharedPayload,
		RHIGlobalIlluminationRenderStats* globalIlluminationStats)
	{
		if (!snapshot.m_submissionContext)
		{
			return;
		}

		auto resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<RHIViewSubmissionResources>(
			owner,
			snapshot.m_cameraIndex,
			0u);
		auto sharedResources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<RHISharedViewSubmissionResources>(
			owner,
			(std::numeric_limits<uint32_t>::max)(),
			1u);
		auto& driver = Renderer::GetDriver();
		auto commands = App::GetSubmodule<Renderer>()->GetDriverCommands();
		if (!resources->m_frameBindings)
		{
			resources->m_frameBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(
				resources->m_frameBindings,
				"frameData",
				sizeof(UboFrameData),
				0u,
				EShaderBindingType::UniformBuffer);
			driver->AddBufferToShaderBindings(
				resources->m_frameBindings,
				"previousFrameData",
				sizeof(UboFrameData),
				1u,
				EShaderBindingType::UniformBuffer);
		}
		snapshot.m_frameBindings = resources->m_frameBindings;

		auto linearDepthAttachment = owner->GetRenderTarget("LinearDepth");
		const glm::ivec2 lightCullingViewportSize = linearDepthAttachment ?
			linearDepthAttachment->GetExtent() : glm::ivec2{};
		const bool bRecreateLightCulling = linearDepthAttachment &&
			(!resources->m_lightCullingBindings ||
				resources->m_lightCullingDepth != linearDepthAttachment ||
				resources->m_lightCullingViewportSize != lightCullingViewportSize);
		if (bRecreateLightCulling)
		{
			const uint32_t numTilesX =
				(lightCullingViewportSize.x - 1) / LightCullingNode::TileSize + 1;
			const uint32_t numTilesY =
				(lightCullingViewportSize.y - 1) / LightCullingNode::TileSize + 1;
			const size_t numTiles = static_cast<size_t>(numTilesX) * numTilesY;
			resources->m_lightCullingBindings = driver->CreateShaderBindings();
			driver->AddSsboToShaderBindings(
				resources->m_lightCullingBindings,
				"culledLights",
				sizeof(uint32_t) * numTiles * LightCullingNode::LightsPerTile,
				1u,
				0u,
				true);
			driver->AddSsboToShaderBindings(
				resources->m_lightCullingBindings,
				"lightsGrid",
				sizeof(uint32_t) * numTiles * 2u,
				1u,
				1u,
				true);
			driver->AddSamplerToShaderBindings(
				resources->m_lightCullingBindings,
				"linearDepth",
				linearDepthAttachment,
				2u);
			resources->m_lightCullingBindings->RecalculateCompatibility();
			resources->m_lightCullingDepth = linearDepthAttachment;
			resources->m_lightCullingViewportSize = lightCullingViewportSize;
		}
		snapshot.m_rhiLightCullingData = resources->m_lightCullingBindings;

		auto lightsTemplate = snapshot.m_rhiLightsData;
		if (lightsTemplate == resources->m_lightsBindings && resources->m_lightsTemplate)
		{
			lightsTemplate = resources->m_lightsTemplate;
		}
		const uint64_t lightsTemplateRevision = lightsTemplate ?
			lightsTemplate->GetDescriptorRevision() : 0ull;
		const size_t numLights = snapshot.m_cpuLightsData ? snapshot.m_cpuLightsData->Num() : 0u;
		const size_t numShadowMatrices = snapshot.m_shadowMatrices.Num();
		const size_t numShadowIndices = snapshot.m_shadowIndices.Num();
		const size_t numShadowAtlasTiles = snapshot.m_shadowAtlasTiles.Num();
		const bool bRecreateLightStorage = !sharedResources->m_lightsStorage ||
			sharedResources->m_lightCapacity < numLights;
		if (bRecreateLightStorage)
		{
			sharedResources->m_lightCapacity = GrowSubmissionCapacity(
				sharedResources->m_lightCapacity,
				numLights);
			sharedResources->m_lightsStorage = driver->CreateShaderBindings();
			driver->AddSsboToShaderBindings(
				sharedResources->m_lightsStorage,
				"light",
				sizeof(RHILightShaderData),
				sharedResources->m_lightCapacity,
				0u,
				true);
			sharedResources->m_lightsStorage->RecalculateCompatibility();
			sharedResources->m_uploadedLightingRevision = InvalidContentHash;
			sharedResources->m_lightsSource.Clear();
		}
		if (bUploadSharedPayload && snapshot.m_cpuLightsData &&
			(sharedResources->m_lightsSource != snapshot.m_cpuLightsData ||
				sharedResources->m_uploadedLightingRevision != snapshot.m_lightingRevision))
		{
			if (!snapshot.m_cpuLightsData->IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					sharedResources->m_lightsStorage->GetOrAddShaderBinding("light"),
					snapshot.m_cpuLightsData->GetData(),
					snapshot.m_cpuLightsData->Num() * sizeof(RHILightShaderData),
					0u);
			}
			sharedResources->m_lightsSource = snapshot.m_cpuLightsData;
			sharedResources->m_uploadedLightingRevision = snapshot.m_lightingRevision;
		}

		const RHIGlobalIlluminationSnapshotPtr globalIllumination =
			snapshot.m_globalIllumination;
		if (globalIlluminationStats)
		{
			*globalIlluminationStats = BuildGlobalIlluminationRenderStats(
				globalIllumination.GetRawPtr());
			globalIlluminationStats->m_mode =
				snapshot.m_globalIlluminationMode;
			globalIlluminationStats->m_bEnabled =
				snapshot.m_bGlobalIlluminationEnabled;
			globalIlluminationStats->m_flightSlot =
				snapshot.m_submissionContext->GetFlightSlot();
			if (!globalIllumination)
			{
				globalIlluminationStats->m_qualityBudget =
					App::GetActiveGraphicsSettings()
						.m_maxGiProbeStatesPerSnapshot;
			}
		}
		bool bHasGlobalIllumination = globalIllumination &&
			globalIllumination->m_layout &&
			!globalIllumination->m_states.IsEmpty();
		size_t numGlobalIlluminationNodes = 0u;
		size_t numGlobalIlluminationBricks = 0u;
		size_t numGlobalIlluminationProbes = 0u;
		size_t numGlobalIlluminationCoefficients = 0u;
		size_t numGlobalIlluminationStates = 0u;
		if (bHasGlobalIllumination)
		{
			numGlobalIlluminationBricks =
				globalIllumination->m_layout->m_bricks.Num();
			numGlobalIlluminationProbes =
				globalIllumination->m_layout->m_probes.Num();
			numGlobalIlluminationStates =
				globalIllumination->m_states.Num();
			bHasGlobalIllumination = numGlobalIlluminationBricks > 0u &&
				numGlobalIlluminationProbes > 0u &&
				numGlobalIlluminationStates > 0u &&
				numGlobalIlluminationStates <=
					globalIllumination->m_qualityBudget &&
				numGlobalIlluminationProbes <=
					(std::numeric_limits<size_t>::max)() /
						numGlobalIlluminationStates;
			if (bHasGlobalIllumination)
			{
				numGlobalIlluminationNodes =
					numGlobalIlluminationBricks * 2u - 1u;
				numGlobalIlluminationCoefficients =
					numGlobalIlluminationProbes *
					numGlobalIlluminationStates;
			}
		}

		const bool bRecreateGlobalIlluminationStorage =
			!sharedResources->m_globalIlluminationStorage ||
			sharedResources->m_globalIlluminationNodeCapacity <
				numGlobalIlluminationNodes ||
			sharedResources->m_globalIlluminationBrickCapacity <
				numGlobalIlluminationBricks ||
			sharedResources->m_globalIlluminationProbeCapacity <
				numGlobalIlluminationProbes ||
			sharedResources->m_globalIlluminationCoefficientCapacity <
				numGlobalIlluminationCoefficients ||
			sharedResources->m_globalIlluminationStateCapacity <
				numGlobalIlluminationStates;
		if (bRecreateGlobalIlluminationStorage)
		{
			sharedResources->m_globalIlluminationNodeCapacity =
				GrowSubmissionCapacity(
					sharedResources->m_globalIlluminationNodeCapacity,
					numGlobalIlluminationNodes);
			sharedResources->m_globalIlluminationBrickCapacity =
				GrowSubmissionCapacity(
					sharedResources->m_globalIlluminationBrickCapacity,
					numGlobalIlluminationBricks);
			sharedResources->m_globalIlluminationProbeCapacity =
				GrowSubmissionCapacity(
					sharedResources->m_globalIlluminationProbeCapacity,
					numGlobalIlluminationProbes);
			sharedResources->m_globalIlluminationCoefficientCapacity =
				GrowSubmissionCapacity(
					sharedResources->m_globalIlluminationCoefficientCapacity,
					numGlobalIlluminationCoefficients);
			sharedResources->m_globalIlluminationStateCapacity =
				GrowSubmissionCapacity(
					sharedResources->m_globalIlluminationStateCapacity,
					numGlobalIlluminationStates);
			sharedResources->m_globalIlluminationStorage =
				driver->CreateShaderBindings();
			driver->AddSsboToShaderBindings(
				sharedResources->m_globalIlluminationStorage,
				"globalIlluminationHeader",
				sizeof(RHIGlobalIlluminationGpuHeader),
				1u,
				0u,
				true);
			driver->AddSsboToShaderBindings(
				sharedResources->m_globalIlluminationStorage,
				"globalIlluminationBvh",
				sizeof(RHIGlobalIlluminationGpuBvhNode),
				sharedResources->m_globalIlluminationNodeCapacity,
				1u,
				true);
			driver->AddSsboToShaderBindings(
				sharedResources->m_globalIlluminationStorage,
				"globalIlluminationBricks",
				sizeof(RHIGlobalIlluminationGpuBrick),
				sharedResources->m_globalIlluminationBrickCapacity,
				2u,
				true);
			driver->AddSsboToShaderBindings(
				sharedResources->m_globalIlluminationStorage,
				"globalIlluminationProbes",
				sizeof(RHIGlobalIlluminationGpuProbe),
				sharedResources->m_globalIlluminationProbeCapacity,
				3u,
				true);
			driver->AddSsboToShaderBindings(
				sharedResources->m_globalIlluminationStorage,
				"globalIlluminationCoefficients",
				sizeof(RHIGlobalIlluminationGpuCoefficients),
				sharedResources->m_globalIlluminationCoefficientCapacity,
				4u,
				true);
			driver->AddSsboToShaderBindings(
				sharedResources->m_globalIlluminationStorage,
				"globalIlluminationStates",
				sizeof(RHIGlobalIlluminationGpuState),
				sharedResources->m_globalIlluminationStateCapacity,
				5u,
				true);
			sharedResources->m_globalIlluminationStorage
				->RecalculateCompatibility();
			sharedResources->m_uploadedGlobalIlluminationLayout =
				InvalidContentHash;
			sharedResources->m_uploadedGlobalIlluminationCoefficients =
				InvalidContentHash;
			sharedResources->m_uploadedGlobalIlluminationStates =
				InvalidContentHash;
			sharedResources->m_uploadedGlobalIlluminationHeader =
				InvalidContentHash;
		}
		if (globalIlluminationStats &&
			sharedResources->m_globalIlluminationStorage)
		{
			globalIlluminationStats->m_gpuAllocatedBytes =
				sizeof(RHIGlobalIlluminationGpuHeader) +
				sharedResources->m_globalIlluminationNodeCapacity *
					sizeof(RHIGlobalIlluminationGpuBvhNode) +
				sharedResources->m_globalIlluminationBrickCapacity *
					sizeof(RHIGlobalIlluminationGpuBrick) +
				sharedResources->m_globalIlluminationProbeCapacity *
					sizeof(RHIGlobalIlluminationGpuProbe) +
				sharedResources->m_globalIlluminationCoefficientCapacity *
					sizeof(RHIGlobalIlluminationGpuCoefficients) +
				sharedResources->m_globalIlluminationStateCapacity *
					sizeof(RHIGlobalIlluminationGpuState);
		}

		if (bUploadSharedPayload)
		{
			bool bGlobalIlluminationPayloadReady = bHasGlobalIllumination;
			std::string globalIlluminationDiagnostic;
			if (bHasGlobalIllumination)
			{
				const uint64_t layoutSignature =
					ComputeGlobalIlluminationLayoutSignature(*globalIllumination);
				if (sharedResources->m_uploadedGlobalIlluminationLayout !=
					layoutSignature)
				{
					RHIGlobalIlluminationGpuLayout gpuLayout;
					bGlobalIlluminationPayloadReady =
						BuildGlobalIlluminationGpuLayout(
							*globalIllumination->m_layout,
							gpuLayout,
							globalIlluminationDiagnostic);
					if (bGlobalIlluminationPayloadReady)
					{
						const uint64_t layoutBytes =
							static_cast<uint64_t>(gpuLayout.m_nodes.Num()) *
								sizeof(RHIGlobalIlluminationGpuBvhNode) +
							static_cast<uint64_t>(gpuLayout.m_bricks.Num()) *
								sizeof(RHIGlobalIlluminationGpuBrick) +
							static_cast<uint64_t>(gpuLayout.m_probes.Num()) *
								sizeof(RHIGlobalIlluminationGpuProbe);
						commands->UpdateShaderBinding(
							transferCommandList,
							sharedResources->m_globalIlluminationStorage
								->GetOrAddShaderBinding("globalIlluminationBvh"),
							gpuLayout.m_nodes.GetData(),
							gpuLayout.m_nodes.Num() *
								sizeof(RHIGlobalIlluminationGpuBvhNode),
							0u);
						commands->UpdateShaderBinding(
							transferCommandList,
							sharedResources->m_globalIlluminationStorage
								->GetOrAddShaderBinding("globalIlluminationBricks"),
							gpuLayout.m_bricks.GetData(),
							gpuLayout.m_bricks.Num() *
								sizeof(RHIGlobalIlluminationGpuBrick),
							0u);
						commands->UpdateShaderBinding(
							transferCommandList,
							sharedResources->m_globalIlluminationStorage
								->GetOrAddShaderBinding("globalIlluminationProbes"),
							gpuLayout.m_probes.GetData(),
							gpuLayout.m_probes.Num() *
								sizeof(RHIGlobalIlluminationGpuProbe),
							0u);
						sharedResources->m_uploadedGlobalIlluminationLayout =
							layoutSignature;
						if (globalIlluminationStats)
						{
							globalIlluminationStats->m_copiedCpuBytes += layoutBytes;
							globalIlluminationStats->m_uploadedGpuBytes += layoutBytes;
						}
					}
				}

				const uint64_t coefficientSignature =
					ComputeGlobalIlluminationCoefficientSignature(
						*globalIllumination);
				if (bGlobalIlluminationPayloadReady &&
					sharedResources->m_uploadedGlobalIlluminationCoefficients !=
						coefficientSignature)
				{
					TVector<RHIGlobalIlluminationGpuCoefficients> coefficients;
					bGlobalIlluminationPayloadReady =
						BuildGlobalIlluminationGpuCoefficients(
							*globalIllumination,
							coefficients,
							globalIlluminationDiagnostic);
					if (bGlobalIlluminationPayloadReady)
					{
						const uint64_t coefficientBytes =
							static_cast<uint64_t>(coefficients.Num()) *
								sizeof(RHIGlobalIlluminationGpuCoefficients);
						commands->UpdateShaderBinding(
							transferCommandList,
							sharedResources->m_globalIlluminationStorage
								->GetOrAddShaderBinding(
									"globalIlluminationCoefficients"),
							coefficients.GetData(),
							coefficients.Num() *
								sizeof(RHIGlobalIlluminationGpuCoefficients),
							0u);
						sharedResources
							->m_uploadedGlobalIlluminationCoefficients =
							coefficientSignature;
						if (globalIlluminationStats)
						{
							globalIlluminationStats->m_copiedCpuBytes += coefficientBytes;
							globalIlluminationStats->m_uploadedGpuBytes += coefficientBytes;
						}
					}
				}

				const uint64_t stateSignature =
					ComputeGlobalIlluminationStateSignature(*globalIllumination);
				if (bGlobalIlluminationPayloadReady &&
					sharedResources->m_uploadedGlobalIlluminationStates !=
						stateSignature)
				{
					TVector<RHIGlobalIlluminationGpuState> states;
					bGlobalIlluminationPayloadReady =
						BuildGlobalIlluminationGpuStates(
							*globalIllumination,
							states,
							globalIlluminationDiagnostic);
					if (bGlobalIlluminationPayloadReady)
					{
						const uint64_t stateBytes =
							static_cast<uint64_t>(states.Num()) *
								sizeof(RHIGlobalIlluminationGpuState);
						commands->UpdateShaderBinding(
							transferCommandList,
							sharedResources->m_globalIlluminationStorage
								->GetOrAddShaderBinding("globalIlluminationStates"),
							states.GetData(),
							states.Num() *
								sizeof(RHIGlobalIlluminationGpuState),
							0u);
						sharedResources->m_uploadedGlobalIlluminationStates =
							stateSignature;
						if (globalIlluminationStats)
						{
							globalIlluminationStats->m_copiedCpuBytes += stateBytes;
							globalIlluminationStats->m_uploadedGpuBytes += stateBytes;
						}
					}
				}
			}

			if (bHasGlobalIllumination && !bGlobalIlluminationPayloadReady)
			{
				SAILOR_LOG_ERROR(
					"Cannot publish Global Illumination ECS GPU snapshot: %s.",
					globalIlluminationDiagnostic.c_str());
			}
			const RHIGlobalIlluminationGpuHeader header =
				BuildGlobalIlluminationGpuHeader(
					bGlobalIlluminationPayloadReady
						? globalIllumination.GetRawPtr()
						: nullptr,
					ResolveGlobalIlluminationDebug(snapshot.m_renderMode),
					snapshot.m_globalIlluminationMode,
					snapshot.m_bGlobalIlluminationEnabled);
			const uint64_t headerHash = HashSubmissionValue(header);
			if (sharedResources->m_uploadedGlobalIlluminationHeader !=
				headerHash)
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					sharedResources->m_globalIlluminationStorage
						->GetOrAddShaderBinding("globalIlluminationHeader"),
					&header,
					sizeof(header),
					0u);
				sharedResources->m_uploadedGlobalIlluminationHeader =
					headerHash;
				if (globalIlluminationStats)
				{
					globalIlluminationStats->m_copiedCpuBytes += sizeof(header);
					globalIlluminationStats->m_uploadedGpuBytes += sizeof(header);
				}
			}
			if (globalIlluminationStats)
			{
				globalIlluminationStats->m_bActive =
					bGlobalIlluminationPayloadReady &&
					globalIlluminationStats->m_bEnabled;
				globalIlluminationStats->m_loadedBricks =
					bGlobalIlluminationPayloadReady
						? globalIlluminationStats->m_totalBricks
						: 0u;
			}
		}

		size_t frameGraphSamplerHash = 0u;
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_irradianceCubemap")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_brdfSampler")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_envCubemap")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_sheenEnvCubemap")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_localEnvCubemap")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetSampler("g_localSheenEnvCubemap")));
		HashCombine(frameGraphSamplerHash, Sailor::GetHash(owner->GetRenderTarget("g_AO")));
		const bool bRecreateLights = !resources->m_lightsBindings ||
			bRecreateLightCulling ||
			resources->m_lightsTemplate != lightsTemplate ||
			resources->m_sharedLightsStorage != sharedResources->m_lightsStorage ||
			resources->m_sharedGlobalIlluminationStorage !=
				sharedResources->m_globalIlluminationStorage ||
			resources->m_lightsTemplateRevision != lightsTemplateRevision ||
			resources->m_frameGraphSamplerHash != frameGraphSamplerHash ||
			resources->m_shadowMatrixCapacity < numShadowMatrices ||
			resources->m_shadowIndexCapacity < numShadowIndices ||
			resources->m_shadowAtlasTileCapacity < numShadowAtlasTiles;

		if (bRecreateLights)
		{
			resources->m_shadowMatrixCapacity = GrowSubmissionCapacity(resources->m_shadowMatrixCapacity, numShadowMatrices);
			resources->m_shadowIndexCapacity = GrowSubmissionCapacity(resources->m_shadowIndexCapacity, numShadowIndices);
			resources->m_shadowAtlasTileCapacity = GrowSubmissionCapacity(resources->m_shadowAtlasTileCapacity, numShadowAtlasTiles);
			resources->m_lightsBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(resources->m_lightsBindings, "localReflection",
				sizeof(LocalReflectionParameters), 20u, EShaderBindingType::UniformBuffer);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_lightsStorage->GetOrAddShaderBinding("light"),
				"light",
				0u);
			if (resources->m_lightCullingBindings)
			{
				driver->AddShaderBinding(
					resources->m_lightsBindings,
					resources->m_lightCullingBindings->GetOrAddShaderBinding("culledLights"),
					"culledLights",
					1u);
				driver->AddShaderBinding(
					resources->m_lightsBindings,
					resources->m_lightCullingBindings->GetOrAddShaderBinding("lightsGrid"),
					"lightsGrid",
					2u);
			}
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"lightsMatrices",
				sizeof(glm::mat4),
				resources->m_shadowMatrixCapacity,
				6u,
				true);
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"shadowIndices",
				sizeof(uint32_t),
				resources->m_shadowIndexCapacity,
				7u,
				true);
			driver->AddSsboToShaderBindings(
				resources->m_lightsBindings,
				"shadowAtlasTiles",
				sizeof(uint32_t),
				resources->m_shadowAtlasTileCapacity,
				11u,
				true);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationHeader"),
				"globalIlluminationHeader",
				12u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationBvh"),
				"globalIlluminationBvh",
				13u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationBricks"),
				"globalIlluminationBricks",
				14u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationProbes"),
				"globalIlluminationProbes",
				15u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationCoefficients"),
				"globalIlluminationCoefficients",
				16u);
			driver->AddShaderBinding(
				resources->m_lightsBindings,
				sharedResources->m_globalIlluminationStorage
					->GetOrAddShaderBinding("globalIlluminationStates"),
				"globalIlluminationStates",
				17u);
			CloneTextureBindings(lightsTemplate, resources->m_lightsBindings);
			if (auto texture = owner->GetSampler("g_irradianceCubemap"))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_irradianceCubemap",
					texture,
					3u);
			}
			if (auto texture = owner->GetSampler("g_brdfSampler"))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_brdfSampler",
					texture,
					4u);
			}
			if (auto texture = owner->GetSampler("g_envCubemap"))
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_envCubemap",
					texture,
					5u);
			}
			auto sheenEnvironment = owner->GetSampler("g_sheenEnvCubemap");
			if (!sheenEnvironment)
			{
				sheenEnvironment = owner->GetSampler("g_envCubemap");
			}
			if (sheenEnvironment)
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_sheenEnvCubemap",
					sheenEnvironment,
					19u);
			}
			if (auto texture = owner->GetRenderTarget("g_AO"))
			{
				driver->AddSamplerToShaderBindings(resources->m_lightsBindings, "g_aoSampler", texture, 8u);
			}
			auto localEnvironment = owner->GetSampler("g_localEnvCubemap");
			if (!localEnvironment) localEnvironment = owner->GetSampler("g_envCubemap");
			if (localEnvironment)
				driver->AddSamplerToShaderBindings(resources->m_lightsBindings, "g_localEnvCubemap", localEnvironment, 21u);
			auto localSheen = owner->GetSampler("g_localSheenEnvCubemap");
			if (!localSheen) localSheen = sheenEnvironment;
			if (localSheen)
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_localSheenEnvCubemap", localSheen, 22u);
			}
			if (auto texture = driver->GetDefaultTexture())
			{
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_transmissionFramebufferSampler",
					texture,
					10u);
				driver->AddSamplerToShaderBindings(
					resources->m_lightsBindings,
					"g_globalIlluminationProbeCellIndicesSampler",
					texture,
					18u);
			}
			resources->m_lightsBindings->RecalculateCompatibility();
			resources->m_lightsTemplate = lightsTemplate;
			resources->m_sharedLightsStorage = sharedResources->m_lightsStorage;
			resources->m_sharedGlobalIlluminationStorage =
				sharedResources->m_globalIlluminationStorage;
			resources->m_lightsTemplateRevision = lightsTemplateRevision;
			resources->m_frameGraphSamplerHash = frameGraphSamplerHash;
			resources->m_shadowMatricesHash = InvalidContentHash;
			resources->m_shadowIndicesHash = InvalidContentHash;
			resources->m_shadowAtlasTilesHash = InvalidContentHash;
		}

		LocalReflectionParameters localParameters{};
		if (auto environment = owner->GetGraphNode("Environment").DynamicCast<EnvironmentNode>())
			localParameters = environment->GetLocalReflectionParameters();
		commands->UpdateShaderBinding(transferCommandList,
			resources->m_lightsBindings->GetOrAddShaderBinding("localReflection"),
			&localParameters, sizeof(localParameters), 0u);

		const uint64_t shadowMatricesHash = HashSubmissionValues(snapshot.m_shadowMatrices);
		if (resources->m_shadowMatricesHash != shadowMatricesHash)
		{
			if (!snapshot.m_shadowMatrices.IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					resources->m_lightsBindings->GetOrAddShaderBinding("lightsMatrices"),
					snapshot.m_shadowMatrices.GetData(),
					snapshot.m_shadowMatrices.Num() * sizeof(glm::mat4),
					0u);
			}
			resources->m_shadowMatricesHash = shadowMatricesHash;
		}

		const uint64_t shadowIndicesHash = HashSubmissionValues(snapshot.m_shadowIndices);
		if (resources->m_shadowIndicesHash != shadowIndicesHash)
		{
			if (!snapshot.m_shadowIndices.IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					resources->m_lightsBindings->GetOrAddShaderBinding("shadowIndices"),
					snapshot.m_shadowIndices.GetData(),
					snapshot.m_shadowIndices.Num() * sizeof(uint32_t),
					0u);
			}
			resources->m_shadowIndicesHash = shadowIndicesHash;
		}

		const uint64_t shadowAtlasTilesHash = HashSubmissionValues(snapshot.m_shadowAtlasTiles);
		if (resources->m_shadowAtlasTilesHash != shadowAtlasTilesHash)
		{
			if (!snapshot.m_shadowAtlasTiles.IsEmpty())
			{
				commands->UpdateShaderBinding(
					transferCommandList,
					resources->m_lightsBindings->GetOrAddShaderBinding("shadowAtlasTiles"),
					snapshot.m_shadowAtlasTiles.GetData(),
					snapshot.m_shadowAtlasTiles.Num() * sizeof(uint32_t),
					0u);
			}
			resources->m_shadowAtlasTilesHash = shadowAtlasTilesHash;
		}

		snapshot.m_rhiLightsData = resources->m_lightsBindings;

		const size_t numBoneMatrices = snapshot.m_cpuBoneMatrices ? snapshot.m_cpuBoneMatrices->Num() : 0u;
		if (numBoneMatrices == 0u)
		{
			snapshot.m_boneMatrices.Clear();
			return;
		}

		const bool bRecreateBones = !sharedResources->m_boneBindings ||
			sharedResources->m_boneCapacity < numBoneMatrices;
		if (bRecreateBones)
		{
			sharedResources->m_boneCapacity = GrowSubmissionCapacity(
				sharedResources->m_boneCapacity,
				numBoneMatrices);
			sharedResources->m_boneBindings = driver->CreateShaderBindings();
			driver->AddSsboToShaderBindings(
				sharedResources->m_boneBindings,
				"bones",
				sizeof(glm::mat4),
				sharedResources->m_boneCapacity,
				0u,
				true);
			sharedResources->m_boneBindings->RecalculateCompatibility();
			sharedResources->m_uploadedAnimationRevision = InvalidContentHash;
			sharedResources->m_bonesSource.Clear();
		}

		if (bUploadSharedPayload &&
			(sharedResources->m_bonesSource != snapshot.m_cpuBoneMatrices ||
				sharedResources->m_uploadedAnimationRevision != snapshot.m_animationRevision))
		{
			commands->UpdateShaderBinding(
				transferCommandList,
				sharedResources->m_boneBindings->GetOrAddShaderBinding("bones"),
				snapshot.m_cpuBoneMatrices->GetData(),
				numBoneMatrices * sizeof(glm::mat4),
				0u);
			sharedResources->m_bonesSource = snapshot.m_cpuBoneMatrices;
			sharedResources->m_uploadedAnimationRevision = snapshot.m_animationRevision;
		}
		snapshot.m_boneMatrices = sharedResources->m_boneBindings;
	}
}

RHIGlobalIlluminationRenderStats
RHIFrameGraph::GetGlobalIlluminationRenderStats() const
{
	auto& storage = GetGlobalIlluminationRenderStatsStorage();
	storage.m_lock.Lock();
	const RHIGlobalIlluminationRenderStats result =
		storage.m_owner == this
			? storage.m_stats
			: RHIGlobalIlluminationRenderStats{};
	storage.m_lock.Unlock();
	return result;
}

void RHIFrameGraph::Clear()
{
	ResetCurrentDepthPyramids();
	m_motionHistory.Clear();
	m_samplers.Clear();
	m_graph.Clear();
	m_values.Clear();
	m_renderTargets.Clear();
	m_surfaces.Clear();
}

FrameGraphNodePtr RHIFrameGraph::GetGraphNode(const std::string& tag)
{
	const size_t index = m_graph.FindIf([&](const auto& lhs) { return lhs->GetTag() == tag; });
	if (index != -1)
	{
		return m_graph[index];
	}

	return nullptr;
}

void RHIFrameGraph::SetSampler(const std::string& name, RHI::RHITexturePtr sampler)
{
	m_samplers[name] = sampler;
}

void RHIFrameGraph::SetRenderTarget(const std::string& name, RHI::RHIRenderTargetPtr sampler)
{
	m_renderTargets[name] = sampler;
}

void RHIFrameGraph::SetSurface(const std::string& name, RHI::RHISurfacePtr surface)
{
	m_surfaces[name] = surface;
}

glm::ivec2 RHIFrameGraph::GetSceneRenderExtent()
{
	if (const auto debugDraw = GetGraphNode("DebugDraw"))
	{
		if (const auto colorAttachment = debugDraw->GetResolvedAttachment("color"))
		{
			const glm::ivec2 extent = colorAttachment->GetExtent();
			return glm::ivec2(
				(std::max)(extent.x, 1),
				(std::max)(extent.y, 1));
		}
	}

	if (const auto mainSurface = GetSurface("Main"))
	{
		const auto colorAttachment = mainSurface->GetResolved() ?
			mainSurface->GetResolved() : mainSurface->GetTarget();
		if (colorAttachment)
		{
			const glm::ivec2 extent = colorAttachment->GetExtent();
			return glm::ivec2(
				(std::max)(extent.x, 1),
				(std::max)(extent.y, 1));
		}
	}

	if (const auto mainTarget = GetRenderTarget("Main"))
	{
		const glm::ivec2 extent = mainTarget->GetExtent();
		return glm::ivec2(
			(std::max)(extent.x, 1),
			(std::max)(extent.y, 1));
	}

	const glm::ivec2 viewportExtent = App::GetMainWindow()->GetRenderArea();
	const Settings::GraphicsExtent fallbackExtent =
		Settings::ResolveRenderDimensions(
			static_cast<uint32_t>((std::max)(viewportExtent.x, 1)),
			static_cast<uint32_t>((std::max)(viewportExtent.y, 1)),
			App::GetActiveGraphicsSettings().m_resolutionFactor);
	return glm::ivec2(
		static_cast<int32_t>(fallbackExtent.m_width),
		static_cast<int32_t>(fallbackExtent.m_height));
}

void RHIFrameGraph::FillFrameData(RHI::RHICommandListPtr transferCmdList, RHI::RHISceneViewSnapshot& snapshot, WorldPtr world, float worldTime)
{
	SAILOR_PROFILE_FUNCTION();

	auto frameData = CaptureMotionHistory(snapshot, world, worldTime, GetSceneRenderExtent()).m_frameData;
	const auto& previousFrame = snapshot.m_previousMotionFrame ?
		snapshot.m_previousMotionFrame->m_frameData : frameData;
	frameData.m_deltaTime = snapshot.m_previousMotionFrame ?
		worldTime - previousFrame.m_currentTime : 0.0f;

	if (!snapshot.m_frameBindings)
	{
		snapshot.m_frameBindings = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(snapshot.m_frameBindings, "frameData", sizeof(RHI::UboFrameData), 0, RHI::EShaderBindingType::UniformBuffer);
		Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(snapshot.m_frameBindings, "previousFrameData", sizeof(RHI::UboFrameData), 1, RHI::EShaderBindingType::UniformBuffer);
	}

	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList, snapshot.m_frameBindings->GetOrAddShaderBinding("frameData"), &frameData, sizeof(frameData));
	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList, snapshot.m_frameBindings->GetOrAddShaderBinding("previousFrameData"), &previousFrame, sizeof(previousFrame));

	auto resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<RHIViewSubmissionResources>(this, snapshot.m_cameraIndex, 0u);
	const auto bones = snapshot.m_previousMotionFrame ? snapshot.m_previousMotionFrame->m_bones : snapshot.m_cpuBoneMatrices;
	const size_t count = bones ? bones->Num() : 0u;
	if (resources->m_previousBoneCapacity < (std::max)(size_t{ 1u }, count))
	{
		resources->m_previousBoneCapacity = GrowSubmissionCapacity(resources->m_previousBoneCapacity, (std::max)(size_t{ 1u }, count));
		Renderer::GetDriver()->AddSsboToShaderBindings(snapshot.m_frameBindings, "previousBones",
			sizeof(glm::mat4), resources->m_previousBoneCapacity, 2u, true);
	}
	const glm::mat4 identity(1.0f);
	Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList,
		snapshot.m_frameBindings->GetOrAddShaderBinding("previousBones"),
		count ? bones->GetData() : &identity, (std::max)(size_t{ 1u }, count) * sizeof(glm::mat4));

}

void RHIFrameGraph::CompleteMotionHistory(RHI::RHISceneViewPtr sceneView, bool succeeded)
{
	if (!succeeded)
	{
		m_motionHistory.Clear();
		return;
	}
	m_motionHistory.Resize(sceneView->m_snapshots.Num());
	for (const auto& snapshot : sceneView->m_snapshots)
	{
		m_motionHistory[snapshot.m_cameraIndex] = TSharedPtr<RHIMotionHistoryFrame>::Make(
			CaptureMotionHistory(snapshot, sceneView->m_world,
				sceneView->m_currentTime, GetSceneRenderExtent()));
	}
}

TVector<Sailor::Tasks::TaskPtr<void, void>> RHIFrameGraph::Prepare(RHI::RHISceneViewPtr rhiSceneView)
{
	TVector<Sailor::Tasks::TaskPtr<void, void>> res;

	auto frameRefPtr = this->ToRefPtr<RHIFrameGraph>();
	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		snapshot.m_previousMotionFrame.Clear();
		if (snapshot.m_cameraIndex < m_motionHistory.Num() && m_motionHistory[snapshot.m_cameraIndex])
		{
			const auto current = CaptureMotionHistory(snapshot, rhiSceneView->m_world,
				rhiSceneView->m_currentTime, GetSceneRenderExtent());
			if (IsMotionHistoryContinuous(*m_motionHistory[snapshot.m_cameraIndex], current))
			{
				snapshot.m_previousMotionFrame = m_motionHistory[snapshot.m_cameraIndex];
			}
		}
		for (auto& node : m_graph)
		{
			auto task = node->Prepare(frameRefPtr, snapshot);
			if (task.IsValid())
			{
				res.Emplace(std::move(task));
			}
		}
	}

	return res;
}

bool RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView,
	TVector<RHI::RHICommandListPtr>& outTransferCommandLists,
	TVector<RHI::RHICommandListPtr>& outCommandLists,
	RHISemaphorePtr inSignalSemaphore,
	RHISemaphorePtr& outWaitSemaphore)
{
	SAILOR_PROFILE_FUNCTION();
	m_drawCallStats = {};
	RHIGlobalIlluminationRenderStats globalIlluminationRenderStats;

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto& driver = RHI::Renderer::GetDriver();
	auto driverCommands = renderer->GetDriverCommands();
	RHISemaphorePtr frameGraphChainSemaphore = inSignalSemaphore;
	auto submissionProgress = TSharedPtr<RHISubmissionProgress>::Make();
	submissionProgress->SetLastSuccessfulSemaphore(inSignalSemaphore);
	outWaitSemaphore = inSignalSemaphore;

	if (!rhiSceneView->m_snapshots.IsEmpty() &&
		rhiSceneView->m_snapshots[0].m_submissionContext)
	{
		auto resourceUploadCommandList = renderer->GetDriver()->CreateCommandList(
			false,
			RHI::ECommandListQueue::Compute);
		driver->SetDebugName(resourceUploadCommandList, "FrameGraph:SharedResourceUpload");
		driverCommands->BeginCommandList(resourceUploadCommandList, true);
		PrepareViewSubmissionResources(
			this,
			resourceUploadCommandList,
			rhiSceneView->m_snapshots[0],
			true,
			&globalIlluminationRenderStats);
		const bool bHasSharedResourceUploads =
			resourceUploadCommandList->GetNumRecordedCommands() > 0u;
		driverCommands->EndCommandList(resourceUploadCommandList);

		if (bHasSharedResourceUploads)
		{
			auto resourceReadySemaphore = driver->CreateWaitSemaphore();
			auto resourceUploadFence = RHIFencePtr::Make();
			driver->SetDebugName(resourceReadySemaphore, "FrameGraph:SharedResourceReady");
			driver->SetDebugName(resourceUploadFence, "FrameGraph:SharedResourceUpload");
			if (!driver->SubmitCommandList(
					resourceUploadCommandList,
					resourceUploadFence,
					resourceReadySemaphore,
					frameGraphChainSemaphore))
			{
				SAILOR_LOG_ERROR("RHIFrameGraph::Process: failed to submit shared resource upload command buffer.");
				return false;
			}

			frameGraphChainSemaphore = resourceReadySemaphore;
			submissionProgress->SetLastSuccessfulSemaphore(resourceReadySemaphore);
		}
	}

	if (!m_postEffectPlane)
	{
		m_postEffectPlane = renderer->GetDriver()->CreateMesh();
		m_postEffectPlane->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		m_postEffectPlane->m_bounds = Math::AABB(vec3(0), vec3(1, 1, 1));

		TVector<VertexP3N3UV2C4> ndcQuad(4);
		ndcQuad[0].m_texcoord = vec2(0.0f, 0.0f);
		ndcQuad[1].m_texcoord = vec2(1.0f, 0.0f);
		ndcQuad[2].m_texcoord = vec2(0.0f, 1.0f);
		ndcQuad[3].m_texcoord = vec2(1.0f, 1.0f);

		ndcQuad[0].m_position = vec3(-1.0f, -1.0f, 0.0f);
		ndcQuad[1].m_position = vec3(1.0f, -1.0f, 0.0f);
		ndcQuad[2].m_position = vec3(-1.0f, 1.0f, 0.0f);
		ndcQuad[3].m_position = vec3(1.0f, 1.0f, 0.0f);

		const TVector<uint32_t> indices = { 0, 1, 2, 2, 1, 3 };

		RHI::Renderer::GetDriver()->UpdateMesh(m_postEffectPlane, &ndcQuad[0], ndcQuad.Num() * sizeof(VertexP3N3UV2C4), &indices[0], sizeof(uint32_t) * indices.Num());
	}

	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		SAILOR_PROFILE_SCOPE("Process snapshot");
		ResetCurrentDepthPyramids();

		auto cmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
		auto transferCmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Compute);

		driver->SetDebugName(cmdList, "FrameGraph:Graphics");
		driver->SetDebugName(transferCmdList, "FrameGraph:Transfer");

		driverCommands->BeginCommandList(cmdList, true);
		uint32_t graphicsGpuFrameTimeRange =
			driver->BeginGpuFrameTimeRange(cmdList);
		driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics", glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));
		driverCommands->BeginCommandList(transferCmdList, true);
		uint32_t transferGpuFrameTimeRange =
			driver->BeginGpuFrameTimeRange(transferCmdList);
		driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer", glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));

		PrepareViewSubmissionResources(
			this,
			transferCmdList,
			snapshot,
			false,
			nullptr);

		driverCommands->BeginDebugRegion(transferCmdList, "Fill Frame Data", DebugContext::Color_CmdTransfer);
		{
			FillFrameData(transferCmdList, snapshot, rhiSceneView->m_world, rhiSceneView->m_currentTime);
		}
		driverCommands->EndDebugRegion(transferCmdList);

		RHI::RHISemaphorePtr chainSemaphore = frameGraphChainSemaphore;
		auto submitsSucceeded = TSharedPtr<std::atomic_bool>::Make(true);

		TVector<Tasks::ITaskPtr> tasks;
		tasks.Reserve(2);

		auto frameRefPtr = this->ToRefPtr<RHIFrameGraph>();

		// Self balancing barriers
		{
			SAILOR_PROFILE_SCOPE("Change default image layout to decrease barriers count");

			driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Decrease barriers count", glm::vec4(1.0f, 0.75f, 0.75f, 0.1f));

			for (const auto& stat : m_lastFrameGpuStats.m_barriers)
			{
				RHITexturePtr texture = stat.m_first;

				// We don't pay attention to depth stencil targets 
				// since they must be in the DepthStencilAttachmentStencil in the end of frame
				if (RHI::IsDepthFormat(texture->GetFormat()))
					continue;

				EImageLayout bestLayout = stat.m_first->GetDefaultLayout();

				uint max = 1;
				if (auto cubemap = texture.DynamicCast<RHICubemap>())
				{
					max = 6 * std::max(1u, cubemap->GetMipLevels());
				}
				else if (auto renderTarget = texture.DynamicCast<RHIRenderTarget>())
				{
					max = std::max(1u, renderTarget->GetMipLevels());
				}

				for (const auto& layout : *stat.Second())
				{
					if (*layout.Second() > max)
					{
						bestLayout = layout.m_first;
						max = *layout.Second();
					}
				}

				if (texture->GetDefaultLayout() != bestLayout)
				{
					driverCommands->ImageMemoryBarrier(cmdList, texture, bestLayout);
					texture->ForceSetDefaultLayout(bestLayout);
				}
			}

			driverCommands->EndDebugRegion(cmdList);

			m_lastFrameGpuStats.m_barriers.Clear();
		}

		const bool bExecuteQueries = driver->StartGpuTracking();

		uint32_t nodeIndex = 0u;
		for (auto& node : m_graph)
		{
			uint32_t graphicsTimestamp = RHI::InvalidGpuTimestampQuery;
			uint32_t computeTimestamp = RHI::InvalidGpuTimestampQuery;
			if (bExecuteQueries)
			{
				std::string timingName =
					std::format("{:02d} {}", nodeIndex, node->GetTag());
				std::string renderQueueTag;
				if (node->TryGetString("Tag", renderQueueTag) &&
					!renderQueueTag.empty())
				{
					timingName += "/" + renderQueueTag;
				}
				std::string shader;
				if (node->TryGetString("shader", shader) && !shader.empty())
				{
					timingName += "/" + shader;
				}

				graphicsTimestamp = driverCommands->BeginGpuTimestamp(
					cmdList,
					timingName);
				computeTimestamp = driverCommands->BeginGpuTimestamp(
					transferCmdList,
					timingName);
			}

			node->Process(frameRefPtr, transferCmdList, cmdList, snapshot);
			if (bExecuteQueries)
			{
				driverCommands->EndGpuTimestamp(
					transferCmdList,
					computeTimestamp);
				driverCommands->EndGpuTimestamp(
					cmdList,
					graphicsTimestamp);
			}
			m_drawCallStats += node->GetDrawCallStats();
			++nodeIndex;

			const uint32_t numRecordedCommands = transferCmdList->GetNumRecordedCommands() + cmdList->GetNumRecordedCommands();
			const uint32_t gpuCost = transferCmdList->GetGPUCost() + cmdList->GetGPUCost();
			if (gpuCost > MaxGpuCost || numRecordedCommands > MaxRecordedCommands)
			{
				SAILOR_PROFILE_SCOPE("Chaining command lists");

				driverCommands->EndDebugRegion(cmdList);
				driver->EndGpuFrameTimeRange(
					cmdList,
					graphicsGpuFrameTimeRange);
				driverCommands->EndCommandList(cmdList);

				driverCommands->EndDebugRegion(transferCmdList);
				driver->EndGpuFrameTimeRange(
					transferCmdList,
					transferGpuFrameTimeRange);
				driverCommands->EndCommandList(transferCmdList);

				// Create tasks
				{
					SAILOR_PROFILE_SCOPE("Create RHI submit cmd lists tasks");
					RHI::RHISemaphorePtr newChainSemaphore = driver->CreateWaitSemaphore();
					driver->SetDebugName(newChainSemaphore, "FrameGraph: newChainSemaphore");

					tasks.RemoveAll([](const auto& task) { return task == nullptr || task->IsFinished(); });

					auto submitCmdList1 = Tasks::CreateTask("Submit chaining cmd lists",
						[=]()
						{
							if (!submitsSucceeded->load(std::memory_order_acquire))
							{
								return;
							}

							auto fence = RHIFencePtr::Make();
							RHI::Renderer::GetDriver()->SetDebugName(fence, std::format("Submit chaining cmd lists"));
							if (!RHI::Renderer::GetDriver()->SubmitCommandList(transferCmdList, fence, newChainSemaphore, chainSemaphore))
							{
								SAILOR_LOG_ERROR("RHIFrameGraph::Process: failed to submit a chained transfer command buffer.");
								submitsSucceeded->store(false, std::memory_order_release);
							}
							else
							{
								submissionProgress->SetLastSuccessfulSemaphore(newChainSemaphore);
							}
						}, EThreadType::RHI);

					if (tasks.Num() > 0)
					{
						submitCmdList1->Join(tasks[tasks.Num() - 1]);
					}

					submitCmdList1->Run();

					chainSemaphore = driver->CreateWaitSemaphore();
					driver->SetDebugName(chainSemaphore, "FrameGraph: chainSemaphore");

					auto submitCmdList2 = Tasks::CreateTask("Submit chaining cmd lists",
						[=]()
						{
							if (!submitsSucceeded->load(std::memory_order_acquire))
							{
								return;
							}

							auto fence = RHIFencePtr::Make();
							RHI::Renderer::GetDriver()->SetDebugName(fence, std::format("Submit chaining cmd lists"));
							if (!RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, fence, chainSemaphore, newChainSemaphore))
							{
								SAILOR_LOG_ERROR("RHIFrameGraph::Process: failed to submit a chained graphics command buffer.");
								submitsSucceeded->store(false, std::memory_order_release);
							}
							else
							{
								submissionProgress->SetLastSuccessfulSemaphore(chainSemaphore);
							}
						}, EThreadType::RHI);

					submitCmdList2->Join(submitCmdList1);
					submitCmdList2->Run();

					tasks.AddRange({ submitCmdList1, submitCmdList2 });
				}

				// New command lists
				{
					SAILOR_PROFILE_SCOPE("Create new command lists");

					cmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
					transferCmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Compute);

					driver->SetDebugName(cmdList, "FrameGraph:Graphics");
					driver->SetDebugName(transferCmdList, "FrameGraph:Transfer");

					driverCommands->BeginCommandList(cmdList, true);
					graphicsGpuFrameTimeRange =
						driver->BeginGpuFrameTimeRange(cmdList);
					driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics", glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));

					driverCommands->BeginCommandList(transferCmdList, true);
					transferGpuFrameTimeRange =
						driver->BeginGpuFrameTimeRange(transferCmdList);
					driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer", glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));
				}
			}
			//TODO: Submit Transfer command lists
		}

		driverCommands->EndDebugRegion(cmdList);
		driver->EndGpuFrameTimeRange(
			cmdList,
			graphicsGpuFrameTimeRange);
		driverCommands->EndCommandList(cmdList);

		driverCommands->EndDebugRegion(transferCmdList);
		driver->EndGpuFrameTimeRange(
			transferCmdList,
			transferGpuFrameTimeRange);
		driverCommands->EndCommandList(transferCmdList);

		{
			SAILOR_PROFILE_SCOPE("Wait for submitting of chaining command lists");
			for (auto& task : tasks)
			{
				task->Wait();
			}
		}

		if (!submitsSucceeded->load(std::memory_order_acquire))
		{
			m_lastFrameGpuStats = driver->FinishGpuTracking();
			outWaitSemaphore = submissionProgress->GetLastSuccessfulSemaphore();
			return false;
		}

		m_lastFrameGpuStats = driver->FinishGpuTracking();

		frameGraphChainSemaphore = chainSemaphore;
		outCommandLists.Emplace(std::move(cmdList));
		outTransferCommandLists.Emplace(transferCmdList);
	}

	outWaitSemaphore = frameGraphChainSemaphore;
	PublishGlobalIlluminationRenderStats(
		this,
		globalIlluminationRenderStats);
	return true;
}

RHI::RHITexturePtr RHIFrameGraph::GetSampler(const std::string& name)
{
	if (!m_samplers.ContainsKey(name))
	{
		return RHITexturePtr();
	}

	return m_samplers[name];
}

RHI::RHIRenderTargetPtr RHIFrameGraph::GetRenderTarget(const std::string& name)
{
	if (!m_renderTargets.ContainsKey(name))
	{
		return nullptr;
	}

	return m_renderTargets[name];
}

RHI::RHISurfacePtr RHIFrameGraph::GetSurface(const std::string& name)
{
	if (!m_surfaces.ContainsKey(name))
	{
		return nullptr;
	}

	return m_surfaces[name];
}
