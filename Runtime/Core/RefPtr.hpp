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

		TRefPtr() noexcept
		{
			static_assert(std::is_base_of<TRefBase, T>::value);
		}

		explicit TRefPtr(T* Ptr) noexcept
		{
			AssignRawPtr(Ptr);
		}

		TRefPtr(const TRefPtr<T>& Ptr) noexcept
		{
			AssignRawPtr(reinterpret_cast<T*>(Ptr.m_pRawPtr));
		}

		TRefPtr& operator=(T* Ptr)
		{
			AssignRawPtr(Ptr);
			return *this;
		}

		TRefPtr& operator=(const TRefPtr<T>& Ptr)
		{
			AssignRawPtr(reinterpret_cast<T*>(Ptr.m_pRawPtr));
			return *this;
		}

		template<typename... TArgs>
		TRefPtr(TArgs... args)
		{
			static_assert(std::is_base_of<TRefBase, T>::value);
			AssignRawPtr(new T(args...));
		}

		template<typename... TArgs>
		TRefPtr& operator=(TArgs... args)
		{
			AssignRawPtr(new T(args...));
			return *this;
		}

		T* operator->()  noexcept { return reinterpret_cast<T*>(m_pRawPtr); }
		const T* operator->() const { return reinterpret_cast<T*>(m_pRawPtr); }

		T& operator*()  noexcept { return *reinterpret_cast<T*>(m_pRawPtr); }
		const T& operator*() const { return *reinterpret_cast<T*>(m_pRawPtr); }

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

		void AssignRawPtr(T* Ptr)
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