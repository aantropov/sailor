#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include "Containers/Octree.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Components/AnimatorComponent.h"
#include "Components/Component.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/AnimationECS.h"
#include "ECS/ECS.h"
#include "ECS/LightingECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Submodules/Editor.h"

using namespace Sailor;

namespace Sailor
{
	class PrefabRollbackTestComponent final : public Component
	{
		SAILOR_REFLECTABLE(PrefabRollbackTestComponent)

	public:

		PrefabRollbackTestComponent() = default;

		float m_value = 0.0f;
		ComponentPtr m_dependency;
	};

	class PrefabMixedDependencyTestComponent final : public Component
	{
		SAILOR_REFLECTABLE(PrefabMixedDependencyTestComponent)

	public:

		PrefabMixedDependencyTestComponent() = default;

		ComponentPtr m_sourceDependency;
		ComponentPtr m_liveDependency;
	};
}

REFL_AUTO(
	type(Sailor::PrefabRollbackTestComponent, bases<Sailor::Component>),
	field(m_value),
	field(m_dependency)
)

REFL_AUTO(
	type(Sailor::PrefabMixedDependencyTestComponent, bases<Sailor::Component>),
	field(m_sourceDependency),
	field(m_liveDependency)
)

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	bool AreMatricesNear(const glm::mat4& lhs, const glm::mat4& rhs, float tolerance = 0.0001f)
	{
		for (glm::length_t column = 0; column < lhs.length(); ++column)
		{
			for (glm::length_t row = 0; row < lhs[column].length(); ++row)
			{
				const float scale = std::max({ 1.0f, std::abs(lhs[column][row]), std::abs(rhs[column][row]) });
				if (std::abs(lhs[column][row] - rhs[column][row]) > tolerance * scale)
				{
					return false;
				}
			}
		}

		return true;
	}

	glm::mat4 CalculateCurrentWorldMatrix(GameObjectPtr gameObject)
	{
		glm::mat4 worldMatrix = glm::identity<glm::mat4>();
		for (auto current = gameObject; current.IsValid(); current = current->GetParent())
		{
			worldMatrix = current->GetTransformComponent().GetTransform().Matrix() * worldMatrix;
		}

		return worldMatrix;
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test source should be readable: " + path.generic_string());
		return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	class LifecycleData final : public ECS::TComponent
	{
	public:

		~LifecycleData() override
		{
			++s_numDestructions;
		}

		bool IsActiveForTest() const { return m_bIsActive; }
		bool IsDirtyForTest() const { return m_bIsDirty; }

		static inline uint32_t s_numDestructions = 0;
		uint32_t m_payload = 0;
	};

	class LifecycleSystem final : public ECS::TSystem<LifecycleSystem, LifecycleData>
	{
	public:

		Tasks::ITaskPtr Tick(float deltaTime) override { return nullptr; }

		uint32_t m_numCleanupCalls = 0;
		uint32_t m_lastCleanupPayload = 0;

	protected:

		void OnComponentUnregistered(size_t index, LifecycleData& component) override
		{
			++m_numCleanupCalls;
			m_lastCleanupPayload = component.m_payload;

			if (m_bReenterCleanup)
			{
				UnregisterComponent(index);
			}
		}

	public:

		bool m_bReenterCleanup = false;
	};

	class AnimationLayoutTestSystem final : public AnimationECS
	{
	public:

		bool TryAllocateForTest(uint32_t numBones, uint32_t& outGpuOffset)
		{
			return TryAllocateBoneRange(numBones, m_nextBoneOffset, outGpuOffset);
		}

		uint32_t GetNextBoneOffsetForTest() const { return m_nextBoneOffset; }
	};

	class PrefabTestWorld final : public World
	{
	public:

		PrefabTestWorld() : World("PrefabRollbackTests", 0, CreateEcs()) {}
		size_t GetPendingDependencyCount() const { return GetNumPendingDependencyResolutions(); }
		bool RemovePrefabMetadataForTest(
			const InstanceId& rootInstanceId)
		{
			if (!m_prefabInstances.ContainsKey(rootInstanceId))
			{
				return false;
			}

			m_prefabInstances.Remove(rootInstanceId);
			return true;
		}

	private:

		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			return systems;
		}
	};

	class PrefabDocumentTestAsset final : public Prefab
	{
	public:

		PrefabDocumentTestAsset(const FileId& fileId) :
			Prefab(fileId) {}

		static PrefabPtr Capture(
			PrefabTestWorld& world,
			GameObjectPtr root,
			const FileId& fileId = FileId::Invalid)
		{
			auto result =
				TObjectPtr<PrefabDocumentTestAsset>::Make(
					world.GetAllocator(),
					fileId);
			SerializeGameObject(
				root,
				static_cast<uint32_t>(-1),
				result->m_components,
				result->m_gameObjects,
				nullptr);

			std::string diagnostic;
			result->m_bIsReady.store(
				result->ValidateForInstantiation(
					diagnostic),
				std::memory_order_release);
			return result;
		}

		static bool MarkExpandedLinkedRecord(
			PrefabPtr prefab,
			const TMap<InstanceId, InstanceId>& mappings,
			std::string& outDiagnostic)
		{
			auto result =
				prefab.DynamicCast<
					PrefabDocumentTestAsset>();
			if (!result)
			{
				outDiagnostic =
					"the expanded fixture has an unexpected type";
				return false;
			}

			result->m_linkedInstanceIds = mappings;
			result->m_bLinkedInstanceRecord = true;
			result->m_bExpandedLinkedInstanceRecord =
				true;
			result->m_detachedSupplementalInstanceIds.
				Clear();
			TSet<InstanceId> mappedLiveIds;
			for (const auto& mapping : mappings)
			{
				mappedLiveIds.Insert(
					*mapping.m_second);
			}
			for (const auto& gameObject :
				result->m_gameObjects)
			{
				if (!mappedLiveIds.Contains(
						gameObject.m_instanceId))
				{
					result->
						m_detachedSupplementalInstanceIds.
							Insert(
								gameObject.m_instanceId);
				}
			}

			return result->ValidateForInstantiation(
				outDiagnostic);
		}
	};

	class WorldPrefabDocumentFixture final : public WorldPrefab
	{
	public:

		WorldPrefabDocumentFixture() : WorldPrefab(FileId::Invalid) {}

		void AddPrefab(const PrefabPtr& prefab)
		{
			m_gameObjects.Add(prefab);
			m_bIsReady.store(true, std::memory_order_release);
		}

		static bool Reconcile(
			const PrefabPtr& expandedPrefab,
			const PrefabPtr& sourcePrefab,
			const TMap<InstanceId, InstanceId>& savedSourceToInstanceIds,
			TSet<InstanceId>& reservedInstanceIds,
			TMap<InstanceId, InstanceId>& outSourceToInstanceIds,
			std::string& outDiagnostic)
		{
			return ReconcileLinkedInstanceIds(
				expandedPrefab,
				sourcePrefab,
				savedSourceToInstanceIds,
				reservedInstanceIds,
				outSourceToInstanceIds,
				outDiagnostic);
		}

		static bool BuildUpdatedOverrides(
			const PrefabPtr& expandedPrefab,
			const PrefabPtr& sourcePrefab,
			const PrefabPtr& effectiveBaseline,
			const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
			TMap<InstanceId, YAML::Node>& outGameObjectOverrides,
			TMap<InstanceId, ReflectedData>& outComponentOverrides,
			std::string& outDiagnostic)
		{
			return BuildUpdatedLinkedOverrides(
				expandedPrefab,
				sourcePrefab,
				effectiveBaseline,
				sourceToInstanceIds,
				outGameObjectOverrides,
				outComponentOverrides,
				outDiagnostic);
		}

		static bool CommitLinkedUpdate(
			WorldPtr world,
			const InstanceId& rootInstanceId,
			const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
			const PrefabPtr& effectiveBaseline,
			std::string& outDiagnostic)
		{
			TVector<PendingPrefabLinkUpdate> pendingUpdates;
			PendingPrefabLinkUpdate pendingUpdate;
			pendingUpdate.m_rootInstanceId = rootInstanceId;
			pendingUpdate.m_sourceToInstanceIds =
				sourceToInstanceIds;
			pendingUpdate.m_effectiveBaseline =
				effectiveBaseline;
			pendingUpdates.Add(std::move(pendingUpdate));
			return CommitLinkedInstanceUpdates(
				world,
				pendingUpdates,
				outDiagnostic);
		}

		void MarkSerializationFailure(std::string diagnostic)
		{
			m_loadDiagnostic = std::move(diagnostic);
			m_bIsReady.store(false, std::memory_order_release);
		}
	};

	class AnimationMeshTestWorld final : public World
	{
	public:

		AnimationMeshTestWorld() : World("AnimationMeshTests", 0, CreateEcs()) {}

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

	void TestComponentSlotsAreResetAndFreedOnce()
	{
		LifecycleData::s_numDestructions = 0;
		LifecycleSystem system;

		const size_t first = system.RegisterComponent();
		auto& data = system.GetComponentData(first);
		Require(system.IsComponentRegistered(first), "new component slot should be registered");
		Require(data.IsActiveForTest(), "new component slot should be active");
		Require(!data.IsDirtyForTest(), "registration should preserve component-specific dirty tracking");

		data.m_payload = 42;
		system.m_bReenterCleanup = true;
		system.UnregisterComponent(first);

		Require(!system.IsComponentRegistered(first), "unregistered component slot should be inactive");
		Require(system.GetComponentData(first).IsDirtyForTest(), "released component slot should request stale system-state cleanup");
		Require(system.m_numCleanupCalls == 1, "system cleanup hook should run exactly once");
		Require(system.m_lastCleanupPayload == 42, "cleanup hook should observe the live component state");
		Require(LifecycleData::s_numDestructions == 1, "unregister should destroy the released component state");
		Require(system.GetComponentData(first).m_payload == 0, "released component slot should be default reconstructed");

		system.UnregisterComponent(first);
		system.UnregisterComponent(ECS::InvalidIndex);
		system.UnregisterComponent(first + 100);
		Require(system.m_numCleanupCalls == 1, "duplicate and invalid unregister calls should be ignored");
		Require(LifecycleData::s_numDestructions == 1, "duplicate unregister should not destroy a slot twice");

		const size_t reused = system.RegisterComponent();
		Require(reused == first, "the released slot should be reused once");
		Require(system.GetComponentData(reused).m_payload == 0, "reused slot should not retain prior component state");
		Require(!system.GetComponentData(reused).IsDirtyForTest(), "reused component should start with clean component-specific dirty state");

		const size_t second = system.RegisterComponent();
		Require(second != reused, "a duplicate free-list entry must not hand out an active slot twice");
	}

	void TestPreferredEditorInstanceIdsArePreserved()
	{
		PrefabTestWorld world;
		InstanceId gameObjectId;
		gameObjectId.Deserialize(YAML::Node("10010010010010010000"));
		auto gameObject = world.Instantiate("PreferredIdentity", gameObjectId);
		Require(static_cast<bool>(gameObject), "a free preferred game-object identity should be accepted");
		Require(gameObject->GetInstanceId() == gameObjectId, "the preferred game-object identity should be preserved");
		Require(!world.Instantiate("DuplicateIdentity", gameObjectId), "a duplicate preferred game-object identity should be rejected");

		InstanceId componentId;
		componentId.Deserialize(YAML::Node("1111111111111111_10010010010010010000"));
		ComponentPtr component = TObjectPtr<PrefabRollbackTestComponent>::Make(world.GetAllocator());
		auto added = gameObject->AddComponentRaw(component, componentId);
		Require(static_cast<bool>(added), "a free preferred component identity should be accepted");
		Require(added->GetInstanceId() == componentId, "the preferred component identity should be preserved");

		ComponentPtr duplicate = TObjectPtr<PrefabRollbackTestComponent>::Make(world.GetAllocator());
		Require(!gameObject->AddComponentRaw(duplicate, componentId), "a duplicate preferred component identity should be rejected");

		InstanceId wrongOwnerId;
		wrongOwnerId.Deserialize(YAML::Node("2222222222222222_20020020020020020000"));
		ComponentPtr wrongOwner = TObjectPtr<PrefabRollbackTestComponent>::Make(world.GetAllocator());
		Require(!gameObject->AddComponentRaw(wrongOwner, wrongOwnerId), "a preferred component identity for another owner should be rejected");

		world.Clear();
	}

	void TestTransformParentCleanupPreservesPendingReparent()
	{
		PrefabTestWorld world;
		auto previousParent = world.Instantiate("PreviousParent");
		auto nextParent = world.Instantiate("NextParent");
		auto child = world.Instantiate("Child");
		auto* transforms = world.GetECS<TransformECS>();

		child->SetParent(previousParent);
		transforms->Tick(0.0f);
		transforms->PostTick();

		const size_t previousParentIndex = transforms->GetComponentIndex(&previousParent->GetTransformComponent());
		const size_t nextParentIndex = transforms->GetComponentIndex(&nextParent->GetTransformComponent());
		Require(child->GetTransformComponent().GetParent() == previousParentIndex,
			"the transform fixture should establish its original parent before reparenting");

		child->SetParent(nextParent);
		world.DestroyImmediate(previousParent);
		transforms->Tick(0.0f);

		Require(static_cast<bool>(child), "reparenting away should keep the child alive when its previous parent is destroyed");
		Require(child->GetParent() == nextParent, "the game-object hierarchy should retain the requested new parent");
		Require(child->GetTransformComponent().GetParent() == nextParentIndex,
			"transform cleanup should preserve a pending reparent away from the released slot");

		world.Clear();
	}

	void TestEditorKeepWorldReparentUsesCurrentTransforms()
	{
		PrefabTestWorld world;
		auto parent = world.Instantiate("Parent");
		auto child = world.Instantiate("Child");
		parent->GetTransformComponent().SetPosition(glm::vec3(10.0f, 0.0f, 0.0f));
		child->GetTransformComponent().SetPosition(glm::vec3(1.0f, 0.0f, 0.0f));

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(editor.ReparentObject(child->GetInstanceId(), parent->GetInstanceId(), true),
			"keep-world reparent should accept a live parent and child");

		auto* transforms = world.GetECS<TransformECS>();
		transforms->Tick(0.0f);
		transforms->PostTick();

		const glm::vec3 childWorldPosition = child->GetTransformComponent().GetWorldPosition();
		Require(glm::distance(childWorldPosition, glm::vec3(1.0f, 0.0f, 0.0f)) < 0.001f,
			"keep-world reparent should use current local transforms before the first transform tick");

		world.Clear();
	}

	void TestEditorKeepWorldReparentRejectsSingularParentWithoutMutation()
	{
		PrefabTestWorld world;
		auto previousParent = world.Instantiate("PreviousParent");
		auto singularParent = world.Instantiate("SingularParent");
		auto child = world.Instantiate("Child");
		child->GetTransformComponent().SetPosition(glm::vec3(3.0f, 4.0f, 5.0f));
		child->GetTransformComponent().SetScale(glm::vec4(1.5f, 0.75f, 2.0f, 1.0f));
		child->SetParent(previousParent);
		singularParent->GetTransformComponent().SetScale(glm::vec4(0.0f, 1.0f, 1.0f, 1.0f));

		auto* transforms = world.GetECS<TransformECS>();
		transforms->Tick(0.0f);
		transforms->PostTick();

		const Math::Transform localBefore = child->GetTransformComponent().GetTransform();
		const glm::mat4 worldBefore = CalculateCurrentWorldMatrix(child);
		const size_t ecsParentBefore = child->GetTransformComponent().GetParent();

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(!editor.ReparentObject(child->GetInstanceId(), singularParent->GetInstanceId(), true),
			"keep-world reparent should reject a singular parent transform");
		Require(child->GetParent() == previousParent,
			"a rejected singular reparent must preserve the game-object parent");
		Require(AreMatricesNear(child->GetTransformComponent().GetTransform().Matrix(), localBefore.Matrix()),
			"a rejected singular reparent must preserve the local transform");
		Require(AreMatricesNear(CalculateCurrentWorldMatrix(child), worldBefore),
			"a rejected singular reparent must preserve the world transform");

		transforms->Tick(0.0f);
		Require(child->GetTransformComponent().GetParent() == ecsParentBefore,
			"a rejected singular reparent must preserve the ECS parent relationship");
		world.Clear();
	}

	void TestEditorKeepWorldReparentPreservesMirroredTransform()
	{
		PrefabTestWorld world;
		auto mirroredParent = world.Instantiate("MirroredParent");
		auto child = world.Instantiate("Child");
		mirroredParent->GetTransformComponent().SetPosition(glm::vec3(10.0f, -2.0f, 4.0f));
		mirroredParent->GetTransformComponent().SetScale(glm::vec4(-2.0f, 2.0f, 2.0f, 1.0f));
		child->GetTransformComponent().SetPosition(glm::vec3(1.0f, 3.0f, -5.0f));
		child->GetTransformComponent().SetScale(glm::vec4(1.0f, 2.0f, 0.5f, 1.0f));
		const glm::mat4 worldBefore = CalculateCurrentWorldMatrix(child);

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(editor.ReparentObject(child->GetInstanceId(), mirroredParent->GetInstanceId(), true),
			"keep-world reparent should accept an exactly representable mirrored transform");
		Require(AreMatricesNear(CalculateCurrentWorldMatrix(child), worldBefore),
			"keep-world reparent should preserve a mirrored world transform exactly");

		const glm::vec4 localScale = child->GetTransformComponent().GetScale();
		Require(localScale.x * localScale.y * localScale.z < 0.0f,
			"mirrored decomposition should retain reflection in one signed scale axis");

		auto* transforms = world.GetECS<TransformECS>();
		transforms->Tick(0.0f);
		Require(AreMatricesNear(child->GetTransformComponent().GetCachedWorldMatrix(), worldBefore),
			"the transform ECS should retain the mirrored world transform after reparenting");
		world.Clear();
	}

	void TestEditorKeepWorldReparentRejectsShearedCandidateWithoutMutation()
	{
		PrefabTestWorld world;
		auto parent = world.Instantiate("NonUniformRotatedParent");
		auto child = world.Instantiate("Child");
		parent->GetTransformComponent().SetRotation(
			glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
		parent->GetTransformComponent().SetScale(glm::vec4(2.0f, 1.0f, 1.0f, 1.0f));
		child->GetTransformComponent().SetPosition(glm::vec3(2.0f, 3.0f, 4.0f));
		const Math::Transform localBefore = child->GetTransformComponent().GetTransform();
		const glm::mat4 worldBefore = CalculateCurrentWorldMatrix(child);

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(!editor.ReparentObject(child->GetInstanceId(), parent->GetInstanceId(), true),
			"keep-world reparent should reject a local matrix that requires shear");
		Require(!child->GetParent(),
			"a rejected sheared reparent must preserve the original root relationship");
		Require(AreMatricesNear(child->GetTransformComponent().GetTransform().Matrix(), localBefore.Matrix()),
			"a rejected sheared reparent must preserve the local transform");
		Require(AreMatricesNear(CalculateCurrentWorldMatrix(child), worldBefore),
			"a rejected sheared reparent must preserve the world transform");
		world.Clear();
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
	}

	void TestClearingMeshModelAlsoClearsMaterials()
	{
		StaticMeshRendererData data;
		data.GetMaterials().Add(MaterialPtr());
		Require(data.GetMaterials().Num() == 1, "mesh renderer fixture should contain a material slot");

		data.SetModel(ModelPtr());
		Require(data.GetMaterials().IsEmpty(), "clearing a model should clear its stale material overrides");
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

		system.SetAnimation(replacedAnimator, TObjectPtr<Animation>());
		Require(system.GetComponentData(replacedAnimator).m_gpuOffset == invalidOffset &&
			system.GetComponentData(survivingAnimator).m_gpuOffset == invalidOffset,
			"replacing one animation should invalidate every offset in the compact GPU layout");
		Require(system.GetNextBoneOffsetForTest() == 0,
			"replacing an animation should restart compact GPU allocation from the beginning");

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

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string shaderSource = ReadText(sourceRoot / "Content/Shaders/Standard_glTF.shader");
		Require(shaderSource.find("INVALID_SKELETON_OFFSET = 0xFFFFFFFFu") != std::string::npos,
			"the skinned shader should define the invalid skeleton marker");
		Require(shaderSource.find("if (offset != INVALID_SKELETON_OFFSET)") != std::string::npos,
			"the skinned shader should avoid bone-buffer reads for an invalid skeleton offset");
	}

	void TestExpiredWorldPrefabInvalidatesLoadedCacheContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string importerSource = ReadText(
			sourceRoot / "Runtime/AssetRegistry/World/WorldPrefabImporter.cpp");
		const size_t updateBegin = importerSource.find(
			"void WorldPrefabImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)");
		const size_t updateEnd = importerSource.find(
			"void WorldPrefabImporter::OnImportAsset", updateBegin);

		Require(updateBegin != std::string::npos && updateEnd != std::string::npos,
			"world prefab importer must expose its asset-update handler");
		const std::string updateBody = importerSource.substr(updateBegin, updateEnd - updateBegin);
		Require(updateBody.find("if (!bWasExpired)") != std::string::npos,
			"unchanged world metadata must preserve the loaded world prefab cache");
		Require(updateBody.find("m_loadedWorldPrefabs.Remove(uid)") != std::string::npos,
			"an expired world source must invalidate its loaded world prefab");
		Require(updateBody.find("m_promises.Remove(uid)") != std::string::npos,
			"an expired world source must invalidate its completed load promise");
		Require(updateBody.find("dynamic_cast<PrefabAssetInfo*>") != std::string::npos &&
			updateBody.find("m_loadedWorldPrefabs.Clear()") != std::string::npos &&
			updateBody.find("m_promises.Clear()") != std::string::npos,
			"an expired linked prefab source must invalidate cached merged world prefabs");
	}

	void TestEmptyEditorWorldBootstrapContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string engineLoopSource = ReadText(
			sourceRoot / "Runtime/Engine/EngineLoop.cpp");
		const size_t createEmptyWorldBegin = engineLoopSource.find(
			"TSharedPtr<World> EngineLoop::CreateEmptyWorld");
		const size_t createEmptyWorldEnd = engineLoopSource.find(
			"TSharedPtr<World> EngineLoop::InstantiateWorld", createEmptyWorldBegin);

		Require(createEmptyWorldBegin != std::string::npos && createEmptyWorldEnd != std::string::npos,
			"engine loop must expose the empty-world bootstrap");
		const std::string createEmptyWorldBody = engineLoopSource.substr(
			createEmptyWorldBegin,
			createEmptyWorldEnd - createEmptyWorldBegin);
		Require(createEmptyWorldBody.find("AddComponent<CameraComponent>()") != std::string::npos,
			"an empty editor world must create its camera component");
		Require(createEmptyWorldBody.find("AddComponent<EditorComponent>()") != std::string::npos,
			"an empty editor world must create its editor controller component");
		Require(createEmptyWorldBody.find("AddComponent<TestComponent>()") == std::string::npos,
			"an empty editor world must not create the legacy test component");

		const size_t ensureInfrastructureBegin = engineLoopSource.find(
			"void EnsureEditorWorldInfrastructure");
		Require(ensureInfrastructureBegin != std::string::npos &&
			ensureInfrastructureBegin < createEmptyWorldBegin,
			"engine loop must define its editor-world infrastructure bootstrap");
		const std::string ensureInfrastructureBody = engineLoopSource.substr(
			ensureInfrastructureBegin,
			createEmptyWorldBegin - ensureInfrastructureBegin);
		Require(ensureInfrastructureBody.find("GetComponent<CameraComponent>().IsInited()") != std::string::npos,
			"editor-world bootstrap must reuse the first initialized scene camera before BeginPlay");
		Require(ensureInfrastructureBody.find("GetComponent<EditorComponent>().IsInited()") != std::string::npos,
			"editor-world bootstrap must detect an initialized editor controller before BeginPlay");
		Require(ensureInfrastructureBody.find("GameObjectPtr editorOwner") != std::string::npos &&
			ensureInfrastructureBody.find("editorOwner->GetComponent<CameraComponent>().IsInited()") != std::string::npos,
			"an existing editor controller must own a camera instead of creating a duplicate controller");
		Require(ensureInfrastructureBody.find("Instantiate(\"Editor Camera\")") != std::string::npos,
			"a scene without a camera must receive a named editor camera");
		Require(ensureInfrastructureBody.find("AddComponent<CameraComponent>()") != std::string::npos &&
			ensureInfrastructureBody.find("AddComponent<EditorComponent>()") != std::string::npos,
			"the fallback editor camera must contain camera and editor controller components");
		Require(ensureInfrastructureBody.find("AddComponent<TestComponent>()") == std::string::npos,
			"editor-world infrastructure must never add the legacy test component");

		const size_t instantiateWorldEnd = engineLoopSource.find(
			"bool EngineLoop::ExitWorld", createEmptyWorldEnd);
		Require(instantiateWorldEnd != std::string::npos,
			"engine loop must expose the end of its world-loading bootstrap");
		const std::string instantiateWorldBody = engineLoopSource.substr(
			createEmptyWorldEnd,
			instantiateWorldEnd - createEmptyWorldEnd);
		Require(instantiateWorldBody.find("EditorWorldMask") != std::string::npos &&
			instantiateWorldBody.find("EnsureEditorWorldInfrastructure(newWorld)") != std::string::npos,
			"world loading must ensure editor infrastructure only for editor worlds");

		const YAML::Node editorWorld = YAML::Load(ReadText(sourceRoot / "Content/Editor.world"));
		const YAML::Node prefabs = editorWorld["prefabs"];
		Require(prefabs && prefabs.IsSequence() && prefabs.size() > 0,
			"the engine editor scene must contain its camera prefab");

		bool bHasCameraComponent = false;
		bool bHasEditorComponent = false;
		bool bHasTestComponent = false;
		for (const auto& prefab : prefabs)
		{
			const YAML::Node components = prefab["components"];
			if (!components || !components.IsSequence())
			{
				continue;
			}

			for (const auto& component : components)
			{
				const std::string typeName = component["typename"].as<std::string>("");
				bHasCameraComponent |= typeName == "Sailor::CameraComponent";
				bHasEditorComponent |= typeName == "Sailor::EditorComponent";
				bHasTestComponent |= typeName == "Sailor::TestComponent";
			}
		}

		Require(bHasCameraComponent,
			"the engine editor scene must retain its camera component");
		Require(bHasEditorComponent,
			"the engine editor scene must retain its editor controller component");
		Require(!bHasTestComponent,
			"the engine editor scene must not contain the legacy test component");

		const YAML::Node cameraPrefab = prefabs[0];
		const YAML::Node cameraObjects = cameraPrefab["gameObjects"];
		const YAML::Node cameraComponents = cameraPrefab["components"];
		Require(cameraObjects && cameraObjects.IsSequence() && cameraObjects.size() == 1,
			"the editor camera prefab must contain exactly one game object");
		const YAML::Node componentIndices = cameraObjects[0]["components"];
		Require(componentIndices && componentIndices.IsSequence() && componentIndices.size() == 2,
			"the editor camera must reference only its camera and editor controller components");
		Require(componentIndices[0].as<uint32_t>() == 0 && componentIndices[1].as<uint32_t>() == 1,
			"the editor camera component indices must stay contiguous after test-component removal");
		Require(cameraComponents && cameraComponents.IsSequence() && cameraComponents.size() == 2,
			"the editor camera prefab must store exactly two components");
		Require(cameraComponents[0]["typename"].as<std::string>() == "Sailor::CameraComponent" &&
			cameraComponents[1]["typename"].as<std::string>() == "Sailor::EditorComponent",
			"the editor camera prefab must store CameraComponent followed by EditorComponent");
	}

	void TestWorkspaceEditorStartupWorldContract()
	{
		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string appHeader = ReadText(sourceRoot / "Runtime/Sailor.h");
		const std::string appSource = ReadText(sourceRoot / "Runtime/Sailor.cpp");

		Require(appHeader.find("std::string m_world = \"Editor.world\"") != std::string::npos,
			"standalone engine startup must retain its existing Editor.world default");

		const size_t parseBegin = appSource.find("AppArgs ParseCommandLineArgs");
		const size_t parseEnd = appSource.find("void App::Initialize", parseBegin);
		Require(parseBegin != std::string::npos && parseEnd != std::string::npos,
			"application startup must expose its command-line parser");
		const std::string parseBody = appSource.substr(parseBegin, parseEnd - parseBegin);
		const size_t newWorldFlag = parseBody.find("else if (arg == \"--new-world\")");
		const size_t clearWorld = parseBody.find("params.m_world.clear()", newWorldFlag);
		Require(newWorldFlag != std::string::npos && clearWorld != std::string::npos,
			"the explicit workspace new-world flag must clear the standalone default scene");

		const size_t worldBootstrapBegin = appSource.find("auto worldParams =");
		const size_t worldBootstrapEnd = appSource.find("if (auto editor =", worldBootstrapBegin);
		Require(worldBootstrapBegin != std::string::npos && worldBootstrapEnd != std::string::npos,
			"application startup must expose its initial world bootstrap");
		const std::string worldBootstrap = appSource.substr(
			worldBootstrapBegin,
			worldBootstrapEnd - worldBootstrapBegin);
		Require(worldBootstrap.find("if (!params.m_world.empty()") != std::string::npos,
			"an explicit new world must skip startup asset loading");
		Require(worldBootstrap.find("params.m_bIsEditor ? \"New Scene\" : \"New World\"") != std::string::npos,
			"an editor empty-world bootstrap must use the untitled scene name");
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

		Require(firstMesh->GetData().IsDirty(),
			"animation relayout should invalidate the first owned mesh renderer");
		Require(secondMesh->GetData().IsDirty(),
			"animation relayout should invalidate every additional owned mesh renderer");

		world.Clear();
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

		const size_t reused = system.RegisterComponent();
		Require(reused == released, "released sparse light slot should be reused");
		Require(system.GetComponentData(reused).m_type == ELightType::Point,
			"reused light slot should restore default component data");

		const std::filesystem::path sourceRoot = SAILOR_TEST_SOURCE_DIR;
		const std::string lightingSource = ReadText(sourceRoot / "Content/Shaders/Lighting.glsl");
		const std::string cullingSource = ReadText(sourceRoot / "Content/Shaders/ComputeLightCulling.shader");
		Require(lightingSource.find("INVALID_LIGHT_TYPE = 0xFFFFFFFFu") != std::string::npos,
			"shader light layout should define the invalid light marker");
		Require(cullingSource.find("type == INVALID_LIGHT_TYPE") != std::string::npos,
			"light culling should skip invalid sparse slots");
	}

	YAML::Node MakePrefabNode(
		std::initializer_list<uint32_t> parentIndices,
		bool bReferenceMissingComponent = false,
		bool bIncludeParentIndex = true)
	{
		YAML::Node gameObjects(YAML::NodeType::Sequence);
		uint32_t index = 0;
		for (const uint32_t parentIndex : parentIndices)
		{
			Prefab::ReflectedGameObject gameObject{};
			gameObject.m_name = "GameObject" + std::to_string(index++);
			gameObject.m_position = glm::vec4(0.0f);
			gameObject.m_rotation = glm::identity<glm::quat>();
			gameObject.m_scale = glm::vec4(1.0f);
			gameObject.m_parentIndex = parentIndex;
			gameObject.m_instanceId = InstanceId::GenerateNewInstanceId();
			if (bReferenceMissingComponent)
			{
				gameObject.m_components.Add(0);
			}
			YAML::Node gameObjectNode = gameObject.Serialize();
			if (!bIncludeParentIndex)
			{
				gameObjectNode.remove("parentIndex");
			}
			gameObjects.push_back(std::move(gameObjectNode));
		}

		YAML::Node prefabNode;
		prefabNode["gameObjects"] = std::move(gameObjects);
		prefabNode["components"] = YAML::Node(YAML::NodeType::Sequence);
		return prefabNode;
	}

	YAML::Node MakeReflectedComponent(
		const std::string& componentInstanceId,
		const YAML::Node& overrideProperties,
		bool bIncludeInstanceId = true,
		const std::string& typeName =
			PrefabRollbackTestComponent::GetStaticTypeInfo().Name())
	{
		YAML::Node component;
		component["typename"] = typeName;
		component["overrideProperties"] = overrideProperties;
		if (bIncludeInstanceId)
		{
			component["overrideProperties"]["instanceId"] = componentInstanceId;
		}
		return component;
	}

	YAML::Node MakeComponentPrefabNode(const YAML::Node& components)
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		YAML::Node prefabNode = MakePrefabNode({ noParent });
		prefabNode["components"] = components;
		prefabNode["gameObjects"][0]["instanceId"] = "10010010010010010000";
		prefabNode["gameObjects"][0]["components"] = YAML::Node(YAML::NodeType::Sequence);
		for (uint32_t componentIndex = 0; componentIndex < components.size(); ++componentIndex)
		{
			prefabNode["gameObjects"][0]["components"].push_back(componentIndex);
		}
		return prefabNode;
	}

	PrefabPtr DeserializePrefab(PrefabTestWorld& world, const YAML::Node& node)
	{
		PrefabPtr prefab = PrefabPtr::Make(world.GetAllocator(), FileId());
		prefab->Deserialize(node);
		return prefab;
	}

	FileId DeserializeFileId(const char* value)
	{
		FileId fileId;
		fileId.Deserialize(YAML::Node(value));
		return fileId;
	}

	InstanceId DeserializeInstanceId(const char* value)
	{
		InstanceId instanceId;
		instanceId.Deserialize(YAML::Node(value));
		return instanceId;
	}

	PrefabPtr DeserializePrefab(
		PrefabTestWorld& world,
		const FileId& fileId,
		const YAML::Node& node)
	{
		PrefabPtr prefab = PrefabPtr::Make(world.GetAllocator(), fileId);
		prefab->Deserialize(node);
		return prefab;
	}

	void TestLegacyPrefabApiSymbolsRemainAddressable()
	{
		using LegacyGetOverridePrefab =
			bool (Prefab::*)(const PrefabPtr, PrefabPtr) const;
		using LegacyCreate = PrefabPtr (PrefabImporter::*)();
		using FileIdCreate =
			PrefabPtr (PrefabImporter::*)(const FileId&);

		const LegacyGetOverridePrefab legacyGetOverridePrefab =
			static_cast<LegacyGetOverridePrefab>(
				&Prefab::GetOverridePrefab);
		const LegacyCreate legacyCreate =
			static_cast<LegacyCreate>(&PrefabImporter::Create);
		const FileIdCreate fileIdCreate =
			static_cast<FileIdCreate>(&PrefabImporter::Create);

		Require(legacyGetOverridePrefab != nullptr,
			"the legacy exported Prefab::GetOverridePrefab symbol must remain addressable");
		Require(legacyCreate != nullptr,
			"the legacy exported zero-argument PrefabImporter::Create symbol must remain addressable");
		Require(fileIdCreate != nullptr,
			"the FileId-aware PrefabImporter::Create overload must remain independently addressable");

		PrefabTestWorld world;
		const FileId currentFileId =
			DeserializeFileId(
				"{11111111-2222-3333-4444-555555555555}");
		const FileId baseFileId =
			DeserializeFileId(
				"{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}");
		PrefabPtr currentPrefab =
			PrefabPtr::Make(world.GetAllocator(), currentFileId);
		PrefabPtr basePrefab =
			PrefabPtr::Make(world.GetAllocator(), baseFileId);

		Require(
			!(currentPrefab.GetRawPtr()->*legacyGetOverridePrefab)(
				basePrefab,
				PrefabPtr{}),
			"the legacy override API must retain its mismatched-source rejection");

		currentPrefab.DestroyObject(world.GetAllocator());
		basePrefab.DestroyObject(world.GetAllocator());
	}

	void TestLinkedPrefabPersistenceAndWorldContract()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		constexpr const char* sourceRootId = "10010010010010010000";
		constexpr const char* sourceChildId = "20020020020020020000";
		constexpr const char* sourceDependencyId =
			"1111111111111111_10010010010010010000";
		constexpr const char* sourceValueId =
			"2222222222222222_20020020020020020000";
		constexpr const char* liveRootId = "30030030030030030000";
		constexpr const char* liveChildId = "40040040040040040000";
		constexpr const char* externalParentId = "50050050050050050000";
		const FileId sourceFileId =
			DeserializeFileId("{11111111-2222-3333-4444-555555555555}");

		YAML::Node dependencyProperties;
		dependencyProperties["m_dependency"]["fileId"] = "NullFileId";
		dependencyProperties["m_dependency"]["instanceId"] = sourceValueId;

		YAML::Node valueProperties;
		valueProperties["m_value"] = 42.0f;

		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(MakeReflectedComponent(
			sourceDependencyId,
			dependencyProperties));
		components.push_back(MakeReflectedComponent(
			sourceValueId,
			valueProperties));

		YAML::Node sourceNode = MakePrefabNode({ noParent, 0 });
		sourceNode["gameObjects"][0]["instanceId"] = sourceRootId;
		sourceNode["gameObjects"][0]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		sourceNode["gameObjects"][0]["components"].push_back(0);
		sourceNode["gameObjects"][1]["instanceId"] = sourceChildId;
		sourceNode["gameObjects"][1]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		sourceNode["gameObjects"][1]["components"].push_back(1);
		sourceNode["components"] = components;

		PrefabTestWorld world;
		PrefabPtr sourcePrefab =
			DeserializePrefab(world, sourceFileId, sourceNode);
		std::string diagnostic;
		Require(sourcePrefab->ValidateForInstantiation(diagnostic),
			"the linked source prefab fixture should be valid: " + diagnostic);

		TMap<InstanceId, InstanceId> sourceToInstanceIds;
		sourceToInstanceIds[DeserializeInstanceId(sourceRootId)] =
			DeserializeInstanceId(liveRootId);
		sourceToInstanceIds[DeserializeInstanceId(sourceChildId)] =
			DeserializeInstanceId(liveChildId);

		TMap<InstanceId, YAML::Node> gameObjectOverrides;
		YAML::Node childOverride;
		childOverride["name"] = "OverriddenChild";
		childOverride["position"] = glm::vec4(7.0f, 8.0f, 9.0f, 0.0f);
		gameObjectOverrides[DeserializeInstanceId(sourceChildId)] =
			childOverride;

		TMap<InstanceId, ReflectedData> componentOverrides;
		YAML::Node reflectedOverride;
		reflectedOverride["typename"] =
			PrefabRollbackTestComponent::GetStaticTypeInfo().Name();
		reflectedOverride["overrideProperties"]["m_value"] = 99.0f;
		ReflectedData valueOverride;
		valueOverride.Deserialize(reflectedOverride);
		componentOverrides[DeserializeInstanceId(sourceValueId)] =
			valueOverride;

		const InstanceId parentInstanceId =
			DeserializeInstanceId(externalParentId);
		PrefabPtr linkedPrefab =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(linkedPrefab->ConfigureLinkedInstance(
				sourcePrefab,
				sourceToInstanceIds,
				parentInstanceId,
				gameObjectOverrides,
				componentOverrides,
				diagnostic),
			"linked prefab merge should accept stable identities and value overrides: " +
				diagnostic);

		WorldPrefabDocumentFixture document;
		document.AddPrefab(linkedPrefab);
		const YAML::Node serializedWorld = document.Serialize();
		const YAML::Node serializedLinkedPrefab =
			serializedWorld["prefabs"][0];
		Require(
			serializedLinkedPrefab["fileId"].as<FileId>() == sourceFileId,
			"linked world serialization should persist the source FileId");
		Require(
			serializedLinkedPrefab["instanceIds"] &&
				serializedLinkedPrefab["parentInstanceId"] &&
				serializedLinkedPrefab["gameObjectOverrides"] &&
				serializedLinkedPrefab["componentOverrides"],
			"linked world serialization should persist identity, parent, and override metadata");

		const TMap<InstanceId, InstanceId> loadedInstanceIds =
			serializedLinkedPrefab["instanceIds"].as<
				TMap<InstanceId, InstanceId>>();
		const TMap<InstanceId, YAML::Node> loadedGameObjectOverrides =
			serializedLinkedPrefab["gameObjectOverrides"].as<
				TMap<InstanceId, YAML::Node>>();
		const TMap<InstanceId, ReflectedData> loadedComponentOverrides =
			serializedLinkedPrefab["componentOverrides"].as<
				TMap<InstanceId, ReflectedData>>();
		const InstanceId loadedParentId =
			serializedLinkedPrefab["parentInstanceId"].as<InstanceId>();

		PrefabPtr loadedLinkedPrefab =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(loadedLinkedPrefab->ConfigureLinkedInstance(
				sourcePrefab,
				loadedInstanceIds,
				loadedParentId,
				loadedGameObjectOverrides,
				loadedComponentOverrides,
				diagnostic),
			"serialized linked metadata should merge back onto its source prefab: " +
				diagnostic);

		auto externalParent = world.Instantiate(
			"ExternalParent",
			parentInstanceId);
		Require(static_cast<bool>(externalParent),
			"the linked instance external parent should accept its persisted identity");

		const size_t objectCountBeforeInstantiation =
			world.GetGameObjects().Num();
		auto root = world.Instantiate(loadedLinkedPrefab);
		Require(static_cast<bool>(root) && root->GetInstanceId() ==
			DeserializeInstanceId(liveRootId),
			"linked instantiate should preserve the persisted live root identity");
		Require(root->GetParent() == externalParent,
			"linked instantiate should restore its external parent");
		Require(root->GetChildren().Num() == 1 &&
			root->GetChildren()[0]->GetInstanceId() ==
				DeserializeInstanceId(liveChildId),
			"linked instantiate should preserve every mapped child identity");
		GameObjectPtr child = root->GetChildren()[0];

		auto sourceComponent =
			root->GetComponent<PrefabRollbackTestComponent>();
		auto targetComponent =
			child->GetComponent<PrefabRollbackTestComponent>();
		Require(sourceComponent && targetComponent,
			"linked instantiate should recreate its reflected components");
		Require(sourceComponent->m_dependency == targetComponent,
			"linked instantiate should remap internal component references to the live instance");
		Require(targetComponent->m_value == 99.0f,
			"linked instantiate should apply the persisted reflected value override");
		Require(targetComponent->GetInstanceId().ComponentId() ==
			DeserializeInstanceId(sourceValueId).ComponentId(),
			"linked component identity should retain the source-local component id");
		Require(targetComponent->GetInstanceId().GameObjectId() ==
			child->GetInstanceId(),
			"linked component identity should embed its mapped live owner");
		Require(child->GetName() == "OverriddenChild" &&
			child->GetTransformComponent().GetPosition() ==
				glm::vec4(7.0f, 8.0f, 9.0f, 1.0f),
			"linked instantiate should apply name and transform overrides");

		const PrefabInstanceLink* registeredLink = nullptr;
		Require(world.TryGetPrefabInstance(
				child->GetInstanceId(),
				registeredLink) &&
			registeredLink &&
			registeredLink->m_effectiveBaseline &&
			registeredLink->m_effectiveBaseline->GetFileId() ==
				root->GetFileId() &&
			root->GetFileId() == sourceFileId,
			"the root FileId should be authoritative while linked members resolve through matching derived metadata");

		ComponentPtr rejectedComponent =
			TObjectPtr<PrefabRollbackTestComponent>::Make(
				world.GetAllocator());
		Require(!root->AddComponentRaw(rejectedComponent),
			"component additions should be rejected while the prefab is linked");
		Require(!child->RemoveComponent(targetComponent),
			"component removals should be rejected while the prefab is linked");

		auto unlinkedObject = world.Instantiate("Unlinked");
		child->SetParent(unlinkedObject);
		Require(child->GetParent() == root,
			"internal linked game objects should reject reparenting");
		unlinkedObject->SetParent(child);
		Require(!unlinkedObject->GetParent(),
			"unlinked game objects should reject parenting inside a linked instance");

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(!editor.DestroyObject(child->GetInstanceId()),
			"editor deletion should report failure for an internal linked game object");

		world.DestroyImmediate(child);
		Require(static_cast<bool>(world.GetObjectByInstanceId(
			DeserializeInstanceId(liveChildId))),
			"destroying an internal linked game object should be rejected");

		const InstanceId rootIdBeforeBreak = root->GetInstanceId();
		const InstanceId childIdBeforeBreak = child->GetInstanceId();
		const glm::vec4 childPositionBeforeBreak =
			child->GetTransformComponent().GetPosition();
		Require(world.BreakPrefabLink(childIdBeforeBreak),
			"breaking a prefab link through any linked member should succeed");
		Require(!world.IsPrefabLinked(rootIdBeforeBreak) &&
			!root->GetFileId() &&
			root->GetInstanceId() == rootIdBeforeBreak &&
			child->GetInstanceId() == childIdBeforeBreak &&
			child->GetTransformComponent().GetPosition() ==
				childPositionBeforeBreak &&
			targetComponent->m_value == 99.0f,
			"breaking a prefab link should preserve all live ids and values");

		ComponentPtr temporaryComponent =
			TObjectPtr<PrefabRollbackTestComponent>::Make(
				world.GetAllocator());
		temporaryComponent = root->AddComponentRaw(temporaryComponent);
		Require(static_cast<bool>(temporaryComponent),
			"structural changes should become available after breaking the link");
		Require(root->RemoveComponent(temporaryComponent),
			"the temporary post-break component should be removable");

		Require(world.LinkPrefabInstance(
				root,
				sourcePrefab,
				diagnostic),
			"relink should rebuild the deterministic source-to-live mapping: " +
				diagnostic);
		Require(root->GetFileId() == sourceFileId &&
			world.IsPrefabLinked(childIdBeforeBreak),
			"relink should restore the authoritative source FileId and derived membership");

		const size_t linkedCountBeforeRejectedInstantiation =
			world.GetPrefabInstances().Num();
		const size_t objectCountBeforeRejectedInstantiation =
			world.GetGameObjects().Num();
		Require(!world.Instantiate(loadedLinkedPrefab),
			"stale preferred instance ids should reject a second linked instantiate");
		Require(world.GetGameObjects().Num() ==
				objectCountBeforeRejectedInstantiation &&
			world.GetPrefabInstances().Num() ==
				linkedCountBeforeRejectedInstantiation,
			"a rejected stale linked instantiate should roll back world and link state");

		TMap<InstanceId, InstanceId> missingParentMapping;
		missingParentMapping[DeserializeInstanceId(sourceRootId)] =
			DeserializeInstanceId("70070070070070070000");
		missingParentMapping[DeserializeInstanceId(sourceChildId)] =
			DeserializeInstanceId("80080080080080080000");
		PrefabPtr missingParentPrefab =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(missingParentPrefab->ConfigureLinkedInstance(
				sourcePrefab,
				missingParentMapping,
				DeserializeInstanceId("90090090090090090000"),
				gameObjectOverrides,
				componentOverrides,
				diagnostic),
			"the missing-parent rollback fixture should merge before instantiation: " +
				diagnostic);
		Require(!world.Instantiate(missingParentPrefab),
			"a linked instance with a missing external parent should be rejected");
		Require(world.GetGameObjects().Num() ==
				objectCountBeforeRejectedInstantiation &&
			world.GetPrefabInstances().Num() ==
				linkedCountBeforeRejectedInstantiation,
			"a late missing-parent failure should roll back created objects, components, and link state");

		TMap<InstanceId, InstanceId> incompleteMapping;
		incompleteMapping[DeserializeInstanceId(sourceRootId)] =
			DeserializeInstanceId("60060060060060060000");
		PrefabPtr malformedLinkedPrefab =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(!malformedLinkedPrefab->ConfigureLinkedInstance(
				sourcePrefab,
				incompleteMapping,
				InstanceId::Invalid,
				{},
				{},
				diagnostic) &&
			diagnostic.find("mapping") != std::string::npos,
			"a malformed linked identity mapping should produce a load diagnostic");

		const FileId staleSourceId =
			DeserializeFileId("{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}");
		PrefabPtr staleLinkedPrefab =
			PrefabPtr::Make(world.GetAllocator(), staleSourceId);
		Require(!staleLinkedPrefab->ConfigureLinkedInstance(
				sourcePrefab,
				sourceToInstanceIds,
				InstanceId::Invalid,
				{},
				{},
				diagnostic) &&
			diagnostic.find("FileId") != std::string::npos,
			"a stale or mismatched source prefab should produce a FileId diagnostic");

		Require(world.GetGameObjects().Num() >=
			objectCountBeforeInstantiation + 3,
			"the linked persistence fixture should retain its expected live hierarchy");
		world.Clear();
	}

	void TestLinkedPrefabBaselineAndSaveFailureContract()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		constexpr const char* sourceRootId =
			"10010010010010010000";
		constexpr const char* liveRootId =
			"30030030030030030000";
		constexpr const char* sourceComponentId =
			"1111111111111111_10010010010010010000";
		constexpr const char* liveComponentId =
			"1111111111111111_30030030030030030000";
		const FileId sourceFileId =
			DeserializeFileId(
				"{11111111-2222-3333-4444-555555555555}");

		YAML::Node sourceProperties;
		sourceProperties["m_value"] = 1.0f;
		sourceProperties["m_payload"]["outer"]["first"] = 1;
		sourceProperties["m_payload"]["outer"]["second"] = 2;
		YAML::Node sourceComponents(YAML::NodeType::Sequence);
		sourceComponents.push_back(MakeReflectedComponent(
			sourceComponentId,
			sourceProperties));

		YAML::Node sourceNode = MakePrefabNode({ noParent });
		sourceNode["gameObjects"][0]["instanceId"] = sourceRootId;
		sourceNode["gameObjects"][0]["name"] = "SourceV1";
		sourceNode["gameObjects"][0]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		sourceNode["gameObjects"][0]["components"].push_back(0);
		sourceNode["components"] = sourceComponents;

		PrefabTestWorld world;
		PrefabPtr sourcePrefab =
			DeserializePrefab(world, sourceFileId, sourceNode);
		TMap<InstanceId, InstanceId> sourceToInstanceIds;
		sourceToInstanceIds[DeserializeInstanceId(sourceRootId)] =
			DeserializeInstanceId(liveRootId);

		std::string diagnostic;
		PrefabPtr linkedPrefab =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(linkedPrefab->ConfigureLinkedInstance(
				sourcePrefab,
				sourceToInstanceIds,
				InstanceId::Invalid,
				{},
				{},
				diagnostic),
			"the baseline fixture should configure a linked instance: " +
				diagnostic);
		GameObjectPtr root = world.Instantiate(linkedPrefab);
		Require(static_cast<bool>(root),
			"the baseline fixture should instantiate");

		const PrefabInstanceLink* link = nullptr;
		Require(world.TryGetPrefabInstance(root->GetInstanceId(), link) &&
			link &&
			link->m_effectiveBaseline,
			"linked instances should retain an explicit effective baseline");

		YAML::Node evolvedSourceNode = YAML::Clone(sourceNode);
		evolvedSourceNode["gameObjects"][0]["name"] = "SourceV2";
		evolvedSourceNode["gameObjects"][0]["position"] =
			glm::vec4(5.0f, 6.0f, 7.0f, 0.0f);
		evolvedSourceNode["components"][0]["overrideProperties"]["m_value"] =
			10.0f;
		PrefabPtr evolvedSource =
			DeserializePrefab(world, sourceFileId, evolvedSourceNode);

		YAML::Node expandedNode = YAML::Clone(sourceNode);
		expandedNode["gameObjects"][0]["instanceId"] = liveRootId;
		expandedNode["gameObjects"][0]["position"] =
			root->GetTransformComponent().GetPosition();
		expandedNode["gameObjects"][0]["rotation"] =
			root->GetTransformComponent().GetRotation();
		expandedNode["gameObjects"][0]["scale"] =
			root->GetTransformComponent().GetScale();
		expandedNode["components"][0]["overrideProperties"]["instanceId"] =
			liveComponentId;
		YAML::Node reorderedPayload;
		reorderedPayload["outer"]["second"] = 2;
		reorderedPayload["outer"]["first"] = 1;
		expandedNode["components"][0]["overrideProperties"]["m_payload"] =
			reorderedPayload;
		PrefabPtr expandedPrefab =
			DeserializePrefab(world, sourceFileId, expandedNode);

		TMap<InstanceId, YAML::Node> gameObjectOverrides;
		TMap<InstanceId, ReflectedData> componentOverrides;
		Require(WorldPrefabDocumentFixture::BuildUpdatedOverrides(
				expandedPrefab,
				evolvedSource,
				link->m_effectiveBaseline,
				sourceToInstanceIds,
				gameObjectOverrides,
				componentOverrides,
				diagnostic),
			"source evolution should merge against the captured baseline: " +
				diagnostic);
		Require(gameObjectOverrides.IsEmpty() &&
			componentOverrides.IsEmpty(),
			"unrelated source changes and reordered map keys must not become instance overrides when the live instance was not edited");

		YAML::Node editedExpandedNode = YAML::Clone(expandedNode);
		editedExpandedNode["gameObjects"][0]["name"] = "InstanceEdit";
		editedExpandedNode["components"][0]["overrideProperties"]["m_value"] =
			3.0f;
		PrefabPtr editedExpanded =
			DeserializePrefab(world, sourceFileId, editedExpandedNode);
		Require(WorldPrefabDocumentFixture::BuildUpdatedOverrides(
				editedExpanded,
				evolvedSource,
				link->m_effectiveBaseline,
				sourceToInstanceIds,
				gameObjectOverrides,
				componentOverrides,
				diagnostic),
			"live edits should merge against the captured baseline: " +
				diagnostic);
		const InstanceId sourceRoot =
			DeserializeInstanceId(sourceRootId);
		const InstanceId sourceComponent =
			DeserializeInstanceId(sourceComponentId);
		Require(gameObjectOverrides.ContainsKey(sourceRoot) &&
			gameObjectOverrides[sourceRoot]["name"].as<std::string>() ==
				"InstanceEdit" &&
			componentOverrides.ContainsKey(sourceComponent) &&
			componentOverrides[sourceComponent].GetProperties()[
				"m_value"].as<float>() == 3.0f,
			"edits made after instantiation should become explicit overrides");

		TMap<InstanceId, YAML::Node> priorGameObjectOverrides;
		YAML::Node priorGameObjectOverride;
		priorGameObjectOverride["name"] = "PinnedName";
		priorGameObjectOverrides[sourceRoot] =
			priorGameObjectOverride;
		TMap<InstanceId, ReflectedData> priorComponentOverrides;
		YAML::Node priorComponentOverrideNode;
		priorComponentOverrideNode["typename"] =
			PrefabRollbackTestComponent::GetStaticTypeInfo().Name();
		priorComponentOverrideNode["overrideProperties"]["m_value"] =
			2.0f;
		ReflectedData priorComponentOverride;
		priorComponentOverride.Deserialize(priorComponentOverrideNode);
		priorComponentOverrides[sourceComponent] =
			priorComponentOverride;

		PrefabPtr explicitBaseline =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(explicitBaseline->ConfigureLinkedInstance(
				sourcePrefab,
				sourceToInstanceIds,
				InstanceId::Invalid,
				priorGameObjectOverrides,
				priorComponentOverrides,
				diagnostic),
			"the explicit override baseline should configure: " +
				diagnostic);
		YAML::Node overriddenExpandedNode = YAML::Clone(expandedNode);
		overriddenExpandedNode["gameObjects"][0]["name"] =
			"PinnedName";
		overriddenExpandedNode["components"][0]["overrideProperties"]["m_value"] =
			2.0f;
		PrefabPtr overriddenExpanded =
			DeserializePrefab(world, sourceFileId, overriddenExpandedNode);
		Require(WorldPrefabDocumentFixture::BuildUpdatedOverrides(
				overriddenExpanded,
				evolvedSource,
				explicitBaseline,
				sourceToInstanceIds,
				gameObjectOverrides,
				componentOverrides,
				diagnostic),
			"existing explicit overrides should merge across source evolution: " +
				diagnostic);
		Require(gameObjectOverrides.ContainsKey(sourceRoot) &&
			gameObjectOverrides[sourceRoot]["name"].as<std::string>() ==
				"PinnedName" &&
			componentOverrides.ContainsKey(sourceComponent) &&
			componentOverrides[sourceComponent].GetProperties()[
				"m_value"].as<float>() == 2.0f,
			"existing explicit overrides should survive unrelated source changes");

		WorldPrefabDocumentFixture failedDocument;
		failedDocument.MarkSerializationFailure(
			"linked source asset is unavailable");
		Require(failedDocument.Serialize().IsNull(),
			"a failed linked world serialization should not emit an empty replacement world");
		WorldPrefabDocumentFixture nonReadyDocument;
		Require(nonReadyDocument.Serialize().IsNull(),
			"a non-ready world document should not emit partial YAML without a diagnostic");
		const std::filesystem::path failedSavePath =
			std::filesystem::temp_directory_path() /
			"sailor-linked-prefab-failed-save.world";
		{
			std::ofstream existingScene(
				failedSavePath,
				std::ios::binary | std::ios::trunc);
			existingScene << "existing-scene";
		}
		Require(!failedDocument.SaveToFile(
				failedSavePath.generic_string()) &&
			ReadText(failedSavePath) == "existing-scene",
			"a failed linked world document should not overwrite a scene file");
		std::filesystem::remove(failedSavePath);

		const std::filesystem::path sourceRootPath =
			SAILOR_TEST_SOURCE_DIR;
		const std::string editorSource = ReadText(
			sourceRootPath / "Runtime/Submodules/Editor.cpp");
		const std::string interopSource = ReadText(
			sourceRootPath / "Runtime/Editor/EditorInterop.cpp");
		const std::string worldPrefabSource = ReadText(
			sourceRootPath /
				"Runtime/AssetRegistry/World/WorldPrefabImporter.cpp");
		Require(editorSource.find("!prefab->IsReady()") !=
				std::string::npos &&
			editorSource.find("GetLoadDiagnostic()") !=
				std::string::npos &&
			interopSource.find("node.IsNull()") !=
				std::string::npos,
			"failed world serialization should propagate through Editor and interop as a save failure");
		Require(worldPrefabSource.find(
				"preserving its expanded live state as inline data") ==
				std::string::npos &&
			worldPrefabSource.find(
				"res->m_gameObjects.Add(Prefab::FromGameObject(root));") ==
				std::string::npos,
			"linked serialization failures must never silently flatten an instance into inline scene data");

		world.Clear();
	}

	void TestLinkedPrefabSourceStructureEvolutionContract()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		constexpr const char* sourceRootId = "10010010010010010000";
		constexpr const char* removedSourceChildId =
			"20020020020020020000";
		constexpr const char* addedSourceChildId =
			"AAAAAAAAAAAAAAAAAAAA";
		constexpr const char* addedSourceGrandchildId =
			"BBBBBBBBBBBBBBBBBBBB";
		constexpr const char* liveRootId = "30030030030030030000";
		constexpr const char* removedLiveChildId =
			"40040040040040040000";
		constexpr const char* sourceDependencyId =
			"1111111111111111_10010010010010010000";
		constexpr const char* addedSourceValueId =
			"3333333333333333_BBBBBBBBBBBBBBBBBBBB";
		const FileId sourceFileId =
			DeserializeFileId(
				"{11111111-2222-3333-4444-555555555555}");

		YAML::Node previousSourceNode =
			MakePrefabNode({ noParent, 0 });
		previousSourceNode["gameObjects"][0]["instanceId"] =
			sourceRootId;
		previousSourceNode["gameObjects"][1]["instanceId"] =
			removedSourceChildId;

		YAML::Node expandedRecordNode = previousSourceNode;
		expandedRecordNode["gameObjects"][0]["instanceId"] =
			liveRootId;
		expandedRecordNode["gameObjects"][1]["instanceId"] =
			removedLiveChildId;

		YAML::Node dependencyProperties;
		dependencyProperties["m_dependency"]["fileId"] = "NullFileId";
		dependencyProperties["m_dependency"]["instanceId"] =
			addedSourceValueId;
		YAML::Node valueProperties;
		valueProperties["m_value"] = 55.0f;
		YAML::Node evolvedComponents(YAML::NodeType::Sequence);
		evolvedComponents.push_back(MakeReflectedComponent(
			sourceDependencyId,
			dependencyProperties));
		evolvedComponents.push_back(MakeReflectedComponent(
			addedSourceValueId,
			valueProperties));

		YAML::Node evolvedSourceNode =
			MakePrefabNode({ noParent, 0, 1 });
		evolvedSourceNode["gameObjects"][0]["instanceId"] =
			sourceRootId;
		evolvedSourceNode["gameObjects"][0]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		evolvedSourceNode["gameObjects"][0]["components"].push_back(0);
		evolvedSourceNode["gameObjects"][1]["instanceId"] =
			addedSourceChildId;
		evolvedSourceNode["gameObjects"][2]["instanceId"] =
			addedSourceGrandchildId;
		evolvedSourceNode["gameObjects"][2]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		evolvedSourceNode["gameObjects"][2]["components"].push_back(1);
		evolvedSourceNode["components"] = evolvedComponents;

		PrefabTestWorld world;
		PrefabPtr expandedRecord =
			DeserializePrefab(world, expandedRecordNode);
		PrefabPtr evolvedSource = DeserializePrefab(
			world,
			sourceFileId,
			evolvedSourceNode);
		std::string diagnostic;
		Require(expandedRecord->ValidateForInstantiation(diagnostic) &&
			evolvedSource->ValidateForInstantiation(diagnostic),
			"the structural evolution fixtures should be valid: " +
				diagnostic);

		TMap<InstanceId, InstanceId> savedMappings;
		savedMappings[DeserializeInstanceId(sourceRootId)] =
			DeserializeInstanceId(liveRootId);
		savedMappings[
			DeserializeInstanceId(removedSourceChildId)] =
			DeserializeInstanceId(removedLiveChildId);

		TSet<InstanceId> reservedInstanceIds;
		reservedInstanceIds.Insert(
			DeserializeInstanceId(liveRootId));
		reservedInstanceIds.Insert(
			DeserializeInstanceId(removedLiveChildId));
		TSet<InstanceId> repeatedReservedInstanceIds =
			reservedInstanceIds;

		TMap<InstanceId, InstanceId> reconciledMappings;
		TMap<InstanceId, InstanceId> repeatedMappings;
		Require(WorldPrefabDocumentFixture::Reconcile(
				expandedRecord,
				evolvedSource,
				savedMappings,
				reservedInstanceIds,
				reconciledMappings,
				diagnostic),
			"source structural evolution should reconcile linked ids: " +
				diagnostic);
		Require(WorldPrefabDocumentFixture::Reconcile(
				expandedRecord,
				evolvedSource,
				savedMappings,
				repeatedReservedInstanceIds,
				repeatedMappings,
				diagnostic),
			"repeated source reconciliation should succeed: " +
				diagnostic);

		const InstanceId sourceRoot =
			DeserializeInstanceId(sourceRootId);
		const InstanceId removedSourceChild =
			DeserializeInstanceId(removedSourceChildId);
		const InstanceId addedSourceChild =
			DeserializeInstanceId(addedSourceChildId);
		const InstanceId addedSourceGrandchild =
			DeserializeInstanceId(addedSourceGrandchildId);
		Require(reconciledMappings.Num() == 3 &&
			reconciledMappings.ContainsKey(sourceRoot) &&
			reconciledMappings[sourceRoot] ==
				DeserializeInstanceId(liveRootId),
			"reconciliation should retain the surviving root's live identity");
		Require(!reconciledMappings.ContainsKey(removedSourceChild),
			"reconciliation should drop mappings for removed source game objects");
		Require(reconciledMappings.ContainsKey(addedSourceChild) &&
			reconciledMappings.ContainsKey(addedSourceGrandchild) &&
			reconciledMappings[addedSourceChild].IsGameObjectId() &&
			reconciledMappings[addedSourceGrandchild].IsGameObjectId() &&
			reconciledMappings[addedSourceChild] !=
				reconciledMappings[addedSourceGrandchild] &&
			reconciledMappings[addedSourceChild] !=
				DeserializeInstanceId(removedLiveChildId) &&
			reconciledMappings[addedSourceGrandchild] !=
				DeserializeInstanceId(removedLiveChildId),
			"new source nodes should receive collision-free canonical live identities");
		Require(repeatedMappings[addedSourceChild] ==
				reconciledMappings[addedSourceChild] &&
			repeatedMappings[addedSourceGrandchild] ==
				reconciledMappings[addedSourceGrandchild],
			"new source node identities should be deterministic across repositories and reloads");

		TMap<InstanceId, YAML::Node> survivingOverrides;
		YAML::Node rootOverride;
		rootOverride["name"] = "PersistedRootOverride";
		survivingOverrides[sourceRoot] = rootOverride;
		PrefabPtr reconciledPrefab =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(reconciledPrefab->ConfigureLinkedInstance(
				evolvedSource,
				reconciledMappings,
				InstanceId::Invalid,
				survivingOverrides,
				{},
				diagnostic),
			"the reconciled evolved prefab should merge surviving overrides: " +
				diagnostic);

		auto root = world.Instantiate(reconciledPrefab);
		Require(root && root->GetInstanceId() ==
				DeserializeInstanceId(liveRootId) &&
			root->GetName() == "PersistedRootOverride",
			"evolved linked instantiate should retain surviving ids and overrides");
		Require(root->GetChildren().Num() == 1,
			"evolved linked instantiate should add the new source child");
		GameObjectPtr evolvedChild = root->GetChildren()[0];
		Require(evolvedChild->GetChildren().Num() == 1,
			"evolved linked instantiate should add the new source grandchild");
		GameObjectPtr evolvedGrandchild =
			evolvedChild->GetChildren()[0];
		Require(evolvedChild->GetInstanceId() ==
				reconciledMappings[addedSourceChild] &&
			evolvedGrandchild->GetInstanceId() ==
				reconciledMappings[addedSourceGrandchild],
			"evolved linked instantiate should materialize the new source hierarchy");

		auto dependency =
			root->GetComponent<PrefabRollbackTestComponent>();
		auto target = evolvedGrandchild->
			GetComponent<PrefabRollbackTestComponent>();
		Require(dependency && target &&
			dependency->m_dependency == target &&
			target->m_value == 55.0f,
			"internal references should resolve through generated ids for new source nodes");

		world.Clear();
	}

	void TestLinkedPrefabMembershipFollowsEvolvedSourceMapping()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		constexpr const char* sourceRootId =
			"10010010010010010000";
		constexpr const char* removedSourceChildId =
			"20020020020020020000";
		constexpr const char* liveRootId =
			"30030030030030030000";
		constexpr const char* removedLiveChildId =
			"40040040040040040000";
		const FileId sourceFileId =
			DeserializeFileId(
				"{11111111-2222-3333-4444-555555555555}");

		YAML::Node sourceNode = MakePrefabNode({ noParent, 0 });
		sourceNode["gameObjects"][0]["instanceId"] =
			sourceRootId;
		sourceNode["gameObjects"][1]["instanceId"] =
			removedSourceChildId;

		PrefabTestWorld world;
		PrefabPtr sourcePrefab = DeserializePrefab(
			world,
			sourceFileId,
			sourceNode);
		TMap<InstanceId, InstanceId> initialMappings;
		initialMappings[DeserializeInstanceId(sourceRootId)] =
			DeserializeInstanceId(liveRootId);
		initialMappings[
			DeserializeInstanceId(removedSourceChildId)] =
			DeserializeInstanceId(removedLiveChildId);

		std::string diagnostic;
		PrefabPtr linkedPrefab =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(linkedPrefab->ConfigureLinkedInstance(
				sourcePrefab,
				initialMappings,
				InstanceId::Invalid,
				{},
				{},
				diagnostic),
			"the removed-source membership fixture should configure: " +
				diagnostic);

		GameObjectPtr root = world.Instantiate(linkedPrefab);
		Require(root && root->GetChildren().Num() == 1,
			"the removed-source membership fixture should instantiate");
		GameObjectPtr removedChild = root->GetChildren()[0];
		const InstanceId rootInstanceId = root->GetInstanceId();
		const InstanceId removedChildInstanceId =
			removedChild->GetInstanceId();
		Require(world.IsPrefabLinked(rootInstanceId) &&
			world.IsPrefabLinked(removedChildInstanceId) &&
			!world.CanModifyPrefabStructure(removedChildInstanceId),
			"the previous source child should initially be a locked linked member");

		const PrefabInstanceLink* initialLink = nullptr;
		Require(world.TryGetPrefabInstance(
				rootInstanceId,
				initialLink) &&
			initialLink &&
			initialLink->m_effectiveBaseline,
			"the linked fixture should expose its effective baseline");

		TMap<InstanceId, InstanceId> evolvedMappings;
		evolvedMappings[DeserializeInstanceId(sourceRootId)] =
			rootInstanceId;
		Require(WorldPrefabDocumentFixture::CommitLinkedUpdate(
				&world,
				rootInstanceId,
				evolvedMappings,
				initialLink->m_effectiveBaseline,
				diagnostic),
			"committing an evolved source mapping should succeed: " +
				diagnostic);

		Require(world.IsPrefabLinked(rootInstanceId) &&
			!world.IsPrefabLinked(removedChildInstanceId) &&
			world.CanModifyPrefabStructure(removedChildInstanceId),
			"a live child removed from the source mapping must stop resolving as a linked member");

		ComponentPtr temporaryComponent =
			TObjectPtr<PrefabRollbackTestComponent>::Make(
				world.GetAllocator());
		temporaryComponent =
			removedChild->AddComponentRaw(temporaryComponent);
		Require(temporaryComponent &&
			removedChild->RemoveComponent(temporaryComponent),
			"the removed-source child should allow structural edits immediately");

		const size_t objectCountBeforeChildCleanup =
			world.GetGameObjects().Num();
		world.DestroyImmediate(removedChild);
		Require(world.GetGameObjects().Num() + 1 ==
				objectCountBeforeChildCleanup &&
			!world.GetObjectByInstanceId(removedChildInstanceId),
			"the removed-source child should be cleaned up exactly once");

		Require(world.BreakPrefabLink(rootInstanceId) &&
			world.GetPrefabInstances().IsEmpty() &&
			!world.IsPrefabLinked(rootInstanceId) &&
			!world.BreakPrefabLink(rootInstanceId),
			"breaking the surviving root should purge the remaining membership exactly once");

		world.Clear();
	}

	void TestPrefabRootFileIdRemainsAuthoritativeWhenDerivedMetadataIsMissing()
	{
		constexpr uint32_t noParent =
			static_cast<uint32_t>(-1);
		const FileId sourceFileId =
			DeserializeFileId(
				"{11111111-2222-3333-4444-666666666666}");

		PrefabTestWorld world;
		PrefabPtr sourcePrefab = DeserializePrefab(
			world,
			sourceFileId,
			MakePrefabNode({ noParent, 0 }));
		std::string diagnostic;
		Require(sourcePrefab->ValidateForInstantiation(
				diagnostic),
			"the FileId-authority fixture should be valid: " +
				diagnostic);

		GameObjectPtr root = world.Instantiate(sourcePrefab);
		Require(root &&
			root->GetChildren().Num() == 1 &&
			root->GetFileId() == sourceFileId &&
			world.IsPrefabInstanceRoot(root->GetInstanceId()) &&
			world.IsPrefabLinked(root->GetInstanceId()),
			"a non-empty root FileId should establish the prefab link");
		GameObjectPtr child = root->GetChildren()[0];
		Require(world.IsPrefabLinked(child->GetInstanceId()),
			"a mapped source child should initially resolve through derived membership");

		Require(world.RemovePrefabMetadataForTest(
				root->GetInstanceId()),
			"the fixture should remove only derived prefab metadata");
		const PrefabInstanceLink* missingLink = nullptr;
		Require(root->GetFileId() == sourceFileId &&
			world.IsPrefabInstanceRoot(root->GetInstanceId()) &&
			world.IsPrefabLinked(root->GetInstanceId()) &&
			!world.TryGetPrefabInstance(
				root->GetInstanceId(),
				missingLink) &&
			!world.IsPrefabLinked(child->GetInstanceId()),
			"missing derived metadata must not erase the authoritative root link or create a valid child membership");

		Require(world.BreakPrefabLink(root->GetInstanceId()) &&
			!root->GetFileId() &&
			!world.IsPrefabLinked(root->GetInstanceId()) &&
			world.LinkPrefabInstance(
				root,
				sourcePrefab,
				diagnostic),
			"break should clear the authoritative FileId and stale caches so the hierarchy can be relinked: " +
				diagnostic);
		Require(root->GetFileId() == sourceFileId &&
			world.IsPrefabLinked(child->GetInstanceId()),
			"relink should republish the source FileId after rebuilding derived membership");

		world.Clear();
	}

	void TestDetachedSupplementalPrefabPersistenceAndStrictRestore()
	{
		constexpr uint32_t noParent =
			static_cast<uint32_t>(-1);
		constexpr const char* sourceRootId =
			"10010010010010010000";
		constexpr const char* removedSourceChildId =
			"20020020020020020000";
		constexpr const char* liveRootId =
			"30030030030030030000";
		constexpr const char* removedLiveChildId =
			"40040040040040040000";
		constexpr const char* sourceMixedComponentId =
			"1111111111111111_10010010010010010000";
		constexpr const char* sourceTargetComponentId =
			"2222222222222222_10010010010010010000";
		constexpr const char* removedSourceComponentId =
			"3333333333333333_20020020020020020000";
		constexpr const char* removedLiveComponentId =
			"3333333333333333_40040040040040040000";
		const FileId sourceFileId =
			DeserializeFileId(
				"{11111111-2222-3333-4444-555555555555}");
		const InstanceId sourceRoot =
			DeserializeInstanceId(sourceRootId);
		const InstanceId liveRoot =
			DeserializeInstanceId(liveRootId);
		const InstanceId removedLiveChild =
			DeserializeInstanceId(removedLiveChildId);
		const InstanceId removedLiveComponent =
			DeserializeInstanceId(removedLiveComponentId);

		YAML::Node removedComponentProperties;
		removedComponentProperties["m_value"] = 73.0f;
		YAML::Node previousSourceComponents(
			YAML::NodeType::Sequence);
		previousSourceComponents.push_back(
			MakeReflectedComponent(
				removedSourceComponentId,
				removedComponentProperties));
		YAML::Node previousSourceNode =
			MakePrefabNode({ noParent, 0 });
		previousSourceNode["gameObjects"][0]["instanceId"] =
			sourceRootId;
		previousSourceNode["gameObjects"][1]["instanceId"] =
			removedSourceChildId;
		previousSourceNode["gameObjects"][1]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		previousSourceNode["gameObjects"][1]["components"].
			push_back(0);
		previousSourceNode["components"] =
			previousSourceComponents;

		YAML::Node expandedRecordNode =
			YAML::Clone(previousSourceNode);
		expandedRecordNode["gameObjects"][0]["instanceId"] =
			liveRootId;
		expandedRecordNode["gameObjects"][1]["instanceId"] =
			removedLiveChildId;
		expandedRecordNode["gameObjects"][1]["name"] =
			"DetachedEdited";
		expandedRecordNode["gameObjects"][1]["position"] =
			glm::vec4(4.0f, 5.0f, 6.0f, 0.0f);
		expandedRecordNode["components"][0]
			["overrideProperties"]["instanceId"] =
			removedLiveComponentId;

		YAML::Node sourceReference;
		sourceReference["fileId"] = "NullFileId";
		sourceReference["instanceId"] =
			sourceTargetComponentId;
		YAML::Node mixedProperties;
		mixedProperties["m_sourceDependency"] =
			sourceReference;
		YAML::Node targetProperties;
		targetProperties["m_value"] = 21.0f;
		YAML::Node evolvedComponents(
			YAML::NodeType::Sequence);
		evolvedComponents.push_back(
			MakeReflectedComponent(
				sourceMixedComponentId,
				mixedProperties,
				true,
				PrefabMixedDependencyTestComponent::
					GetStaticTypeInfo().Name()));
		evolvedComponents.push_back(
			MakeReflectedComponent(
				sourceTargetComponentId,
				targetProperties));
		YAML::Node evolvedSourceNode =
			MakePrefabNode({ noParent });
		evolvedSourceNode["gameObjects"][0]["instanceId"] =
			sourceRootId;
		evolvedSourceNode["gameObjects"][0]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		evolvedSourceNode["gameObjects"][0]["components"].
			push_back(0);
		evolvedSourceNode["gameObjects"][0]["components"].
			push_back(1);
		evolvedSourceNode["components"] =
			evolvedComponents;

		PrefabTestWorld world;
		PrefabPtr previousSource = DeserializePrefab(
			world,
			sourceFileId,
			previousSourceNode);
		PrefabPtr evolvedSource = DeserializePrefab(
			world,
			sourceFileId,
			evolvedSourceNode);
		PrefabPtr expandedRecord = DeserializePrefab(
			world,
			expandedRecordNode);
		std::string diagnostic;
		Require(previousSource->ValidateForInstantiation(
				diagnostic) &&
			evolvedSource->ValidateForInstantiation(
				diagnostic) &&
			expandedRecord->ValidateForInstantiation(
				diagnostic),
			"the detached supplemental evolution fixtures should be valid: " +
				diagnostic);

		TMap<InstanceId, InstanceId> evolvedMappings;
		evolvedMappings[sourceRoot] = liveRoot;

		YAML::Node liveReference;
		liveReference["fileId"] = "NullFileId";
		liveReference["instanceId"] =
			removedLiveComponentId;
		YAML::Node mixedOverrideNode;
		mixedOverrideNode["typename"] =
			PrefabMixedDependencyTestComponent::
				GetStaticTypeInfo().Name();
		mixedOverrideNode["overrideProperties"]
			["m_liveDependency"] = liveReference;
		ReflectedData mixedOverride;
		mixedOverride.Deserialize(mixedOverrideNode);
		Require(mixedOverride.IsValid(),
			"the mixed source/live dependency override should be valid");
		TMap<InstanceId, ReflectedData> componentOverrides;
		componentOverrides[
			DeserializeInstanceId(
				sourceMixedComponentId)] =
			mixedOverride;

		auto buildLinkedRecord =
			[&](const PrefabPtr& expanded)
			{
				PrefabPtr linked =
					PrefabPtr::Make(
						world.GetAllocator(),
						sourceFileId);
				Require(linked->ConfigureLinkedInstance(
						evolvedSource,
						evolvedMappings,
						InstanceId::Invalid,
						{},
						componentOverrides,
						diagnostic),
					"the evolved linked record should configure: " +
						diagnostic);
				Require(linked->
						AppendDetachedSupplementalHierarchy(
							expanded,
							diagnostic),
					"the removed source child should append as supplemental data: " +
						diagnostic);
				return linked;
			};

		const YAML::Node firstPersistedNode =
			expandedRecord->Serialize();
		PrefabPtr persistedExpandedRecord =
			DeserializePrefab(
				world,
				YAML::Load(
					YAML::Dump(
						firstPersistedNode)));
		PrefabPtr linkedRecord =
			buildLinkedRecord(
				persistedExpandedRecord);
		GameObjectPtr root =
			world.Instantiate(linkedRecord, true);
		Require(root &&
			root->GetInstanceId() == liveRoot &&
			root->GetChildren().Num() == 1,
			"the linked record should restore the mapped root and supplemental child with exact ids");
		GameObjectPtr detachedChild =
			root->GetChildren()[0];
		Require(detachedChild->GetInstanceId() ==
				removedLiveChild &&
			detachedChild->GetParent() == root &&
			detachedChild->GetName() ==
				"DetachedEdited" &&
			detachedChild->GetTransformComponent().
				GetPosition() ==
				glm::vec4(4.0f, 5.0f, 6.0f, 1.0f),
			"supplemental hierarchy edits and parent semantics should survive reload");
		Require(world.IsPrefabLinked(
				root->GetInstanceId()) &&
			!world.IsPrefabLinked(
				detachedChild->GetInstanceId()) &&
			world.CanModifyPrefabStructure(
				detachedChild->GetInstanceId()),
			"supplemental children must remain editable and outside reverse linked membership");

		auto sourceTarget =
			root->GetComponent<
				PrefabRollbackTestComponent>();
		auto mixed =
			root->GetComponent<
				PrefabMixedDependencyTestComponent>();
		auto detachedTarget =
			detachedChild->GetComponent<
				PrefabRollbackTestComponent>();
		Require(sourceTarget &&
			mixed &&
			detachedTarget &&
			sourceTarget->m_value == 21.0f &&
			detachedTarget->m_value == 73.0f &&
			mixed->m_sourceDependency ==
				sourceTarget &&
			mixed->m_liveDependency ==
				detachedTarget,
			"a single component should resolve mixed source and supplemental live aliases");

		PrefabPtr firstSavedExpanded =
			PrefabDocumentTestAsset::Capture(
				world,
				root);
		Require(PrefabDocumentTestAsset::
				MarkExpandedLinkedRecord(
					firstSavedExpanded,
					evolvedMappings,
					diagnostic),
			"the expanded save record should remain valid after linked serialization metadata is attached: " +
				diagnostic);
		const std::string firstSavedYaml =
			YAML::Dump(
				firstSavedExpanded->Serialize());
		PrefabPtr secondExpandedRecord =
			DeserializePrefab(
				world,
				YAML::Load(firstSavedYaml));
		world.Clear();

		PrefabPtr secondLinkedRecord =
			buildLinkedRecord(
				secondExpandedRecord);
		root = world.Instantiate(
			secondLinkedRecord,
			true);
		Require(root &&
			root->GetChildren().Num() == 1 &&
			root->GetChildren()[0]->GetInstanceId() ==
				removedLiveChild,
			"save-load should preserve the supplemental hierarchy without duplicating source nodes");
		detachedChild = root->GetChildren()[0];
		PrefabPtr secondSavedExpanded =
			PrefabDocumentTestAsset::Capture(
				world,
				root);
		Require(PrefabDocumentTestAsset::
				MarkExpandedLinkedRecord(
					secondSavedExpanded,
					evolvedMappings,
					diagnostic),
			"the second expanded save record should retain valid supplemental metadata: " +
				diagnostic);
		Require(YAML::Dump(
				secondSavedExpanded->Serialize()) ==
				firstSavedYaml,
			"the expanded linked hierarchy should be stable across save-load-save");

		PrefabPtr detachedSnapshotSource =
			PrefabDocumentTestAsset::Capture(
				world,
				detachedChild);
		YAML::Node detachedSnapshotNode =
			detachedSnapshotSource->Serialize();
		::Serialize(
			detachedSnapshotNode,
			"detachedFromPrefab",
			true);
		::Serialize(
			detachedSnapshotNode,
			"parentInstanceId",
			liveRoot);
		PrefabPtr detachedSnapshot =
			DeserializePrefab(
				world,
				YAML::Load(
					YAML::Dump(
						detachedSnapshotNode)));
		Require(detachedSnapshot->
				ValidateForInstantiation(
					diagnostic) &&
			detachedSnapshot->
				IsDetachedFromPrefabRecord(),
			"the detached undo snapshot should round-trip its strict restore marker: " +
				diagnostic);

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		const size_t objectCountBeforeRejectedRestore =
			world.GetGameObjects().Num();
		Require(!editor.InstantiatePrefab(
				detachedSnapshot,
				liveRoot,
				false) &&
			world.GetGameObjects().Num() ==
				objectCountBeforeRejectedRestore,
			"a detached snapshot must reject non-strict restore without mutation");

		GameObjectPtr wrongParent =
			world.Instantiate("WrongParent");
		Require(wrongParent &&
			!editor.InstantiatePrefab(
				detachedSnapshot,
				wrongParent->GetInstanceId(),
				true),
			"a detached snapshot must reject a call-parent mismatch");

		world.DestroyImmediate(detachedChild);
		const size_t objectCountBeforeRestore =
			world.GetGameObjects().Num();
		Require(editor.InstantiatePrefab(
				detachedSnapshot,
				liveRoot,
				true),
			"strict detached undo should restore below the exact linked parent");
		GameObjectPtr restoredChild =
			world.GetObjectByInstanceId(
				removedLiveChild).
				DynamicCast<GameObject>();
		Require(restoredChild &&
			restoredChild->GetParent() == root &&
			!world.IsPrefabLinked(
				removedLiveChild) &&
			restoredChild->GetComponent<
				PrefabRollbackTestComponent>()->
				GetInstanceId() ==
				removedLiveComponent,
			"detached undo should restore exact object/component ids without relinking the child");
		Require(!editor.InstantiatePrefab(
				detachedSnapshot,
				liveRoot,
				true) &&
			world.GetGameObjects().Num() ==
				objectCountBeforeRestore + 1,
			"a detached strict-id collision should be rejected atomically");

		PrefabPtr linkedSnapshotSource =
			PrefabDocumentTestAsset::Capture(
				world,
				root);
		YAML::Node linkedSnapshotNode =
			linkedSnapshotSource->Serialize();
		::Serialize(
			linkedSnapshotNode,
			"linkedPrefabSnapshot",
			true);
		::Serialize(
			linkedSnapshotNode,
			"fileId",
			sourceFileId);
		::Serialize(
			linkedSnapshotNode,
			"parentInstanceId",
			InstanceId::Invalid);
		::Serialize(
			linkedSnapshotNode,
			"instanceIds",
			evolvedMappings);
		::Serialize(
			linkedSnapshotNode,
			"gameObjectOverrides",
			TMap<InstanceId, YAML::Node>{});
		::Serialize(
			linkedSnapshotNode,
			"componentOverrides",
			componentOverrides);
		PrefabPtr linkedSnapshot =
			DeserializePrefab(
				world,
				YAML::Load(
					YAML::Dump(
						linkedSnapshotNode)));
		Require(linkedSnapshot->
				ValidateForInstantiation(
					diagnostic) &&
			linkedSnapshot->
				IsLinkedPrefabSnapshotRecord() &&
			linkedSnapshot->
				GetLinkedSnapshotSourceFileId() ==
				sourceFileId,
			"the linked-root undo snapshot should retain its validated source metadata: " +
				diagnostic);

		const size_t objectCountBeforeDirectReject =
			world.GetGameObjects().Num();
		Require(!world.Instantiate(
				linkedSnapshot,
				false) &&
			!world.Instantiate(
				linkedSnapshot,
				true) &&
			world.GetGameObjects().Num() ==
				objectCountBeforeDirectReject,
			"linked snapshot markers must never instantiate directly or non-strictly");

		PrefabPtr restoredLinkedRecord =
			PrefabPtr::Make(
				world.GetAllocator(),
				linkedSnapshot->
					GetLinkedSnapshotSourceFileId());
		Require(restoredLinkedRecord->
				ConfigureLinkedInstance(
					evolvedSource,
					linkedSnapshot->
						GetLinkedInstanceIds(),
					linkedSnapshot->
						GetLinkedParentInstanceId(),
					linkedSnapshot->
						GetLinkedGameObjectOverrides(),
					linkedSnapshot->
						GetLinkedComponentOverrides(),
					diagnostic) &&
			restoredLinkedRecord->
				AppendDetachedSupplementalHierarchy(
					linkedSnapshot,
					diagnostic),
			"the strict linked-root restore should resolve source metadata before mutation: " +
				diagnostic);
		Require(!editor.InstantiatePrefab(
				restoredLinkedRecord,
				InstanceId::Invalid,
				true) &&
			world.GetGameObjects().Num() ==
				objectCountBeforeDirectReject,
			"a linked-root exact-id collision should fail before partial mutation");

		world.DestroyImmediate(root);
		Require(editor.InstantiatePrefab(
				restoredLinkedRecord,
				InstanceId::Invalid,
				true),
			"strict linked-root undo should restore source link and supplemental children together");
		GameObjectPtr restoredRoot =
			world.GetObjectByInstanceId(
				liveRoot).
				DynamicCast<GameObject>();
		restoredChild =
			world.GetObjectByInstanceId(
				removedLiveChild).
				DynamicCast<GameObject>();
		Require(restoredRoot &&
			restoredChild &&
			restoredChild->GetParent() ==
				restoredRoot &&
			world.IsPrefabLinked(liveRoot) &&
			!world.IsPrefabLinked(
				removedLiveChild),
			"linked-root undo must relink only mapped source nodes and retain supplemental hierarchy");

		world.Clear();
	}

	void TestRemovingComponentCancelsPendingDependencyResolution()
	{
		YAML::Node meshRendererNode;
		meshRendererNode["typename"] = MeshRendererComponent::GetStaticTypeInfo().Name();
		meshRendererNode["overrideProperties"]["instanceId"] =
			"1111111111111111_10010010010010010000";
		meshRendererNode["overrideProperties"]["model"]["fileId"] = "NullFileId";
		meshRendererNode["overrideProperties"]["model"]["instanceId"] =
			"AAAAAAAAAAAAAAAA_BBBBBBBBBBBBBBBB";

		YAML::Node survivingProperties;
		survivingProperties["m_dependency"]["fileId"] = "NullFileId";
		survivingProperties["m_dependency"]["instanceId"] =
			"CCCCCCCCCCCCCCCC_DDDDDDDDDDDDDDDD";

		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(meshRendererNode);
		components.push_back(MakeReflectedComponent(
			"2222222222222222_10010010010010010000",
			survivingProperties));

		PrefabTestWorld world;
		auto root = world.Instantiate(DeserializePrefab(world, MakeComponentPrefabNode(components)));
		Require(static_cast<bool>(root),
			"the unresolved component fixture should instantiate successfully");
		Require(world.GetPendingDependencyCount() == 2,
			"both unresolved components should enter the pending dependency queue");

		auto meshComponent = root->GetComponent(0);
		auto meshRenderer = meshComponent.DynamicCast<MeshRendererComponent>();
		Require(static_cast<bool>(meshRenderer),
			"the first fixture component should be a mesh renderer");

		auto* meshEcs = world.GetECS<StaticMeshRendererECS>();
		const size_t releasedSlot = meshRenderer->GetComponentIndex();
		Require(meshEcs->IsComponentRegistered(releasedSlot),
			"the unresolved mesh renderer should own a live ECS slot before removal");

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(editor.RemoveComponent(meshComponent->GetInstanceId()),
			"removing the unresolved mesh renderer should succeed");
		Require(!meshEcs->IsComponentRegistered(releasedSlot),
			"removing the mesh renderer should release its ECS slot");
		Require(world.GetPendingDependencyCount() == 1,
			"removing one component should cancel only its pending dependency work");

		auto replacement = root->AddComponent<MeshRendererComponent>();
		Require(replacement->GetComponentIndex() == releasedSlot,
			"the replacement mesh renderer should reuse the released ECS slot");
		world.ResolveExternalDependencies();
		Require(world.GetPendingDependencyCount() == 1,
			"retrying dependencies should preserve the surviving unresolved component");
		Require(meshEcs->IsComponentRegistered(releasedSlot) &&
			replacement->GetComponentIndex() == releasedSlot,
			"stale dependency work must not mutate the replacement mesh renderer slot");
		world.Clear();
	}

	void TestExplicitNullMeshReferenceDoesNotRemainPending()
	{
		YAML::Node meshRendererNode;
		meshRendererNode["typename"] = MeshRendererComponent::GetStaticTypeInfo().Name();
		meshRendererNode["overrideProperties"]["instanceId"] =
			"1111111111111111_10010010010010010000";
		meshRendererNode["overrideProperties"]["model"]["fileId"] = "NullFileId";
		meshRendererNode["overrideProperties"]["model"]["instanceId"] = "NullInstanceId";

		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(meshRendererNode);

		PrefabTestWorld world;
		auto root = world.Instantiate(DeserializePrefab(world, MakeComponentPrefabNode(components)));
		Require(static_cast<bool>(root),
			"the explicit-null mesh renderer fixture should instantiate successfully");
		Require(world.GetPendingDependencyCount() == 0,
			"an explicit null object reference should be resolved instead of retried every frame");
		world.Clear();
	}

	void TestEditorUpdateReplacesStaleMeshDependencyResolution()
	{
		YAML::Node meshRendererNode;
		meshRendererNode["typename"] = MeshRendererComponent::GetStaticTypeInfo().Name();
		meshRendererNode["overrideProperties"]["instanceId"] =
			"1111111111111111_10010010010010010000";
		meshRendererNode["overrideProperties"]["model"]["fileId"] = "NullFileId";
		meshRendererNode["overrideProperties"]["model"]["instanceId"] =
			"AAAAAAAAAAAAAAAA_BBBBBBBBBBBBBBBB";

		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(meshRendererNode);

		PrefabTestWorld world;
		auto survivor = world.Instantiate(DeserializePrefab(world, MakeComponentPrefabNode(components)));
		Require(static_cast<bool>(survivor),
			"the survivor mesh renderer fixture should instantiate successfully");
		Require(world.GetPendingDependencyCount() == 1,
			"the unresolved original model should enter the pending dependency queue");

		auto meshRenderer = survivor->GetComponent<MeshRendererComponent>();
		Require(static_cast<bool>(meshRenderer),
			"the survivor should expose its mesh renderer");

		auto duckOwner = world.Instantiate("DuckOwner");
		auto duckRenderer = duckOwner->AddComponent<MeshRendererComponent>();
		ModelPtr duckModel = ModelPtr::Make(world.GetAllocator(), FileId());
		duckRenderer->SetModel(duckModel);

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(editor.DestroyObject(duckOwner->GetInstanceId()),
			"deleting the original duck owner should succeed");

		YAML::Node updateNode;
		updateNode["typename"] = MeshRendererComponent::GetStaticTypeInfo().Name();
		updateNode["overrideProperties"]["instanceId"] = meshRenderer->GetInstanceId();
		updateNode["overrideProperties"]["model"]["fileId"] = "NullFileId";
		updateNode["overrideProperties"]["model"]["instanceId"] = "NullInstanceId";
		Require(editor.UpdateObject(meshRenderer->GetInstanceId(), YAML::Dump(updateNode)),
			"updating the surviving mesh renderer should succeed");

		meshRenderer->SetModel(duckModel);
		world.ResolveExternalDependencies();
		Require(meshRenderer->GetModel() == duckModel,
			"stale dependency work must not clear a model assigned after deleting another owner");
		Require(world.GetPendingDependencyCount() == 0,
			"the editor update should replace the survivor's stale pending reflection");
		world.Clear();
	}

	void TestEditorUpdatePreservesNewUnresolvedDependency()
	{
		YAML::Node originalProperties;
		originalProperties["m_dependency"]["fileId"] = "NullFileId";
		originalProperties["m_dependency"]["instanceId"] =
			"AAAAAAAAAAAAAAAA_BBBBBBBBBBBBBBBB";

		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(MakeReflectedComponent(
			"1111111111111111_10010010010010010000",
			originalProperties));

		PrefabTestWorld world;
		auto root = world.Instantiate(DeserializePrefab(world, MakeComponentPrefabNode(components)));
		Require(static_cast<bool>(root),
			"the unresolved editor-update fixture should instantiate successfully");
		auto source = root->GetComponent<PrefabRollbackTestComponent>();
		Require(static_cast<bool>(source),
			"the editor-update fixture should expose its reflected component");
		Require(world.GetPendingDependencyCount() == 1,
			"the original unresolved dependency should enter the pending queue");

		InstanceId targetGameObjectId;
		targetGameObjectId.Deserialize(YAML::Node("20020020020020020000"));
		InstanceId targetComponentId;
		targetComponentId.Deserialize(YAML::Node(
			"3333333333333333_20020020020020020000"));

		YAML::Node updatedProperties;
		updatedProperties["m_dependency"]["fileId"] = "NullFileId";
		updatedProperties["m_dependency"]["instanceId"] = targetComponentId.ToString();
		YAML::Node updateNode = MakeReflectedComponent(
			source->GetInstanceId().ToString(),
			updatedProperties);

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(editor.UpdateObject(source->GetInstanceId(), YAML::Dump(updateNode)),
			"updating to a new unresolved dependency should succeed");
		Require(world.GetPendingDependencyCount() == 1,
			"the new unresolved dependency should replace the old pending snapshot");

		auto targetOwner = world.Instantiate("LateTarget", targetGameObjectId);
		Require(static_cast<bool>(targetOwner),
			"the late dependency owner should accept its preferred identity");
		ComponentPtr target = TObjectPtr<PrefabRollbackTestComponent>::Make(world.GetAllocator());
		target = targetOwner->AddComponentRaw(target, targetComponentId);
		Require(static_cast<bool>(target),
			"the late dependency component should accept its preferred identity");

		world.ResolveExternalDependencies();
		Require(source->m_dependency == target,
			"the replacement pending snapshot should resolve the newly selected component");
		Require(world.GetPendingDependencyCount() == 0,
			"the replacement pending snapshot should leave the queue after resolution");
		world.Clear();
	}

	void TestPrefabComponentReferencesFollowRemappedOwners()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		constexpr const char* rootId = "10010010010010010000";
		constexpr const char* childId = "20020020020020020000";
		constexpr const char* sourceComponentId = "1111111111111111_10010010010010010000";
		constexpr const char* targetComponentId = "2222222222222222_20020020020020020000";

		YAML::Node sourceProperties;
		sourceProperties["m_dependency"]["fileId"] = "NullFileId";
		sourceProperties["m_dependency"]["instanceId"] = targetComponentId;

		YAML::Node targetProperties;
		targetProperties["m_value"] = 42.0f;

		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(MakeReflectedComponent(sourceComponentId, sourceProperties));
		components.push_back(MakeReflectedComponent(targetComponentId, targetProperties));

		YAML::Node prefabNode = MakePrefabNode({ noParent, 0 });
		prefabNode["gameObjects"][0]["instanceId"] = rootId;
		prefabNode["gameObjects"][0]["components"] = YAML::Node(YAML::NodeType::Sequence);
		prefabNode["gameObjects"][0]["components"].push_back(0);
		prefabNode["gameObjects"][1]["instanceId"] = childId;
		prefabNode["gameObjects"][1]["components"] = YAML::Node(YAML::NodeType::Sequence);
		prefabNode["gameObjects"][1]["components"].push_back(1);
		prefabNode["components"] = components;

		PrefabTestWorld world;
		auto prefab = DeserializePrefab(world, prefabNode);
		auto firstRoot = world.Instantiate(prefab);
		Require(static_cast<bool>(firstRoot) && firstRoot->GetChildren().Num() == 1,
			"the first saved prefab instance should preserve its hierarchy");

		auto firstSource = firstRoot->GetComponent<PrefabRollbackTestComponent>();
		auto firstTarget = firstRoot->GetChildren()[0]->GetComponent<PrefabRollbackTestComponent>();
		Require(firstSource && firstTarget && firstSource->m_dependency == firstTarget,
			"the first saved prefab instance should resolve its component reference internally");

		auto secondRoot = world.Instantiate(prefab);
		Require(static_cast<bool>(secondRoot) && secondRoot->GetChildren().Num() == 1,
			"a repeated prefab instance should remap colliding game-object identities");

		auto secondSource = secondRoot->GetComponent<PrefabRollbackTestComponent>();
		auto secondTarget = secondRoot->GetChildren()[0]->GetComponent<PrefabRollbackTestComponent>();
		Require(secondSource && secondTarget,
			"the repeated prefab instance should recreate both reflected components");
		Require(secondSource->m_dependency == secondTarget && secondSource->m_dependency != firstTarget,
			"a saved component reference must follow the remapped owner within its prefab instance");
		Require(secondTarget->GetInstanceId().ComponentId().ToString() == "2222222222222222",
			"component remapping must preserve the saved component-local identity");
		Require(secondTarget->GetInstanceId().GameObjectId() == secondRoot->GetChildren()[0]->GetInstanceId(),
			"the remapped component identity must embed its actual game-object owner");
		Require(world.GetPendingDependencyCount() == 0,
			"internally remapped component references must not leak into the pending queue");

		world.Clear();
	}

	void TestStrictPrefabInstantiationPreservesIdsAndRejectsAtomically()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		constexpr const char* rootId =
			"10010010010010010000";
		constexpr const char* childId =
			"20020020020020020000";
		constexpr const char* parentId =
			"30030030030030030000";
		constexpr const char* sourceComponentId =
			"1111111111111111_10010010010010010000";
		constexpr const char* targetComponentId =
			"2222222222222222_20020020020020020000";

		YAML::Node sourceProperties;
		sourceProperties["m_dependency"]["fileId"] =
			"NullFileId";
		sourceProperties["m_dependency"]["instanceId"] =
			targetComponentId;
		YAML::Node targetProperties;
		targetProperties["m_value"] = 42.0f;

		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(MakeReflectedComponent(
			sourceComponentId,
			sourceProperties));
		components.push_back(MakeReflectedComponent(
			targetComponentId,
			targetProperties));

		YAML::Node prefabNode = MakePrefabNode({ noParent, 0 });
		prefabNode["gameObjects"][0]["instanceId"] = rootId;
		prefabNode["gameObjects"][0]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		prefabNode["gameObjects"][0]["components"].push_back(0);
		prefabNode["gameObjects"][1]["instanceId"] = childId;
		prefabNode["gameObjects"][1]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		prefabNode["gameObjects"][1]["components"].push_back(1);
		prefabNode["components"] = components;

		{
			PrefabTestWorld world;
			PrefabPtr prefab = DeserializePrefab(world, prefabNode);
			GameObjectPtr strictRoot =
				world.Instantiate(prefab, true);
			Require(strictRoot &&
				strictRoot->GetInstanceId() ==
					DeserializeInstanceId(rootId) &&
				strictRoot->GetChildren().Num() == 1 &&
				strictRoot->GetChildren()[0]->GetInstanceId() ==
					DeserializeInstanceId(childId),
				"strict prefab instantiate should preserve every saved game object id");

			auto strictSource =
				strictRoot->GetComponent<
					PrefabRollbackTestComponent>();
			auto strictTarget = strictRoot->GetChildren()[0]->
				GetComponent<PrefabRollbackTestComponent>();
			Require(strictSource && strictTarget &&
				strictSource->GetInstanceId() ==
					DeserializeInstanceId(sourceComponentId) &&
				strictTarget->GetInstanceId() ==
					DeserializeInstanceId(targetComponentId) &&
				strictSource->m_dependency == strictTarget,
				"strict prefab instantiate should preserve component identities and internal references");
			Require(world.GetPendingDependencyCount() == 0,
				"strict internal references should resolve without entering the pending queue");

			world.Clear();
		}

		{
			PrefabTestWorld world;
			const InstanceId desiredRootId =
				DeserializeInstanceId(rootId);
			const InstanceId desiredChildId =
				DeserializeInstanceId(childId);
			const InstanceId desiredParentId =
				DeserializeInstanceId(parentId);
			const InstanceId desiredTargetComponentId =
				DeserializeInstanceId(targetComponentId);
			GameObjectPtr existingParent = world.Instantiate(
				"ExistingParent",
				desiredParentId);
			GameObjectPtr childCollision = world.Instantiate(
				"ExistingChildCollision",
				desiredChildId);
			Require(existingParent && childCollision,
				"the strict collision fixture should preserve its requested ids");
			childCollision->SetParent(existingParent);
			ComponentPtr existingComponent =
				TObjectPtr<PrefabRollbackTestComponent>::Make(
					world.GetAllocator());
			existingComponent = childCollision->AddComponentRaw(
				existingComponent,
				desiredTargetComponentId);
			Require(existingComponent &&
				existingParent->GetChildren().Num() == 1,
				"the strict collision fixture should contain the occupied child and component ids");

			PrefabPtr prefab =
				DeserializePrefab(world, prefabNode);
			const size_t objectCountBeforeReject =
				world.GetGameObjects().Num();
			const size_t parentChildCountBeforeReject =
				existingParent->GetChildren().Num();
			const size_t pendingCountBeforeReject =
				world.GetPendingDependencyCount();
			Require(!world.Instantiate(prefab, true),
				"strict prefab instantiate should reject a collision on any saved game object id");
			Require(world.GetGameObjects().Num() ==
					objectCountBeforeReject &&
				existingParent->GetChildren().Num() ==
					parentChildCountBeforeReject &&
				childCollision->GetParent() == existingParent &&
				childCollision->GetComponent(0) ==
					existingComponent &&
				world.GetPendingDependencyCount() ==
					pendingCountBeforeReject &&
				!world.GetObjectByInstanceId(desiredRootId),
				"a strict child collision must be rejected before the first world, parent, component, or dependency mutation");

			GameObjectPtr remappedRoot =
				world.Instantiate(prefab);
			Require(remappedRoot &&
				remappedRoot->GetInstanceId() ==
					desiredRootId &&
				remappedRoot->GetChildren().Num() == 1 &&
				remappedRoot->GetChildren()[0]->GetInstanceId() !=
					desiredChildId,
				"ordinary prefab instantiate should continue remapping only colliding saved ids");
			auto remappedSource =
				remappedRoot->GetComponent<
					PrefabRollbackTestComponent>();
			auto remappedTarget =
				remappedRoot->GetChildren()[0]->
					GetComponent<PrefabRollbackTestComponent>();
			Require(remappedSource && remappedTarget &&
				remappedSource->m_dependency == remappedTarget &&
				remappedTarget->GetInstanceId().ComponentId() ==
					desiredTargetComponentId.ComponentId() &&
				remappedTarget->GetInstanceId().GameObjectId() ==
					remappedRoot->GetChildren()[0]->
						GetInstanceId(),
				"ordinary remapping should retain component-local identity and follow the remapped owner");

			world.Clear();
		}
	}

	void TestEditorPrefabInstantiationRejectsLinkedParentBeforeMutation()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		constexpr const char* sourceParentId =
			"10010010010010010000";
		constexpr const char* liveParentId =
			"20020020020020020000";
		constexpr const char* childRootId =
			"30030030030030030000";
		constexpr const char* childComponentId =
			"1111111111111111_30030030030030030000";
		const FileId sourceFileId =
			DeserializeFileId(
				"{11111111-2222-3333-4444-555555555555}");

		PrefabTestWorld world;
		YAML::Node parentSourceNode =
			MakePrefabNode({ noParent });
		parentSourceNode["gameObjects"][0]["instanceId"] =
			sourceParentId;
		PrefabPtr parentSource = DeserializePrefab(
			world,
			sourceFileId,
			parentSourceNode);

		TMap<InstanceId, InstanceId> parentMappings;
		parentMappings[DeserializeInstanceId(sourceParentId)] =
			DeserializeInstanceId(liveParentId);
		std::string diagnostic;
		PrefabPtr linkedParentPrefab =
			PrefabPtr::Make(world.GetAllocator(), sourceFileId);
		Require(linkedParentPrefab->ConfigureLinkedInstance(
				parentSource,
				parentMappings,
				InstanceId::Invalid,
				{},
				{},
				diagnostic),
			"the linked parent fixture should configure: " +
				diagnostic);
		GameObjectPtr linkedParent =
			world.Instantiate(linkedParentPrefab);
		Require(linkedParent &&
			world.IsPrefabLinked(linkedParent->GetInstanceId()),
			"the editor parent rejection fixture should be linked");

		YAML::Node unresolvedProperties;
		unresolvedProperties["m_dependency"]["fileId"] =
			"NullFileId";
		unresolvedProperties["m_dependency"]["instanceId"] =
			"9999999999999999_AAAAAAAAAAAAAAAAAAAA";
		YAML::Node childComponents(YAML::NodeType::Sequence);
		childComponents.push_back(MakeReflectedComponent(
			childComponentId,
			unresolvedProperties));
		YAML::Node childNode = MakePrefabNode({ noParent });
		childNode["gameObjects"][0]["instanceId"] =
			childRootId;
		childNode["gameObjects"][0]["components"] =
			YAML::Node(YAML::NodeType::Sequence);
		childNode["gameObjects"][0]["components"].push_back(0);
		childNode["components"] = childComponents;
		PrefabPtr childPrefab =
			DeserializePrefab(world, childNode);

		const size_t objectCountBeforeReject =
			world.GetGameObjects().Num();
		const size_t linkCountBeforeReject =
			world.GetPrefabInstances().Num();
		const size_t pendingCountBeforeReject =
			world.GetPendingDependencyCount();
		const size_t parentChildCountBeforeReject =
			linkedParent->GetChildren().Num();

		Editor editor(nullptr, 0, nullptr);
		editor.SetWorld(&world);
		Require(!editor.InstantiatePrefab(
				childPrefab,
				linkedParent->GetInstanceId()),
			"editor prefab instantiate should reject parenting inside a linked prefab");
		Require(world.GetGameObjects().Num() ==
				objectCountBeforeReject &&
			world.GetPrefabInstances().Num() ==
				linkCountBeforeReject &&
			world.GetPendingDependencyCount() ==
				pendingCountBeforeReject &&
			linkedParent->GetChildren().Num() ==
				parentChildCountBeforeReject &&
			!world.GetObjectByInstanceId(
				DeserializeInstanceId(childRootId)),
			"linked-parent rejection must happen before object, link, hierarchy, or dependency mutation");

		world.Clear();
	}

	void RequireRejectedWithoutWorldMutation(
		PrefabTestWorld& world,
		const YAML::Node& prefabNode,
		const std::string& message)
	{
		const size_t initialObjectCount = world.GetGameObjects().Num();
		const size_t initialPendingDependencyCount = world.GetPendingDependencyCount();
		PrefabPtr prefab = DeserializePrefab(world, prefabNode);

		Require(!world.Instantiate(prefab), message + " should be rejected");
		Require(world.GetGameObjects().Num() == initialObjectCount,
			message + " must leave the world object count unchanged");
		Require(world.GetPendingDependencyCount() == initialPendingDependencyCount,
			message + " must leave the pending dependency queue unchanged");
	}

	void TestPrefabTopologyValidationRejectsPartialWorldMutations()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		std::string diagnostic;

		Prefab validPrefab{ FileId() };
		validPrefab.Deserialize(MakePrefabNode({ noParent, 0 }));
		Require(validPrefab.ValidateForInstantiation(diagnostic),
			"a single-root acyclic prefab should pass pre-mutation validation: " + diagnostic);

		Prefab multipleRoots{ FileId() };
		multipleRoots.Deserialize(MakePrefabNode({ noParent, noParent }));
		Require(!multipleRoots.ValidateForInstantiation(diagnostic) && diagnostic.find("exactly one root") != std::string::npos,
			"multiple roots must be rejected before any world objects are created");

		Prefab cyclicHierarchy{ FileId() };
		cyclicHierarchy.Deserialize(MakePrefabNode({ noParent, 2, 1 }));
		Require(!cyclicHierarchy.ValidateForInstantiation(diagnostic) && diagnostic.find("cycle") != std::string::npos,
			"a detached parent cycle must be rejected before world mutation");

		Prefab invalidComponentReference{ FileId() };
		invalidComponentReference.Deserialize(MakePrefabNode({ noParent }, true));
		Require(!invalidComponentReference.ValidateForInstantiation(diagnostic) && diagnostic.find("component index") != std::string::npos,
			"out-of-range component references must be rejected before world mutation");

		Prefab missingParentIndex{ FileId() };
		missingParentIndex.Deserialize(MakePrefabNode({ noParent }, false, false));
		Require(!missingParentIndex.ValidateForInstantiation(diagnostic) && diagnostic.find("parentIndex") != std::string::npos,
			"a missing parentIndex must be rejected instead of reading uninitialized hierarchy state");

		YAML::Node malformedGameObjectIdNode = MakePrefabNode({ noParent });
		malformedGameObjectIdNode["gameObjects"][0]["instanceId"] = "truthy-but-not-a-direct-id";
		Prefab malformedGameObjectId{ FileId() };
		malformedGameObjectId.Deserialize(malformedGameObjectIdNode);
		Require(!malformedGameObjectId.ValidateForInstantiation(diagnostic) && diagnostic.find("invalid instanceId") != std::string::npos,
			"a malformed truthy game-object instanceId must be rejected before world mutation");

		YAML::Node duplicateGameObjectIdNode = MakePrefabNode({ noParent, 0 });
		duplicateGameObjectIdNode["gameObjects"][1]["instanceId"] =
			duplicateGameObjectIdNode["gameObjects"][0]["instanceId"];
		Prefab duplicateGameObjectId{ FileId() };
		duplicateGameObjectId.Deserialize(duplicateGameObjectIdNode);
		Require(!duplicateGameObjectId.ValidateForInstantiation(diagnostic) && diagnostic.find("duplicate instanceId") != std::string::npos,
			"duplicate original game-object instanceIds must be rejected before dependency remapping");

		YAML::Node validProperties;
		validProperties["m_value"] = 1.0f;
		YAML::Node validComponents(YAML::NodeType::Sequence);
		validComponents.push_back(MakeReflectedComponent(
			"1111111111111111_10010010010010010000",
			validProperties));

		YAML::Node duplicateComponentReferenceNode = MakeComponentPrefabNode(validComponents);
		duplicateComponentReferenceNode["gameObjects"][0]["components"].push_back(0);
		Prefab duplicateComponentReference{ FileId() };
		duplicateComponentReference.Deserialize(duplicateComponentReferenceNode);
		Require(!duplicateComponentReference.ValidateForInstantiation(diagnostic) && diagnostic.find("referenced more than once") != std::string::npos,
			"a reflected component must not be instantiated more than once");

		YAML::Node orphanComponentNode = MakeComponentPrefabNode(validComponents);
		orphanComponentNode["gameObjects"][0]["components"] = YAML::Node(YAML::NodeType::Sequence);
		Prefab orphanComponent{ FileId() };
		orphanComponent.Deserialize(orphanComponentNode);
		Require(!orphanComponent.ValidateForInstantiation(diagnostic) && diagnostic.find("unreferenced") != std::string::npos,
			"an orphan reflected component must be rejected before world mutation");

		YAML::Node mismatchedComponentOwnerNode = MakeComponentPrefabNode(validComponents);
		mismatchedComponentOwnerNode["components"][0]["overrideProperties"]["instanceId"] =
			"1111111111111111_20020020020020020000";
		Prefab mismatchedComponentOwner{ FileId() };
		mismatchedComponentOwner.Deserialize(mismatchedComponentOwnerNode);
		Require(!mismatchedComponentOwner.ValidateForInstantiation(diagnostic) && diagnostic.find("different game object") != std::string::npos,
			"a reflected component must belong to the game object that references it");
	}

	void TestPrefabInstantiationRollbackPreservesWorldState()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		PrefabTestWorld world;
		Require(static_cast<bool>(world.Instantiate("ExistingObject")),
			"the rollback fixture should contain an existing object");

		RequireRejectedWithoutWorldMutation(
			world,
			MakePrefabNode({ noParent, noParent }),
			"a multiple-root prefab");
		RequireRejectedWithoutWorldMutation(
			world,
			MakePrefabNode({ noParent, 2, 1 }),
			"a cyclic prefab hierarchy");
		RequireRejectedWithoutWorldMutation(
			world,
			MakePrefabNode({ noParent }, false, false),
			"a prefab with a missing parentIndex");

		YAML::Node malformedGameObjectIdNode = MakePrefabNode({ noParent });
		malformedGameObjectIdNode["gameObjects"][0]["instanceId"] = "truthy-but-not-a-direct-id";
		RequireRejectedWithoutWorldMutation(
			world,
			malformedGameObjectIdNode,
			"a prefab with a malformed game-object instanceId");

		YAML::Node duplicateGameObjectIdNode = MakePrefabNode({ noParent, 0 });
		duplicateGameObjectIdNode["gameObjects"][1]["instanceId"] =
			duplicateGameObjectIdNode["gameObjects"][0]["instanceId"];
		RequireRejectedWithoutWorldMutation(
			world,
			duplicateGameObjectIdNode,
			"a prefab with duplicate game-object instanceIds");

		YAML::Node missingIdComponents(YAML::NodeType::Sequence);
		YAML::Node validProperties;
		validProperties["m_value"] = 1.0f;
		missingIdComponents.push_back(MakeReflectedComponent("", validProperties, false));
		RequireRejectedWithoutWorldMutation(
			world,
			MakeComponentPrefabNode(missingIdComponents),
			"a reflected component with a missing instanceId");

		YAML::Node duplicateIdComponents(YAML::NodeType::Sequence);
		duplicateIdComponents.push_back(MakeReflectedComponent(
			"4444444444444444_10010010010010010000",
			validProperties));
		duplicateIdComponents.push_back(MakeReflectedComponent(
			"4444444444444444_10010010010010010000",
			validProperties));
		RequireRejectedWithoutWorldMutation(
			world,
			MakeComponentPrefabNode(duplicateIdComponents),
			"reflected components with duplicate full instanceIds");

		YAML::Node validComponent(YAML::NodeType::Sequence);
		validComponent.push_back(MakeReflectedComponent(
			"1111111111111111_10010010010010010000",
			validProperties));

		YAML::Node duplicateComponentReferenceNode = MakeComponentPrefabNode(validComponent);
		duplicateComponentReferenceNode["gameObjects"][0]["components"].push_back(0);
		RequireRejectedWithoutWorldMutation(
			world,
			duplicateComponentReferenceNode,
			"a prefab that references one reflected component more than once");

		YAML::Node orphanComponentNode = MakeComponentPrefabNode(validComponent);
		orphanComponentNode["gameObjects"][0]["components"] = YAML::Node(YAML::NodeType::Sequence);
		RequireRejectedWithoutWorldMutation(
			world,
			orphanComponentNode,
			"a prefab with an orphan reflected component");

		YAML::Node mismatchedComponentOwnerNode = MakeComponentPrefabNode(validComponent);
		mismatchedComponentOwnerNode["components"][0]["overrideProperties"]["instanceId"] =
			"1111111111111111_20020020020020020000";
		RequireRejectedWithoutWorldMutation(
			world,
			mismatchedComponentOwnerNode,
			"a prefab with a reflected component owned by another game object");

		YAML::Node malformedValueComponents(YAML::NodeType::Sequence);
		YAML::Node malformedValueProperties;
		malformedValueProperties["m_value"] = "not-a-float";
		malformedValueComponents.push_back(MakeReflectedComponent(
			"1111111111111111_10010010010010010000",
			malformedValueProperties));
		RequireRejectedWithoutWorldMutation(
			world,
			MakeComponentPrefabNode(malformedValueComponents),
			"a reflected component with a malformed scalar property");

		YAML::Node lateFailureComponents(YAML::NodeType::Sequence);
		YAML::Node unresolvedProperties;
		unresolvedProperties["m_dependency"]["fileId"] = "NullFileId";
		unresolvedProperties["m_dependency"]["instanceId"] =
			"AAAAAAAAAAAAAAAA_BBBBBBBBBBBBBBBB";

		YAML::Node unresolvedComponents(YAML::NodeType::Sequence);
		unresolvedComponents.push_back(MakeReflectedComponent(
			"5555555555555555_10010010010010010000",
			unresolvedProperties));
		const size_t initialObjectCount = world.GetGameObjects().Num();
		const size_t initialPendingDependencyCount = world.GetPendingDependencyCount();
		GameObjectPtr unresolvedRoot = world.Instantiate(DeserializePrefab(
			world,
			MakeComponentPrefabNode(unresolvedComponents)));
		Require(static_cast<bool>(unresolvedRoot),
			"the unresolved dependency fixture should instantiate successfully");
		Require(world.GetPendingDependencyCount() == initialPendingDependencyCount + 1,
			"an unresolved reflected dependency should enter the pending queue");
		world.DestroyImmediate(unresolvedRoot);
		Require(world.GetGameObjects().Num() == initialObjectCount,
			"destroying the unresolved dependency fixture should restore the object count");
		Require(world.GetPendingDependencyCount() == initialPendingDependencyCount,
			"destroying the unresolved dependency fixture should restore the pending queue");

		lateFailureComponents.push_back(MakeReflectedComponent(
			"2222222222222222_10010010010010010000",
			unresolvedProperties));

		YAML::Node malformedReferenceProperties;
		malformedReferenceProperties["m_dependency"] = "not-an-object-reference";
		lateFailureComponents.push_back(MakeReflectedComponent(
			"3333333333333333_10010010010010010000",
			malformedReferenceProperties));
		RequireRejectedWithoutWorldMutation(
			world,
			MakeComponentPrefabNode(lateFailureComponents),
			"a late malformed reference after queuing an unresolved dependency");

		world.Clear();
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "ComponentSlotsAreResetAndFreedOnce", TestComponentSlotsAreResetAndFreedOnce },
		{ "PreferredEditorInstanceIdsArePreserved", TestPreferredEditorInstanceIdsArePreserved },
		{ "TransformParentCleanupPreservesPendingReparent", TestTransformParentCleanupPreservesPendingReparent },
		{ "EditorKeepWorldReparentUsesCurrentTransforms", TestEditorKeepWorldReparentUsesCurrentTransforms },
		{ "EditorKeepWorldReparentRejectsSingularParentWithoutMutation", TestEditorKeepWorldReparentRejectsSingularParentWithoutMutation },
		{ "EditorKeepWorldReparentPreservesMirroredTransform", TestEditorKeepWorldReparentPreservesMirroredTransform },
		{ "EditorKeepWorldReparentRejectsShearedCandidateWithoutMutation", TestEditorKeepWorldReparentRejectsShearedCandidateWithoutMutation },
		{ "OctreeRelocationPreservesElementCount", TestOctreeRelocationPreservesElementCount },
		{ "ClearingMeshModelAlsoClearsMaterials", TestClearingMeshModelAlsoClearsMaterials },
		{ "AnimationGpuBoneLayoutContract", TestAnimationGpuBoneLayoutContract },
		{ "ExpiredWorldPrefabInvalidatesLoadedCacheContract", TestExpiredWorldPrefabInvalidatesLoadedCacheContract },
		{ "EmptyEditorWorldBootstrapContract", TestEmptyEditorWorldBootstrapContract },
		{ "WorkspaceEditorStartupWorldContract", TestWorkspaceEditorStartupWorldContract },
		{ "AnimationRelayoutMarksEveryOwnedMeshDirty", TestAnimationRelayoutMarksEveryOwnedMeshDirty },
		{ "SparseLightSlotInvalidationAndReuse", TestSparseLightSlotInvalidationAndReuse },
		{ "RemovingComponentCancelsPendingDependencyResolution", TestRemovingComponentCancelsPendingDependencyResolution },
		{ "ExplicitNullMeshReferenceDoesNotRemainPending", TestExplicitNullMeshReferenceDoesNotRemainPending },
		{ "EditorUpdateReplacesStaleMeshDependencyResolution", TestEditorUpdateReplacesStaleMeshDependencyResolution },
		{ "EditorUpdatePreservesNewUnresolvedDependency", TestEditorUpdatePreservesNewUnresolvedDependency },
		{ "LegacyPrefabApiSymbolsRemainAddressable", TestLegacyPrefabApiSymbolsRemainAddressable },
		{ "PrefabComponentReferencesFollowRemappedOwners", TestPrefabComponentReferencesFollowRemappedOwners },
		{ "LinkedPrefabPersistenceAndWorldContract", TestLinkedPrefabPersistenceAndWorldContract },
		{ "LinkedPrefabBaselineAndSaveFailureContract", TestLinkedPrefabBaselineAndSaveFailureContract },
		{ "LinkedPrefabSourceStructureEvolutionContract", TestLinkedPrefabSourceStructureEvolutionContract },
		{ "LinkedPrefabMembershipFollowsEvolvedSourceMapping", TestLinkedPrefabMembershipFollowsEvolvedSourceMapping },
		{ "PrefabRootFileIdRemainsAuthoritativeWhenDerivedMetadataIsMissing", TestPrefabRootFileIdRemainsAuthoritativeWhenDerivedMetadataIsMissing },
		{ "DetachedSupplementalPrefabPersistenceAndStrictRestore", TestDetachedSupplementalPrefabPersistenceAndStrictRestore },
		{ "StrictPrefabInstantiationPreservesIdsAndRejectsAtomically", TestStrictPrefabInstantiationPreservesIdsAndRejectsAtomically },
		{ "EditorPrefabInstantiationRejectsLinkedParentBeforeMutation", TestEditorPrefabInstantiationRejectsLinkedParentBeforeMutation },
		{ "PrefabTopologyValidationRejectsPartialWorldMutations", TestPrefabTopologyValidationRejectsPartialWorldMutations },
		{ "PrefabInstantiationRollbackPreservesWorldState", TestPrefabInstantiationRollbackPreservesWorldState },
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
