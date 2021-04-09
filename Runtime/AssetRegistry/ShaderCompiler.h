#pragma once
#include "Sailor.h"
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>

using namespace std;
using namespace nlohmann;

namespace Sailor { class UID; }
namespace Sailor
{
	namespace GfxDeviceVulkan
	{
		class Shader
		{
		public:

			std::string glslCommon;

			std::string glslVertex;
			std::string glslFragment;

			std::vector<std::string> includes;
			std::vector<std::string> defines;
		};

		class ShaderCompiler
		{
		public:

			static bool SAILOR_API CompileToSPIRV(const string& source, const vector<string>& defines, const vector<string>& includes, vector<uint32_t>& outByteCode);

			static SAILOR_API void CreatePrecompiledShaders(const UID& assetUID);
		};

	}
}

namespace ns
{
	SAILOR_API void to_json(json& j, const Sailor::GfxDeviceVulkan::Shader& p);
	SAILOR_API void from_json(const json& j, Sailor::GfxDeviceVulkan::Shader& p);
}
