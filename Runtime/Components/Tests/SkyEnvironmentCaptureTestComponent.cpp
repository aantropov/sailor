#include "Components/Tests/SkyEnvironmentCaptureTestComponent.h"
#include "Components/CameraComponent.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "FrameGraph/EnvironmentNode.h"
#include "FrameGraph/SkyNode.h"
#include "RHI/Texture.h"
#include <array>
#include <atomic>
#include <format>

using namespace Sailor;

struct SkyEnvironmentCaptureTestComponent::CaptureState
{
	enum class Stage { Initialize, Baseline, FirstCapture, LatestCapture };

	explicit CaptureState(RHI::RHIFrameGraphPtr frameGraph) : m_frameGraph(frameGraph)
	{
		constexpr float illuminance[] = { 30000.0f, 12000.0f, 75000.0f };
		for (size_t i = 0; i < m_parameters.size(); ++i)
		{
			m_parameters[i].m_sunIlluminance = glm::vec4(glm::vec3(illuminance[i]), 0.0f);
			m_parameters[i].m_cloudsDensity = 0.0f;
			m_parameters[i].m_cloudsCoverage = 0.0f;
			m_parameters[i].m_cloudScatteringScale = 0.0f;
			m_parameters[i].m_sunShaftsIntensity = 0.0f;
		}
	}

	CheckResult Check()
	{
		if (m_bCancelled)
		{
			return {};
		}

		auto sky = m_frameGraph->GetGraphNode("Sky").DynamicCast<Framegraph::SkyNode>();
		auto environment = m_frameGraph->GetGraphNode("Environment").DynamicCast<Framegraph::EnvironmentNode>();
		if (!sky || !environment)
		{
			return { false, "Sky and Environment nodes are required." };
		}

		if (m_stage == Stage::Initialize)
		{
			sky->SetSkyParams(m_parameters[0]);
			m_stage = Stage::Baseline;
			return {};
		}

		SkyParameters publishedSky, publishedEnvironment;
		if (!sky->GetEnvironmentSkyParams(publishedSky) ||
			!environment->GetEnvironmentSkyParams(publishedEnvironment))
		{
			return m_stage == Stage::Baseline ? CheckResult{} :
				CheckResult{ false, "Published environment disappeared during recapture." };
		}
		if (!(publishedSky == publishedEnvironment))
		{
			return { false, "Sky and Environment published different parameter snapshots." };
		}

		constexpr const char* samplers[] = {
			"g_skyCubemap", "g_envCubemap", "g_irradianceCubemap", "g_sheenEnvCubemap"
		};
		std::array<RHI::RHITexturePtr, 4> textures;
		for (size_t i = 0; i < textures.size(); ++i)
		{
			textures[i] = m_frameGraph->GetSampler(samplers[i]);
			if (!textures[i])
			{
				return { false, std::format("Published environment has no {}.", samplers[i]) };
			}
		}
		if (m_frameGraph->GetSampler("g_rawEnvCubemap") != textures[0])
		{
			return { false, "Environment consumed a different cubemap from the published Sky." };
		}

		if (m_stage == Stage::Baseline)
		{
			if (!(publishedSky == m_parameters[0]))
			{
				return {};
			}
			m_publishedTextures = textures;
			sky->SetSkyParams(m_parameters[1]);
			m_stage = Stage::FirstCapture;
			return {};
		}

		const bool bFirstCapture = m_stage == Stage::FirstCapture;
		const size_t previous = bFirstCapture ? 0 : 1;
		const size_t next = previous + 1;
		const SkyParameters currentSky = sky->GetSkyParams();
		if (publishedSky == m_parameters[previous])
		{
			if (textures != m_publishedTextures)
			{
				return { false, "Published cubemaps changed before their parameter snapshot completed." };
			}
			if (bFirstCapture && currentSky == m_parameters[1] && !m_bRequestedLatest)
			{
				// B is being rendered while A is still published. Queue C now.
				sky->SetSkyParams(m_parameters[2]);
				m_bRequestedLatest = true;
			}
			if (m_bRequestedLatest && !(currentSky == m_parameters[previous]))
			{
				++m_pendingObservations[previous];
			}
			return {};
		}

		if (!(publishedSky == m_parameters[next]) || !m_bRequestedLatest)
		{
			return { false, "Captures did not publish A, B, then the mid-capture request C." };
		}
		if (m_pendingObservations[previous] == 0)
		{
			return { false, "No observation retained the previous environment during capture." };
		}
		for (size_t i = 0; i < textures.size(); ++i)
		{
			if (textures[i] == m_publishedTextures[i])
			{
				return { false, std::format("Completed capture reused the previous {}.", samplers[i]) };
			}
		}

		if (bFirstCapture)
		{
			m_publishedTextures = textures;
			m_stage = Stage::LatestCapture;
			return {};
		}
		return { true, std::format(
			"A -> B -> C published matching Sky/Environment parameters and cubemaps; "
			"C requested during B capture; prior resources retained in {} and {} observations. "
			"Publication consistency checked; pixels were not read back.",
			m_pendingObservations[0], m_pendingObservations[1]) };
	}

	// Only Render inspects capture state; EndPlay may cancel pending work.
	std::atomic_bool m_bCancelled = false;
	RHI::RHIFrameGraphPtr m_frameGraph;
	std::array<SkyParameters, 3> m_parameters;
	std::array<RHI::RHITexturePtr, 4> m_publishedTextures;
	std::array<uint32_t, 2> m_pendingObservations{};
	Stage m_stage = Stage::Initialize;
	bool m_bRequestedLatest = false;
};

void SkyEnvironmentCaptureTestComponent::BeginPlay()
{
	TestCaseComponent::BeginPlay();
	auto cameraObject = GetWorld()->Instantiate("Sky capture test camera");
	auto camera = cameraObject->AddComponent<CameraComponent>();
	camera->SetZNear(0.1f);
	camera->SetZFar(1000.0f);
}

void SkyEnvironmentCaptureTestComponent::Tick(float)
{
	if (IsFinished())
	{
		return;
	}
	if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 60000)
	{
		if (m_capture)
		{
			m_capture->m_bCancelled = true;
		}
		MarkFailed("Sky environment capture did not complete within 60 seconds.");
		return;
	}

	if (m_check)
	{
		if (!m_check->IsFinished())
		{
			return;
		}
		const CheckResult result = m_check->GetResult();
		m_check.Clear();
		if (result.m_bPassed)
		{
			AddJournalEvent("SkyEnvironmentCaptureEvidence", result.m_message);
			MarkPassed();
			return;
		}
		if (!result.m_message.empty())
		{
			MarkFailed(result.m_message);
			return;
		}
	}

	if (!m_capture)
	{
		auto renderer = App::GetSubmodule<RHI::Renderer>();
		if (!renderer || !renderer->GetFrameGraph() || !renderer->GetFrameGraph()->GetRHI())
		{
			return;
		}
		m_capture = std::make_shared<CaptureState>(renderer->GetFrameGraph()->GetRHI());
	}
	m_check = Tasks::CreateTaskWithResult<CheckResult>("Validate sky environment capture",
		[capture = m_capture]() { return capture->Check(); }, EThreadType::Render);
	m_check->Run();
}

void SkyEnvironmentCaptureTestComponent::EndPlay()
{
	if (m_capture)
	{
		m_capture->m_bCancelled = true;
	}
	m_check.Clear();
	m_capture.reset();
	TestCaseComponent::EndPlay();
}
