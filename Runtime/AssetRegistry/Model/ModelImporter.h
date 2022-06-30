#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include "Containers/ConcurrentMap.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/AssetInfo.h"
#include "ModelAssetInfo.h"
#include "JobSystem/JobSystem.h"
#include "ModelAssetInfo.h"
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Math/Bounds.h"

namespace Sailor::RHI
{
	class VertexP3N3UV2C4;
}

namespace Sailor
{
	using ModelPtr = TObjectPtr<class Model>;

	class Model : public Object
	{
	public:

		SAILOR_API Model(UID uid, TVector<RHI::RHIMeshPtr> meshes = {}) :
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

	protected:

		TVector<RHI::RHIMeshPtr> m_meshes;
		std::atomic<bool> m_bIsReady;

		Math::AABB m_boundsAabb;
		Math::Sphere m_boundsSphere;

		friend class ModelImporter;
	};

	class ModelImporter final : public TSubmodule<ModelImporter>, public IAssetInfoHandlerListener
	{
	public:

		SAILOR_API ModelImporter(ModelAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~ModelImporter() override;

		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;

		SAILOR_API JobSystem::TaskPtr<ModelPtr> LoadModel(UID uid, ModelPtr& outModel);
		SAILOR_API bool LoadModel_Immediate(UID uid, ModelPtr& outModel);

		SAILOR_API JobSystem::TaskPtr<bool> LoadDefaultMaterials(UID uid, TVector<MaterialPtr>& outMaterials);

	protected:

		SAILOR_API static bool ImportObjModel(ModelAssetInfoPtr assetInfo, TVector<RHI::RHIMeshPtr>& outMeshes, Math::AABB& outBoundsAabb, Math::Sphere& outBoundsSphere);

		SAILOR_API void GenerateMaterialAssets(ModelAssetInfoPtr assetInfo);

		TConcurrentMap<UID, JobSystem::TaskPtr<ModelPtr>> m_promises;
		TConcurrentMap<UID, ModelPtr> m_loadedModels;

		ObjectAllocatorPtr m_allocator;
	};
}
