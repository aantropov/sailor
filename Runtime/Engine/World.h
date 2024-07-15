#pragma once
#include "Sailor.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "Engine/Frame.h"
#include "Engine/Types.h"
#include "RHI/DebugContext.h"
#include "ECS/ECS.h"

namespace Sailor
{
	class World
	{
	public:

		SAILOR_API World(std::string name);

		SAILOR_API virtual ~World() = default;

		World(const World&) = delete;
		World& operator=(const World&) = delete;

		SAILOR_API World(World&&) = default;
		SAILOR_API World& operator=(World&&) = default;

		SAILOR_API GameObjectPtr Instantiate(PrefabPtr prefab);
		SAILOR_API GameObjectPtr Instantiate(const std::string& name = "Untitled");
		SAILOR_API void Destroy(GameObjectPtr object);

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

		SAILOR_API const std::string& GetName() const { return m_name; }

		SAILOR_API void ResolveExternalDependencies();

	protected:

		SAILOR_API GameObjectPtr NewGameObject(const std::string& name, const InstanceId& instanceId);

		float m_time{};
		float m_smoothDeltaTime = 0.016f;

		size_t m_currentFrame;
		std::string m_name;

		TVector<GameObjectPtr> m_objects;
		TMap<InstanceId, ObjectPtr> m_objectsMap;

		TVector<size_t> m_sortedEcs;
		TMap<size_t, Sailor::ECS::TBaseSystemPtr> m_ecs;

		FrameInputState m_frameInput;

		RHI::RHICommandListPtr m_commandList;
		TUniquePtr<RHI::DebugContext> m_pDebugContext;

		Memory::ObjectAllocatorPtr m_allocator;
		bool m_bIsBeginPlayCalled;

		TList<GameObjectPtr, Memory::TInlineAllocator<sizeof(GameObjectPtr) * 32>> m_pendingDestroyObjects;

		TVector<TPair<ComponentPtr, ReflectedData>> ComponentsToResolveDependencies;
	};
}
