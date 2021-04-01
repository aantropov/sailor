#pragma once
#include "Singleton.hpp"

using namespace std;

namespace Sailor
{
	struct ShaderCacheEntry
	{
	};

	struct ShaderCacheData
	{
	};

	class ShaderCache : Singleton<ShaderCache>
	{

	public:

		void Initialize();

	};
}
