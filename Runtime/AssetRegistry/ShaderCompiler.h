#pragma once
#include "ExportDef.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Singleton.hpp"
#include "ShaderCache.h"

namespace Sailor
{
	class Shader : IJsonSerializable
	{
	public:

		// Shader's code
		std::string m_glslVertex;
		std::string m_glslFragment;

		// Library code
		std::string m_glslCommon;

		std::vector<std::string> m_includes;
		std::vector<std::string> m_defines;

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData);
	};

	class ShaderCompiler final : public Singleton<ShaderCompiler>
	{
	public:

		static SAILOR_API void Initialize();

		static SAILOR_API void GeneratePrecompiledGlslPermutations(const UID& assetUID);
		SAILOR_API std::weak_ptr<Shader> LoadShader(const UID& uid);

		virtual SAILOR_API ~ShaderCompiler() override;

	protected:

		ShaderCache m_shaderCache;
		std::unordered_map<UID, std::shared_ptr<Shader>> m_loadedShaders;

		static SAILOR_API void GeneratePrecompiledGlsl(Shader* shader, std::string& outGLSLCode, const std::vector<std::string>& defines = {});
		static SAILOR_API void ConvertRawShaderToJson(const std::string& shaderText, std::string& outCodeInJSON);
		static SAILOR_API bool ConvertFromJsonToGlslCode(const std::string& shaderText, std::string& outPureGLSL);

		static bool SAILOR_API CompileGlslToSpirv(const std::string& source, const std::vector<std::string>& defines, const std::vector<std::string>& includes, std::vector<uint32_t>& outByteCode);

	private:

		static constexpr char const* BeginCodeTag = "BEGIN_CODE";
		static constexpr char const* EndCodeTag = "END_CODE";
		static constexpr char const* EndLineTag = " END_LINE ";
	};

}
