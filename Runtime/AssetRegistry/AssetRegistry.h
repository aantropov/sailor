#pragma once
#include <string>
#include <vector>

#include "Singleton.hpp"

namespace Sailor
{
	class Window;

	class AssetRegistry : public Singleton<AssetRegistry>
	{
	public:
		
		static bool ReadFile(const std::string& filename, std::vector<char>& buffer);
	};
}