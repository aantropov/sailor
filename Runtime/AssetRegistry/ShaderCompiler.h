#pragma once
#include "ExportDef.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Singleton.hpp"
#include "ShaderCache.h"

namespace Sailor
{
	enum class EShaderKind
	{
		Vertex,
		Fragment
	};

	class ShaderAsset : IJsonSerializable
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

		static SAILOR_API void CompileAllPermutations(const UID& assetUID);
		SAILOR_API std::weak_ptr<ShaderAsset> LoadShaderAsset(const UID& uid);

		static SAILOR_API void GetSpirvCode(const UID& assetUID, const std::vector<std::string>& defines, std::vector<char>& outVertexByteCode, std::vector<char>& outFragmentByteCode);

		virtual SAILOR_API ~ShaderCompiler() override;

	protected:

		ShaderCache m_shaderCache;
		std::unordered_map<UID, std::shared_ptr<ShaderAsset>> m_loadedShaders;

		static SAILOR_API void GeneratePrecompiledGlsl(ShaderAsset* shader, std::string& outGLSLCode, const std::vector<std::string>& defines = {});
		static SAILOR_API void ConvertRawShaderToJson(const std::string& shaderText, std::string& outCodeInJSON);
		static SAILOR_API bool ConvertFromJsonToGlslCode(const std::string& shaderText, std::string& outPureGLSL);

		static SAILOR_API void ForceCompilePermutation(const UID& assetUID, unsigned int permutation);
		static SAILOR_API bool CompileGlslToSpirv(const std::string& source, const std::string& filename, EShaderKind shaderKind, const std::vector<std::string>& defines, const std::vector<std::string>& includes, std::vector<char>& outByteCode);
		
		static SAILOR_API unsigned int GetPermutation(const std::vector<std::string>& defines, const std::vector<std::string>& actualDefines);
		static SAILOR_API std::vector<std::string> GetDefines(const std::vector<std::string>& defines, unsigned int permutation);

	private:

		static constexpr char const* BeginCodeTag = "BEGIN_CODE";
		static constexpr char const* EndCodeTag = "END_CODE";
		static constexpr char const* EndLineTag = " END_LINE ";
	};
}
