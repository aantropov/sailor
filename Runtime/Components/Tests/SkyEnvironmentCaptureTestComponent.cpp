#include "Components/Tests/SkyEnvironmentCaptureTestComponent.h"
#include "Platform/Time.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/LightingECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/EnvironmentNode.h"
#include "FrameGraph/LinearizeDepthNode.h"
#include "FrameGraph/PostProcessNode.h"
#include "FrameGraph/SkyNode.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/RenderTarget.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include <glm/gtc/packing.hpp>
#include <array>
#include <atomic>
#include <format>

using namespace Sailor;

namespace
{
	const std::array<glm::vec3, 7> MarkerPositions = {
		glm::vec3(4, 0, 0), glm::vec3(-4, 0, 0), glm::vec3(0, 4, 0),
		glm::vec3(0, -4, 0), glm::vec3(0, 0, 4), glm::vec3(0, 0, -4),
		glm::vec3(4, 1, -1.4f)
	};
	const std::array<glm::vec3, 7> MarkerColors = {
		glm::vec3(4, 0.25f, 0.5f), glm::vec3(0.5f, 5, 0.25f), glm::vec3(0.25f, 0.5f, 6),
		glm::vec3(7, 1, 0.5f), glm::vec3(1, 8, 2), glm::vec3(2, 1, 9),
		glm::vec3(12, 10, 2)
	};
	const glm::vec3 ShadowReceiverPoint(2.0f, -2.0f, 5.95f);
	const glm::vec3 UnshadowedReceiverPoint(3.1f, -3.1f, 5.95f);

	Framegraph::LocalReflectionCaptureRequest LocalRequest(const glm::vec3& position)
	{
		Framegraph::LocalReflectionCaptureRequest request;
		request.m_parameters = { glm::vec4(position, 1), glm::vec4(-10, -10, -10, 1), glm::vec4(10, 10, 10, 0) };
		request.m_farPlane = 30.0f;
		return request;
	}

	bool SameParameters(const Framegraph::LocalReflectionParameters& lhs,
		const Framegraph::LocalReflectionParameters& rhs)
	{
		return lhs.m_positionBlend == rhs.m_positionBlend && lhs.m_minEnabled == rhs.m_minEnabled &&
			lhs.m_max == rhs.m_max;
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

		auto sky = m_frameGraph->GetGraphNode("Sky"_h).DynamicCast<Framegraph::SkyNode>();
		auto environment = m_frameGraph->GetGraphNode("Environment"_h).DynamicCast<Framegraph::EnvironmentNode>();
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

		const StringHash samplers[] = {
			"g_skyCubemap"_h, "g_envCubemap"_h, "g_irradianceCubemap"_h, "g_sheenEnvCubemap"_h
		};
		std::array<RHI::RHITexturePtr, 4> textures;
		for (size_t i = 0; i < textures.size(); ++i)
		{
			textures[i] = m_frameGraph->GetSampler(samplers[i]);
			if (!textures[i])
			{
				return { false, std::format("Published environment has no {}.", samplers[i].ToString()) };
			}
		}
		if (m_frameGraph->GetSampler("g_rawEnvCubemap"_h) != textures[0])
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
				return { false, std::format("Completed capture reused the previous {}.", samplers[i].ToString()) };
			}
		}

		if (bFirstCapture)
		{
			m_publishedTextures = textures;
			m_stage = Stage::LatestCapture;
			return {};
		}
		m_publishedTextures = textures;
		return { true, std::format(
			"A -> B -> C published matching Sky/Environment parameters and cubemaps; "
			"C requested during B capture; prior resources retained in {} and {} observations. "
			"Publication consistency checked; pixels were not read back.",
			m_pendingObservations[0], m_pendingObservations[1]) };
	}

	~CaptureState()
	{
		for (auto& material : m_fixtureMaterials)
		{
			if (material)
			{
				material.DestroyObject(m_materialAllocator);
			}
		}
	}

	bool MatchesLocalPublication(uint64_t generation,
		const Framegraph::LocalReflectionParameters& parameters,
		const std::array<RHI::RHITexturePtr, 2>& textures)
	{
		auto environment = m_frameGraph->GetGraphNode("Environment"_h).DynamicCast<Framegraph::EnvironmentNode>();
		return environment && environment->IsLocalReflectionReady() == (generation != 0u) &&
			environment->GetLocalReflectionGeneration() == generation &&
			SameParameters(environment->GetLocalReflectionParameters(), parameters) &&
			m_frameGraph->GetSampler("g_localEnvCubemap"_h) == textures[0] &&
			m_frameGraph->GetSampler("g_localSheenEnvCubemap"_h) == textures[1];
	}

	std::string BeginReadback(const Framegraph::LocalReflectionCubemap& capture,
		RHI::RHICubemapPtr specular)
	{
		using namespace RHI;
		if (!specular)
		{
			return "The published specular reflection is not a cubemap.";
		}
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(command, true);
		const std::array<RHICubemapPtr, 3> cubes = { capture.m_cubemap, specular, m_retainedRaw };
		m_readbackCompletion = RHIFencePtr::Make();
		for (size_t cubeIndex = 0u; cubeIndex < cubes.size(); ++cubeIndex)
		{
			const auto& cube = cubes[cubeIndex];
			if (!cube)
			{
				continue;
			}
			const auto format = cube->GetFormat();
			if (format != EFormat::R16G16B16A16_SFLOAT && format != EFormat::R32G32B32A32_SFLOAT)
			{
				return "Reflection readback requires a linear floating point cubemap.";
			}
			const uint32_t size = static_cast<uint32_t>(cube->GetExtent().x);
			const size_t pixelBytes = format == EFormat::R16G16B16A16_SFLOAT ? 8u : 16u;
			m_readbackSizes[cubeIndex] = size;
			m_readbackFormats[cubeIndex] = format;
			m_readbackCompletion->AddDependency(cube);
			commands->ImageMemoryBarrier(command, cube, EImageLayout::TransferSrcOptimal);
			for (uint32_t face = 0u; face < 6u; ++face)
			{
				auto& buffer = m_readbacks[cubeIndex * 6u + face];
				buffer = driver->CreateBuffer(size_t(size) * size * pixelBytes,
					EBufferUsageBit::BufferTransferDst_Bit,
					EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
				if (!buffer)
				{
					return "Reflection readback allocation failed.";
				}
				m_readbackCompletion->AddDependency(buffer);
				commands->CopyImageToBuffer(command, cube->GetFace(face), buffer);
			}
			commands->ImageMemoryBarrier(command, cube, EImageLayout::ShaderReadOnlyOptimal);
		}
		commands->MemoryBarrier(command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		commands->EndCommandList(command);
		if (!driver->SubmitCommandList(command, m_readbackCompletion))
		{
			return "Reflection readback submission failed.";
		}
		return {};
	}

	glm::vec4 ReadPixel(size_t cubeIndex, uint32_t face, uint32_t x, uint32_t y) const
	{
		const auto* bytes = m_readbacks[cubeIndex * 6u + face]->GetPointer();
		const size_t pixel = size_t(y) * m_readbackSizes[cubeIndex] + x;
		if (m_readbackFormats[cubeIndex] == RHI::EFormat::R32G32B32A32_SFLOAT)
		{
			return static_cast<const glm::vec4*>(bytes)[pixel];
		}
		const auto* half = static_cast<const uint16_t*>(bytes) + pixel * 4u;
		return glm::vec4(glm::unpackHalf1x16(half[0]), glm::unpackHalf1x16(half[1]),
			glm::unpackHalf1x16(half[2]), glm::unpackHalf1x16(half[3]));
	}

	uint64_t HashFace(size_t cubeIndex, uint32_t face) const
	{
		const auto* bytes = static_cast<const uint8_t*>(m_readbacks[cubeIndex * 6u + face]->GetPointer());
		const size_t size = m_readbackSizes[cubeIndex];
		const size_t pixelBytes = m_readbackFormats[cubeIndex] == RHI::EFormat::R16G16B16A16_SFLOAT ? 8u : 16u;
		uint64_t hash = 14695981039346656037ull;
		for (size_t byte = 0u; byte < size * size * pixelBytes; ++byte)
		{
			hash = (hash ^ bytes[byte]) * 1099511628211ull;
		}
		return hash;
	}

	std::string ValidatePixels(const Framegraph::LocalReflectionCubemap& capture, bool shadowsDisabled)
	{
		const auto nearColor = [](const glm::vec3& actual, const glm::vec3& expected)
		{
			return glm::all(glm::lessThan(glm::abs(actual - expected), glm::vec3(0.05f)));
		};
		for (size_t cubeIndex = 0u; cubeIndex < 2u; ++cubeIndex)
		{
			const uint32_t size = m_readbackSizes[cubeIndex];
			for (uint32_t face = 0u; face < 6u; ++face)
			{
				for (uint32_t y = 0u; y < size; ++y)
				{
					for (uint32_t x = 0u; x < size; ++x)
					{
						const auto pixel = ReadPixel(cubeIndex, face, x, y);
						for (int channel = 0; channel < 4; ++channel)
						{
							if (!std::isfinite(pixel[channel]) || pixel[channel] < 0.0f || pixel[channel] > 65504.0f)
							{
								return std::format("Invalid HDR pixel in {} face {} at {},{}.", cubeIndex, face, x, y);
							}
						}
					}
				}
				const auto center = ReadPixel(cubeIndex, face, size / 2u, size / 2u);
				if (!nearColor(glm::vec3(center), MarkerColors[face]))
				{
					return std::format("{} face {} expected scene emission [{},{},{}], got [{},{},{}].",
						cubeIndex == 0u ? "Raw capture" : "Specular mip zero", face,
						MarkerColors[face].x, MarkerColors[face].y, MarkerColors[face].z, center.x, center.y, center.z);
				}
			}
			const glm::vec3 position(capture.m_parameters.m_positionBlend);
			const auto markerDirection = MarkerPositions[6] - position;
			const float u = 0.5f * (1.0f - markerDirection.z / markerDirection.x);
			const float v = 0.5f * (1.0f - markerDirection.y / markerDirection.x);
			const auto marker = ReadPixel(cubeIndex, 0u, static_cast<uint32_t>(u * size), static_cast<uint32_t>(v * size));
			if (!nearColor(glm::vec3(marker), MarkerColors[6]))
			{
				return std::format("Asymmetric +X marker has wrong cubemap orientation or capture origin: {} at {},{} is [{},{},{}].",
					cubeIndex, u, v, marker.x, marker.y, marker.z);
			}
		}
		const uint32_t size = m_readbackSizes[0];
		const auto background = ReadPixel(0u, 2u, size / 16u, size / 16u);
		if (glm::dot(glm::vec3(background), glm::vec3(1)) <= 0.0001f)
		{
			return "Open-sky pixels were replaced with a black capture background.";
		}
		uint32_t changedFaces = 0u;
		for (uint32_t face = 0u; face < 6u; ++face)
		{
			const uint64_t hash = HashFace(0u, face);
			if (m_retainedRaw)
			{
				if (HashFace(2u, face) != m_retainedHashes[face])
				{
					return "Replacing the reflection modified a retained previous raw cubemap.";
				}
				changedFaces += hash != m_retainedHashes[face] ? 1u : 0u;
			}
			else
			{
				m_retainedHashes[face] = hash;
			}
		}
		if (m_retainedRaw && changedFaces < 2u)
		{
			return "Moving the reflection capture did not change scene pixels in multiple directions.";
		}
		if (!m_retainedRaw)
		{
			m_retainedRaw = capture.m_cubemap;
		}
		const auto receiverLuminance = [&](const glm::vec3& point)
		{
			const glm::vec3 direction = point - glm::vec3(capture.m_parameters.m_positionBlend);
			const uint32_t x = static_cast<uint32_t>((0.5f + 0.5f * direction.x / direction.z) * size);
			const uint32_t y = static_cast<uint32_t>((0.5f - 0.5f * direction.y / direction.z) * size);
			float result = 0.0f;
			for (uint32_t dy = y - 1u; dy <= y + 1u; ++dy)
			{
				for (uint32_t dx = x - 1u; dx <= x + 1u; ++dx)
				{
					result += glm::dot(glm::vec3(ReadPixel(0u, 4u, dx, dy)), glm::vec3(0.2126f, 0.7152f, 0.0722f));
				}
			}
			return result / 9.0f;
		};
		const float blocked = receiverLuminance(ShadowReceiverPoint);
		const float lit = receiverLuminance(UnshadowedReceiverPoint);
		if (shadowsDisabled)
		{
			if (blocked < m_shadowedReceiver * 1.5f + 0.1f || lit < 1.0f ||
				std::abs(lit - m_unshadowedReceiver) > (std::max)(1.0f, lit * 0.25f))
			{
				return std::format("Point-light shadow did not affect the captured receiver correctly: blocked {} -> {}, "
					"unblocked {} -> {} when shadows were disabled.", m_shadowedReceiver, blocked, m_unshadowedReceiver, lit);
			}
			m_shadowEvidence = std::format("Point-light PCF blocker changed receiver luminance {} -> {} with shadows disabled; "
				"unblocked control {} -> {}.", m_shadowedReceiver, blocked, m_unshadowedReceiver, lit);
		}
		else
		{
			m_shadowedReceiver = blocked;
			m_unshadowedReceiver = lit;
		}
		return {};
	}

	CheckResult CheckLocalReflection(const Framegraph::LocalReflectionCubemap& capture,
		uint64_t expectedGeneration, const Framegraph::LocalReflectionParameters& expectedParameters,
		bool shadowsDisabled)
	{
		if (m_bCancelled)
		{
			return {};
		}
		auto environment = m_frameGraph->GetGraphNode("Environment"_h).DynamicCast<Framegraph::EnvironmentNode>();
		if (!environment)
		{
			return { false, "The Environment node disappeared during a local capture." };
		}
		if (environment->GetLocalReflectionGeneration() != expectedGeneration)
		{
			return MatchesLocalPublication(m_localGeneration, m_localParameters, m_localTextures) ? CheckResult{} :
				CheckResult{ false, "A pending capture lost the previous complete reflection or published a superseded request." };
		}
		const std::array<RHI::RHITexturePtr, 2> textures = {
			m_frameGraph->GetSampler("g_localEnvCubemap"_h), m_frameGraph->GetSampler("g_localSheenEnvCubemap"_h)
		};
		if (!textures[0] || !textures[1] || !MatchesLocalPublication(expectedGeneration, expectedParameters, textures))
		{
			return { false, "The completed local reflection has inconsistent generation, parameters or filtered maps." };
		}
		if (capture.m_generation != expectedGeneration)
		{
			return {};
		}
		if (!capture.IsValid() || !SameParameters(capture.m_parameters, expectedParameters))
		{
			return { false, "LightingECS returned an invalid raw capture or mismatched capture parameters." };
		}
		if (!m_readbackCompletion)
		{
			if (textures[0] == m_localTextures[0] || textures[1] == m_localTextures[1])
			{
				return { false, "Replacing a reflection reused a previous writable filtered map." };
			}
			const auto error = BeginReadback(capture, textures[0].DynamicCast<RHI::RHICubemap>());
			return { false, error };
		}
		const auto readbackStatus = m_readbackCompletion->GetStatus();
		if (readbackStatus == RHI::EFenceStatus::Failed)
		{
			return { false, "The reflection pixel readback failed." };
		}
		if (readbackStatus != RHI::EFenceStatus::Finished)
		{
			return {};
		}
		if (const auto error = ValidatePixels(capture, shadowsDisabled); !error.empty())
		{
			return { false, error };
		}
		m_localGeneration = expectedGeneration;
		m_localParameters = expectedParameters;
		m_localTextures = textures;
		m_readbackCompletion.Clear();
		for (auto& buffer : m_readbacks)
		{
			buffer.Clear();
		}
		return { true, std::format("Generation {}: raw and specular mip-zero pixels show all six distinct emissive boxes, "
			"the asymmetric +X marker and an open-sky background; capture position [{},{},{}].",
			expectedGeneration, expectedParameters.m_positionBlend.x, expectedParameters.m_positionBlend.y,
			expectedParameters.m_positionBlend.z) + (shadowsDisabled ? " " + m_shadowEvidence : "") };
	}

	CheckResult CheckCaptureRecovery()
	{
		using namespace RHI;
		if (m_bCancelled)
		{
			return {};
		}
		if (!MatchesLocalPublication(m_localGeneration, m_localParameters, m_localTextures))
		{
			return { false, "An isolated failed capture changed the accepted reflection publication." };
		}
		if (m_recoveryFence)
		{
			if (m_recoveryFence->GetStatus() == EFenceStatus::Failed)
			{
				return { false, "The skipped-pass fixture could not retire its GPU submissions." };
			}
			if (!m_recoveryFence->IsFinished())
			{
				return {};
			}
			m_recoveryFence.Clear();
			m_recoveryJob->m_completion.Clear();
			if (!m_recoveryError.empty())
			{
				return { false, m_recoveryError };
			}
			if (m_recoveryPhase == 1u)
			{
				if (!m_forcedPassSkipped || m_recoveryJob->m_bSubmitted || !m_recoveryJob->m_bRetry)
				{
					return { false, "A missing capture depth-linearization target was not rejected as an incomplete render pass." };
				}
				++m_recoveryPhase;
			}
			else if (m_recoveryJob->m_bSubmitted)
			{
				if (m_recoveryPhase == 2u)
				{
					const auto* pixels = static_cast<const uint16_t*>(m_recoveryReadback->GetPointer());
					const size_t center = (8u * 16u + 8u) * 4u;
					float radiance = 0.0f;
					for (size_t channel = 0u; channel < 3u; ++channel)
					{
						const float value = glm::unpackHalf1x16(pixels[center + channel]);
						if (!std::isfinite(value) || value < 0.0f)
						{
							return { false, "Recovered capture contained an invalid HDR pixel." };
						}
						radiance += value;
					}
					if (radiance <= 0.0001f)
					{
						return { false, "Recovered capture did not render its sky into the completed cubemap." };
					}
					m_recoveryOuter.Clear();
					m_recoveryJob.Clear();
					m_recoveryReadback.Clear();
					return { true, "An actual missing LinearizeDepth target recorded zero draws and prevented capture publication; "
						"after partial submissions retired, restoring the target completed six faces/mips with nonzero HDR pixels. "
						"The accepted ECS reflection stayed unchanged throughout failure and recovery." };
				}
				++m_recoveryPhase;
			}
		}
		if (++m_recoveryAttempts > 16u)
		{
			return { false, "The isolated capture could not finish shader/pipeline warmup in sixteen attempts." };
		}
		if (!m_recoveryJob)
		{
			m_recoveryJob = TRefPtr<LocalReflectionCaptureJob>::Make();
			m_recoveryJob->m_request = LocalRequest(glm::vec3(0));
			m_recoveryJob->m_request.m_faceSize = 16u;
			m_recoveryJob->m_generation = 1u;
			m_recoveryJob->m_submission = RHISubmissionCompletionTokenPtr::Make();
			const glm::vec3 directions[] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
				{ 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
			const glm::vec3 up[] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 },
				{ 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
			for (uint32_t face = 0u; face < 6u; ++face)
			{
				auto view = RHISceneViewPtr::Make();
				view->m_snapshots.Resize(1u);
				auto& snapshot = view->m_snapshots[0];
				snapshot.m_cameraIndex = face;
				snapshot.m_camera = TUniquePtr<CameraData>::Make();
				snapshot.m_camera->SetFov(90.0f);
				snapshot.m_camera->SetAspect(1.0f);
				snapshot.m_camera->SetZNear(0.1f);
				snapshot.m_camera->SetZFar(30.0f);
				snapshot.m_camera->SetViewMatrix(glm::lookAtRH(glm::vec3(0), directions[face], up[face]));
				snapshot.m_camera->SetProjectionMatrix(Math::PerspectiveRH(glm::radians(90.0f), 1.0f, 0.1f, 30.0f));
				snapshot.m_cameraTransform = Math::Transform::FromMatrix(snapshot.m_camera->GetInvViewMatrix());
				snapshot.m_bGlobalIlluminationEnabled = false;
				snapshot.m_submissionCompletionToken = m_recoveryJob->m_submission;
				m_recoveryJob->m_faces[face] = view;
			}
			m_recoveryOuter = RHIRenderSubmissionContextPtr::Make();
			m_recoveryOuter->BeginSubmission(m_recoveryAttempts, 0u);
			for (const auto& task : m_recoveryJob->Prepare(m_frameGraph, m_recoveryOuter))
			{
				task->Execute();
			}
		}
		else
		{
			m_recoveryOuter->BeginSubmission(m_recoveryAttempts, 0u);
			m_recoveryJob->m_context->BeginSubmission(m_recoveryAttempts, 0u, 0u, m_recoveryJob->m_generation);
			m_recoveryJob->m_completion = m_recoveryOuter->GetOrCreateFrameCompletion();
			m_recoveryJob->m_submission->Reset();
			for (auto& face : m_recoveryJob->m_faces)
			{
				for (const auto& task : m_recoveryJob->m_graph->Prepare(face))
				{
					task->Execute();
				}
			}
		}
		if (!m_recoveryJob->m_bPrepared || !m_recoveryJob->m_graph)
		{
			return { false, "The isolated capture could not initialize its private frame graph." };
		}
		TRefPtr<Framegraph::LinearizeDepthNode> linearize;
		TRefPtr<Framegraph::PostProcessNode> sky;
		for (auto node : m_recoveryJob->m_graph->GetGraph())
		{
			if (auto candidate = node.DynamicCast<Framegraph::LinearizeDepthNode>())
			{
				linearize = candidate;
			}
			if (auto candidate = node.DynamicCast<Framegraph::PostProcessNode>();
				candidate && candidate->GetString("shader"_h) == "Shaders/ReflectionCaptureSky.shader")
			{
				sky = candidate;
			}
		}
		if (!linearize || !sky)
		{
			return { false, "The skipped-pass fixture requires real capture depth and sky nodes." };
		}
		linearize->SetRHIResource("target"_h, m_recoveryPhase == 1u ? RHIResourcePtr{} :
			m_recoveryJob->m_graph->GetRenderTarget("LinearDepth"_h));
		m_recoveryJob->m_bRetry = false;
		m_recoveryJob->m_bSubmitted = false;
		RHISemaphorePtr ready;
		const bool accepted = m_recoveryJob->Submit(ready);
		m_forcedPassSkipped = linearize->GetDrawCallStats().m_numInstances == 0u &&
			sky->GetDrawCallStats().m_numInstances == 1u;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto finish = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(finish, true);
		if (m_recoveryPhase == 2u && m_recoveryJob->m_bSubmitted)
		{
			m_recoveryReadback = driver->CreateBuffer(16u * 16u * 8u, EBufferUsageBit::BufferTransferDst_Bit,
				EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
			commands->ImageMemoryBarrier(finish, m_recoveryJob->m_cubemap, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(finish, m_recoveryJob->m_cubemap->GetFace(2u), m_recoveryReadback);
			commands->MemoryBarrier(finish, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->ImageMemoryBarrier(finish, m_recoveryJob->m_cubemap, EImageLayout::ShaderReadOnlyOptimal);
		}
		commands->EndCommandList(finish);
		m_recoveryFence = m_recoveryJob->m_completion;
		if (!driver->SubmitCommandList(finish, m_recoveryFence, {}, ready))
		{
			return { false, "The skipped-pass fixture could not submit its retirement fence." };
		}
		m_recoveryJob->m_submission->Complete(accepted && m_recoveryJob->m_bSubmitted);
		if (!accepted)
		{
			m_recoveryError = "A capture GPU command submission failed during the skipped-pass fixture.";
		}
		return {};
	}

	CheckResult CheckCaptureCancellation()
	{
		if (m_bCancelled)
		{
			return {};
		}
		if (!m_cancellationState)
		{
			m_cancellationState = TSharedPtr<LocalReflectionCaptureState>::Make();
			m_cancellationGeneration = m_cancellationState->Request(LocalRequest(glm::vec3(0)));
		}
		auto first = m_cancellationState->Capture(RHI::RHISceneViewPtr::Make());
		if (!first)
		{
			return m_cancellationState->GetStatus().m_state == ELocalReflectionCaptureState::Pending ? CheckResult{} :
				CheckResult{ false, "The cancellation fixture could not warm its capture shaders." };
		}
		if (first->m_generation != m_cancellationGeneration)
		{
			return { false, "The cancellation fixture started a different generation." };
		}
		m_cancellationState->Cancel(first);
		const auto cancelled = m_cancellationState->GetStatus();
		if (cancelled.m_state != ELocalReflectionCaptureState::Pending ||
			cancelled.m_requestedGeneration != m_cancellationGeneration)
		{
			return { false, "Cancelling an unsubmitted capture lost its pending request." };
		}
		auto retried = m_cancellationState->Capture(RHI::RHISceneViewPtr::Make());
		if (!retried || retried->m_generation != m_cancellationGeneration ||
			!SameParameters(retried->m_request.m_parameters, first->m_request.m_parameters))
		{
			return { false, "A skipped render submission did not preserve the capture generation and parameters." };
		}
		const auto latestRequest = LocalRequest(glm::vec3(1.25f, 0, 0));
		const auto latestGeneration = m_cancellationState->Request(latestRequest);
		m_cancellationState->Cancel(retried);
		auto latest = m_cancellationState->Capture(RHI::RHISceneViewPtr::Make());
		if (!latest || latestGeneration <= m_cancellationGeneration || latest->m_generation != latestGeneration ||
			!SameParameters(latest->m_request.m_parameters, latestRequest.m_parameters))
		{
			return { false, "Cancelling a superseded capture replaced the newer queued request." };
		}
		m_cancellationState->Reset();
		m_cancellationState->Cancel(latest);
		if (m_cancellationState->Capture(RHI::RHISceneViewPtr::Make()))
		{
			return { false, "Cancelling a reset capture resurrected its obsolete request." };
		}
		const auto reset = m_cancellationState->GetStatus();
		if (reset.m_state != ELocalReflectionCaptureState::Idle || reset.m_requestedGeneration != 0u ||
			reset.m_publishedGeneration != 0u || reset.m_capturesSubmitted != 0u)
		{
			return { false, "The cancellation fixture changed publication or submission counters without GPU work." };
		}
		m_cancellationState.Clear();
		return { true, "Cancelling an unsubmitted capture retained its generation/parameters for retry; "
			"a newer queued request took precedence, and reset prevented resurrection without any GPU submissions." };
	}

	CheckResult CheckWarm()
	{
		if (m_bCancelled)
		{
			return {};
		}
		if (!MatchesLocalPublication(m_localGeneration, m_localParameters, m_localTextures))
		{
			return { false, "Warm frames changed the accepted reflection publication." };
		}
		if (++m_warmObservations < 8u)
		{
			return {};
		}
		return { true, "Eight warm observations retained the same reflection maps and parameters with no capture submissions." };
	}

	CheckResult CheckReset()
	{
		if (m_bCancelled)
		{
			return {};
		}
		if (!MatchesLocalPublication(0u, {}, {}))
		{
			if (m_resetObservations == 0u && MatchesLocalPublication(m_localGeneration, m_localParameters, m_localTextures))
			{
				return {};
			}
			return { false, "Reset allowed an incomplete capture to replace or restore the local reflection." };
		}
		const StringHash names[] = { "g_skyCubemap"_h, "g_envCubemap"_h, "g_irradianceCubemap"_h, "g_sheenEnvCubemap"_h };
		for (size_t i = 0u; i < m_publishedTextures.size(); ++i)
		{
			if (m_frameGraph->GetSampler(names[i]) != m_publishedTextures[i])
			{
				return { false, "Reset of a local reflection changed the distant Sky/Environment publication." };
			}
		}
		if (++m_resetObservations < 12u)
		{
			return {};
		}
		return { true, "Reset during capture/filtering cleared local parameters, maps, generation and readiness for twelve "
			"observations; stale completion did not republish, and distant Sky/Environment resources stayed intact." };
	}

	// Render owns validation/readback state; Main initializes the private fixture materials.
	std::atomic_bool m_bCancelled = false;
	RHI::RHIFrameGraphPtr m_frameGraph;
	std::array<SkyParameters, 3> m_parameters;
	std::array<RHI::RHITexturePtr, 4> m_publishedTextures;
	std::array<uint32_t, 2> m_pendingObservations{};
	std::array<RHI::RHITexturePtr, 2> m_localTextures{};
	Framegraph::LocalReflectionParameters m_localParameters;
	uint64_t m_localGeneration = 0u;
	std::array<RHI::RHIBufferPtr, 18> m_readbacks;
	std::array<uint32_t, 3> m_readbackSizes{};
	std::array<RHI::EFormat, 3> m_readbackFormats{};
	RHI::RHIFencePtr m_readbackCompletion;
	RHI::RHICubemapPtr m_retainedRaw;
	std::array<uint64_t, 6> m_retainedHashes{};
	Memory::ObjectAllocatorPtr m_materialAllocator = Memory::ObjectAllocatorPtr::Make();
	TVector<MaterialPtr> m_fixtureMaterials;
	ShaderSetPtr m_markerShader;
	Stage m_stage = Stage::Initialize;
	bool m_bRequestedLatest = false;
	float m_shadowedReceiver = 0.0f;
	float m_unshadowedReceiver = 0.0f;
	std::string m_shadowEvidence;
	TRefPtr<LocalReflectionCaptureJob> m_recoveryJob;
	RHI::RHIRenderSubmissionContextPtr m_recoveryOuter;
	RHI::RHIFencePtr m_recoveryFence;
	RHI::RHIBufferPtr m_recoveryReadback;
	uint32_t m_recoveryPhase = 0u;
	uint32_t m_recoveryAttempts = 0u;
	bool m_forcedPassSkipped = false;
	std::string m_recoveryError;
	TSharedPtr<LocalReflectionCaptureState> m_cancellationState;
	uint64_t m_cancellationGeneration = 0u;
	uint32_t m_warmObservations = 0u;
	uint32_t m_resetObservations = 0u;
};

SkyEnvironmentCaptureTestComponent::~SkyEnvironmentCaptureTestComponent() = default;

bool SkyEnvironmentCaptureTestComponent::PrepareReflectionMarkers(std::string& error)
{
	if (m_bMarkersInitialized)
	{
		return GetWorld()->GetCurrentFrame() > m_markersReadyFrame + 2u;
	}
	if (!m_capture->m_markerShader)
	{
		const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Unlit.shader");
		if (!info)
		{
			error = "The emissive reflection marker shader is missing.";
			return false;
		}
		App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_capture->m_markerShader);
	}
	if (!m_capture->m_markerShader || !m_capture->m_markerShader->IsReady())
	{
		return false;
	}
	auto objects = m_markers;
	objects.Add(m_shadowReceiver);
	objects.Add(m_shadowBlocker);
	for (auto marker : objects)
	{
		auto mesh = marker->GetComponent<MeshRendererComponent>();
		if (!mesh || !mesh->GetModel() || !mesh->GetModel()->IsReady() || mesh->GetMaterials().IsEmpty())
		{
			return false;
		}
		for (const auto& material : mesh->GetMaterials())
		{
			if (!material || !material->IsReady())
			{
				return false;
			}
		}
	}
	for (size_t i = 0u; i < m_markers.Num(); ++i)
	{
		auto material = MaterialPtr::Make(m_capture->m_materialAllocator, FileId::Invalid);
		material->SetShader(m_capture->m_markerShader);
		material->SetRenderState(RHI::RenderState(true, true, 0.0f, false, RHI::ECullMode::Back,
			RHI::EBlendMode::None, RHI::EFillMode::Fill, "Opaque"_h.GetHash()));
		material->SetUniform("material.albedo"_h, glm::vec4(1));
		material->SetUniform("material.emission"_h, glm::vec4(MarkerColors[i], 0));
		material->UpdateRHIResourceAndUniforms();
		m_capture->m_fixtureMaterials.Add(material);
		auto mesh = m_markers[i]->GetComponent<MeshRendererComponent>();
		for (auto& slot : mesh->GetMaterials())
		{
			slot = material;
		}
		mesh->GetData().MarkDirty();
	}
	for (auto object : { m_shadowReceiver, m_shadowBlocker })
	{
		auto mesh = object->GetComponent<MeshRendererComponent>();
		const bool receiver = object == m_shadowReceiver;
		auto source = mesh->GetMaterials()[0];
		auto material = MaterialPtr::Make(m_capture->m_materialAllocator, FileId::Invalid);
		material->SetShader(receiver ? source->GetShader() : m_capture->m_markerShader);
		material->SetRenderState(source->GetRenderState());
		if (receiver)
		{
			material->SetUniform("material.baseColorFactor"_h, glm::vec4(0.5f, 0.5f, 0.5f, 1));
			material->SetUniform("material.emissiveFactor"_h, glm::vec4(0));
			material->SetUniform("material.roughnessFactor"_h, 1.0f);
			material->SetUniform("material.metallicFactor"_h, 0.0f);
			material->SetUniform("material.normalScale"_h, 1.0f);
			material->SetUniform("material.alphaCutoff"_h, 0.5f);
			material->SetUniform("material.occlusionStrength"_h, 1.0f);
		}
		else
		{
			material->SetUniform("material.albedo"_h, glm::vec4(1));
			material->SetUniform("material.emission"_h, glm::vec4(0.01f, 0.01f, 0.01f, 0));
		}
		material->UpdateRHIResourceAndUniforms();
		m_capture->m_fixtureMaterials.Add(material);
		for (auto& slot : mesh->GetMaterials())
		{
			slot = material;
		}
		mesh->GetData().MarkDirty();
	}
	m_bMarkersInitialized = true;
	m_markersReadyFrame = GetWorld()->GetCurrentFrame();
	return false;
}

void SkyEnvironmentCaptureTestComponent::BeginPlay()
{
	TestCaseComponent::BeginPlay();
	auto cameraObject = GetWorld()->Instantiate("Sky capture test camera");
	auto camera = cameraObject->AddComponent<CameraComponent>();
	camera->SetZNear(0.1f);
	camera->SetZFar(1000.0f);
	camera->SetFov(120.0f);
	cameraObject->GetTransformComponent().SetRotation(glm::quat_cast(glm::inverse(
		glm::lookAtRH(glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0)))));
	for (size_t i = 0u; i < MarkerPositions.size(); ++i)
	{
		auto marker = GetWorld()->Instantiate("Reflection emission marker");
		marker->SetMobilityType(EMobilityType::Stationary);
		marker->GetTransformComponent().SetPosition(glm::vec4(MarkerPositions[i], 1));
		auto mesh = marker->AddComponent<MeshRendererComponent>();
		if (!mesh->LoadModel("Models/Box/Box.gltf"))
		{
			MarkFailed("Cannot load reflection marker geometry.");
			return;
		}
		const auto bounds = mesh->GetModel()->GetBoundsAABB();
		const glm::vec3 dimensions = bounds.m_max - bounds.m_min;
		if (!bounds.IsValid() || glm::any(glm::lessThanEqual(dimensions, glm::vec3(0))))
		{
			MarkFailed("Reflection marker geometry has invalid bounds.");
			return;
		}
		marker->GetTransformComponent().SetScale(glm::vec4(glm::vec3(i == 6u ? 0.5f : 1.6f) / dimensions, 1));
		m_markers.Add(marker);
	}
	const auto addShadowBox = [&](const char* name, const glm::vec3& position, const glm::vec3& dimensions)
	{
		auto object = GetWorld()->Instantiate(name);
		object->SetMobilityType(EMobilityType::Stationary);
		object->GetTransformComponent().SetPosition(glm::vec4(position, 1));
		auto mesh = object->AddComponent<MeshRendererComponent>();
		if (!mesh->LoadModel("Models/Box/Box.gltf"))
		{
			return GameObjectPtr{};
		}
		const auto bounds = mesh->GetModel()->GetBoundsAABB();
		object->GetTransformComponent().SetScale(glm::vec4(dimensions / (bounds.m_max - bounds.m_min), 1));
		return object;
	};
	m_shadowReceiver = addShadowBox("Reflection shadow receiver", glm::vec3(2, -2, 6), glm::vec3(3, 3, 0.1f));
	m_shadowBlocker = addShadowBox("Reflection shadow blocker", glm::vec3(3, -0.5f, 4), glm::vec3(0.65f));
	if (!m_shadowReceiver || !m_shadowBlocker)
	{
		MarkFailed("Cannot load lit reflection shadow geometry.");
		return;
	}
	m_localLight = GetWorld()->Instantiate("Reflection point shadow light");
	m_localLight->GetTransformComponent().SetPosition(glm::vec4(4, 1, 2, 1));
	auto light = m_localLight->AddComponent<LightComponent>();
	light->SetLightType(ELightType::Point);
	light->SetRadius(20.0f);
	light->SetIntensity(glm::vec3(1000000.0f));
	light->SetIndirectLightingIntensity(0.0f);
	light->SetShadowType(RHI::EShadowType::PCF);
	light->SetShadowFilter(ELightShadowFilter::Hard);
	light->SetShadowQuality(ELightShadowQuality::VeryLow);
}

void SkyEnvironmentCaptureTestComponent::Tick(float)
{
	if (IsFinished())
	{
		return;
	}
	if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 120000)
	{
		if (m_capture)
		{
			m_capture->m_bCancelled = true;
		}
		MarkFailed("Sky and GPU scene reflection validation did not complete within 120 seconds.");
		return;
	}
	if (!m_capture)
	{
		auto renderer = App::GetSubmodule<RHI::Renderer>();
		if (!renderer || !renderer->GetFrameGraph() || !renderer->GetFrameGraph()->GetRHI())
		{
			return;
		}
		m_capture = TSharedPtr<CaptureState>::Make(renderer->GetFrameGraph()->GetRHI());
	}
	auto lighting = GetWorld()->GetECS<LightingECS>();
	if (m_check)
	{
		if (!m_check->IsFinished())
		{
			return;
		}
		const CheckResult result = m_check->GetResult();
		m_check.Clear();
		if (!result.m_bPassed && !result.m_message.empty())
		{
			MarkFailed(result.m_message);
			return;
		}
		if (result.m_bPassed)
		{
			if (!m_bSkyCaptureComplete)
			{
				AddJournalEvent("SkyEnvironmentCaptureEvidence", result.m_message);
				m_bSkyCaptureComplete = true;
			}
			else if (m_localStage == LocalStage::FirstCapture)
			{
				AddJournalEvent("LocalReflectionPixels", result.m_message);
				const auto superseded = lighting->RequestLocalReflection(LocalRequest(glm::vec3(-0.25f, 0, 0)));
				m_expectedLocalGeneration = lighting->RequestLocalReflection(LocalRequest(glm::vec3(0.5f, 0.3f, 0)));
				if (!superseded || m_expectedLocalGeneration <= superseded)
				{
					MarkFailed("Queued reflection generations were rejected or did not advance.");
					return;
				}
				++m_expectedSubmissions;
				m_localStage = LocalStage::Replacement;
			}
			else if (m_localStage == LocalStage::Replacement)
			{
				AddJournalEvent("LocalReflectionReplacement", result.m_message +
					" The superseded request was coalesced; retained previous raw pixels remained byte-identical.");
				m_localLight->GetComponent<LightComponent>()->SetShadowType(RHI::EShadowType::None);
				m_expectedLocalGeneration = lighting->RequestLocalReflection(LocalRequest(glm::vec3(0.5f, 0.3f, 0)));
				if (!m_expectedLocalGeneration)
				{
					MarkFailed("The unshadowed reflection request was rejected.");
					return;
				}
				++m_expectedSubmissions;
				m_localStage = LocalStage::ShadowsDisabled;
			}
			else if (m_localStage == LocalStage::ShadowsDisabled)
			{
				AddJournalEvent("LocalReflectionShadowPixels", result.m_message);
				m_localStage = LocalStage::CaptureRecovery;
			}
			else if (m_localStage == LocalStage::CaptureRecovery)
			{
				AddJournalEvent("LocalReflectionCaptureRecovery", result.m_message);
				m_localStage = LocalStage::CaptureCancellation;
			}
			else if (m_localStage == LocalStage::CaptureCancellation)
			{
				AddJournalEvent("LocalReflectionCaptureCancellation", result.m_message);
				m_localStage = LocalStage::Warm;
			}
			else if (m_localStage == LocalStage::Warm)
			{
				AddJournalEvent("LocalReflectionWarm", result.m_message);
				m_expectedLocalGeneration = lighting->RequestLocalReflection(LocalRequest(glm::vec3(-0.5f, 0, 0)));
				if (!m_expectedLocalGeneration)
				{
					MarkFailed("The reset fixture capture was rejected.");
					return;
				}
				++m_expectedSubmissions;
				m_localStage = LocalStage::AwaitReset;
			}
			else if (m_localStage == LocalStage::Reset)
			{
				AddJournalEvent("LocalReflectionEvidence", result.m_message);
				MarkPassed();
				return;
			}
		}
	}

	if (!m_bSkyCaptureComplete)
	{
		m_check = Tasks::CreateTaskWithResult<CheckResult>("Validate sky environment capture"_h,
			[capture = m_capture]() { return capture->Check(); }, EThreadType::Render);
		m_check->Run();
		return;
	}
	std::string error;
	if (!PrepareReflectionMarkers(error))
	{
		if (!error.empty())
		{
			MarkFailed(error);
		}
		return;
	}
	if (m_localStage == LocalStage::None)
	{
		m_expectedSubmissions = lighting->GetLocalReflectionStatus().m_capturesSubmitted + 1u;
		m_expectedLocalGeneration = lighting->RequestLocalReflection(LocalRequest(glm::vec3(0)));
		if (!m_expectedLocalGeneration)
		{
			MarkFailed("A valid scene reflection request was rejected.");
			return;
		}
		m_localStage = LocalStage::FirstCapture;
	}
	const auto status = lighting->GetLocalReflectionStatus();
	if (status.m_state == ELocalReflectionCaptureState::Failed || status.m_capturesSubmitted > m_expectedSubmissions)
	{
		MarkFailed("GPU reflection capture failed or submitted redundant capture work.");
		return;
	}
	if (m_localStage == LocalStage::AwaitReset)
	{
		if (status.m_state != ELocalReflectionCaptureState::Capturing &&
			status.m_state != ELocalReflectionCaptureState::Filtering)
		{
			return;
		}
		lighting->ResetLocalReflection();
		m_localStage = LocalStage::Reset;
	}
	if (m_localStage == LocalStage::Reset)
	{
		const auto reset = lighting->GetLocalReflectionStatus();
		if (reset.m_requestedGeneration != 0u || reset.m_publishedGeneration != 0u ||
			lighting->GetLocalReflectionCapture().m_cubemap)
		{
			MarkFailed("LightingECS retained a local capture after reset.");
			return;
		}
		m_check = Tasks::CreateTaskWithResult<CheckResult>("Validate local reflection reset"_h,
			[capture = m_capture]() { return capture->CheckReset(); }, EThreadType::Render);
	}
	else if (m_localStage == LocalStage::Warm || m_localStage == LocalStage::CaptureRecovery ||
		m_localStage == LocalStage::CaptureCancellation)
	{
		if (status.m_capturesSubmitted != m_expectedSubmissions || status.m_publishedGeneration != m_expectedLocalGeneration)
		{
			MarkFailed("Warm frames changed the accepted capture generation or work count.");
			return;
		}
		m_check = Tasks::CreateTaskWithResult<CheckResult>("Validate retained local reflection"_h,
			[capture = m_capture, stage = m_localStage]()
			{
				if (stage == LocalStage::CaptureRecovery)
				{
					return capture->CheckCaptureRecovery();
				}
				if (stage == LocalStage::CaptureCancellation)
				{
					return capture->CheckCaptureCancellation();
				}
				return capture->CheckWarm();
			}, EThreadType::Render);
	}
	else
	{
		const auto raw = lighting->GetLocalReflectionCapture();
		const auto parameters = LocalRequest(m_localStage == LocalStage::FirstCapture ?
			glm::vec3(0) : glm::vec3(0.5f, 0.3f, 0)).m_parameters;
		m_check = Tasks::CreateTaskWithResult<CheckResult>("Validate GPU local reflection pixels"_h,
			[capture = m_capture, raw, generation = m_expectedLocalGeneration, parameters,
				shadowsDisabled = m_localStage == LocalStage::ShadowsDisabled]()
			{
				return capture->CheckLocalReflection(raw, generation, parameters, shadowsDisabled);
			}, EThreadType::Render);
	}
	m_check->Run();
}

void SkyEnvironmentCaptureTestComponent::EndPlay()
{
	if (m_capture)
	{
		m_capture->m_bCancelled = true;
		GetWorld()->GetECS<LightingECS>()->ResetLocalReflection();
	}
	m_check.Clear();
	m_capture.Clear();
	m_markers.Clear();
	m_shadowReceiver.Clear();
	m_shadowBlocker.Clear();
	m_localLight.Clear();
	TestCaseComponent::EndPlay();
}
