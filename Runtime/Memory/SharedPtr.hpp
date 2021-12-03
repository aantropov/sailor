#pragma once
#include "Core/Defines.h"
#include <atomic>
#include <cassert>
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
	public:

		virtual size_t Hash() const
		{
			std::hash<const void*> p;
			return p(m_pControlBlock);
		}

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

		TSharedPtr() noexcept = default;

		// Raw pointers
		TSharedPtr(T* pRawPtr) noexcept
		{
			AssignRawPtr(pRawPtr, nullptr);
		}

		TSharedPtr& operator=(T* pRawPtr)
		{
			AssignRawPtr(pRawPtr, nullptr);
			return *this;
		}

		// Basic copy/assignment
		TSharedPtr(const TSharedPtr& pSharedPtr) noexcept
		{
			AssignRawPtr(pSharedPtr.m_pRawPtr, pSharedPtr.m_pControlBlock);
		}

		TSharedPtr(TSharedPtr&& pSharedPtr) noexcept
		{
			Swap(std::move(pSharedPtr));
		}

		TSharedPtr& operator=(TSharedPtr pSharedPtr) noexcept
		{
			Swap(std::move(pSharedPtr));
			return *this;
		}

		// Other types copy/assignment
		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TSharedPtr(const TSharedPtr<R>& pDerivedPtr) noexcept
		{
			AssignRawPtr(static_cast<T*>(pDerivedPtr.m_pRawPtr), pDerivedPtr.m_pControlBlock);
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TSharedPtr(TSharedPtr<R>&& pSharedPtr) noexcept
		{
			Swap(std::move(pSharedPtr));
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TSharedPtr& operator=(TSharedPtr<R> pSharedPtr) noexcept
		{
			Swap(std::move(pSharedPtr));
			return *this;
		}

		T* GetRawPtr() const noexcept { return m_pRawPtr; }

		T* operator->()  noexcept { return m_pRawPtr; }
		const T* operator->() const { return m_pRawPtr; }

		T& operator*() noexcept { return *m_pRawPtr; }
		const T& operator*() const { return *m_pRawPtr; }

		bool IsShared() const  noexcept { return m_pRawPtr != nullptr && m_pControlBlock->m_sharedPtrCounter > 1; }

		operator bool() const  noexcept { return m_pRawPtr != nullptr; }

		bool operator==(const TSharedPtr& pRhs) const
		{
			return m_pRawPtr == pRhs.m_pRawPtr;
		}

		bool operator!=(const TSharedPtr& pRhs) const
		{
			return m_pRawPtr != pRhs.m_pRawPtr;
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

			m_pControlBlock = nullptr;
			m_pRawPtr = nullptr;

			if (m_pRawPtr = pRawPtr)
			{
				m_pControlBlock = !pControlBlock ? new TSmartPtrControlBlock() : pControlBlock;
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
				bool bShouldRemoveControlBlock = 0 == m_pControlBlock->m_weakPtrCounter;

				delete m_pRawPtr;
				m_pRawPtr = nullptr;

				if (bShouldRemoveControlBlock)
				{
					delete m_pControlBlock;
					m_pControlBlock = nullptr;
				}
			}
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> || std::is_same_v<T, R>>>
		void Swap(TSharedPtr<R>&& pSharedPtr)
		{
			if (m_pRawPtr == static_cast<T*>(pSharedPtr.m_pRawPtr))
			{
				return;
			}

			if (m_pRawPtr)
			{
				DecrementRefCounter();
			}

			m_pRawPtr = pSharedPtr.m_pRawPtr;
			m_pControlBlock = pSharedPtr.m_pControlBlock;

			pSharedPtr.m_pRawPtr = nullptr;
			pSharedPtr.m_pControlBlock = nullptr;
		}

		friend class TSharedPtr;

		template<typename>
		friend class TWeakPtr;
	};
}

namespace std
{
	template<>
	struct std::hash<Sailor::TSmartPtrBase>
	{
		SAILOR_API std::size_t operator()(const Sailor::TSmartPtrBase& p) const
		{
			return p.Hash();
		}
	};
}

namespace std
{
	template<typename T>
	struct std::hash<Sailor::TSharedPtr<T>>
	{
		SAILOR_API std::size_t operator()(const Sailor::TSharedPtr<T>& p) const
		{
			return p.Hash();
		}
	};
}