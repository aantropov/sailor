#pragma once
#include "Sailor.h"
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "ShaderCache.h"

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

			// Shader's code
			std::string glslVertex;
			std::string glslFragment;

			// Library code
			std::string glslCommon;

			std::vector<std::string> includes;
			std::vector<std::string> defines;
		};

		class ShaderCompiler final : Singleton<ShaderCompiler>
		{
		public:

			static void Initialize();
			
			static bool SAILOR_API CompileToSPIRV(const string& source, const vector<string>& defines, const vector<string>& includes, vector<uint32_t>& outByteCode);
			static SAILOR_API void CreatePrecompiledShaders(const UID& assetUID);

			virtual SAILOR_API ~ShaderCompiler() = default;
			
		protected:

			ShaderCache shaderCache;
			
			static SAILOR_API void JSONtify(std::string& shaderText);
		};

	}
}

namespace ns
{
	SAILOR_API void to_json(json& j, const Sailor::GfxDeviceVulkan::Shader& p);
	SAILOR_API void from_json(const json& j, Sailor::GfxDeviceVulkan::Shader& p);
}
