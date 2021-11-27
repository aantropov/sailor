#pragma once
#include "Core/Defines.h"
#include <string>
#include <vector>
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Singleton.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "ShaderAssetInfo.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "ShaderCache.h"

namespace Sailor
{
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

		SAILOR_API bool ContainsFragment() const { return !m_glslFragment.empty(); }
		SAILOR_API bool ContainsVertex() const { return !m_glslVertex.empty(); }
		SAILOR_API bool ContainsCommon() const { return !m_glslCommon.empty(); }

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData);
	};

	class ShaderCompiler final : public TSingleton<ShaderCompiler>, public IAssetInfoHandlerListener
	{
	public:
		static SAILOR_API void Initialize();

		static SAILOR_API void CompileAllPermutations(const UID& assetUID);

		SAILOR_API TWeakPtr<ShaderAsset> LoadShaderAsset(const UID& uid);

		static SAILOR_API void GetSpirvCode(const UID& assetUID, const std::vector<std::string>& defines, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, bool bIsDebug);

		virtual SAILOR_API ~ShaderCompiler() override;

		static SAILOR_API bool CompileGlslToSpirv(const std::string& source, RHI::EShaderStage shaderKind, const std::vector<std::string>& defines, const std::vector<std::string>& includes, RHI::ShaderByteCode& outByteCode, bool bIsDebug);

		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;
		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

	protected:

		ShaderCache m_shaderCache;
		std::unordered_map<UID, TSharedPtr<ShaderAsset>> m_loadedShaders;

		static SAILOR_API void GeneratePrecompiledGlsl(ShaderAsset* shader, std::string& outGLSLCode, const std::vector<std::string>& defines = {});
		static SAILOR_API void ConvertRawShaderToJson(const std::string& shaderText, std::string& outCodeInJSON);
		static SAILOR_API bool ConvertFromJsonToGlslCode(const std::string& shaderText, std::string& outPureGLSL);

		static SAILOR_API void ForceCompilePermutation(const UID& assetUID, uint32_t permutation);

		static SAILOR_API uint32_t GetPermutation(const std::vector<std::string>& defines, const std::vector<std::string>& actualDefines);
		static SAILOR_API std::vector<std::string> GetDefines(const std::vector<std::string>& defines, uint32_t permutation);

	private:

		static constexpr char const* JsonBeginCodeTag = "BEGIN_CODE";
		static constexpr char const* JsonEndCodeTag = "END_CODE";
		static constexpr char const* JsonEndLineTag = " END_LINE ";
	};
}
