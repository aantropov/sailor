#include "ViewSubmissionResources.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "Containers/Hash.h"
#include "Settings/GraphicsSettings.h"
#include "Core/LogMacros.h"

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
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
}

void Sailor::RHI::UploadSharedLighting(RHICommandListPtr commandList,
	const RHISceneViewSnapshot& snapshot, RHISharedViewSubmissionResources& resources)
{
	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	const size_t numLights = snapshot.m_cpuLightsData ? snapshot.m_cpuLightsData->Num() : 0u;
	const bool bRecreateLightStorage = !resources.m_lightsStorage ||
		resources.m_lightCapacity < numLights;
	if (bRecreateLightStorage)
	{
		resources.m_lightCapacity = GrowSubmissionCapacity(
			resources.m_lightCapacity,
			numLights);
		resources.m_lightsStorage = driver->CreateShaderBindings();
		driver->AddSsboToShaderBindings(
			resources.m_lightsStorage,
			"light",
			sizeof(RHILightShaderData),
			resources.m_lightCapacity,
			0u,
			true);
		resources.m_lightsStorage->RecalculateCompatibility();
		resources.m_uploadedLightingRevision = InvalidContentHash;
		resources.m_lightsSource.Clear();
	}
	if (snapshot.m_cpuLightsData &&
		(resources.m_lightsSource != snapshot.m_cpuLightsData ||
			resources.m_uploadedLightingRevision != snapshot.m_lightingRevision))
	{
		if (!snapshot.m_cpuLightsData->IsEmpty())
		{
			commands->UpdateShaderBinding(
				commandList,
				resources.m_lightsStorage->GetOrAddShaderBinding("light"),
				snapshot.m_cpuLightsData->GetData(),
				snapshot.m_cpuLightsData->Num() * sizeof(RHILightShaderData),
				0u);
		}
		resources.m_lightsSource = snapshot.m_cpuLightsData;
		resources.m_uploadedLightingRevision = snapshot.m_lightingRevision;
	}
}

RHIGlobalIlluminationRenderStats Sailor::RHI::UploadGlobalIllumination(RHICommandListPtr commandList,
	const RHISceneViewSnapshot& snapshot, RHISharedViewSubmissionResources& resources)
{
	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	const auto& globalIllumination = snapshot.m_globalIllumination;
	auto stats = BuildGlobalIlluminationRenderStats(globalIllumination.GetRawPtr());
	stats.m_mode = snapshot.m_globalIlluminationMode;
	stats.m_bEnabled = snapshot.m_bGlobalIlluminationEnabled;
	stats.m_flightSlot = snapshot.m_submissionContext->GetFlightSlot();
	if (!globalIllumination)
	{
		stats.m_qualityBudget =
			App::GetActiveGraphicsSettings()
				.m_maxGiProbeStatesPerSnapshot;
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
		!resources.m_globalIlluminationStorage ||
		resources.m_globalIlluminationNodeCapacity <
			numGlobalIlluminationNodes ||
		resources.m_globalIlluminationBrickCapacity <
			numGlobalIlluminationBricks ||
		resources.m_globalIlluminationProbeCapacity <
			numGlobalIlluminationProbes ||
		resources.m_globalIlluminationCoefficientCapacity <
			numGlobalIlluminationCoefficients ||
		resources.m_globalIlluminationStateCapacity <
			numGlobalIlluminationStates;
	if (bRecreateGlobalIlluminationStorage)
	{
		resources.m_globalIlluminationNodeCapacity =
			GrowSubmissionCapacity(
				resources.m_globalIlluminationNodeCapacity,
				numGlobalIlluminationNodes);
		resources.m_globalIlluminationBrickCapacity =
			GrowSubmissionCapacity(
				resources.m_globalIlluminationBrickCapacity,
				numGlobalIlluminationBricks);
		resources.m_globalIlluminationProbeCapacity =
			GrowSubmissionCapacity(
				resources.m_globalIlluminationProbeCapacity,
				numGlobalIlluminationProbes);
		resources.m_globalIlluminationCoefficientCapacity =
			GrowSubmissionCapacity(
				resources.m_globalIlluminationCoefficientCapacity,
				numGlobalIlluminationCoefficients);
		resources.m_globalIlluminationStateCapacity =
			GrowSubmissionCapacity(
				resources.m_globalIlluminationStateCapacity,
				numGlobalIlluminationStates);
		resources.m_globalIlluminationStorage =
			driver->CreateShaderBindings();
		driver->AddSsboToShaderBindings(
			resources.m_globalIlluminationStorage,
			"globalIlluminationHeader",
			sizeof(RHIGlobalIlluminationGpuHeader),
			1u,
			0u,
			true);
		driver->AddSsboToShaderBindings(
			resources.m_globalIlluminationStorage,
			"globalIlluminationBvh",
			sizeof(RHIGlobalIlluminationGpuBvhNode),
			resources.m_globalIlluminationNodeCapacity,
			1u,
			true);
		driver->AddSsboToShaderBindings(
			resources.m_globalIlluminationStorage,
			"globalIlluminationBricks",
			sizeof(RHIGlobalIlluminationGpuBrick),
			resources.m_globalIlluminationBrickCapacity,
			2u,
			true);
		driver->AddSsboToShaderBindings(
			resources.m_globalIlluminationStorage,
			"globalIlluminationProbes",
			sizeof(RHIGlobalIlluminationGpuProbe),
			resources.m_globalIlluminationProbeCapacity,
			3u,
			true);
		driver->AddSsboToShaderBindings(
			resources.m_globalIlluminationStorage,
			"globalIlluminationCoefficients",
			sizeof(RHIGlobalIlluminationGpuCoefficients),
			resources.m_globalIlluminationCoefficientCapacity,
			4u,
			true);
		driver->AddSsboToShaderBindings(
			resources.m_globalIlluminationStorage,
			"globalIlluminationStates",
			sizeof(RHIGlobalIlluminationGpuState),
			resources.m_globalIlluminationStateCapacity,
			5u,
			true);
		resources.m_globalIlluminationStorage
			->RecalculateCompatibility();
		resources.m_uploadedGlobalIlluminationLayout =
			InvalidContentHash;
		resources.m_uploadedGlobalIlluminationCoefficients =
			InvalidContentHash;
		resources.m_uploadedGlobalIlluminationStates =
			InvalidContentHash;
		resources.m_uploadedGlobalIlluminationHeader =
			InvalidContentHash;
	}
	stats.m_gpuAllocatedBytes =
		sizeof(RHIGlobalIlluminationGpuHeader) +
		resources.m_globalIlluminationNodeCapacity *
			sizeof(RHIGlobalIlluminationGpuBvhNode) +
		resources.m_globalIlluminationBrickCapacity *
			sizeof(RHIGlobalIlluminationGpuBrick) +
		resources.m_globalIlluminationProbeCapacity *
			sizeof(RHIGlobalIlluminationGpuProbe) +
		resources.m_globalIlluminationCoefficientCapacity *
			sizeof(RHIGlobalIlluminationGpuCoefficients) +
		resources.m_globalIlluminationStateCapacity *
			sizeof(RHIGlobalIlluminationGpuState);

	bool bGlobalIlluminationPayloadReady = bHasGlobalIllumination;
	std::string globalIlluminationDiagnostic;
	if (bHasGlobalIllumination)
	{
		const uint64_t layoutSignature =
			ComputeGlobalIlluminationLayoutSignature(*globalIllumination);
		if (resources.m_uploadedGlobalIlluminationLayout !=
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
					commandList,
					resources.m_globalIlluminationStorage
						->GetOrAddShaderBinding("globalIlluminationBvh"),
					gpuLayout.m_nodes.GetData(),
					gpuLayout.m_nodes.Num() *
						sizeof(RHIGlobalIlluminationGpuBvhNode),
					0u);
				commands->UpdateShaderBinding(
					commandList,
					resources.m_globalIlluminationStorage
						->GetOrAddShaderBinding("globalIlluminationBricks"),
					gpuLayout.m_bricks.GetData(),
					gpuLayout.m_bricks.Num() *
						sizeof(RHIGlobalIlluminationGpuBrick),
					0u);
				commands->UpdateShaderBinding(
					commandList,
					resources.m_globalIlluminationStorage
						->GetOrAddShaderBinding("globalIlluminationProbes"),
					gpuLayout.m_probes.GetData(),
					gpuLayout.m_probes.Num() *
						sizeof(RHIGlobalIlluminationGpuProbe),
					0u);
				resources.m_uploadedGlobalIlluminationLayout =
					layoutSignature;
				stats.m_copiedCpuBytes += layoutBytes;
				stats.m_uploadedGpuBytes += layoutBytes;
			}
		}

		const uint64_t coefficientSignature =
			ComputeGlobalIlluminationCoefficientSignature(
				*globalIllumination);
		if (bGlobalIlluminationPayloadReady &&
			resources.m_uploadedGlobalIlluminationCoefficients !=
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
					commandList,
					resources.m_globalIlluminationStorage
						->GetOrAddShaderBinding(
							"globalIlluminationCoefficients"),
					coefficients.GetData(),
					coefficients.Num() *
						sizeof(RHIGlobalIlluminationGpuCoefficients),
					0u);
				resources.m_uploadedGlobalIlluminationCoefficients =
					coefficientSignature;
				stats.m_copiedCpuBytes += coefficientBytes;
				stats.m_uploadedGpuBytes += coefficientBytes;
			}
		}

		const uint64_t stateSignature =
			ComputeGlobalIlluminationStateSignature(*globalIllumination);
		if (bGlobalIlluminationPayloadReady &&
			resources.m_uploadedGlobalIlluminationStates !=
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
					commandList,
					resources.m_globalIlluminationStorage
						->GetOrAddShaderBinding("globalIlluminationStates"),
					states.GetData(),
					states.Num() *
						sizeof(RHIGlobalIlluminationGpuState),
					0u);
				resources.m_uploadedGlobalIlluminationStates =
					stateSignature;
				stats.m_copiedCpuBytes += stateBytes;
				stats.m_uploadedGpuBytes += stateBytes;
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
	const uint64_t headerHash = HashBytes(&header, sizeof(header));
	if (resources.m_uploadedGlobalIlluminationHeader !=
		headerHash)
	{
		commands->UpdateShaderBinding(
			commandList,
			resources.m_globalIlluminationStorage
				->GetOrAddShaderBinding("globalIlluminationHeader"),
			&header,
			sizeof(header),
			0u);
		resources.m_uploadedGlobalIlluminationHeader =
			headerHash;
		stats.m_copiedCpuBytes += sizeof(header);
		stats.m_uploadedGpuBytes += sizeof(header);
	}
	stats.m_bActive =
		bGlobalIlluminationPayloadReady &&
		stats.m_bEnabled;
	stats.m_loadedBricks =
		bGlobalIlluminationPayloadReady
			? stats.m_totalBricks
			: 0u;
	return stats;
}

void Sailor::RHI::UploadSharedBones(RHICommandListPtr commandList,
	const RHISceneViewSnapshot& snapshot, RHISharedViewSubmissionResources& resources)
{
	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	const size_t numBoneMatrices = snapshot.m_cpuBoneMatrices ? snapshot.m_cpuBoneMatrices->Num() : 0u;
	// Skinned mesh shaders still declare the bones set when no animation is
	// assigned. Keep a valid identity buffer for their invalid-offset path.
	const size_t requiredBoneCapacity = (std::max)(size_t{ 1u }, numBoneMatrices);

	const bool bRecreateBones = !resources.m_boneBindings ||
		resources.m_boneCapacity < requiredBoneCapacity;
	if (bRecreateBones)
	{
		resources.m_boneCapacity = GrowSubmissionCapacity(
			resources.m_boneCapacity,
			requiredBoneCapacity);
		resources.m_boneBindings = driver->CreateShaderBindings();
		driver->AddSsboToShaderBindings(
			resources.m_boneBindings,
			"bones",
			sizeof(glm::mat4),
			resources.m_boneCapacity,
			0u,
			true);
		resources.m_boneBindings->RecalculateCompatibility();
		resources.m_uploadedAnimationRevision = InvalidContentHash;
		resources.m_bonesSource.Clear();
	}

	if (resources.m_bonesSource != snapshot.m_cpuBoneMatrices ||
		resources.m_uploadedAnimationRevision != snapshot.m_animationRevision)
	{
		const glm::mat4 identity(1.0f);
		commands->UpdateShaderBinding(
			commandList,
			resources.m_boneBindings->GetOrAddShaderBinding("bones"),
			numBoneMatrices ? snapshot.m_cpuBoneMatrices->GetData() : &identity,
			requiredBoneCapacity * sizeof(glm::mat4),
			0u);
		resources.m_bonesSource = snapshot.m_cpuBoneMatrices;
		resources.m_uploadedAnimationRevision = snapshot.m_animationRevision;
	}
}
