#pragma once
#include "Defines.h"
#include <atomic>
#include <type_traits>

namespace Sailor
{
	template<typename T>
	class TUniquePtr final
	{
	public:

		template<typename... TArgs>
		static TUniquePtr<T> Make(TArgs&&... args) noexcept
		{
			return TUniquePtr<T>(new T(std::forward<TArgs>(args)...));
		}

		TUniquePtr() noexcept = default;

		// Delete
		TUniquePtr(const TUniquePtr& pPtr) noexcept = delete;
		TUniquePtr& operator=(const TUniquePtr& pPtr) = delete;

		// Raw pointers
		TUniquePtr(T* pRawPtr) noexcept
		{
			if (m_pRawPtr != pRawPtr)
			{
				Clear();
				m_pRawPtr = pRawPtr;
			}
		}

		TUniquePtr& operator=(T* pRawPtr)
		{
			if (m_pRawPtr != pRawPtr)
			{
				Clear();
				m_pRawPtr = pRawPtr;
			}
			return *this;
		}

		// Basic copy/assignment
		TUniquePtr(TUniquePtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
		}

		TUniquePtr& operator=(TUniquePtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
			return *this;
		}

		// Other types copy/assignment
		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TUniquePtr(TUniquePtr<R>&& pDerivedPtr) noexcept
		{
			Swap(std::move(pDerivedPtr));
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
		TUniquePtr& operator=(TUniquePtr<R> pPtr) noexcept
		{
			Swap(std::move(pPtr));
			return *this;
		}

		T* GetRawPtr() const noexcept { return m_pRawPtr; }

		const T* operator->() const { return m_pRawPtr; }
		T* operator->() { return m_pRawPtr; }

		T& operator*()  noexcept { return *m_pRawPtr; }
		const T& operator*() const { return *m_pRawPtr; }

		operator bool() const  noexcept { return m_pRawPtr != nullptr; }

		bool operator==(const TUniquePtr& pRhs) const
		{
			return m_pRawPtr == pRhs.m_pRawPtr;
		}

		bool operator!=(const TUniquePtr& pRhs) const
		{
			return m_pRawPtr != pRhs.m_pRawPtr;
		}

		void Clear() noexcept
		{
			delete m_pRawPtr;
			m_pRawPtr = nullptr;
		}

		virtual ~TUniquePtr()
		{
			Clear();
		}

	protected:

	private:

		T* m_pRawPtr = nullptr;

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> || std::is_same_v<T, R>>>
		void Swap(TUniquePtr<R>&& pPtr)
		{
			if (m_pRawPtr == static_cast<T*>(pPtr.m_pRawPtr))
			{
				// Don't waste the object
				pPtr.m_pRawPtr = nullptr;
				return;
			}

			Clear();

			m_pRawPtr = pPtr.m_pRawPtr;
			pPtr.m_pRawPtr = nullptr;
		}

		friend class TUniquePtr;
	};
}