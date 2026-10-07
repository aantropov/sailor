#include "Containers/Octree.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Components/AnimatorComponent.h"
#include "Components/LandscapeComponent.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/LightingECS.h"
#include "RHI/SceneView.h"
#include "RHI/Material.h"
#include "RHI/VertexDescription.h"
#include "Settings/GraphicsSettings.h"
#include "Support/EcsTestFixtures.h"
#include "Support/ScopeExit.h"

#include <functional>
#include <iostream>
#include <limits>
#include <utility>

using namespace Sailor;
using namespace Sailor::Tests;

namespace
{
	class PublishedMeshTestModel final : public Model
	{
	public:
		explicit PublishedMeshTestModel(const TVector<glm::mat4>& transforms = { glm::mat4(1.0f) }) : Model(FileId::Invalid)
		{
			for (const auto& matrix : transforms)
			{
				auto mesh = RHI::RHIMeshPtr::Make();
				mesh->m_vertexDescription = RHI::RHIVertexDescriptionPtr::Make();
				mesh->m_bounds = Math::AABB(glm::vec3(-1.0f), glm::vec3(1.0f));
				auto bounds = mesh->m_bounds;
				bounds.Apply(matrix);
				m_boundsAabb.Extend(bounds);
				m_renderInstances.Add(RenderInstance{ static_cast<uint32_t>(m_meshes.Num()), -1, matrix });
				m_meshes.Add(std::move(mesh));
			}
		}
		bool IsReady() const override { return m_ready; }
		bool m_ready = false;
	};

	class PublishedMeshTestMaterial final : public Material
	{
	public:
		explicit PublishedMeshTestMaterial(bool bCastsShadows = false) : Material(FileId::Invalid)
		{
			if (bCastsShadows) m_renderMetadata.m_renderQueueTag = "Opaque"_h.GetHash();
			// CPU scene publication needs a material identity, not a GPU pipeline.
			m_rhiMaterials.At_Lock(0u) = RHI::RHIMaterialPtr::Make(
				RHI::RenderState{}, RHI::RHIShaderPtr{}, RHI::RHIShaderPtr{});
			m_rhiMaterials.Unlock(0u);
		}
		bool IsReady() const override { return true; }
	};

	class AnimationMeshTestWorld final : public World
	{
	public:

		AnimationMeshTestWorld() : World("AnimationMeshTests", 0, CreateEcs()) {}
		void AdvanceFrame() { ++m_currentFrame; }

	private:

		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<AnimationECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			return systems;
		}
	};

	void SetLayoutAnimationPose(AnimationPtr animation, uint32_t bonesCount, float position)
	{
		animation->m_numBones = bonesCount;
		animation->m_numFrames = 1;
		animation->m_frames.Resize(bonesCount);
		animation->m_restPose.Resize(bonesCount);
		animation->m_parentBoneIndices.Resize(bonesCount);
		for (uint32_t index = 0; index < bonesCount; ++index)
		{
			Math::Transform pose;
			pose.m_position = glm::vec4(position, 0.0f, 0.0f, 1.0f);
			animation->m_frames[index] = pose;
			animation->m_restPose[index] = pose;
			animation->m_parentBoneIndices[index] = -1;
		}
		animation->m_skeletonSignature = bonesCount;
		++animation->m_revision;
	}

	class SceneRemovalTestWorld final : public World
	{
	public:
		SceneRemovalTestWorld() : World("Scene removal", 0, CreateEcs()) {}
		using World::DestroyPendingGameObjects;
		void AdvanceFrame() { ++m_currentFrame; }

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<PublishedMeshTestSystem>::Make());
			systems.Add(TUniquePtr<LandscapeECS>::Make());
			return systems;
		}
	};

	void TestSceneRemovalsPublishOnce()
	{
		enum class Removal { Component, Immediate, Deferred };
		for (uint32_t count : { 32u, 64u })
		{
			for (Removal removal : { Removal::Component, Removal::Immediate, Removal::Deferred })
			{
				SceneRemovalTestWorld world;
				auto model = TObjectPtr<PublishedMeshTestModel>::Make(world.GetAllocator());
				model->m_ready = true;
				auto material = TObjectPtr<PublishedMeshTestMaterial>::Make(world.GetAllocator(), true);
				ScopeExit cleanup([&]()
				{
					world.Clear();
					material.DestroyObject(world.GetAllocator());
					model.DestroyObject(world.GetAllocator());
				});
				auto* meshes = world.GetECS<StaticMeshRendererECS>();
				auto* landscapes = world.GetECS<LandscapeECS>();
				TVector<GameObjectPtr> objects;
				TVector<size_t> meshSlots, landscapeSlots;
				const auto addOwner = [&](uint32_t i)
				{
					auto object = world.Instantiate("Scene owner");
					object->SetMobilityType(static_cast<EMobilityType>(i % 3));
					object->GetTransformComponent().SetPosition(glm::vec3(0, 0, -5));
					auto mesh = object->AddComponent<MeshRendererComponent>();
					mesh->SetModel(model);
					mesh->GetMaterials().Add(material);
					auto landscape = object->AddComponent<LandscapeComponent>();
					auto& data = landscapes->GetComponentData(landscape->GetComponentIndex());
					LandscapeChunk chunk;
					chunk.m_buildRevision = 1;
					chunk.m_localBounds = Math::AABB(glm::vec3(-1), glm::vec3(1));
					chunk.m_resource = RHI::RHISceneProxyResourcePtr::Make();
					data.m_chunks.Add(std::move(chunk));
					return object;
				};
				for (uint32_t i = 0; i < count + 3; ++i)
				{
					auto object = addOwner(i);
					meshSlots.Add(object->GetComponent<MeshRendererComponent>()->GetComponentIndex());
					landscapeSlots.Add(object->GetComponent<LandscapeComponent>()->GetComponentIndex());
					objects.Add(object);
				}
				world.GetECS<TransformECS>()->Tick(0.0f);
				meshes->BeginPlay();
				meshes->Tick(0.0f);
				landscapes->BeginPlay();
				const auto capture = [&]()
				{
					auto view = RHI::RHISceneViewPtr::Make();
					meshes->CopySceneView(view);
					landscapes->AppendSceneView(view);
					return view;
				};
				const auto numRecords = [](const RHI::RHISpatialSceneVersionPtr& version)
				{
					const auto& scene = *version->m_sceneVersion;
					return scene.m_staticHandles->Num() + scene.m_stationaryHandles->Num() + scene.m_dynamicHandles->Num();
				};
				auto retained = capture();
				Require(retained->m_sceneVersions.Num() == 2 &&
					numRecords(retained->m_sceneVersions[0]) == count + 3 &&
					numRecords(retained->m_sceneVersions[1]) == count + 3,
					"both actual ECS publishers must contain all fixture records");
				const auto meshRevision = meshes->GetGlobalIlluminationGeometryRevision();
				const auto landscapeRevision = landscapes->GetGlobalIlluminationGeometryRevision();
				for (uint32_t i = 0; i < count; ++i)
				{
					auto object = objects[i];
					if (removal == Removal::Component)
					{
						auto mesh = object->GetComponent<MeshRendererComponent>();
						auto landscape = object->GetComponent<LandscapeComponent>();
						meshes->UnregisterComponent(mesh->GetComponentIndex());
						landscapes->UnregisterComponent(landscape->GetComponentIndex());
						// Release component handles before exercising slot reuse below.
						object->RemoveComponent(mesh);
						object->RemoveComponent(landscape);
					}
					else if (removal == Removal::Immediate) world.DestroyImmediate(object);
					else world.Destroy(object);
				}
				if (removal == Removal::Deferred) world.DestroyPendingGameObjects();
				auto current = capture();
				const auto meshPublications = current->m_sceneVersions[0]->m_revision - retained->m_sceneVersions[0]->m_revision;
				const auto landscapePublications = current->m_sceneVersions[1]->m_revision - retained->m_sceneVersions[1]->m_revision;
				std::cout << "Scene removal: " << count << " owners, mode=" << static_cast<int>(removal) <<
					", mesh publications=" << meshPublications << ", landscape publications=" << landscapePublications << std::endl;
				Require(meshPublications == 1 && landscapePublications == 1,
					"a removal batch must publish each scene once at the next consumer boundary");
				Require(meshes->GetGlobalIlluminationGeometryRevision() != meshRevision &&
					landscapes->GetGlobalIlluminationGeometryRevision() != landscapeRevision,
					"the new publication must invalidate GI for removed static and stationary geometry");
				for (size_t i = 0; i < 2; ++i)
				{
					Require(numRecords(current->m_sceneVersions[i]) == 3 &&
						numRecords(retained->m_sceneVersions[i]) == count + 3,
						"new views must omit deleted records while retained views keep them");
					for (const auto* handles : { retained->m_sceneVersions[i]->m_sceneVersion->m_staticHandles.GetRawPtr(),
						retained->m_sceneVersions[i]->m_sceneVersion->m_stationaryHandles.GetRawPtr(),
						retained->m_sceneVersions[i]->m_sceneVersion->m_dynamicHandles.GetRawPtr() })
					{
						for (const auto handle : *handles)
						{
							const RHI::RHISceneInstanceRecord* record = nullptr;
							Require(retained->m_sceneVersions[i]->m_sceneVersion->Resolve(handle, record) && record && record->m_topology,
								"old records and topology must remain alive after their owners are removed");
						}
					}
				}
				auto unchanged = capture();
				Require(unchanged->m_sceneVersions == current->m_sceneVersions,
					"unchanged captures must reuse both published versions");
				Math::Frustum frustum;
				frustum.ExtractFrustumPlanes(glm::mat4(1), 1, 60, 0.1f, 100);
				Require(current->TraceScene(frustum).Num() == 6 && retained->TraceScene(frustum).Num() == (count + 3) * 2,
					"both old and new spatial roots must agree with their own record versions");

				// Merge a removal with a normal mesh update before the next capture.
				auto survivor = objects[count + 1];
				world.DestroyImmediate(objects[count]);
				world.AdvanceFrame();
				survivor->GetTransformComponent().SetPosition(glm::vec3(3, 0, -5));
				world.GetECS<TransformECS>()->Tick(0.0f);
				meshes->Tick(0.0f);
				auto moved = capture();
				Require(moved->m_sceneVersions[0]->m_revision == current->m_sceneVersions[0]->m_revision + 1 &&
					numRecords(moved->m_sceneVersions[0]) == 2 && numRecords(moved->m_sceneVersions[1]) == 2,
					"Tick updates and pending removals must share one mesh publication");

				world.DestroyImmediate(survivor);
				world.DestroyImmediate(objects[count + 2]);
				auto replacement = addOwner(0);
				Require(meshSlots.Contains(replacement->GetComponent<MeshRendererComponent>()->GetComponentIndex()) &&
					landscapeSlots.Contains(replacement->GetComponent<LandscapeComponent>()->GetComponentIndex()),
					"both component pools must reuse a genuinely removed slot");
				world.GetECS<TransformECS>()->Tick(0.0f);
				meshes->Tick(0.0f);
				auto reused = capture();
				Require(numRecords(reused->m_sceneVersions[0]) == 1 && numRecords(reused->m_sceneVersions[1]) == 1 &&
					reused->TraceScene(frustum).Num() == 2 &&
					reused->m_sceneVersions[0]->m_revision == moved->m_sceneVersions[0]->m_revision + 1 &&
					reused->m_sceneVersions[1]->m_revision == moved->m_sceneVersions[1]->m_revision + 1,
					"slot reuse before publication must retain only the replacement record and bounds");
				world.DestroyImmediate(replacement);
				auto empty = capture();
				Require(numRecords(empty->m_sceneVersions[0]) == 0 && numRecords(empty->m_sceneVersions[1]) == 0 &&
					empty->m_sceneVersions[0]->m_revision == reused->m_sceneVersions[0]->m_revision + 1 &&
					empty->m_sceneVersions[1]->m_revision == reused->m_sceneVersions[1]->m_revision + 1,
					"removing the last records must publish one empty scene, not keep deleted geometry");
				world.Clear();
				Require(capture()->m_sceneVersions.IsEmpty(), "Clear must reset both publication owners");
			}
		}
	}

	AnimationPtr MakeLayoutAnimation(const Memory::ObjectAllocatorPtr& allocator, uint32_t bonesCount, float position)
	{
		auto animation = AnimationPtr::Make(allocator, FileId{});
		SetLayoutAnimationPose(animation, bonesCount, position);
		return animation;
	}

	void TestOctreeRelocationPreservesElementCount()
	{
		TOctree<size_t> octree(glm::ivec3(0), 128, 4);
		const glm::ivec3 positions[] = {
			{ -24, -24, -24 }, { 24, -24, -24 }, { -24, -24, 24 }, { 24, -24, 24 },
			{ -24, 24, -24 }, { 24, 24, -24 }, { -24, 24, 24 }, { 24, 24, 24 }
		};

		for (size_t index = 0; index < 8; index++)
		{
			Require(octree.Insert(positions[index], glm::ivec3(1), index), "octree fixture insertion should succeed");
		}

		Require(octree.Num() == 8, "octree should contain every fixture element");
		Require(octree.Update(glm::ivec3(24, 24, 24), glm::ivec3(1), size_t(0)),
			"moving an element to another octant should succeed");
		Require(octree.Num() == 8, "relocating an existing element must not increase the element count");

		Require(!octree.Update(glm::ivec3(1000), glm::ivec3(1), size_t(0)),
			"moving an element outside the root should fail");
		Require(!octree.Contains(0), "failed relocation should remove the out-of-bounds element");
		Require(octree.Num() == 7, "failed relocation should decrement the element count exactly once");

		Require(octree.Insert(glm::ivec3(-24), glm::ivec3(1), size_t(0)),
			"the removed element should remain insertable");
		Require(octree.Num() == 8, "reinsertion should restore the expected count");

		TOctree<size_t> traceOctree(glm::ivec3(0), 128, 4);
		Require(traceOctree.Insert(glm::ivec3(0, 0, -5), glm::ivec3(1), size_t(42)),
			"the trace fixture insertion should succeed");
		Math::Frustum traceFrustum;
		traceFrustum.ExtractFrustumPlanes(glm::mat4(1.0f), 1.0f, 60.0f, 0.1f, 10.0f);
		TVector<size_t> tracedElements;
		traceOctree.Trace(traceFrustum, tracedElements);
		Require(tracedElements.Num() == 1 && tracedElements[0] == 42,
			"vector frustum tracing should return the matching element exactly once");
		size_t callbackCount = 0;
		traceOctree.Trace(traceFrustum, [&callbackCount](size_t element)
			{
				Require(element == 42, "callback frustum tracing should publish the matching element");
				++callbackCount;
			});
		Require(callbackCount == 1,
			"callback frustum tracing should visit the matching element exactly once");
	}

	void TestFrameZeroMeshPublicationStaysStable()
	{
		PrefabTestWorld world;
		auto object = world.Instantiate("UnchangedIdentityMesh");
		object->SetMobilityType(EMobilityType::Static);
		auto* transforms = world.GetECS<TransformECS>();
		Require(object->GetTransformComponent().GetFrameLastChange() == 0u,
			"the fixture must use a legitimate frame-zero transform");
		auto* meshes = world.GetECS<StaticMeshRendererECS>();
		meshes->BeginPlay();
		const auto slot = meshes->RegisterComponent();
		auto& data = meshes->GetComponentData(slot);
		data.SetOwner(object);
		auto model = TObjectPtr<PublishedMeshTestModel>::Make(world.GetAllocator());
		data.SetModel(model);
		data.GetMaterials().Add(TObjectPtr<PublishedMeshTestMaterial>::Make(world.GetAllocator()));
		meshes->Tick(0.016f);
		Require(meshes->GetRHIScene()->GetCurrentVersion()->m_staticHandles->IsEmpty(),
			"a pending model must not publish a render instance");
		model->m_ready = true;
		world.AdvanceFrame();
		meshes->Tick(0.016f);
		const auto published = meshes->GetRHIScene()->GetCurrentVersion();
		Require(published->m_staticHandles->Num() == 1u,
			"a ready frame-zero model must be published exactly once");
		const auto handle = (*published->m_staticHandles)[0];
		const auto revision = meshes->GetGlobalIlluminationContributorRevision();
		const RHI::RHISceneInstanceRecord* original = nullptr;
		Require(published->Resolve(handle, original) && original,
			"the published instance must resolve");
		for (size_t frame = 0; frame < 32; ++frame)
		{
			world.AdvanceFrame();
			meshes->Tick(0.016f);
			Require(meshes->GetRHIScene()->GetCurrentVersion() == published &&
				meshes->GetGlobalIlluminationContributorRevision() == revision,
				"unchanged frame-zero geometry must not republish or invalidate GI");
		}
		world.AdvanceFrame();
		object->GetTransformComponent().SetPosition(glm::vec3(2.0f, 0.0f, 0.0f));
		transforms->Tick(0.016f);
		transforms->PostTick();
		meshes->Tick(0.016f);
		const auto moved = meshes->GetRHIScene()->GetCurrentVersion();
		const RHI::RHISceneInstanceRecord* current = nullptr;
		Require(moved != published && moved->Resolve(handle, current) && current &&
			current->m_worldMatrix[3].x == 2.0f &&
			current->m_topology == original->m_topology &&
			meshes->GetGlobalIlluminationContributorRevision() != revision,
			"a later transform must update bounds and GI while retaining mesh topology");
		Require(original->m_worldMatrix[3].x == 0.0f &&
			current->m_worldBounds != original->m_worldBounds,
			"transform publication must leave retained records unchanged");
		world.AdvanceFrame();
		object->GetTransformComponent().SetPosition(glm::vec3(4.0f, 0.0f, 0.0f));
		transforms->Tick(0.016f);
		transforms->PostTick();
		meshes->Tick(0.016f);
		const auto movedAgain = meshes->GetRHIScene()->GetCurrentVersion();
		const RHI::RHISceneInstanceRecord* latest = nullptr;
		Require(movedAgain->Resolve(handle, latest) && latest &&
			latest->m_worldMatrix[3].x == 4.0f && current->m_worldMatrix[3].x == 2.0f &&
			latest->m_topology == original->m_topology,
			"successive transform updates must retain topology without lagging a frame");
		meshes->UnregisterComponent(slot);
		auto removedView = RHI::RHISceneViewPtr::Make();
		meshes->CopySceneView(removedView);
		Require(meshes->GetRHIScene()->GetCurrentVersion()->m_staticHandles->IsEmpty(),
			"the next scene capture after unregister must omit the removed instance");
		world.Clear();
	}

	void TestLocalMeshTopologySurvivesSingularOwnerTransforms()
	{
		AnimationMeshTestWorld world;
		auto* transforms = world.GetECS<TransformECS>();
		auto* meshes = world.GetECS<StaticMeshRendererECS>();
		const TVector<glm::mat4> localMatrices{
			glm::translate(glm::mat4(1.0f), glm::vec3(2, 0, 0)),
			glm::translate(glm::mat4(1.0f), glm::vec3(0, 3, 0)) };
		auto model = TObjectPtr<PublishedMeshTestModel>::Make(world.GetAllocator(), localMatrices);
		model->m_ready = true;
		auto material = TObjectPtr<PublishedMeshTestMaterial>::Make(world.GetAllocator(), true);
		auto parent = world.Instantiate("Scaled parent");
		auto owner = world.Instantiate("Multi-mesh child");
		owner->SetMobilityType(EMobilityType::Dynamic);
		owner->SetParent(parent);
		parent->GetTransformComponent().SetPosition(glm::vec3(10, 2, -8));
		owner->GetTransformComponent().SetPosition(glm::vec3(1, 2, 3));
		auto meshComponent = owner->AddComponent<MeshRendererComponent>();
		meshComponent->SetModel(model);
		meshComponent->GetMaterials().Add(material);
		auto clip = MakeLayoutAnimation(world.GetAllocator(), 2, 1.0f);
		auto animator = owner->AddComponent<AnimatorComponent>();
		animator->SetAnimation(clip);
		world.GetECS<AnimationECS>()->Tick(0.0f);
		meshes->BeginPlay();
		RHI::RHISceneVersionPtr first;
		const RHI::RHISceneInstanceRecord* firstRecord = nullptr;
		RHI::RHISceneProxyResourcePtr topology;
		glm::mat4 firstWorld(1.0f);
		for (const auto scale : { glm::vec3(0, 2, 3), glm::vec3(-2, 3, 4), glm::vec3(2), glm::vec3(0) })
		{
			world.AdvanceFrame();
			parent->GetTransformComponent().SetScale(glm::vec4(scale, 1.0f));
			transforms->Tick(0.0f);
			transforms->PostTick();
			meshes->Tick(0.0f);
			const auto version = meshes->GetRHIScene()->GetCurrentVersion();
			Require(version->m_dynamicHandles->Num() == 1, "singular scale must still publish a valid render instance");
			const auto handle = (*version->m_dynamicHandles)[0];
			const RHI::RHISceneInstanceRecord* record = nullptr;
			Require(version->Resolve(handle, record), "the transformed instance must resolve");
			if (!first)
			{
				first = version;
				firstRecord = record;
				firstWorld = record->m_worldMatrix;
				topology = record->m_topology.StaticCast<const RHI::RHISceneProxyResource>();
			}
			Require(record->m_topology == topology && firstRecord->m_worldMatrix == firstWorld,
				"zero and negative scale updates must retain local topology and the old scene publication");
			const auto expectedWorld = parent->GetTransformComponent().GetCachedWorldMatrix() *
				owner->GetTransformComponent().GetTransform().Matrix();
			auto expectedBounds = model->GetBoundsAABB();
			expectedBounds.Apply(expectedWorld);
			Require(AreMatricesNear(record->m_worldMatrix, expectedWorld) && record->m_worldBounds == expectedBounds,
				"the instance record must own the composed parent/child transform and world bounds");
			RHI::RHIVisibleSceneProxy visible(handle, *record, *topology);
			RHI::RHIVisibleShadowCaster shadow(handle, *record, *topology);
			Require(shadow.GetSource() && shadow.GetSource()->m_meshes.Num() == localMatrices.Num() &&
				visible.GetSkeletonOffset() == animator->GetSkeletonOffset() &&
				shadow.GetSkeletonOffset() == visible.GetSkeletonOffset(),
				"main and shadow views must use the same instance skeleton with all source meshes");
			for (size_t index = 0; index < localMatrices.Num(); ++index)
			{
				Require(topology->m_proxy.m_meshModelMatrices[index] == localMatrices[index] &&
					shadow.GetSource()->m_meshes[index].m_localMatrix == localMatrices[index] &&
					AreMatricesNear(visible.ResolveMeshWorldMatrix(index), expectedWorld * localMatrices[index]) &&
					AreMatricesNear(shadow.ResolveMeshWorldMatrix(shadow.GetSource()->m_meshes[index]), expectedWorld * localMatrices[index]),
					"both passes must compose original local matrices without inverse reconstruction or double transformation");
			}
		}
		auto beforeRebuild = RHI::RHISceneViewPtr::Make();
		meshes->CopySceneView(beforeRebuild);
		world.AdvanceFrame();
		meshComponent->GetData().MarkDirty();
		parent->GetTransformComponent().SetPosition(glm::vec3(20, 2, -8));
		transforms->Tick(0.0f);
		transforms->PostTick();
		meshes->Tick(0.0f);
		auto rebuilt = RHI::RHISceneViewPtr::Make();
		meshes->CopySceneView(rebuilt);
		Require(rebuilt->m_shadowCastersRevision > beforeRebuild->m_shadowCastersRevision,
			"a simultaneous topology rebuild and owner move must invalidate shadows even when local shadow meshes are equal");
		world.Clear();
		clip.DestroyObject(world.GetAllocator());
		material.DestroyObject(world.GetAllocator());
		model.DestroyObject(world.GetAllocator());
	}

	void TestMeshBatchesWithSharedOwnerAndRegistrationHoles()
	{
		PrefabTestWorld world;
		auto owner = world.Instantiate("MultipleMeshBatches");
		owner->SetMobilityType(EMobilityType::Dynamic);
		auto* meshes = world.GetECS<StaticMeshRendererECS>();
		meshes->BeginPlay();
		auto model = TObjectPtr<PublishedMeshTestModel>::Make(world.GetAllocator());
		model->m_ready = true;
		auto material = TObjectPtr<PublishedMeshTestMaterial>::Make(world.GetAllocator());
		TVector<size_t> slots;
		for (size_t i = 0u; i < 1031u; ++i)
		{
			const auto slot = meshes->RegisterComponent();
			slots.Add(slot);
			auto& data = meshes->GetComponentData(slot);
			data.SetOwner(owner);
			data.SetModel(model);
			data.GetMaterials().Add(material);
		}
		meshes->Tick(0.016f);
		auto before = meshes->GetRHIScene()->GetCurrentVersion();
		Require(before->m_dynamicHandles->Num() == slots.Num(), "initial publication must include every component batch");
		auto retainedView = RHI::RHISceneViewPtr::Make();
		retainedView->AddSceneVersion(static_cast<PublishedMeshTestSystem*>(meshes)->GetSpatialVersion());
		size_t removed = 0u;
		for (size_t i = 0u; i < slots.Num(); i += 17u)
		{
			meshes->UnregisterComponent(slots[i]);
			++removed;
		}
		world.AdvanceFrame();
		owner->GetTransformComponent().SetPosition(glm::vec3(4.0f, 0.0f, 0.0f));
		auto* transforms = world.GetECS<TransformECS>();
		transforms->Tick(0.016f);
		transforms->PostTick();
		meshes->Tick(0.016f);
		auto moved = meshes->GetRHIScene()->GetCurrentVersion();
		Require(moved->m_dynamicHandles->Num() == slots.Num() - removed,
			"registration holes and a partial last batch must preserve exactly the live instances");
		auto currentView = RHI::RHISceneViewPtr::Make();
		currentView->AddSceneVersion(static_cast<PublishedMeshTestSystem*>(meshes)->GetSpatialVersion());
		Math::Frustum narrowView;
		narrowView.ExtractFrustumPlanes(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f)),
			1.0f, 20.0f, 0.1f, 10.0f);
		Require(retainedView->TraceScene(narrowView).Num() == slots.Num() &&
			currentView->TraceScene(narrowView).IsEmpty(),
			"a delayed render view must retain its old spatial root after the world moves and removes instances");
		for (const auto handle : *moved->m_dynamicHandles)
		{
			const RHI::RHISceneInstanceRecord* oldRecord = nullptr;
			const RHI::RHISceneInstanceRecord* newRecord = nullptr;
			Require(before->Resolve(handle, oldRecord) && moved->Resolve(handle, newRecord) &&
				oldRecord->m_worldMatrix[3].x == 0.0f && newRecord->m_worldMatrix[3].x == 4.0f &&
				oldRecord->m_topology == newRecord->m_topology,
				"every batch must publish the new transform and retain immutable topology and old snapshots");
		}
		world.AdvanceFrame();
		meshes->Tick(0.016f);
		Require(meshes->GetRHIScene()->GetCurrentVersion() == moved,
			"reused batch scratch must not replay old changes when the scene is unchanged");
		meshes->EndPlay();
	}

	void TestClearingMeshModelAlsoClearsMaterials()
	{
		StaticMeshRendererData data;
		data.GetMaterials().Add(MaterialPtr());
		Require(data.GetMaterials().Num() == 1, "mesh renderer fixture should contain a material slot");

		data.SetModel(ModelPtr());
		Require(data.GetMaterials().IsEmpty(), "clearing a model should clear its stale material overrides");
	}

	void TestStaticMeshLodSelectionUsesScreenCoverage()
	{
		StaticMeshRendererData data;
		Require(data.ResolveLod(0.0f, 1u) == 0u &&
			data.ResolveLod(1.0f, 1u) == 0u,
			"mesh renderer must keep LOD0 when no generated LOD is available");
		data.SetLodSettings(0u, 2u, TVector<float>{ 0.05f, 0.25f });
		Require(data.ResolveLod(1.0f, 3u) == 0u &&
			data.ResolveLod(0.25f, 3u) == 0u &&
			data.ResolveLod(0.249f, 3u) == 1u &&
			data.ResolveLod(0.049f, 3u) == 2u,
			"mesh renderer LOD selection must follow descending screen-coverage thresholds");

		data.SetLodSettings(1u, 5u, TVector<float>{ 0.25f, 0.05f });
		Require(data.ResolveLod(1.0f, 3u) == 1u,
			"mesh renderer minimum LOD must clamp high-coverage selection");
		Require(data.ResolveLod(0.0f, 2u) == 1u,
			"mesh renderer maximum LOD must clamp to available model geometry");

		const glm::mat4 projection = glm::perspective(
			glm::radians(60.0f),
			1.0f,
			0.1f,
			1000.0f);
		const glm::mat4 view(1.0f);
		const float nearCoverage = RHI::CalculateScreenCoverage(
			Math::AABB(glm::vec3(0.0f, 0.0f, -5.0f), glm::vec3(1.0f)),
			view,
			projection);
		const float farCoverage = RHI::CalculateScreenCoverage(
			Math::AABB(glm::vec3(0.0f, 0.0f, -20.0f), glm::vec3(1.0f)),
			view,
			projection);
		const float offscreenCoverage = RHI::CalculateScreenCoverage(
			Math::AABB(glm::vec3(100.0f, 0.0f, -5.0f), glm::vec3(1.0f)),
			view,
			projection);
		const float behindCameraCoverage = RHI::CalculateScreenCoverage(
			Math::AABB(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(1.0f)),
			view,
			projection);
		const float cameraIntersectionCoverage = RHI::CalculateScreenCoverage(
			Math::AABB(glm::vec3(0.0f), glm::vec3(1.0f)),
			view,
			projection);
		Require(nearCoverage <= 1.0f &&
			nearCoverage > farCoverage && farCoverage > 0.0f,
			"projected AABB coverage must decrease as the same object recedes from the camera");
		Require(offscreenCoverage == 0.0f,
			"projected AABB coverage must exclude bounds outside the viewport");
		Require(behindCameraCoverage == 0.0f,
			"projected AABB coverage must exclude bounds behind the camera");
		Require(cameraIntersectionCoverage == 1.0f,
			"projected AABB coverage must conservatively select the highest LOD when bounds cross the camera plane");

	}

	void TestMeshLodSettingsNormalizeBeforeInitialization()
	{
		MeshRendererComponent authoring;
		authoring.SetMinLod(4u);
		authoring.SetMaxLod(1u);
		authoring.SetScreenCoverageThresholds(TVector<float>{
			std::numeric_limits<float>::quiet_NaN(), -1.0f, 0.4f,
			std::numeric_limits<float>::infinity(), 2.0f, 0.1f });
		Require(authoring.GetMinLod() == 4u && authoring.GetMaxLod() == 4u &&
			authoring.GetScreenCoverageThresholds() == TVector<float>{ 1.0f, 0.4f, 0.1f, 0.0f, 0.0f, 0.0f },
			"LOD authoring must normalize before an owner or ECS slot exists");

		StaticMeshRendererData data;
		data.SetLodSettings(0u, 2u, TVector<float>{ 0.05f, 0.25f });
		Require(!data.IsDirty(),
			"an equivalent ordering of the default thresholds must not dirty new renderer data");
		data.SetLodSettings(4u, 1u, authoring.GetScreenCoverageThresholds());
		Require(data.IsDirty() && data.ResolveLod(1.0f, 8u) == 4u && data.ResolveLod(0.0f, 8u) == 4u,
			"changed LOD settings must dirty renderer data and clamp the maximum to the minimum");
	}

	void TestMeshLodChangesPublishWithoutTransformEdits()
	{
		for (const auto mobility : { EMobilityType::Static, EMobilityType::Stationary })
		{
			PrefabTestWorld world;
			auto object = world.Instantiate("Unmoving LOD mesh");
			object->SetMobilityType(mobility);
			auto renderer = object->AddComponent<MeshRendererComponent>();
			auto model = TObjectPtr<PublishedMeshTestModel>::Make(world.GetAllocator());
			model->m_ready = true;
			auto material = TObjectPtr<PublishedMeshTestMaterial>::Make(world.GetAllocator());
			renderer->SetModel(model);
			renderer->GetMaterials().Add(material);
			const auto transformFrame = object->GetTransformComponent().GetFrameLastChange();
			const auto materialRevision = material->GetContentRevision();
			auto* meshes = world.GetECS<StaticMeshRendererECS>();
			meshes->BeginPlay();

			auto tick = [&]()
				{
					world.AdvanceFrame();
					meshes->Tick(0.016f);
					Require(object->GetTransformComponent().GetFrameLastChange() == transformFrame &&
						material->GetContentRevision() == materialRevision,
						"LOD publication must not rely on a transform or material edit");
					return meshes->GetRHIScene()->GetCurrentVersion();
				};
			const auto initial = tick();
			const auto& handles = mobility == EMobilityType::Static ?
				initial->m_staticHandles : initial->m_stationaryHandles;
			Require(handles && handles->Num() == 1u,
				"the ready mesh component must publish exactly one instance");
			const auto handle = (*handles)[0];
			auto policy = [&](const RHI::RHISceneVersionPtr& version) -> const RHI::RHILodPolicy&
				{
					const RHI::RHISceneInstanceRecord* record = nullptr;
					Require(version->Resolve(handle, record) && record && record->m_topology,
						"the retained mesh instance must resolve its topology");
					const auto* resource = dynamic_cast<const RHI::RHISceneProxyResource*>(record->m_topology.GetRawPtr());
					Require(resource != nullptr, "the mesh topology must carry a published scene proxy");
					return resource->m_proxy.m_lodPolicy;
				};
			const auto& initialPolicy = policy(initial);
			Require(initialPolicy.m_bEnabled && initialPolicy.m_minLod == 0u && initialPolicy.m_maxLod == 2u &&
				initialPolicy.m_screenCoverageThresholds == TVector<float>{ 0.25f, 0.05f },
				"the initial scene must contain the component's default LOD policy");

			renderer->SetMinLod(1u);
			const auto minimumChanged = tick();
			Require(minimumChanged != initial && policy(minimumChanged).m_minLod == 1u &&
				policy(minimumChanged).m_maxLod == 2u && initialPolicy.m_minLod == 0u,
				"changing minimum LOD must publish a new policy without mutating the retained scene");
			renderer->SetMinLod(1u);
			Require(!renderer->GetData().IsDirty() && tick() == minimumChanged,
				"repeating minimum LOD must not create another scene version");

			renderer->SetMaxLod(5u);
			const auto maximumChanged = tick();
			Require(maximumChanged != minimumChanged && policy(maximumChanged).m_maxLod == 5u &&
				policy(minimumChanged).m_maxLod == 2u,
				"changing maximum LOD must update the scene while the previous policy remains intact");
			renderer->SetMaxLod(0u);
			const auto maximumClamped = tick();
			Require(maximumClamped != maximumChanged && policy(maximumClamped).m_maxLod == 1u,
				"a maximum below minimum LOD must publish the clamped limit");
			renderer->SetMaxLod(1u);
			Require(!renderer->GetData().IsDirty() && tick() == maximumClamped,
				"a canonically equivalent maximum must not create another scene version");

			renderer->SetScreenCoverageThresholds(TVector<float>{
				-0.2f, 0.6f, 2.0f, std::numeric_limits<float>::quiet_NaN() });
			const auto thresholdsChanged = tick();
			const TVector<float> expectedThresholds{ 1.0f, 0.6f, 0.0f, 0.0f };
			Require(thresholdsChanged != maximumClamped &&
				policy(thresholdsChanged).m_screenCoverageThresholds == expectedThresholds &&
				policy(maximumClamped).m_screenCoverageThresholds == TVector<float>{ 0.25f, 0.05f },
				"threshold changes must publish normalized values without modifying an older snapshot");
			renderer->SetScreenCoverageThresholds(TVector<float>{
				0.6f, -4.0f, std::numeric_limits<float>::infinity(), 3.0f });
			Require(renderer->GetScreenCoverageThresholds() == expectedThresholds &&
				!renderer->GetData().IsDirty() && tick() == thresholdsChanged,
				"equivalent normalized authoring thresholds must leave the published scene unchanged");
			renderer->GetData().SetLodSettings(1u, 0u, TVector<float>{
				3.0f, std::numeric_limits<float>::quiet_NaN(), 0.6f, -1.0f });
			Require(!renderer->GetData().IsDirty() && tick() == thresholdsChanged,
				"direct ECS LOD updates must compare canonical limits and thresholds too");

			renderer->SetScreenCoverageThresholds({});
			const auto thresholdsCleared = tick();
			Require(thresholdsCleared != thresholdsChanged && policy(thresholdsCleared).m_screenCoverageThresholds.IsEmpty() &&
				policy(thresholdsChanged).m_screenCoverageThresholds == expectedThresholds,
				"clearing thresholds must publish an empty policy without discarding the retained thresholds");
			renderer->SetScreenCoverageThresholds({});
			Require(!renderer->GetData().IsDirty() && tick() == thresholdsCleared,
				"repeating an empty threshold list must not create another scene version");
			world.Clear();
		}
	}

	void TestLocalLightShadowContract()
	{
		Require(LightingECS::GetLocalShadowMapCount(ELightType::Point) == 6u,
			"a point light must render all six shadow faces");
		Require(LightingECS::GetLocalShadowMapCount(ELightType::Spot) == 1u,
			"a spot light must render one perspective shadow map");
		Require(LightingECS::GetLocalShadowMapCount(ELightType::Directional) == 0u,
			"local shadow allocation must not replace directional CSM");
		Require(LightingECS::GetLocalShadowResolution(ELightShadowQuality::VeryLow) == 256u &&
			LightingECS::GetLocalShadowResolution(ELightShadowQuality::Low) == 512u &&
			LightingECS::GetLocalShadowResolution(ELightShadowQuality::Medium) == 1024u &&
			LightingECS::GetLocalShadowResolution(ELightShadowQuality::High) == 2048u,
			"local shadow quality must map to the documented power-of-two resolutions");
		Require(LightingECS::LocalShadowMinResolution == 64u,
			"distant local lights must be allowed to fall back to 64x64 shadow tiles");
		Require(LightingECS::MaxShadowsInView >= 1804u &&
			LightingECS::MaxShadowMapSamplers < LightingECS::MaxShadowsInView,
			"shadow matrix capacity must cover 300 point lights without allocating one sampler per face");
		Require(LightingECS::DefaultShadowsMemoryBudgetMb == 768.0f,
			"the runtime shadow cache must expose the planned 768 MB default budget");

	}

	void TestCsmSnapshotInvalidatesWhenCascadeProjectionMoves()
	{
		Require(std::abs(LightingECS::ShadowCascadeLevels[LightingECS::NumCascades - 1] - 1.0f) <= 0.0001f,
			"the last shadow cascade must reach the configured shadow distance");
		Require(Settings::GraphicsQualityProfile{}.m_shadowDistance == 200.0f,
			"the default profile should preserve 200 meter CSM coverage");
		Require(LightingECS::ShadowCascadeBlendFraction > 0.0f,
			"adjacent CSM projections must overlap for seam-free transitions");

		const glm::mat4 cachedLightMatrix(1.0f);
		glm::mat4 movedLightMatrix = cachedLightMatrix;
		movedLightMatrix[3].x = 0.01f;

		CSMLightState cachedState{};
		cachedState.m_componentIndex = 7u;
		cachedState.m_shadowType = RHI::EShadowType::PCF;
		cachedState.m_lightMatrix = cachedLightMatrix;
		cachedState.m_sceneRevision = 23u;
		cachedState.m_casterSceneVersions =
			TSharedPtr<TVector<RHI::RHISceneVersionPtr>>::Make();
		cachedState.m_submissionToken = RHI::RHISubmissionCompletionTokenPtr::Make();
		cachedState.m_submissionToken->Complete(true);
		cachedState.m_payloadCompletionToken =
			RHI::RHISubmissionCompletionTokenPtr::Make();
		cachedState.m_payloadCompletionToken->Complete(true);
		Math::Frustum shadowFrustum;
		Require(cachedState.CanReuse(
			cachedState.m_componentIndex,
			cachedState.m_shadowType,
			cachedState.m_lightMatrix,
			0u,
			cachedState.m_sceneRevision,
			cachedState.m_casterSceneVersions,
			shadowFrustum),
			"an unchanged light and shadow-caster scene should reuse its CSM snapshot before tracing");
		Require(!cachedState.CanReuse(
			cachedState.m_componentIndex,
			cachedState.m_shadowType,
			movedLightMatrix,
			0u,
			cachedState.m_sceneRevision,
			cachedState.m_casterSceneVersions,
			shadowFrustum),
			"a changed cascade projection must invalidate the cached shadow map even for camera motion below the old threshold");

		CSMLightState pendingState = cachedState;
		CSMLightState cameraLodState = cachedState;
		cameraLodState.m_bContainsCameraLodCasters = true;
		cameraLodState.m_lodCameraRevision = 5u;
		Require(cameraLodState.CanReuse(cameraLodState.m_componentIndex, cameraLodState.m_shadowType,
			cameraLodState.m_lightMatrix, 5u, cameraLodState.m_sceneRevision,
			cameraLodState.m_casterSceneVersions, shadowFrustum) &&
			!cameraLodState.CanReuse(cameraLodState.m_componentIndex, cameraLodState.m_shadowType,
				cameraLodState.m_lightMatrix, 6u, cameraLodState.m_sceneRevision,
				cameraLodState.m_casterSceneVersions, shadowFrustum),
			"camera-dependent LOD must invalidate both local and directional shadow caches when its camera reference changes");
		cameraLodState.m_bContainsCameraLodCasters = false;
		Require(cameraLodState.CanReuse(cameraLodState.m_componentIndex, cameraLodState.m_shadowType,
			cameraLodState.m_lightMatrix, 6u, cameraLodState.m_sceneRevision,
			cameraLodState.m_casterSceneVersions, shadowFrustum),
			"camera changes must preserve cached shadows whose casters have no camera-dependent LOD");
		pendingState.m_submissionToken = RHI::RHISubmissionCompletionTokenPtr::Make();
		Require(!pendingState.CanReuse(
			pendingState.m_componentIndex,
			pendingState.m_shadowType,
			pendingState.m_lightMatrix,
			0u,
			pendingState.m_sceneRevision,
			pendingState.m_casterSceneVersions,
			shadowFrustum),
			"a pending shadow resource must not leak into another submission");
		Require(pendingState.CanReuse(
			pendingState.m_componentIndex,
			pendingState.m_shadowType,
			pendingState.m_lightMatrix,
			0u,
			pendingState.m_sceneRevision,
			pendingState.m_casterSceneVersions,
			shadowFrustum,
			pendingState.m_submissionToken),
			"multiple cameras in one submission may reuse the same scheduled shadow update");

		CSMLightState incompletePayloadState = cachedState;
		incompletePayloadState.m_payloadCompletionToken =
			RHI::RHISubmissionCompletionTokenPtr::Make();
		incompletePayloadState.m_payloadCompletionToken->Complete(false);
		Require(!incompletePayloadState.CanReuse(
			incompletePayloadState.m_componentIndex,
			incompletePayloadState.m_shadowType,
			incompletePayloadState.m_lightMatrix,
			0u,
			incompletePayloadState.m_sceneRevision,
			incompletePayloadState.m_casterSceneVersions,
			shadowFrustum),
			"a shadow pass with incomplete mesh or material dependencies must be rebuilt");

		CSMLightState pendingPayloadState = pendingState;
		pendingPayloadState.m_payloadCompletionToken =
			RHI::RHISubmissionCompletionTokenPtr::Make();
		Require(pendingPayloadState.CanReuse(
			pendingPayloadState.m_componentIndex,
			pendingPayloadState.m_shadowType,
			pendingPayloadState.m_lightMatrix,
			0u,
			pendingPayloadState.m_sceneRevision,
			pendingPayloadState.m_casterSceneVersions,
			shadowFrustum,
			pendingPayloadState.m_submissionToken),
			"multiple cameras in one submission may share one pending shadow payload build");

		auto nextSubmissionToken = RHI::RHISubmissionCompletionTokenPtr::Make();
		CSMLightState dynamicState = cachedState;
		dynamicState.m_bContainsDynamicCasters = true;
		Require(!dynamicState.CanReuse(
			dynamicState.m_componentIndex,
			dynamicState.m_shadowType,
			dynamicState.m_lightMatrix,
			0u,
			dynamicState.m_sceneRevision,
			dynamicState.m_casterSceneVersions,
			shadowFrustum,
			nextSubmissionToken),
			"a shadow map containing dynamic casters must be rebuilt for every submission");
		Require(dynamicState.CanReuse(
			dynamicState.m_componentIndex,
			dynamicState.m_shadowType,
			dynamicState.m_lightMatrix,
			0u,
			dynamicState.m_sceneRevision,
			dynamicState.m_casterSceneVersions,
			shadowFrustum,
			dynamicState.m_submissionToken),
			"multiple cameras in one submission may share a scheduled dynamic shadow update");

		CSMLightState animatedState = cachedState;
		animatedState.m_animationRevision = 41ull;
		animatedState.m_bContainsAnimatedCasters = true;
		Require(animatedState.CanReuse(
			animatedState.m_componentIndex,
			animatedState.m_shadowType,
			animatedState.m_lightMatrix,
			0u,
			animatedState.m_sceneRevision,
			animatedState.m_casterSceneVersions,
			shadowFrustum,
			nextSubmissionToken,
			41ull),
			"an unchanged bone-matrix generation may reuse its shadow map");
		Require(!animatedState.CanReuse(
			animatedState.m_componentIndex,
			animatedState.m_shadowType,
			animatedState.m_lightMatrix,
			0u,
			animatedState.m_sceneRevision,
			animatedState.m_casterSceneVersions,
			shadowFrustum,
			nextSubmissionToken,
			42ull),
			"a new bone-matrix generation must invalidate animated shadows even when RHIScene is unchanged");
	}

	void TestCsmShadowTargetFormatTracksShadowMode()
	{
		Require(
			LightingECS::GetCsmShadowMapFormat(RHI::EShadowType::PCF) ==
				LightingECS::ShadowMapFormat,
			"a PCF near cascade must use the compact single-channel shadow target");
		Require(
			LightingECS::GetCsmShadowMapFormat(RHI::EShadowType::EVSM) ==
				LightingECS::ShadowMapFormat_Evsm,
			"an EVSM near cascade must use the four-channel floating-point moments target");
		Require(
			LightingECS::GetCsmShadowMapFormat(RHI::EShadowType::PCF) !=
				LightingECS::GetCsmShadowMapFormat(RHI::EShadowType::EVSM),
			"switching the near cascade between PCF and EVSM must invalidate an incompatible flight target");

	}

	void TestAnimationGpuBoneLayoutContract()
	{
		const uint32_t invalidOffset = AnimatorComponentData::InvalidGpuOffset;
		AnimatorComponentData animatorData;
		StaticMeshRendererData meshData;
		Require(animatorData.m_gpuOffset == invalidOffset,
			"an animator without an animation should not reference the GPU bone buffer");
		Require(meshData.GetSkeletonOffset() == invalidOffset,
			"a mesh without an allocated skeleton should publish the invalid offset");

		uint32_t nextOffset = 0;
		uint32_t allocatedOffset = 0;
		Require(!AnimationECS::TryAllocateBoneRange(0, nextOffset, allocatedOffset),
			"a zero-bone animation should not allocate a GPU range");
		Require(nextOffset == 0 && allocatedOffset == invalidOffset,
			"a rejected zero-bone allocation should preserve the layout cursor and invalid offset");

		AnimationLayoutTestSystem system;
		const size_t replacedAnimator = system.RegisterComponent();
		const size_t survivingAnimator = system.RegisterComponent();
		Require(system.TryAllocateForTest(10, allocatedOffset),
			"the first animation range should fit");
		system.GetComponentData(replacedAnimator).m_gpuOffset = allocatedOffset;
		system.GetComponentData(replacedAnimator).SetBonesCount(10);
		Require(system.TryAllocateForTest(10, allocatedOffset),
			"the neighboring animation range should fit");
		system.GetComponentData(survivingAnimator).m_gpuOffset = allocatedOffset;
		system.GetComponentData(survivingAnimator).SetBonesCount(10);

		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		auto sameSkeleton = AnimationPtr::Make(allocator, FileId{});
		sameSkeleton->m_numBones = 10;
		system.SetAnimation(replacedAnimator, sameSkeleton);
		Require(system.GetComponentData(replacedAnimator).m_gpuOffset == 0 &&
			system.GetComponentData(survivingAnimator).m_gpuOffset == 10 &&
			system.GetNextBoneOffsetForTest() == 20,
			"switching animation data without changing the skeleton size must preserve every GPU bone range");

		system.SetAnimation(replacedAnimator, TObjectPtr<Animation>());
		Require(system.GetComponentData(replacedAnimator).m_gpuOffset == 0 &&
			system.GetComponentData(survivingAnimator).m_gpuOffset == 10 &&
			system.GetNextBoneOffsetForTest() == 20,
			"a pending layout change must preserve the offsets used by the last published palette");
		system.Tick(0.0f);
		Require(system.GetComponentData(replacedAnimator).m_gpuOffset == invalidOffset &&
			system.GetComponentData(survivingAnimator).m_gpuOffset == invalidOffset,
			"Tick must invalidate the old layout before allocating active owners again");
		Require(system.GetNextBoneOffsetForTest() == 0,
			"the ownerless allocator fixture must leave the rebuilt layout empty");

		uint32_t replacementOffset = invalidOffset;
		uint32_t survivorOffset = invalidOffset;
		Require(system.TryAllocateForTest(100, replacementOffset),
			"a replacement animation with more bones should receive a new range");
		Require(system.TryAllocateForTest(10, survivorOffset),
			"the neighboring animation should be reallocated after the replacement");
		Require(replacementOffset == 0 && survivorOffset == 100,
			"replacement relayout should keep neighboring bone ranges disjoint");

		nextOffset = 0;
		Require(AnimationECS::TryAllocateBoneRange(AnimationECS::BonesMaxNum, nextOffset, allocatedOffset),
			"an exact-capacity animation range should fit");
		Require(nextOffset == AnimationECS::BonesMaxNum,
			"an exact-capacity allocation should advance the cursor to the buffer boundary");
		Require(!AnimationECS::TryAllocateBoneRange(1, nextOffset, allocatedOffset),
			"an allocation beyond the bone buffer capacity should be rejected");
		Require(nextOffset == AnimationECS::BonesMaxNum && allocatedOffset == invalidOffset,
			"capacity overflow should not clamp to a writable-looking offset");
		nextOffset = AnimationECS::BonesMaxNum + 1;
		Require(!AnimationECS::TryAllocateBoneRange(1, nextOffset, allocatedOffset) &&
			nextOffset == AnimationECS::BonesMaxNum + 1 && allocatedOffset == invalidOffset,
			"an out-of-range cursor should be rejected without unsigned-capacity underflow");

		sameSkeleton.DestroyObject(allocator);

	}

	void TestAnimationRelayoutMarksEveryOwnedMeshDirty()
	{
		AnimationMeshTestWorld world;
		auto gameObject = world.Instantiate("AnimatedMeshes");
		auto firstMesh = gameObject->AddComponent<MeshRendererComponent>();
		auto secondMesh = gameObject->AddComponent<MeshRendererComponent>();
		gameObject->AddComponent<AnimatorComponent>();

		Require(!firstMesh->GetData().IsDirty() && !secondMesh->GetData().IsDirty(),
			"mesh fixtures should start with clean ECS data");

		world.GetECS<AnimationECS>()->InvalidateGpuLayout();
		Require(!firstMesh->GetData().IsDirty() && !secondMesh->GetData().IsDirty(),
			"requesting a relayout must not visit owned meshes before Tick");
		world.GetECS<AnimationECS>()->Tick(0.0f);

		Require(firstMesh->GetData().IsDirty(),
			"animation relayout should invalidate the first owned mesh renderer");
		Require(secondMesh->GetData().IsDirty(),
			"animation relayout should invalidate every additional owned mesh renderer");

		world.Clear();
	}

	void TestAnimationRemovalsPublishOneCompactedPalette()
	{
		for (uint32_t count : { 32u, 64u })
		{
			AnimationMeshTestWorld world;
			auto* animations = world.GetECS<AnimationECS>();
			auto* meshes = world.GetECS<StaticMeshRendererECS>();
			auto model = TObjectPtr<PublishedMeshTestModel>::Make(world.GetAllocator());
			model->m_ready = true;
			auto material = TObjectPtr<PublishedMeshTestMaterial>::Make(world.GetAllocator());
			TVector<GameObjectPtr> owners;
			TVector<TObjectPtr<AnimatorComponent>> animators;
			TVector<MeshRendererComponentPtr> renderers;
			TVector<AnimationPtr> clips;
			for (uint32_t index = 0; index < count; ++index)
			{
				auto owner = world.Instantiate("Batched animation removal");
				owner->SetMobilityType(EMobilityType::Static);
				owners.Add(owner);
				for (uint32_t mesh = 0; mesh < 2; ++mesh)
				{
					auto renderer = owner->AddComponent<MeshRendererComponent>();
					renderer->SetModel(model);
					renderer->GetMaterials().Add(material);
					renderers.Add(renderer);
				}
				auto clip = MakeLayoutAnimation(world.GetAllocator(), 1, static_cast<float>(index + 1));
				clips.Add(clip);
				auto animator = owner->AddComponent<AnimatorComponent>();
				animator->SetAnimation(clip);
				animators.Add(animator);
			}
			meshes->BeginPlay();
			animations->Tick(0.0f);
			meshes->Tick(0.0f);
			auto retained = RHI::RHISceneViewPtr::Make();
			animations->FillAnimationData(retained);
			const auto oldScene = meshes->GetRHIScene()->GetCurrentVersion();
			Require(retained->m_cpuBoneMatrices && retained->m_cpuBoneMatrices->Num() == count &&
				oldScene->m_staticHandles->Num() == count * 2,
				"the fixture must publish one bone per animator and both meshes per owner");

			for (uint32_t index = 0; index < count; index += 2)
			{
				Require(owners[index]->RemoveComponent(animators[index]) && !animators[index],
					"ordinary component removal must destroy the animator while keeping its owner and meshes");
				Require(renderers[index * 2]->GetData().IsDirty() && renderers[index * 2 + 1]->GetData().IsDirty(),
					"removing an animator must immediately invalidate every mesh still owned by that object");
				Require(!renderers[(index + 1) * 2]->GetData().IsDirty() &&
					!renderers[(index + 1) * 2 + 1]->GetData().IsDirty() &&
					animators[index + 1]->GetSkeletonOffset() == index + 1,
					"each removal must leave unrelated mesh policy and surviving published offsets untouched");
				auto pending = RHI::RHISceneViewPtr::Make();
				animations->FillAnimationData(pending);
				Require(pending->m_cpuBoneMatrices == retained->m_cpuBoneMatrices &&
					pending->m_animationRevision == retained->m_animationRevision &&
					meshes->GetRHIScene()->GetCurrentVersion() == oldScene,
					"FillAnimationData must not rebuild or publish an intermediate layout during a removal batch");
			}

			world.AdvanceFrame();
			animations->Tick(0.0f);
			meshes->Tick(0.0f);
			auto compacted = RHI::RHISceneViewPtr::Make();
			animations->FillAnimationData(compacted);
			Require(compacted->m_animationRevision == retained->m_animationRevision + 1 &&
				compacted->m_cpuBoneMatrices != retained->m_cpuBoneMatrices &&
				compacted->m_cpuBoneMatrices->Num() == count / 2,
				"one Tick must publish exactly one compacted palette for the whole removal batch");
			const auto newScene = meshes->GetRHIScene()->GetCurrentVersion();
			for (const auto handle : *oldScene->m_staticHandles)
			{
				const RHI::RHISceneInstanceRecord* oldRecord = nullptr;
				const RHI::RHISceneInstanceRecord* newRecord = nullptr;
				Require(oldScene->Resolve(handle, oldRecord) && newScene->Resolve(handle, newRecord),
					"removing only the animator must preserve both mesh handles");
				const uint32_t previousOffset = oldRecord->m_skeletonOffset;
				Require(previousOffset < count &&
					(*retained->m_cpuBoneMatrices)[previousOffset][3].x == static_cast<float>(previousOffset + 1),
					"the delayed scene and its retained palette must still describe the original pose");
				if (previousOffset % 2 == 0)
				{
					Require(newRecord->m_skeletonOffset == AnimatorComponentData::InvalidGpuOffset,
						"every mesh of an owner without its animator must stop referencing the palette");
				}
				else
				{
					Require(newRecord->m_skeletonOffset == previousOffset / 2 &&
						(*compacted->m_cpuBoneMatrices)[newRecord->m_skeletonOffset][3].x ==
							static_cast<float>(previousOffset + 1),
						"surviving meshes must reference their own pose in the compacted palette");
				}
			}
			auto sameFrame = RHI::RHISceneViewPtr::Make();
			animations->FillAnimationData(sameFrame);
			Require(sameFrame->m_cpuBoneMatrices == compacted->m_cpuBoneMatrices &&
				sameFrame->m_animationRevision == compacted->m_animationRevision,
				"additional consumers must reuse the same committed publication");
			world.Clear();
			Require(retained->m_cpuBoneMatrices->Num() == count && compacted->m_cpuBoneMatrices->Num() == count / 2,
				"both retained palettes must outlive teardown");
			for (auto& clip : clips)
			{
				clip.DestroyObject(world.GetAllocator());
			}
			material.DestroyObject(world.GetAllocator());
			model.DestroyObject(world.GetAllocator());
		}
	}

	void TestPausedAnimationReusesPublishedPose()
	{
		AnimationMeshTestWorld world;
		auto* animations = world.GetECS<AnimationECS>();
		auto clip = MakeLayoutAnimation(world.GetAllocator(), 1, 1.0f);
		clip->m_numFrames = 2;
		clip->m_fps = 1.0f;
		clip->m_duration = 1.0f;
		clip->m_frames.Add(clip->m_frames[0]);
		clip->m_frames[1].m_position.x = 3.0f;
		auto animator = world.Instantiate("Paused animated mesh")->AddComponent<AnimatorComponent>();
		animator->SetAnimation(clip);
		animator->Stop();
		animations->Tick(0.0f);
		auto paused = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(paused);
		for (uint32_t tick = 0; tick < 200; ++tick)
		{
			animations->Tick(1.0f / 60.0f);
			auto current = RHI::RHISceneViewPtr::Make();
			animations->FillAnimationData(current);
			Require(current->m_cpuBoneMatrices == paused->m_cpuBoneMatrices &&
				current->m_animationRevision == paused->m_animationRevision,
				"200 paused ticks must reuse the palette and animated-shadow revision");
		}
		animator->Play();
		animations->Tick(0.5f);
		auto resumed = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(resumed);
		Require(resumed->m_animationRevision == paused->m_animationRevision + 1 &&
			(*resumed->m_cpuBoneMatrices)[0][3].x == 2.0f &&
			(*paused->m_cpuBoneMatrices)[0][3].x == 1.0f,
			"resuming must publish the advanced pose once and preserve retained paused matrices");
		animations->Tick(0.0f);
		auto unchanged = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(unchanged);
		Require(unchanged->m_cpuBoneMatrices == resumed->m_cpuBoneMatrices &&
			unchanged->m_animationRevision == resumed->m_animationRevision,
			"a playing animator with zero elapsed time must also reuse its unchanged pose");
		world.Clear();
		clip.DestroyObject(world.GetAllocator());
	}

	void TestAnimationLayoutRefreshesOnceAfterSkeletonChanges()
	{
		AnimationMeshTestWorld world;
		auto* animations = world.GetECS<AnimationECS>();
		auto* meshes = world.GetECS<StaticMeshRendererECS>();
		auto model = TObjectPtr<PublishedMeshTestModel>::Make(world.GetAllocator());
		model->m_ready = true;
		auto material = TObjectPtr<PublishedMeshTestMaterial>::Make(world.GetAllocator());
		auto firstOwner = world.Instantiate("Changing skeleton");
		auto secondOwner = world.Instantiate("Neighboring skeleton");
		auto firstMesh = firstOwner->AddComponent<MeshRendererComponent>();
		auto secondMesh = secondOwner->AddComponent<MeshRendererComponent>();
		for (auto mesh : { firstMesh, secondMesh })
		{
			mesh->SetModel(model);
			mesh->GetMaterials().Add(material);
		}
		auto firstClip = MakeLayoutAnimation(world.GetAllocator(), 2, 2.0f);
		auto secondClip = MakeLayoutAnimation(world.GetAllocator(), 1, 8.0f);
		auto replacementClip = MakeLayoutAnimation(world.GetAllocator(), 2, 5.0f);
		auto first = firstOwner->AddComponent<AnimatorComponent>();
		auto second = secondOwner->AddComponent<AnimatorComponent>();
		first->SetAnimation(firstClip);
		second->SetAnimation(secondClip);
		meshes->BeginPlay();
		animations->Tick(0.0f);
		meshes->Tick(0.0f);
		auto initial = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(initial);
		Require(first->GetSkeletonOffset() == 0 && second->GetSkeletonOffset() == 2 &&
			initial->m_cpuBoneMatrices->Num() == 3,
			"neighboring skeletons must start with disjoint compact ranges");

		first->SetAnimation(replacementClip);
		Require(first->GetSkeletonOffset() == 0 && second->GetSkeletonOffset() == 2 &&
			firstMesh->GetData().IsDirty() && !secondMesh->GetData().IsDirty(),
			"a same-sized clip switch must update only its owner's mesh policy");
		world.AdvanceFrame();
		animations->Tick(0.0f);
		Require(!secondMesh->GetData().IsDirty(),
			"same-sized clip replacement must not trigger a deferred global relayout");
		meshes->Tick(0.0f);
		auto replaced = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(replaced);
		Require(second->GetSkeletonOffset() == 2 && replaced->m_cpuBoneMatrices->Num() == 3 &&
			(*replaced->m_cpuBoneMatrices)[0][3].x == 5.0f &&
			(*initial->m_cpuBoneMatrices)[0][3].x == 2.0f,
			"same-sized replacement must change the pose without moving neighboring offsets or retained matrices");

		SetLayoutAnimationPose(replacementClip, 2, 6.0f);
		world.AdvanceFrame();
		animations->Tick(0.0f);
		Require(!secondMesh->GetData().IsDirty() && second->GetSkeletonOffset() == 2,
			"same-sized asset revision refresh must not invalidate another animator's mesh policy");
		meshes->Tick(0.0f);
		animations->FillAnimationData(replaced);
		Require((*replaced->m_cpuBoneMatrices)[0][3].x == 6.0f,
			"a same-sized hot reload must still publish its changed pose");

		SetLayoutAnimationPose(replacementClip, 3, 9.0f);
		SetLayoutAnimationPose(secondClip, 2, 8.0f);
		Require(first->GetSkeletonOffset() == 0 && second->GetSkeletonOffset() == 2,
			"asset edits must not change offsets before their owner Tick");
		world.AdvanceFrame();
		animations->Tick(0.0f);
		meshes->Tick(0.0f);
		auto resized = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(resized);
		Require(first->GetSkeletonOffset() == 0 && second->GetSkeletonOffset() == 3 &&
			resized->m_cpuBoneMatrices->Num() == 5 &&
			resized->m_animationRevision == replaced->m_animationRevision + 1 &&
			(*resized->m_cpuBoneMatrices)[2][3].x == 9.0f && (*resized->m_cpuBoneMatrices)[3][3].x == 8.0f,
			"all changed assets must refresh before a single compact layout and palette are published");

		first->SetAnimation({});
		auto pending = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(pending);
		Require(first->GetSkeletonOffset() == 0 && second->GetSkeletonOffset() == 3 &&
			pending->m_cpuBoneMatrices == resized->m_cpuBoneMatrices,
			"clearing an animation must retain the last complete layout until Tick");
		animations->Tick(0.0f);
		Require(first->GetSkeletonOffset() == AnimatorComponentData::InvalidGpuOffset &&
			second->GetSkeletonOffset() == 0,
			"a zero-bone skeleton must release its range without leaving holes");
		second->GetData().m_gpuOffset = AnimationECS::BonesMaxNum;
		animations->Tick(0.0f);
		Require(second->GetSkeletonOffset() == 0,
			"range validation must request and consume the relayout before sampling in the same Tick");
		world.Clear();
		firstClip.DestroyObject(world.GetAllocator());
		secondClip.DestroyObject(world.GetAllocator());
		replacementClip.DestroyObject(world.GetAllocator());
		material.DestroyObject(world.GetAllocator());
		model.DestroyObject(world.GetAllocator());
	}

	void TestAnimationLayoutRecoversCapacityAndLastRemoval()
	{
		AnimationMeshTestWorld world;
		auto* animations = world.GetECS<AnimationECS>();
		auto fullClip = MakeLayoutAnimation(world.GetAllocator(), AnimationECS::BonesMaxNum, 2.0f);
		auto smallClip = MakeLayoutAnimation(world.GetAllocator(), 1, 9.0f);
		auto firstOwner = world.Instantiate("Full palette");
		auto secondOwner = world.Instantiate("Waiting for space");
		auto first = firstOwner->AddComponent<AnimatorComponent>();
		auto second = secondOwner->AddComponent<AnimatorComponent>();
		first->SetAnimation(fullClip);
		second->SetAnimation(smallClip);
		animations->Tick(0.0f);
		auto full = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(full);
		Require(first->GetSkeletonOffset() == 0 && second->GetSkeletonOffset() == AnimatorComponentData::InvalidGpuOffset &&
			full->m_cpuBoneMatrices->Num() == AnimationECS::BonesMaxNum,
			"capacity overflow must leave the waiting animator unallocated without damaging the full palette");
		Require(firstOwner->RemoveComponent(first), "the full-range animator must be removable");
		animations->Tick(0.0f);
		auto compacted = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(compacted);
		Require(second->GetSkeletonOffset() == 0 && compacted->m_cpuBoneMatrices->Num() == 1 &&
			compacted->m_animationRevision == full->m_animationRevision + 1 &&
			(*compacted->m_cpuBoneMatrices)[0][3].x == 9.0f &&
			(*full->m_cpuBoneMatrices)[AnimationECS::BonesMaxNum - 1][3].x == 2.0f,
			"one relayout must reuse released capacity while preserving the retained full palette");
		Require(secondOwner->RemoveComponent(second), "the last animator must be removable");
		auto pending = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(pending);
		Require(pending->m_cpuBoneMatrices == compacted->m_cpuBoneMatrices,
			"last removal must not mutate the palette before Tick");
		animations->Tick(0.0f);
		auto empty = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(empty);
		Require(empty->m_cpuBoneMatrices && empty->m_cpuBoneMatrices->IsEmpty() &&
			empty->m_animationRevision == compacted->m_animationRevision + 1,
			"last removal must publish one empty palette");
		animations->Tick(0.0f);
		auto stillEmpty = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(stillEmpty);
		Require(stillEmpty->m_cpuBoneMatrices == empty->m_cpuBoneMatrices &&
			stillEmpty->m_animationRevision == empty->m_animationRevision,
			"a consumed empty relayout must not remain dirty on later ticks");
		animations->InvalidateGpuLayout();
		world.Clear();
		auto cleared = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(cleared);
		Require(!cleared->m_cpuBoneMatrices && cleared->m_animationRevision == 0,
			"Clear must discard pending relayout state and the current publication");
		auto replacement = world.Instantiate("Fresh animator after Clear")->AddComponent<AnimatorComponent>();
		replacement->SetAnimation(smallClip);
		animations->Tick(0.0f);
		Require(replacement->GetSkeletonOffset() == 0,
			"authoring reuse after Clear must allocate from a fresh layout");
		world.Clear();
		fullClip.DestroyObject(world.GetAllocator());
		smallClip.DestroyObject(world.GetAllocator());
	}

	void TestSparseLightSlotInvalidationAndReuse()
	{
		Require(LightingECS::GetGpuLightSlotsCount(LightingECS::LightsMaxNum) == LightingECS::LightsMaxNum,
			"the exact GPU light capacity should remain addressable");
		Require(LightingECS::GetGpuLightSlotsCount(static_cast<size_t>(LightingECS::LightsMaxNum) + 1) == LightingECS::LightsMaxNum,
			"a light slot beyond GPU capacity should be clamped before buffer access");

		LightingECS system;
		const size_t released = system.RegisterComponent();
		const size_t survivor = system.RegisterComponent();
		system.GetComponentData(released).m_type = ELightType::Directional;
		system.GetComponentData(survivor).m_type = ELightType::Spot;

		system.UnregisterComponent(released);
		Require(!system.IsComponentRegistered(released), "released light slot should be inactive");
		Require(system.IsComponentRegistered(survivor), "sparse light removal should preserve later slots");
		Require(system.GetComponentData(survivor).m_type == ELightType::Spot,
			"sparse light removal should preserve surviving light data");

		const LightingECS::LightShaderData invalidShaderData{};
		Require(invalidShaderData.m_type == LightingECS::LightShaderData::InvalidType,
			"released GPU light payload should use an explicit invalid marker");
		Require(offsetof(LightingECS::LightShaderData, m_shadowBias) == 12u,
			"the profile shadow bias must occupy the existing std430 light padding");
		Require(offsetof(LightingECS::LightShaderData, m_shadowDistance) == 28u,
			"shadow distance must occupy the world-position padding without shifting GPU light fields");
		Require(
			offsetof(LightingECS::LightShaderData, m_cutOff) == 64u &&
			offsetof(LightingECS::LightShaderData, m_bounds) == 80u &&
			sizeof(LightingECS::LightShaderData) == 96u,
			"the CPU light payload must match the shader's std430 layout");
		Require(invalidShaderData.m_shadowBias == 0.0f,
			"an invalid GPU light payload should not introduce receiver bias");

		const size_t reused = system.RegisterComponent();
		Require(reused == released, "released sparse light slot should be reused");
		Require(system.GetComponentData(reused).m_type == ELightType::Point,
			"reused light slot should restore default component data");

	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "OctreeRelocationPreservesElementCount", TestOctreeRelocationPreservesElementCount },
		{ "ClearingMeshModelAlsoClearsMaterials", TestClearingMeshModelAlsoClearsMaterials },
		{ "MeshBatchesWithSharedOwnerAndRegistrationHoles", TestMeshBatchesWithSharedOwnerAndRegistrationHoles },
		{ "FrameZeroMeshPublicationStaysStable", TestFrameZeroMeshPublicationStaysStable },
		{ "LocalMeshTopologySurvivesSingularOwnerTransforms", TestLocalMeshTopologySurvivesSingularOwnerTransforms },
		{ "StaticMeshLodSelectionUsesScreenCoverage", TestStaticMeshLodSelectionUsesScreenCoverage },
		{ "MeshLodSettingsNormalizeBeforeInitialization", TestMeshLodSettingsNormalizeBeforeInitialization },
		{ "MeshLodChangesPublishWithoutTransformEdits", TestMeshLodChangesPublishWithoutTransformEdits },
		{ "LocalLightShadowContract", TestLocalLightShadowContract },
		{ "CsmSnapshotInvalidatesWhenCascadeProjectionMoves", TestCsmSnapshotInvalidatesWhenCascadeProjectionMoves },
		{ "CsmShadowTargetFormatTracksShadowMode", TestCsmShadowTargetFormatTracksShadowMode },
		{ "AnimationGpuBoneLayoutContract", TestAnimationGpuBoneLayoutContract },
		{ "AnimationRelayoutMarksEveryOwnedMeshDirty", TestAnimationRelayoutMarksEveryOwnedMeshDirty },
		{ "AnimationRemovalsPublishOneCompactedPalette", TestAnimationRemovalsPublishOneCompactedPalette },
		{ "PausedAnimationReusesPublishedPose", TestPausedAnimationReusesPublishedPose },
		{ "AnimationLayoutRefreshesOnceAfterSkeletonChanges", TestAnimationLayoutRefreshesOnceAfterSkeletonChanges },
		{ "AnimationLayoutRecoversCapacityAndLastRemoval", TestAnimationLayoutRecoversCapacityAndLastRemoval },
		{ "SparseLightSlotInvalidationAndReuse", TestSparseLightSlotInvalidationAndReuse },
		{ "SceneRemovalsPublishOnce", TestSceneRemovalsPublishOnce },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& e)
		{
			std::cerr << "[FAIL] " << test.first << ": " << e.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
