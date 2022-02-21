#pragma once
#include <typeindex>
#include <iostream>
#include "Sailor.h"
#include "Core/Defines.h"
#include "JobSystem/JobSystem.h"
#include "Memory/ObjectPtr.hpp"
#include "Containers/Concepts.h"
#include "Core/Submodule.h"
#include "Memory/UniquePtr.hpp"

namespace Sailor::ECS
{
	constexpr size_t InvalidIndex = ((size_t)-1);

	using TBaseSystemPtr = TUniquePtr<class TBaseSystem>;

	class SAILOR_API ECSFactory : public TSubmodule<ECSFactory>
	{
	public:

		static void RegisterECS(size_t typeInfo, std::function<TBaseSystemPtr(void)> factoryMethod)
		{
			s_factoryMethods[typeInfo] = factoryMethod;
		}

		TVector<TBaseSystemPtr> CreateECS() const;

	protected:

		// Memory::MallocAllocator - due to there is no way to control the order of static members initialization
		static TMap<size_t, std::function<TBaseSystemPtr(void)>, Memory::MallocAllocator> s_factoryMethods;
	};

	class SAILOR_API TBaseSystem
	{
	public:

		virtual size_t RegisterComponent() = 0;
		virtual void UnregisterComponent(size_t index) = 0;

		virtual void BeginPlay() {}
		virtual JobSystem::ITaskPtr Tick(float deltaTime) = 0;
		virtual void EndPlay() {}

		virtual size_t GetComponentType() const { return (size_t)-1; }
	};

	template<typename TECS, typename TData>
	class SAILOR_API TSystem : public TBaseSystem
	{
		//static_assert(IsBaseOf<TData, TComponentData>::value, "TData must inherit from TComponentData");

	public:

		TSystem()
		{
			TSystem::s_registrationFactoryMethod;
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
			m_freeList.PushBack(index);
		}

		__forceinline TData& GetComponentData(size_t index) { return m_components[index]; }

		static size_t GetStaticType() { return std::type_index(typeid(TSystem)).hash_code(); }
		virtual size_t GetComponentType() const override { return TSystem::GetComponentStaticType(); }
		static size_t GetComponentStaticType() { return std::type_index(typeid(TData)).hash_code(); }

		__forceinline size_t GetComponentIndex(TData* rawPtr) const
		{
			const auto lhs = (size_t)(rawPtr);
			const auto rhs = (size_t)(&(m_components[0]));

			return (lhs - rhs) / sizeof(TData);
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

		protected:

			static bool s_bRegistered;
		};

		static volatile RegistrationFactoryMethod s_registrationFactoryMethod;
	};

	template<typename T, typename R>
	using TSystemPtr = TUniquePtr<class TSystem<T, R>>;

#ifndef _SAILOR_IMPORT_
	template<typename T, typename R>
	TSystem<T, R>::RegistrationFactoryMethod volatile TSystem<T, R>::s_registrationFactoryMethod;

	template<typename T, typename R>
	bool TSystem<T, R>::RegistrationFactoryMethod::s_bRegistered = false;
#endif
}
