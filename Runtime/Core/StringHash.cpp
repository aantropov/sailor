#include "StringHash.h"
#include "Memory/Memory.h"
#include "Memory/UniquePtr.hpp"
#include "Containers/ConcurrentMap.h"
#include <type_traits>

using namespace Sailor;
using HashedStringsContainer = TConcurrentMap<StringHash, std::string, 32, ERehashPolicy::Never, Memory::MallocAllocator>;

namespace Sailor::Internal
{
	TUniquePtr<HashedStringsContainer> g_pHashedStrings;
}

HashedStringsContainer& GetHashedStrings()
{
	static ::std::once_flag once;
	::std::call_once(once, []
		{
			Internal::g_pHashedStrings = TUniquePtr<HashedStringsContainer>::Make(1024);
		});

	return *Internal::g_pHashedStrings;
}

const std::string& StringHash::ToString() const
{
	if (!std::is_constant_evaluated())
	{
		return GetStrFromHashedStringsTable(*this);
	}

	check(false);
	static const std::string s_notFound = "";
	return s_notFound;
}

StringHash StringHash::Runtime(std::string_view str)
{
	return StringHash{ str };
}

void StringHash::AddToHashedStringsTable(StringHash hash, std::string_view str)
{
	auto& HashedStrings = GetHashedStrings();

	auto it = HashedStrings.Find(hash);
	if (it == HashedStrings.end())
	{
		HashedStrings.At_Lock(hash, std::string(str));
		HashedStrings.Unlock(hash);
	}
	else
	{
		// Avoid hard-failing on rare hash collisions or cache races during debug startup.
		// Keep the first inserted string for this hash.
		if (it->Second() != str)
		{
			// Keep running in debug; preserving first mapping is safer than aborting.
			return;
		}
	}
}

const std::string& StringHash::GetStrFromHashedStringsTable(StringHash hash)
{
	return GetHashedStrings()[hash];
}
