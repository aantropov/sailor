#include "AssetRegistry.h"
#include <fstream>

using namespace Sailor;

bool AssetRegistry::ReadFile(const std::string& filename, std::vector<char>& buffer)
{
	std::ifstream file(filename, std::ios::ate | std::ios::binary);

	if (!file.is_open())
	{
		return false;
	}

	size_t fileSize = (size_t)file.tellg();
	buffer.clear();
	buffer.reserve(fileSize);

	file.seekg(0);
	file.read(buffer.data(), fileSize);

	file.close();
	return true;
}