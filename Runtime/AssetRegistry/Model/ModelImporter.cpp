#include "ModelImporter.h"
#include "GltfImporterUtils.h"

#include "AssetRegistry/AssetRegistry.h"
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

ModelImporter::ModelImporter(ModelAssetInfoHandler* infoHandler)
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
	auto areGeneratedAssetsValid = [](const TVector<FileId>& fileIds, bool bRequireUniqueFileIds)
	{
		AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
		if (assetRegistry == nullptr)
		{
			return false;
		}

		TSet<FileId> uniqueFileIds;
		for (const FileId& fileId : fileIds)
		{
			if (!fileId || (bRequireUniqueFileIds && uniqueFileIds.Contains(fileId)) ||
				assetRegistry->GetAssetInfoPtr(fileId) == nullptr)
			{
				return false;
			}
			uniqueFileIds.Insert(fileId);
		}
		return true;
	};

	if (ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo))
	{
		if (modelAssetInfo->IsWritable())
		{
			const TVector<FileId>& materials = modelAssetInfo->GetDefaultMaterials();
			const bool bMaterialsNeedRepair =
				materials.Num() > 0 && !areGeneratedAssetsValid(materials, modelAssetInfo->ShouldBatchByMaterial());
			const bool bShouldRegenerateMaterials = modelAssetInfo->ShouldGenerateMaterials() &&
													((bWasExpired && materials.Num() == 0) || bMaterialsNeedRepair);
			if (bShouldRegenerateMaterials && GenerateMaterialAssets(modelAssetInfo))
			{
				assetInfo->SaveMetaFile();
			}
			else if (modelAssetInfo->ShouldGenerateMaterials() && bWasExpired && materials.Num() > 0 &&
					 !bMaterialsNeedRepair)
			{
				UpdateGeneratedMaterialProperties(modelAssetInfo);
			}

			const TVector<FileId>& animations = modelAssetInfo->GetAnimations();
			const bool bAnimationsNeedRepair = animations.Num() > 0 && !areGeneratedAssetsValid(animations, true);
			if (((bWasExpired && animations.Num() == 0) || bAnimationsNeedRepair) &&
				GenerateAnimationAssets(modelAssetInfo))
			{
				assetInfo->SaveMetaFile();
			}
		}

		if (bWasExpired)
		{
			GenerateFingerprintAsync(modelAssetInfo);
		}
	}
}

void ModelImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
	ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo);
	if (!modelAssetInfo)
	{
		return;
	}

	if (modelAssetInfo->IsWritable())
	{
		if (modelAssetInfo->ShouldGenerateMaterials() && modelAssetInfo->GetDefaultMaterials().Num() == 0 &&
			GenerateMaterialAssets(modelAssetInfo))
		{
			assetInfo->SaveMetaFile();
		}

		if (modelAssetInfo->GetAnimations().Num() == 0 && GenerateAnimationAssets(modelAssetInfo))
		{
			assetInfo->SaveMetaFile();
		}
	}

	GenerateFingerprintAsync(modelAssetInfo);
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
	ModelAssetInfoPtr pAssetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid);

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedModel = m_loadedModels.At_Lock(uid, ModelPtr());

	// Check loaded assets
	if (loadedModel)
	{
		const bool bNeedCpuBuffers = pAssetInfo && pAssetInfo->ShouldKeepCpuBuffers() && !loadedModel->HasCpuMeshes();
		if (bNeedCpuBuffers && !promise)
		{
			loadedModel = nullptr;
		}
		else
		{
			outModel = loadedModel;
			auto res = promise ? promise : Tasks::TaskPtr<ModelPtr>::Make(outModel);

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

		// The way to drop qualifiers inside lambda
		auto& boundsSphere = pModel->m_boundsSphere;
		auto& boundsAabb = pModel->m_boundsAabb;

		struct Data
		{
			TVector<MeshContext> m_parsedMeshes;
			TVector<glm::mat4> m_inverseBind;
			TVector<GltfImporterUtils::SceneNode> m_sceneNodes;
			TVector<std::string> m_sourceMeshNames;
			tinygltf::Model m_gltfModel;
			bool m_bIsImported = false;
			bool m_bShouldKeepCpuBuffers = false;
			bool m_bShouldGenerateBLAS = false;
		};

		auto loadDataTask = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load model",
			[pAssetInfo, &boundsAabb, &boundsSphere]()
			{
				TSharedPtr<Data> pData = TSharedPtr<Data>::Make();
				pData->m_bShouldKeepCpuBuffers = pAssetInfo->ShouldKeepCpuBuffers();
				pData->m_bShouldGenerateBLAS = pAssetInfo->ShouldGenerateBLAS();
				pData->m_bIsImported = ImportModel(pAssetInfo->GetAssetFilepath(),
					pAssetInfo->GetUnitScale(),
					pAssetInfo->ShouldBatchByMaterial(),
					pAssetInfo->ShouldFlipTexcoordY(),
					pData->m_parsedMeshes,
					boundsAabb,
					boundsSphere,
					pData->m_inverseBind,
					&pData->m_gltfModel);
				if (pData->m_bIsImported)
				{
					ModelLodGeneration::Prepare(*pAssetInfo, pData->m_parsedMeshes);
				}
				if (pData->m_bIsImported)
				{
					pData->m_bIsImported = GltfImporterUtils::CollectSceneNodes(
						pData->m_gltfModel, pAssetInfo->GetUnitScale(), pData->m_sceneNodes);
				}
				if (pData->m_bIsImported)
				{
					pData->m_sourceMeshNames.Reserve(pData->m_gltfModel.meshes.size());
					for (size_t meshIndex = 0; meshIndex < pData->m_gltfModel.meshes.size(); ++meshIndex)
					{
						const std::string& sourceName = pData->m_gltfModel.meshes[meshIndex].name;
						pData->m_sourceMeshNames.Add(
							sourceName.empty() ? "Mesh_" + std::to_string(meshIndex) : sourceName);
					}
				}
				return pData;
			});
		auto migrationTask = loadDataTask->Then(
			[this, pAssetInfo, uid](TSharedPtr<Data> pData)
			{
				if (pData->m_bIsImported)
				{
					UpdateGeneratedMaterialPropertiesOnDemand(pAssetInfo, pData->m_gltfModel);
				}
				pData->m_gltfModel = tinygltf::Model();
				m_generatedMaterialMigrationTasks.Remove(uid);
			},
			"Migrate generated model materials",
			EThreadType::Main);
		m_generatedMaterialMigrationTasks.At_Lock(uid, nullptr) = migrationTask;
		m_generatedMaterialMigrationTasks.Unlock(uid);

		promise =
			loadDataTask
				->Then<ModelPtr>(
					[pModel](TSharedPtr<Data> pData) mutable
					{
						if (pData->m_bIsImported)
						{
							pModel->m_meshes.Clear();
							pModel->m_cpuMeshes.Clear();
							pModel->m_nodes.Clear();
							pModel->m_sourceMeshes.Clear();
							pModel->m_renderInstances.Clear();
							pModel->m_bSupportsEditableHierarchy = true;
							pModel->m_meshes.Reserve(pData->m_parsedMeshes.Num());
							pModel->m_sourceMeshes.Resize(pData->m_sourceMeshNames.Num());
							for (size_t sourceMeshIndex = 0; sourceMeshIndex < pData->m_sourceMeshNames.Num();
								++sourceMeshIndex)
							{
								pModel->m_sourceMeshes[sourceMeshIndex].m_name =
									std::move(pData->m_sourceMeshNames[sourceMeshIndex]);
							}
							if (pData->m_bShouldKeepCpuBuffers || pData->m_bShouldGenerateBLAS)
							{
								pModel->m_cpuMeshes.Reserve(pData->m_parsedMeshes.Num());
							}

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
								TVector<RHI::VertexP3N3T3B3UV2C4I4W4> uploadVertices = mesh.outVertices;
								TVector<uint32_t> uploadIndices = mesh.outIndices;
								TVector<uint32_t> lodVertexOffsets;
								TVector<uint32_t> lodFirstIndices;
								lodVertexOffsets.Reserve(mesh.lods.Num());
								lodFirstIndices.Reserve(mesh.lods.Num());
								for (const auto& lod : mesh.lods)
								{
									lodVertexOffsets.Add(static_cast<uint32_t>(uploadVertices.Num()));
									lodFirstIndices.Add(static_cast<uint32_t>(uploadIndices.Num()));
									uploadVertices.AddRange(lod.m_vertices);
									uploadIndices.AddRange(lod.m_indices);
								}
								pMesh->m_indexCount = static_cast<uint32_t>(mesh.outIndices.Num());
								pMesh->m_firstIndex = 0u;
								pMesh->m_vertexOffset = 0u;
								RHI::Renderer::GetDriver()->UpdateMesh(pMesh,
									uploadVertices.GetData(),
									sizeof(RHI::VertexP3N3T3B3UV2C4I4W4) * uploadVertices.Num(),
									uploadIndices.GetData(),
									sizeof(uint32_t) * uploadIndices.Num());
								pMesh->m_lods.Reserve(mesh.lods.Num());
								for (size_t lodIndex = 0; lodIndex < mesh.lods.Num(); ++lodIndex)
								{
									const auto& lodGeometry = mesh.lods[lodIndex];
									if (lodGeometry.m_vertices.IsEmpty() || lodGeometry.m_indices.IsEmpty())
									{
										continue;
									}

									RHI::RHIMeshPtr lodMesh = RHI::Renderer::GetDriver()->CreateMesh();
									lodMesh->m_vertexDescription = pMesh->m_vertexDescription;
									lodMesh->m_vertexBuffer = pMesh->m_vertexBuffer;
									lodMesh->m_indexBuffer = pMesh->m_indexBuffer;
									lodMesh->m_bounds = pMesh->m_bounds;
									lodMesh->m_materialIndex = pMesh->m_materialIndex;
									lodMesh->m_bakedVolumeScale = pMesh->m_bakedVolumeScale;
									lodMesh->m_indexCount = static_cast<uint32_t>(lodGeometry.m_indices.Num());
									lodMesh->m_firstIndex = lodFirstIndices[lodIndex];
									lodMesh->m_vertexOffset = lodVertexOffsets[lodIndex];
									pMesh->m_lods.Add(std::move(lodMesh));
								}

								const uint32_t renderMeshIndex = static_cast<uint32_t>(pModel->m_meshes.Num());
								pModel->m_meshes.Emplace(pMesh);
								if (mesh.sourceMeshIndex >= 0 &&
									static_cast<size_t>(mesh.sourceMeshIndex) < pModel->m_sourceMeshes.Num())
								{
									Model::SourceMesh& sourceMesh =
										pModel->m_sourceMeshes[static_cast<size_t>(mesh.sourceMeshIndex)];
									sourceMesh.m_renderMeshIndices.Add(renderMeshIndex);
									sourceMesh.m_bounds.Extend(mesh.bounds);
								}

								if (pData->m_bShouldKeepCpuBuffers || pData->m_bShouldGenerateBLAS)
								{
									Model::MeshCpuData cpuMesh{};
									cpuMesh.m_vertices = std::move(mesh.outVertices);
									cpuMesh.m_indices = std::move(mesh.outIndices);
									cpuMesh.m_bounds = mesh.bounds;
									cpuMesh.m_materialIndex = mesh.materialIndex;
									pModel->m_cpuMeshes.Add(std::move(cpuMesh));
								}
							}

							ModelImporter::PopulateModelSceneHierarchy(*pModel, pData->m_sceneNodes);

							pModel->m_inverseBind = std::move(pData->m_inverseBind);
							pModel->ProceedCpuMeshes(pData->m_bShouldGenerateBLAS, pData->m_bShouldKeepCpuBuffers);
							pModel->Flush();
						}
						return pModel;
					},
					"Update RHI Meshes",
					EThreadType::RHI)
				->ToTaskWithResult();

		outModel = loadedModel = pModel;
		promise->Run();

		m_loadedModels.Unlock(uid);
		m_promises.Unlock(uid);

		return promise;
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
	return task->GetResult().IsValid();
}

Tasks::TaskPtr<bool> ModelImporter::LoadDefaultMaterials(FileId uid, TVector<MaterialPtr>& outMaterials)
{
	outMaterials.Clear();

	if (ModelAssetInfoPtr modelInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		Tasks::TaskPtr<bool> loadingFinished =
			Tasks::CreateTaskWithResult<bool>("Load Default Materials", []() { return true; });
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

		App::GetSubmodule<Tasks::Scheduler>()->Run(loadingFinished);
		return loadingFinished;
	}

	return Tasks::TaskPtr<bool>::Make(false);
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

	LoadModel(uid, outModel);
	out = outModel;
	return true;
}

void ModelImporter::CollectGarbage()
{
	TVector<FileId> uidsToRemove;

	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto promise = m_promises.At_Lock(id);

		if (!promise.IsValid() || (promise.IsValid() && promise->IsFinished()))
		{
			FileId uid = id;
			uidsToRemove.Emplace(uid);
		}

		m_promises.Unlock(id);
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}
