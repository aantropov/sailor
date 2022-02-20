#pragma once
#include "Sailor.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
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

		SAILOR_API void Tick(float deltaTime);

		SAILOR_API Memory::ObjectAllocatorPtr GetAllocator() { return m_allocator; }
		SAILOR_API const Memory::ObjectAllocatorPtr& GetAllocator() const { return m_allocator; }

		template<typename T>
		SAILOR_API __forceinline T* GetECS()
		{
			const size_t typeId = T::GetComponentStaticType();
			return m_ecs[typeId].StaticCast<T>();
		}

	protected:

		std::string m_name;
		TVector<GameObjectPtr> m_objects;
		TList<GameObjectPtr, Memory::TInlineAllocator<sizeof(GameObjectPtr) * 32>> m_pendingDestroyObjects;
		TMap<size_t, Sailor::ECS::TBaseSystemPtr> m_ecs;

		Memory::ObjectAllocatorPtr m_allocator;
	};
}
