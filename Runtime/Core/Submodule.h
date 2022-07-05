#pragma once
#include "Defines.h"
#include "Memory/UniquePtr.hpp"

namespace Sailor
{
	// Used to generate typeId
	class SAILOR_API SubmoduleBase
	{
	public:
		static constexpr int32_t InvalidSubmoduleTypeId = -1;

		virtual ~SubmoduleBase() noexcept = default;
		virtual void CollectGarbage() {}

	protected:
		static int32_t s_nextTypeId;
	};

	template<typename T>
	class SAILOR_API TSubmodule : public SubmoduleBase
	{
	public:

		virtual ~TSubmodule() noexcept = default;

		template<typename... TArgs>
		static TUniquePtr<TSubmodule> Make(TArgs&&... args)
		{
			// We're generating typeId by Submodule class only once
			if (TSubmodule::s_typeId == -1)
			{
				TSubmodule::s_typeId = SubmoduleBase::s_nextTypeId;
				SubmoduleBase::s_nextTypeId++;
			}

			return TUniquePtr<TSubmodule>(new T(std::forward<TArgs>(args)...));
		}

		static int32_t GetTypeId() { return s_typeId; }

		TSubmodule(TSubmodule&& move) noexcept = default;
		TSubmodule& operator =(TSubmodule&& rhs) noexcept = default;

	protected:

		TSubmodule() noexcept = default;
		TSubmodule(TSubmodule& copy) noexcept = delete;
		TSubmodule& operator =(TSubmodule& rhs) noexcept = delete;

		static int32_t s_typeId;
	};

#ifndef _SAILOR_IMPORT_
	template<typename T>
	int32_t TSubmodule<T>::s_typeId = SubmoduleBase::InvalidSubmoduleTypeId;
#endif
}