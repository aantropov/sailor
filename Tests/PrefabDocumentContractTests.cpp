#include "Core/YamlUtils.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include "Components/MeshRendererComponent.h"
#include "Submodules/Editor.h"
#include "Support/TempDirectory.h"
#include "Support/PrefabTestDocument.h"
#include "Support/LifecyclePrefab.h"
#include "Support/ScopeExit.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <utility>

using namespace Sailor;
using namespace Sailor::Tests;

namespace
{
	void TestUndefinedComponentsPreserveSceneData()
	{
		PrefabTestWorld world;
		Tests::ScopeExit cleanup([&]() { world.Clear(); });
		YAML::Node knownProperties;
		knownProperties["m_value"] = 12.0f;
		YAML::Node unknownProperties;
		unknownProperties["settings"]["enabled"] = false;
		unknownProperties["settings"]["values"] = TVector<float>{ 3.5f, 8.0f };
		unknownProperties["target"]["instanceId"] = "2222222222222222_10010010010010010000";
		YAML::Node components;
		components.push_back(MakeReflectedComponent("2222222222222222_10010010010010010000", knownProperties));
		components.push_back(MakeReflectedComponent("3333333333333333_10010010010010010000",
			unknownProperties, true, "UnavailableModule::SceneComponent"));
		const auto document = MakeComponentPrefabNode(components);
		auto prefab = DeserializePrefab(world, document);
		std::string diagnostic;
		Require(prefab->ValidateForInstantiation(diagnostic), "Missing code must not invalidate scene data: " + diagnostic);
		auto root = world.Instantiate(prefab);
		Require(root && root->GetComponents().Num() == 2 && root->GetComponent<PrefabRollbackTestComponent>()->m_value == 12,
			"A missing workspace type must not prevent known components from loading");
		auto saved = PrefabDocumentTestAsset::Capture(world, root);
		Require(Utils::AreYamlNodesEqual(saved->Serialize()["components"][1], document["components"][1]),
			"Saving an undefined component must preserve its original typename, identity and nested values");
		auto duplicate = world.Instantiate(saved, EPrefabInstanceIdPolicy::GenerateNew);
		Require(duplicate && duplicate->GetInstanceId() != root->GetInstanceId(), "Undefined components must support duplication");
		const auto copy = duplicate->GetComponent(1)->GetReflectedData().Serialize();
		Require(copy["overrideProperties"]["target"]["instanceId"].as<InstanceId>() == duplicate->GetComponent(0)->GetInstanceId() &&
			copy["overrideProperties"]["instanceId"].as<InstanceId>() == duplicate->GetComponent(1)->GetInstanceId(),
			"Duplicating undefined data must remap internal references and preserve component identity");
		Require(Utils::AreYamlNodesEqual(saved->Serialize()["components"][1], document["components"][1]),
			"Duplicating undefined data must not change the source prefab");
		auto restored = world.Instantiate(DeserializePrefab(world, saved->Serialize()));
		Require(restored && restored->GetComponents().Num() == 2,
			"A world snapshot containing undefined components must remain loadable after saving");
		Require(world.GetPendingDependencyCount() == 0, "Undefined components must not wait forever for unavailable code");
	}

	void TestPrefabReferenceContextDoesNotCopyWorld()
	{
		for (uint32_t unrelatedCount : { 32u, 4096u })
		{
			PrefabTestWorld world;
			Tests::ScopeExit cleanup([&]() { world.Clear(); });
			for (uint32_t index = 0; index < unrelatedCount; ++index) world.Instantiate("Unrelated");
			auto external = world.Instantiate("External")->AddComponent<PrefabRollbackTestComponent>();
			auto source = world.Instantiate("Source");
			auto child = world.Instantiate("Source child");
			child->SetParent(source);
			auto local = child->AddComponent<PrefabRollbackTestComponent>();
			auto references = source->AddComponent<PrefabReferenceContextTestComponent>();
			references->m_local = local;
			references->m_external = external;
			references->m_owner = source;
			auto prefab = PrefabDocumentTestAsset::Capture(world, source);
			const size_t contextLimit = 2 * (2 + source->GetComponents().Num() + child->GetComponents().Num()) + 1;
			PrefabReferenceContextTestComponent::s_maxContextSize = 0;
			for (uint32_t instance = 0; instance < 16; ++instance)
			{
				const auto policy = (instance % 2) != 0
					? EPrefabInstanceIdPolicy::GenerateNew : EPrefabInstanceIdPolicy::PreserveAvailable;
				auto root = world.Instantiate(prefab, policy);
				Require(root && root != source && root->GetChildren().Num() == 1,
					"Small prefabs must instantiate alongside unrelated world objects and colliding source IDs");
				auto copy = root->GetComponent<PrefabReferenceContextTestComponent>();
				Require(copy && copy->m_owner == root && copy->m_external == external &&
					copy->m_local == root->GetChildren()[0]->GetComponent<PrefabRollbackTestComponent>() && copy->m_local != local,
					"One resolve must preserve both local-ID precedence and external component references");
			}
			const auto sourceId = source->GetInstanceId();
			const auto childId = child->GetInstanceId();
			const auto localId = local->GetInstanceId();
			const auto referenceId = references->GetInstanceId();
			const size_t objectCount = world.GetObjects().Num();
			Require(!world.Instantiate(prefab, EPrefabInstanceIdPolicy::RequireExact) &&
				world.GetObjects().Num() == objectCount,
				"Exact restoration must reject occupied IDs without changing the world");
			world.DestroyImmediate(source);
			auto restored = world.Instantiate(prefab, EPrefabInstanceIdPolicy::RequireExact);
			Require(restored && restored->GetInstanceId() == sourceId && restored->GetChildren().Num() == 1 &&
				restored->GetChildren()[0]->GetInstanceId() == childId,
				"Exact restoration must preserve hierarchy IDs among unrelated objects");
			auto restoredReferences = restored->GetComponent<PrefabReferenceContextTestComponent>();
			Require(restoredReferences && restoredReferences->GetInstanceId() == referenceId &&
				restoredReferences->m_owner == restored && restoredReferences->m_external == external &&
				restoredReferences->m_local && restoredReferences->m_local->GetInstanceId() == localId,
				"Exact restoration must resolve both restored components and the surviving external component");
			Require(world.GetPendingDependencyCount() == 0 && PrefabReferenceContextTestComponent::s_maxContextSize > 0 &&
				PrefabReferenceContextTestComponent::s_maxContextSize <= contextLimit,
				"Prefab resolution must materialize only local aliases and referenced external owners, not the whole world");
			std::cout << "Prefab lookup: " << unrelatedCount << " unrelated objects, 16 instances and exact restore, at most "
				<< PrefabReferenceContextTestComponent::s_maxContextSize << " context entries\n";
		}
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "test source should be readable: " + path.generic_string());
		std::string text = std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
		text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
		return text;
	}

	class EditorModelInstanceTestModel final : public Model
	{
	public:

		EditorModelInstanceTestModel() : Model(FileId::Invalid) {}

		void SetHierarchy(TVector<Node> nodes, bool bSupportsEditableHierarchy)
		{
			m_nodes = std::move(nodes);
			for (const auto& node : m_nodes)
			{
				if (node.m_meshIndex >= 0 &&
					static_cast<uint32_t>(node.m_meshIndex) >=
						m_sourceMeshes.Num())
				{
					m_sourceMeshes.Resize(
						static_cast<uint32_t>(node.m_meshIndex) + 1);
				}

				if (node.m_meshIndex >= 0)
				{
					auto& sourceMesh = m_sourceMeshes[
						static_cast<uint32_t>(node.m_meshIndex)];
					if (sourceMesh.m_renderMeshIndices.IsEmpty())
					{
						sourceMesh.m_renderMeshIndices.Add(0);
					}
				}
			}
			m_bSupportsEditableHierarchy = bSupportsEditableHierarchy;
			m_bIsReady.store(true, std::memory_order_release);
		}

		bool IsReady() const override
		{
			return m_bSimulatePendingGpuUpload ? false : Model::IsReady();
		}

		bool m_bSimulatePendingGpuUpload = false;
	};

	class WorldPrefabDocumentFixture final : public WorldPrefab
	{
	public:

		explicit WorldPrefabDocumentFixture(const FileId& fileId = FileId::Invalid) : WorldPrefab(fileId) {}

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

	void TestEditorModelInstanceCreatesHierarchyOrFlatRenderer()
	{
		PrefabTestWorld world;
		Editor editor(nullptr);
		editor.SetWorld(&world);

		auto model = TObjectPtr<EditorModelInstanceTestModel>::Make(
			world.GetAllocator());
		TVector<Model::Node> nodes;
		Model::Node pivot;
		pivot.m_name = "Pivot";
		pivot.m_sourceNodeIndex = 3;
		pivot.m_parentIndex = -1;
		pivot.m_localTransform.m_position = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
		nodes.Add(pivot);

		Model::Node firstMesh;
		firstMesh.m_name = "FirstMesh";
		firstMesh.m_sourceNodeIndex = 4;
		firstMesh.m_parentIndex = 0;
		firstMesh.m_meshIndex = 7;
		firstMesh.m_localTransform.m_position = glm::vec4(4.0f, 5.0f, 6.0f, 1.0f);
		nodes.Add(firstMesh);

		Model::Node repeatedMesh = firstMesh;
		repeatedMesh.m_name = "RepeatedMesh";
		repeatedMesh.m_sourceNodeIndex = 5;
		repeatedMesh.m_localTransform.m_position = glm::vec4(-4.0f, -5.0f, -6.0f, 1.0f);
		nodes.Add(repeatedMesh);
		model->SetHierarchy(std::move(nodes), true);
		model->m_bSimulatePendingGpuUpload = true;

		const InstanceId hierarchyRootId = InstanceId::GenerateNewInstanceId();
		InstanceId createdRootId;
		Require(
			editor.CreateModelInstance(
				model,
				"HierarchyModel",
				InstanceId::Invalid,
				true,
				nullptr,
				hierarchyRootId,
				createdRootId) &&
			createdRootId == hierarchyRootId,
			"hierarchy model creation must accept pending GPU uploads and preserve the preferred root id");

		auto hierarchyRoot = world
			.GetObjectByInstanceId(hierarchyRootId)
			.DynamicCast<GameObject>();
		Require(
			hierarchyRoot &&
			hierarchyRoot->GetName() == "HierarchyModel" &&
			!hierarchyRoot->GetComponent<MeshRendererComponent>() &&
			hierarchyRoot->GetChildren().Num() == 1,
			"editable model creation must use an asset root without a mesh renderer");
		auto pivotObject = hierarchyRoot->GetChildren()[0];
		Require(
			pivotObject->GetName() == "Pivot" &&
			pivotObject->GetChildren().Num() == 2 &&
			pivotObject->GetTransformComponent().GetPosition().x == 1.0f,
			"editable model creation must preserve the node parent and local transform");
		for (const auto& meshObject : pivotObject->GetChildren())
		{
			auto meshRenderer = meshObject->GetComponent<MeshRendererComponent>();
			Require(
				meshRenderer &&
				meshRenderer->GetModel() == model &&
				meshRenderer->GetMeshIndex() == 7,
				"repeated source-mesh references must create independent renderers with the same source mesh index");
		}

		const InstanceId flatRootId = InstanceId::GenerateNewInstanceId();
		Require(
			editor.CreateModelInstance(
				model,
				"FlatModel",
				InstanceId::Invalid,
				false,
				nullptr,
				flatRootId,
				createdRootId) &&
			createdRootId == flatRootId,
			"flat model creation must preserve the preferred root id");
		auto flatRoot = world
			.GetObjectByInstanceId(flatRootId)
			.DynamicCast<GameObject>();
		auto flatRenderer = flatRoot
			? flatRoot->GetComponent<MeshRendererComponent>()
			: MeshRendererComponentPtr{};
		Require(
			flatRoot &&
			flatRoot->GetChildren().IsEmpty() &&
			flatRenderer &&
			flatRenderer->GetModel() == model &&
			flatRenderer->GetMeshIndex() == Model::AllMeshes,
			"flat model creation must attach the complete model at meshIndex -1");

		auto largeModel = TObjectPtr<EditorModelInstanceTestModel>::Make(
			world.GetAllocator());
		TVector<Model::Node> largeNodes;
		constexpr uint32_t c_numLargeNodes = 1696;
		constexpr uint32_t c_numLargeMeshNodes = 1532;
		largeNodes.Reserve(c_numLargeNodes);
		for (uint32_t nodeIndex = 0; nodeIndex < c_numLargeNodes; ++nodeIndex)
		{
			Model::Node node;
			node.m_name = "LargeNode_" + std::to_string(nodeIndex);
			node.m_sourceNodeIndex = nodeIndex;
			node.m_parentIndex = -1;
			node.m_meshIndex = nodeIndex < c_numLargeMeshNodes ? 0 : Model::AllMeshes;
			largeNodes.Add(std::move(node));
		}
		largeModel->SetHierarchy(std::move(largeNodes), true);

		const InstanceId largeRootId = InstanceId::GenerateNewInstanceId();
		Require(
			editor.CreateModelInstance(
				largeModel,
				"LargeHierarchyModel",
				InstanceId::Invalid,
				true,
				nullptr,
				largeRootId,
				createdRootId) &&
			createdRootId == largeRootId,
			"Bistro-scale hierarchy creation must preserve the preferred root id");
		auto largeRoot = world
			.GetObjectByInstanceId(largeRootId)
			.DynamicCast<GameObject>();
		Require(
			largeRoot && largeRoot->GetChildren().Num() == c_numLargeNodes,
			"Bistro-scale hierarchy creation must keep every source node");

		world.Clear();
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

	void TestGameObjectMobilityHierarchyAndPersistence()
	{
		PrefabTestWorld world;
		auto parent = world.Instantiate("MobilityParent");
		auto child = world.Instantiate("MobilityChild");
		auto grandChild = world.Instantiate("MobilityGrandChild");

		child->SetMobilityType(EMobilityType::Static);
		child->SetParent(parent);
		Require(child->GetMobilityType() == EMobilityType::Stationary,
			"parenting must promote a less-movable child to the parent's mobility");

		grandChild->SetMobilityType(EMobilityType::Static);
		grandChild->SetParent(child);
		Require(grandChild->GetMobilityType() == EMobilityType::Stationary,
			"parenting must preserve the mobility invariant at every hierarchy level");

		parent->SetMobilityType(EMobilityType::Dynamic);
		Require(child->GetMobilityType() == EMobilityType::Dynamic &&
			grandChild->GetMobilityType() == EMobilityType::Dynamic,
			"making a parent more movable must promote its full descendant hierarchy");

		child->SetMobilityType(EMobilityType::Static);
		Require(child->GetMobilityType() == EMobilityType::Dynamic,
			"a child cannot be made less movable than its parent");

		child->SetParent(GameObjectPtr());
		child->SetMobilityType(EMobilityType::Static);
		Require(child->GetMobilityType() == EMobilityType::Static &&
			grandChild->GetMobilityType() == EMobilityType::Dynamic,
			"detached hierarchies may lower their root mobility without lowering more-movable descendants");
		world.Clear();

		auto persistedRoot = world.Instantiate("PersistedStaticRoot");
		auto persistedChild = world.Instantiate("PersistedStationaryChild");
		auto persistedGrandChild = world.Instantiate("PersistedDynamicGrandChild");
		persistedRoot->SetMobilityType(EMobilityType::Static);
		persistedChild->SetMobilityType(EMobilityType::Stationary);
		persistedGrandChild->SetMobilityType(EMobilityType::Dynamic);
		persistedChild->SetParent(persistedRoot);
		persistedGrandChild->SetParent(persistedChild);

		const InstanceId persistedRootId = persistedRoot->GetInstanceId();
		PrefabPtr captured = PrefabDocumentTestAsset::Capture(
			world,
			persistedRoot);
		const YAML::Node serialized = captured->Serialize();
		Require(serialized["gameObjects"][0]["mobilityType"].as<std::string>() ==
				"Static" &&
			serialized["gameObjects"][1]["mobilityType"].as<std::string>() ==
				"Stationary" &&
			serialized["gameObjects"][2]["mobilityType"].as<std::string>() ==
				"Dynamic",
			"prefab serialization must preserve GameObject mobility for the full hierarchy");

		world.DestroyImmediate(persistedRoot);
		GameObjectPtr restoredRoot = world.Instantiate(DeserializePrefab(world, serialized), EPrefabInstanceIdPolicy::RequireExact);
		Require(restoredRoot &&
			restoredRoot->GetInstanceId() == persistedRootId &&
			restoredRoot->GetMobilityType() == EMobilityType::Static &&
			restoredRoot->GetChildren().Num() == 1 &&
			restoredRoot->GetChildren()[0]->GetMobilityType() ==
				EMobilityType::Stationary &&
			restoredRoot->GetChildren()[0]->GetChildren().Num() == 1 &&
			restoredRoot->GetChildren()[0]->GetChildren()[0]->GetMobilityType() ==
				EMobilityType::Dynamic,
			"prefab instantiation must restore GameObject mobility without component-owned state");

		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		YAML::Node missingMobility = MakePrefabNode({ noParent, 0 });
		missingMobility["gameObjects"][0].remove("mobilityType");
		PrefabPtr incompletePrefab = DeserializePrefab(world, missingMobility);
		std::string diagnostic;
		Require(!incompletePrefab->ValidateForInstantiation(diagnostic) &&
			diagnostic.find("mobilityType") != std::string::npos,
			"prefab YAML must provide mobilityType for every GameObject");

		YAML::Node invalidHierarchy = MakePrefabNode({ noParent, 0 });
		invalidHierarchy["gameObjects"][0]["mobilityType"] = "Dynamic";
		invalidHierarchy["gameObjects"][1]["mobilityType"] = "Static";
		Prefab invalidPrefab{ FileId() };
		invalidPrefab.Deserialize(invalidHierarchy);
		Require(!invalidPrefab.ValidateForInstantiation(diagnostic) &&
			diagnostic.find("less movable") != std::string::npos,
			"serialized hierarchies with a less-movable child must be rejected before instantiation");

		auto edited = world.Instantiate("EditorMobility");
		edited->SetMobilityType(EMobilityType::Dynamic);
		YAML::Node editedYaml = PrefabDocumentTestAsset::Capture(
			world,
			edited)->Serialize()["gameObjects"][0];
		editedYaml["mobilityType"] = "Static";
		Editor editor(nullptr);
		editor.SetWorld(&world);
		Require(editor.UpdateObject(
				edited->GetInstanceId(),
				YAML::Dump(editedYaml)) &&
			edited->GetMobilityType() == EMobilityType::Static,
			"the editor GameObject update path must apply authored mobility");

		world.Clear();
	}

	void TestMeshRendererMaterialOverridesAreReflectedAndPersisted()
	{
		auto areOverridesEquivalent = [](const TVector<FileId>& lhs, const TVector<FileId>& rhs)
			{
				if (lhs.Num() != rhs.Num())
				{
					return false;
				}

				for (size_t materialIndex = 0; materialIndex < lhs.Num(); ++materialIndex)
				{
					if (static_cast<bool>(lhs[materialIndex]) != static_cast<bool>(rhs[materialIndex]) ||
						(lhs[materialIndex] && lhs[materialIndex] != rhs[materialIndex]))
					{
						return false;
					}
				}

				return true;
			};

		const auto& properties = MeshRendererComponent::GetStaticTypeInfo().Properties();
		Require(properties.ContainsKey("overrideMaterials") &&
			properties["overrideMaterials"] == "List<FileId>",
			"mesh renderer material overrides must be exported as an editable FileId list");
		Require(properties.ContainsKey("minLod") &&
			properties.ContainsKey("maxLod") &&
			properties.ContainsKey("screenCoverageThresholds") &&
			properties["screenCoverageThresholds"] == "List<float>",
			"mesh renderer LOD limits and screen-coverage thresholds must be editable reflected properties");

		PrefabTestWorld world;
		auto root = world.Instantiate("MaterialOverrides");
		auto meshRenderer = root->AddComponent<MeshRendererComponent>();
		Require(meshRenderer->GetOverrideMaterials().IsEmpty(),
			"a mesh renderer must not copy model defaults into its authored material overrides");
		const ReflectedData defaultReflection = meshRenderer->GetReflectedData();
		const bool bHasDefaultOverrides =
			defaultReflection.GetProperties().ContainsKey("overrideMaterials");
		const YAML::Node defaultOverrides = bHasDefaultOverrides
			? defaultReflection.GetProperties()["overrideMaterials"]
			: YAML::Node();
		Require(!bHasDefaultOverrides || defaultOverrides.IsNull() ||
			(defaultOverrides.IsSequence() && defaultOverrides.size() == 0),
			"a mesh renderer must omit or serialize its default material override list as empty: " +
			YAML::Dump(defaultOverrides));

		const FileId firstMaterial =
			DeserializeFileId("{11111111-AAAA-BBBB-CCCC-111111111111}");
		const FileId secondMaterial =
			DeserializeFileId("{22222222-AAAA-BBBB-CCCC-222222222222}");
		TVector<FileId> overrides{ firstMaterial, FileId::Invalid, secondMaterial };

		meshRenderer->SetOverrideMaterials(overrides);
		meshRenderer->SetMinLod(1u);
		meshRenderer->SetMaxLod(2u);
		meshRenderer->SetScreenCoverageThresholds(
			TVector<float>{ -1.0f, 0.25f, 2.0f });
		const TVector<float> clampedCoverageThresholds{
			1.0f, 0.25f, 0.0f };
		Require(
			meshRenderer->GetScreenCoverageThresholds() ==
				clampedCoverageThresholds,
			"mesh renderer screen coverage must stay normalized to [0, 1]");
		meshRenderer->SetScreenCoverageThresholds(
			TVector<float>{ 0.05f, 0.25f });
		Require(meshRenderer->GetOverrideMaterials() == overrides,
			"assigning material overrides must preserve their slot order and inherited gaps");
		Require(meshRenderer->GetData().IsDirty(),
			"assigning material overrides must invalidate the renderer ECS data");

		const ReflectedData reflection = meshRenderer->GetReflectedData();
		Require(reflection.GetProperties().ContainsKey("overrideMaterials"),
			"mesh renderer reflection must contain material overrides");
		const YAML::Node& reflectedOverrides =
			reflection.GetProperties()["overrideMaterials"];
		Require(reflectedOverrides.IsSequence(),
			"mesh renderer reflection must serialize material overrides as a sequence: " +
			YAML::Dump(reflectedOverrides));
		Require(areOverridesEquivalent(
				reflectedOverrides.as<TVector<FileId>>(), overrides),
			"mesh renderer reflection must serialize every override material slot: " +
			YAML::Dump(reflectedOverrides));

		PrefabPtr captured = PrefabDocumentTestAsset::Capture(world, root);
		const YAML::Node serializedPrefab = captured->Serialize();
		world.DestroyImmediate(root);

		PrefabPtr restoredPrefab = DeserializePrefab(world, serializedPrefab);
		auto restoredRoot = world.Instantiate(restoredPrefab, EPrefabInstanceIdPolicy::RequireExact);
		Require(static_cast<bool>(restoredRoot),
			"a prefab containing material overrides must survive a YAML round trip");
		auto restoredRenderer = restoredRoot->GetComponent<MeshRendererComponent>();
		Require(restoredRenderer && areOverridesEquivalent(
			restoredRenderer->GetOverrideMaterials(), overrides),
			"prefab instantiation must restore component-owned material overrides");
		const TVector<float> expectedCoverageThresholds{ 0.25f, 0.05f };
		Require(restoredRenderer->GetMinLod() == 1u &&
			restoredRenderer->GetMaxLod() == 2u &&
			restoredRenderer->GetScreenCoverageThresholds() ==
				expectedCoverageThresholds,
			"prefab instantiation must restore sorted mesh renderer LOD settings");

		restoredRenderer->SetModel(ModelPtr());
		Require(areOverridesEquivalent(
			restoredRenderer->GetOverrideMaterials(), overrides),
			"an unresolved or null model must not discard serialized material overrides");
		Require(restoredRenderer->GetMaterials().IsEmpty(),
			"a null model must clear resolved runtime materials while preserving override IDs");
		world.Clear();
	}

	InstanceId DeserializeInstanceId(const char* value)
	{
		InstanceId instanceId;
		instanceId.Deserialize(YAML::Node(value));
		return instanceId;
	}

	template<typename TDocument>
	void CheckSaveToFileResults(const TDocument& document, const char* filename)
	{
		Tests::TempDirectory directory("document-save");
		const auto path = directory.Path(filename);
		const auto metadataPath = directory.Path(std::string(filename) + ".asset");
		const auto backupPath = directory.Path("previous-source");
		const FileId fileId = document.GetFileId();
		const YAML::Node expected = document.Serialize();
		const std::string metadata = "fileId: \"" + fileId.ToString() +
			"\"\nfilename: \"" + filename + "\"\ncustom: preserved\n";
		auto writeText = [](const std::filesystem::path& target, const std::string& contents)
			{
				std::ofstream output(target, std::ios::binary | std::ios::trunc);
				output << contents;
				output.close();
				Require(static_cast<bool>(output), "the test fixture should write its own files");
			};
		auto requireIdentity = [&]()
			{
				Require(document.GetFileId() == fileId && ReadText(metadataPath) == metadata,
					"saving source content must not replace its FileId or rewrite its metadata sidecar");
			};
		auto requireSavedDocument = [&](const std::filesystem::path& target)
			{
				Require(Utils::AreYamlNodesEqual(YAML::Load(ReadText(target)), expected),
					"successful saves must contain the complete serialized document after parsing");
				requireIdentity();
			};

		writeText(metadataPath, metadata);
		Require(document.SaveToFile(path.generic_string()), "saving a new source file should succeed");
		requireSavedDocument(path);
		writeText(path, "stale source contents");
		Require(document.SaveToFile(path.generic_string()), "replacing an existing source file should succeed");
		requireSavedDocument(path);
		const std::string savedContents = ReadText(path);

		std::filesystem::rename(path, backupPath);
		Require(std::filesystem::create_directory(path), "the fixture should block the destination with a directory");
		const auto sentinelPath = path / "keep.txt";
		writeText(sentinelPath, "keep this directory");
		Require(!document.SaveToFile(path.generic_string()),
			"a failed atomic replacement must not be reported as a successful document save");
		Require(ReadText(sentinelPath) == "keep this directory" && ReadText(backupPath) == savedContents,
			"a blocked destination must leave the existing directory and saved source untouched");
		requireIdentity();
		for (const auto& entry : std::filesystem::directory_iterator(directory.Get()))
		{
			Require(entry.path() == path || entry.path() == metadataPath || entry.path() == backupPath,
				"failed publication must remove its temporary sibling file");
		}

		Require(std::filesystem::remove(sentinelPath) && std::filesystem::remove(path),
			"the fixture should remove only its own destination blocker");
		std::filesystem::rename(backupPath, path);
		Require(document.SaveToFile(path.generic_string()),
			"saving should retry successfully after the destination is restored");
		requireSavedDocument(path);

		const auto parent = directory.Path("blocked-parent");
		const auto nestedPath = parent / filename;
		writeText(parent, "not a directory");
		Require(!document.SaveToFile(nestedPath.generic_string()),
			"a regular file in the parent path must be reported as a write failure");
		Require(ReadText(parent) == "not a directory" && ReadText(path) == savedContents,
			"a failed parent-directory creation must preserve existing files");
		requireIdentity();
		Require(std::filesystem::remove(parent), "the fixture should remove its own parent-path blocker");
		Require(document.SaveToFile(nestedPath.generic_string()),
			"saving should retry successfully once its parent path can be created");
		requireSavedDocument(nestedPath);
		for (const auto& entry : std::filesystem::directory_iterator(parent))
		{
			Require(entry.path() == nestedPath,
				"saving source content must not create a sidecar or leave temporary files behind");
		}
		for (const auto& entry : std::filesystem::directory_iterator(directory.Get()))
		{
			Require(entry.path() == path || entry.path() == metadataPath || entry.path() == parent,
				"successful saves and retries must not leave temporary sibling files behind");
		}
	}

	void TestPrefabSaveReportsWriteFailures()
	{
		const FileId fileId = FileId::CreateNewFileId();
		Prefab prefab(fileId);
		prefab.Deserialize(MakePrefabNode({ static_cast<uint32_t>(-1), 0 }));
		std::string diagnostic;
		Require(prefab.ValidateForInstantiation(diagnostic), "the saved prefab fixture must be valid: " + diagnostic);
		CheckSaveToFileResults(prefab, "saved.prefab");
	}

	void TestWorldSaveReportsWriteFailures()
	{
		PrefabTestWorld sourceWorld;
		auto prefab = DeserializePrefab(sourceWorld, MakePrefabNode({ static_cast<uint32_t>(-1), 0 }));
		WorldPrefabDocumentFixture document(FileId::CreateNewFileId());
		document.AddPrefab(prefab);
		Require(document.IsReady(), "the saved world fixture must be ready");
		CheckSaveToFileResults(document, "saved.world");
	}

	void TestPrefabFactoryCreatesIndependentInlineAndAssetDocuments()
	{
		TempDirectory directory("prefab-factory");
		std::filesystem::create_directory(directory.Path("Content"));
		const auto context = Workspace::ResolveWorkspaceContext(directory.Get(), {});
		Require(context.IsSuccess(), "the prefab factory fixture must own its cache and content");
		AssetRegistry registry(context.m_context, nullptr);
		PrefabAssetInfoHandler handler(&registry);
		PrefabImporter importer(&handler);
		const FileId sourceId = FileId::CreateNewFileId();
		auto inlinePrefab = importer.Create();
		auto explicitInline = importer.Create(FileId::Invalid);
		auto first = importer.Create(sourceId);
		auto second = importer.Create(sourceId);
		Require(inlinePrefab && explicitInline && first && second &&
			inlinePrefab->GetFileId() == FileId::Invalid && explicitInline->GetFileId() == FileId::Invalid &&
			first->GetFileId() == sourceId && second->GetFileId() == sourceId,
			"inline factories must leave FileId unset; asset factories must preserve the requested identity");
		Require(inlinePrefab.GetRawPtr() != explicitInline.GetRawPtr() && first.GetRawPtr() != second.GetRawPtr() &&
			!inlinePrefab->IsReady() && !explicitInline->IsReady() && !first->IsReady() && !second->IsReady(),
			"Create must allocate independent unprepared documents, not cached or prevalidated assets");

		auto document = MakePrefabNode({ static_cast<uint32_t>(-1) });
		document["gameObjects"][0]["name"] = "First";
		first->Deserialize(document);
		document["gameObjects"][0]["name"] = "Second";
		second->Deserialize(document);
		document["gameObjects"][0]["name"] = "Inline";
		inlinePrefab->Deserialize(document);
		PrefabTestWorld world;
		auto firstRoot = world.Instantiate(first);
		auto secondRoot = world.Instantiate(second);
		auto inlineRoot = world.Instantiate(inlinePrefab);
		Require(firstRoot && secondRoot && inlineRoot && firstRoot->GetName() == "First" &&
			secondRoot->GetName() == "Second" && inlineRoot->GetName() == "Inline",
			"factory documents sharing a FileId must keep independent payloads through actual instantiation");
		Require(first->GetFileId() == sourceId && second->GetFileId() == sourceId &&
			inlinePrefab->GetFileId() == FileId::Invalid && explicitInline->Serialize()["gameObjects"].size() == 0,
			"deserialization must preserve factory identity and must not fill a different inline document");
		world.Clear();
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
		childOverride["mobilityType"] = "Dynamic";
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
			child->GetMobilityType() == EMobilityType::Dynamic &&
			child->GetTransformComponent().GetPosition() ==
				glm::vec4(7.0f, 8.0f, 9.0f, 1.0f),
			"linked instantiate should apply name, mobility, and transform overrides");

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

		Editor editor(nullptr);
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

	void TestGameplayPrefabCopiesAllowStructureChanges()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		const InstanceId sourceRootId = DeserializeInstanceId("10010010010010010000");
		const InstanceId sourceChildId = DeserializeInstanceId("20020020020020020000");
		const InstanceId sourceValueId = DeserializeInstanceId("2222222222222222_20020020020020020000");
		const InstanceId liveRootId = DeserializeInstanceId("30030030030030030000");
		const InstanceId liveChildId = DeserializeInstanceId("40040040040040040000");
		const InstanceId parentId = DeserializeInstanceId("50050050050050050000");
		const FileId sourceFileId = DeserializeFileId("{11111111-2222-3333-4444-555555555555}");

		for (const bool bLinkedRecord : { false, true })
		{
			PrefabTestWorld editor;
			PrefabTestWorld gameplay(static_cast<EWorldBehaviourMask>(EWorldBehaviourBit::CallBeginPlay));
			YAML::Node dependencyProperties;
			dependencyProperties["m_dependency"]["fileId"] = "NullFileId";
			dependencyProperties["m_dependency"]["instanceId"] = sourceValueId;
			YAML::Node valueProperties;
			valueProperties["m_value"] = 42.0f;
			YAML::Node sourceNode = MakePrefabNode({ noParent, 0 });
			sourceNode["gameObjects"][0]["instanceId"] = sourceRootId;
			sourceNode["gameObjects"][0]["components"].push_back(0);
			sourceNode["gameObjects"][1]["instanceId"] = sourceChildId;
			sourceNode["gameObjects"][1]["components"].push_back(1);
			sourceNode["components"].push_back(MakeReflectedComponent(
				"1111111111111111_10010010010010010000", dependencyProperties));
			sourceNode["components"].push_back(MakeReflectedComponent(sourceValueId.ToString(), valueProperties));
			PrefabPtr sourcePrefab = DeserializePrefab(editor, sourceFileId, sourceNode);
			const std::string sourceBefore = YAML::Dump(sourcePrefab->Serialize());
			PrefabPtr inputPrefab = sourcePrefab;
			WorldPrefabDocumentFixture document;

			if (bLinkedRecord)
			{
				TMap<InstanceId, InstanceId> instanceIds;
				instanceIds[sourceRootId] = liveRootId;
				instanceIds[sourceChildId] = liveChildId;
				TMap<InstanceId, YAML::Node> objectOverrides;
				objectOverrides[sourceChildId]["name"] = "OverriddenChild";
				objectOverrides[sourceChildId]["mobilityType"] = "Dynamic";
				objectOverrides[sourceChildId]["position"] = glm::vec4(7.0f, 8.0f, 9.0f, 0.0f);
				TMap<InstanceId, ReflectedData> componentOverrides;
				YAML::Node valueOverride;
				valueOverride["typename"] = PrefabRollbackTestComponent::GetStaticTypeInfo().Name();
				valueOverride["overrideProperties"]["m_value"] = 99.0f;
				componentOverrides[sourceValueId].Deserialize(valueOverride);
				PrefabPtr linkedPrefab = PrefabPtr::Make(editor.GetAllocator(), sourceFileId);
				std::string diagnostic;
				Require(linkedPrefab->ConfigureLinkedInstance(sourcePrefab, instanceIds, parentId,
					objectOverrides, componentOverrides, diagnostic),
					"the gameplay linked fixture should accept its overrides: " + diagnostic);
				document.AddPrefab(linkedPrefab);
				const YAML::Node persisted = YAML::Load(YAML::Dump(document.Serialize()))["prefabs"][0];
				inputPrefab = PrefabPtr::Make(editor.GetAllocator(), persisted["fileId"].as<FileId>());
				Require(inputPrefab->ConfigureLinkedInstance(sourcePrefab,
					persisted["instanceIds"].as<TMap<InstanceId, InstanceId>>(),
					persisted["parentInstanceId"].as<InstanceId>(),
					persisted["gameObjectOverrides"].as<TMap<InstanceId, YAML::Node>>(),
					persisted["componentOverrides"].as<TMap<InstanceId, ReflectedData>>(), diagnostic),
					"persisted linked data should resolve against its source: " + diagnostic);
			}
			const std::string documentBefore = YAML::Dump(document.Serialize());
			const std::string inputBefore = YAML::Dump(inputPrefab->Serialize());

			auto editorParent = editor.Instantiate("EditorParent", parentId);
			auto gameplayParent = gameplay.Instantiate("GameplayParent", parentId);
			auto editorRoot = editor.Instantiate(inputPrefab);
			auto root = gameplay.Instantiate(inputPrefab);
			Require(editorRoot && root && root->GetChildren().Num() == 1,
				"both world modes should instantiate the same prefab hierarchy");
			auto editorChild = editorRoot->GetChildren()[0];
			auto child = root->GetChildren()[0];
			auto dependency = root->GetComponent<PrefabRollbackTestComponent>();
			auto value = child->GetComponent<PrefabRollbackTestComponent>();
			Require(dependency && value && dependency->m_dependency == value,
				"gameplay copies should resolve internal references to their live components");
			Require(root->GetInstanceId() == (bLinkedRecord ? liveRootId : sourceRootId) &&
				child->GetInstanceId() == (bLinkedRecord ? liveChildId : sourceChildId) &&
				value->GetInstanceId() == InstanceId(sourceValueId.ComponentId(), child->GetInstanceId()),
				"removing authoring links must preserve serialized object and component identities");
			Require(value->m_value == (bLinkedRecord ? 99.0f : 42.0f),
				"gameplay copies should retain reflected values and linked overrides");
			if (bLinkedRecord)
			{
				Require(root->GetParent() == gameplayParent && editorRoot->GetParent() == editorParent &&
					child->GetName() == "OverriddenChild" && child->GetMobilityType() == EMobilityType::Dynamic &&
					child->GetTransformComponent().GetPosition() == glm::vec4(7.0f, 8.0f, 9.0f, 1.0f),
					"gameplay copies should retain their external parent and game-object overrides");
			}
			Require(editorRoot->GetFileId() == sourceFileId && editor.IsPrefabLinked(editorChild->GetInstanceId()),
				"the editor must retain source identity and authoring membership");
			const PrefabInstanceLink* link = nullptr;
			Require(!root->GetFileId() && !gameplay.IsPrefabInstanceRoot(root->GetInstanceId()) &&
				!gameplay.IsPrefabLinked(child->GetInstanceId()) &&
				!gameplay.TryGetPrefabInstance(root->GetInstanceId(), link) && gameplay.GetPrefabInstances().IsEmpty(),
				"gameplay copies must not retain authoring links or stale source mappings");

			auto editorValue = editorChild->GetComponent<PrefabRollbackTestComponent>();
			Require(!editorRoot->AddComponent<PrefabRollbackTestComponent>() && !editorChild->RemoveComponent(editorValue),
				"editor-linked instances should still reject component structure changes");
			editorChild->SetParent(editorParent);
			editor.DestroyImmediate(editorChild);
			Require(editorChild && editorChild->GetParent() == editorRoot,
				"editor-linked children should still reject reparenting and deletion");

			value->m_value = -7.0f;
			child->SetName("GameplayOnly");
			auto added = root->AddComponent<PrefabRollbackTestComponent>();
			Require(added && root->RemoveComponent(added) && child->RemoveComponent(value),
				"gameplay copies should allow adding and removing components");
			ComponentPtr rawComponent = TObjectPtr<PrefabRollbackTestComponent>::Make(gameplay.GetAllocator());
			Require(root->AddComponentRaw(rawComponent) && root->RemoveComponent(rawComponent),
				"gameplay copies should also allow the raw component API");
			child->SetParent(gameplayParent);
			Require(child->GetParent() == gameplayParent && root->GetChildren().IsEmpty(),
				"gameplay copies should allow reparenting internal children");
			child->SetParent(root);
			auto addedChild = gameplay.Instantiate("GameplayChild");
			addedChild->SetParent(root);
			Require(addedChild->GetParent() == root,
				"new gameplay objects should be allowed inside an instantiated hierarchy");
			const InstanceId childId = child->GetInstanceId();
			gameplay.DestroyImmediate(child);
			Require(!gameplay.GetObjectByInstanceId(childId) && root->GetChildren().Num() == 1 &&
				root->GetChildren()[0] == addedChild,
				"destroying a gameplay prefab child should remove only that live object");

			Require(sourcePrefab->GetFileId() == sourceFileId && YAML::Dump(sourcePrefab->Serialize()) == sourceBefore &&
				YAML::Dump(inputPrefab->Serialize()) == inputBefore && YAML::Dump(document.Serialize()) == documentBefore &&
				editorValue->m_value == (bLinkedRecord ? 99.0f : 42.0f),
				"gameplay changes must leave source data, persisted overrides, and the editor instance untouched");
			gameplay.Clear();
			editor.Clear();
		}
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
		evolvedSourceNode["gameObjects"][0]["mobilityType"] = "Dynamic";
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
		editedExpandedNode["gameObjects"][0]["mobilityType"] = "Static";
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
			gameObjectOverrides[sourceRoot]["mobilityType"].as<std::string>() ==
				"Static" &&
			componentOverrides.ContainsKey(sourceComponent) &&
			componentOverrides[sourceComponent].GetProperties()[
				"m_value"].as<float>() == 3.0f,
			"edits made after instantiation should become explicit overrides");

		TMap<InstanceId, YAML::Node> priorGameObjectOverrides;
		YAML::Node priorGameObjectOverride;
		priorGameObjectOverride["name"] = "PinnedName";
		priorGameObjectOverride["mobilityType"] = "Static";
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
		overriddenExpandedNode["gameObjects"][0]["mobilityType"] =
			"Static";
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
			gameObjectOverrides[sourceRoot]["mobilityType"].as<std::string>() ==
				"Static" &&
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
		Tests::TempDirectory failedSaveDirectory("linked-prefab-failed-save");
		const auto failedSavePath = failedSaveDirectory.Path("scene.world");
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
		Require(!nonReadyDocument.SaveToFile(failedSavePath.generic_string()) &&
			ReadText(failedSavePath) == "existing-scene",
			"a non-ready world document must preserve the previous scene without writing");

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
			world.Instantiate(linkedRecord, EPrefabInstanceIdPolicy::RequireExact);
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
		const size_t objectCountBeforeExpandedRestore = world.GetGameObjects().Num();
		Require(firstSavedExpanded->IsLinkedInstanceRecord() && !world.Instantiate(firstSavedExpanded) &&
			world.GetGameObjects().Num() == objectCountBeforeExpandedRestore,
			"expanded linked records must retain linked semantics but reject direct instantiation without mutation");
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
		root = world.Instantiate(secondLinkedRecord, EPrefabInstanceIdPolicy::RequireExact);
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

		Editor editor(nullptr);
		editor.SetWorld(&world);
		const size_t objectCountBeforeRejectedRestore =
			world.GetGameObjects().Num();
		Require(!editor.InstantiatePrefab(detachedSnapshot, liveRoot, EPrefabInstanceIdPolicy::PreserveAvailable) &&
			world.GetGameObjects().Num() ==
				objectCountBeforeRejectedRestore,
			"a detached snapshot must reject non-strict restore without mutation");

		GameObjectPtr wrongParent =
			world.Instantiate("WrongParent");
		Require(wrongParent &&
			!editor.InstantiatePrefab(detachedSnapshot, wrongParent->GetInstanceId(), EPrefabInstanceIdPolicy::RequireExact),
			"a detached snapshot must reject a call-parent mismatch");

		world.DestroyImmediate(detachedChild);
		const size_t objectCountBeforeRestore =
			world.GetGameObjects().Num();
		Require(editor.InstantiatePrefab(detachedSnapshot, liveRoot, EPrefabInstanceIdPolicy::RequireExact),
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
		Require(!editor.InstantiatePrefab(detachedSnapshot, liveRoot, EPrefabInstanceIdPolicy::RequireExact) &&
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
		Require(!world.Instantiate(linkedSnapshot, EPrefabInstanceIdPolicy::PreserveAvailable) &&
			!world.Instantiate(linkedSnapshot, EPrefabInstanceIdPolicy::RequireExact) &&
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
		Require(!editor.InstantiatePrefab(restoredLinkedRecord, InstanceId::Invalid, EPrefabInstanceIdPolicy::RequireExact) &&
			world.GetGameObjects().Num() ==
				objectCountBeforeDirectReject,
			"a linked-root exact-id collision should fail before partial mutation");

		world.DestroyImmediate(root);
		Require(editor.InstantiatePrefab(restoredLinkedRecord, InstanceId::Invalid, EPrefabInstanceIdPolicy::RequireExact),
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

	void TestForcedPrefabIdsRemapInternalAndPreserveExternalReferences()
	{
		constexpr uint32_t noParent = static_cast<uint32_t>(-1);
		constexpr const char* rootId =
			"10010010010010010000";
		constexpr const char* childId =
			"20020020020020020000";
		constexpr const char* externalOwnerId =
			"30030030030030030000";
		constexpr const char* sourceComponentId =
			"1111111111111111_10010010010010010000";
		constexpr const char* internalTargetId =
			"2222222222222222_20020020020020020000";
		constexpr const char* externalTargetId =
			"3333333333333333_30030030030030030000";

		PrefabTestWorld world;
		GameObjectPtr externalOwner = world.Instantiate(
			"ExternalTarget",
			DeserializeInstanceId(externalOwnerId));
		ComponentPtr externalTarget =
			TObjectPtr<PrefabRollbackTestComponent>::Make(
				world.GetAllocator());
		externalTarget = externalOwner->AddComponentRaw(
			externalTarget,
			DeserializeInstanceId(externalTargetId));
		Require(static_cast<bool>(externalTarget),
			"the external dependency fixture should be created");

		YAML::Node sourceProperties;
		sourceProperties["m_sourceDependency"]["fileId"] =
			"NullFileId";
		sourceProperties["m_sourceDependency"]["instanceId"] =
			internalTargetId;
		sourceProperties["m_liveDependency"]["fileId"] =
			"NullFileId";
		sourceProperties["m_liveDependency"]["instanceId"] =
			externalTargetId;

		YAML::Node components(YAML::NodeType::Sequence);
		components.push_back(MakeReflectedComponent(
			sourceComponentId,
			sourceProperties,
			true,
			PrefabMixedDependencyTestComponent::
				GetStaticTypeInfo().Name()));
		components.push_back(MakeReflectedComponent(
			internalTargetId,
			{}));

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

		GameObjectPtr copiedRoot = world.Instantiate(DeserializePrefab(world, prefabNode), EPrefabInstanceIdPolicy::GenerateNew);
		Require(copiedRoot &&
			copiedRoot->GetInstanceId() !=
				DeserializeInstanceId(rootId) &&
			copiedRoot->GetChildren().Num() == 1 &&
			copiedRoot->GetChildren()[0]->GetInstanceId() !=
				DeserializeInstanceId(childId),
			"forced prefab instantiation should assign new ids to every copied game object");

		auto copiedSource = copiedRoot->GetComponent<
			PrefabMixedDependencyTestComponent>();
		auto copiedInternalTarget = copiedRoot->GetChildren()[0]->
			GetComponent<PrefabRollbackTestComponent>();
		Require(copiedSource && copiedInternalTarget &&
			copiedSource->m_sourceDependency ==
				copiedInternalTarget &&
			copiedSource->m_liveDependency == externalTarget,
			"forced prefab instantiation should remap internal references and preserve external references");
		Require(world.GetPendingDependencyCount() == 0,
			"all copied references should resolve without pending work");

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
				world.Instantiate(prefab, EPrefabInstanceIdPolicy::RequireExact);
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
			Require(!world.Instantiate(prefab, EPrefabInstanceIdPolicy::RequireExact),
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

		Editor editor(nullptr);
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
		std::string_view message)
	{
		const size_t initialObjectCount = world.GetGameObjects().Num();
		const size_t initialPendingDependencyCount = world.GetPendingDependencyCount();
		PrefabPtr prefab = DeserializePrefab(world, prefabNode);

		if (world.Instantiate(prefab))
			throw std::runtime_error(std::string(message) + " should be rejected");
		if (world.GetGameObjects().Num() != initialObjectCount)
			throw std::runtime_error(std::string(message) + " must leave the world object count unchanged");
		if (world.GetPendingDependencyCount() != initialPendingDependencyCount)
			throw std::runtime_error(std::string(message) + " must leave the pending dependency queue unchanged");
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
		{ "UndefinedComponentsPreserveSceneData", TestUndefinedComponentsPreserveSceneData },
		{ "EditorModelInstanceCreatesHierarchyOrFlatRenderer", TestEditorModelInstanceCreatesHierarchyOrFlatRenderer },
		{ "PreferredEditorInstanceIdsArePreserved", TestPreferredEditorInstanceIdsArePreserved },
		{ "GameObjectMobilityHierarchyAndPersistence", TestGameObjectMobilityHierarchyAndPersistence },
		{ "MeshRendererMaterialOverridesAreReflectedAndPersisted", TestMeshRendererMaterialOverridesAreReflectedAndPersisted },
		{ "PrefabFactoryCreatesIndependentInlineAndAssetDocuments", TestPrefabFactoryCreatesIndependentInlineAndAssetDocuments },
		{ "PrefabSaveReportsWriteFailures", TestPrefabSaveReportsWriteFailures },
		{ "WorldSaveReportsWriteFailures", TestWorldSaveReportsWriteFailures },
		{ "PrefabComponentReferencesFollowRemappedOwners", TestPrefabComponentReferencesFollowRemappedOwners },
		{ "ForcedPrefabIdsRemapInternalAndPreserveExternalReferences", TestForcedPrefabIdsRemapInternalAndPreserveExternalReferences },
		{ "PrefabReferenceContextDoesNotCopyWorld", TestPrefabReferenceContextDoesNotCopyWorld },
		{ "LinkedPrefabPersistenceAndWorldContract", TestLinkedPrefabPersistenceAndWorldContract },
		{ "GameplayPrefabCopiesAllowStructureChanges", TestGameplayPrefabCopiesAllowStructureChanges },
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
