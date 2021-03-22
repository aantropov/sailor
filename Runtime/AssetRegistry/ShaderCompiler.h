#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <shaderc/shaderc.h>

using namespace std;

namespace Sailor
{
	namespace GfxDeviceVulkan
	{
		namespace ShaderCompiler
		{
			bool CompileToSPIRV(const string& source, const vector<string>& defines, const vector<string>& includes, vector<uint32_t>& outByteCode);
		}
	}
}
