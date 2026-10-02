#include "ModelImporter.h"
#include "GltfImporterUtils.h"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelLodGeneration.h"
#include "ModelAssetInfo.h"
#include "Memory/ObjectAllocator.hpp"
#include "RHI/Renderer.h"
#include "RHI/VertexDescription.h"

#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#include <tiny_gltf.h>

using namespace Sailor;

ModelImporter::ModelImporter(ModelAssetInfoHandler* infoHandler, Tasks::Scheduler* scheduler, AssetRegistry* assetRegistry) :
	m_scheduler(scheduler),
	m_assetRegistry(assetRegistry)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

ModelImporter::~ModelImporter()
{
	for (auto& model : m_loadedModels)
	{
		model.m_second.DestroyObject(m_allocator);
	}
}

std::string ModelImporter::GetLodCacheFilename(const FileId& fileId, uint32_t lodLevel)
{
	if (!fileId || lodLevel == 0u)
	{
		return {};
	}

	const std::filesystem::path filename = fileId.ToString() + "_lod" + std::to_string(lodLevel) + ".bin";
	return filename == filename.filename() ? filename.string() : std::string{};
}

void ModelImporter::GenerateLods(TVector<MeshContext>& meshes, uint32_t numLods, float reductionFactor)
{
	ModelLodGeneration::Generate(meshes, numLods, reductionFactor);
}

void ModelImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	SAILOR_PROFILE_FUNCTION();
	SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());
	if (ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo))
	{
		UpdateGeneratedAssets(modelAssetInfo, bWasExpired);
	}
}

void ModelImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
	if (ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo))
	{
		UpdateGeneratedAssets(modelAssetInfo, true);
	}
}

bool ModelImporter::UpdateGeneratedAssets(ModelAssetInfoPtr assetInfo, bool bWasExpired)
{
	if (!assetInfo->IsWritable())
	{
		return true;
	}

	AssetRegistry& assetRegistry = *m_assetRegistry;
	auto areGeneratedAssetsValid = [&assetRegistry](const TVector<FileId>& fileIds, bool bRequireUniqueFileIds)
	{
		TSet<FileId> uniqueFileIds;
		for (const FileId& fileId : fileIds)
		{
			if (!fileId || (bRequireUniqueFileIds && uniqueFileIds.Contains(fileId)) ||
				assetRegistry.GetAssetInfoPtr(fileId) == nullptr)
			{
				return false;
			}
			uniqueFileIds.Insert(fileId);
		}
		return true;
	};

	const TVector<FileId>& materials = assetInfo->GetDefaultMaterials();
	const bool bMaterialsNeedRepair = !materials.IsEmpty() &&
		!areGeneratedAssetsValid(materials, false);
	const bool bGenerateMaterials = assetInfo->ShouldGenerateMaterials() &&
		(bWasExpired || bMaterialsNeedRepair);

	const TVector<FileId>& animations = assetInfo->GetAnimations();
	bool bAnimationsNeedRepair = !areGeneratedAssetsValid(animations, true);
	for (const FileId& fileId : animations)
	{
		const auto* animation = assetRegistry.GetAssetInfoPtr<AnimationAssetInfoPtr>(fileId);
		std::error_code error;
		bAnimationsNeedRepair |= animation == nullptr ||
			!std::filesystem::is_regular_file(animation->GetMetaFilepath(), error);
	}
	const bool bGenerateAnimations = bWasExpired || bAnimationsNeedRepair;
	if (!bGenerateMaterials && !bGenerateAnimations)
	{
		return true;
	}

	const auto token = assetRegistry.BeginAssetProcessing(assetInfo);
	if (!token)
	{
		return false;
	}

	TVector<FileId> previousMaterials = materials;
	TVector<FileId> previousAnimations = animations;
	bool bSucceeded = true;
	if (bGenerateMaterials)
	{
		bSucceeded = GenerateMaterialAssets(assetInfo);
	}
	bool bAnimationsChanged = false;
	if (bSucceeded && bGenerateAnimations)
	{
		bSucceeded = GenerateAnimationAssets(assetInfo, bAnimationsChanged);
	}
	if (bSucceeded && (previousMaterials != assetInfo->GetDefaultMaterials() || bAnimationsChanged))
	{
		bSucceeded = assetInfo->SaveMetaFile();
	}
	if (!bSucceeded)
	{
		assetInfo->GetDefaultMaterials() = std::move(previousMaterials);
		assetInfo->GetAnimations() = std::move(previousAnimations);
	}

	assetRegistry.CompleteAssetProcessing(token, bSucceeded);
	return bSucceeded;
}

void ModelImporter::PopulateModelSceneHierarchy(Model& model, TVector<GltfImporterUtils::SceneNode>& sourceNodes)
{
	model.m_nodes.Clear();
	model.m_renderInstances.Clear();
	model.m_bSupportsEditableHierarchy = true;
	model.m_nodes.Reserve(sourceNodes.Num());
	for (auto& sourceNode : sourceNodes)
	{
		Model::Node node{};
		node.m_name = std::move(sourceNode.m_name);
		node.m_sourceNodeIndex = sourceNode.m_sourceNodeIndex;
		node.m_parentIndex = sourceNode.m_parentIndex;
		node.m_meshIndex = sourceNode.m_meshIndex;
		node.m_skinIndex = sourceNode.m_skinIndex;
		node.m_localTransform = sourceNode.m_localTransform;
		node.m_localMatrix = sourceNode.m_localMatrix;
		node.m_worldMatrix = sourceNode.m_worldMatrix;
		node.m_bTransformDecomposable = sourceNode.m_bTransformDecomposable;
		model.m_bSupportsEditableHierarchy &= node.m_bTransformDecomposable && node.m_skinIndex < 0;
		model.m_nodes.Add(std::move(node));
	}

	for (size_t nodeIndex = 0; nodeIndex < model.m_nodes.Num(); ++nodeIndex)
	{
		const Model::Node& node = model.m_nodes[nodeIndex];
		if (!model.IsSourceMeshIndexValid(node.m_meshIndex))
		{
			continue;
		}

		const Model::SourceMesh& sourceMesh = model.m_sourceMeshes[static_cast<size_t>(node.m_meshIndex)];
		for (uint32_t renderMeshIndex : sourceMesh.m_renderMeshIndices)
		{
			Model::RenderInstance instance{};
			instance.m_renderMeshIndex = renderMeshIndex;
			instance.m_nodeIndex = static_cast<int32_t>(nodeIndex);
			instance.m_modelMatrix = node.m_skinIndex >= 0 ? glm::mat4(1.0f) : node.m_worldMatrix;
			model.m_renderInstances.Add(std::move(instance));
		}
	}
}

Tasks::TaskPtr<ModelPtr> ModelImporter::LoadModel(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();
	ModelAssetInfoPtr pAssetInfo = m_assetRegistry->GetAssetInfoPtr<ModelAssetInfoPtr>(uid);

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedModel = m_loadedModels.At_Lock(uid, ModelPtr());

	if (promise && !promise->IsFinished())
	{
		// Loading tasks may still be writing the model, including its CPU meshes.
		outModel = loadedModel;
		auto result = promise;
		m_loadedModels.Unlock(uid);
		m_promises.Unlock(uid);
		return result;
	}
	if (promise && !promise->GetResult())
	{
		loadedModel = nullptr;
		promise = nullptr;
	}

	// Check loaded assets
	if (loadedModel)
	{
		const bool bNeedCpuBuffers = pAssetInfo && pAssetInfo->ShouldKeepCpuBuffers() && !loadedModel->HasCpuMeshes();
		if (bNeedCpuBuffers)
		{
			loadedModel = nullptr;
			promise = nullptr;
		}
		else
		{
			outModel = loadedModel;
			auto res = promise ? promise : Tasks::TaskPtr<ModelPtr>::Make(outModel, m_scheduler);

			m_loadedModels.Unlock(uid);
			m_promises.Unlock(uid);

			return res;
		}
	}

	// There is no promise, we need to load model
	if (pAssetInfo)
	{
		SAILOR_PROFILE_TEXT(pAssetInfo->GetAssetFilepath().c_str());

		ModelPtr pModel = ModelPtr::Make(m_allocator, uid);

		struct Data
		{
			TVector<MeshContext> m_parsedMeshes;
			bool m_bIsImported = false;
		};

		auto loadDataTask = Tasks::CreateTask<TSharedPtr<Data>>(*m_scheduler, "Load model",
			[this, pAssetInfo, pModel]() mutable
			{
				TSharedPtr<Data> pData = TSharedPtr<Data>::Make();
				tinygltf::Model gltfModel;
				const bool bKeepCpuBuffers = pAssetInfo->ShouldKeepCpuBuffers();
				const bool bGenerateBLAS = pAssetInfo->ShouldGenerateBLAS();
				pData->m_bIsImported = ImportModel(pAssetInfo->GetAssetFilepath(),
					pAssetInfo->GetUnitScale(),
					pAssetInfo->ShouldBatchByMaterial(),
					pAssetInfo->ShouldFlipTexcoordY(),
					pData->m_parsedMeshes,
					pModel->m_boundsAabb,
					pModel->m_boundsSphere,
					pModel->m_inverseBind,
					&gltfModel);
				if (!pData->m_bIsImported)
				{
					return pData;
				}

				ModelLodGeneration::Prepare(*pAssetInfo, pData->m_parsedMeshes);
				TVector<GltfImporterUtils::SceneNode> sceneNodes;
				pData->m_bIsImported = GltfImporterUtils::CollectSceneNodes(
					gltfModel, pAssetInfo->GetUnitScale(), sceneNodes);
				if (!pData->m_bIsImported)
				{
					return pData;
				}

#if defined(SAILOR_MODEL_IMPORT_TEST_HOOKS)
				if (m_beforeCpuPreparationForTests) m_beforeCpuPreparationForTests();
#endif
				pModel->m_sourceMeshes.Resize(gltfModel.meshes.size());
				for (size_t meshIndex = 0; meshIndex < gltfModel.meshes.size(); ++meshIndex)
				{
					const auto& name = gltfModel.meshes[meshIndex].name;
					pModel->m_sourceMeshes[meshIndex].m_name = name.empty() ? "Mesh_" + std::to_string(meshIndex) : name;
				}
				if (bKeepCpuBuffers || bGenerateBLAS)
				{
					pModel->m_cpuMeshes.Reserve(pData->m_parsedMeshes.Num());
				}

				uint32_t renderMeshIndex = 0;
				for (const auto& mesh : pData->m_parsedMeshes)
				{
					if (!mesh.HasGeometry())
					{
						continue;
					}
					if (mesh.sourceMeshIndex >= 0 && static_cast<size_t>(mesh.sourceMeshIndex) < pModel->m_sourceMeshes.Num())
					{
						auto& sourceMesh = pModel->m_sourceMeshes[static_cast<size_t>(mesh.sourceMeshIndex)];
						sourceMesh.m_renderMeshIndices.Add(renderMeshIndex);
						sourceMesh.m_bounds.Extend(mesh.bounds);
					}
					++renderMeshIndex;
					if (bKeepCpuBuffers || bGenerateBLAS)
					{
						Model::MeshCpuData cpuMesh{};
						cpuMesh.m_vertices = mesh.outVertices;
						cpuMesh.m_indices = mesh.outIndices;
						cpuMesh.m_bounds = mesh.bounds;
						cpuMesh.m_materialIndex = static_cast<int32_t>(mesh.materialSlot);
						pModel->m_cpuMeshes.Add(std::move(cpuMesh));
					}
				}
				PopulateModelSceneHierarchy(*pModel, sceneNodes);
				pModel->ProceedCpuMeshes(bGenerateBLAS, bKeepCpuBuffers);
				return pData;
			});

		promise =
			loadDataTask
				->Then<ModelPtr>(
					[pModel](TSharedPtr<Data> pData) mutable
					{
						if (pData->m_bIsImported)
						{
							pModel->m_meshes.Reserve(pData->m_parsedMeshes.Num());

							for (size_t meshIndex = 0; meshIndex < pData->m_parsedMeshes.Num(); ++meshIndex)
							{
								auto& mesh = pData->m_parsedMeshes[meshIndex];
								if (!mesh.HasGeometry())
								{
									continue;
								}

								RHI::RHIMeshPtr pMesh = RHI::Renderer::GetDriver()->CreateMesh();
								pMesh->m_vertexDescription =
									RHI::Renderer::GetDriver()
										->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4I4W4>();
								pMesh->m_bounds = mesh.bounds;
								pMesh->m_materialIndex = mesh.materialSlot != (std::numeric_limits<uint32_t>::max)()
															 ? mesh.materialSlot
															 : static_cast<uint32_t>(meshIndex);
								pMesh->m_bakedVolumeScale = mesh.bakedVolumeScale;
								pMesh->m_indexCount = static_cast<uint32_t>(mesh.outIndices.Num());
								TVector<RHI::VertexP3N3T3B3UV2C4I4W4> uploadVertices = std::move(mesh.outVertices);
								TVector<uint32_t> uploadIndices = std::move(mesh.outIndices);
								pMesh->m_firstIndex = 0u;
								pMesh->m_vertexOffset = 0u;
								pMesh->m_lods.Reserve(mesh.lods.Num());
								for (const auto& lodGeometry : mesh.lods)
								{
									RHI::RHIMeshPtr lodMesh = RHI::Renderer::GetDriver()->CreateMesh();
									lodMesh->m_vertexDescription = pMesh->m_vertexDescription;
									lodMesh->m_bounds = pMesh->m_bounds;
									lodMesh->m_materialIndex = pMesh->m_materialIndex;
									lodMesh->m_bakedVolumeScale = pMesh->m_bakedVolumeScale;
									if (lodGeometry.m_indices.IsEmpty())
									{
										const auto& previous = pMesh->m_lods.IsEmpty() ? pMesh : *pMesh->m_lods.Last();
										lodMesh->m_indexCount = previous->m_indexCount;
										lodMesh->m_firstIndex = previous->m_firstIndex;
										lodMesh->m_vertexOffset = previous->m_vertexOffset;
									}
									else
									{
										lodMesh->m_indexCount = static_cast<uint32_t>(lodGeometry.m_indices.Num());
										lodMesh->m_firstIndex = static_cast<uint32_t>(uploadIndices.Num());
										lodMesh->m_vertexOffset = static_cast<uint32_t>(uploadVertices.Num());
										uploadVertices.AddRange(lodGeometry.m_vertices);
										uploadIndices.AddRange(lodGeometry.m_indices);
									}
									pMesh->m_lods.Add(std::move(lodMesh));
								}
								RHI::Renderer::GetDriver()->UpdateMesh(pMesh,
									uploadVertices.GetData(),
									sizeof(RHI::VertexP3N3T3B3UV2C4I4W4) * uploadVertices.Num(),
									uploadIndices.GetData(),
									sizeof(uint32_t) * uploadIndices.Num());
								for (auto& lodMesh : pMesh->m_lods)
								{
									lodMesh->m_vertexBuffer = pMesh->m_vertexBuffer;
									lodMesh->m_indexBuffer = pMesh->m_indexBuffer;
								}

								pModel->m_meshes.Emplace(pMesh);
							}

							pModel->Flush();
						}
						return pModel->IsStructurallyReady() ? pModel : ModelPtr{};
					},
					"Update RHI Meshes",
					EThreadType::RHI)
				->ToTaskWithResult();

		outModel = loadedModel = pModel;
		promise->Run();
		auto result = promise;

		m_loadedModels.Unlock(uid);
		m_promises.Unlock(uid);

		return result;
	}

	outModel = nullptr;
	m_loadedModels.Unlock(uid);
	m_promises.Unlock(uid);

	return Tasks::TaskPtr<ModelPtr>();
}

bool ModelImporter::LoadModel_Immediate(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadModel(uid, outModel);
	if (!task)
	{
		outModel = nullptr;
		return false;
	}

	task->Wait();
	outModel = task->GetResult();
	return outModel && outModel->IsStructurallyReady();
}

Tasks::TaskPtr<bool> ModelImporter::LoadDefaultMaterials(FileId uid, TVector<MaterialPtr>& outMaterials)
{
	outMaterials.Clear();

	if (ModelAssetInfoPtr modelInfo = m_assetRegistry->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		Tasks::TaskPtr<bool> loadingFinished =
			Tasks::CreateTask<bool>(*m_scheduler, "Load Default Materials", []() { return true; });
		const TVector<FileId>& defaultMaterials = modelInfo->GetDefaultMaterials();
		outMaterials.Resize(defaultMaterials.Num());

		for (size_t materialIndex = 0; materialIndex < defaultMaterials.Num(); ++materialIndex)
		{
			MaterialPtr material;
			Tasks::ITaskPtr loadMaterial;
			const FileId& materialFileId = defaultMaterials[materialIndex];
			if (materialFileId &&
				(loadMaterial = App::GetSubmodule<MaterialImporter>()->LoadMaterial(materialFileId, material)))
			{
				if (material)
				{
					// Preserve the glTF material slot even if an adjacent generated
					// material is temporarily unavailable. RHIMesh::m_materialIndex
					// refers to this original slot and must never address a compacted
					// list.
					outMaterials[materialIndex] = material;
					// TODO: Add hot reloading dependency
					loadingFinished->Join(loadMaterial);
				}
			}
		}

		m_scheduler->Run(loadingFinished);
		return loadingFinished;
	}

	return Tasks::TaskPtr<bool>::Make(false, m_scheduler);
}

bool ModelImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	ModelPtr outModel;
	if (bImmediate)
	{
		bool bRes = LoadModel_Immediate(uid, outModel);
		out = outModel;
		return bRes;
	}

	auto task = LoadModel(uid, outModel);
	out = outModel;
	return static_cast<bool>(task);
}

void ModelImporter::CollectGarbage()
{
	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto& promise = m_promises.At_Lock(id);
		if (!promise || promise->IsFinished())
		{
			if (promise && !promise->GetResult())
			{
				m_loadedModels.Remove(id);
			}
			// Read and removal share the stripe, so a retry cannot replace this attempt in between.
			m_promises.ForcelyRemove(id);
		}
		m_promises.Unlock(id);
	}
}
