#pragma once
#include "Core/Defines.h"
#include <atomic>
#include <type_traits>

namespace Sailor
{
	using TSmartPtrCounter = std::atomic<uint32_t>;
	class TRefPtrBase;

	template<typename T>
	class TRefPtr;

	class TRefBase
	{
	public:

		SAILOR_API virtual ~TRefBase() = default;

		template<typename T> requires IsBaseOf<TRefBase, T>
		SAILOR_API TRefPtr<T> ToRefPtr();

	protected:

		TSmartPtrCounter m_refCounter = 0;
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

	protected:

		TRefPtrBase() = default;

		TRefBase* m_pRawPtr = nullptr;
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

		// Raw pointers
		TRefPtr(TRefBase* pRawPtr) noexcept
		{
			AssignRawPtr(pRawPtr);
		}

		TRefPtr& operator=(T* pRawPtr)
		{
			AssignRawPtr(pRawPtr);
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
		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R>>>
		TRefPtr(const TRefPtr<R>& pRefPtr) noexcept
		{
			AssignRawPtr(pRefPtr.GetRawPtr());
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R>>>
		TRefPtr(TRefPtr<R>&& pRefPtr) noexcept
		{
			Swap(std::move(pRefPtr));
		}

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R>>>
		TRefPtr& operator=(TRefPtr<R> pRefPtr) noexcept
		{
			Swap(std::move(pRefPtr));
			return *this;
		}

		template<typename R>
		TRefPtr<R> DynamicCast() noexcept
		{
			return TRefPtr<R>(dynamic_cast<R*>(GetRawPtr()));
		}

		template<typename R>
		inline TRefPtr<R> StaticCast() noexcept
		{
			return TRefPtr<R>(static_cast<R*>(GetRawPtr()));
		}

		T* GetRawPtr() const noexcept { return static_cast<T*>(m_pRawPtr); }

		T* operator->()  noexcept { return static_cast<T*>(m_pRawPtr); }
		const T* operator->() const { return static_cast<T*>(m_pRawPtr); }

		T& operator*()  noexcept { return *static_cast<T*>(m_pRawPtr); }
		const T& operator*() const { return *static_cast<T*>(m_pRawPtr); }

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

		template<typename R, typename = std::enable_if_t<std::is_base_of_v<T, R> || std::is_same_v<T, R>>>
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
	struct std::hash<Sailor::TRefPtrBase>
	{
		SAILOR_API std::size_t operator()(const Sailor::TRefPtrBase& p) const
		{
			return p.GetHash();
		}
	};

	template<typename T>
	struct std::hash<Sailor::TRefPtr<T>>
	{
		SAILOR_API std::size_t operator()(const Sailor::TRefPtr<T>& p) const
		{
			return p.GetHash();
		}
	};
}