#pragma once
#include <cassert>
#include <type_traits>
#include "SharedPtr.hpp"

namespace Sailor
{
	template<typename T, typename TGlobalAllocator = Sailor::Memory::DefaultGlobalAllocator>
	class TWeakPtr final
	{
	public:

		TWeakPtr() noexcept = default;

		// Basic copy/assignment
		TWeakPtr(const TWeakPtr& pWeakPtr) noexcept
		{
			AssignRawPtr(pWeakPtr.m_pRawPtr, pWeakPtr.m_pControlBlock);
		}

		// Basic copy/assignment
		TWeakPtr& operator=(const TWeakPtr& pWeakPtr) noexcept
		{
			AssignRawPtr(pWeakPtr.m_pRawPtr, pWeakPtr.m_pControlBlock);
			return *this;
		}

		TWeakPtr(TWeakPtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
		}

		TWeakPtr& operator=(TWeakPtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
			return *this;
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TWeakPtr(const TWeakPtr<R>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TWeakPtr(TWeakPtr<R>&& pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TWeakPtr& operator=(TWeakPtr<R> pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
			return *this;
		}

		// Shared pointer
		TWeakPtr(const TSharedPtr<T>& pSharedPtr) noexcept
		{
			AssignRawPtr(pSharedPtr.m_pRawPtr, pSharedPtr.m_pControlBlock);
		}

		TWeakPtr& operator=(const TSharedPtr<T>& pSharedPtr) noexcept
		{
			AssignRawPtr(pSharedPtr.m_pRawPtr, pSharedPtr.m_pControlBlock);
			return *this;
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TWeakPtr(const TSharedPtr<R>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TWeakPtr& operator=(const TSharedPtr<R>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
			return *this;
		}

		TSharedPtr<T> Lock() const
		{
			check(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);

			TSharedPtr<T> pRes;
			pRes.AssignRawPtr(m_pRawPtr, m_pControlBlock);
			return pRes;
		}

		TSharedPtr<T> TryLock() const
		{
			TSharedPtr<T> pRes;
			if (!(*this))
			{
				return pRes;
			}

			pRes.AssignRawPtr(m_pRawPtr, m_pControlBlock);
			return pRes;
		}

		bool IsValid() const noexcept { return m_pRawPtr != nullptr; }
		explicit operator bool() const noexcept { return m_pRawPtr != nullptr && m_pControlBlock->m_sharedPtrCounter > 0; }

		bool operator==(const TWeakPtr<T>& pRhs) const
		{
			return m_pRawPtr == pRhs.m_pRawPtr;
		}

		bool operator!=(const TWeakPtr<T>& pRhs) const
		{
			return m_pRawPtr != pRhs.m_pRawPtr;
		}

		void Clear() noexcept
		{
			DecrementRefCounter();
			m_pRawPtr = nullptr;
			m_pControlBlock = nullptr;
		}

		~TWeakPtr()
		{
			DecrementRefCounter();
		}

		size_t GetHash() const
		{
			// TODO: implement hash_combine
			std::hash<const void*> p;
			return p(m_pControlBlock);
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
			if (m_pControlBlock != nullptr &&
				--m_pControlBlock->m_weakPtrCounter == 0 &&
				m_pControlBlock->m_sharedPtrCounter == 0)
			{
				TGlobalAllocator::free(m_pControlBlock);
				m_pControlBlock = nullptr;
				m_pRawPtr = nullptr;
			}
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> || std::is_same_v<T, R>>>
		void Swap(TWeakPtr<R>&& pPtr)
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

		friend class TWeakPtr;
		friend class TSharedPtr<T>;
	};
}

namespace std
{
	template<typename T>
	struct std::hash<Sailor::TWeakPtr<T>>
	{
		SAILOR_API std::size_t operator()(const Sailor::TWeakPtr<T>& p) const
		{
			return p.GetHash();
		}
	};
}