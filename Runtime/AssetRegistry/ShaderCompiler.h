#pragma once
#include "ExportDef.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Singleton.hpp"
#include "ShaderCache.h"

using namespace std;
using namespace nlohmann;

namespace Sailor { class UID; }
namespace Sailor
{
	class Shader
	{
	public:

		// Shader's code
		std::string m_glslVertex;
		std::string m_glslFragment;

		// Library code
		std::string m_glslCommon;

		std::vector<std::string> m_includes;
		std::vector<std::string> m_defines;
	};

	class ShaderCompiler final : public Singleton<ShaderCompiler>
	{
	public:

		static SAILOR_API void Initialize();

		static SAILOR_API void GeneratePrecompiledGLSLPermutations(const UID& assetUID, std::vector<std::string>& outPrecompiledShadersCode);
		SAILOR_API std::weak_ptr<Shader> LoadShader(const UID& uid);

		virtual SAILOR_API ~ShaderCompiler() override = default;

	protected:

		ShaderCache m_shaderCache;
		std::unordered_map<UID, std::shared_ptr<Shader>> m_loadedShaders;

		static SAILOR_API void GeneratePrecompiledGLSL(Shader* shader, std::string& outGLSLCode, const std::vector<std::string>& defines = {});
		static SAILOR_API void ConvertRawShaderToJSON(const std::string& shaderText, std::string& outCodeInJSON);
		static SAILOR_API bool ConvertFromJSONToGLSLCode(const std::string& shaderText, std::string& outPureGLSL);

		static bool SAILOR_API CompileGLSLToSPIRV(const std::string& source, const std::vector<std::string>& defines, const std::vector<std::string>& includes, std::vector<uint32_t>& outByteCode);

	private:

		static constexpr char const* BeginCodeTag = "BEGIN_CODE";
		static constexpr char const* EndCodeTag = "END_CODE";
		static constexpr char const* EndLineTag = " END_LINE ";
	};

}

namespace ns
{
	SAILOR_API void to_json(json& j, const Sailor::Shader& p);
	SAILOR_API void from_json(const json& j, Sailor::Shader& p);
}
