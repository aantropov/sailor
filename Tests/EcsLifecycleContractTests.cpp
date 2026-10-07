#include "Components/AnimatorComponent.h"
#include "Components/LandscapeComponent.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/PhysicsECS.h"
#include "Submodules/Editor.h"
#include "Support/EcsTestFixtures.h"
#include "Support/PrefabTestDocument.h"
#include "Support/LifecyclePrefab.h"
#include "Support/ScopeExit.h"

#include <functional>
#include <iostream>
#include <limits>
#include <utility>

using namespace Sailor;
using namespace Sailor::Tests;

namespace
{
	constexpr EWorldBehaviourMask GameplayMask =
		(uint8_t)EWorldBehaviourBit::CallBeginPlay | (uint8_t)EWorldBehaviourBit::Tickable;

	glm::mat4 CalculateCurrentWorldMatrix(GameObjectPtr gameObject)
	{
		glm::mat4 worldMatrix = glm::identity<glm::mat4>();
		for (auto current = gameObject; current.IsValid(); current = current->GetParent())
		{
			worldMatrix = current->GetTransformComponent().GetTransform().Matrix() * worldMatrix;
		}

		return worldMatrix;
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

		void Tick(float deltaTime) override {}

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

	class TransformClearTestSystem final : public TransformECS
	{
	public:
		size_t GetNumSlotsForTest() const { return m_components.Num(); }
		size_t GetNumDirtyForTest() const { return m_dirtyComponents.Num(); }
		void ResetRemovalVisits() { m_numRemovalVisits = 0; }
		size_t GetRemovalVisits() const { return m_numRemovalVisits; }
		size_t GetNumPendingParentsForTest() const { return m_pendingChildren.Num(); }
	};

	class BulkClearTestWorld final : public World
	{
	public:
		BulkClearTestWorld() : World("BulkClearTests", 0, CreateEcs()) {}

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformClearTestSystem>::Make());
			systems.Add(TUniquePtr<AnimationLayoutTestSystem>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			systems.Add(TUniquePtr<LandscapeECS>::Make());
			systems.Add(TUniquePtr<PhysicsECS>::Make());
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

	void TestTransformRotationIsCanonicalBeforePublication()
	{
		PrefabTestWorld world;
		auto object = world.Instantiate("Rotation owner");
		auto& transform = object->GetTransformComponent();
		auto* transforms = world.GetECS<TransformECS>();
		transforms->Tick(0.0f);
		transforms->PostTick();
		const auto rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
		world.AdvanceFrame();
		transform.SetRotation(rotation * 5.0f);
		Require(transform.IsDirty() && transform.GetFrameLastChange() == world.GetCurrentFrame(),
			"a changed rotation must publish a dirty transform at the current frame");
		Require(std::abs(glm::length(transform.GetRotation()) - 1.0f) < 0.00001f &&
			std::abs(glm::dot(rotation, transform.GetRotation())) > 0.99999f,
			"ECS rotation must be normalized before Tick or matrix readers run");
		transforms->Tick(0.0f);
		transforms->PostTick();
		Require(!transform.IsDirty() && AreMatricesNear(transform.GetCachedWorldMatrix(), glm::mat4_cast(rotation)),
			"Tick must publish the normalized rotation without changing its orientation");
		const auto changedFrame = transform.GetFrameLastChange();
		world.AdvanceFrame();
		transform.SetRotation(transform.GetRotation());
		Require(!transform.IsDirty() && transform.GetFrameLastChange() == changedFrame,
			"assigning the stored rotation must not publish an artificial change");
		transform.SetRotation(glm::quat(0, 0, 0, 0));
		Require(transform.IsDirty() && transform.GetRotation() == Math::quat_Identity,
			"invalid authored rotation must recover to identity at the ECS boundary");
		transforms->Tick(0.0f);
		transforms->PostTick();
		const auto identityFrame = transform.GetFrameLastChange();
		world.AdvanceFrame();
		transform.SetRotation(glm::quat(std::numeric_limits<float>::infinity(), 0, 0, 0));
		Require(!transform.IsDirty() && transform.GetFrameLastChange() == identityFrame,
			"input recovering to the stored identity must not dirty the transform");
		world.Clear();
	}

	void TestPrefabRotationUsesCanonicalTransformBoundary()
	{
		PrefabTestWorld world;
		YAML::Node document = MakeLifecyclePrefabDocument();
		Prefab::ReflectedGameObject authored;
		authored.Deserialize(document["gameObjects"][0]);
		const auto rotation = glm::angleAxis(0.8f, glm::vec3(0, 1, 0));
		authored.m_rotation = rotation * 7.0f;
		document["gameObjects"][0] = authored.Serialize();
		auto prefab = DeserializePrefab(world, document);
		auto root = world.Instantiate(prefab);
		Require(root && std::abs(glm::length(root->GetTransformComponent().GetRotation()) - 1.0f) < 0.00001f,
			"prefab hydration must store a unit rotation before any world update");
		Require(std::abs(glm::dot(root->GetTransformComponent().GetRotation(), rotation)) > 0.99999f,
			"prefab hydration must retain the authored orientation");
		auto captured = PrefabDocumentTestAsset::Capture(world, root);
		Require(captured && captured->IsReady(), "canonical transform must remain serializable");
		Prefab::ReflectedGameObject serialized;
		serialized.Deserialize(captured->Serialize()["gameObjects"][0]);
		Require(std::abs(glm::length(serialized.m_rotation) - 1.0f) < 0.00001f &&
			std::abs(glm::dot(serialized.m_rotation, rotation)) > 0.99999f,
			"prefab capture must serialize the canonical rotation, not the authored magnitude");
		world.Clear();
	}

	void TestTransformRemovalWorkIsLocal()
	{
		for (uint32_t unrelatedCount : { 0u, 4096u })
		{
			for (uint32_t count : { 32u, 64u })
			{
				BulkClearTestWorld world;
				Tests::ScopeExit cleanup([&]() { world.Clear(); });
				auto* transforms = world.GetECS<TransformClearTestSystem>();
				for (uint32_t i = 0; i < unrelatedCount; ++i)
					world.Instantiate("Unrelated")->GetTransformComponent().SetPosition(glm::vec3(3));
				auto first = world.Instantiate("Applied parent");
				auto second = world.Instantiate("Pending parent");
				first->GetTransformComponent().SetPosition(glm::vec3(10, 0, 0));
				second->GetTransformComponent().SetPosition(glm::vec3(20, 0, 0));
				auto survivor = world.Instantiate("Surviving child");
				survivor->GetTransformComponent().SetPosition(glm::vec3(1, 0, 0));
				survivor->SetParent(first);
				TVector<GameObjectPtr> removed;
				for (uint32_t i = 0; i < count; ++i)
				{
					auto child = world.Instantiate("Removed child");
					child->SetParent(first);
					removed.Add(child);
				}
				transforms->Tick(0);
				for (auto& child : removed) child->SetParent(second);
				transforms->ResetRemovalVisits();
				// Remove from the middle too, not just the back of the packed lists.
				for (uint32_t parity : { 0u, 1u })
					for (uint32_t i = parity; i < count; i += 2) world.DestroyImmediate(removed[i]);
				const size_t visits = transforms->GetRemovalVisits();
				std::cout << "Transform removal: " << count << " children, " << unrelatedCount
					<< " unrelated objects, " << visits << " examined entries\n";
				Require(visits > 0 && visits <= 12 * count,
					"Transform unlink work must scale with removed relationships, not total world or dirty-list size");
				for (uint32_t i = 0; i < count; ++i)
					world.Instantiate("Reused slot")->GetTransformComponent().SetPosition(glm::vec3(100, 0, 0));
				transforms->Tick(0);
				const auto survivorIndex = transforms->GetComponentIndex(&survivor->GetTransformComponent());
				Require(first->GetTransformComponent().GetChildren().Num() == 1 &&
					first->GetTransformComponent().GetChildren()[0] == survivorIndex &&
					second->GetTransformComponent().GetChildren().IsEmpty() &&
					survivor->GetTransformComponent().GetWorldPosition().x == 11,
					"Removal and slot reuse must retain surviving transforms without stale children");
				Require(transforms->GetNumDirtyForTest() == 0, "Both Tick paths must drain the dirty queue");
			}
		}
	}

	void TestTransformParentCleanupPreservesPendingReparent()
	{
		for (bool deferred : { false, true })
		{
			PrefabTestWorld world;
			auto previousParent = world.Instantiate("PreviousParent");
			auto nextParent = world.Instantiate("NextParent");
			auto child = world.Instantiate("Child");
			auto* transforms = world.GetECS<TransformECS>();
			previousParent->GetTransformComponent().SetPosition(glm::vec3(10.0f, 0.0f, 0.0f));
			nextParent->GetTransformComponent().SetPosition(glm::vec3(20.0f, 0.0f, 0.0f));
			child->GetTransformComponent().SetPosition(glm::vec3(1.0f, 0.0f, 0.0f));

			child->SetParent(previousParent);
			transforms->Tick(0.0f);
			transforms->PostTick();

			const size_t previousParentIndex = transforms->GetComponentIndex(&previousParent->GetTransformComponent());
			const size_t nextParentIndex = transforms->GetComponentIndex(&nextParent->GetTransformComponent());
			Require(child->GetTransformComponent().GetParent() == previousParentIndex &&
				child->GetTransformComponent().GetWorldPosition().x == 11.0f,
				"the transform fixture should establish its original parent before reparenting");

			child->SetParent(nextParent);
			if (deferred)
			{
				world.Destroy(previousParent);
				world.DestroyPendingGameObjects();
			}
			else
			{
				world.DestroyImmediate(previousParent);
			}

			auto replacement = world.Instantiate("ReusedParentSlot");
			Require(transforms->GetComponentIndex(&replacement->GetTransformComponent()) == previousParentIndex,
				"the fixture must reuse the released parent transform slot");
			replacement->GetTransformComponent().SetPosition(glm::vec3(100.0f, 0.0f, 0.0f));
			transforms->Tick(0.0f);
			transforms->PostTick();

			Require(static_cast<bool>(child), "reparenting away should keep the child alive when its previous parent is destroyed");
			Require(child->GetParent() == nextParent && nextParent->GetChildren().Num() == 1 &&
				nextParent->GetChildren()[0] == child,
				"both destruction paths must preserve the requested game-object parent");
			Require(child->GetTransformComponent().GetParent() == nextParentIndex &&
				child->GetTransformComponent().GetWorldPosition().x == 21.0f,
				"transform cleanup must preserve the pending reparent across old-parent slot reuse");
			Require(!replacement->GetParent() && replacement->GetChildren().IsEmpty() &&
				replacement->GetTransformComponent().GetParent() == ECS::InvalidIndex &&
				replacement->GetTransformComponent().GetChildren().IsEmpty(),
				"a reused transform slot must not inherit the deleted parent's hierarchy");

			world.Clear();
		}
	}

	void TestDirectTransformRemovalPreservesPendingEdges()
	{
		for (uint32_t unrelatedCount : { 0u, 256u })
		{
			for (uint32_t removalOrder = 0; removalOrder < 4; ++removalOrder)
			{
				BulkClearTestWorld world;
				Tests::ScopeExit cleanup([&]() { world.Clear(); });
				auto* transforms = world.GetECS<TransformClearTestSystem>();
				for (uint32_t i = 0; i < unrelatedCount; ++i) world.Instantiate("Unrelated");
				auto previous = world.Instantiate("Previous parent");
				auto next = world.Instantiate("Next parent");
				auto child = world.Instantiate("Direct transform child");
				auto grandchild = world.Instantiate("Direct transform grandchild");
				previous->GetTransformComponent().SetPosition(glm::vec3(10, 0, 0));
				next->GetTransformComponent().SetPosition(glm::vec3(20, 0, 0));
				child->GetTransformComponent().SetPosition(glm::vec3(1, 0, 0));
				grandchild->GetTransformComponent().SetPosition(glm::vec3(2, 0, 0));
				child->GetTransformComponent().SetNewParent(&previous->GetTransformComponent());
				grandchild->GetTransformComponent().SetNewParent(&child->GetTransformComponent());
				transforms->Tick(0);
				const size_t previousIndex = transforms->GetComponentIndex(&previous->GetTransformComponent());
				const size_t nextIndex = transforms->GetComponentIndex(&next->GetTransformComponent());
				Require(!child->GetParent() && grandchild->GetTransformComponent().GetWorldPosition().x == 13,
					"Direct transform links must work without borrowing the GameObject hierarchy");
				child->GetTransformComponent().SetNewParent(&next->GetTransformComponent());
				child->GetTransformComponent().SetNewParent(&previous->GetTransformComponent());
				Require(transforms->GetNumPendingParentsForTest() == 0,
					"Cancelling a pending move must remove its reverse link");
				child->GetTransformComponent().SetNewParent(&next->GetTransformComponent());
				child->GetTransformComponent().SetNewParent(&next->GetTransformComponent());
				Require(child->GetTransformComponent().GetParent() == previousIndex &&
					previous->GetTransformComponent().GetChildren().Num() == 1 &&
					next->GetTransformComponent().GetChildren().IsEmpty() &&
					child->GetTransformComponent().GetWorldPosition().x == 11,
					"A requested parent must not change published hierarchy or matrices before Tick");
				if (removalOrder == 0 || removalOrder == 2) world.DestroyImmediate(previous);
				if (removalOrder != 0) world.DestroyImmediate(next);
				if (removalOrder == 3) world.DestroyImmediate(previous);
				const bool bPreviousRemoved = removalOrder != 1;
				Require(child && grandchild && child->GetTransformComponent().GetParent() ==
					(bPreviousRemoved ? ECS::InvalidIndex : previousIndex),
					"Removing either parent must clear only relationships targeting that slot");
				for (uint32_t i = 0; i < (removalOrder >= 2 ? 2u : 1u); ++i)
				{
					auto replacement = world.Instantiate("Reused parent slot");
					const size_t index = transforms->GetComponentIndex(&replacement->GetTransformComponent());
					Require(index == previousIndex || index == nextIndex, "The fixture must reuse a removed parent slot");
					replacement->GetTransformComponent().SetPosition(glm::vec3(100, 0, 0));
				}
				transforms->Tick(0);
				const bool bNextSurvives = removalOrder == 0;
				Require(child->GetTransformComponent().GetParent() == (bNextSurvives ? nextIndex : ECS::InvalidIndex) &&
					child->GetTransformComponent().GetWorldPosition().x == (bNextSurvives ? 21 : 1) &&
					grandchild->GetTransformComponent().GetWorldPosition().x == (bNextSurvives ? 23 : 3),
					"Pending moves and descendant matrices must survive either removal order and slot reuse");
				Require(transforms->GetNumDirtyForTest() == 0 && transforms->GetNumPendingParentsForTest() == 0,
					"Publishing a hierarchy must consume pending links and dirty entries");
				world.Clear();
				Require(transforms->GetNumSlotsForTest() == 0 && transforms->GetNumPendingParentsForTest() == 0,
					"Clear must retire reverse links together with component storage");
			}
		}
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

	void TestWorldClearBatchesHierarchyAndStorageCleanup()
	{
		for (uint32_t count : { 32u, 64u })
		{
			BulkClearTestWorld world;
			auto* transforms = world.GetECS<TransformClearTestSystem>();
			auto* animations = world.GetECS<AnimationLayoutTestSystem>();
			TVector<GameObjectPtr> objects;
			for (uint32_t index = 0; index < count; ++index)
			{
				objects.Add(world.Instantiate("Child created before parent"));
			}
			auto parent = world.Instantiate("Wide hierarchy root");
			for (auto& child : objects)
			{
				child->SetParent(parent);
			}
			objects.Add(parent);
			for (uint32_t index = 0; index < count; ++index)
			{
				objects.Add(world.Instantiate("Independent root"));
			}

			TVector<InstanceId> ended;
			size_t dirtyCount = 0;
			for (auto& object : objects)
			{
				object->GetTransformComponent().SetPosition(glm::vec3(3.0f));
				object->AddComponent<AnimatorComponent>()->GetData().SetBonesCount(1);
				auto observer = object->AddComponent<LifecycleTestComponent>();
				observer->SetValue(2.0f);
				const InstanceId id = object->GetInstanceId();
				const InstanceId parentId = object->GetParent() ? object->GetParent()->GetInstanceId() : InstanceId::Invalid;
				observer->m_onEnd = [&, id, parentId]()
				{
					Require(world.IsClearing() && !ended.Contains(id), "bulk teardown must end each component exactly once");
					Require(!parentId || ended.Contains(parentId), "Clear must visit the parent before its earlier-created children");
					Require(transforms->GetNumDirtyForTest() == dirtyCount &&
						animations->GetNextBoneOffsetForTest() == objects.Num(),
						"bulk unregister must leave dirty-queue cleanup and bone relayout to EndPlay");
					ended.Add(id);
				};
			}
			animations->Tick(0.0f);
			dirtyCount = transforms->GetNumDirtyForTest();
			Require(dirtyCount > 0 && animations->GetNextBoneOffsetForTest() == objects.Num(),
				"the fixture must have both pending transforms and allocated bone ranges");
			world.Destroy(objects[0]);
			world.Clear();
			Require(!world.IsClearing() && world.GetGameObjects().IsEmpty() && ended.Num() == objects.Num(),
				"bulk clear must finish every hierarchy and consume pending deletion without duplicate callbacks");
			Require(transforms->GetNumSlotsForTest() == 0 && transforms->GetNumDirtyForTest() == 0 &&
				animations->GetNumSlotsForTest() == 0 && animations->GetNextBoneOffsetForTest() == 0,
				"EndPlay must clear derived queues, animation storage and allocation cursors");
			for (const auto& object : objects)
			{
				Require(!object, "retained game object handles must be invalid after Clear");
			}
			for (const auto& id : ended)
			{
				Require(!world.GetObjectByInstanceId(id), "Clear must remove every destroyed object from the instance lookup");
			}
			world.Clear();
			Require(ended.Num() == objects.Num(), "repeated Clear must not repeat component cleanup");

			auto replacement = world.Instantiate("Authoring after Clear");
			replacement->AddComponent<AnimatorComponent>()->GetData().SetBonesCount(1);
			replacement->GetTransformComponent().SetPosition(glm::vec3(7.0f, 0.0f, 0.0f));
			transforms->Tick(0.0f);
			transforms->PostTick();
			animations->Tick(0.0f);
			Require(transforms->GetNumSlotsForTest() == 1 && animations->GetNumSlotsForTest() == 1 &&
				replacement->GetTransformComponent().GetWorldPosition().x == 7.0f &&
				replacement->GetComponent<AnimatorComponent>()->GetSkeletonOffset() == 0,
				"authoring reuse must start fresh slots without stale dirty indices or bone offsets");
			world.Clear();
		}
	}

	void TestWorldClearDestroysDescendantsReparentedByEndPlay()
	{
		for (bool reparentToEarlierRoot : { false, true })
		{
			PrefabTestWorld world;
			auto earlierRoot = reparentToEarlierRoot ? world.Instantiate("Earlier root") : GameObjectPtr{};
			auto root = world.Instantiate("Root");
			auto child = world.Instantiate("Child");
			auto grandchild = world.Instantiate("Grandchild");
			child->SetParent(root);
			grandchild->SetParent(child);
			const InstanceId grandchildId = grandchild->GetInstanceId();
			TVector<InstanceId> ended;
			for (auto object : { earlierRoot, root, child, grandchild })
			{
				if (object)
				{
					const InstanceId id = object->GetInstanceId();
					object->AddComponent<LifecycleTestComponent>()->m_onEnd = [&, id]()
					{
						Require(!ended.Contains(id), "callback-reparented hierarchies must end every component once");
						ended.Add(id);
					};
				}
			}
			auto callbackOwner = earlierRoot ? earlierRoot : root;
			const InstanceId callbackOwnerId = callbackOwner->GetInstanceId();
			callbackOwner->GetComponent<LifecycleTestComponent>()->m_onEnd = [&, callbackOwnerId]()
			{
				Require(!ended.Contains(callbackOwnerId), "the reparenting callback must run once");
				ended.Add(callbackOwnerId);
				grandchild->SetParent(earlierRoot);
			};
			world.Clear();
			const size_t expectedCount = reparentToEarlierRoot ? 4u : 3u;
			Require(ended.Num() == expectedCount && ended.Contains(grandchildId) && !grandchild &&
				!world.GetObjectByInstanceId(grandchildId) && world.GetGameObjects().IsEmpty(),
				"Clear must destroy original descendants detached or moved into an already-visited root by EndPlay");
			world.Clear();
			Require(ended.Num() == expectedCount, "repeated Clear must not revisit callback-reparented objects");
		}
	}

	void TestWorldClearUnlinksAllPrefabsBeforeCallbacks()
	{
		for (uint32_t count : { 16u, 32u })
		{
			PrefabTestWorld world;
			auto sourceRoot = world.Instantiate("Source root");
			auto sourceChild = world.Instantiate("Source child");
			sourceChild->SetParent(sourceRoot);
			sourceRoot->AddComponent<LifecycleTestComponent>();
			sourceChild->AddComponent<LifecycleTestComponent>();
			const FileId sourceId = FileId::CreateNewFileId();
			auto source = PrefabDocumentTestAsset::Capture(world, sourceRoot, sourceId);
			const std::string sourceText = YAML::Dump(source->Serialize());
			world.DestroyImmediate(sourceRoot);

			TVector<GameObjectPtr> roots;
			uint32_t ended = 0;
			for (uint32_t index = 0; index < count; ++index)
			{
				auto root = world.Instantiate(source);
				Require(root && world.IsPrefabInstanceRoot(root->GetInstanceId()), "the fixture must retain editor prefab linkage");
				roots.Add(root);
				for (auto object : { root, root->GetChildren()[0] })
				{
					object->GetComponent<LifecycleTestComponent>()->m_onEnd = [&]()
					{
						++ended;
						Require(world.GetPrefabInstances().IsEmpty(), "all derived prefab records must be cleared before teardown callbacks");
						for (const auto& live : world.GetGameObjects())
						{
							if (live)
							{
								Require(!live->GetFileId() && !world.IsPrefabLinked(live->GetInstanceId()) &&
									world.CanModifyPrefabStructure(live->GetInstanceId()),
									"no live prefab marker or membership may block bulk component cleanup");
							}
						}
					};
				}
			}
			auto parent = world.Instantiate("External parent created last");
			for (auto& root : roots)
			{
				root->SetParent(parent);
			}
			world.Clear();
			Require(ended == count * 2 && world.GetGameObjects().IsEmpty() &&
				YAML::Dump(source->Serialize()) == sourceText && source->GetFileId() == sourceId,
				"bulk unlink must clean every linked component without changing the source prefab");
			world.Clear();
			Require(ended == count * 2, "linked instance cleanup must remain idempotent");
			source.DestroyObject(world.GetAllocator());
		}
	}

	void TestWorldClearRetainsPublishedAnimationAndLandscape()
	{
		BulkClearTestWorld world;
		auto* animations = world.GetECS<AnimationECS>();
		auto* landscapes = world.GetECS<LandscapeECS>();
		auto animation = AnimationPtr::Make(world.GetAllocator(), FileId{});
		animation->m_numBones = 1;
		animation->m_numFrames = 1;
		animation->m_parentBoneIndices.Add(-1);
		Math::Transform pose;
		pose.m_position = glm::vec4(5.0f, 0.0f, 0.0f, 1.0f);
		animation->m_frames.Add(pose);
		animation->m_restPose.Add(pose);
		uint32_t ended = 0;
		RHI::RHISpatialSceneVersionPtr publishedLandscape;
		for (uint32_t index = 0; index < 32; ++index)
		{
			auto object = world.Instantiate("Published owner");
			object->SetMobilityType(EMobilityType::Static);
			object->AddComponent<AnimatorComponent>()->SetAnimation(animation);
			const auto landscape = object->AddComponent<LandscapeComponent>();
			auto& data = landscapes->GetComponentData(landscape->GetComponentIndex());
			LandscapeChunk chunk;
			chunk.m_buildRevision = 1;
			chunk.m_localBounds = Math::AABB(glm::vec3(-1.0f), glm::vec3(1.0f));
			chunk.m_resource = RHI::RHISceneProxyResourcePtr::Make();
			data.m_chunks.Add(std::move(chunk));
			object->AddComponent<LifecycleTestComponent>()->m_onEnd = [&]()
			{
				++ended;
				auto currentView = RHI::RHISceneViewPtr::Make();
				landscapes->AppendSceneView(currentView);
				Require(currentView->m_sceneVersions.Num() == 1 && currentView->m_sceneVersions[0] == publishedLandscape,
					"individual landscape cleanup during Clear must not publish intermediate scene versions");
			};
		}
		auto removedBeforeClear = world.Instantiate("Removal awaiting publication");
		removedBeforeClear->SetMobilityType(EMobilityType::Static);
		auto extraLandscape = removedBeforeClear->AddComponent<LandscapeComponent>();
		LandscapeChunk extraChunk;
		extraChunk.m_localBounds = Math::AABB(glm::vec3(-1), glm::vec3(1));
		extraChunk.m_resource = RHI::RHISceneProxyResourcePtr::Make();
		landscapes->GetComponentData(extraLandscape->GetComponentIndex()).m_chunks.Add(std::move(extraChunk));
		animations->Tick(0.0f);
		landscapes->BeginPlay();
		auto retained = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(retained);
		landscapes->AppendSceneView(retained);
		Require(retained->m_cpuBoneMatrices && retained->m_cpuBoneMatrices->Num() == 32 &&
			(*retained->m_cpuBoneMatrices)[0][3].x == 5.0f && retained->m_sceneVersions.Num() == 1,
			"the fixture must publish real CPU bone matrices and a landscape scene");
		publishedLandscape = retained->m_sceneVersions[0];
		Require(publishedLandscape->m_sceneVersion->m_staticHandles->Num() == 33,
			"every prepared landscape chunk must be present in the published scene");
		const auto handle = (*publishedLandscape->m_sceneVersion->m_staticHandles)[0];
		const RHI::RHISceneInstanceRecord* before = nullptr;
		Require(publishedLandscape->m_sceneVersion->Resolve(handle, before) && before && before->m_topology,
			"the retained landscape handle must resolve its topology");
		const auto* topology = before->m_topology.GetRawPtr();

		world.DestroyImmediate(removedBeforeClear);
		world.Clear();
		const RHI::RHISceneInstanceRecord* after = nullptr;
		Require(ended == 32 && publishedLandscape->m_sceneVersion->Resolve(handle, after) &&
			after->m_topology.GetRawPtr() == topology && (*retained->m_cpuBoneMatrices)[0][3].x == 5.0f,
			"already-published scene records and bone snapshots must outlive their destroyed world components");
		auto empty = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(empty);
		landscapes->AppendSceneView(empty);
		Require(!empty->m_cpuBoneMatrices && empty->m_animationRevision == 0 && empty->m_sceneVersions.IsEmpty(),
			"new consumers after Clear must not receive the old world publication");

		animation->m_frames[0].m_position.x = 9.0f;
		auto replacement = world.Instantiate("New animation owner");
		replacement->AddComponent<AnimatorComponent>()->SetAnimation(animation);
		animations->Tick(0.0f);
		auto next = RHI::RHISceneViewPtr::Make();
		animations->FillAnimationData(next);
		Require(next->m_cpuBoneMatrices && next->m_cpuBoneMatrices->Num() == 1 &&
			(*next->m_cpuBoneMatrices)[0][3].x == 9.0f && retained->m_cpuBoneMatrices->Num() == 32 &&
			(*retained->m_cpuBoneMatrices)[0][3].x == 5.0f,
			"new authoring publication must not overwrite snapshots retained across Clear");
		world.Clear();
		animation.DestroyObject(world.GetAllocator());
	}

	void TestDestroyHierarchyUnlinksSurvivingParent()
	{
		enum class Destruction { Immediate, DeferredParentFirst, DeferredChildFirst, QueuedChildImmediateParent };
		for (Destruction mode : { Destruction::Immediate, Destruction::DeferredParentFirst,
			Destruction::DeferredChildFirst, Destruction::QueuedChildImmediateParent })
		{
			PrefabTestWorld world(GameplayMask);
			auto parent = world.Instantiate("SurvivingParent");
			auto root = world.Instantiate("RemovedRoot");
			auto child = world.Instantiate("RemovedChild");
			auto sibling = world.Instantiate("SurvivingSibling");
			root->SetParent(parent);
			child->SetParent(root);
			sibling->SetParent(parent);
			auto rootComponent = root->AddComponent<LifecycleTestComponent>();
			auto childComponent = child->AddComponent<LifecycleTestComponent>();
			const InstanceId rootId = root->GetInstanceId();
			const InstanceId childId = child->GetInstanceId();
			auto* transforms = world.GetECS<TransformECS>();
			const size_t rootIndex = transforms->GetComponentIndex(&root->GetTransformComponent());
			const size_t childIndex = transforms->GetComponentIndex(&child->GetTransformComponent());
			const size_t siblingIndex = transforms->GetComponentIndex(&sibling->GetTransformComponent());
			world.TickLifecycle();
			transforms->Tick(0.0f);
			transforms->PostTick();
			const uint32_t ended = LifecycleTestComponent::s_ended;

			if (mode == Destruction::DeferredParentFirst || mode == Destruction::DeferredChildFirst)
			{
				if (mode == Destruction::DeferredChildFirst)
				{
					world.Destroy(child);
				}
				world.Destroy(root);
				world.Destroy(root);
				world.Destroy(child);
				Require(root && child && parent->GetChildren().Num() == 2 &&
					LifecycleTestComponent::s_ended == ended,
					"deferred destruction must leave the hierarchy intact until its lifecycle phase");
				world.DestroyPendingGameObjects();
			}
			else
			{
				if (mode == Destruction::QueuedChildImmediateParent)
				{
					world.Destroy(child);
				}
				world.DestroyImmediate(root);
			}

			Require(!root && !child && !rootComponent && !childComponent &&
				!world.GetObjectByInstanceId(rootId) && !world.GetObjectByInstanceId(childId),
				"both destruction paths must release the subtree and its components");
			Require(world.GetGameObjects().Num() == 2 && parent->GetChildren().Num() == 1 &&
				parent->GetChildren()[0] == sibling && sibling->GetParent() == parent,
				"a surviving parent must retain only its live children after either destruction path");
			Require(!transforms->IsComponentRegistered(rootIndex) && !transforms->IsComponentRegistered(childIndex) &&
				parent->GetTransformComponent().GetChildren().Num() == 1 &&
				parent->GetTransformComponent().GetChildren()[0] == siblingIndex,
				"the transform hierarchy must agree with the surviving game-object hierarchy");
			world.DestroyPendingGameObjects();
			Require(LifecycleTestComponent::s_ended == ended + 2,
				"duplicate and descendant delete requests must not repeat component cleanup");
			world.Clear();
		}
	}

	void TestDestroyLinkedRootUnlinksExternalParent()
	{
		for (bool deferred : { false, true })
		{
			PrefabTestWorld world;
			auto authoredRoot = world.Instantiate("AuthoredRoot");
			auto authoredChild = world.Instantiate("AuthoredChild");
			authoredChild->SetParent(authoredRoot);
			const FileId sourceFileId = DeserializeFileId("{11111111-2222-3333-4444-555555555555}");
			auto source = PrefabDocumentTestAsset::Capture(world, authoredRoot, sourceFileId);
			const std::string sourceBefore = YAML::Dump(source->Serialize());
			world.DestroyImmediate(authoredRoot);

			auto externalParent = world.Instantiate("ExternalParent");
			auto sibling = world.Instantiate("ExternalSibling");
			sibling->SetParent(externalParent);
			auto root = world.Instantiate(source);
			Require(root && root->GetChildren().Num() == 1,
				"the linked-root destruction fixture must instantiate its child");
			auto child = root->GetChildren()[0];
			const InstanceId rootId = root->GetInstanceId();
			const InstanceId childId = child->GetInstanceId();
			root->SetParent(externalParent);
			Require(root->GetParent() == externalParent && world.IsPrefabInstanceRoot(rootId) &&
				world.IsPrefabLinked(childId),
				"a linked authoring root may be parented beneath an external object");
			auto* transforms = world.GetECS<TransformECS>();
			const size_t siblingIndex = transforms->GetComponentIndex(&sibling->GetTransformComponent());
			transforms->Tick(0.0f);
			transforms->PostTick();

			world.Destroy(child);
			world.DestroyImmediate(child);
			world.DestroyPendingGameObjects();
			Require(child && child->GetParent() == root && world.IsPrefabLinked(childId),
				"both public destruction APIs must still reject an internal linked child");
			if (deferred)
			{
				world.Destroy(root);
				Require(world.IsPrefabInstanceRoot(rootId) && externalParent->GetChildren().Num() == 2,
					"queuing a linked root must retain its authoring link until destruction");
				world.DestroyPendingGameObjects();
			}
			else
			{
				world.DestroyImmediate(root);
			}

			Require(!root && !child && world.GetGameObjects().Num() == 2 &&
				externalParent->GetChildren().Num() == 1 && externalParent->GetChildren()[0] == sibling &&
				sibling->GetParent() == externalParent,
				"destroying a linked root must unlink it from its surviving external parent");
			Require(world.GetPrefabInstances().IsEmpty() && !world.IsPrefabLinked(rootId) &&
				!world.IsPrefabLinked(childId) && YAML::Dump(source->Serialize()) == sourceBefore,
				"destroying the instance must clear live links without changing the source prefab");
			Require(externalParent->GetTransformComponent().GetChildren().Num() == 1 &&
				externalParent->GetTransformComponent().GetChildren()[0] == siblingIndex,
				"a linked root's transform must also be removed from its external parent");
			world.DestroyPendingGameObjects();
			world.Clear();
			source.DestroyObject(world.GetAllocator());
		}
	}

	void TestEditorLifecycleNeverStartsGameplay()
	{
		PrefabTestWorld world((uint8_t)EWorldBehaviourBit::EditorTick | (uint8_t)EWorldBehaviourBit::EcsTickable);
		auto original = world.Instantiate("BeforeFirstFrame")->AddComponent<LifecycleTestComponent>();
		Require(original->m_bPublishedAtInitialize && !original->IsValid(),
			"Initialize must see the component in its owner before gameplay activation");
		original->SetValue(17.0f);
		Require(original->GetSlotValue() == 17.0f,
			"Initialize must allocate the ECS slot immediately for reflected setters and editor preview");
		world.TickLifecycle();

		auto laterOwner = world.Instantiate("AfterFirstFrame");
		auto typed = laterOwner->AddComponent<LifecycleTestComponent>();
		auto raw = TObjectPtr<LifecycleTestComponent>::Make(world.GetAllocator());
		Require(static_cast<bool>(laterOwner->AddComponentRaw(raw)), "raw editor component creation must succeed");
		Require(typed->m_bPublishedAtInitialize && raw->m_bPublishedAtInitialize &&
			typed->m_begins == 0 && raw->m_begins == 0,
			"both component creation paths must initialize without starting gameplay in an already ticking editor");
		world.TickLifecycle();
		Require(original->m_editorTicks == 2 && typed->m_editorTicks == 1 && raw->m_editorTicks == 1 &&
			original->m_begins == 0 && typed->m_begins == 0 && raw->m_begins == 0 &&
			original->m_ticks == 0 && typed->m_ticks == 0 && raw->m_ticks == 0,
			"editor callbacks must remain available without any gameplay BeginPlay or Tick");
		world.Clear();
	}

	void TestBeginPlayAndTickMasksRemainIndependent()
	{
		PrefabTestWorld tickOnly((uint8_t)EWorldBehaviourBit::Tickable);
		auto inactive = tickOnly.Instantiate()->AddComponent<LifecycleTestComponent>();
		tickOnly.TickLifecycle();
		tickOnly.TickLifecycle();
		Require(inactive->m_begins == 0 && inactive->m_ticks == 0,
			"Tickable alone must not implicitly start an inactive gameplay component");
		tickOnly.Clear();

		PrefabTestWorld beginOnly((uint8_t)EWorldBehaviourBit::CallBeginPlay);
		auto started = beginOnly.Instantiate()->AddComponent<LifecycleTestComponent>();
		beginOnly.TickLifecycle();
		beginOnly.TickLifecycle();
		Require(started->m_begins == 1 && started->m_bValidAtBegin && started->m_ticks == 0,
			"CallBeginPlay must activate once, set validity before the callback and not enable gameplay Tick");
		beginOnly.Clear();
	}

	void TestPrefabBeginsAfterHydrationAndHierarchy()
	{
		PrefabTestWorld world(GameplayMask);
		world.TickLifecycle();
		YAML::Node prefabNode = Tests::MakeLifecyclePrefabDocument();
		auto prefab = DeserializePrefab(world, prefabNode);
		auto root = world.Instantiate(prefab);
		Require(root && root->GetChildren().Num() == 1, "the runtime prefab must commit its complete hierarchy");
		auto child = root->GetChildren()[0];
		auto rootComponent = root->GetComponent<LifecycleTestComponent>();
		auto childComponent = child->GetComponent<LifecycleTestComponent>();
		Require(rootComponent && childComponent && rootComponent->m_bPublishedAtInitialize && childComponent->m_bPublishedAtInitialize &&
			rootComponent->GetSlotValue() == 31.0f && childComponent->GetSlotValue() == 47.0f &&
			rootComponent->m_begins == 0 && childComponent->m_begins == 0,
			"prefab hydration must use initialized ECS slots without eager gameplay callbacks");
		world.TickLifecycle();
		Require(rootComponent->m_begins == 1 && childComponent->m_begins == 1 &&
			rootComponent->m_valueAtBegin == 31.0f && childComponent->m_valueAtBegin == 47.0f &&
			rootComponent->m_dependencyAtBegin == childComponent && childComponent->m_dependencyAtBegin == rootComponent &&
			!rootComponent->m_parentAtBegin && childComponent->m_parentAtBegin == root &&
			rootComponent->m_positionAtBegin.x == 10.0f && childComponent->m_positionAtBegin.x == 11.0f &&
			rootComponent->m_bValidAtBegin && childComponent->m_bValidAtBegin,
			"BeginPlay must observe authored properties, both internal references, transforms and the committed parent");
		Require(rootComponent->m_ticks == 0 && childComponent->m_ticks == 0,
			"a component's first lifecycle callback is BeginPlay, not BeginPlay followed by Tick in the same phase");
		world.TickLifecycle();
		Require(rootComponent->m_begins == 1 && childComponent->m_begins == 1 &&
			rootComponent->m_ticks == 1 && childComponent->m_ticks == 1,
			"hydrated components must begin exactly once and tick on subsequent frames");
		world.Clear();
		prefab.DestroyObject(world.GetAllocator());
	}

	void TestFailedPrefabNeverBeginsGameplay()
	{
		PrefabTestWorld world(GameplayMask);
		world.Instantiate("ExistingObject");
		world.TickLifecycle();
		const uint32_t initialized = LifecycleTestComponent::s_initialized;
		const uint32_t begun = LifecycleTestComponent::s_begun;
		const uint32_t ended = LifecycleTestComponent::s_ended;
		YAML::Node components(YAML::NodeType::Sequence);
		for (uint32_t index = 0; index < 2; ++index)
		{
			YAML::Node properties;
			properties["value"] = index == 0 ? "15.0" : "not-a-float";
			components.push_back(MakeReflectedComponent(
				index == 0 ? "1111111111111111_10010010010010010000" : "2222222222222222_10010010010010010000",
				properties, true, LifecycleTestComponent::GetStaticTypeInfo().Name()));
		}
		auto prefab = DeserializePrefab(world, MakeComponentPrefabNode(components));
		Require(!world.Instantiate(prefab), "a malformed property must reject the prefab after component initialization");
		Require(world.GetGameObjects().Num() == 1 && world.GetPendingDependencyCount() == 0 &&
			LifecycleTestComponent::s_initialized == initialized + 2 && LifecycleTestComponent::s_ended == ended + 2 &&
			LifecycleTestComponent::s_begun == begun,
			"rollback must release initialized components without running their gameplay callbacks");
		world.TickLifecycle();
		Require(LifecycleTestComponent::s_begun == begun,
			"failed prefab components must not remain eligible for a later lifecycle phase");
		world.Clear();
		prefab.DestroyObject(world.GetAllocator());
	}

	void TestBeginPlayWaitsForExternalReferences()
	{
		PrefabTestWorld world(GameplayMask);
		world.TickLifecycle();
		const InstanceId targetId = InstanceId::GenerateNewInstanceId();
		const InstanceId targetComponentId = InstanceId::GenerateNewComponentId(targetId);
		YAML::Node properties;
		properties["value"] = 23.0f;
		properties["m_dependency"]["fileId"] = "NullFileId";
		properties["m_dependency"]["instanceId"] = targetComponentId.ToString();
		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(MakeReflectedComponent(
			"1111111111111111_10010010010010010000", properties, true,
			LifecycleTestComponent::GetStaticTypeInfo().Name()));
		auto prefab = DeserializePrefab(world, MakeComponentPrefabNode(components));
		auto owner = world.Instantiate(prefab);
		Require(static_cast<bool>(owner), "an external reference may be resolved after the prefab commits");
		auto source = owner->GetComponent<LifecycleTestComponent>();
		world.TickLifecycle();
		world.ResolveExternalDependencies();
		world.TickLifecycle();
		Require(world.GetPendingDependencyCount() == 1 && source->m_begins == 0 && source->m_ticks == 0 && !source->IsValid(),
			"a committed component with an unresolved external reference must not activate or tick");

		auto targetOwner = world.Instantiate("LateDependency", targetId);
		auto target = TObjectPtr<LifecycleTestComponent>::Make(world.GetAllocator());
		Require(static_cast<bool>(targetOwner->AddComponentRaw(target, targetComponentId)),
			"the later component must retain the referenced instance identity");
		world.ResolveExternalDependencies();
		Require(world.GetPendingDependencyCount() == 0 && source->m_dependency == target && source->m_begins == 0,
			"reference resolution marks readiness but must not run BeginPlay outside the lifecycle phase");
		world.TickLifecycle();
		Require(source->m_begins == 1 && source->m_dependencyAtBegin == target && source->m_valueAtBegin == 23.0f &&
			source->m_ticks == 0 && target->m_begins == 1,
			"the next lifecycle phase must activate the now-resolved component exactly once");
		world.TickLifecycle();
		Require(source->m_begins == 1 && source->m_ticks == 1,
			"a previously pending reference must not cause repeated BeginPlay");
		world.Clear();
		prefab.DestroyObject(world.GetAllocator());
	}

	void TestPendingReferencesStayWithinPrefabInstance()
	{
		for (bool forceNewIds : { true, false })
		{
			PrefabTestWorld world(GameplayMask);
			world.TickLifecycle();
			const InstanceId externalOwnerId = InstanceId::GenerateNewInstanceId();
			const InstanceId externalComponentId = InstanceId::GenerateNewComponentId(externalOwnerId);
			YAML::Node prefabNode = MakePrefabNode({ static_cast<uint32_t>(-1), 0 });
			const InstanceId sourceRootId(prefabNode["gameObjects"][0]["instanceId"].as<std::string>());
			const InstanceId sourceChildId(prefabNode["gameObjects"][1]["instanceId"].as<std::string>());
			const InstanceId rootComponentId = InstanceId::GenerateNewComponentId(sourceRootId);
			const InstanceId childComponentId = InstanceId::GenerateNewComponentId(sourceChildId);
			YAML::Node rootProperties;
			rootProperties["value"] = 19.0f;
			rootProperties["m_dependency"]["fileId"] = "NullFileId";
			rootProperties["m_dependency"]["instanceId"] = childComponentId.ToString();
			rootProperties["externalDependency"]["fileId"] = "NullFileId";
			rootProperties["externalDependency"]["instanceId"] = externalComponentId.ToString();
			prefabNode["components"].push_back(MakeReflectedComponent(rootComponentId.ToString(), rootProperties, true,
				LifecycleTestComponent::GetStaticTypeInfo().Name()));
			prefabNode["components"].push_back(MakeReflectedComponent(childComponentId.ToString(), YAML::Node(), true,
				LifecycleTestComponent::GetStaticTypeInfo().Name()));
			prefabNode["gameObjects"][0]["components"].push_back(0);
			prefabNode["gameObjects"][1]["components"].push_back(1);
			auto prefab = DeserializePrefab(world, prefabNode);
			const std::string sourceBefore = YAML::Dump(prefab->Serialize());

			TVector<GameObjectPtr> instances;
			const uint32_t instanceCount = forceNewIds ? 1 : 2;
			for (uint32_t index = 0; index < instanceCount; ++index)
			{
				auto root = world.Instantiate(prefab, forceNewIds ? EPrefabInstanceIdPolicy::GenerateNew : EPrefabInstanceIdPolicy::PreserveAvailable);
				Require(root && root->GetChildren().Num() == 1, "the mixed-reference prefab must commit a complete instance");
				auto child = root->GetChildren()[0];
				auto component = root->GetComponent<LifecycleTestComponent>();
				Require(component->m_dependency == child->GetComponent<LifecycleTestComponent>() &&
					!component->GetExternalDependency(), "initial resolution must bind the local sibling while leaving the external reference pending");
				if (forceNewIds || index > 0)
				{
					Require(root->GetInstanceId() != sourceRootId && child->GetInstanceId() != sourceChildId,
						"forced IDs and repeated-instance collisions must remap both source game objects");
				}
				instances.Add(root);
			}

			for (uint32_t retry = 0; retry < 2; ++retry)
			{
				world.ResolveExternalDependencies();
				world.TickLifecycle();
				Require(world.GetPendingDependencyCount() == instanceCount, "each unresolved external reference must remain pending");
				for (auto& root : instances)
				{
					auto child = root->GetChildren()[0];
					auto component = root->GetComponent<LifecycleTestComponent>();
					Require(component->m_dependency == child->GetComponent<LifecycleTestComponent>() &&
						component->m_begins == 0 && component->m_ticks == 0,
						"pending retries must preserve this instance's sibling, without switching to the source-ID instance or starting gameplay");
				}
			}

			auto externalOwner = world.Instantiate("LateExternal", externalOwnerId);
			auto external = TObjectPtr<LifecycleTestComponent>::Make(world.GetAllocator());
			Require(static_cast<bool>(externalOwner->AddComponentRaw(external, externalComponentId)),
				"the external dependency must appear later with its original identity");
			world.ResolveExternalDependencies();
			Require(world.GetPendingDependencyCount() == 0, "live internal IDs and the newly created external object must resolve together");
			world.TickLifecycle();
			world.TickLifecycle();
			for (auto& root : instances)
			{
				auto child = root->GetChildren()[0];
				auto component = root->GetComponent<LifecycleTestComponent>();
				Require(component->m_begins == 1 && component->m_ticks == 1 && component->m_valueAtBegin == 19.0f &&
					component->m_dependencyAtBegin == child->GetComponent<LifecycleTestComponent>() &&
					component->m_externalDependencyAtBegin == external,
					"BeginPlay must run once with both correct references after the external dependency resolves");
			}
			Require(YAML::Dump(prefab->Serialize()) == sourceBefore,
				"per-instance pending-reference remapping must never modify the source prefab's YAML nodes");
			world.Clear();
			prefab.DestroyObject(world.GetAllocator());
		}
	}

	void TestBeginPlayCanChangeComponentLists()
	{
		PrefabTestWorld world(GameplayMask);
		auto owner = world.Instantiate("MutatingOwner");
		auto mutating = owner->AddComponent<LifecycleTestComponent>();
		auto removed = owner->AddComponent<LifecycleTestComponent>();
		auto survivor = owner->AddComponent<LifecycleTestComponent>();
		auto otherOwner = world.Instantiate("LaterOwner");
		auto otherOriginal = otherOwner->AddComponent<LifecycleTestComponent>();
		TVector<TObjectPtr<LifecycleTestComponent>> additions;
		const uint32_t ended = LifecycleTestComponent::s_ended;
		mutating->m_onBegin = [&]()
			{
				Require(owner->RemoveComponent(removed), "BeginPlay must be able to remove a not-yet-started sibling");
				for (uint32_t index = 0; index < 24; ++index)
				{
					additions.Add(owner->AddComponent<LifecycleTestComponent>());
				}
				additions.Add(otherOwner->AddComponent<LifecycleTestComponent>());
				for (uint32_t index = 0; index < 32; ++index)
				{
					additions.Add(world.Instantiate("SpawnedDuringBegin")->AddComponent<LifecycleTestComponent>());
				}
				Require(owner->RemoveComponent(mutating), "BeginPlay must be able to remove its own component");
			};
		world.TickLifecycle();
		Require(!removed && !mutating && LifecycleTestComponent::s_ended == ended + 2 &&
			survivor->m_begins == 1 && otherOriginal->m_begins == 1,
			"self removal, removal of the next component and vector growth must not skip or repeat surviving callbacks");
		for (auto& component : additions)
		{
			Require(component && component->m_bPublishedAtInitialize && component->m_begins == 0 && component->m_ticks == 0,
				"components added to this owner, a later owner or a new object must wait for the next lifecycle frame");
		}
		world.TickLifecycle();
		Require(survivor->m_ticks == 1 && otherOriginal->m_ticks == 1,
			"surviving components must tick once after the mutating BeginPlay frame");
		for (auto& component : additions)
		{
			Require(component->m_begins == 1 && component->m_ticks == 0,
				"each callback-created component must begin exactly once on the next frame");
		}
		world.TickLifecycle();
		for (auto& component : additions)
		{
			Require(component->m_begins == 1 && component->m_ticks == 1,
				"callback-created components must join the ordinary Tick phase after BeginPlay");
		}
		world.Clear();
	}

	void TestBeginPlayCanDestroyItsOwner()
	{
		PrefabTestWorld world(GameplayMask);
		auto owner = world.Instantiate("DestroyedDuringBegin");
		auto mutating = owner->AddComponent<LifecycleTestComponent>();
		auto sibling = owner->AddComponent<LifecycleTestComponent>();
		auto otherOwner = world.Instantiate("RemovedBeforeItsTurn");
		auto otherComponent = otherOwner->AddComponent<LifecycleTestComponent>();
		auto survivor = world.Instantiate("Survivor")->AddComponent<LifecycleTestComponent>();
		TObjectPtr<LifecycleTestComponent> replacement;
		const uint32_t begun = LifecycleTestComponent::s_begun;
		const uint32_t ended = LifecycleTestComponent::s_ended;
		mutating->m_onBegin = [&]()
			{
				world.DestroyImmediate(otherOwner);
				replacement = world.Instantiate("Replacement")->AddComponent<LifecycleTestComponent>();
				world.DestroyImmediate(owner);
			};
		world.TickLifecycle();
		Require(!owner && !mutating && !sibling && !otherOwner && !otherComponent &&
			world.GetGameObjects().Num() == 2 && LifecycleTestComponent::s_ended == ended + 3 &&
			LifecycleTestComponent::s_begun == begun + 2 && survivor->m_begins == 1 && replacement->m_begins == 0,
			"destroyed handles must be skipped after callbacks, without activating removed siblings or new replacement objects");
		world.TickLifecycle();
		Require(survivor->m_ticks == 1 && replacement->m_begins == 1 && replacement->m_ticks == 0,
			"the surviving world must continue normally after immediate destruction inside BeginPlay");
		world.Clear();
	}

	void TestTickAdditionsWaitForNextLifecycleFrame()
	{
		PrefabTestWorld world(GameplayMask);
		auto owner = world.Instantiate("TickMutation");
		auto original = owner->AddComponent<LifecycleTestComponent>();
		auto laterOwner = world.Instantiate("LaterOwner");
		TObjectPtr<LifecycleTestComponent> added;
		original->m_onTick = [&]()
			{
				if (!added)
				{
					added = laterOwner->AddComponent<LifecycleTestComponent>();
				}
			};
		world.TickLifecycle();
		world.TickLifecycle();
		Require(added && added->m_begins == 0 && added->m_ticks == 0 && original->m_ticks == 1,
			"a component added from Tick to a later object must not begin in that same phase");
		world.TickLifecycle();
		Require(added->m_begins == 1 && added->m_ticks == 0 && original->m_ticks == 2,
			"Tick-created components must begin at the next lifecycle boundary");
		world.Clear();
	}

	void TestRemovingAnotherOwnersComponentHasNoEffect()
	{
		PrefabTestWorld world;
		auto first = world.Instantiate("First");
		auto second = world.Instantiate("Second");
		auto firstComponent = first->AddComponent<LifecycleTestComponent>();
		auto secondComponent = second->AddComponent<LifecycleTestComponent>();
		const uint32_t ended = LifecycleTestComponent::s_ended;
		Require(!first->RemoveComponent(secondComponent), "RemoveComponent must reject a component absent from this owner's list");
		Require(firstComponent && secondComponent && first->GetComponents().Num() == 1 && second->GetComponents().Num() == 1 &&
			LifecycleTestComponent::s_ended == ended,
			"a failed list lookup must not run EndPlay or destroy another owner's component");
		Require(second->RemoveComponent(secondComponent) && !secondComponent && LifecycleTestComponent::s_ended == ended + 1,
			"the owning object must still be able to remove the component exactly once");
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

	void TestWorldObjectInlineStorage()
	{
		auto world = TUniquePtr<PrefabTestWorld>::Make();
		ScopeExit cleanup([&]() { world->Clear(); });
		const auto& objects = std::as_const(*world).GetGameObjects();
		const auto storageBegin = reinterpret_cast<uintptr_t>(&objects);
		const auto storageEnd = storageBegin + sizeof(objects);
		for (uint32_t cycle = 0; cycle < 2; ++cycle)
		{
			for (uint32_t i = 0; i < 16'000; ++i)
			{
				world->Instantiate("Inline object");
				const auto address = reinterpret_cast<uintptr_t>(&*objects.Last());
				Require(address >= storageBegin && address + sizeof(GameObjectPtr) <= storageEnd,
					"all 16000 world-list entries must reside in the inline buffer, including after Clear");
			}
			const auto* firstSlot = &*objects.begin();
			auto overflow = world->Instantiate("Overflow");
			const auto overflowAddress = reinterpret_cast<uintptr_t>(&*objects.Last());
			Require(objects.Num() == 16'001 && (overflowAddress < storageBegin || overflowAddress >= storageEnd),
				"objects beyond the inline capacity must use the fallback allocator");
			world->DestroyImmediate(*std::next(objects.begin(), 8'000));
			Require(objects.Num() == 16'000 && &*objects.begin() == firstSlot && *objects.Last() == overflow,
				"removing an inline node must preserve surviving entries and the overflow node");
			world->Clear();
			Require(objects.IsEmpty() && !overflow, "Clear must release both inline and fallback entries");
		}
		std::cout << "World objects: 16000 inline entries, overflow, middle removal and full reuse after Clear passed\n";
	}

	void TestWorldRemovalWorkIsLocal()
	{
		enum class Removal { Component, Immediate, Deferred };
		for (uint32_t unrelated : { 0u, 4096u })
		{
			for (uint32_t count : { 32u, 64u })
			{
				for (Removal removal : { Removal::Component, Removal::Immediate, Removal::Deferred })
				{
					PrefabTestWorld world;
					ScopeExit cleanup([&]() { world.Clear(); });
					const auto targetId = InstanceId::GenerateNewInstanceId();
					const auto componentId = InstanceId::GenerateNewComponentId(targetId);
					YAML::Node properties;
					properties["m_dependency"]["fileId"] = "NullFileId";
					properties["m_dependency"]["instanceId"] = componentId.ToString();
					const auto reflection = Reflection::CreateReflectedData(LifecycleTestComponent::GetStaticTypeInfo(), properties);
					auto first = world.Instantiate("First survivor");
					TVector<GameObjectPtr> owners;
					TVector<TObjectPtr<LifecycleTestComponent>> components;
					for (uint32_t i = 0; i < unrelated + count; ++i)
					{
						auto owner = world.Instantiate("Waiting owner");
						auto component = owner->AddComponent<LifecycleTestComponent>();
						world.ApplyComponentReflection(component, reflection, false);
						owners.Add(owner);
						components.Add(component);
					}
					auto last = world.Instantiate("Last survivor");
					world.SetEditorSelection({ last->GetInstanceId(), first->GetInstanceId() });
					Require(world.GetPendingDependencyCount() == unrelated + count,
						"the fixture must populate the actual World dependency queue");
					const auto snapshot = world.GetGameObjects();
					const auto& liveObjects = std::as_const(world).GetGameObjects();
					world.ResetRemovalVisits();
					for (uint32_t parity = 0; parity < 2; ++parity)
					{
						for (uint32_t i = parity; i < count; i += 2)
						{
							const uint32_t index = unrelated + i;
							if (removal == Removal::Component)
								Require(owners[index]->RemoveComponent(components[index]), "component removal must succeed");
							else if (removal == Removal::Immediate) world.DestroyImmediate(owners[index]);
							else world.Destroy(owners[index]);
						}
					}
					if (removal == Removal::Deferred) world.DestroyPendingGameObjects();
					const auto visits = world.GetRemovalVisits();
					std::cout << "World removal: " << count << " owners, " << unrelated << " unrelated pending records, mode=" <<
						static_cast<int>(removal) << ", " << visits << " examined entries" << std::endl;
					Require(visits > 0 && visits <= count * 4,
						"World unlink must not scan or shift unrelated objects or pending dependency records per removal");
					Require(world.GetPendingDependencyCount() == unrelated &&
						world.GetPrimaryEditorSelection() == first && snapshot.Num() == unrelated + count + 2,
						"removal must preserve unrelated pending work, insertion-based selection and the existing snapshot");
					size_t position = 0;
					for (const auto& owner : liveObjects)
					{
						const auto expected = position == 0 ? first :
							position <= unrelated + (removal == Removal::Component ? count : 0) ? owners[position - 1] : last;
						Require(owner == expected, "live enumeration must preserve the exact surviving insertion order");
						++position;
					}
					Require(position == unrelated + (removal == Removal::Component ? count : 0) + 2,
						"the const live collection must observe the removals without a second query");

					auto replacementOwner = world.Instantiate("Replacement");
					auto replacement = replacementOwner->AddComponent<LifecycleTestComponent>();
					replacement->SetValue(71);
					auto targetOwner = world.Instantiate("Late target", targetId);
					auto target = TObjectPtr<LifecycleTestComponent>::Make(world.GetAllocator());
					Require(static_cast<bool>(targetOwner->AddComponentRaw(target, componentId)), "the real late target must retain its identity");
					world.ResolveExternalDependencies();
					Require(world.GetPendingDependencyCount() == 0 && !replacement->m_dependency &&
						replacement->GetValue() == 71 && replacement->GetSlotValue() == 71,
						"removed pending records must not mutate a replacement component when the target appears");
					for (uint32_t i = 0; i < unrelated; ++i)
						Require(components[i]->m_dependency == target, "every surviving dependency must resolve to the late target");
					for (uint32_t i = unrelated; i < unrelated + count; ++i)
						Require(!components[i], "all requested components must be invalidated");

					world.Clear();
					auto afterClear = world.Instantiate("After Clear");
					auto pending = afterClear->AddComponent<LifecycleTestComponent>();
					world.ApplyComponentReflection(pending, reflection, false);
					Require(world.GetPendingDependencyCount() == 1, "Clear must reset the pending queue for the next world contents");
					Require(afterClear->RemoveComponent(pending) && world.GetPendingDependencyCount() == 0,
						"a fresh pending record must remain removable after Clear");
				}
			}
		}
	}

	void TestPendingDependenciesAreCancelledBeforeEndPlay()
	{
		for (uint32_t mode = 0; mode < 3; ++mode)
		{
			PrefabTestWorld world;
			ScopeExit cleanup([&]() { world.Clear(); });
			YAML::Node properties;
			properties["m_dependency"]["fileId"] = "NullFileId";
			properties["m_dependency"]["instanceId"] = InstanceId::GenerateNewComponentId(InstanceId::GenerateNewInstanceId()).ToString();
			const auto reflection = Reflection::CreateReflectedData(LifecycleTestComponent::GetStaticTypeInfo(), properties);
			auto survivor = world.Instantiate("Survivor")->AddComponent<LifecycleTestComponent>();
			world.ApplyComponentReflection(survivor, reflection, false);
			auto owner = world.Instantiate("Removed owner");
			uint32_t ended = 0;
			for (uint32_t i = 0; i < 3; ++i)
			{
				auto component = owner->AddComponent<LifecycleTestComponent>();
				world.ApplyComponentReflection(component, reflection, false);
				component->m_onEnd = [&]()
				{
					++ended;
					Require(world.GetPendingDependencyCount() == (mode == 2 ? 0 : 1),
						"all of a removed owner's requests must be cancelled before its first EndPlay callback");
					world.ResolveExternalDependencies();
				};
			}
			if (mode == 0) world.DestroyImmediate(owner);
			else if (mode == 1) { world.Destroy(owner); world.DestroyPendingGameObjects(); }
			else world.Clear();
			Require(ended == 3 && !owner, "immediate, deferred and bulk removal must end every component once");
		}
	}

	void TestPrefabRollbackPreservesUnrelatedPendingRequests()
	{
		PrefabTestWorld world;
		ScopeExit cleanup([&]() { LifecycleTestComponent::s_onEnd = {}; world.Clear(); });
		const auto targetId = InstanceId::GenerateNewInstanceId();
		const auto componentId = InstanceId::GenerateNewComponentId(targetId);
		YAML::Node properties;
		properties["m_dependency"]["fileId"] = "NullFileId";
		properties["m_dependency"]["instanceId"] = componentId.ToString();
		auto survivor = world.Instantiate("Unrelated waiting owner")->AddComponent<LifecycleTestComponent>();
		world.ApplyComponentReflection(survivor,
			Reflection::CreateReflectedData(LifecycleTestComponent::GetStaticTypeInfo(), properties), false);
		auto document = MakeLifecyclePrefabDocument();
		TMap<InstanceId, InstanceId> ids;
		for (uint32_t i = 0; i < 2; ++i)
		{
			document["components"][i]["overrideProperties"]["m_dependency"] = YAML::Clone(properties["m_dependency"]);
			ids[document["gameObjects"][i]["instanceId"].as<InstanceId>()] = InstanceId::GenerateNewInstanceId();
		}
		const auto sourceId = DeserializeFileId("1234567890ABCDEF");
		auto source = DeserializePrefab(world, sourceId, document);
		auto linked = PrefabPtr::Make(world.GetAllocator(), sourceId);
		ScopeExit releasePrefabs([&]() { linked.DestroyObject(world.GetAllocator()); source.DestroyObject(world.GetAllocator()); });
		std::string diagnostic;
		Require(linked->ConfigureLinkedInstance(source, ids, InstanceId::GenerateNewInstanceId(), {}, {}, diagnostic),
			"the missing-parent fixture must configure successfully before the late instantiation failure");
		uint32_t ended = 0;
		bool bCancelledBeforeCallbacks = true;
		LifecycleTestComponent::s_onEnd = [&]()
		{
			++ended;
			bCancelledBeforeCallbacks &= world.GetPendingDependencyCount() == 1;
		};
		Require(!world.Instantiate(linked), "the missing parent must reject the prefab after its requests were queued");
		LifecycleTestComponent::s_onEnd = {};
		Require(ended == 2 && bCancelledBeforeCallbacks && world.GetPendingDependencyCount() == 1 &&
			world.GetGameObjects().Num() == 1 && survivor,
			"rollback must cancel all new requests before callbacks and retain unrelated world state");
		auto targetOwner = world.Instantiate("Late target", targetId);
		auto target = TObjectPtr<LifecycleTestComponent>::Make(world.GetAllocator());
		Require(static_cast<bool>(targetOwner->AddComponentRaw(target, componentId)), "the external target must retain its identity");
		world.ResolveExternalDependencies();
		Require(world.GetPendingDependencyCount() == 0 && survivor->m_dependency == target,
			"the pending request which predates rollback must still resolve normally");
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
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "ComponentSlotsAreResetAndFreedOnce", TestComponentSlotsAreResetAndFreedOnce },
		{ "TransformRotationIsCanonicalBeforePublication", TestTransformRotationIsCanonicalBeforePublication },
		{ "PrefabRotationUsesCanonicalTransformBoundary", TestPrefabRotationUsesCanonicalTransformBoundary },
		{ "EditorLifecycleNeverStartsGameplay", TestEditorLifecycleNeverStartsGameplay },
		{ "BeginPlayAndTickMasksRemainIndependent", TestBeginPlayAndTickMasksRemainIndependent },
		{ "PrefabBeginsAfterHydrationAndHierarchy", TestPrefabBeginsAfterHydrationAndHierarchy },
		{ "FailedPrefabNeverBeginsGameplay", TestFailedPrefabNeverBeginsGameplay },
		{ "BeginPlayWaitsForExternalReferences", TestBeginPlayWaitsForExternalReferences },
		{ "PendingReferencesStayWithinPrefabInstance", TestPendingReferencesStayWithinPrefabInstance },
		{ "BeginPlayCanChangeComponentLists", TestBeginPlayCanChangeComponentLists },
		{ "BeginPlayCanDestroyItsOwner", TestBeginPlayCanDestroyItsOwner },
		{ "TickAdditionsWaitForNextLifecycleFrame", TestTickAdditionsWaitForNextLifecycleFrame },
		{ "RemovingAnotherOwnersComponentHasNoEffect", TestRemovingAnotherOwnersComponentHasNoEffect },
		{ "DestroyHierarchyUnlinksSurvivingParent", TestDestroyHierarchyUnlinksSurvivingParent },
		{ "DestroyLinkedRootUnlinksExternalParent", TestDestroyLinkedRootUnlinksExternalParent },
		{ "TransformParentCleanupPreservesPendingReparent", TestTransformParentCleanupPreservesPendingReparent },
		{ "TransformRemovalWorkIsLocal", TestTransformRemovalWorkIsLocal },
		{ "DirectTransformRemovalPreservesPendingEdges", TestDirectTransformRemovalPreservesPendingEdges },
		{ "EditorKeepWorldReparentUsesCurrentTransforms", TestEditorKeepWorldReparentUsesCurrentTransforms },
		{ "EditorKeepWorldReparentRejectsSingularParentWithoutMutation", TestEditorKeepWorldReparentRejectsSingularParentWithoutMutation },
		{ "EditorKeepWorldReparentPreservesMirroredTransform", TestEditorKeepWorldReparentPreservesMirroredTransform },
		{ "EditorKeepWorldReparentRejectsShearedCandidateWithoutMutation", TestEditorKeepWorldReparentRejectsShearedCandidateWithoutMutation },
		{ "WorldClearBatchesHierarchyAndStorageCleanup", TestWorldClearBatchesHierarchyAndStorageCleanup },
		{ "WorldClearDestroysDescendantsReparentedByEndPlay", TestWorldClearDestroysDescendantsReparentedByEndPlay },
		{ "WorldClearUnlinksAllPrefabsBeforeCallbacks", TestWorldClearUnlinksAllPrefabsBeforeCallbacks },
		{ "WorldClearRetainsPublishedAnimationAndLandscape", TestWorldClearRetainsPublishedAnimationAndLandscape },
		{ "RemovingComponentCancelsPendingDependencyResolution", TestRemovingComponentCancelsPendingDependencyResolution },
		{ "ExplicitNullMeshReferenceDoesNotRemainPending", TestExplicitNullMeshReferenceDoesNotRemainPending },
		{ "WorldRemovalWorkIsLocal", TestWorldRemovalWorkIsLocal },
		{ "WorldObjectInlineStorage", TestWorldObjectInlineStorage },
		{ "PendingDependenciesAreCancelledBeforeEndPlay", TestPendingDependenciesAreCancelledBeforeEndPlay },
		{ "PrefabRollbackPreservesUnrelatedPendingRequests", TestPrefabRollbackPreservesUnrelatedPendingRequests },
		{ "EditorUpdateReplacesStaleMeshDependencyResolution", TestEditorUpdateReplacesStaleMeshDependencyResolution },
		{ "EditorUpdatePreservesNewUnresolvedDependency", TestEditorUpdatePreservesNewUnresolvedDependency },
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
