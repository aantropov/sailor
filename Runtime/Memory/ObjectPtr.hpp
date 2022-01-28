#pragma once
#include <cassert>
#include <type_traits>
#include "SharedPtr.hpp"

namespace Sailor
{
	class Object;

	// The idea under TObjectPtr is proper handling of game objects with fuzzy ownership
	// Any exampler of TObjectPtr is
	// 1. Able to Destroy object by calling DestroyObject method
	// 2. Able to check that object is destroyed
	// That allows to write safe game related code and destroy the objects 
	// from the proper ownership, like World destroys GameObject, GameObject destroy Component
	// and all TObjectPtr pointers to destroyed objects aren't become dangling

	template<typename T>
	class TObjectPtr final
	{
		static_assert(std::is_base_of<Object, T>::value, "T must inherit from Object");

	public:

		template<typename... TArgs>
		static TObjectPtr<T> Make(TArgs&&... args) noexcept
		{
			return TObjectPtr<T>(new T(std::forward<TArgs>(args)...));
		}

		TObjectPtr() noexcept = default;

		// Raw pointers
		TObjectPtr(T* pRawPtr) noexcept
		{
			AssignRawPtr(pRawPtr, nullptr);

			// We're storing object
			m_pControlBlock->m_sharedPtrCounter = pRawPtr ? 1 : 0;
		}

		// Basic copy/assignment
		TObjectPtr(const TObjectPtr& pWeakPtr) noexcept
		{
			AssignRawPtr(pWeakPtr.m_pRawPtr, pWeakPtr.m_pControlBlock);
		}

		// Basic copy/assignment
		TObjectPtr& operator=(const TObjectPtr& pWeakPtr) noexcept
		{
			AssignRawPtr(pWeakPtr.m_pRawPtr, pWeakPtr.m_pControlBlock);
			return *this;
		}

		TObjectPtr(TObjectPtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
		}

		TObjectPtr& operator=(TObjectPtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
			return *this;
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TObjectPtr(const TObjectPtr<R>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TObjectPtr(TObjectPtr<R>&& pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TObjectPtr& operator=(TObjectPtr<R> pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
			return *this;
		}

		T* GetRawPtr() const noexcept 
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return m_pRawPtr; 
		}

		T* operator->()  noexcept 
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return m_pRawPtr; 
		}

		const T* operator->() const 
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return m_pRawPtr; 
		}

		T& operator*() noexcept 
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return *m_pRawPtr; 
		}

		const T& operator*() const 
		{
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);
			return *m_pRawPtr; 
		}

		operator bool() const noexcept { return m_pRawPtr != nullptr && m_pControlBlock->m_sharedPtrCounter > 0; }

		bool operator==(const TObjectPtr<T>& pRhs) const
		{
			return m_pRawPtr == pRhs.m_pRawPtr;
		}

		bool operator!=(const TObjectPtr<T>& pRhs) const
		{
			return m_pRawPtr != pRhs.m_pRawPtr;
		}

		void Clear() noexcept
		{
			DecrementRefCounter();
			m_pRawPtr = nullptr;
			m_pControlBlock = nullptr;
		}

		~TObjectPtr()
		{
			DecrementRefCounter();
		}

		size_t GetHash() const
		{
			// TODO: implement hash_combine
			std::hash<const void*> p;
			return p(m_pControlBlock);
		}

		void DestroyObject()
		{
			assert(m_pRawPtr && m_pControlBlock);
			assert(m_pControlBlock->m_weakPtrCounter > 0 && m_pControlBlock->m_sharedPtrCounter > 0);

			if (--m_pControlBlock->m_sharedPtrCounter == 0)
			{
				delete m_pRawPtr;
				m_pRawPtr = nullptr;
			}
		}

	protected:

	private:

		T* m_pRawPtr = nullptr;
		TSmartPtrControlBlock* m_pControlBlock = nullptr;

		void AssignRawPtr(T* pRawPtr, TSmartPtrControlBlock* pControlBlock)
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
				m_pControlBlock = pControlBlock;
				m_pRawPtr = pRawPtr;
				IncrementRefCounter();
			}
		}

		void IncrementRefCounter()
		{
			m_pControlBlock->m_weakPtrCounter++;
		}

		void DecrementRefCounter()
		{
			// If we dstroy the last -> we destroy object
			if (m_pControlBlock != nullptr && --m_pControlBlock->m_weakPtrCounter == 0)
			{
				if (m_pControlBlock->m_sharedPtrCounter > 0)
				{
					DestroyObject();
				}

				delete m_pControlBlock;
				m_pControlBlock = nullptr;
			}
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> || std::is_same_v<T, R>>>
		void Swap(TObjectPtr<R>&& pPtr)
		{
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

			pPtr.m_pRawPtr = nullptr;
			pPtr.m_pControlBlock = nullptr;
		}

		friend class TObjectPtr;
		friend class TSharedPtr<T>;
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