#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Pair.h"
#include "Containers/Vector.h"
#include "Containers/Map.h"
#include "Containers/ConcurrentMap.h"

#include "Core/YamlSerializable.h"
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "Engine/Types.h"
#include "RHI/Types.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"

namespace Sailor
{
	class Material : public Object
	{
	public:

		static SAILOR_API MaterialPtr CreateInstance(WorldPtr world, const MaterialPtr& material);

		SAILOR_API Material(FileId uid) : Object(uid) {}

		SAILOR_API virtual bool IsReady() const override;
		SAILOR_API bool IsDirty() const { return m_bIsDirty.load(); }

		SAILOR_API virtual Tasks::ITaskPtr OnHotReload() override;

		SAILOR_API ShaderSetPtr GetShader() { return m_shader; }
		SAILOR_API RHI::RHIShaderBindingSetPtr GetShaderBindings() { return m_commonShaderBindings; }

		SAILOR_API const TConcurrentMap<std::string, TexturePtr>& GetSamplers() const { return m_samplers; }
		SAILOR_API const TConcurrentMap<std::string, glm::vec4>& GetUniformsVec4() const { return m_uniformsVec4; }
		SAILOR_API const TConcurrentMap<std::string, float>& GetUniformsFloat() const { return m_uniformsFloat; }

		SAILOR_API void ClearSamplers();
		SAILOR_API void ClearUniforms();

		SAILOR_API const RHI::RenderState& GetRenderState() const { return m_renderState; }

		SAILOR_API void UpdateRHIResource();

		// TODO: Incapsulate & isolate
		SAILOR_API RHI::RHIMaterialPtr GetOrAddRHI(RHI::RHIVertexDescriptionPtr vertexDescription);

		const auto& GetRHIMaterials() const { return m_rhiMaterials; }

		SAILOR_API void SetSampler(const std::string& name, TexturePtr value);
		SAILOR_API void SetUniform(const std::string& name, glm::vec4 value);
		SAILOR_API void SetUniform(const std::string& name, float value);
		SAILOR_API void SetShader(ShaderSetPtr shader) { m_shader = shader; }
		SAILOR_API void SetRenderState(const RHI::RenderState& renderState) { m_renderState = renderState; }

		SAILOR_API ShaderSetPtr GetShader() const { return m_shader; }

	protected:

		void ForcelyUpdateUniforms();
		void UpdateUniforms(RHI::RHICommandListPtr cmdList);

		std::atomic<bool> m_bIsDirty{};

		ShaderSetPtr m_shader{};
		RHI::RHIShaderBindingSetPtr m_commonShaderBindings{};

		RHI::RenderState m_renderState{};

		TConcurrentMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr, 24, ERehashPolicy::Never> m_rhiMaterials{};
		TConcurrentMap<std::string, TexturePtr> m_samplers{};
		TConcurrentMap<std::string, glm::vec4> m_uniformsVec4{};
		TConcurrentMap<std::string, float> m_uniformsFloat{};

		friend class MaterialImporter;
	};

	class MaterialAsset : public IYamlSerializable
	{
	public:

		struct Data
		{
			std::string m_name = "Untitled";
			RHI::RenderState m_renderState;

			std::string m_renderQueue = "Opaque";
			TVector<std::string> m_shaderDefines;
			TMap<std::string, FileId> m_samplers;
			TMap<std::string, glm::vec4> m_uniformsVec4;
			TMap<std::string, float> m_uniformsFloat;

			FileId m_shader;
		};

		SAILOR_API virtual ~MaterialAsset() = default;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

		SAILOR_API const RHI::RenderState& GetRenderState() const { return m_pData->m_renderState; }
		SAILOR_API const std::string& GetRenderQueue() const { return m_pData->m_renderQueue; }
		SAILOR_API const FileId& GetShader() const { return m_pData->m_shader; }
		SAILOR_API const auto& GetShaderDefines() const { return  m_pData->m_shaderDefines; }
		SAILOR_API const auto& GetSamplers() const { return m_pData->m_samplers; }
		SAILOR_API const auto& GetUniformsVec4() const { return m_pData->m_uniformsVec4; }
		SAILOR_API const auto& GetUniformsFloat() const { return m_pData->m_uniformsFloat; }

	protected:

		TUniquePtr<Data> m_pData;
		friend class MaterialImporter;
	};

	class MaterialImporter final : public TSubmodule<MaterialImporter>, public IAssetInfoHandlerListener
	{
	public:

		SAILOR_API MaterialImporter(MaterialAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~MaterialImporter() override;

		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		SAILOR_API TSharedPtr<MaterialAsset> LoadMaterialAsset(FileId uid);

		SAILOR_API bool LoadMaterial_Immediate(FileId uid, MaterialPtr& outMaterial);
		SAILOR_API Tasks::TaskPtr<MaterialPtr> LoadMaterial(FileId uid, MaterialPtr& outMaterial);

		SAILOR_API const FileId& CreateMaterialAsset(const std::string& assetpath, MaterialAsset::Data data);

		SAILOR_API MaterialPtr GetLoadedMaterial(FileId uid);
		SAILOR_API Tasks::TaskPtr<MaterialPtr> GetLoadPromise(FileId uid);

		SAILOR_API virtual void CollectGarbage() override;

		SAILOR_API Memory::ObjectAllocatorPtr& GetAllocator() { return m_allocator; }

	protected:

		SAILOR_API bool IsMaterialLoaded(FileId uid) const;

		TConcurrentMap<FileId, Tasks::TaskPtr<MaterialPtr>> m_promises;
		TConcurrentMap<FileId, MaterialPtr> m_loadedMaterials;

		Memory::ObjectAllocatorPtr m_allocator;
	};

	using MaterialPtr = TObjectPtr<Material>;
}
