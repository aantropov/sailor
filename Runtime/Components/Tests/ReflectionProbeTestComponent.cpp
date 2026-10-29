#include "Components/Tests/ReflectionProbeTestComponent.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/ReflectionProbeComponent.h"
#include "Components/SkyComponent.h"
#include "ECS/TransformECS.h"
#include "ECS/ReflectionProbeRendering.h"
#include "ECS/StaticMeshRendererECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Platform/Time.h"
#include "RHI/Renderer.h"
#include "RHI/Scene.h"
#include "RHI/Texture.h"

#include <format>

using namespace Sailor;

namespace
{
	const std::array<glm::vec3, 2> ProbePositions{ glm::vec3(-5, 0, 0), glm::vec3(5, 0, 0) };
}

ReflectionProbeTestComponent::~ReflectionProbeTestComponent()
{
	for (auto& material : m_materials)
	{
		if (material)
		{
			material.DestroyObject(m_allocator);
		}
	}
}

GameObjectPtr ReflectionProbeTestComponent::AddBox(const char* name, glm::vec3 position, glm::vec3 dimensions)
{
	auto object = GetWorld()->Instantiate(name);
	object->SetMobilityType(EMobilityType::Stationary);
	object->GetTransformComponent().SetPosition(glm::vec4(position, 1));
	auto mesh = object->AddComponent<MeshRendererComponent>();
	if (!mesh->LoadModel("Models/Box/Box.gltf"))
	{
		return {};
	}
	const auto bounds = mesh->GetModel()->GetBoundsAABB();
	object->GetTransformComponent().SetScale(glm::vec4(dimensions / (bounds.m_max - bounds.m_min), 1));
	return object;
}

void ReflectionProbeTestComponent::BeginPlay()
{
	TestCaseComponent::BeginPlay();
	m_camera = GetWorld()->Instantiate("Reflection probe validation camera");
	auto camera = m_camera->AddComponent<CameraComponent>();
	camera->SetFov(55.0f);
	camera->SetZNear(0.1f);
	camera->SetZFar(100.0f);
	m_camera->GetTransformComponent().SetPosition(glm::vec4(0, 0, 18, 1));
	auto sky = GetWorld()->Instantiate("Reflection probe validation sky")->AddComponent<SkyComponent>();
	sky->SetSunIlluminance(glm::vec3(0));
	sky->SetAmbient(0.0f);
	sky->SetCloudsDensity(0.0f);
	sky->SetCloudsCoverage(0.0f);
	for (uint32_t index = 0u; index < 2u; ++index)
	{
		m_mirrors[index] = AddBox(index == 0u ? "Red chrome mirror" : "Green chrome mirror",
			ProbePositions[index], glm::vec3(2.5f, 3, 0.05f));
		m_markers[index] = AddBox(index == 0u ? "Red reflection marker" : "Green reflection marker",
			ProbePositions[index] + glm::vec3(3, 0, 0), glm::vec3(0.1f, 8, 8));
		if (!m_mirrors[index] || !m_markers[index])
		{
			MarkFailed("Reflection probe validation geometry could not be loaded.");
			return;
		}
		m_mirrors[index]->GetTransformComponent().SetRotation(glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0)));
	}
	m_renderState = GetWorld()->GetECS<ReflectionsECS>()->GetRenderState();
}

bool ReflectionProbeTestComponent::PrepareMaterials()
{
	if (!m_materials.IsEmpty())
	{
		if (GetWorld()->GetCurrentFrame() <= m_stageFrame + 4u)
		{
			return false;
		}
		for (const auto& material : m_materials)
		{
			if (!material->IsReady())
			{
				return false;
			}
		}
		for (uint32_t index = 0u; index < 2u; ++index)
		{
			m_probes[index] = GetWorld()->Instantiate(index == 0u ? "Red reflection probe" : "Green reflection probe");
			m_probes[index]->GetTransformComponent().SetPosition(glm::vec4(ProbePositions[index], 1));
			m_components[index] = m_probes[index]->AddComponent<ReflectionProbeComponent>();
			m_components[index]->SetRadius(4.0f);
			m_components[index]->SetResolution(32u);
			m_components[index]->SetUpdateMode(EReflectionProbeUpdateMode::OnLoad);
		}
		return true;
	}
	if (!m_markerShader || !m_mirrorShader)
	{
		for (const auto source : { std::pair("Shaders/Unlit.shader", &m_markerShader),
			std::pair("Shaders/Standard_glTF.shader", &m_mirrorShader) })
		{
			const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(source.first);
			if (info)
			{
				App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), *source.second);
			}
		}
	}
	if (!m_markerShader || !m_mirrorShader || !m_markerShader->IsReady() || !m_mirrorShader->IsReady())
	{
		return false;
	}
	for (auto object : { m_mirrors[0], m_mirrors[1], m_markers[0], m_markers[1] })
	{
		auto mesh = object->GetComponent<MeshRendererComponent>();
		if (!mesh->GetModel()->IsReady() || mesh->GetMaterials().IsEmpty())
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
	auto mirror = MaterialPtr::Make(m_allocator, FileId::Invalid);
	mirror->SetShader(m_mirrorShader);
	mirror->SetRenderState(RHI::RenderState(true, true, 0, false, RHI::ECullMode::Back,
		RHI::EBlendMode::None, RHI::EFillMode::Fill, "Opaque"_h.GetHash()));
	mirror->SetUniform("material.baseColorFactor"_h, glm::vec4(1));
	mirror->SetUniform("material.emissiveFactor"_h, glm::vec4(0));
	mirror->SetUniform("material.roughnessFactor"_h, 0.03f);
	mirror->SetUniform("material.metallicFactor"_h, 1.0f);
	mirror->SetUniform("material.normalScale"_h, 1.0f);
	mirror->SetUniform("material.occlusionStrength"_h, 1.0f);
	mirror->SetUniform("material.alphaCutoff"_h, 0.5f);
	mirror->UpdateRHIResourceAndUniforms();
	m_materials.Add(mirror);
	for (uint32_t index = 0u; index < 2u; ++index)
	{
		auto marker = MaterialPtr::Make(m_allocator, FileId::Invalid);
		marker->SetShader(m_markerShader);
		marker->SetRenderState(mirror->GetRenderState());
		marker->SetUniform("material.albedo"_h, glm::vec4(1));
		marker->SetUniform("material.emission"_h, index == 0u ? glm::vec4(6, 0.01f, 0.01f, 0) : glm::vec4(0.01f, 6, 0.01f, 0));
		marker->UpdateRHIResourceAndUniforms();
		m_materials.Add(marker);
		for (auto target : { std::pair(m_mirrors[index], mirror), std::pair(m_markers[index], marker) })
		{
			auto mesh = target.first->GetComponent<MeshRendererComponent>();
			for (auto& material : mesh->GetMaterials())
			{
				material = target.second;
			}
			mesh->GetData().MarkDirty();
		}
	}
	m_stageFrame = GetWorld()->GetCurrentFrame();
	return false;
}

bool ReflectionProbeTestComponent::HasProbeIndex(uint32_t object, uint32_t expected) const
{
	const auto version = GetWorld()->GetECS<StaticMeshRendererECS>()->GetRHIScene()->GetCurrentVersion();
	if (!version)
	{
		return false;
	}
	const auto component = m_mirrors[object]->GetComponent<MeshRendererComponent>()->GetComponentIndex();
	for (const auto& handles : { version->m_staticHandles, version->m_stationaryHandles, version->m_dynamicHandles })
	{
		if (!handles)
		{
			continue;
		}
		for (auto handle : *handles)
		{
			const RHI::RHISceneInstanceRecord* record = nullptr;
			if (version->Resolve(handle, record) && record->m_producerKey == component)
			{
				return record->m_reflectionProbeIndex == expected;
			}
		}
	}
	return false;
}

bool ReflectionProbeTestComponent::RequestPixels()
{
	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto graph = renderer ? renderer->GetFrameGraph() : FrameGraphPtr{};
	if (!graph)
	{
		return false;
	}
	auto node = graph->GetRHI()->GetGraphNode("CopyTextureToRam"_h).DynamicCast<Framegraph::CopyTextureToRamNode>();
	if (!node)
	{
		MarkFailed("Reflection probe validation requires the frame capture node.");
		return false;
	}
	m_capture = node->DoOneCapture();
	return true;
}

bool ReflectionProbeTestComponent::CheckPixels(bool removed)
{
	if (!m_capture || !m_capture->IsFinished())
	{
		return false;
	}
	const auto frame = m_capture->GetResult();
	TVector<glm::u8vec4> pixels;
	if (!frame || !frame->CopySrgbPixels(pixels))
	{
		MarkFailed("Reflection probe frame readback failed.");
		return false;
	}
	const auto& camera = m_camera->GetComponent<CameraComponent>()->GetData();
	std::array<glm::vec3, 2> colors{};
	for (uint32_t object = 0u; object < 2u; ++object)
	{
		const glm::vec4 clip = camera.GetProjectionMatrix() * camera.GetViewMatrix() * glm::vec4(ProbePositions[object], 1);
		const glm::vec2 center = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * glm::vec2(frame->m_extent);
		for (int32_t y = -2; y <= 2; ++y)
		{
			for (int32_t x = -2; x <= 2; ++x)
			{
				const int32_t column = glm::clamp(int32_t(center.x) + x, 0, frame->m_extent.x - 1);
				const int32_t row = glm::clamp(int32_t(center.y) + y, 0, frame->m_extent.y - 1);
				colors[object] += glm::vec3(pixels[row * frame->m_extent.x + column]) / 25.0f;
			}
		}
	}
	std::string error;
	CaptureScreenshot(*frame, removed ? "reflection-probes-removed" : "reflection-probes-initial", error);
	AddJournalEvent("ReflectionProbePixels", std::format("{}: left RGB={:.1f},{:.1f},{:.1f}; right RGB={:.1f},{:.1f},{:.1f}",
		removed ? "removed" : "initial", colors[0].r, colors[0].g, colors[0].b, colors[1].r, colors[1].g, colors[1].b));
	const bool green = colors[1].g > 60.0f && colors[1].g > colors[1].r + 30.0f && colors[1].g > colors[1].b + 30.0f;
	const bool red = colors[0].r > 60.0f && colors[0].r > colors[0].g + 30.0f && colors[0].r > colors[0].b + 30.0f;
	if (!green || (!removed && !red) || (removed && colors[0].r >= m_initialPixels[0].r - 20.0f))
	{
		MarkFailed("Shared chrome material did not sample the selected per-instance reflection maps or removed-probe fallback.");
		return false;
	}
	m_initialPixels = removed ? m_initialPixels : colors;
	m_capture.Clear();
	return true;
}

void ReflectionProbeTestComponent::Tick(float)
{
	if (IsFinished())
	{
		return;
	}
	if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 45000)
	{
		MarkFailed(std::format("Reflection probe component validation timed out in stage {}.", int(m_stage)));
		return;
	}
	const uint64_t frame = GetWorld()->GetCurrentFrame();
	if (m_stage == Stage::Prepare)
	{
		if (PrepareMaterials())
		{
			m_stage = Stage::Initial;
		}
		return;
	}
	if (m_stage == Stage::Initial)
	{
		for (uint32_t index = 0u; index < 2u; ++index)
		{
			m_slots[index] = m_components[index]->GetData().GetProbeIndex();
			const auto status = m_renderState->GetStatus(m_slots[index]);
			if (status.m_publishedGeneration == 0u || status.m_capturesSubmitted == 0u)
			{
				return;
			}
			m_initialGenerations[index] = status.m_publishedGeneration;
			m_initialCaptures[index] = status.m_capturesSubmitted;
		}
		if (!HasProbeIndex(0u, m_slots[0]) || !HasProbeIndex(1u, m_slots[1]))
		{
			MarkFailed("Scene instance records did not select the two authored probe components.");
			return;
		}
		m_stage = Stage::InitialPixels;
		m_stageFrame = frame;
		return;
	}
	if (m_stage == Stage::InitialPixels && frame > m_stageFrame + 4u)
	{
		if (!m_capture)
		{
			RequestPixels();
			return;
		}
		if (CheckPixels(false))
		{
			m_components[0]->SetUpdateMode(EReflectionProbeUpdateMode::EveryFrame);
			m_components[1]->SetUpdateInterval(0.1f);
			m_components[1]->SetUpdateMode(EReflectionProbeUpdateMode::Interval);
			m_stage = Stage::Dynamic;
		}
		return;
	}
	if (m_stage == Stage::Dynamic)
	{
		m_probes[0]->GetTransformComponent().SetPosition(glm::vec4(ProbePositions[0] + glm::vec3(0.2f * std::sin(float(frame)), 0, 0), 1));
		const auto left = m_renderState->GetStatus(m_slots[0]);
		const auto right = m_renderState->GetStatus(m_slots[1]);
		if (left.m_publishedGeneration > m_initialGenerations[0] && right.m_publishedGeneration > m_initialGenerations[1] &&
			left.m_capturesSubmitted >= m_initialCaptures[0] + 2u && right.m_capturesSubmitted > m_initialCaptures[1])
		{
			AddJournalEvent("ReflectionProbeUpdates", std::format("Moving EveryFrame probe completed {} captures; Interval probe completed {} captures.",
				left.m_capturesSubmitted, right.m_capturesSubmitted));
			m_components[0]->SetUpdateMode(EReflectionProbeUpdateMode::OnLoad);
			m_components[1]->SetUpdateMode(EReflectionProbeUpdateMode::OnLoad);
			m_stage = Stage::Freeze;
		}
		return;
	}
	if (m_stage == Stage::Freeze)
	{
		for (auto slot : m_slots)
		{
			const auto status = m_renderState->GetStatus(slot);
			if (status.m_publishedGeneration == 0u || status.m_publishedGeneration != status.m_requestedGeneration)
			{
				return;
			}
		}
		m_retained = m_renderState->GetSnapshot();
		m_mirrors[0]->GetTransformComponent().SetPosition(glm::vec4(ProbePositions[1] + glm::vec3(0, 0, 0.5f), 1));
		m_stageFrame = frame;
		m_stage = Stage::Move;
		return;
	}
	if (m_stage == Stage::Move && frame > m_stageFrame + 3u)
	{
		if (!HasProbeIndex(0u, m_slots[1]))
		{
			MarkFailed("Moving a mesh between probe spheres did not update its scene instance probe index.");
			return;
		}
		m_mirrors[0]->GetTransformComponent().SetPosition(glm::vec4(ProbePositions[0], 1));
		m_probes[0]->RemoveComponent(m_components[0]);
		m_components[0].Clear();
		m_stageFrame = frame;
		m_stage = Stage::Remove;
		return;
	}
	if (m_stage == Stage::Remove && frame > m_stageFrame + 4u)
	{
		const auto snapshot = m_renderState->GetSnapshot();
		if (!HasProbeIndex(0u, RHI::InvalidReflectionProbeIndex) || !HasProbeIndex(1u, m_slots[1]) ||
			snapshot->m_textures[m_slots[0] * 2u] ||
			snapshot->m_textures[m_slots[1] * 2u] != m_retained->m_textures[m_slots[1] * 2u])
		{
			MarkFailed("Removing a probe did not clear its mesh selection while retaining the other probe.");
			return;
		}
		m_stage = Stage::RemovedPixels;
	}
	if (m_stage == Stage::RemovedPixels)
	{
		if (!m_capture)
		{
			RequestPixels();
			return;
		}
		if (CheckPixels(true))
		{
			AddJournalEvent("ReflectionProbeEvidence", "Two authored components, shared-material per-instance reflection pixels, OnLoad/EveryFrame/Interval captures, moving probe, moving mesh, removal and retained other-probe maps passed.");
			MarkPassed();
		}
	}
}

void ReflectionProbeTestComponent::EndPlay()
{
	m_capture.Clear();
	m_renderState.Clear();
	m_retained.Clear();
	TestCaseComponent::EndPlay();
}
