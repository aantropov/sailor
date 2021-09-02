#pragma once
#include "Defines.h"
#include <atomic>

namespace Sailor
{
	using TRefPtrCounter = std::atomic<uint32_t>;
	class TRefPtrBase;

	class TRefBase
	{
	public:

		virtual ~TRefBase() = default;

	protected:

		TRefPtrCounter RefCounter = 0;
		friend class TRefPtrBase;
	};

	class TRefPtrBase
	{
	protected:

		TRefBase* m_pRawPtr = nullptr;

		TRefPtrCounter& GetRefCounter() const noexcept { return m_pRawPtr->RefCounter; }

		virtual ~TRefPtrBase() = default;
	};

	template<typename T>
	class TRefPtr final : public TRefPtrBase
	{
	public:

		template<typename... TArgs>
		static TRefPtr<T> Make(TArgs... args) noexcept
		{
			static_assert(std::is_base_of<TRefBase, T>::value);
			return TRefPtr<T>(new T(args...));
		}

		TRefPtr() noexcept
		{
			static_assert(std::is_base_of<TRefBase, T>::value);
		}

		TRefPtr(T* Ptr) noexcept
		{
			static_assert(std::is_base_of<TRefBase, T>::value);
			AssignRawPtr(Ptr);
		}

		template<class R>
		TRefPtr(const TRefPtr<R>& Ptr) noexcept
		{
			AssignRawPtr(Ptr.GetRawPtr());
		}

		TRefPtr(const TRefPtr<T>& Ptr) noexcept
		{
			AssignRawPtr(Ptr.GetRawPtr());
		}

		TRefPtr& operator=(T* Ptr)
		{
			AssignRawPtr(Ptr);
			return *this;
		}

		TRefPtr& operator=(const TRefPtr<T>& Ptr)
		{
			AssignRawPtr(Ptr.m_pRawPtr);
			return *this;
		}

		T* GetRawPtr() const noexcept { return static_cast<T*>(m_pRawPtr); }

		T* operator->()  noexcept { return static_cast<T*>(m_pRawPtr); }
		const T* operator->() const { return static_cast<T*>(m_pRawPtr); }

		T& operator*()  noexcept { return *static_cast<T*>(m_pRawPtr); }
		const T& operator*() const { return *static_cast<T*>(m_pRawPtr); }

		template<class R>
		TRefPtr& operator=(const TRefPtr<R>& Ptr)
		{
			AssignRawPtr(static_cast<TRefBase*>(Ptr.m_pRawPtr));
			return *this;
		}

		bool IsShared() const  noexcept { return GetRefCounter() > 1; }

		operator bool() const  noexcept { return m_pRawPtr != nullptr; }

		bool operator==(const TRefPtr<T>& Rhs) const
		{
			return m_pRawPtr == Rhs->m_pRawPtr;
		}

		bool operator!=(const TRefPtr<T>& Rhs) const
		{
			return m_pRawPtr != Rhs->m_pRawPtr;
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

		void AssignRawPtr(TRefBase* Ptr)
		{
			if (m_pRawPtr == Ptr)
			{
				return;
			}

			if (m_pRawPtr)
			{
				DecrementRefCounter();
			}

			if (m_pRawPtr = Ptr)
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