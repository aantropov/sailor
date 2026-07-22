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
}

REFL_AUTO(
	type(Sailor::PrefabRollbackTestComponent, bases<Sailor::Component>),
	field(m_value),
	field(m_dependency)
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

	private:

		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			return systems;
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
		bool bIncludeInstanceId = true)
	{
		YAML::Node component;
		component["typename"] = PrefabRollbackTestComponent::GetStaticTypeInfo().Name();
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
		{ "AnimationRelayoutMarksEveryOwnedMeshDirty", TestAnimationRelayoutMarksEveryOwnedMeshDirty },
		{ "SparseLightSlotInvalidationAndReuse", TestSparseLightSlotInvalidationAndReuse },
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
