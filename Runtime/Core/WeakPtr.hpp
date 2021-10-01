#pragma once
#include <cassert>
#include <type_traits>
#include "SharedPtr.hpp"

namespace Sailor
{
	template<typename T>
	class TWeakPtr final : public TSmartPtrBase
	{
	public:

		TWeakPtr() noexcept = default;

		// Basic copy/assignment
		TWeakPtr(const TWeakPtr& pWeakPtr) noexcept
		{
			AssignRawPtr(pWeakPtr.m_pRawPtr, pWeakPtr.m_pControlBlock);
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
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.pControlBlock);
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
			assert(m_pControlBlock != nullptr && m_pControlBlock->m_sharedPtrCounter > 0);

			TSharedPtr<T> pRes;
			pRes.AssignRawPtr(m_pRawPtr, m_pControlBlock);
			return pRes;
		}

		operator bool() const noexcept { return m_pRawPtr != nullptr && m_pControlBlock->m_sharedPtrCounter > 0; }

		bool operator==(const TWeakPtr<T>& pRhs) const
		{
			return m_pRawPtr == pRhs->m_pRawPtr;
		}

		bool operator!=(const TWeakPtr<T>& pRhs) const
		{
			return m_pRawPtr != pRhs->m_pRawPtr;
		}

		void Clear() noexcept
		{
			DecrementRefCounter();
			m_pRawPtr = nullptr;
			m_pControlBlock = nullptr;
		}

		virtual ~TWeakPtr() override
		{
			DecrementRefCounter();
		}

	protected:

	private:

		T* m_pRawPtr = nullptr;

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

			m_pControlBlock = pControlBlock;

			if (m_pRawPtr = pRawPtr)
			{
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
				delete m_pControlBlock;
				m_pControlBlock = nullptr;
			}
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> || std::is_same_v<T, R>>>
		void Swap(TWeakPtr<R>&& pPtr)
		{
			if (m_pRawPtr == static_cast<T*>(pPtr.m_pRawPtr))
			{
				pPtr.m_pRawPtr = nullptr;
				pPtr.m_pControlBlock = nullptr;
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