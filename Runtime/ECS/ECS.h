#pragma once
#include <typeindex>
#include <iostream>
#include "Sailor.h"
#include "Core/Defines.h"
#include "Engine/Types.h"
#include "Tasks/Scheduler.h"
#include "Memory/ObjectPtr.hpp"
#include "Containers/Concepts.h"
#include "Core/Submodule.h"
#include "Memory/UniquePtr.hpp"

namespace Sailor::ECS
{
	constexpr size_t InvalidIndex = ((size_t)-1);

	class TComponent
	{
	public:

		SAILOR_API virtual void Clear() {}
		SAILOR_API virtual ~TComponent() = default;

		SAILOR_API virtual void MarkDirty() { m_bIsDirty = true; }

		SAILOR_API __forceinline void SetOwner(const ObjectPtr& owner) { m_owner = owner; }
		SAILOR_API __forceinline const ObjectPtr& GetOwner() const { return m_owner; }
		SAILOR_API __forceinline const size_t& GetFrameLastChange() const { return m_frameLastChange; }

	protected:

		ObjectPtr m_owner{};

		size_t m_frameLastChange = 0;
		bool m_bIsActive : 1 = true;
		bool m_bIsDirty : 1 = false;
	};

	using TBaseSystemPtr = TUniquePtr<class TBaseSystem>;

	class SAILOR_API ECSFactory : public TSubmodule<ECSFactory>
	{
	public:

		static void RegisterECS(size_t typeInfo, std::function<TBaseSystemPtr(void)> factoryMethod);
		TVector<TBaseSystemPtr> CreateECS() const;

	protected:
	};

	class SAILOR_API TBaseSystem
	{
	public:

		void Initialize(WorldPtr world) { m_world = world; }
		WorldPtr GetWorld() const { return m_world; }

		virtual size_t RegisterComponent() = 0;
		virtual void UnregisterComponent(size_t index) = 0;

		virtual void BeginPlay() {}
		virtual Tasks::ITaskPtr Tick(float deltaTime) = 0;
		virtual Tasks::ITaskPtr PostTick() { return nullptr; }
		virtual void EndPlay() {}

		virtual size_t GetComponentType() const { return (size_t)-1; }

		virtual uint32_t GetOrder() const { return 100; }

		void UpdateGameObject(GameObjectPtr gameObject, size_t lastFrameChanges);

		virtual ~TBaseSystem() = default;

	private:

		WorldPtr m_world{};
	};

	// Derive from TSystem (not from TBaseSystem), that is CRTP(Curiously recurrent template pattern)
	template<typename TECS, typename TData>
	class SAILOR_API TSystem : public TBaseSystem
	{
		static_assert(IsBaseOf<TComponent, TData>, "TData must inherit from TComponent");

	public:

		TSystem()
		{
			auto res = TSystem::s_registrationFactoryMethod;
			res.DoWork();
		}

		virtual size_t RegisterComponent() override
		{
			if (m_freeList.Num() == 0)
			{
				m_components.AddDefault(1);
				return m_components.Num() - 1;
			}

			size_t res = *(m_freeList.Last());
			m_freeList.PopBack();
			return res;
		}

		virtual void UnregisterComponent(size_t index) override
		{
			if (index != InvalidIndex)
			{
				m_components[index].Clear();
			}

			m_freeList.PushBack(index);
		}

		__forceinline TData& GetComponentData(size_t index) { return m_components[index]; }

		static size_t GetStaticType() { return std::type_index(typeid(TSystem)).hash_code(); }
		virtual size_t GetComponentType() const override { return TSystem::GetComponentStaticType(); }
		static size_t GetComponentStaticType() { return std::type_index(typeid(TData)).hash_code(); }

		__forceinline size_t GetComponentIndex(const TData* rawPtr) const
		{
			if (rawPtr == nullptr)
			{
				return ECS::InvalidIndex;
			}

			const intptr_t lhs = (intptr_t)(rawPtr);
			const intptr_t rhs = (intptr_t)(&(m_components[0]));
			const auto index = (lhs - rhs) / sizeof(TData);

			check(index < m_components.Num());

			return index;
		}

		virtual void EndPlay() override
		{
			m_components.Clear();
			m_freeList.Clear();
		}

	protected:

		TVector<TData> m_components;
		TList<size_t> m_freeList;

		class SAILOR_API RegistrationFactoryMethod
		{
		public:

			RegistrationFactoryMethod()
			{
				if (!s_bRegistered)
				{
					ECSFactory::RegisterECS(GetComponentStaticType(), []() { return TUniquePtr<TECS>::Make(); });
					s_bRegistered = true;
				}
			}

			void DoWork() {}

		protected:

			static bool s_bRegistered;
		};

		static RegistrationFactoryMethod s_registrationFactoryMethod;
	};

	template<typename T, typename R>
	using TSystemPtr = TUniquePtr<class TSystem<T, R>>;

#ifndef _SAILOR_IMPORT_
	template<typename T, typename R>
	typename TSystem<T, R>::RegistrationFactoryMethod TSystem<T, R>::s_registrationFactoryMethod;

	template<typename T, typename R>
	bool TSystem<T, R>::RegistrationFactoryMethod::s_bRegistered = false;
#endif

}
