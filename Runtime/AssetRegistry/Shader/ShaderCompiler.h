#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Pair.h"
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "ShaderAssetInfo.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "ShaderCache.h"
#include "JobSystem/Tasks.h"
#include "Framework/Object.h"

namespace Sailor
{
	class ShaderSet : public Object
	{
	public:

		// We aren't able to create shader without asset
		ShaderSet(UID uid) : Object(std::move(uid)) {}

		virtual bool IsReady() const override;

		const RHI::ShaderPtr& GetVertexShaderRHI() const { return m_rhiVertexShader; }
		RHI::ShaderPtr& GetVertexShaderRHI() { return m_rhiVertexShader; }

		const RHI::ShaderPtr& GetFragmentShaderRHI() const { return m_rhiFragmentShader; }
		RHI::ShaderPtr& GetFragmentShaderRHI() { return m_rhiFragmentShader; }

		const RHI::ShaderPtr& GetDebugVertexShaderRHI() const { return m_rhiVertexShaderDebug; }
		const RHI::ShaderPtr& GetDebugFragmentShaderRHI() const { return m_rhiFragmentShaderDebug; }

	protected:

		RHI::ShaderPtr m_rhiVertexShader;
		RHI::ShaderPtr m_rhiFragmentShader;

		RHI::ShaderPtr m_rhiVertexShaderDebug;
		RHI::ShaderPtr m_rhiFragmentShaderDebug;

		friend class ShaderCompiler;
	};

	using ShaderSetPtr = TWeakPtr<class ShaderSet>;

	class ShaderAsset : IJsonSerializable
	{
	public:

		const std::string& GetGlslVertexCode() const { return m_glslVertex; }
		const std::string& GetGlslFragmentCode() const { return m_glslFragment; }
		const std::string& GetGlslCommonCode() const { return m_glslCommon; }

		const TVector<std::string>& GetIncludes() const { return m_includes; }
		const TVector<std::string>& GetSupportedDefines() const { return m_defines; }

		SAILOR_API bool ContainsFragment() const { return !m_glslFragment.empty(); }
		SAILOR_API bool ContainsVertex() const { return !m_glslVertex.empty(); }
		SAILOR_API bool ContainsCommon() const { return !m_glslCommon.empty(); }

		virtual SAILOR_API void Serialize(nlohmann::json& outData) const;
		virtual SAILOR_API void Deserialize(const nlohmann::json& inData);

	protected:

		// Shader's code
		std::string m_glslVertex;
		std::string m_glslFragment;

		// Library code
		std::string m_glslCommon;

		TVector<std::string> m_includes;
		TVector<std::string> m_defines;
	};

	class ShaderCompiler final : public TSubmodule<ShaderCompiler>, public IAssetInfoHandlerListener
	{
	public:
		SAILOR_API ShaderCompiler(ShaderAssetInfoHandler* infoHandler);

		SAILOR_API JobSystem::TaskPtr<void, void> CompileAllPermutations(const UID& assetUID);
		SAILOR_API TWeakPtr<ShaderAsset> LoadShaderAsset(const UID& uid);

		virtual SAILOR_API ~ShaderCompiler() override;

		virtual SAILOR_API void OnImportAsset(AssetInfoPtr assetInfo) override;
		virtual SAILOR_API void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		SAILOR_API bool LoadShader_Immediate(UID uid, ShaderSetPtr& outShader, const TVector<string>& defines = {});
		SAILOR_API JobSystem::TaskPtr<bool> LoadShader(UID uid, ShaderSetPtr& outShader, const TVector<string>& defines = {});

	protected:

		ShaderCache m_shaderCache;

		TConcurrentMap<UID, TVector<TPair<uint32_t, JobSystem::TaskPtr<bool>>>> m_promises;
		TConcurrentMap<UID, TSharedPtr<ShaderAsset>> m_loadedShaderAssets;
		TConcurrentMap<UID, TVector<TPair<uint32_t, TSharedPtr<ShaderSet>>>> m_loadedShaders;

		// ShaderAsset related functions
		SAILOR_API static void GeneratePrecompiledGlsl(ShaderAsset* shader, std::string& outGLSLCode, const TVector<std::string>& defines = {});
		SAILOR_API static void ConvertRawShaderToJson(const std::string& shaderText, std::string& outCodeInJSON);
		SAILOR_API static bool ConvertFromJsonToGlslCode(const std::string& shaderText, std::string& outPureGLSL);

		// Compile related functions
		SAILOR_API void ForceCompilePermutation(const UID& assetUID, uint32_t permutation);
		SAILOR_API void GetSpirvCode(const UID& assetUID, const TVector<std::string>& defines, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, bool bIsDebug);
		SAILOR_API void GetSpirvCode(const UID& assetUID, uint32_t permutation, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, bool bIsDebug);
		SAILOR_API static bool CompileGlslToSpirv(const std::string& source, RHI::EShaderStage shaderKind, const TVector<std::string>& defines, const TVector<std::string>& includes, RHI::ShaderByteCode& outByteCode, bool bIsDebug);

		SAILOR_API static uint32_t GetPermutation(const TVector<std::string>& defines, const TVector<std::string>& actualDefines);
		SAILOR_API static TVector<std::string> GetDefines(const TVector<std::string>& defines, uint32_t permutation);

		SAILOR_API bool UpdateRHIResource(TSharedPtr<ShaderSet> shader, uint32_t permutation);

	private:

		static constexpr char const* JsonBeginCodeTag = "BEGIN_CODE";
		static constexpr char const* JsonEndCodeTag = "END_CODE";
		static constexpr char const* JsonEndLineTag = " END_LINE ";
	};
}
