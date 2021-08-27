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

		TRefPtrCounter& GetRefCounter() const { return m_pRawPtr->RefCounter; }

		virtual ~TRefPtrBase() = default;
	};

	template<typename T>
	class TRefPtr : public TRefPtrBase
	{
	public:

		TRefPtr() : m_pRawPtr(nullptr) 
		{
			static_assert(std::is_base_of<TRefBase, T>);
		}

		explicit TRefPtr(T* Ptr)
		{
			AssignRawPtr(Ptr);
		}

		TRefPtr(const TRefPtr<T>& Ptr)
		{
			AssignRawPtr(reinterpret_cast<T*>(Ptr.m_pRawPtr));
		}

		TRefPtr& operator=(T* Ptr)
		{
			AssignRawPtr(Ptr);
		}

		TRefPtr& operator=(const TRefPtr<T>& Ptr)
		{
			AssignRawPtr(reinterpret_cast<T*>(Ptr.m_pRawPtr));
		}

		template<typename... TArgs>
		TRefPtr(TArgs... args)
		{
			AssignRawPtr(new T(args...));
		}

		template<typename... TArgs>
		TRefPtr& operator=(TArgs... args)
		{
			AssignRawPtr(new T(args...));
			return *this;
		}

		T* operator->() { return reinterpret_cast<T*>(m_pRawPtr); }
		const T* operator->() const { return reinterpret_cast<T*>(m_pRawPtr); }

		T& operator*() { return *reinterpret_cast<T*>(m_pRawPtr); }
		const T& operator*() const { return *reinterpret_cast<T*>(m_pRawPtr); }

		bool IsShared() const { return GetRefCounter() > 1; }

		operator bool() const { return m_pRawPtr != nullptr; }

		bool operator==(const TRefPtr<T>& Rhs) const
		{
			return m_pRawPtr == Rhs->m_pRawPtr;
		}

		bool operator!=(const TRefPtr<T>& Rhs) const
		{
			return m_pRawPtr != Rhs->m_pRawPtr;
		}

		void Clear()
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