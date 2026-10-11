#pragma once
#include "Core/Defines.h"
#include "Containers/Concepts.h"
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace Sailor
{
	using TSmartPtrCounter = std::atomic<uint32_t>;
	class TRefPtrBase;

	template<typename T>
	class TRefPtr;

	class TRefBase
	{
	protected:

		TRefBase() = default;

	public:

		SAILOR_API virtual ~TRefBase() = default;

		template<typename T> requires IsBaseOf<TRefBase, T>
		SAILOR_API TRefPtr<T> ToRefPtr();

		SAILOR_API TRefBase(TRefBase&& copy) noexcept
			: m_refCounter(copy.m_refCounter.exchange(0))
		{

		}

		SAILOR_API TRefBase& operator =(TRefBase&& rhs) noexcept
		{
			if (this != &rhs)
			{
				m_refCounter.store(rhs.m_refCounter.exchange(0));
			}

			return *this;
		}

	protected:

		mutable TSmartPtrCounter m_refCounter = 0;
		friend class TRefPtrBase;
	};

	class SAILOR_API TRefPtrBase
	{
	public:

		size_t GetHash() const
		{
			std::hash<const void*> p;
			return p(m_pRawPtr);
		}

		bool operator==(const TRefPtrBase& pRhs) const
		{
			return m_pRawPtr == pRhs.m_pRawPtr;
		}

	protected:

		TRefPtrBase() = default;

		const TRefBase* m_pRawPtr = nullptr;
		TSmartPtrCounter& GetRefCounter() const noexcept { return m_pRawPtr->m_refCounter; }
	};

	template<typename T>
	class SAILOR_API TRefPtr final : public TRefPtrBase
	{
	public:

		template<typename... TArgs>
		static TRefPtr Make(TArgs&& ... args)
		{
			return TRefPtr(new T(std::forward<TArgs>(args)...));
		}

		TRefPtr() noexcept = default;
		TRefPtr(std::nullptr_t) noexcept {}

		// Raw pointers
		TRefPtr(T* pRawPtr) noexcept
		{
			AssignRawPtr(pRawPtr);
		}

		TRefPtr& operator=(T* pRawPtr)
		{
			AssignRawPtr(pRawPtr);
			return *this;
		}

		TRefPtr& operator=(std::nullptr_t) noexcept
		{
			Clear();
			return *this;
		}

		// Basic copy/assignment
		TRefPtr(const TRefPtr& pRefPtr) noexcept
		{
			AssignRawPtr(pRefPtr.m_pRawPtr);
		}

		TRefPtr(TRefPtr&& pRefPtr) noexcept
		{
			Swap(std::move(pRefPtr));
		}

		TRefPtr& operator=(TRefPtr pRefPtr) noexcept
		{
			Swap(std::move(pRefPtr));
			return *this;
		}

		// Other types copy/assignment
		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*>>>
		TRefPtr(const TRefPtr<R>& pRefPtr) noexcept
		{
			AssignRawPtr(pRefPtr.GetRawPtr());
		}

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*>>>
		TRefPtr(TRefPtr<R>&& pRefPtr) noexcept
		{
			Swap(std::move(pRefPtr));
		}

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*>>>
		TRefPtr& operator=(TRefPtr<R> pRefPtr) noexcept
		{
			Swap(std::move(pRefPtr));
			return *this;
		}

		template<typename R, typename = std::enable_if_t<!std::is_const_v<T> || std::is_const_v<R>>>
		TRefPtr<R> DynamicCast() const noexcept
		{
			return TRefPtr<R>(dynamic_cast<R*>(GetRawPtr()));
		}

		template<typename R, typename = std::enable_if_t<!std::is_const_v<T> || std::is_const_v<R>>>
		inline TRefPtr<R> StaticCast() const noexcept
		{
			return TRefPtr<R>(static_cast<R*>(GetRawPtr()));
		}

		// Constructors and conversions preserve T's constness across the erased base.
		T* GetRawPtr() const noexcept { return static_cast<T*>(const_cast<TRefBase*>(m_pRawPtr)); }

		T* operator->()  noexcept { return GetRawPtr(); }
		const T* operator->() const { return GetRawPtr(); }

		T& operator*()  noexcept { return *GetRawPtr(); }
		const T& operator*() const { return *GetRawPtr(); }

		uint32_t NumRefs() const noexcept { return GetRefCounter().load(); }
		bool IsShared() const  noexcept { return GetRefCounter() > 1; }
		bool IsValid() const noexcept { return m_pRawPtr != nullptr; }

		explicit operator bool() const noexcept { return m_pRawPtr != nullptr; }

		bool operator==(const TRefPtr& pRhs) const
		{
			return m_pRawPtr == pRhs.m_pRawPtr;
		}

		void Clear() noexcept
		{
			AssignRawPtr(nullptr);
		}

		~TRefPtr()
		{
			DecrementRefCounter();
		}

	protected:

	private:

		void AssignRawPtr(const TRefBase* pRawPtr)
		{
			if (m_pRawPtr == pRawPtr)
			{
				return;
			}

			if (m_pRawPtr)
			{
				DecrementRefCounter();
			}

			m_pRawPtr = pRawPtr;
			if (m_pRawPtr)
			{
				IncrementRefCounter();
			}
		}

		void IncrementRefCounter() const
		{
			++GetRefCounter();
		}

		void DecrementRefCounter()
		{
			if (m_pRawPtr != nullptr && --GetRefCounter() == 0)
			{
				delete m_pRawPtr;
				m_pRawPtr = nullptr;
			}
		}

		template<typename R, typename = std::enable_if_t<std::is_convertible_v<R*, T*>>>
		void Swap(TRefPtr<R>&& pRefPtr)
		{
			if (m_pRawPtr == pRefPtr.m_pRawPtr)
			{
				return;
			}

			if (m_pRawPtr)
			{
				DecrementRefCounter();
			}

			m_pRawPtr = pRefPtr.m_pRawPtr;
			pRefPtr.m_pRawPtr = nullptr;
		}
		
		template<typename>
		friend class TRefPtr;
	};

	template<typename T> requires IsBaseOf<TRefBase, T>
	TRefPtr<T> TRefBase::ToRefPtr()
	{
		return TRefPtr<T>(static_cast<T*>(this));
	}
}

namespace std
{
	template<>
	struct hash<Sailor::TRefPtrBase>
	{
		SAILOR_API std::size_t operator()(const Sailor::TRefPtrBase& p) const
		{
			return p.GetHash();
		}
	};

	template<typename T>
	struct hash<Sailor::TRefPtr<T>>
	{
		SAILOR_API std::size_t operator()(const Sailor::TRefPtr<T>& p) const
		{
			return p.GetHash();
		}
	};
}
