#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "Core/Utils.h"
#include "Raytracing/MaterialUtils.h"
#include "Raytracing/PathTracer.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <tiny_gltf.h>

using namespace Sailor;

namespace
{
	class ControllableMesh final : public RHI::RHIMesh
	{
	public:
		bool IsReady() const override
		{
			return m_bReady;
		}

		void SetReady(bool bReady)
		{
			m_bReady = bReady;
		}

	private:
		bool m_bReady = false;
	};

	class PathTracerBasisProbe final : public Raytracing::PathTracer
	{
	public:
		void Evaluate(
			const ModelPtr& model,
			const glm::mat4& worldMatrix,
			glm::vec3& outNormal,
			glm::vec3& outTangent,
			glm::vec3& outBitangent)
		{
			m_tlasInstances.Clear();
			TLASInstance instance{};
			instance.m_model = model;
			instance.m_worldMatrix = worldMatrix;
			instance.m_inverseWorldMatrix = glm::inverse(worldMatrix);
			m_tlasInstances.Add(std::move(instance));

			TLASHit hit{};
			hit.m_instanceIndex = 0;
			hit.m_triangleIndex = 0;
			hit.m_hit.m_barycentricCoordinate = glm::vec3(1.0f, 0.0f, 0.0f);
			GetShadingBasis(hit, outNormal, outTangent, outBitangent);
		}

		bool OrientAgainstRay(
			const glm::vec3& rayDirection,
			glm::vec3& inOutNormal,
			glm::vec3& inOutBitangent)
		{
			return OrientShadingBasisAgainstRay(
				rayDirection,
				inOutNormal,
				inOutBitangent);
		}
	};

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(),
			"test source should be readable: " + path.generic_string());
		return std::string(
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	RHI::VertexP3N3T3B3UV2C4I4W4 MakeVertex(const glm::vec3& position)
	{
		RHI::VertexP3N3T3B3UV2C4I4W4 vertex{};
		vertex.m_position = position;
		vertex.m_normal = glm::vec3(0.0f, 0.0f, 1.0f);
		vertex.m_tangent = glm::vec3(1.0f, 0.0f, 0.0f);
		vertex.m_bitangent = glm::vec3(0.0f, 1.0f, 0.0f);
		return vertex;
	}

	bool NearlyEqual(float lhs, float rhs, float epsilon = 0.0001f)
	{
		return std::abs(lhs - rhs) <= epsilon;
	}

	void RequireVec3Near(
		const glm::vec3& actual,
		const glm::vec3& expected,
		const std::string& message)
	{
		Require(
			NearlyEqual(actual.x, expected.x) &&
				NearlyEqual(actual.y, expected.y) &&
				NearlyEqual(actual.z, expected.z),
			message);
	}

	Model::MeshCpuData MakeTriangleMesh(uint32_t thirdIndex)
	{
		Model::MeshCpuData mesh;
		mesh.m_vertices = {
			MakeVertex(glm::vec3(0.0f, 0.0f, 0.0f)),
			MakeVertex(glm::vec3(1.0f, 0.0f, 0.0f)),
			MakeVertex(glm::vec3(0.0f, 1.0f, 0.0f))
		};
		mesh.m_indices = { 0, 1, thirdIndex };
		return mesh;
	}

	void TestModelReadinessTracksMeshUploads()
	{
		auto firstMesh = TRefPtr<ControllableMesh>::Make();
		auto secondMesh = TRefPtr<ControllableMesh>::Make();
		Model model(
			FileId{},
			TVector<RHI::RHIMeshPtr>{ firstMesh, secondMesh });

		model.Flush();
		Require(!model.IsReady(),
			"a structurally complete model must wait for every mesh upload");

		firstMesh->SetReady(true);
		Require(!model.IsReady(),
			"one completed mesh upload must not publish the whole model");

		secondMesh->SetReady(true);
		Require(model.IsReady(),
			"the model must become ready without another Flush once all uploads finish");

		Model emptyModel(FileId{});
		emptyModel.Flush();
		Require(!emptyModel.IsReady(),
			"an empty model must remain unavailable");

		Model modelWithNullMesh(
			FileId{},
			TVector<RHI::RHIMeshPtr>{ RHI::RHIMeshPtr{} });
		modelWithNullMesh.Flush();
		Require(!modelWithNullMesh.IsReady(),
			"a model with a null mesh must remain unavailable");
	}

	void TestMeshContextRejectsEmptyGpuUploads()
	{
		ModelImporter::MeshContext emptyMesh;
		Require(!emptyMesh.HasGeometry(),
			"an empty mesh context must not reach the GPU upload path");

		emptyMesh.outVertices.Add(MakeVertex(glm::vec3(0.0f)));
		Require(!emptyMesh.HasGeometry(),
			"vertices without indices must not reach the GPU upload path");

		emptyMesh.outIndices = { 0, 0, 0 };
		Require(emptyMesh.HasGeometry(),
			"a mesh context with vertices and indices must remain uploadable");
	}

	void TestGltfAlphaModesResolveRenderState()
	{
		const auto transparent =
			GltfImporterUtils::ResolveMaterialAlphaMode("BLEND");
		Require(std::string(transparent.m_renderQueue) == "Transparent",
			"glTF BLEND materials must use the Transparent queue");
		Require(!transparent.m_bEnableZWrite,
			"glTF BLEND materials must not write depth");
		Require(!transparent.m_bAlphaCutout,
			"glTF BLEND materials must not use the alpha-cutout path");
		Require(transparent.m_blendMode == RHI::EBlendMode::AlphaBlending,
			"glTF BLEND materials must enable alpha blending");

		const auto masked =
			GltfImporterUtils::ResolveMaterialAlphaMode("MASK");
		Require(std::string(masked.m_renderQueue) == "Masked",
			"glTF MASK materials must use the Masked queue");
		Require(masked.m_bEnableZWrite && masked.m_bAlphaCutout,
			"glTF MASK materials must write depth through the cutout path");
		Require(masked.m_blendMode == RHI::EBlendMode::None,
			"glTF MASK materials must not enable alpha blending");

		for (const std::string alphaMode : { "OPAQUE", "", "UNKNOWN" })
		{
			const auto opaque =
				GltfImporterUtils::ResolveMaterialAlphaMode(alphaMode);
			Require(std::string(opaque.m_renderQueue) == "Opaque",
				"non-BLEND glTF materials must default to the Opaque queue");
			Require(opaque.m_bEnableZWrite && !opaque.m_bAlphaCutout,
				"opaque glTF materials must retain the default depth path");
			Require(opaque.m_blendMode == RHI::EBlendMode::None,
				"opaque glTF materials must not enable blending");
		}

		const auto transmission =
			GltfImporterUtils::ResolveMaterialAlphaMode("OPAQUE", true);
		Require(std::string(transmission.m_renderQueue) == "Transparent",
			"glTF transmission materials must use the Transparent queue");
		Require(!transmission.m_bEnableZWrite &&
			!transmission.m_bAlphaCutout,
			"opaque glTF transmission must not write depth or use alpha cutout");
		Require(transmission.m_blendMode == RHI::EBlendMode::None,
			"glTF transmission must compose the opaque framebuffer without alpha blending");

		const auto blendedTransmission =
			GltfImporterUtils::ResolveMaterialAlphaMode("BLEND", true);
		Require(blendedTransmission.m_blendMode ==
			RHI::EBlendMode::AlphaBlending,
			"alpha coverage must retain alpha blending when combined with transmission");

		const auto maskedTransmission =
			GltfImporterUtils::ResolveMaterialAlphaMode("MASK", true);
		Require(maskedTransmission.m_bAlphaCutout &&
			!maskedTransmission.m_bEnableZWrite,
			"masked transmission must retain cutout without writing depth");
	}

	void TestMaterialAssetRetainsRenderQueue()
	{
		YAML::Node source;
		source["renderQueue"] = "Transparent";

		MaterialAsset materialAsset;
		materialAsset.Deserialize(source);

		Require(materialAsset.GetRenderQueue() == "Transparent",
			"material deserialization must retain its authored render queue");
		Require(materialAsset.GetRenderState().GetTag() ==
			GetHash(std::string("Transparent")),
			"material render state tag must match the retained render queue");
	}

	void TestGltfTransmissionExtensionResolvesMaterialFields()
	{
		tinygltf::Material material;
		Require(!GltfImporterUtils::ResolveMaterialTransmission(
			material,
			2).IsEnabled(),
			"materials without KHR_materials_transmission must remain disabled");

		tinygltf::Value::Object textureInfo;
		textureInfo.emplace("index", tinygltf::Value(1));
		tinygltf::Value::Object extension;
		extension.emplace("transmissionFactor", tinygltf::Value(1.25));
		extension.emplace(
			"transmissionTexture",
			tinygltf::Value(std::move(textureInfo)));
		material.extensions["KHR_materials_transmission"] =
			tinygltf::Value(std::move(extension));

		const auto transmission =
			GltfImporterUtils::ResolveMaterialTransmission(material, 2);
		Require(transmission.IsEnabled() && transmission.m_factor == 1.0f,
			"glTF transmission factor must be enabled and clamped to one");
		Require(transmission.m_textureIndex == 1,
			"valid glTF transmission textures must retain their index");
		Require(transmission.m_thicknessFactor == 0.0f &&
			transmission.m_thicknessTextureIndex == -1 &&
			transmission.m_attenuationColor == glm::vec3(1.0f) &&
			transmission.m_attenuationDistance ==
				(std::numeric_limits<float>::max)() &&
			transmission.m_indexOfRefraction == 1.5f,
			"transmission materials must retain glTF volume and IOR defaults");

		tinygltf::Value::Object thicknessTexture;
		thicknessTexture.emplace("index", tinygltf::Value(0));
		tinygltf::Value::Array attenuationColor{
			tinygltf::Value(0.25),
			tinygltf::Value(0.5),
			tinygltf::Value(0.75)
		};
		tinygltf::Value::Object volumeExtension;
		volumeExtension.emplace("thicknessFactor", tinygltf::Value(0.22));
		volumeExtension.emplace(
			"thicknessTexture",
			tinygltf::Value(std::move(thicknessTexture)));
		volumeExtension.emplace(
			"attenuationColor",
			tinygltf::Value(std::move(attenuationColor)));
		volumeExtension.emplace("attenuationDistance", tinygltf::Value(2.0));
		material.extensions["KHR_materials_volume"] =
			tinygltf::Value(std::move(volumeExtension));

		tinygltf::Value::Object iorExtension;
		iorExtension.emplace("ior", tinygltf::Value(1.33));
		material.extensions["KHR_materials_ior"] =
			tinygltf::Value(std::move(iorExtension));

		const auto volumeTransmission =
			GltfImporterUtils::ResolveMaterialTransmission(material, 2);
		Require(std::abs(volumeTransmission.m_thicknessFactor - 0.22f) <
			0.0001f,
			"glTF volume thickness must be imported");
		Require(volumeTransmission.m_thicknessTextureIndex == 0,
			"valid glTF thickness textures must retain their index");
		Require(volumeTransmission.m_attenuationColor ==
			glm::vec3(0.25f, 0.5f, 0.75f),
			"glTF attenuation color must be imported");
		Require(volumeTransmission.m_attenuationDistance == 2.0f,
			"glTF attenuation distance must be imported");
		Require(std::abs(volumeTransmission.m_indexOfRefraction - 1.33f) <
			0.0001f,
			"glTF index of refraction must be imported");

		const auto scaledVolumeTransmission =
			GltfImporterUtils::ResolveMaterialTransmission(
				material,
				2,
				10000.0f);
		Require(std::abs(scaledVolumeTransmission.m_thicknessFactor -
			2200.0f) < 0.01f,
			"glTF volume thickness must follow the model unit scale");
		Require(std::abs(scaledVolumeTransmission.m_attenuationDistance -
			20000.0f) < 0.01f,
			"glTF attenuation distance must follow the model unit scale");

		const auto invalidTexture =
			GltfImporterUtils::ResolveMaterialTransmission(material, 1);
		Require(invalidTexture.m_textureIndex == -1,
			"out-of-range glTF transmission textures must be ignored");
	}

	void TestGeneratedMaterialMigrationPreservesAuthoredProperties()
	{
		YAML::Node material = YAML::Load(R"(
renderQueue: Opaque
bEnableZWrite: true
bCustomDepthShader: true
blendMode: AlphaBlending
cullMode: Front
shaderUid: custom-shader
defines:
- CLEAR_COAT
- CUSTOM_FEATURE
- ALPHA_CUTOUT
uniformsFloat:
  material.roughnessFactor: 0.37
  material.alphaCutoff: 0.1
  material.transmissionFactor: 0.25
uniformsVec4:
  material.baseColorFactor: [0.1, 0.2, 0.3, 1.0]
  material.attenuationColor: [1.0, 0.0, 0.0, 1.0]
samplers:
  baseColorSampler: authored-base-color
  transmissionSampler: stale-transmission
  thicknessSampler: stale-thickness
)");
		YAML::Node generated = YAML::Load(R"(
renderQueue: Transparent
bEnableZWrite: false
bCustomDepthShader: false
blendMode: None
defines:
- TRANSMISSION
uniformsFloat:
  material.alphaCutoff: 0.5
  material.transmissionFactor: 1.0
  material.thicknessFactor: 2.0
  material.attenuationDistance: 4.0
  material.indexOfRefraction: 1.5
uniformsVec4:
  material.attenuationColor: [0.8, 0.8, 0.8, 1.0]
samplers:
  transmissionSampler: generated-transmission
)");

		Require(GltfImporterUtils::MergeGeneratedMaterialProperties(
			material,
			generated),
			"valid generated material properties must be mergeable");
		Require(material["renderQueue"].as<std::string>() == "Transparent" &&
			!material["bEnableZWrite"].as<bool>() &&
			!material["bCustomDepthShader"].as<bool>() &&
			material["blendMode"].as<std::string>() == "None",
			"alpha and transmission render state must follow the glTF source");
		Require(material["cullMode"].as<std::string>() == "Front" &&
			material["shaderUid"].as<std::string>() == "custom-shader",
			"unrelated authored render and shader properties must be preserved");
		Require(material["uniformsFloat"]["material.roughnessFactor"]
			.as<float>() == 0.37f &&
			material["uniformsFloat"]["material.transmissionFactor"]
			.as<float>() == 1.0f &&
			material["uniformsFloat"]["material.alphaCutoff"]
			.as<float>() == 0.5f,
			"migration must replace only importer-owned scalar uniforms");
		Require(material["samplers"]["baseColorSampler"].as<std::string>() ==
			"authored-base-color" &&
			material["samplers"]["transmissionSampler"].as<std::string>() ==
				"generated-transmission" &&
			!material["samplers"]["thicknessSampler"],
			"migration must preserve authored samplers and remove stale managed samplers");

		bool bHasClearCoat = false;
		bool bHasCustomFeature = false;
		bool bHasTransmission = false;
		bool bHasAlphaCutout = false;
		for (const YAML::Node& define : material["defines"])
		{
			const std::string value = define.as<std::string>();
			bHasClearCoat |= value == "CLEAR_COAT";
			bHasCustomFeature |= value == "CUSTOM_FEATURE";
			bHasTransmission |= value == "TRANSMISSION";
			bHasAlphaCutout |= value == "ALPHA_CUTOUT";
		}
		Require(bHasClearCoat && bHasCustomFeature && bHasTransmission &&
			!bHasAlphaCutout,
			"migration must preserve unrelated defines and replace managed defines");

		YAML::Node opaqueGenerated = YAML::Load(R"(
renderQueue: Opaque
bEnableZWrite: true
bCustomDepthShader: false
blendMode: None
defines: []
uniformsFloat:
  material.alphaCutoff: 0.25
)");
		Require(GltfImporterUtils::MergeGeneratedMaterialProperties(
			material,
			opaqueGenerated),
			"transmission removal must be mergeable");
		Require(!material["uniformsFloat"]["material.transmissionFactor"] &&
			!material["uniformsFloat"]["material.thicknessFactor"] &&
			!material["uniformsVec4"]["material.attenuationColor"] &&
			!material["samplers"]["transmissionSampler"],
			"removing the glTF extension must remove stale managed properties");

		YAML::Node malformed = YAML::Load("defines: INVALID");
		const YAML::Node malformedBefore = YAML::Clone(malformed);
		Require(!GltfImporterUtils::MergeGeneratedMaterialProperties(
			malformed,
			generated) &&
			Utils::AreYamlNodesEqual(malformed, malformedBefore),
			"malformed authored YAML must be rejected without partial mutation");
	}

	void TestGeneratedMaterialMigrationRecognizesLegacyOwnership()
	{
		const std::filesystem::path sourceRoot =
			std::filesystem::path(__FILE__).parent_path().parent_path();
		const std::string importer = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Model/ModelImporter.cpp");
		const size_t migrationBegin = importer.find(
			"bool ModelImporter::UpdateGeneratedMaterialProperties(");
		const size_t migrationEnd = importer.find(
			"Tasks::TaskPtr<ModelPtr> ModelImporter::LoadModel",
			migrationBegin);
		Require(migrationBegin != std::string::npos &&
			migrationEnd != std::string::npos,
			"generated material migration implementation must remain present");
		const std::string migration = importer.substr(
			migrationBegin,
			migrationEnd - migrationBegin);

		for (const std::string contract : {
			"sanitizeLegacyMaterialStem",
			"gltfModel.materials[materialIndex].name",
			"legacyMaterialPath",
			"defaultMaterials[materialIndex]",
			"std::filesystem::equivalent",
			"GetAllAssetInfos<TextureAssetInfo>",
			"TMap<int32_t, FileId> textureIdsByGltfIndex",
			"ModelImporter::CreateTextureAsset(",
			"generatedTextureVirtualPath",
			"m_generatedMaterialMigrationComplete.At_Lock",
			"assetRegistry->UpdateAsset(materialId)",
			"AtomicReplaceWorkspaceCacheText" })
		{
			Require(migration.find(contract) != std::string::npos,
				"generated material migration lost its ownership/reload contract: " +
					contract);
		}

		const size_t textureHelperBegin = importer.find(
			"FileId ModelImporter::CreateTextureAsset(");
		const size_t textureHelperEnd = importer.find(
			"FileId CreateAnimationAsset(",
			textureHelperBegin);
		Require(textureHelperBegin != std::string::npos &&
			textureHelperEnd != std::string::npos,
			"generated texture identity helper must remain present");
		const std::string textureHelper = importer.substr(
			textureHelperBegin,
			textureHelperEnd - textureHelperBegin);
		Require(textureHelper.find(
				"assetRegistry->RegisterGeneratedSecondaryAssetInfo(filepath)") !=
				std::string::npos &&
			textureHelper.find("FileId::CreateNewFileId()") !=
				std::string::npos &&
			textureHelper.find("existingTextureInfo->GetGlbTextureIndex()") !=
				std::string::npos &&
			textureHelper.find("AtomicReplaceWorkspaceCacheText") !=
				std::string::npos,
			"generated texture updates must reuse a compatible existing FileId "
			"and allocate only a genuinely new secondary asset");

		const size_t customSkipBegin = migration.find(
			"if (!findOwnedMaterial(");
		const size_t customSkipEnd = migration.find(
			"const tinygltf::Material& sourceMaterial",
			customSkipBegin);
		Require(customSkipBegin != std::string::npos &&
			customSkipEnd != std::string::npos,
			"custom material ownership check must remain present");
		const std::string customSkip = migration.substr(
			customSkipBegin,
			customSkipEnd - customSkipBegin);
		Require(customSkip.find("continue;") != std::string::npos &&
			customSkip.find("bSucceeded = false") == std::string::npos,
			"preserving a custom replacement is a successful skip and must not "
			"leave on-demand migration retrying forever");

		const size_t loadModelBegin = importer.find(
			"Tasks::TaskPtr<ModelPtr> ModelImporter::LoadModel(");
		const size_t loadModelEnd = importer.find(
			"bool ModelImporter::LoadModel_Immediate(",
			loadModelBegin);
		Require(loadModelBegin != std::string::npos &&
			loadModelEnd != std::string::npos,
			"model load implementation must remain present");
		const std::string loadModel = importer.substr(
			loadModelBegin,
			loadModelEnd - loadModelBegin);
		Require(loadModel.find("&pData->m_gltfModel") != std::string::npos &&
			loadModel.find(
				"UpdateGeneratedMaterialPropertiesOnDemand") !=
				std::string::npos &&
			loadModel.find("EThreadType::Main") != std::string::npos &&
			loadModel.find(
				"m_generatedMaterialMigrationTasks") != std::string::npos,
			"unchanged legacy materials must migrate on demand from the "
			"already parsed model on the main thread without blocking RHI upload");
	}

	void TestGltfMaterialTextureColorSpaces()
	{
		const std::filesystem::path sourceRoot =
			std::filesystem::path(__FILE__).parent_path().parent_path();
		const std::string importer = ReadText(
			sourceRoot /
			"Runtime/AssetRegistry/Model/ModelImporter.cpp");

		for (const std::string textureSuffix : {
			"_ormTexture.png.asset",
			"_occlusionTexture.png.asset",
			"_clearcoatTexture.png.asset",
			"_clearcoatRoughnessTexture.png.asset",
			"_sheenRoughnessTexture.png.asset" })
		{
			const size_t textureOffset = importer.find(textureSuffix);
			Require(textureOffset != std::string::npos,
				"glTF data texture generation should remain present: " +
				textureSuffix);
			const size_t lineEnd = importer.find('\n', textureOffset);
			Require(importer.substr(textureOffset, lineEnd - textureOffset)
				.find("R8G8B8A8_UNORM") != std::string::npos,
				"glTF scalar/data textures must bypass sRGB decoding: " +
				textureSuffix);
		}

		for (const std::string textureSuffix : {
			"_baseColorTexture.png.asset",
			"_emissionTexture.png.asset",
			"_sheenColorTexture.png.asset" })
		{
			const size_t textureOffset = importer.find(textureSuffix);
			Require(textureOffset != std::string::npos,
				"glTF color texture generation should remain present: " +
				textureSuffix);
			const size_t lineEnd = importer.find('\n', textureOffset);
			Require(importer.substr(textureOffset, lineEnd - textureOffset)
				.find("R8G8B8A8_SRGB") != std::string::npos,
				"glTF color textures must retain sRGB decoding: " +
				textureSuffix);
		}
	}

	void TestCompactedMeshesRetainMaterialSlots()
	{
		RHI::RHIMesh mesh;
		Require(mesh.ResolveMaterialIndex(1, 3) == 1,
			"generated meshes must retain positional material fallback");

		mesh.m_materialIndex = 2;
		Require(mesh.ResolveMaterialIndex(0, 3) == 2,
			"a compacted mesh must retain its original material slot");
		Require(mesh.ResolveMaterialIndex(0, 0) ==
			(std::numeric_limits<size_t>::max)(),
			"material resolution without materials must remain invalid");
	}

	void TestGeneratedTangentsPreserveMirroredUvHandedness()
	{
		const glm::vec3 vertices[] = {
			glm::vec3(0.0f, 0.0f, 0.0f),
			glm::vec3(1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f)
		};
		const glm::vec2 regularUvs[] = {
			glm::vec2(0.0f, 0.0f),
			glm::vec2(1.0f, 0.0f),
			glm::vec2(0.0f, 1.0f)
		};
		const glm::vec2 mirroredUvs[] = {
			glm::vec2(0.0f, 0.0f),
			glm::vec2(-1.0f, 0.0f),
			glm::vec2(0.0f, 1.0f)
		};
		const glm::vec3 normal(0.0f, 0.0f, 1.0f);

		glm::vec3 tangent(0.0f);
		glm::vec3 bitangent(0.0f);
		Raytracing::GenerateTangentBitangent(
			tangent,
			bitangent,
			vertices,
			regularUvs);
		RequireVec3Near(tangent, glm::vec3(1.0f, 0.0f, 0.0f),
			"regular UVs must produce the expected tangent");
		RequireVec3Near(bitangent, glm::vec3(0.0f, 1.0f, 0.0f),
			"regular UVs must produce the expected bitangent");
		Require(glm::dot(glm::cross(normal, tangent), bitangent) > 0.0f,
			"regular UVs must retain positive tangent-space handedness");

		tangent = glm::vec3(0.0f);
		bitangent = glm::vec3(0.0f);
		Raytracing::GenerateTangentBitangent(
			tangent,
			bitangent,
			vertices,
			mirroredUvs);
		RequireVec3Near(tangent, glm::vec3(-1.0f, 0.0f, 0.0f),
			"mirrored UVs must produce the mirrored tangent");
		RequireVec3Near(bitangent, glm::vec3(0.0f, 1.0f, 0.0f),
			"mirrored UVs must retain the UV-derived bitangent");
		Require(glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f,
			"mirrored UVs must retain negative tangent-space handedness");
	}

	void TestPathTracerTransformsShadingBasisCorrectly()
	{
		const glm::vec3 localNormal = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
		const glm::vec3 localTangent = glm::normalize(glm::vec3(1.0f, 0.0f, -1.0f));
		const glm::vec3 localBitangent = -glm::normalize(
			glm::cross(localNormal, localTangent));

		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::LocalMemory_SingleThread);
		ModelPtr model = ModelPtr::Make(allocator, FileId{});
		Model::MeshCpuData mesh = MakeTriangleMesh(2);
		for (auto& vertex : mesh.m_vertices)
		{
			vertex.m_normal = localNormal;
			vertex.m_tangent = localTangent;
			vertex.m_bitangent = localBitangent;
		}
		model->GetCpuMeshes().Add(std::move(mesh));
		Require(model->BuildBLAS(),
			"path tracer basis fixture must build its BLAS");

		const glm::mat4 worldMatrix = glm::scale(
			glm::mat4(1.0f),
			glm::vec3(2.0f, 1.0f, 0.5f));
		const glm::mat3 linearMatrix(worldMatrix);
		const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linearMatrix));
		const glm::vec3 expectedNormal = glm::normalize(normalMatrix * localNormal);
		const glm::vec3 transformedTangent = linearMatrix * localTangent;
		const glm::vec3 expectedTangent = glm::normalize(
			transformedTangent - expectedNormal *
				glm::dot(expectedNormal, transformedTangent));
		const glm::vec3 transformedBitangent = linearMatrix * localBitangent;
		const float expectedHandedness = glm::dot(
			glm::cross(expectedNormal, expectedTangent),
			transformedBitangent) < 0.0f ? -1.0f : 1.0f;
		const glm::vec3 expectedBitangent = glm::normalize(
			glm::cross(expectedNormal, expectedTangent)) * expectedHandedness;

		PathTracerBasisProbe pathTracer;
		glm::vec3 actualNormal(0.0f);
		glm::vec3 actualTangent(0.0f);
		glm::vec3 actualBitangent(0.0f);
		pathTracer.Evaluate(
			model,
			worldMatrix,
			actualNormal,
			actualTangent,
			actualBitangent);

		RequireVec3Near(actualNormal, expectedNormal,
			"path tracer normals must use the inverse-transpose world transform");
		RequireVec3Near(actualTangent, expectedTangent,
			"path tracer tangents must use the linear world transform and Gram-Schmidt");
		RequireVec3Near(actualBitangent, expectedBitangent,
			"path tracer bitangents must retain tangent-space handedness");
		Require(NearlyEqual(glm::dot(actualNormal, actualTangent), 0.0f) &&
			NearlyEqual(glm::dot(actualNormal, actualBitangent), 0.0f) &&
			NearlyEqual(glm::dot(actualTangent, actualBitangent), 0.0f),
			"path tracer world-space TBN must remain orthogonal under non-uniform scale");

		const glm::vec3 frontNormal = actualNormal;
		const glm::vec3 frontTangent = actualTangent;
		const glm::vec3 frontBitangent = actualBitangent;
		Require(!pathTracer.OrientAgainstRay(
				frontNormal,
				actualNormal,
				actualBitangent),
			"a ray traveling with the shading normal must hit the back face");
		RequireVec3Near(actualNormal, -frontNormal,
			"back-face orientation must flip the shading normal");
		RequireVec3Near(actualTangent, frontTangent,
			"back-face orientation must preserve the tangent direction");
		RequireVec3Near(actualBitangent, -frontBitangent,
			"back-face orientation must flip the bitangent with the normal");
		Require(
			glm::dot(
				glm::cross(actualNormal, actualTangent),
				actualBitangent) *
			glm::dot(
				glm::cross(frontNormal, frontTangent),
				frontBitangent) > 0.0f,
			"back-face orientation must preserve tangent-space handedness");
	}

	void TestBuildBlasRejectsOutOfRangeIndicesAtomically()
	{
		Model model(FileId{});
		model.GetCpuMeshes() = {
			MakeTriangleMesh(2),
			MakeTriangleMesh(3)
		};

		Require(!model.BuildBLAS(),
			"BLAS construction must reject a mesh with an out-of-range index");
		Require(!model.HasBLAS(),
			"a rejected mesh must not retain a BLAS");
		Require(model.GetBLASTriangles().Num() == 0,
			"a rejected mesh must not retain triangles from earlier valid meshes");
		Require(!model.BuildBLAS(),
			"repeated BLAS construction on malformed geometry must remain safe");
		Require(!model.HasBLAS() && model.GetBLASTriangles().Num() == 0,
			"repeated rejection must preserve clean BLAS state");
	}

	void TestBuildBlasRecoversAfterGeometryIsCorrected()
	{
		Model model(FileId{});
		model.GetCpuMeshes() = { MakeTriangleMesh(3) };
		Require(!model.BuildBLAS(),
			"the malformed geometry precondition must be rejected");

		model.GetCpuMeshes() = { MakeTriangleMesh(2), MakeTriangleMesh(2) };
		Require(model.BuildBLAS(),
			"BLAS construction must recover after geometry is corrected");
		Require(model.HasBLAS(),
			"corrected geometry must produce a BLAS");
		Require(model.GetBLASTriangles().Num() == 2,
			"both corrected triangles must be included");
	}

	void TestBuildBlasIgnoresEmptyMeshes()
	{
		Model model(FileId{});
		model.GetCpuMeshes() = {
			Model::MeshCpuData(),
			MakeTriangleMesh(2)
		};
		Require(model.BuildBLAS(),
			"an empty mesh must not prevent valid geometry from building");
		Require(model.GetBLASTriangles().Num() == 1,
			"only the valid mesh must contribute a triangle");

		model.GetCpuMeshes() = { Model::MeshCpuData() };
		Require(!model.BuildBLAS(),
			"a model containing only empty meshes must be rejected");
		Require(!model.HasBLAS() && model.GetBLASTriangles().Num() == 0,
			"an empty model must leave BLAS state clean");
		Require(!model.GetBLAS().IsValid(),
			"a failed rebuild must release the previous BLAS");
	}

	void TestBuildBlasRejectsIncompleteAndNonFiniteGeometry()
	{
		Model model(FileId{});
		Model::MeshCpuData incomplete = MakeTriangleMesh(2);
		incomplete.m_indices.Add(0);
		model.GetCpuMeshes() = { std::move(incomplete) };
		Require(!model.BuildBLAS(),
			"BLAS construction must reject an incomplete triangle");
		Require(!model.HasBLAS() && model.GetBLASTriangles().Num() == 0,
			"an incomplete triangle must leave BLAS state clean");

		Model::MeshCpuData nonFinite = MakeTriangleMesh(2);
		nonFinite.m_vertices[0].m_position.x =
			std::numeric_limits<float>::quiet_NaN();
		model.GetCpuMeshes() = { std::move(nonFinite) };
		Require(!model.BuildBLAS(),
			"BLAS construction must reject non-finite positions");
		Require(!model.HasBLAS() && model.GetBLASTriangles().Num() == 0,
			"non-finite geometry must leave BLAS state clean");
	}

	void TestBuildBlasSanitizesExtremeVertexFrames()
	{
		Model model(FileId{});
		Model::MeshCpuData mesh = MakeTriangleMesh(2);
		const float maxValue = std::numeric_limits<float>::max();
		for (auto& vertex : mesh.m_vertices)
		{
			vertex.m_normal = glm::vec3(maxValue);
			vertex.m_tangent = glm::vec3(maxValue, -maxValue, 0.0f);
			vertex.m_bitangent = glm::vec3(0.0f, maxValue, -maxValue);
		}
		model.GetCpuMeshes() = { std::move(mesh) };

		Require(model.BuildBLAS(),
			"finite extreme vertex frames must be sanitized safely");
		Require(model.GetBLASTriangles().Num() == 1,
			"the sanitized triangle must be retained");
		const auto& triangle = model.GetBLASTriangles()[0];
		for (size_t i = 0; i < 3; ++i)
		{
			Require(
				std::isfinite(triangle.m_normals[i].x) &&
				std::isfinite(triangle.m_normals[i].y) &&
				std::isfinite(triangle.m_normals[i].z) &&
				std::isfinite(triangle.m_tangent[i].x) &&
				std::isfinite(triangle.m_tangent[i].y) &&
				std::isfinite(triangle.m_tangent[i].z) &&
				std::isfinite(triangle.m_bitangent[i].x) &&
				std::isfinite(triangle.m_bitangent[i].y) &&
				std::isfinite(triangle.m_bitangent[i].z),
				"sanitized vertex frames must remain finite");
		}
	}

	void TestBuildBlasHandlesExtremeCentroidRange()
	{
		Model model(FileId{});
		Model::MeshCpuData mesh;
		const float extremes[] = {
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::max(),
			-1.0f,
			0.0f,
			1.0f
		};

		for (float position : extremes)
		{
			const uint32_t firstVertex =
				static_cast<uint32_t>(mesh.m_vertices.Num());
			for (size_t i = 0; i < 3; ++i)
			{
				mesh.m_vertices.Add(MakeVertex(glm::vec3(position, 0.0f, 0.0f)));
				mesh.m_indices.Add(firstVertex + static_cast<uint32_t>(i));
			}
		}
		model.GetCpuMeshes() = { std::move(mesh) };

		Require(model.BuildBLAS(),
			"finite extreme centroid ranges must not corrupt BVH binning");
		Require(model.GetBLASTriangles().Num() == 5,
			"all extreme-range triangles must be retained");
	}

	void TestCollectMeshInstancesUsesActiveSceneHierarchy()
	{
		tinygltf::Model model;
		model.meshes.resize(1);
		model.nodes.resize(4);

		model.nodes[0].translation = { 10.0, 0.0, 0.0 };
		model.nodes[0].children = { 1 };
		model.nodes[1].mesh = 0;
		model.nodes[1].translation = { 0.0, 2.0, 0.0 };
		model.nodes[1].rotation = {
			0.0,
			0.0,
			0.7071067811865475,
			0.7071067811865476
		};
		model.nodes[1].scale = { 2.0, 1.0, 1.0 };

		model.nodes[2].mesh = 0;
		model.nodes[2].translation = { 100.0, 0.0, 0.0 };
		model.nodes[2].matrix = {
			1.0, 0.0, 0.0, 0.0,
			0.0, 1.0, 0.0, 0.0,
			0.0, 0.0, 1.0, 0.0,
			-4.0, 0.0, 0.0, 1.0
		};

		model.nodes[3].mesh = 0;
		model.nodes[3].translation = { 1000.0, 0.0, 0.0 };

		tinygltf::Scene scene;
		scene.nodes = { 0, 2 };
		model.scenes.push_back(std::move(scene));
		model.defaultScene = 0;

		TVector<GltfImporterUtils::MeshInstance> instances;
		Require(
			GltfImporterUtils::CollectMeshInstances(model, instances),
			"valid active-scene hierarchy must be traversed");
		Require(instances.Num() == 2,
			"one mesh resource referenced by two active nodes must emit two instances");
		Require(instances[0].m_nodeIndex == 1 &&
			instances[0].m_meshIndex == 0 &&
			instances[0].m_skinIndex == -1,
			"mesh instance must retain its source node, mesh, and skin indices");
		Require(instances[1].m_nodeIndex == 2 &&
			instances[1].m_meshIndex == 0,
			"matrix-authored mesh node must be emitted in active-scene order");

		const glm::mat4 unitScale =
			glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
		const glm::mat4 firstGeometryTransform =
			unitScale * instances[0].m_worldTransform;
		RequireVec3Near(
			glm::vec3(firstGeometryTransform *
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
			glm::vec3(20.0f, 4.0f, 0.0f),
			"parent and child translations must accumulate and receive unit scale");
		RequireVec3Near(
			glm::vec3(firstGeometryTransform *
				glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)),
			glm::vec3(20.0f, 8.0f, 0.0f),
			"node TRS must use glTF translation-rotation-scale order");
		RequireVec3Near(
			glm::vec3(firstGeometryTransform *
				glm::vec4(0.0f, 1.0f, 0.0f, 1.0f)),
			glm::vec3(18.0f, 4.0f, 0.0f),
			"non-uniform node scale and rotation must compose correctly");

		const glm::mat4 secondGeometryTransform =
			unitScale * instances[1].m_worldTransform;
		RequireVec3Near(
			glm::vec3(secondGeometryTransform *
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
			glm::vec3(-8.0f, 0.0f, 0.0f),
			"node matrix must take precedence over TRS properties");
	}

	void TestUnitScaleDoesNotShrinkImportedDirections()
	{
		GltfImporterUtils::MeshInstance instance;
		glm::mat4 quarterTurn(1.0f);
		quarterTurn[0] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
		quarterTurn[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
		instance.m_worldTransform =
			quarterTurn *
			glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.5f));

		const auto transforms =
			GltfImporterUtils::ResolveMeshInstanceTransforms(instance, 10000.0f);
		RequireVec3Near(
			transforms.m_bakedVolumeScale,
			glm::vec3(2.0f, 1.0f, 0.5f),
			"baked glTF node scale must survive vertex flattening without inheriting unit scale");
		RequireVec3Near(
			glm::vec3(transforms.m_geometryTransform * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)),
			glm::vec3(0.0f, 20000.0f, 0.0f),
			"unit scale must still affect imported positions");

		const glm::mat3 normalTransform =
			glm::transpose(glm::inverse(transforms.m_directionTransform));
		const glm::vec3 importedNormal = normalTransform * glm::vec3(0.0f, 0.0f, 1.0f);
		Require(
			glm::length(importedNormal) > 1.0f,
			"large unit scale must not shrink imported normals below the sanitizer threshold");
		RequireVec3Near(
			glm::normalize(importedNormal),
			glm::vec3(0.0f, 0.0f, 1.0f),
			"node transforms must still be applied to imported normals");

		const auto mirroredTransforms =
			GltfImporterUtils::ResolveMeshInstanceTransforms(instance, -10000.0f);
		Require(
			glm::determinant(mirroredTransforms.m_directionTransform) < 0.0f,
			"negative unit scale must retain mirrored winding without scaling directions");
		RequireVec3Near(
			mirroredTransforms.m_bakedVolumeScale,
			transforms.m_bakedVolumeScale,
			"model unit-scale sign must not be applied twice to volume thickness");

		instance.m_skinIndex = 0;
		const auto skinnedTransforms =
			GltfImporterUtils::ResolveMeshInstanceTransforms(instance, 10000.0f);
		RequireVec3Near(
			skinnedTransforms.m_bakedVolumeScale,
			glm::vec3(1.0f),
			"skinned meshes must not retain a node transform that is not baked into their vertices");
	}

	void TestCollectMeshInstancesRejectsCyclesAndPreservesLegacyMeshes()
	{
		tinygltf::Model cyclicModel;
		cyclicModel.meshes.resize(1);
		cyclicModel.nodes.resize(2);
		cyclicModel.nodes[0].mesh = 0;
		cyclicModel.nodes[0].children = { 1 };
		cyclicModel.nodes[1].children = { 0 };
		tinygltf::Scene cyclicScene;
		cyclicScene.nodes = { 0 };
		cyclicModel.scenes.push_back(std::move(cyclicScene));
		cyclicModel.defaultScene = 0;

		TVector<GltfImporterUtils::MeshInstance> instances;
		Require(
			!GltfImporterUtils::CollectMeshInstances(cyclicModel, instances),
			"cyclic glTF node hierarchies must be rejected");
		Require(instances.IsEmpty(),
			"failed traversal must not retain partial mesh instances");

		tinygltf::Model legacyModel;
		legacyModel.meshes.resize(2);
		Require(
			GltfImporterUtils::CollectMeshInstances(legacyModel, instances),
			"mesh-only glTF assets must retain legacy import support");
		Require(instances.Num() == 2 &&
			instances[0].m_meshIndex == 0 &&
			instances[1].m_meshIndex == 1,
			"mesh-only assets must emit one identity instance per mesh resource");
	}

	void TestCollectMeshInstancesHandlesDeepHierarchyIteratively()
	{
		constexpr size_t NumNodes = 8192;
		tinygltf::Model model;
		model.meshes.resize(1);
		model.nodes.resize(NumNodes);
		for (size_t nodeIndex = 0; nodeIndex + 1 < NumNodes; ++nodeIndex)
		{
			model.nodes[nodeIndex].children = {
				static_cast<int32_t>(nodeIndex + 1)
			};
		}
		model.nodes[NumNodes - 1].mesh = 0;
		tinygltf::Scene scene;
		scene.nodes = { 0 };
		model.scenes.push_back(std::move(scene));
		model.defaultScene = 0;

		TVector<GltfImporterUtils::MeshInstance> instances;
		Require(
			GltfImporterUtils::CollectMeshInstances(model, instances),
			"deep valid glTF hierarchies must not overflow the native stack");
		Require(instances.Num() == 1 &&
			instances[0].m_nodeIndex == static_cast<int32_t>(NumNodes - 1),
			"deep hierarchy traversal must reach the leaf mesh node");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "ModelReadinessTracksMeshUploads", TestModelReadinessTracksMeshUploads },
		{ "MeshContextRejectsEmptyGpuUploads", TestMeshContextRejectsEmptyGpuUploads },
		{ "GltfAlphaModesResolveRenderState", TestGltfAlphaModesResolveRenderState },
		{ "MaterialAssetRetainsRenderQueue", TestMaterialAssetRetainsRenderQueue },
		{ "GltfTransmissionExtensionResolvesMaterialFields", TestGltfTransmissionExtensionResolvesMaterialFields },
		{ "GeneratedMaterialMigrationPreservesAuthoredProperties", TestGeneratedMaterialMigrationPreservesAuthoredProperties },
		{ "GeneratedMaterialMigrationRecognizesLegacyOwnership", TestGeneratedMaterialMigrationRecognizesLegacyOwnership },
		{ "GltfMaterialTextureColorSpaces", TestGltfMaterialTextureColorSpaces },
		{ "CompactedMeshesRetainMaterialSlots", TestCompactedMeshesRetainMaterialSlots },
		{ "GeneratedTangentsPreserveMirroredUvHandedness", TestGeneratedTangentsPreserveMirroredUvHandedness },
		{ "PathTracerTransformsShadingBasisCorrectly", TestPathTracerTransformsShadingBasisCorrectly },
		{ "BuildBlasRejectsOutOfRangeIndicesAtomically", TestBuildBlasRejectsOutOfRangeIndicesAtomically },
		{ "BuildBlasRecoversAfterGeometryIsCorrected", TestBuildBlasRecoversAfterGeometryIsCorrected },
		{ "BuildBlasIgnoresEmptyMeshes", TestBuildBlasIgnoresEmptyMeshes },
		{ "BuildBlasRejectsIncompleteAndNonFiniteGeometry", TestBuildBlasRejectsIncompleteAndNonFiniteGeometry },
		{ "BuildBlasSanitizesExtremeVertexFrames", TestBuildBlasSanitizesExtremeVertexFrames },
		{ "BuildBlasHandlesExtremeCentroidRange", TestBuildBlasHandlesExtremeCentroidRange },
		{ "CollectMeshInstancesUsesActiveSceneHierarchy", TestCollectMeshInstancesUsesActiveSceneHierarchy },
		{ "UnitScaleDoesNotShrinkImportedDirections", TestUnitScaleDoesNotShrinkImportedDirections },
		{ "CollectMeshInstancesRejectsCyclesAndPreservesLegacyMeshes", TestCollectMeshInstancesRejectsCyclesAndPreservesLegacyMeshes },
		{ "CollectMeshInstancesHandlesDeepHierarchyIteratively", TestCollectMeshInstancesHandlesDeepHierarchyIteratively }
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": "
				<< error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
