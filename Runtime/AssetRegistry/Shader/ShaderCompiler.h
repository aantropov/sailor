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
#include "ShaderCache.h"
#include "JobSystem/Tasks.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"

namespace Sailor
{
	class ShaderSet : public Object
	{
	public:

		// We aren't able to create shader without asset
		SAILOR_API ShaderSet(UID uid) : Object(std::move(uid)) {}

		SAILOR_API virtual bool IsReady() const override;

		SAILOR_API const RHI::RHIShaderPtr& GetVertexShaderRHI() const { return m_rhiVertexShader; }
		SAILOR_API RHI::RHIShaderPtr& GetVertexShaderRHI() { return m_rhiVertexShader; }

		SAILOR_API const RHI::RHIShaderPtr& GetFragmentShaderRHI() const { return m_rhiFragmentShader; }
		SAILOR_API RHI::RHIShaderPtr& GetFragmentShaderRHI() { return m_rhiFragmentShader; }

		SAILOR_API const RHI::RHIShaderPtr& GetDebugVertexShaderRHI() const { return m_rhiVertexShaderDebug; }
		SAILOR_API const RHI::RHIShaderPtr& GetDebugFragmentShaderRHI() const { return m_rhiFragmentShaderDebug; }

	protected:

		RHI::RHIShaderPtr m_rhiVertexShader;
		RHI::RHIShaderPtr m_rhiFragmentShader;

		RHI::RHIShaderPtr m_rhiVertexShaderDebug;
		RHI::RHIShaderPtr m_rhiFragmentShaderDebug;

		friend class ShaderCompiler;
	};

	using ShaderSetPtr = TObjectPtr<class ShaderSet>;

	class ShaderAsset : IJsonSerializable
	{
	public:

		SAILOR_API __forceinline const std::string& GetGlslVertexCode() const { return m_glslVertex; }
		SAILOR_API __forceinline const std::string& GetGlslFragmentCode() const { return m_glslFragment; }
		SAILOR_API __forceinline const std::string& GetGlslCommonCode() const { return m_glslCommon; }

		SAILOR_API __forceinline const TVector<std::string>& GetIncludes() const { return m_includes; }
		SAILOR_API __forceinline const TVector<std::string>& GetSupportedDefines() const { return m_defines; }

		SAILOR_API bool ContainsFragment() const { return !m_glslFragment.empty(); }
		SAILOR_API bool ContainsVertex() const { return !m_glslVertex.empty(); }
		SAILOR_API bool ContainsCommon() const { return !m_glslCommon.empty(); }

		SAILOR_API virtual void Serialize(nlohmann::json& outData) const;
		SAILOR_API virtual void Deserialize(const nlohmann::json& inData);

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

		SAILOR_API JobSystem::TaskPtr<bool> CompileAllPermutations(const UID& assetUID);
		SAILOR_API TWeakPtr<ShaderAsset> LoadShaderAsset(const UID& uid);

		SAILOR_API virtual ~ShaderCompiler() override;

		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		SAILOR_API bool LoadShader_Immediate(UID uid, ShaderSetPtr& outShader, const TVector<string>& defines = {});
		SAILOR_API JobSystem::TaskPtr<ShaderSetPtr> LoadShader(UID uid, ShaderSetPtr& outShader, const TVector<string>& defines = {});

	protected:

		ShaderCache m_shaderCache;
		Memory::ObjectAllocatorPtr m_allocator;

		TConcurrentMap<UID, TVector<TPair<uint32_t, JobSystem::TaskPtr<ShaderSetPtr>>>> m_promises;
		TConcurrentMap<UID, TSharedPtr<ShaderAsset>> m_shaderAssetsCache;
		TConcurrentMap<UID, TVector<TPair<uint32_t, ShaderSetPtr>>> m_loadedShaders;

		// ShaderAsset related functions
		SAILOR_API static void GeneratePrecompiledGlsl(ShaderAsset* shader, std::string& outGLSLCode, const TVector<std::string>& defines = {});
		SAILOR_API static void ConvertRawShaderToJson(const std::string& shaderText, std::string& outCodeInJSON);
		SAILOR_API static bool ConvertFromJsonToGlslCode(const std::string& shaderText, std::string& outPureGLSL);

		// Compile related functions
		SAILOR_API bool ForceCompilePermutation(const UID& assetUID, uint32_t permutation);
		SAILOR_API bool GetSpirvCode(const UID& assetUID, const TVector<std::string>& defines, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, bool bIsDebug);
		SAILOR_API bool GetSpirvCode(const UID& assetUID, uint32_t permutation, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, bool bIsDebug);
		SAILOR_API static bool CompileGlslToSpirv(const std::string& source, RHI::EShaderStage shaderKind, const TVector<std::string>& defines, const TVector<std::string>& includes, RHI::ShaderByteCode& outByteCode, bool bIsDebug);

		SAILOR_API static uint32_t GetPermutation(const TVector<std::string>& defines, const TVector<std::string>& actualDefines);
		SAILOR_API static TVector<std::string> GetDefines(const TVector<std::string>& defines, uint32_t permutation);

		SAILOR_API bool UpdateRHIResource(ShaderSetPtr shader, uint32_t permutation);

	private:

		static constexpr char const* JsonBeginCodeTag = "BEGIN_CODE";
		static constexpr char const* JsonEndCodeTag = "END_CODE";
		static constexpr char const* JsonEndLineTag = " END_LINE ";
	};
}
