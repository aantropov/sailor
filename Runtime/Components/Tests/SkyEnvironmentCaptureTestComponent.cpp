#include "Components/Tests/SkyEnvironmentCaptureTestComponent.h"
#include "Components/CameraComponent.h"
#include "Components/SkyComponent.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "FrameGraph/EnvironmentNode.h"
#include "FrameGraph/SkyNode.h"
#include "RHI/Texture.h"
#include <array>
#include <atomic>
#include <format>
#include <future>

using namespace Sailor;

namespace
{
	Framegraph::LocalReflectionParameters LocalParameters(uint32_t samples)
	{
		const float x = static_cast<float>(samples);
		return { { x, 0.0f, 0.0f, 1.0f }, { x - 2.0f, -2.0f, -2.0f, 1.0f }, { x + 2.0f, 2.0f, 2.0f, 0.0f } };
	}

	Framegraph::LocalReflectionImage LocalImage(uint32_t samples)
	{
		Framegraph::LocalReflectionImage image;
		image.m_parameters = LocalParameters(samples);
		image.m_extent = { 8u, 4u };
		image.m_samplesPerPixel = samples;
		image.m_pixels.Resize(32);
		for (auto& pixel : image.m_pixels)
		{
			pixel = glm::vec4(0.25f * static_cast<float>(samples), 0.5f, 1.0f, 1.0f);
		}
		return image;
	}
}

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

	bool MatchesLocalPublication(uint32_t samples, const std::array<RHI::RHITexturePtr, 2>& textures)
	{
		auto environment = m_frameGraph->GetGraphNode("Environment").DynamicCast<Framegraph::EnvironmentNode>();
		const auto actual = environment->GetLocalReflectionParameters();
		const auto expected = samples ? LocalParameters(samples) : Framegraph::LocalReflectionParameters{};
		return environment->IsLocalReflectionReady() == (samples != 0) &&
			environment->GetLocalReflectionSamples() == samples &&
			actual.m_positionBlend == expected.m_positionBlend && actual.m_minEnabled == expected.m_minEnabled &&
			actual.m_max == expected.m_max &&
			m_frameGraph->GetSampler("g_localEnvCubemap") == textures[0] &&
			m_frameGraph->GetSampler("g_localSheenEnvCubemap") == textures[1];
	}

	bool MatchesPendingLocalPublication(uint32_t previousSamples, uint32_t pendingSamples)
	{
		if (MatchesLocalPublication(previousSamples, m_localTextures))
		{
			return true;
		}
		const std::array<RHI::RHITexturePtr, 2> textures = {
			m_frameGraph->GetSampler("g_localEnvCubemap"), m_frameGraph->GetSampler("g_localSheenEnvCubemap")
		};
		return textures[0] && textures[1] && textures[0] != m_localTextures[0] && textures[1] != m_localTextures[1] &&
			MatchesLocalPublication(pendingSamples, textures);
	}

	CheckResult CheckLocalReflection(uint32_t samples)
	{
		if (m_bCancelled)
		{
			return {};
		}
		auto environment = m_frameGraph->GetGraphNode("Environment").DynamicCast<Framegraph::EnvironmentNode>();
		if (samples && environment->GetLocalReflectionSamples() != samples)
		{
			const bool bCoherent = samples == 1u ? MatchesLocalPublication(0u, m_localTextures) :
				MatchesPendingLocalPublication(1u, 2u);
			return bCoherent ? CheckResult{} :
				CheckResult{ false, "Pending local reflection has neither the prior nor a coherent intermediate publication." };
		}
		const std::array<RHI::RHITexturePtr, 2> textures = {
			m_frameGraph->GetSampler("g_localEnvCubemap"), m_frameGraph->GetSampler("g_localSheenEnvCubemap")
		};
		if (!MatchesLocalPublication(samples, textures))
		{
			return { false, "Local reflection samples, readiness and box parameters disagree." };
		}
		if (!samples)
		{
			if (textures[0] || textures[1])
			{
				return m_bLocalResetObserved ? CheckResult{ false, "Local reflection cubemaps reappeared after reset." } :
					CheckResult{};
			}
			if (!m_bLocalResetObserved)
			{
				m_bLocalResetObserved = true;
				return {};
			}
			return { true, "Owned local images reached samples 1 and 4 with matching box parameters and both cubemaps; "
				"queued 2 -> 4 and 8 -> reset observations matched either the prior publication or a coherent intermediate update. "
				"Reset cleared parameters, readiness, samples and both samplers across two observations. "
				"Caller image data was mutated and destroyed before queued updates ran; GPU upload start and pixels were not inspected." };
		}
		if (!textures[0] || !textures[1] || textures[0] == m_localTextures[0] || textures[1] == m_localTextures[1])
		{
			return { false, "Completed local reflection did not publish two new cubemaps." };
		}
		m_localTextures = textures;
		return { true, {} };
	}

	// Only Render inspects capture state; EndPlay may cancel pending work.
	std::atomic_bool m_bCancelled = false;
	RHI::RHIFrameGraphPtr m_frameGraph;
	std::array<SkyParameters, 3> m_parameters;
	std::array<RHI::RHITexturePtr, 4> m_publishedTextures;
	std::array<uint32_t, 2> m_pendingObservations{};
	std::array<RHI::RHITexturePtr, 2> m_localTextures{};
	Stage m_stage = Stage::Initialize;
	bool m_bRequestedLatest = false;
	bool m_bLocalResetObserved = false;
};

SkyEnvironmentCaptureTestComponent::~SkyEnvironmentCaptureTestComponent() = default;

bool SkyEnvironmentCaptureTestComponent::QueueLocalReflection(
	uint32_t pendingSamples, uint32_t finalSamples, uint32_t publishedSamples)
{
	auto environment = m_capture->m_frameGraph->GetGraphNode("Environment").DynamicCast<Framegraph::EnvironmentNode>();
	std::promise<void> releaseRender;
	Tasks::CreateTask("Hold Render for local reflection submissions",
		[ready = releaseRender.get_future().share()]() { ready.wait(); }, EThreadType::Render)->Run();
	const auto submitImage = [&](uint32_t samples)
	{
		auto image = LocalImage(samples);
		const bool bAccepted = environment->SetLocalReflection(image);
		image.m_parameters = {};
		image.m_samplesPerPixel = 99u;
		image.m_pixels.Clear();
		return bAccepted;
	};
	if (!submitImage(pendingSamples))
	{
		return false;
	}
	if (pendingSamples != finalSamples)
	{
		m_localPublicationCheck = Tasks::CreateTaskWithResult<bool>("Observe coherent local reflection update",
			[capture = m_capture, publishedSamples, pendingSamples]()
			{
				return capture->MatchesPendingLocalPublication(publishedSamples, pendingSamples);
			}, EThreadType::Render);
		m_localPublicationCheck->Run();
		if (finalSamples && !submitImage(finalSamples))
		{
			return false;
		}
		if (!finalSamples)
		{
			environment->ResetLocalReflection();
		}
	}
	m_expectedLocalSamples = finalSamples;
	releaseRender.set_value();
	return true;
}

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
			if (m_bHandoffComplete)
			{
				bool bQueued = true;
				if (!m_bSkyCaptureComplete)
				{
					AddJournalEvent("SkyEnvironmentCaptureEvidence", result.m_message);
					m_bSkyCaptureComplete = true;
					bQueued = QueueLocalReflection(1u, 1u, 0u);
				}
				else if (m_expectedLocalSamples == 1u)
				{
					bQueued = QueueLocalReflection(2u, 4u, 1u);
				}
				else if (m_expectedLocalSamples == 4u)
				{
					bQueued = QueueLocalReflection(8u, 0u, 4u);
				}
				else
				{
					AddJournalEvent("LocalReflectionEvidence", result.m_message);
					MarkPassed();
					return;
				}
				if (!bQueued)
				{
					MarkFailed("Valid local reflection image was rejected.");
					return;
				}
			}
			else
			{
				AddJournalEvent("SkyComponentHandoffEvidence", result.m_message);
				m_bHandoffComplete = true;
			}
		}
		else if (!result.m_message.empty())
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
		if (!m_bHandoffComplete)
		{
			auto sky = renderer->GetFrameGraph()->GetRHI()->GetGraphNode("Sky").DynamicCast<Framegraph::SkyNode>();
			if (!sky)
			{
				MarkFailed("Sky component handoff requires a Sky node.");
				return;
			}

			// Keep Render behind the producer's lifetime without waiting on Main.
			std::promise<void> releaseRender;
			Tasks::CreateTask("Hold Render for sky producer destruction",
				[ready = releaseRender.get_future().share()]() { ready.wait(); }, EThreadType::Render)->Run();
			{
				SkyComponent producer;
				producer.SetCloudsDensity(0.25f);
				producer.Tick(0.0f);
				const SkyParameters expected = producer.GetSkyParameters();
				auto updateCheck = Tasks::CreateTaskWithResult<bool>("Observe copied sky component parameters",
					[sky, expected]() { return sky->GetSkyParams() == expected; }, EThreadType::Render);
				updateCheck->Run();
				producer.SetCloudsDensity(0.75f);
				producer.EndPlay();
				m_check = Tasks::CreateTaskWithResult<CheckResult>("Observe ordered sky component reset",
					[sky, updateCheck]() -> CheckResult
					{
						if (!updateCheck->GetResult())
						{
							return { false, "Queued sky update did not preserve parameters after producer mutation and destruction." };
						}
						if (!(sky->GetSkyParams() == SkyParameters{}))
						{
							return { false, "Sky component EndPlay did not reset parameters after its queued update." };
						}
						return { true, "SkyComponent Tick and EndPlay reached Render in update/reset order; "
							"copied parameters survived producer mutation and destruction before Render was released." };
					}, EThreadType::Render);
				m_check->Run();
			}
			releaseRender.set_value();
			return;
		}
		m_capture = TSharedPtr<CaptureState>::Make(renderer->GetFrameGraph()->GetRHI());
	}
	m_check = Tasks::CreateTaskWithResult<CheckResult>("Validate sky environment capture",
		[capture = m_capture, bLocal = m_bSkyCaptureComplete, samples = m_expectedLocalSamples,
		publication = m_localPublicationCheck]() -> CheckResult
		{
			if (publication && (!publication->IsFinished() || !publication->GetResult()))
			{
				return { false, "Queued local reflection update has neither the prior nor a coherent intermediate publication." };
			}
			return bLocal ? capture->CheckLocalReflection(samples) : capture->Check();
		}, EThreadType::Render);
	m_check->Run();
}

void SkyEnvironmentCaptureTestComponent::EndPlay()
{
	if (m_capture)
	{
		m_capture->m_bCancelled = true;
	}
	m_check.Clear();
	m_localPublicationCheck.Clear();
	m_capture.Clear();
	TestCaseComponent::EndPlay();
}
