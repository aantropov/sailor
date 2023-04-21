#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Pair.h"
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Submodule.h"
#include "Core/YamlSerializable.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "ShaderAssetInfo.h"
#include "RHI/Types.h"
#include "ShaderCache.h"
#include "Tasks/Tasks.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"

namespace Sailor
{
	class ShaderSet : public Object
	{
	public:

		// We aren't able to create shader without asset
		SAILOR_API ShaderSet(UID uid, const TVector<std::string>& defines) : Object(std::move(uid)), m_defines(defines) {}

		SAILOR_API virtual bool IsReady() const override;

		SAILOR_API const RHI::RHIShaderPtr& GetComputeShaderRHI() const { return m_rhiComputeShader; }
		SAILOR_API RHI::RHIShaderPtr& GetComputeShaderRHI() { return m_rhiComputeShader; }

		SAILOR_API const RHI::RHIShaderPtr& GetVertexShaderRHI() const { return m_rhiVertexShader; }
		SAILOR_API RHI::RHIShaderPtr& GetVertexShaderRHI() { return m_rhiVertexShader; }

		SAILOR_API const RHI::RHIShaderPtr& GetFragmentShaderRHI() const { return m_rhiFragmentShader; }
		SAILOR_API RHI::RHIShaderPtr& GetFragmentShaderRHI() { return m_rhiFragmentShader; }

		SAILOR_API const RHI::RHIShaderPtr& GetDebugVertexShaderRHI() const { return m_rhiVertexShaderDebug; }
		SAILOR_API const RHI::RHIShaderPtr& GetDebugFragmentShaderRHI() const { return m_rhiFragmentShaderDebug; }
		SAILOR_API const RHI::RHIShaderPtr& GetDebugComputeShaderRHI() const { return m_rhiComputeShaderDebug; }

		SAILOR_API const TVector<RHI::EFormat>& GetColorAttachments() const { return m_colorAttachments; }
		SAILOR_API RHI::EFormat GetDepthStencilAttachment() const { return m_depthStencilAttachment; }

		SAILOR_API const TVector<std::string>& GetDefines() const { return m_defines; }

	protected:

		RHI::RHIShaderPtr m_rhiVertexShader{};
		RHI::RHIShaderPtr m_rhiFragmentShader{};

		RHI::RHIShaderPtr m_rhiVertexShaderDebug{};
		RHI::RHIShaderPtr m_rhiFragmentShaderDebug{};

		RHI::RHIShaderPtr m_rhiComputeShader;
		RHI::RHIShaderPtr m_rhiComputeShaderDebug;

		TVector<RHI::EFormat> m_colorAttachments{};
		RHI::EFormat m_depthStencilAttachment = RHI::EFormat::UNDEFINED;

		TVector<std::string> m_defines{};

		friend class ShaderCompiler;
	};

	using ShaderSetPtr = TObjectPtr<class ShaderSet>;

	class ShaderAsset : IYamlSerializable
	{
	public:

		SAILOR_API __forceinline const std::string& GetGlslVertexCode() const { return m_glslVertex; }
		SAILOR_API __forceinline const std::string& GetGlslFragmentCode() const { return m_glslFragment; }
		SAILOR_API __forceinline const std::string& GetGlslComputeCode() const { return m_glslCompute; }
		SAILOR_API __forceinline const std::string& GetGlslCommonCode() const { return m_glslCommon; }

		SAILOR_API __forceinline const TVector<std::string>& GetIncludes() const { return m_includes; }
		SAILOR_API __forceinline const TVector<std::string>& GetSupportedDefines() const { return m_defines; }

		SAILOR_API const TVector<RHI::EFormat>& GetColorAttachments() const { return m_colorAttachments; }
		SAILOR_API RHI::EFormat GetDepthStencilAttachment() const { return m_depthStencilAttachment; }

		SAILOR_API bool ContainsFragment() const { return !m_glslFragment.empty(); }
		SAILOR_API bool ContainsVertex() const { return !m_glslVertex.empty(); }
		SAILOR_API bool ContainsCommon() const { return !m_glslCommon.empty(); }
		SAILOR_API bool ContainsCompute() const { return !m_glslCompute.empty(); }

		SAILOR_API virtual void Deserialize(const YAML::Node& inData);

	protected:

		// Shader's code
		std::string m_glslVertex;
		std::string m_glslFragment;
		std::string m_glslCompute;

		// Library code
		std::string m_glslCommon;

		TVector<RHI::EFormat> m_colorAttachments;
		RHI::EFormat m_depthStencilAttachment = RHI::EFormat::UNDEFINED;

		TVector<std::string> m_includes;
		TVector<std::string> m_defines;
	};

	class ShaderCompiler final : public TSubmodule<ShaderCompiler>, public IAssetInfoHandlerListener
	{
		const bool bShouldAutoCompileAllPermutations = false;
		
		// Version is used to generate shader's code with all constants
		const uint32_t Version = 1;
		static constexpr const char* ConstantsLibrary = "../Content/Shaders/Constants.glsl";

	public:
		SAILOR_API ShaderCompiler(ShaderAssetInfoHandler* infoHandler);

		SAILOR_API Tasks::TaskPtr<bool> CompileAllPermutations(const UID& uid);
		SAILOR_API TWeakPtr<ShaderAsset> LoadShaderAsset(const UID& uid);

		SAILOR_API virtual ~ShaderCompiler() override;

		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		SAILOR_API bool LoadShader_Immediate(UID uid, ShaderSetPtr& outShader, const TVector<string>& defines = {});
		SAILOR_API Tasks::TaskPtr<ShaderSetPtr> LoadShader(UID uid, ShaderSetPtr& outShader, const TVector<string>& defines = {});

		SAILOR_API virtual void CollectGarbage() override;

	protected:

		ShaderCache m_shaderCache;
		Memory::ObjectAllocatorPtr m_allocator;

		TConcurrentMap<UID, TVector<TPair<uint32_t, Tasks::TaskPtr<ShaderSetPtr>>>> m_promises;
		TConcurrentMap<UID, TSharedPtr<ShaderAsset>> m_shaderAssetsCache;
		TConcurrentMap<UID, TVector<TPair<uint32_t, ShaderSetPtr>>> m_loadedShaders;

		SAILOR_API void UpdateConstantsLibrary();

		// ShaderAsset related functions
		SAILOR_API static void GeneratePrecompiledGlsl(ShaderAsset* shader, std::string& outGLSLCode, const TVector<std::string>& includes = {}, const TVector<std::string>& defines = {});

		// Compile related functions
		SAILOR_API bool ForceCompilePermutation(ShaderAssetInfoPtr assetInfo, uint32_t permutation);
		SAILOR_API bool GetSpirvCode(const UID& assetUID, const TVector<std::string>& defines, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, RHI::ShaderByteCode& outComputeByteCode, bool bIsDebug);
		SAILOR_API bool GetSpirvCode(const UID& assetUID, uint32_t permutation, RHI::ShaderByteCode& outVertexByteCode, RHI::ShaderByteCode& outFragmentByteCode, RHI::ShaderByteCode& outComputeByteCode, bool bIsDebug);
		SAILOR_API static bool CompileGlslToSpirv(const std::string& filename, const std::string& source, RHI::EShaderStage shaderKind, RHI::ShaderByteCode& outByteCode, bool bIsDebug);

		SAILOR_API static uint32_t GetPermutation(const TVector<std::string>& defines, const TVector<std::string>& actualDefines);
		SAILOR_API static TVector<std::string> GetDefines(const TVector<std::string>& defines, uint32_t permutation);

		SAILOR_API bool UpdateRHIResource(ShaderSetPtr shader, uint32_t permutation);

		SAILOR_API Tasks::TaskPtr<bool> CompileAllPermutations(ShaderAssetInfoPtr shaderAssetInfo);
		SAILOR_API TWeakPtr<ShaderAsset> LoadShaderAsset(ShaderAssetInfoPtr shaderAssetInfo);
	};
}
