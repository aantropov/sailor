#pragma once
#include "Containers/Containers.h"
#include "Core/Defines.h"
#include "Core/FileRevision.h"
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
#include "Engine/Object.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "RHI/Mesh.h"
#include "RHI/Material.h"
#include "RHI/VertexDescription.h"
#include "Math/Bounds.h"
#include "Math/Transform.h"
#include "Raytracing/BVH.h"
#include <glm/mat4x4.hpp>
#include "Core/YamlSerializable.h"
#include "Core/Reflection.h"

#include <limits>

namespace tinygltf
{
	class Model;
}

namespace Sailor
{
	namespace GltfImporterUtils
	{
		struct SceneNode;
	}

	using ModelPtr = TObjectPtr<class Model>;

	class Model : public Object, public IYamlSerializable
	{
	  public:
		static constexpr int32_t AllMeshes = -1;

		struct MeshCpuData
		{
			TVector<RHI::VertexP3N3T3B3UV2C4I4W4> m_vertices;
			TVector<uint32_t> m_indices;
			Math::AABB m_bounds{};
			int32_t m_materialIndex = -1;
		};

		struct Node
		{
			std::string m_name;
			int32_t m_sourceNodeIndex = -1;
			int32_t m_parentIndex = -1;
			int32_t m_meshIndex = -1;
			int32_t m_skinIndex = -1;
			Math::Transform m_localTransform{};
			glm::mat4 m_localMatrix{1.0f};
			glm::mat4 m_worldMatrix{1.0f};
			bool m_bTransformDecomposable = true;
		};

		struct SourceMesh
		{
			std::string m_name;
			TVector<uint32_t> m_renderMeshIndices;
			Math::AABB m_bounds{};
		};

		struct RenderInstance
		{
			uint32_t m_renderMeshIndex = 0;
			int32_t m_nodeIndex = -1;
			glm::mat4 m_modelMatrix{1.0f};
		};

		struct BLASData
		{
			TSharedPtr<Raytracing::BVH> m_blas{};
			TVector<Math::Triangle> m_triangles{};

			bool IsValid() const
			{
				return m_blas.IsValid() && !m_triangles.IsEmpty();
			}
		};

		SAILOR_API Model(FileId uid, TVector<RHI::RHIMeshPtr> meshes = {})
			: Object(std::move(uid)), m_meshes(std::move(meshes))
		{
		}

		SAILOR_API const TVector<RHI::RHIMeshPtr>& GetMeshes() const
		{
			return m_meshes;
		}
		SAILOR_API TVector<RHI::RHIMeshPtr>& GetMeshes()
		{
			return m_meshes;
		}
		SAILOR_API const TVector<Node>& GetNodes() const
		{
			return m_nodes;
		}
		SAILOR_API const TVector<SourceMesh>& GetSourceMeshes() const
		{
			return m_sourceMeshes;
		}
		SAILOR_API const TVector<RenderInstance>& GetRenderInstances() const
		{
			return m_renderInstances;
		}
		SAILOR_API bool SupportsEditableHierarchy() const
		{
			return m_bSupportsEditableHierarchy;
		}
		SAILOR_API bool IsSourceMeshIndexValid(int32_t meshIndex) const;
		SAILOR_API bool CollectRenderData(int32_t meshIndex,
			TVector<RHI::RHIMeshPtr>& outMeshes,
			TVector<glm::mat4>& outModelMatrices,
			Math::AABB& outBounds) const;

		// Should be triggered after mesh/material changes
		SAILOR_API void Flush();

		// The model hierarchy and RHIMesh objects are available after the importer
		// task completes, while their GPU uploads may still be in flight.
		SAILOR_API bool IsStructurallyReady() const;
		SAILOR_API virtual bool IsReady() const override;
		SAILOR_API virtual ~Model() = default;

		SAILOR_API const Math::AABB& GetBoundsAABB() const
		{
			return m_boundsAabb;
		}
		SAILOR_API const Math::AABB& GetBoundsAABB(int32_t meshIndex) const;
		SAILOR_API const Math::Sphere& GetBoundsSphere() const
		{
			return m_boundsSphere;
		}
		SAILOR_API const TVector<glm::mat4>& GetInverseBind() const
		{
			return m_inverseBind;
		}
		SAILOR_API TVector<glm::mat4>& GetInverseBind()
		{
			return m_inverseBind;
		}
		SAILOR_API const TVector<MeshCpuData>& GetCpuMeshes() const
		{
			return m_cpuMeshes;
		}
		SAILOR_API TVector<MeshCpuData>& GetCpuMeshes()
		{
			return m_cpuMeshes;
		}
		SAILOR_API bool HasCpuMeshes() const
		{
			return m_cpuMeshes.Num() > 0;
		}
		SAILOR_API bool BuildBLAS();
		SAILOR_API bool HasBLAS() const
		{
			return m_blas.IsValid() && m_blasTriangles.Num() > 0;
		}
		SAILOR_API bool HasBLAS(int32_t meshIndex) const;
		SAILOR_API const TSharedPtr<Raytracing::BVH>& GetBLAS() const
		{
			return m_blas;
		}
		SAILOR_API const TSharedPtr<Raytracing::BVH>& GetBLAS(int32_t meshIndex) const;
		SAILOR_API const TVector<Math::Triangle>& GetBLASTriangles() const
		{
			return m_blasTriangles;
		}
		SAILOR_API const TVector<Math::Triangle>& GetBLASTriangles(int32_t meshIndex) const;

		SAILOR_API virtual YAML::Node Serialize() const override;
		SAILOR_API virtual void Deserialize(const YAML::Node& inData) override;

	  private:
		SAILOR_API void ProceedCpuMeshes(bool bShouldGenerateBLAS, bool bShouldKeepCpuBuffers);
		bool BuildBLASData(const TVector<RenderInstance>& instances, BLASData& outData) const;

	  protected:
		TVector<RHI::RHIMeshPtr> m_meshes;
		TVector<Node> m_nodes;
		TVector<SourceMesh> m_sourceMeshes;
		TVector<RenderInstance> m_renderInstances;
		bool m_bSupportsEditableHierarchy = true;
		std::atomic<bool> m_bIsReady{};
		mutable std::atomic<bool> m_bGpuReady{};
		TVector<glm::mat4> m_inverseBind;
		TVector<MeshCpuData> m_cpuMeshes;
		TSharedPtr<Raytracing::BVH> m_blas{};
		TVector<Math::Triangle> m_blasTriangles{};
		TVector<BLASData> m_sourceMeshBlases{};

		Math::AABB m_boundsAabb;
		Math::Sphere m_boundsSphere;

		friend class ModelImporter;
	};

	class ModelImporter final : public TSubmodule<ModelImporter>, public IAssetInfoHandlerListener, public IAssetFactory
	{
	  public:
		struct MeshContext
		{
			struct LodGeometry
			{
				TVector<RHI::VertexP3N3T3B3UV2C4I4W4> m_vertices;
				TVector<uint32_t> m_indices;
			};

			TMap<RHI::VertexP3N3T3B3UV2C4I4W4, uint32_t> uniqueVertices;
			TVector<RHI::VertexP3N3T3B3UV2C4I4W4> outVertices;
			TVector<uint32_t> outIndices;
			TVector<LodGeometry> lods;
			Math::AABB bounds{};
			int32_t materialIndex = -1;
			uint32_t materialSlot = (std::numeric_limits<uint32_t>::max)();
			glm::vec3 bakedVolumeScale{1.0f};
			int32_t sourceMeshIndex = -1;

			bool HasGeometry() const
			{
				return !outVertices.IsEmpty() && !outIndices.IsEmpty();
			}
		};

		SAILOR_API ModelImporter(ModelAssetInfoHandler* infoHandler);
		SAILOR_API virtual ~ModelImporter() override;

		SAILOR_API virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;
		SAILOR_API virtual void OnImportAsset(AssetInfoPtr assetInfo) override;

		SAILOR_API bool LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate = true) override;
		SAILOR_API Tasks::TaskPtr<ModelPtr> LoadModel(FileId uid, ModelPtr& outModel);
		SAILOR_API bool LoadModel_Immediate(FileId uid, ModelPtr& outModel);
		SAILOR_API static void GenerateLods(TVector<MeshContext>& meshes, uint32_t numLods, float reductionFactor);
		SAILOR_API static std::string GetLodCacheFilename(const FileId& fileId, uint32_t lodLevel);

		SAILOR_API Tasks::TaskPtr<bool> LoadDefaultMaterials(FileId uid, TVector<MaterialPtr>& outMaterials);

		SAILOR_API virtual void CollectGarbage() override;

	  protected:
		SAILOR_API bool GenerateMaterialAssets(ModelAssetInfoPtr assetInfo);
		bool UpdateGeneratedMaterialProperties(ModelAssetInfoPtr assetInfo);
		bool UpdateGeneratedMaterialProperties(ModelAssetInfoPtr assetInfo, const tinygltf::Model& gltfModel);
		bool UpdateGeneratedMaterialPropertiesOnDemand(ModelAssetInfoPtr assetInfo, const tinygltf::Model& gltfModel);
		static FileId CreateTextureAsset(const std::string& filepath,
			const std::string& sourceFilename,
			uint32_t sourceTextureIndex,
			bool bShouldGenerateMips = true,
			RHI::EFormat format = RHI::EFormat::R8G8B8A8_SRGB,
			RHI::ETextureClamping clamping = RHI::ETextureClamping::Repeat,
			RHI::ETextureFiltration filtration = RHI::ETextureFiltration::Linear,
			bool bShouldKeepCpuBuffers = false);
		SAILOR_API bool GenerateAnimationAssets(ModelAssetInfoPtr assetInfo);
		static bool ImportModel(ModelAssetInfoPtr assetInfo,
			TVector<MeshContext>& outParsedMeshes,
			Math::AABB& outBoundsAabb,
			Math::Sphere& outBoundsSphere,
			TVector<glm::mat4>& outInverseBind);
		static bool ImportModel(const std::string& assetFilepath,
			float unitScale,
			bool bShouldBatchByMaterial,
			bool bFlipTexcoordY,
			TVector<MeshContext>& outParsedMeshes,
			Math::AABB& outBoundsAabb,
			Math::Sphere& outBoundsSphere,
			TVector<glm::mat4>& outInverseBind,
			tinygltf::Model* outGltfModel = nullptr);
		static void PopulateModelSceneHierarchy(Model& model, TVector<GltfImporterUtils::SceneNode>& sourceNodes);
		static bool GenerateFingerprint(const FileId& fileId,
			const std::string& assetFilepath,
			float unitScale,
			bool bShouldBatchByMaterial,
			bool bFlipTexcoordY,
			const std::string& outputPath,
			uint64_t requestGeneration,
			const FileRevision& sourceRevision);
		static void GenerateFingerprintAsync(ModelAssetInfoPtr modelAssetInfo);

		TConcurrentMap<FileId, Tasks::TaskPtr<ModelPtr>> m_promises;
		TConcurrentMap<FileId, ModelPtr> m_loadedModels;
		TConcurrentMap<FileId, bool> m_generatedMaterialMigrationComplete;
		TConcurrentMap<FileId, Tasks::ITaskPtr> m_generatedMaterialMigrationTasks;

		ObjectAllocatorPtr m_allocator;
	};
}
