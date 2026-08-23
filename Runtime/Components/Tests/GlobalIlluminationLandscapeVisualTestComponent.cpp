#include "Components/Tests/GlobalIlluminationLandscapeVisualTestComponent.h"

#include "AssetRegistry/GlobalIllumination/ProbeVolumeSampling.h"
#include "Components/Tests/GlobalIlluminationLandscapeTestScene.h"
#include "ECS/GlobalIlluminationECS.h"
#include "ECS/LandscapeECS.h"
#include "Engine/World.h"
#include "Sailor.h"

#include <cmath>

using namespace Sailor;

namespace
{
	constexpr uint32_t ValidationGraceFrames = 150u;
}

void GlobalIlluminationLandscapeVisualTestComponent::BeginPlay()
{
	VisualTestCaseComponent::BeginPlay();
	m_previousRenderMode = App::GetEditorRenderMode();
	m_bRenderModeChanged = App::SetEditorRenderMode(
		RHI::ESceneViewRenderMode::GlobalIlluminationOnly);
	AddJournalEvent(
		"GiOnlyEnabled",
		"Direct lighting and environment specular are suppressed for the "
		"evening landscape capture.");
}

void GlobalIlluminationLandscapeVisualTestComponent::Tick(float deltaTime)
{
	if (IsFinished())
	{
		return;
	}

	++m_totalFrames;
	if (App::GetEditorRenderMode() !=
		RHI::ESceneViewRenderMode::GlobalIlluminationOnly)
	{
		App::SetEditorRenderMode(
			RHI::ESceneViewRenderMode::GlobalIlluminationOnly);
	}

	if (!m_bSceneValidated)
	{
		std::string diagnostic;
		float receiverEnergy = 0.0f;
		if (ValidateLandscapeAndSecondaryLighting(
				diagnostic,
				receiverEnergy))
		{
			m_bSceneValidated = true;
			AddJournalEvent(
				"SecondaryBounceEvidence",
				"The receiver's direct sun ray is blocked by the saved ridge; "
				"GI-only irradiance energy is " +
					std::to_string(receiverEnergy) +
					" from the baked evening state.");
		}
		else if (m_totalFrames > ValidationGraceFrames)
		{
			MarkFailed(diagnostic);
			return;
		}
	}

	if (m_totalFrames + 20u >= GetCaptureAfterFrames() &&
		!m_bSceneValidated)
	{
		MarkFailed(
			"The evening landscape, probe state, or secondary-lighting evidence "
			"was not ready before capture.");
		return;
	}

	VisualTestCaseComponent::Tick(deltaTime);
}

void GlobalIlluminationLandscapeVisualTestComponent::EndPlay()
{
	if (m_bRenderModeChanged)
	{
		App::SetEditorRenderMode(m_previousRenderMode);
	}
	VisualTestCaseComponent::EndPlay();
}

bool GlobalIlluminationLandscapeVisualTestComponent::
	ValidateLandscapeAndSecondaryLighting(
		std::string& outDiagnostic,
		float& outReceiverEnergy) const
{
	outReceiverEnergy = 0.0f;
	if (!GetWorld())
	{
		outDiagnostic = "The visual test has no world.";
		return false;
	}

	auto* landscape = GetWorld()->GetECS<LandscapeECS>();
	TVector<LandscapeBakeGeometrySnapshot> landscapeSnapshots;
	if (!landscape || !landscape->CollectBakeGeometrySnapshots(
			landscapeSnapshots,
			outDiagnostic) ||
		landscapeSnapshots.Num() <
			GlobalIlluminationLandscapeTestScene::LandscapeChunksX *
			GlobalIlluminationLandscapeTestScene::LandscapeChunksZ)
	{
		if (outDiagnostic.empty())
		{
			outDiagnostic =
				"The saved landscape did not publish all CPU/GPU chunks.";
		}
		return false;
	}

	auto* globalIllumination = GetWorld()->GetECS<GlobalIlluminationECS>();
	const GlobalIlluminationSnapshotPtr snapshot = globalIllumination ?
		globalIllumination->GetActiveSnapshot() : GlobalIlluminationSnapshotPtr{};
	if (!snapshot || snapshot->m_states.Num() != 1u)
	{
		outDiagnostic =
			"The Evening Landscape Bounce state is not the sole active GI state.";
		return false;
	}
	const RHI::RHIGlobalIlluminationState& state = snapshot->m_states[0];
	if (state.m_name != "Evening Landscape Bounce" ||
		state.m_mode != EGlobalIlluminationProbeMode::Blend ||
		std::abs(state.m_effectiveWeight - 1.0f) > 0.001f ||
		!state.m_data)
	{
		outDiagnostic =
			"The evening landscape GI state has an invalid identity, mode, or weight.";
		return false;
	}

	glm::vec3 irradiance{};
	if (!SampleProbeVolumeIrradiance(
			*state.m_data,
			GlobalIlluminationLandscapeTestScene::GetReceiverEvidencePoint(),
			glm::vec3(0.0f, 1.0f, 0.0f),
			irradiance))
	{
		outDiagnostic =
			"The saved shadow receiver is outside the active probe topology.";
		return false;
	}
	outReceiverEnergy = glm::dot(
		irradiance,
		glm::vec3(0.2126f, 0.7152f, 0.0722f));
	if (!std::isfinite(outReceiverEnergy) || outReceiverEnergy <= 0.015f)
	{
		outDiagnostic =
			"The directly occluded receiver has no measurable secondary irradiance.";
		return false;
	}

	if (App::GetEditorRenderMode() !=
		RHI::ESceneViewRenderMode::GlobalIlluminationOnly)
	{
		outDiagnostic = "The visual capture is not in GI-only mode.";
		return false;
	}
	outDiagnostic.clear();
	return true;
}
