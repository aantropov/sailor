#pragma once
#include "Defines.h"
#include <atomic>
#include <type_traits>

namespace Sailor
{
	using TSmartPtrCounter = std::atomic<uint32_t>;

	template<typename>
	class TWeakPtr;

	class TSmartPtrControlBlock
	{
	public:
		TSmartPtrCounter m_weakPtrCounter = 0;
		TSmartPtrCounter m_sharedPtrCounter = 0;
	};

	class TSmartPtrBase
	{
	protected:
		TSmartPtrControlBlock* m_pControlBlock = nullptr;

		virtual ~TSmartPtrBase() = default;
	};

	template<typename T>
	class TSharedPtr final : public TSmartPtrBase
	{
	public:

		template<typename... TArgs>
		static TSharedPtr<T> Make(TArgs&&... args) noexcept
		{
			return TSharedPtr<T>(new T(std::forward<TArgs>(args)...));
		}

		TSharedPtr() noexcept { }

		TSharedPtr(T* Ptr) noexcept
		{
			AssignRawPtr(Ptr, new TSmartPtrControlBlock());
		}

		template<typename R,
			typename = std::enable_if_t<
			std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
			TSharedPtr(const TSharedPtr<R>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.pControlBlock);
		}

		TSharedPtr(const TSharedPtr<T>& pSharedPtr) noexcept
		{
			AssignRawPtr(pSharedPtr.m_pRawPtr, pSharedPtr.m_pControlBlock);
		}

		TSharedPtr& operator=(T* Ptr)
		{
			AssignRawPtr(Ptr, new TSmartPtrControlBlock());
			return *this;
		}

		TSharedPtr& operator=(const TSharedPtr<T>& pDerivedPtr)
		{
			AssignRawPtr(pDerivedPtr.m_pRawPtr, pDerivedPtr.m_pControlBlock);
			return *this;
		}

		T* GetRawPtr() const noexcept { return m_pRawPtr; }

		T* operator->()  noexcept { return m_pRawPtr; }
		const T* operator->() const { return m_pRawPtr; }

		T& operator*()  noexcept { return *m_pRawPtr; }
		const T& operator*() const { return *m_pRawPtr; }

		template<typename R,
			typename = std::enable_if_t<
			std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
			TSharedPtr& operator=(const TSharedPtr<R>& pDerivedPtr)
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
			return *this;
		}

		bool IsShared() const  noexcept { return m_pRawPtr != nullptr && m_pControlBlock->m_sharedPtrCounter > 1; }

		operator bool() const  noexcept { return m_pRawPtr != nullptr; }

		bool operator==(const TSharedPtr<T>& pRhs) const
		{
			return m_pRawPtr == pRhs->m_pRawPtr;
		}

		bool operator!=(const TSharedPtr<T>& pRhs) const
		{
			return m_pRawPtr != pRhs->m_pRawPtr;
		}

		void Clear() noexcept
		{
			DecrementRefCounter();
			m_pRawPtr = nullptr;
			m_pControlBlock = nullptr;
		}

		virtual ~TSharedPtr() override
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
			m_pControlBlock->m_sharedPtrCounter++;
		}

		void DecrementRefCounter()
		{
			if (m_pRawPtr != nullptr &&
				--m_pControlBlock->m_sharedPtrCounter == 0)
			{
				delete m_pRawPtr;
				m_pRawPtr = nullptr;

				if (m_pControlBlock->m_weakPtrCounter == 0)
				{
					delete m_pControlBlock;
					m_pControlBlock = nullptr;
				}
			}
		}

		friend class TSharedPtr;

		template<typename>
		friend class TWeakPtr;
	};
}