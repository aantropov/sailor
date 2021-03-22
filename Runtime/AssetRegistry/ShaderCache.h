#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <shaderc/shaderc.h>

namespace Sailor
{
	class ShaderCache
	{

		
	private:
		
		std::vector<uint32_t> CompileToSPIRV(const char* sPath);
	};
}
