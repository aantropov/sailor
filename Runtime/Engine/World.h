#pragma once
#include "Sailor.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "Engine/Frame.h"
#include "ECS/ECS.h"

namespace Sailor
{
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using WorldPtr = class World*;

	class World
	{
	public:

		SAILOR_API World(std::string name);

		virtual SAILOR_API ~World() = default;

		SAILOR_API World(const World&) = delete;
		SAILOR_API World& operator=(const World&) = delete;

		SAILOR_API World(World&&) = default;
		SAILOR_API World& operator=(World&&) = default;

		SAILOR_API GameObjectPtr Instantiate(const std::string& name = "Untitled");
		SAILOR_API void Destroy(GameObjectPtr object);

		SAILOR_API void Tick(class FrameState& frameState);

		SAILOR_API Memory::ObjectAllocatorPtr GetAllocator() { return m_allocator; }
		SAILOR_API const Memory::ObjectAllocatorPtr& GetAllocator() const { return m_allocator; }

		SAILOR_API const FrameInputState& GetInput() const { return m_frameInput; }
		SAILOR_API int64_t GetTime() const { return m_time; }

		SAILOR_API RHI::CommandListPtr GetCommandList() const { return m_commandList; }

		template<typename T>
		SAILOR_API __forceinline T* GetECS()
		{
			const size_t typeId = T::GetComponentStaticType();
			return m_ecs[typeId].StaticCast<T>();
		}

		SAILOR_API TVector<GameObjectPtr> GetGameObjects() { return m_objects; }

		SAILOR_API void Clear();

	protected:

		std::string m_name;
		TVector<GameObjectPtr> m_objects;
		TList<GameObjectPtr, Memory::TInlineAllocator<sizeof(GameObjectPtr) * 32>> m_pendingDestroyObjects;
		TMap<size_t, Sailor::ECS::TBaseSystemPtr> m_ecs;
		
		FrameInputState m_frameInput;
		int64_t m_time;
		RHI::CommandListPtr m_commandList;

		Memory::ObjectAllocatorPtr m_allocator;
	};
}
