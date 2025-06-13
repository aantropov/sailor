#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "Engine/Types.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/AssetFactory.h"
#include "ModelAssetInfo.h"
#include "Tasks/Scheduler.h"
#include "ModelAssetInfo.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "Math/Bounds.h"
#include "Core/YamlSerializable.h"
#include "Core/Reflection.h"

namespace Sailor::RHI
{
	class VertexP3N3T3B3UV2C4;
}

namespace Sailor
{
	using ModelPtr = TObjectPtr<class Model>;

	class Model : public Object, public IYamlSerializable
	{
	public:

		SAILOR_API Model(FileId uid, TVector<RHI::RHIMeshPtr> meshes = {}) :
			Object(std::move(uid)),
			m_meshes(std::move(meshes)) {}

		SAILOR_API const TVector<RHI::RHIMeshPtr>& GetMeshes() const { return m_meshes; }
		SAILOR_API TVector<RHI::RHIMeshPtr>& GetMeshes() { return m_meshes; }

		// Should be triggered after mesh/material changes
		SAILOR_API void Flush();

		SAILOR_API virtual bool IsReady() const override;
		SAILOR_API virtual ~Model() = default;

		SAILOR_API const Math::AABB& GetBoundsAABB() const { return m_boundsAabb; }
		SAILOR_API const Math::Sphere& GetBoundsSphere() const { return m_boundsSphere; }

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

	protected:

		TVector<RHI::RHIMeshPtr> m_meshes;
		std::atomic<bool> m_bIsReady{};

		Math::AABB m_boundsAabb;
		Math::Sphere m_boundsSphere;

		friend class ModelImporter;
	};

	class ModelImporter final : public TSubmodule<ModelImporter>, public IAssetInfoHandlerListener, public IAssetFactory
	{
	public:

		struct MeshContext
		{
			std::unordered_map<RHI::VertexP3N3T3B3UV2C4, uint32_t> uniqueVertices;
			TVector<RHI::VertexP3N3T3B3UV2C4> outVertices;
			TVector<uint32_t> outIndices;
			Math::AABB bounds{};
		};

		SAILOR_API ModelImporter(ModelAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~ModelImporter() override;

		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;

		SAILOR_API bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) override;
		SAILOR_API Tasks::TaskPtr<ModelPtr> LoadModel(FileId uid, ModelPtr& outModel);
		SAILOR_API bool LoadModel_Immediate(FileId uid, ModelPtr& outModel);

		SAILOR_API Tasks::TaskPtr<bool> LoadDefaultMaterials(FileId uid, TVector<MaterialPtr>& outMaterials);

		SAILOR_API virtual void CollectGarbage() override;

	protected:

SAILOR_API static bool ImportModel(ModelAssetInfoPtr assetInfo, TVector<MeshContext>& outParsedMeshes, Math::AABB& outBoundsAabb, Math::Sphere& outBoundsSphere);

SAILOR_API void GenerateMaterialAssets(ModelAssetInfoPtr assetInfo);
SAILOR_API void GenerateAnimationAssets(ModelAssetInfoPtr assetInfo);

		TConcurrentMap<FileId, Tasks::TaskPtr<ModelPtr>> m_promises;
		TConcurrentMap<FileId, ModelPtr> m_loadedModels;

		ObjectAllocatorPtr m_allocator;
	};
}
