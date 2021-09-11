#pragma once
#include "Defines.h"
#include <atomic>
#include <type_traits>

namespace Sailor
{
	using TSmartPtrCounter = std::atomic<uint32_t>;
	class TRefPtrBase;

	class TRefBase
	{
	public:

		virtual ~TRefBase() = default;

	protected:

		TSmartPtrCounter RefCounter = 0;
		friend class TRefPtrBase;
	};

	class TRefPtrBase
	{
	protected:

		TRefBase* m_pRawPtr = nullptr;

		TSmartPtrCounter& GetRefCounter() const noexcept { return m_pRawPtr->RefCounter; }

		virtual ~TRefPtrBase() = default;
	};

	template<typename T>
	class TRefPtr final : public TRefPtrBase
	{
	public:

		template<typename... TArgs>
		static TRefPtr<T> Make(TArgs... args) noexcept
		{
			return TRefPtr<T>(new T(args...));
		}

		TRefPtr() noexcept = default;

		TRefPtr(T* Ptr) noexcept
		{
			AssignRawPtr(Ptr);
		}

		template<typename R,
			typename = std::enable_if_t<
			std::is_base_of_v<T, R> && !std::is_same_v<T, R>>>
			TRefPtr(const TRefPtr<R>& pRefPtr) noexcept
		{
			AssignRawPtr(pRefPtr.GetRawPtr());
		}

		TRefPtr(const TRefPtr<T>& pRawPtr) noexcept
		{
			AssignRawPtr(pRawPtr.GetRawPtr());
		}

		TRefPtr& operator=(T* pRawPtr)
		{
			AssignRawPtr(pRawPtr);
			return *this;
		}

		TRefPtr& operator=(const TRefPtr<T>& pRefPtr)
		{
			AssignRawPtr(pRefPtr.m_pRawPtr);
			return *this;
		}

		T* GetRawPtr() const noexcept { return static_cast<T*>(m_pRawPtr); }

		T* operator->()  noexcept { return static_cast<T*>(m_pRawPtr); }
		const T* operator->() const { return static_cast<T*>(m_pRawPtr); }

		T& operator*()  noexcept { return *static_cast<T*>(m_pRawPtr); }
		const T& operator*() const { return *static_cast<T*>(m_pRawPtr); }

		template<class R>
		TRefPtr& operator=(const TRefPtr<R>& pRefPtr)
		{
			AssignRawPtr(static_cast<TRefBase*>(pRefPtr.m_pRawPtr));
			return *this;
		}

		bool IsShared() const  noexcept { return GetRefCounter() > 1; }

		operator bool() const  noexcept { return m_pRawPtr != nullptr; }

		bool operator==(const TRefPtr<T>& pRhs) const
		{
			return m_pRawPtr == pRhs->GetRawPtr();
		}

		bool operator!=(const TRefPtr<T>& pRhs) const
		{
			return m_pRawPtr != pRhs->m_pRawPtr;
		}

		void Clear() noexcept
		{
			AssignRawPtr(nullptr);
		}

		virtual ~TRefPtr() override
		{
			DecrementRefCounter();
		}

	protected:

	private:

		void AssignRawPtr(TRefBase* pRawPtr)
		{
			if (m_pRawPtr == pRawPtr)
			{
				return;
			}

			if (m_pRawPtr)
			{
				DecrementRefCounter();
			}

			if (m_pRawPtr = pRawPtr)
			{
				IncrementRefCounter();
			}
		}

		void IncrementRefCounter()
		{
			GetRefCounter()++;
		}

		void DecrementRefCounter()
		{
			if (m_pRawPtr != nullptr && --GetRefCounter() == 0)
			{
				delete m_pRawPtr;
				m_pRawPtr = nullptr;
			}
		}
	};
}