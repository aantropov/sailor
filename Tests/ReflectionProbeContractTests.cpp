#include "Components/ReflectionProbeComponent.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/ReflectionProbeRendering.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "RHI/Material.h"
#include "Support/EcsTestFixtures.h"
#include "Support/ScopeExit.h"

#include <future>
#include <iostream>
#include <limits>

using namespace Sailor;
using namespace Sailor::Tests;

namespace
{
	class ProbeWorld final : public World
	{
	public:
		ProbeWorld() : World("Reflection probe contracts", 0, CreateSystems()) {}
		void AdvanceFrame() { ++m_currentFrame; }
		void Prepare(float deltaTime = 0.0f)
		{
			AdvanceFrame();
			GetECS<TransformECS>()->Tick(deltaTime);
			GetECS<ReflectionsECS>()->Tick(deltaTime);
			GetECS<StaticMeshRendererECS>()->Tick(deltaTime);
			GetECS<TransformECS>()->PostTick();
		}

	private:
		static TVector<ECS::TBaseSystemPtr> CreateSystems()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<ReflectionsECS>::Make());
			systems.Add(TUniquePtr<PublishedMeshTestSystem>::Make());
			return systems;
		}
	};

	class ProbeTestModel final : public Model
	{
	public:
		ProbeTestModel() : Model(FileId::Invalid)
		{
			auto mesh = RHI::RHIMeshPtr::Make();
			mesh->m_vertexDescription = RHI::RHIVertexDescriptionPtr::Make();
			mesh->m_bounds = Math::AABB(glm::vec3(-1), glm::vec3(1));
			m_boundsAabb = mesh->m_bounds;
			m_renderInstances.Add(RenderInstance{ 0u, -1, glm::mat4(1) });
			m_meshes.Add(std::move(mesh));
		}
		bool IsReady() const override { return true; }
	};

	class ProbeTestMaterial final : public Material
	{
	public:
		ProbeTestMaterial() : Material(FileId::Invalid)
		{
			m_renderMetadata.m_renderQueueTag = "Opaque"_h.GetHash();
			m_rhiMaterials.At_Lock(0u) = RHI::RHIMaterialPtr::Make(
				RHI::RenderState{}, RHI::RHIShaderPtr{}, RHI::RHIShaderPtr{});
			m_rhiMaterials.Unlock(0u);
		}
		bool IsReady() const override { return true; }
	};

	void TestSpatialSelection()
	{
		TVector<ReflectionProbeSpatialEntry> entries{
			{ glm::vec3(-1.25f, 0, 0), 3.0f, 7u, "b" },
			{ glm::vec3(1.25f, 0, 0), 3.0f, 3u, "a" },
			{ glm::vec3(-100.5f, 0, 0), 0.5f, 8u, "c" }
		};
		for (uint32_t index = 0u; index < 20u; ++index)
		{
			entries.Add({ glm::vec3(index * 10.0f, 100, 0), 2.0f, index, "d" });
		}
		auto snapshot = TSharedPtr<const ReflectionProbeSpatialSnapshot>::Make(42u, std::move(entries));
		Require(snapshot->GetRevision() == 42u, "the immutable snapshot must preserve its publication revision");
		Require(snapshot->FindNearest(glm::vec3(-2, 0, 0)) == 7u, "selection must choose the nearest containing sphere");
		Require(snapshot->FindNearest(glm::vec3(0)) == 3u, "equal-distance probes must use stable owner ID tie-breaking");
		Require(snapshot->FindNearest(glm::vec3(-101, 0, 0)) == 8u, "negative fractional bounds and sphere boundaries must remain queryable");
		Require(snapshot->FindNearest(glm::vec3(0, 4, 0)) == RHI::InvalidReflectionProbeIndex,
			"an AABB candidate outside the exact sphere must fall back to the sky");
		std::vector<std::future<void>> workers;
		for (uint32_t worker = 0u; worker < 8u; ++worker)
		{
			workers.emplace_back(std::async(std::launch::async, [snapshot]()
			{
				for (uint32_t repeat = 0u; repeat < 1000u; ++repeat)
				{
					Require(snapshot->FindNearest(glm::vec3(0)) == 3u, "worker queries must be immutable and deterministic");
				}
			}));
		}
		for (auto& worker : workers)
		{
			worker.get();
		}
	}

	void TestComponentReflectionAndScheduling()
	{
		ProbeWorld world;
		ScopeExit cleanup([&]() { world.Clear(); });
		auto object = world.Instantiate("Probe");
		auto probe = object->AddComponent<ReflectionProbeComponent>();
		auto* reflections = world.GetECS<ReflectionsECS>();
		const auto state = reflections->GetRenderState();
		const auto& type = ReflectionProbeComponent::GetStaticTypeInfo();
		const auto metadata = type.Serialize();
		Require(metadata["properties"]["reflectionMap"].as<std::string>() == "TObjectPtr<Sailor::Texture>",
			"the editor must see reflectionMap as a normal Texture asset reference");
		const auto& defaults = Reflection::GetCDO(type.Name()).GetProperties();
		Require(defaults["updateInterval"].as<float>() == 20.0f && defaults["resolution"].as<uint32_t>() == 128u,
			"the editor must receive meaningful component defaults without a World");
		world.Prepare();
		const auto slot = probe->GetData().GetProbeIndex();
		const auto generation = probe->GetData().GetSlotGeneration();
		const auto initial = state->GetStatus(slot).m_requestedGeneration;
		Require(initial != 0u, "OnLoad must request an initial dynamic capture");
		object->GetTransformComponent().SetPosition(glm::vec3(1, 0, 0));
		world.Prepare(100.0f);
		Require(state->GetStatus(slot).m_requestedGeneration == initial,
			"OnLoad must not repeat merely because time passes or its owner moves");
		probe->SetUpdateMode(EReflectionProbeUpdateMode::Interval);
		world.Prepare();
		const auto intervalInitial = state->GetStatus(slot).m_requestedGeneration;
		object->GetTransformComponent().SetPosition(glm::vec3(2, 0, 0));
		world.Prepare(19.5f);
		Require(state->GetStatus(slot).m_requestedGeneration == intervalInitial,
			"moving Interval probes must still wait for the authored duration");
		const auto spatialRevision = reflections->GetSpatialSnapshot()->GetRevision();
		world.Prepare(0.5f);
		Require(state->GetStatus(slot).m_requestedGeneration > intervalInitial, "Interval must request a capture at 20 seconds");
		probe->SetUpdateMode(EReflectionProbeUpdateMode::EveryFrame);
		world.Prepare();
		const auto frameInitial = state->GetStatus(slot).m_requestedGeneration;
		world.Prepare();
		Require(state->GetStatus(slot).m_requestedGeneration > frameInitial,
			"EveryFrame must request each prepared frame even when prior work is pending");
		Require(reflections->GetSpatialSnapshot()->GetRevision() == spatialRevision,
			"capture scheduling must not invalidate spatial assignments");
		probe->SetResolution(129u);
		probe->SetRadius(-1.0f);
		probe->SetUpdateInterval(std::numeric_limits<float>::quiet_NaN());
		Require(probe->GetResolution() == 256u && probe->GetRadius() == 0.01f && probe->GetUpdateInterval() == 20.0f,
			"authored values must produce a supported power-of-two capture and finite sphere/timer");
		const auto serialized = probe->GetReflectedData().Serialize();
		ReflectedData data;
		data.Deserialize(serialized);
		auto replacement = world.Instantiate("Reloaded probe")->AddComponent<ReflectionProbeComponent>();
		replacement->ApplyReflection(data);
		Require(replacement->GetResolution() == 256u && replacement->GetRadius() == 0.01f &&
			replacement->GetUpdateMode() == EReflectionProbeUpdateMode::EveryFrame,
			"component configuration must round-trip through the editor/world reflection schema");
		probe->SetUpdateMode(EReflectionProbeUpdateMode::Static);
		world.Prepare();
		Require(state->GetStatus(slot).m_slotGeneration == 0u, "a missing static map must remove stale dynamic capture state");
		world.DestroyImmediate(object);
		world.Prepare();
		auto reused = world.Instantiate("Reused slot")->AddComponent<ReflectionProbeComponent>();
		world.Prepare();
		Require(reused->GetData().GetProbeIndex() == slot && reused->GetData().GetSlotGeneration() > generation,
			"reused slots must advance their lifetime generation");
		const auto reusedStatus = state->GetStatus(slot);
		state->Remove(slot, generation);
		state->Request(slot, generation);
		Framegraph::LocalReflectionCaptureRequest staleRequest;
		staleRequest.m_parameters.m_positionBlend = glm::vec4(0, 0, 0, 0.1f);
		staleRequest.m_parameters.m_minEnabled = glm::vec4(-1, -1, -1, 1);
		staleRequest.m_parameters.m_max = glm::vec4(1);
		state->Upsert(slot, generation, staleRequest);
		Require(state->GetStatus(slot).m_requestedGeneration == reusedStatus.m_requestedGeneration &&
			state->GetStatus(slot).m_slotGeneration == reusedStatus.m_slotGeneration,
			"stale requests, upserts and removals must not affect a replacement probe");
	}

	void TestStaticMeshAssignmentsDoNotRebuildTopologyOrShadows()
	{
		ProbeWorld world;
		auto model = TObjectPtr<ProbeTestModel>::Make(world.GetAllocator());
		auto material = TObjectPtr<ProbeTestMaterial>::Make(world.GetAllocator());
		ScopeExit cleanup([&]()
		{
			world.Clear();
			material.DestroyObject(world.GetAllocator());
			model.DestroyObject(world.GetAllocator());
		});
		auto owner = world.Instantiate("Static mesh");
		owner->SetMobilityType(EMobilityType::Static);
		auto mesh = owner->AddComponent<MeshRendererComponent>();
		mesh->SetModel(model);
		mesh->GetMaterials().Add(material);
		auto* meshes = world.GetECS<StaticMeshRendererECS>();
		meshes->BeginPlay();
		world.Prepare();
		const auto before = meshes->GetRHIScene()->GetCurrentVersion();
		const auto handle = (*before->m_staticHandles)[0];
		const RHI::RHISceneInstanceRecord* original = nullptr;
		Require(before->Resolve(handle, original) && original->m_reflectionProbeIndex == RHI::InvalidReflectionProbeIndex,
			"meshes without probes must use the explicit sky sentinel");
		auto probeOwner = world.Instantiate("New probe");
		auto probe = probeOwner->AddComponent<ReflectionProbeComponent>();
		world.Prepare();
		const auto after = meshes->GetRHIScene()->GetCurrentVersion();
		const RHI::RHISceneInstanceRecord* assigned = nullptr;
		Require(after->Resolve(handle, assigned) && assigned->m_reflectionProbeIndex == probe->GetData().GetProbeIndex(),
			"creating a probe must update an already published, unchanged static mesh");
		Require(assigned->m_topology == original->m_topology && assigned->m_topologyRevision == original->m_topologyRevision &&
			after->m_shadowRevision == before->m_shadowRevision && after->m_spatialRevision == before->m_spatialRevision,
			"probe-only assignment must retain mesh/material topology, shadows and scene spatial indices");
		Require(original->m_reflectionProbeIndex == RHI::InvalidReflectionProbeIndex,
			"older submissions must retain their immutable reflection assignment");
		probeOwner->GetTransformComponent().SetPosition(glm::vec3(100, 0, 0));
		world.Prepare();
		const RHI::RHISceneInstanceRecord* moved = nullptr;
		Require(meshes->GetRHIScene()->GetCurrentVersion()->Resolve(handle, moved) &&
			moved->m_reflectionProbeIndex == RHI::InvalidReflectionProbeIndex,
			"moving a probe outside a static object must remove the old assignment");
		probeOwner->GetTransformComponent().SetPosition(glm::vec3(0));
		probeOwner->GetTransformComponent().SetScale(glm::vec4(1, 3, 2, 1));
		probe->SetRadius(2.0f);
		world.Prepare();
		const auto spatial = world.GetECS<ReflectionsECS>()->GetSpatialSnapshot();
		Require(spatial->FindNearest(glm::vec3(0, 6, 0)) == probe->GetData().GetProbeIndex(),
			"the influence sphere must use the maximum world scale consistently");
		world.DestroyImmediate(probeOwner);
		world.Prepare();
		Require(meshes->GetRHIScene()->GetCurrentVersion()->Resolve(handle, moved) &&
			moved->m_reflectionProbeIndex == RHI::InvalidReflectionProbeIndex,
			"destroying a probe must clear static object assignments");
	}

	void TestCapacityAndWorldReset()
	{
		ProbeWorld world;
		ScopeExit cleanup([&]() { world.Clear(); });
		TVector<TObjectPtr<ReflectionProbeComponent>> probes;
		for (uint32_t index = 0u; index <= RHI::MaxReflectionProbes; ++index)
		{
			probes.Add(world.Instantiate("Capacity probe")->AddComponent<ReflectionProbeComponent>());
		}
		world.Prepare();
		auto* reflections = world.GetECS<ReflectionsECS>();
		Require(reflections->GetSpatialSnapshot()->GetEntries().Num() == RHI::MaxReflectionProbes &&
			probes[RHI::MaxReflectionProbes]->GetData().GetProbeIndex() == RHI::InvalidReflectionProbeIndex,
			"capacity overflow must leave the extra probe unassigned without corrupting the shared table");
		const auto releasedSlot = probes[0]->GetData().GetProbeIndex();
		world.DestroyImmediate(probes[0]->GetOwner());
		world.Prepare();
		Require(probes[RHI::MaxReflectionProbes]->GetData().GetProbeIndex() == releasedSlot,
			"a waiting probe must acquire a slot when capacity becomes available");
		const auto retained = reflections->GetSpatialSnapshot();
		world.Clear();
		Require(reflections->GetSpatialSnapshot()->GetEntries().IsEmpty() &&
			retained->GetEntries().Num() == RHI::MaxReflectionProbes,
			"world reset must publish an empty snapshot while retained worker snapshots remain valid");
	}
}

int main()
{
	try
	{
		TestSpatialSelection();
		TestComponentReflectionAndScheduling();
		TestStaticMeshAssignmentsDoNotRebuildTopologyOrShadows();
		TestCapacityAndWorldReset();
		std::cout << "Reflection probe contracts passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
