#include "StringHash.h"
#include "Memory/Memory.h"
#include "Memory/UniquePtr.hpp"
#include "Containers/ConcurrentMap.h"
#include <mutex>
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
	return GetStrFromHashedStringsTable(*this);
}

StringHash StringHash::Runtime(std::string_view str)
{
	return StringHash{ str };
}

void StringHash::AddToHashedStringsTable(StringHash hash, std::string_view str)
{
	auto& strings = GetHashedStrings();
	if (!strings.ContainsKey(hash))
	{
		strings.Insert(hash, std::string(str));
	}
}

const std::string& StringHash::GetStrFromHashedStringsTable(StringHash hash)
{
	// Entries are never replaced or removed, and the table never rehashes.
	return GetHashedStrings()[hash];
}
