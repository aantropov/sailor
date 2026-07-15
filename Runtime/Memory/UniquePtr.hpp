#pragma once
#include "Core/Defines.h"
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace Sailor
{
	template<typename T>
	class SAILOR_API TUniquePtr final
	{
	public:

		template<typename... TArgs>
		static TUniquePtr<T> Make(TArgs&&... args)
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

		__forceinline T* GetRawPtr() const noexcept { return m_pRawPtr; }

		template<typename R>
		__forceinline R* StaticCast() { return static_cast<R*>(m_pRawPtr); }

		template<typename R>
		__forceinline R* DynamicCast() { return dynamic_cast<R*>(m_pRawPtr); }

		const T* operator->() const { return m_pRawPtr; }
		T* operator->() { return m_pRawPtr; }

		T& operator*()  noexcept { return *m_pRawPtr; }
		const T& operator*() const { return *m_pRawPtr; }

		bool IsValid() const noexcept { return m_pRawPtr != nullptr; }

		explicit operator bool() const  noexcept { return m_pRawPtr != nullptr; }

		bool operator==(const TUniquePtr& pRhs) const
		{
			return m_pRawPtr == pRhs.m_pRawPtr;
		}

		bool operator!=(const TUniquePtr& pRhs) const
		{
			return m_pRawPtr != pRhs.m_pRawPtr;
		}

		T* Release() noexcept
		{
			T* pRawPtr = m_pRawPtr;
			m_pRawPtr = nullptr;
			return pRawPtr;
		}

		void Clear() noexcept
		{
			if (m_pRawPtr)
			{
				delete m_pRawPtr;
				m_pRawPtr = nullptr;
			}
		}

		~TUniquePtr()
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

			if (pPtr)
			{
				m_pRawPtr = pPtr.m_pRawPtr;
				pPtr.m_pRawPtr = nullptr;
			}
		}

		template<typename>
		friend class TUniquePtr;
	};

	template<typename T>
	class SAILOR_API TUniquePtr<T[]> final
	{
	public:

		static TUniquePtr<T[]> Make(size_t numElements)
		{
			return TUniquePtr<T[]>(new T[numElements]());
		}

		TUniquePtr() noexcept = default;
		TUniquePtr(const TUniquePtr&) noexcept = delete;
		TUniquePtr& operator=(const TUniquePtr&) = delete;

		explicit TUniquePtr(T* pRawPtr) noexcept : m_pRawPtr(pRawPtr) {}

		TUniquePtr(TUniquePtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
		}

		TUniquePtr& operator=(TUniquePtr&& pPtr) noexcept
		{
			Swap(std::move(pPtr));
			return *this;
		}

		T* GetRawPtr() const noexcept { return m_pRawPtr; }
		T& operator[](size_t index) noexcept { return m_pRawPtr[index]; }
		const T& operator[](size_t index) const noexcept { return m_pRawPtr[index]; }
		bool IsValid() const noexcept { return m_pRawPtr != nullptr; }
		explicit operator bool() const noexcept { return m_pRawPtr != nullptr; }

		T* Release() noexcept
		{
			T* pRawPtr = m_pRawPtr;
			m_pRawPtr = nullptr;
			return pRawPtr;
		}

		void Clear() noexcept
		{
			delete[] m_pRawPtr;
			m_pRawPtr = nullptr;
		}

		~TUniquePtr()
		{
			Clear();
		}

	private:

		T* m_pRawPtr = nullptr;

		void Swap(TUniquePtr&& pPtr) noexcept
		{
			if (this == &pPtr)
			{
				return;
			}

			Clear();
			m_pRawPtr = pPtr.m_pRawPtr;
			pPtr.m_pRawPtr = nullptr;
		}
	};
}
