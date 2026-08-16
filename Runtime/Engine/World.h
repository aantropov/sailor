#pragma once
#include "Sailor.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "Engine/Frame.h"
#include "Engine/Types.h"
#include "Engine/InstanceId.h"
#include "AssetRegistry/FileId.h"
#include "Containers/Map.h"
#include "RHI/DebugContext.h"
#include "ECS/ECS.h"

namespace Sailor
{
	enum class EWorldBehaviourBit : uint8_t
	{
		Tickable = 1 << 0,
		CallBeginPlay = 1 << 1,
		EcsTickable = 1 << 2,
		EditorTick = 1 << 3
	};

	typedef uint8_t EWorldBehaviourMask;

	// Runtime-only data derived for a linked prefab root. The source asset is
	// authoritative only on the live root GameObject::GetFileId().
	struct PrefabInstanceLink final
	{
		InstanceId m_rootInstanceId{};
		TMap<InstanceId, InstanceId> m_sourceToInstanceIds{};
		PrefabPtr m_effectiveBaseline{};
	};

	class World
	{
	public:

		SAILOR_API World(std::string name, EWorldBehaviourMask mask);

		SAILOR_API virtual ~World() = default;

		World(const World&) = delete;
		World& operator=(const World&) = delete;

		SAILOR_API World(World&&) = default;
		SAILOR_API World& operator=(World&&) = default;

		SAILOR_API GameObjectPtr Instantiate(PrefabPtr prefab);
		SAILOR_API GameObjectPtr Instantiate(
			PrefabPtr prefab,
			bool bStrictInstanceIds);
		SAILOR_API GameObjectPtr Instantiate(
			PrefabPtr prefab,
			bool bStrictInstanceIds,
			bool bForceNewInstanceIds);
		SAILOR_API GameObjectPtr Instantiate(const std::string& name = "Untitled");
		SAILOR_API GameObjectPtr Instantiate(const std::string& name, const InstanceId& preferredInstanceId);
		SAILOR_API void Destroy(GameObjectPtr object);
		SAILOR_API void DestroyImmediate(GameObjectPtr object);

		SAILOR_API void Tick(class FrameState& frameState);

		SAILOR_API Memory::ObjectAllocatorPtr GetAllocator() { return m_allocator; }
		SAILOR_API const Memory::ObjectAllocatorPtr& GetAllocator() const { return m_allocator; }

		SAILOR_API const FrameInputState& GetInput() const { return m_frameInput; }
		SAILOR_API float GetTime() const { return m_time; }

		SAILOR_API RHI::RHICommandListPtr GetCommandList() const { return m_commandList; }
		SAILOR_API TUniquePtr<class RHI::DebugContext>& GetDebugContext() { return m_pDebugContext; }

		template<typename T>
		SAILOR_API __forceinline T* GetECS()
		{
			const size_t typeId = T::GetComponentStaticType();
			return m_ecs[typeId].StaticCast<T>();
		}

		SAILOR_API TVector<GameObjectPtr> GetGameObjects() { return m_objects; }
		SAILOR_API const TVector<GameObjectPtr>& GetGameObjects() const { return m_objects; }

		SAILOR_API void Clear();
		SAILOR_API size_t GetCurrentFrame() const { return m_currentFrame; }
		SAILOR_API float GetSmoothDeltaTime() const { return m_smoothDeltaTime; }
		SAILOR_API void SetPhysicsSimulationEnabled(bool value) { m_bPhysicsSimulationEnabled = value; }
		SAILOR_API bool IsPhysicsSimulationEnabled() const { return m_bPhysicsSimulationEnabled; }

		SAILOR_API const std::string& GetName() const { return m_name; }

		SAILOR_API void ResolveExternalDependencies();
		SAILOR_API void SetEditorSelection(const TVector<InstanceId>& selection);
		SAILOR_API bool IsEditorSelected(const InstanceId& instanceId) const;

		SAILOR_API ObjectPtr GetObjectByInstanceId(const InstanceId& instanceId) const;

		SAILOR_API const TMap<InstanceId, ObjectPtr>& GetObjects() const { return m_objectsMap; }
		SAILOR_API const TMap<InstanceId, PrefabInstanceLink>& GetPrefabInstances() const { return m_prefabInstances; }
		SAILOR_API bool TryGetPrefabInstance(
			const InstanceId& objectInstanceId,
			const PrefabInstanceLink*& outLink) const;
		SAILOR_API bool IsPrefabLinked(const InstanceId& objectInstanceId) const;
		SAILOR_API bool IsPrefabInstanceRoot(const InstanceId& objectInstanceId) const;
		SAILOR_API bool LinkPrefabInstance(
			GameObjectPtr root,
			const PrefabPtr& sourcePrefab,
			std::string& outDiagnostic);
		SAILOR_API bool LinkPrefabInstance(
			GameObjectPtr root,
			const PrefabPtr& sourcePrefab,
			const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
			std::string& outDiagnostic);
		SAILOR_API bool BreakPrefabLink(
			const InstanceId& objectInstanceId,
			PrefabInstanceLink* outPreviousLink = nullptr);
		SAILOR_API bool CanModifyPrefabStructure(
			const InstanceId& objectInstanceId,
			std::string* outDiagnostic = nullptr) const;
		SAILOR_API bool CanReparentPrefabObject(
			const InstanceId& objectInstanceId,
			const InstanceId& parentInstanceId,
			std::string* outDiagnostic = nullptr) const;

	protected:

		SAILOR_API World(
			std::string name,
			EWorldBehaviourMask mask,
			TVector<ECS::TBaseSystemPtr>&& ecsArray);

		size_t GetNumPendingDependencyResolutions() const { return ComponentsToResolveDependencies.Num(); }
		void RemovePendingDependencyResolutions(const ComponentPtr& component);
		void ApplyComponentReflection(ComponentPtr component, const ReflectedData& reflection, bool bImmediate);

		SAILOR_API GameObjectPtr NewGameObject(const std::string& name, const InstanceId& instanceId);
		void DestroyGameObjectHierarchy(GameObjectPtr root);
		bool RegisterPrefabInstance(
			GameObjectPtr root,
			const FileId& sourcePrefabId,
			const TMap<InstanceId, InstanceId>& sourceToInstanceIds,
			const PrefabPtr& effectiveBaseline,
			std::string& outDiagnostic);
		void RemovePrefabLinksInHierarchy(GameObjectPtr root);

		float m_time{};
		float m_smoothDeltaTime = 0.016f;

		EWorldBehaviourMask m_mask;

		size_t m_currentFrame;
		std::string m_name;

		TVector<GameObjectPtr> m_objects;
		TMap<InstanceId, ObjectPtr> m_objectsMap;
		TMap<InstanceId, PrefabInstanceLink> m_prefabInstances;
		TMap<InstanceId, InstanceId> m_prefabInstanceRootsByObject;
		TSet<InstanceId> m_editorSelection;

		TVector<size_t> m_sortedEcs;
		TMap<size_t, Sailor::ECS::TBaseSystemPtr> m_ecs;

		FrameInputState m_frameInput;

		RHI::RHICommandListPtr m_commandList;
		TUniquePtr<RHI::DebugContext> m_pDebugContext;

		Memory::ObjectAllocatorPtr m_allocator;
		bool m_bIsBeginPlayCalled;
		bool m_bPhysicsSimulationEnabled = false;

		TList<GameObjectPtr, Memory::TInlineAllocator<sizeof(GameObjectPtr) * 32>> m_pendingDestroyObjects;

		TVector<TPair<ComponentPtr, ReflectedData>> ComponentsToResolveDependencies;

		friend class GameObject;
		friend class Editor;
		friend class WorldPrefab;
	};
}
