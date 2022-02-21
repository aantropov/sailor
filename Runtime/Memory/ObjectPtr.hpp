#pragma once
#include <cassert>
#include <type_traits>
#include "Core/Defines.h"
#include "SharedPtr.hpp"
#include "ObjectAllocator.hpp"

namespace Sailor
{
	class Object;

	// The idea under TObjectPtr is proper handling of game objects with fuzzy ownership
	// Any exampler of TObjectPtr is
	// 1. Able to Destroy object
	// 2. Able to check that object is destroyed
	// 3. To create the object you must specify ObjectAllocator
	// 4. To delete the object you should specify ObjectAllocator
	// That allows to write safe game related code and destroy the objects 
	// from the proper ownership, like World destroys GameObject, GameObject destroy Component
	// and all TObjectPtr pointers to destroyed objects aren't become dangling

	template<typename T>
	class TObjectPtr final
	{
		//static_assert(std::is_base_of<Object, T>::value, "T must inherit from Object");

	public:

		template<typename... TArgs>
		SAILOR_API static TObjectPtr<T> Make(Memory::ObjectAllocatorPtr pAllocator, TArgs&&... args) noexcept
		{
			void* ptr = pAllocator->Allocate(sizeof(T));
			auto pRes = TObjectPtr<T>(new (ptr) T(std::forward<TArgs>(args)...), pAllocator);
			pRes.m_pControlBlock->bAllocatedByCustomAllocator = true;

			return pRes;
		}

		TObjectPtr() noexcept = default;

		// Raw pointers
		SAILOR_API TObjectPtr(T* pRawPtr, Memory::ObjectAllocatorPtr pAllocator) noexcept
		{
			m_pAllocator = std::move(pAllocator);

			AssignRawPtr(pRawPtr, nullptr);

			// We're storing object
			m_pControlBlock->m_sharedPtrCounter = pRawPtr ? 1 : 0;
		}

		// Basic copy/assignment
		SAILOR_API TObjectPtr(const TObjectPtr& pObjectPtr) noexcept
		{
			m_pAllocator = pObjectPtr.m_pAllocator;
			AssignRawPtr(pObjectPtr.m_pRawPtr, pObjectPtr.m_pControlBlock);
		}

		// Basic copy/assignment
		SAILOR_API TObjectPtr& operator=(const TObjectPtr& pObjectPtr) noexcept
		{
			m_pAllocator = pObjectPtr.m_pAllocator;
			AssignRawPtr(pObjectPtr.m_pRawPtr, pObjectPtr.m_pControlBlock);
			return *this;
		}

		SAILOR_API TObjectPtr(TObjectPtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
		}

		SAILOR_API TObjectPtr& operator=(TObjectPtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
			return *this;
		}

		// We support this operator to properly write next code TObjectPtr<T> p = nullptr;
		SAILOR_API TObjectPtr& operator=(T* pRaw) noexcept
		{
			assert(!pRaw);
			Clear();
			return *this;
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		SAILOR_API TObjectPtr(const TObjectPtr<R>& pDerivedPtr) noexcept
		{
			m_pAllocator = pDerivedPtr.m_pAllocator;
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		SAILOR_API TObjectPtr(TObjectPtr<R>&& pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		SAILOR_API TObjectPtr& operator=(TObjectPtr<R> pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
			return *this;
		}

		SAILOR_API T* GetRawPtr() const noexcept
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return static_cast<T*>(m_pRawPtr);
		}

		template<typename R>
		SAILOR_API __forceinline TObjectPtr<R> StaticCast() { return TObjectPtr<R>(static_cast<R*>(m_pRawPtr), m_pAllocator); }

		template<typename R>
		SAILOR_API __forceinline TObjectPtr<R> DynamicCast() { return TObjectPtr<R>(dynamic_cast<R*>(m_pRawPtr), m_pAllocator); }

		SAILOR_API T* operator->()  noexcept
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return static_cast<T*>(m_pRawPtr);
		}

		SAILOR_API const T* operator->() const
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return static_cast<T*>(m_pRawPtr);
		}

		SAILOR_API T& operator*() noexcept
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return *static_cast<T*>(m_pRawPtr);
		}

		SAILOR_API const T& operator*() const
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return *static_cast<T*>(m_pRawPtr);
		}

		SAILOR_API operator bool() const noexcept { return m_pRawPtr != nullptr && m_pControlBlock->m_sharedPtrCounter > 0; }

		SAILOR_API bool operator==(const TObjectPtr<T>& pRhs) const
		{
			return m_pRawPtr == pRhs.m_pRawPtr;
		}

		SAILOR_API bool operator!=(const TObjectPtr<T>& pRhs) const
		{
			return m_pRawPtr != pRhs.m_pRawPtr;
		}

		SAILOR_API void Clear() noexcept
		{
			DecrementRefCounter();
			m_pRawPtr = nullptr;
			m_pControlBlock = nullptr;
			m_pAllocator.Clear();
		}

		SAILOR_API ~TObjectPtr()
		{
			DecrementRefCounter();
		}

		SAILOR_API size_t GetHash() const
		{
			// TODO: implement hash_combine
			std::hash<const void*> p;
			return p(m_pControlBlock);
		}

		// Only allocator handler could destroy the object by design
		SAILOR_API void DestroyObject(Memory::ObjectAllocatorPtr pAllocator)
		{
			assert(pAllocator == m_pAllocator);
			ForcelyDestroyObject();
		}

		// Only if you know what you're doing
		SAILOR_API void ForcelyDestroyObject()
		{
			assert(m_pRawPtr && m_pControlBlock);
			assert(m_pControlBlock->m_sharedPtrCounter > 0);

			if (--m_pControlBlock->m_sharedPtrCounter == 0)
			{
				m_pRawPtr->~Object();
				m_pAllocator->Free(m_pRawPtr);
				m_pRawPtr = nullptr;
			}
		}

	protected:

	private:

		Object* m_pRawPtr = nullptr;
		TSmartPtrControlBlock* m_pControlBlock = nullptr;
		Memory::ObjectAllocatorPtr m_pAllocator = nullptr;

		SAILOR_API void AssignRawPtr(Object* pRawPtr, TSmartPtrControlBlock* pControlBlock)
		{
			if (m_pRawPtr == pRawPtr)
			{
				return;
			}

			if (m_pRawPtr)
			{
				DecrementRefCounter();
			}

			m_pControlBlock = nullptr;
			m_pRawPtr = nullptr;

			if (pRawPtr)
			{
				m_pControlBlock = pControlBlock ? pControlBlock : new (m_pAllocator->Allocate(sizeof(TSmartPtrControlBlock))) TSmartPtrControlBlock();
				m_pRawPtr = pRawPtr;
				IncrementRefCounter();
			}
		}

		SAILOR_API void IncrementRefCounter()
		{
			m_pControlBlock->m_weakPtrCounter++;
		}

		SAILOR_API void DecrementRefCounter()
		{
			// If we destroy the last -> we destroy object
			if (m_pControlBlock != nullptr && --m_pControlBlock->m_weakPtrCounter == 0)
			{
				if (m_pControlBlock->m_sharedPtrCounter > 0)
				{
					ForcelyDestroyObject();
				}

				m_pAllocator->Free(m_pControlBlock);
				m_pControlBlock = nullptr;
				m_pAllocator.Clear();
			}
		}

		template<typename R>
		SAILOR_API void Swap(TObjectPtr<R>&& pPtr) requires IsBaseOf<R, T> || IsSame<T, R>
		{
			// We are sure that all the types are safe
			if (m_pRawPtr == static_cast<T*>(pPtr.m_pRawPtr))
			{
				return;
			}

			if (m_pRawPtr)
			{
				DecrementRefCounter();
			}

			m_pRawPtr = pPtr.m_pRawPtr;
			m_pControlBlock = pPtr.m_pControlBlock;
			m_pAllocator = pPtr.m_pAllocator;

			pPtr.m_pRawPtr = nullptr;
			pPtr.m_pControlBlock = nullptr;
			pPtr.m_pAllocator.Clear();
		}

		template<typename>
		friend class TObjectPtr;
	};
}

namespace std
{
	template<typename T>
	struct std::hash<Sailor::TObjectPtr<T>>
	{
		SAILOR_API std::size_t operator()(const Sailor::TObjectPtr<T>& p) const
		{
			return p.GetHash();
		}
	};
}