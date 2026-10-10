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

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*> && !std::is_same_v<T, R>>>
		TWeakPtr(const TWeakPtr<R, TGlobalAllocator>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
		}

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*> && !std::is_same_v<T, R>>>
		TWeakPtr(TWeakPtr<R, TGlobalAllocator>&& pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
		}

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*> && !std::is_same_v<T, R>>>
		TWeakPtr& operator=(TWeakPtr<R, TGlobalAllocator> pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
			return *this;
		}

		// Shared pointer
		TWeakPtr(const TSharedPtr<T, TGlobalAllocator>& pSharedPtr) noexcept
		{
			AssignRawPtr(pSharedPtr.m_pRawPtr, pSharedPtr.m_pControlBlock);
		}

		TWeakPtr& operator=(const TSharedPtr<T, TGlobalAllocator>& pSharedPtr) noexcept
		{
			AssignRawPtr(pSharedPtr.m_pRawPtr, pSharedPtr.m_pControlBlock);
			return *this;
		}

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*> && !std::is_same_v<T, R>>>
		TWeakPtr(const TSharedPtr<R, TGlobalAllocator>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
		}

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*> && !std::is_same_v<T, R>>>
		TWeakPtr& operator=(const TSharedPtr<R, TGlobalAllocator>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
			return *this;
		}

		TSharedPtr<T, TGlobalAllocator> Lock() const
		{
			auto pRes = TryLock();
			check(pRes);
			return pRes;
		}

		TSharedPtr<T, TGlobalAllocator> TryLock() const
		{
			TSharedPtr<T, TGlobalAllocator> pRes;
			if (!m_pControlBlock)
			{
				return pRes;
			}

			auto count = m_pControlBlock->m_sharedPtrCounter.load();
			while (count != 0)
			{
				if (m_pControlBlock->m_sharedPtrCounter.compare_exchange_weak(count, count + 1))
				{
					++m_pControlBlock->m_weakPtrCounter;
					pRes.m_pRawPtr = m_pRawPtr;
					pRes.m_pControlBlock = m_pControlBlock;
					break;
				}
			}
			return pRes;
		}

		bool IsValid() const noexcept { return m_pRawPtr != nullptr; }
		explicit operator bool() const noexcept { return m_pRawPtr != nullptr && m_pControlBlock->m_sharedPtrCounter > 0; }

		bool operator==(const TWeakPtr& pRhs) const
		{
			return m_pControlBlock == pRhs.m_pControlBlock && m_pRawPtr == pRhs.m_pRawPtr;
		}

		bool operator!=(const TWeakPtr& pRhs) const
		{
			return !(*this == pRhs);
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
			if (m_pRawPtr == pRawPtr && m_pControlBlock == pControlBlock)
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

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*>>>
		void Swap(TWeakPtr<R, TGlobalAllocator>&& pPtr)
		{
			if (static_cast<const void*>(this) == static_cast<const void*>(&pPtr))
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

		template<typename, typename>
		friend class TWeakPtr;
	};
}

namespace std
{
	template<typename T, typename TGlobalAllocator>
	struct hash<Sailor::TWeakPtr<T, TGlobalAllocator>>
	{
		SAILOR_API std::size_t operator()(const Sailor::TWeakPtr<T, TGlobalAllocator>& p) const
		{
			return p.GetHash();
		}
	};
}
