#pragma once
#include "Sailor.h"
#include "JobSystem/JobSystem.h"
#include "Memory/ObjectPtr.hpp"
#include "Containers/Concepts.h"
#include "Core/Submodule.h"
#include "Memory/UniquePtr.hpp"

namespace Sailor::ECS
{
	template<typename TCustomData>
	struct TComponentData
	{
		size_t m_lastProcessedFrame = 0;
		TCustomData m_data;
	};

	using TBaseSystemPtr = TUniquePtr<class TBaseSystem>;

	
	class ECSFactory : public TSubmodule<ECSFactory>
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

	class TBaseSystem
	{
	public:

		virtual size_t RegisterComponent() = 0;
		virtual void UnregisterComponent(size_t index) = 0;

		virtual void BeginPlay() {}
		virtual void Tick(float deltaTime) {}
		virtual void EndPlay() {}

		size_t GetType() const { return std::type_index(typeid(*this)).hash_code(); }
		virtual size_t GetComponentType() const { return (size_t)-1; }
	};

	template<typename TData>
	class TSystem : public TBaseSystem
	{
		//static_assert(IsBaseOf<TData, TComponentData>::value, "TData must inherit from TComponentData");

	public:

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

		TData& GetComponentData(size_t index) { return m_components[index]; }

		static size_t GetStaticType() { return std::type_index(typeid(TSystem)).hash_code(); }
		static size_t GetComponentStaticType() { return std::type_index(typeid(TData)).hash_code(); }

		virtual size_t GetComponentType() const override { return TSystem::GetComponentStaticType(); }

	protected:

		TVector<TData> m_components;
		TList<size_t> m_freeList;

		class RegistrationFactoryMethod
		{
			RegistrationFactoryMethod()
			{
				ECSFactory::RegisterECS(GetComponentStaticType(), []() { return TUniquePtr<TSystem>::Make(); });
			}
		};

		static RegistrationFactoryMethod s_registrationFactoryMethod;
	};

	template<typename T>
	using TSystemPtr = TUniquePtr<class TSystem<T>>;
}
