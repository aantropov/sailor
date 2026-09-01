#include "PathTracer.h"
#include "Tasks/Scheduler.h"
#include "Core/LogMacros.h"
#include "Core/Utils.h"
#include "Math/Math.h"
#include "Math/Bounds.h"
#include "Core/StringHash.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Model/ModelAssetInfo.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"
#include "RHI/Texture.h"
#include <glm/gtc/random.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <thread>

#include <stb_image.h>
#include <tiny_gltf.h>

#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define __STDC_LIB_EXT1__
#include <stb_image_write.h>
#endif 

//#include "assimp/scene.h"
//#include "assimp/postprocess.h"
//#include "assimp/Importer.hpp"
//#include "assimp/DefaultLogger.hpp"
//#include "assimp/LogStream.hpp"

using namespace Sailor;
using namespace Sailor::Math;
using namespace Sailor::Raytracing;

namespace
{
	constexpr uint32_t DefaultGroupSize = 32;
	constexpr float DefaultAspectRatio = 4.0f / 3.0f;
	constexpr float DefaultHfov = glm::radians(60.0f);

	__forceinline uint32_t XorShift32(uint32_t& state)
	{
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	}

	__forceinline uint32_t NextRandomU32()
	{
		thread_local uint32_t s_rngState = 0u;
		if (s_rngState == 0u)
		{
			const uint64_t tidHash = (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
			s_rngState = (uint32_t)((tidHash ^ (tidHash >> 32)) | 1u);
		}

		return XorShift32(s_rngState);
	}

	__forceinline uint32_t NextRandomU32(uint32_t& randomState)
	{
		if (randomState == 0u)
		{
			randomState = 0x6d2b79f5u;
		}
		return XorShift32(randomState);
	}

	__forceinline uint32_t NextRandomRange(
		uint32_t& randomState,
		uint32_t maxExclusive)
	{
		return maxExclusive > 0u ?
			(NextRandomU32(randomState) % maxExclusive) : 0u;
	}

	__forceinline float NextRandom01(uint32_t& randomState)
	{
		return static_cast<float>(
			NextRandomU32(randomState) & 0x00ffffffu) / 16777216.0f;
	}

	__forceinline vec2 NextRandomVec2_01(uint32_t& randomState)
	{
		return vec2(
			NextRandom01(randomState),
			NextRandom01(randomState));
	}

	__forceinline vec3 OffsetRayOrigin(
		const vec3& worldPoint,
		const vec3& geometricNormal,
		const vec3& rayDirection,
		float normalBias,
		float viewBias)
	{
		const float normalSign =
			glm::dot(rayDirection, geometricNormal) >= 0.0f ? 1.0f : -1.0f;
		return worldPoint +
			geometricNormal * normalSign * (std::max)(normalBias, 0.0f) +
			rayDirection * (std::max)(viewBias, 0.0f);
	}

	__forceinline vec3 SanitizeRadiance(const vec3& value)
	{
		vec3 result{};
		for (uint32_t channel = 0u; channel < 3u; ++channel)
		{
			result[channel] = std::isfinite(value[channel]) ?
				(std::max)(value[channel], 0.0f) : 0.0f;
		}
		return result;
	}

	__forceinline vec3 ResolveWorldShadingNormal(
		const mat3& tangentBasis,
		const vec3& tangentSpaceNormal,
		const vec3& fallbackNormal,
		const vec3& orientedGeometricNormal)
	{
		vec3 worldNormal = Math::SafeNormalize(
			tangentBasis * tangentSpaceNormal,
			fallbackNormal);
		const float geometricCosine = dot(
			worldNormal,
			orientedGeometricNormal);
		return Math::SafeNormalize(
			worldNormal - 2.0f * (std::min)(geometricCosine, 0.0f) *
				orientedGeometricNormal,
			orientedGeometricNormal);
	}

	bool IsMaterialFaceVisible(
		const Raytracing::Material& material,
		bool bFrontFace)
	{
		switch (material.m_faceCullMode)
		{
		case Raytracing::FaceCullMode::None:
			return true;
		case Raytracing::FaceCullMode::Front:
			return !bFrontFace;
		case Raytracing::FaceCullMode::Back:
			return bFrontFace;
		case Raytracing::FaceCullMode::FrontAndBack:
			return false;
		}

		return false;
	}

	const Raytracing::BVH* ResolveInstanceBlas(
		const PathTracer::TLASInstance& instance)
	{
		if (instance.m_blas)
		{
			return instance.m_blas.GetRawPtr();
		}
		if (!instance.m_model ||
			!instance.m_model->HasBLAS(instance.m_meshIndex))
		{
			return nullptr;
		}
		return instance.m_model->GetBLAS(instance.m_meshIndex).GetRawPtr();
	}

	const TVector<Math::Triangle>* ResolveInstanceTriangles(
		const PathTracer::TLASInstance& instance)
	{
		if (instance.m_triangles && !instance.m_triangles->IsEmpty())
		{
			return instance.m_triangles.GetRawPtr();
		}
		if (!instance.m_model ||
			!instance.m_model->HasBLAS(instance.m_meshIndex))
		{
			return nullptr;
		}
		const auto& triangles =
			instance.m_model->GetBLASTriangles(instance.m_meshIndex);
		return triangles.IsEmpty() ? nullptr : &triangles;
	}

	bool HasInstanceGeometry(const PathTracer::TLASInstance& instance)
	{
		return ResolveInstanceBlas(instance) != nullptr &&
			ResolveInstanceTriangles(instance) != nullptr &&
			instance.m_worldBounds.IsValid();
	}

	__forceinline vec2 ResolveHitTextureCoordinates(
		const Math::Triangle& triangle,
		const Math::RaycastHit& hit)
	{
		return hit.m_barycentricCoordinate.x * triangle.m_uvs[0] +
			hit.m_barycentricCoordinate.y * triangle.m_uvs[1] +
			hit.m_barycentricCoordinate.z * triangle.m_uvs[2];
	}

	__forceinline vec3 CalculateVolumeAttenuation(
		const vec3& attenuationColor,
		float attenuationDistance,
		float transmissionDistance)
	{
		if (transmissionDistance <= 0.0f ||
			attenuationDistance <= 0.0f ||
			!std::isfinite(attenuationDistance) ||
			attenuationDistance >= (std::numeric_limits<float>::max)())
		{
			return vec3(1.0f);
		}

		const float exponent = transmissionDistance /
			(std::max)(attenuationDistance, std::numeric_limits<float>::epsilon());
		vec3 result(1.0f);
		for (uint32_t component = 0; component < 3; ++component)
		{
			const float sourceColor = attenuationColor[component];
			const float color = std::isfinite(sourceColor) ?
				glm::clamp(sourceColor, 0.0f, 1.0f) :
				1.0f;
			result[component] = color <= 0.0f ?
				0.0f :
				std::pow(color, exponent);
		}

		return result;
	}

	struct PathTracerView
	{
		vec3 m_cameraPos = vec3(0, 0.75f, 5.0f);
		vec3 m_cameraForward = vec3(0, 0, -1.0f);
		vec3 m_cameraUp = vec3(0, 1.0f, 0);
		float m_aspectRatio = DefaultAspectRatio;
		float m_hFov = DefaultHfov;
	};

	void ResolveViewAndDirectionalLights(const tinygltf::Model& gltfModel, const PathTracer::Params& params, const Math::Sphere& sceneBounds, PathTracerView& outView, TVector<DirectionalLight>& outLights)
	{
		const vec3 sceneCenter = sceneBounds.m_center;
		const float sceneRadius = std::max(sceneBounds.m_radius, 0.1f);

		outView.m_cameraPos = sceneCenter + vec3(0.0f, sceneRadius * 0.6f, sceneRadius * 2.5f);
		outView.m_cameraForward = glm::normalize(sceneCenter - outView.m_cameraPos);
		outView.m_cameraUp = vec3(0.0f, 1.0f, 0.0f);
		outView.m_aspectRatio = DefaultAspectRatio;
		outView.m_hFov = DefaultHfov;

		const int32_t defaultSceneIndex = gltfModel.defaultScene >= 0 ? gltfModel.defaultScene : 0;
		if (gltfModel.scenes.empty() || defaultSceneIndex < 0 || defaultSceneIndex >= (int32_t)gltfModel.scenes.size())
		{
			return;
		}

		const tinygltf::Scene& scene = gltfModel.scenes[defaultSceneIndex];
		bool bFoundNamedCamera = false;
		bool bFallbackCameraSet = false;

		std::function<void(int32_t, const glm::mat4&)> traverseNode = [&](int32_t nodeIndex, const glm::mat4& parentMatrix)
		{
			if (nodeIndex < 0 || nodeIndex >= (int32_t)gltfModel.nodes.size())
			{
				return;
			}

			const tinygltf::Node& node = gltfModel.nodes[nodeIndex];
			glm::mat4 localTransform(1.0f);
			if (!GltfImporterUtils::TryComposeNodeMatrix(
					node,
					localTransform))
			{
				return;
			}
			const glm::mat4 world = parentMatrix * localTransform;

			if (node.camera >= 0 && node.camera < (int32_t)gltfModel.cameras.size())
			{
				const tinygltf::Camera& camera = gltfModel.cameras[node.camera];
				const bool bNameMatch = !params.m_camera.empty() && ((camera.name == params.m_camera) || (node.name == params.m_camera));
				const bool bUseCamera = bNameMatch || (params.m_camera.empty() && !bFallbackCameraSet);

				if (bUseCamera && camera.type == "perspective")
				{
					outView.m_cameraPos = vec3(world[3]);
					outView.m_cameraForward = glm::normalize(vec3(world * vec4(0, 0, -1, 0)));
					outView.m_cameraUp = glm::normalize(vec3(world * vec4(0, 1, 0, 0)));

					const float aspect = camera.perspective.aspectRatio > 0.0 ? (float)camera.perspective.aspectRatio : DefaultAspectRatio;
					const float yFov = camera.perspective.yfov > 0.0 ? (float)camera.perspective.yfov : glm::radians(45.0f);

					outView.m_aspectRatio = aspect;
					outView.m_hFov = 2.0f * atan(tan(yFov * 0.5f) * aspect);

					bFallbackCameraSet = true;
					if (bNameMatch)
					{
						bFoundNamedCamera = true;
					}
				}
			}

			auto extIt = node.extensions.find("KHR_lights_punctual");
			if (extIt != node.extensions.end() && extIt->second.IsObject())
			{
				const tinygltf::Value& lightNode = extIt->second;
				if (lightNode.Has("light"))
				{
					const int32_t lightIndex = lightNode.Get("light").Get<int>();
					if (lightIndex >= 0 && lightIndex < (int32_t)gltfModel.lights.size())
					{
						const auto& light = gltfModel.lights[lightIndex];
						if (light.type == "directional")
						{
							DirectionalLight dir{};
							dir.m_direction = glm::normalize(vec3(world * vec4(0, 0, -1, 0)));
							// KHR_lights_punctual defines directional intensity in lux.
							dir.m_intensity = vec3(
								static_cast<float>(light.color[0]),
								static_cast<float>(light.color[1]),
								static_cast<float>(light.color[2])) *
								static_cast<float>(light.intensity);
							outLights.Add(dir);
						}
					}
				}
			}

			if (bFoundNamedCamera)
			{
				return;
			}

			for (int32_t child : node.children)
			{
				traverseNode(child, world);
			}
		};

		for (int32_t root : scene.nodes)
		{
			traverseNode(root, glm::mat4(1.0f));
			if (bFoundNamedCamera)
			{
				break;
			}
		}
	}

	LightProxy MakeDirectionalLightProxy(const DirectionalLight& directional)
	{
		LightProxy lightProxy{};
		lightProxy.m_type = ELightType::Directional;
		lightProxy.m_direction = glm::normalize(directional.m_direction);
		lightProxy.m_intensity = directional.m_intensity;
		return lightProxy;
	}

	float CalculateLocalLightRangeAttenuation(
		const LightProxy& light,
		float distanceToLight)
	{
		// Keep this in sync with CalculateLocalLightRangeAttenuation in
		// Content/Shaders/Lighting.glsl.
		const float safeRadius = std::max(light.m_bounds.x, 0.00001f);
		const float normalizedDistance = glm::clamp(
			distanceToLight / safeRadius,
			0.0f,
			1.0f);
		constexpr float MinimumDistance = 0.01f;
		const float safeDistance = std::max(
			distanceToLight,
			MinimumDistance);
		const float inverseSquareFalloff =
			1.0f / (safeDistance * safeDistance);
		const float normalizedDistanceSquared =
			normalizedDistance * normalizedDistance;
		const float rangeBase = glm::clamp(
			1.0f - normalizedDistanceSquared * normalizedDistanceSquared,
			0.0f,
			1.0f);
		const float rangeWindow = rangeBase * rangeBase;
		return inverseSquareFalloff * rangeWindow;
	}

	bool EvaluateDirectLight(const LightProxy& light,
		const glm::vec3& worldPoint,
		glm::vec3& outDirectionToLight,
		glm::vec3& outRadiance,
		float& outMaxDistance)
	{
		outDirectionToLight = glm::vec3(0.0f);
		outRadiance = glm::vec3(0.0f);
		outMaxDistance = std::numeric_limits<float>::max();

		if (light.m_type == ELightType::Directional)
		{
			outDirectionToLight = -glm::normalize(light.m_direction);
			outRadiance = light.m_intensity;
			return glm::dot(outDirectionToLight, outDirectionToLight) > 0.0f;
		}

		const glm::vec3 toLight = light.m_worldPosition - worldPoint;
		const float distance = glm::length(toLight);
		if (distance <= 1e-4f)
		{
			return false;
		}

		const glm::vec3 directionToLight = toLight / distance;
		if (light.m_bounds.x > 0.0f && distance > light.m_bounds.x)
		{
			return false;
		}

		if (light.m_type == ELightType::Spot)
		{
			const float cosTheta = glm::dot(glm::normalize(light.m_direction), -directionToLight);
			const float outerCos = light.m_cutOff.y;
			const float innerCos = light.m_cutOff.x;

			if (cosTheta < outerCos)
			{
				return false;
			}

			const float coneBase = innerCos > outerCos ?
				glm::clamp((cosTheta - outerCos) / (innerCos - outerCos), 0.0f, 1.0f) :
				1.0f;
			const float cone = coneBase * coneBase;
			outRadiance = light.m_intensity * cone;
		}
		else
		{
			outRadiance = light.m_intensity;
		}

		const float attenuation = CalculateLocalLightRangeAttenuation(
			light,
			distance);
		if (attenuation <= 0.0f)
		{
			return false;
		}

		outDirectionToLight = directionToLight;
		outRadiance *= attenuation;
		outMaxDistance = distance;
		return true;
	}

	template<typename TUniforms, typename TValue>
	bool ReadUniformValue(const TUniforms& uniforms, const std::string& name, TValue& outValue)
	{
		const TValue* pValue = nullptr;
		if (uniforms.Find(name, pValue) && pValue != nullptr)
		{
			outValue = *pValue;
			return true;
		}

		return false;
	}

	bool BuildRaytracingMaterialsFromRuntimeMaterials(
		const TVector<MaterialPtr>& runtimeMaterials,
		TVector<Raytracing::Material>& outMaterials,
		TVector<uint8_t>& outResolvedMaterialSlots,
		TVector<TSharedPtr<CombinedSampler2D>>& outTextures,
		TMap<std::string, uint32_t>& outTextureMapping,
		const PathTracer::ScenePreparationProgressCallback& progress,
		const PathTracer::ScenePreparationWarningCallback& warning,
		PathTracer::ScenePreparationStats& stats,
		bool& outAllTexturesResolved)
	{
		struct CpuTextureSnapshot final
		{
			TVector<uint8_t> m_data{};
			int32_t m_width = 0;
			int32_t m_height = 0;
		};

		outAllTexturesResolved = true;

		outMaterials.Resize(runtimeMaterials.Num());
		outResolvedMaterialSlots.Resize(runtimeMaterials.Num());
		outTextures.Clear();
		outTextureMapping.Clear();
		stats.m_materialSlotCount = runtimeMaterials.Num();
		TMap<const Sailor::Material*, Raytracing::Material>
			convertedMaterials;
		TMap<const Sailor::Material*, uint8_t> convertedMaterialResolution;
		TMap<std::string, TSharedPtr<CpuTextureSnapshot>> cpuTextureSnapshots;

		auto reportMaterialProgress = [&](size_t completed) -> bool
		{
			if (!progress)
			{
				return true;
			}
			PathTracer::ScenePreparationProgress update;
			update.m_stage = PathTracer::EScenePreparationStage::Materials;
			update.m_completed = completed;
			update.m_total = runtimeMaterials.Num();
			return progress(update);
		};
		if (!reportMaterialProgress(0u))
		{
			return false;
		}

		auto addTexture = [&](const TexturePtr& pTexture,
			bool bLinear,
			bool bNormalMap,
			uint8_t channels,
			uint16_t& outTextureIndex,
			std::string& outDiagnostic) -> bool
		{
			outDiagnostic.clear();
			++stats.m_textureReferenceCount;
			if (!pTexture)
			{
				outDiagnostic = "the texture reference is null";
				return false;
			}

			TexturePtr texture = pTexture;
			const FileId fileId = pTexture->GetFileId();
			if (fileId)
			{
				if (auto* textureImporter = App::GetSubmodule<TextureImporter>())
				{
					TexturePtr loadedTexture =
						textureImporter->GetLoadedTexture(fileId);
					if (loadedTexture &&
						(loadedTexture->HasCpuData() || !texture->HasCpuData()))
					{
						texture = loadedTexture;
					}
				}
			}

			const RHI::ETextureClamping clamping = texture->GetRHI() ? texture->GetRHI()->GetClamping() : RHI::ETextureClamping::Repeat;
			const char* clampingKey = clamping == RHI::ETextureClamping::Repeat ? "r" : "c";
			const std::string sourceKey = fileId ?
				fileId.ToString() :
				"runtime:" + std::to_string(
					reinterpret_cast<uintptr_t>(texture.GetRawPtr()));
			const std::string key = sourceKey + "_" +
				std::to_string(channels) + "_" +
				std::to_string((int)bLinear) + "_" +
				std::to_string((int)bNormalMap) + "_" + clampingKey;

			if (outTextureMapping.ContainsKey(key))
			{
				outTextureIndex = static_cast<uint16_t>(outTextureMapping[key]);
				return true;
			}

			TSharedPtr<CpuTextureSnapshot>* cachedSnapshot = nullptr;
			if (!cpuTextureSnapshots.Find(sourceKey, cachedSnapshot))
			{
				auto snapshot = TSharedPtr<CpuTextureSnapshot>::Make();
				snapshot->m_width = texture->GetWidth();
				snapshot->m_height = texture->GetHeight();
				if (texture->HasCpuData())
				{
					snapshot->m_data = texture->GetDecodedData();
				}
				else
				{
					uint32_t mipLevels = 1u;
					if (!fileId || !TextureImporter::DecodeTextureCpu(
							fileId,
							snapshot->m_data,
							snapshot->m_width,
							snapshot->m_height,
							mipLevels))
					{
						outDiagnostic = fileId ?
							"the texture could not be decoded on the CPU" :
							"the runtime texture has no resident CPU pixels or file id";
						return false;
					}
					++stats.m_decodedTextureCount;
				}
				if (snapshot->m_width <= 0 || snapshot->m_height <= 0 ||
					snapshot->m_data.IsEmpty())
				{
					outDiagnostic =
						"the decoded texture has invalid dimensions or no pixel data";
					return false;
				}
				cpuTextureSnapshots.Add(sourceKey, snapshot);
				++stats.m_uniqueTextureCount;
				cpuTextureSnapshots.Find(sourceKey, cachedSnapshot);
			}
			if (!cachedSnapshot || !*cachedSnapshot)
			{
				outDiagnostic = "the CPU texture snapshot is unavailable";
				return false;
			}
			const auto& snapshot = **cachedSnapshot;
			const TVector<uint8_t>* sourceData = &snapshot.m_data;
			const int32_t width = snapshot.m_width;
			const int32_t height = snapshot.m_height;

			if (outTextures.Num() >= static_cast<size_t>(
				Raytracing::Material::InvalidTextureIndex))
			{
				outDiagnostic =
					"the CPU path tracer texture-table limit was exceeded";
				return false;
			}

			TSharedPtr<CombinedSampler2D> sampler = TSharedPtr<CombinedSampler2D>::Make();
			sampler->m_width = width;
			sampler->m_height = height;
			sampler->m_channels = channels;
			sampler->m_clamping = clamping == RHI::ETextureClamping::Repeat ? SamplerClamping::Repeat : SamplerClamping::Clamp;

			const size_t pixelCount = static_cast<size_t>(width) * height;
			const bool bIsFloatTexture = sourceData->Num() ==
				pixelCount * sizeof(glm::vec4);
			if (bIsFloatTexture)
			{
				if (channels == 4)
				{
					sampler->Initialize<vec4, vec4>(
						reinterpret_cast<const vec4*>(sourceData->GetData()),
						bLinear,
						bNormalMap);
				}
				else
				{
					sampler->Initialize<vec3, vec4>(
						reinterpret_cast<const vec4*>(sourceData->GetData()),
						bLinear,
						bNormalMap);
				}
			}
			else
			{
				if (sourceData->Num() != pixelCount * sizeof(glm::u8vec4))
				{
					outDiagnostic =
						"the decoded texture pixel format or byte count is unsupported";
					return false;
				}
				if (channels == 4)
				{
					sampler->Initialize<vec4, u8vec4>(
						reinterpret_cast<const u8vec4*>(sourceData->GetData()),
						bLinear,
						bNormalMap);
				}
				else
				{
					sampler->Initialize<vec3, u8vec4>(
						reinterpret_cast<const u8vec4*>(sourceData->GetData()),
						bLinear,
						bNormalMap);
				}
			}

			const uint32_t index = (uint32_t)outTextures.Num();
			outTextures.Add(sampler);
			outTextureMapping[key] = index;
			outTextureIndex = static_cast<uint16_t>(index);
			return true;
		};

		for (size_t i = 0; i < runtimeMaterials.Num(); i++)
		{
			Raytracing::Material outMaterial{};
			const MaterialPtr pMaterial = runtimeMaterials[i];
			auto reportCompletedMaterial = [&]() -> bool
			{
				const size_t completed = i + 1u;
				return completed % 64u != 0u &&
					completed != runtimeMaterials.Num() ?
					true : reportMaterialProgress(completed);
			};

			if (!pMaterial)
			{
				outMaterials[i] = outMaterial;
				outResolvedMaterialSlots[i] = 1u;
				if (!reportCompletedMaterial())
				{
					return false;
				}
				continue;
			}
			Raytracing::Material* cachedMaterial = nullptr;
			if (convertedMaterials.Find(pMaterial.GetRawPtr(), cachedMaterial))
			{
				outMaterials[i] = *cachedMaterial;
				uint8_t* cachedResolution = nullptr;
				convertedMaterialResolution.Find(
					pMaterial.GetRawPtr(),
					cachedResolution);
				outResolvedMaterialSlots[i] = cachedResolution ?
					*cachedResolution : 0u;
				outAllTexturesResolved &=
					outResolvedMaterialSlots[i] != 0u;
				++stats.m_reusedMaterialCount;
				if (!reportCompletedMaterial())
				{
					return false;
				}
				continue;
			}

			glm::vec4 baseColorFactor(1.0f);
			glm::vec4 emissiveFactor(0.0f);
			glm::vec4 attenuationColor(1.0f);
			glm::vec4 sheenColor(0.0f);
			glm::vec4 layerUvScale(1.0f);
			float roughness = 1.0f;
			float metallic = 1.0f;
			float alphaCutoff = 0.5f;
			float normalScale = 1.0f;
			float clearcoat = 0.0f;
			float clearcoatRoughness = 0.0f;
			float clearcoatNormalScale = 1.0f;
			float sheenRoughness = 0.0f;
			float transmission = 0.0f;
			float thickness = 0.0f;
			float attenuationDistance = (std::numeric_limits<float>::max)();
			float indexOfRefraction = 1.5f;

			ReadUniformValue(pMaterial->GetUniformsVec4(), "material.baseColorFactor", baseColorFactor);
			ReadUniformValue(pMaterial->GetUniformsVec4(), "material.albedo", baseColorFactor);
			ReadUniformValue(pMaterial->GetUniformsVec4(), "material.emissiveFactor", emissiveFactor);
			ReadUniformValue(pMaterial->GetUniformsVec4(), "material.emissive", emissiveFactor);
			ReadUniformValue(pMaterial->GetUniformsVec4(), "material.emission", emissiveFactor);
			ReadUniformValue(pMaterial->GetUniformsVec4(), "material.attenuationColor", attenuationColor);
			ReadUniformValue(pMaterial->GetUniformsVec4(), "material.sheenColorFactor", sheenColor);
			ReadUniformValue(pMaterial->GetUniformsVec4(), "material.layerUvScale", layerUvScale);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.roughnessFactor", roughness);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.roughness", roughness);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.metallicFactor", metallic);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.metallic", metallic);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.alphaCutoff", alphaCutoff);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.normalScale", normalScale);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.clearcoatFactor", clearcoat);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.clearcoatRoughnessFactor", clearcoatRoughness);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.clearcoatNormalScale", clearcoatNormalScale);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.sheenRoughnessFactor", sheenRoughness);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.transmissionFactor", transmission);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.thicknessFactor", thickness);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.attenuationDistance", attenuationDistance);
			ReadUniformValue(pMaterial->GetUniformsFloat(), "material.indexOfRefraction", indexOfRefraction);

			outMaterial.m_baseColorFactor = baseColorFactor;
			outMaterial.m_emissiveFactor = glm::vec3(emissiveFactor);
			for (glm::length_t layer = 0; layer < 4; ++layer)
			{
				outMaterial.m_layerUvScale[layer] =
					std::isfinite(layerUvScale[layer]) ? layerUvScale[layer] : 1.0f;
			}
			outMaterial.m_roughnessFactor = roughness;
			outMaterial.m_metallicFactor = metallic;
			outMaterial.m_normalScale = std::isfinite(normalScale) ?
				normalScale : 1.0f;
			outMaterial.m_clearcoatFactor = std::isfinite(clearcoat) ?
				glm::clamp(clearcoat, 0.0f, 1.0f) : 0.0f;
			outMaterial.m_clearcoatRoughnessFactor =
				std::isfinite(clearcoatRoughness) ?
				glm::clamp(clearcoatRoughness, 0.0f, 1.0f) : 0.0f;
			outMaterial.m_clearcoatNormalScale =
				std::isfinite(clearcoatNormalScale) ?
				clearcoatNormalScale : 1.0f;
			outMaterial.m_sheenRoughnessFactor =
				std::isfinite(sheenRoughness) ?
				glm::clamp(sheenRoughness, 0.0f, 1.0f) : 0.0f;
			outMaterial.m_sheenColorFactor = glm::clamp(
				glm::vec3(sheenColor),
				glm::vec3(0.0f),
				glm::vec3(1.0f));
			outMaterial.m_alphaCutoff = alphaCutoff;
			outMaterial.m_transmissionFactor = std::isfinite(transmission) ?
				glm::clamp(transmission, 0.0f, 1.0f) :
				0.0f;
			outMaterial.m_thicknessFactor = std::isfinite(thickness) ?
				(std::max)(0.0f, thickness) :
				0.0f;
			outMaterial.m_attenuationDistance =
				std::isfinite(attenuationDistance) && attenuationDistance > 0.0f ?
				attenuationDistance :
				(std::numeric_limits<float>::max)();
			for (uint32_t component = 0; component < 3; ++component)
			{
				const float value = attenuationColor[component];
				outMaterial.m_attenuationColor[component] =
					std::isfinite(value) ?
					glm::clamp(value, 0.0f, 1.0f) :
					1.0f;
			}
			outMaterial.m_indexOfRefraction = std::isfinite(indexOfRefraction) &&
				indexOfRefraction >= 1.0f ?
				indexOfRefraction :
				1.5f;

			const RHI::RenderState& renderState = pMaterial->GetRenderState();
			switch (renderState.GetCullMode())
			{
			case RHI::ECullMode::None:
				outMaterial.m_faceCullMode = FaceCullMode::None;
				break;
			case RHI::ECullMode::Front:
				outMaterial.m_faceCullMode = FaceCullMode::Front;
				break;
			case RHI::ECullMode::Back:
				outMaterial.m_faceCullMode = FaceCullMode::Back;
				break;
			case RHI::ECullMode::FrontAndBack:
				outMaterial.m_faceCullMode = FaceCullMode::FrontAndBack;
				break;
			}
			if (renderState.GetBlendMode() != RHI::EBlendMode::None)
			{
				outMaterial.m_blendMode = BlendMode::Blend;
			}
			else if (renderState.IsRequiredCustomDepthShader())
			{
				outMaterial.m_blendMode = BlendMode::Mask;
			}
			else
			{
				outMaterial.m_blendMode = BlendMode::Opaque;
			}

			bool bMaterialResolved = true;
			const std::string materialFileId = pMaterial->GetFileId().ToString();
			const std::string materialName = materialFileId.empty() ?
				"runtime material slot " + std::to_string(i) :
				"material '" + materialFileId + "'";
			auto prepareTexture = [&](const std::string& samplerName,
				const TexturePtr& texture,
				bool bLinear,
				bool bNormalMap,
				uint8_t channels,
				uint16_t& outTextureIndex)
			{
				std::string diagnostic;
				if (addTexture(
						texture,
						bLinear,
						bNormalMap,
						channels,
						outTextureIndex,
						diagnostic))
				{
					return;
				}
				bMaterialResolved = false;
				if (warning)
				{
					const std::string textureFileId = texture ?
						texture->GetFileId().ToString() : std::string();
					warning(
						"could not prepare " + materialName +
						" sampler '" + samplerName + "'" +
						(textureFileId.empty() ? std::string() :
							" texture '" + textureFileId + "'") +
						": " + diagnostic);
				}
			};

			for (const auto& sampler : pMaterial->GetSamplers())
			{
				const std::string& samplerName = sampler.m_first;
				const TexturePtr pTexture = sampler.m_second;

				if (samplerName == "baseColorSampler")
				{
					prepareTexture(samplerName, pTexture, true, false, 4, outMaterial.m_baseColorIndex);
				}
				else if (samplerName == "albedoSampler")
				{
					prepareTexture(samplerName, pTexture, true, false, 4, outMaterial.m_baseColorIndex);
				}
				else if (samplerName == "layer0Sampler")
				{
					prepareTexture(samplerName, pTexture, true, false, 4, outMaterial.m_layerColorIndices[0]);
				}
				else if (samplerName == "layer1Sampler")
				{
					prepareTexture(samplerName, pTexture, true, false, 4, outMaterial.m_layerColorIndices[1]);
				}
				else if (samplerName == "layer2Sampler")
				{
					prepareTexture(samplerName, pTexture, true, false, 4, outMaterial.m_layerColorIndices[2]);
				}
				else if (samplerName == "layer3Sampler")
				{
					prepareTexture(samplerName, pTexture, true, false, 4, outMaterial.m_layerColorIndices[3]);
				}
				else if (samplerName == "normalSampler")
				{
					prepareTexture(samplerName, pTexture, false, true, 3, outMaterial.m_normalIndex);
				}
				else if (samplerName == "ormSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 3, outMaterial.m_metallicRoughnessIndex);
				}
				else if (samplerName == "emissiveSampler")
				{
					prepareTexture(samplerName, pTexture, true, false, 3, outMaterial.m_emissiveIndex);
				}
				else if (samplerName == "occlusionSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 3, outMaterial.m_occlusionIndex);
				}
				else if (samplerName == "roughnessSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 3, outMaterial.m_roughnessIndex);
				}
				else if (samplerName == "metalnessSampler" || samplerName == "metallicSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 3, outMaterial.m_metallicIndex);
				}
				else if (samplerName == "transmissionSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 3, outMaterial.m_transmissionIndex);
				}
				else if (samplerName == "thicknessSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 3, outMaterial.m_thicknessIndex);
				}
				else if (samplerName == "clearcoatSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 3, outMaterial.m_clearcoatIndex);
				}
				else if (samplerName == "clearcoatRoughnessSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 4, outMaterial.m_clearcoatRoughnessIndex);
				}
				else if (samplerName == "clearcoatNormalSampler")
				{
					prepareTexture(samplerName, pTexture, false, true, 3, outMaterial.m_clearcoatNormalIndex);
				}
				else if (samplerName == "sheenColorSampler")
				{
					prepareTexture(samplerName, pTexture, true, false, 3, outMaterial.m_sheenColorIndex);
				}
				else if (samplerName == "sheenRoughnessSampler")
				{
					prepareTexture(samplerName, pTexture, false, false, 4, outMaterial.m_sheenRoughnessIndex);
				}
			}

			outMaterials[i] = outMaterial;
			outResolvedMaterialSlots[i] = bMaterialResolved ? 1u : 0u;
			outAllTexturesResolved &= bMaterialResolved;
			convertedMaterials.Add(pMaterial.GetRawPtr(), outMaterial);
			convertedMaterialResolution.Add(
				pMaterial.GetRawPtr(),
				outResolvedMaterialSlots[i]);
			++stats.m_uniqueMaterialCount;
			if (!reportCompletedMaterial())
			{
				return false;
			}
		}

		return true;
	}

	size_t ComputeMaterialsSignature(const TVector<MaterialPtr>& materials)
	{
		size_t hash = 1469598103934665603ull;
		for (const auto& material : materials)
		{
			size_t value = material ? material.GetHash() : 0;
			if (material)
			{
				HashCombine(value, material->GetContentRevision());
			}
			hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		}
		return hash;
	}
}

void PathTracer::ParseCommandLineArgs(PathTracer::Params& res, const char** args, int32_t num)
{
	if (res.m_height == 0)
	{
		res.m_height = 720;
	}

	if (res.m_msaa == 0)
	{
		res.m_msaa = 1;
	}

	if (res.m_numSamples == 0)
	{
		res.m_numSamples = 16;
	}

	if (res.m_numAmbientSamples == 0)
	{
		res.m_numAmbientSamples = 16;
	}

	if (res.m_maxBounces == 0)
	{
		res.m_maxBounces = 4;
	}

	if (res.m_ambient == vec3(0.0f))
	{
		res.m_ambient = vec3(0.03f);
	}

	if (res.m_rayBiasBase <= 0.0f)
	{
		res.m_rayBiasBase = 0.0f;
	}

	if (res.m_rayBiasScale <= 0.0f)
	{
		res.m_rayBiasScale = 3e-4f;
	}

	for (int32_t i = 1; i < num; i++)
	{
		std::string arg = args[i];

		if (arg == "--in")
		{
			res.m_pathToModel = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--out")
		{
			res.m_output = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--height")
		{
			res.m_height = atoi(Utils::GetArgValue(args, i, num).c_str());
		}
		else if (arg == "--samples")
		{
			const uint32_t samples = atoi(Utils::GetArgValue(args, i, num).c_str());

			res.m_msaa = samples <= 32 ? std::min(4u, samples) : 8u;
			res.m_numSamples = std::max(1u, (uint32_t)std::lround(samples / (float)res.m_msaa));
		}
		else if (arg == "--bounces")
		{
			res.m_maxBounces = atoi(Utils::GetArgValue(args, i, num).c_str());
		}
		else if (arg == "--ambientSamples")
		{
			res.m_numAmbientSamples = std::max(1, atoi(Utils::GetArgValue(args, i, num).c_str()));
		}
		else if (arg == "--camera")
		{
			res.m_camera = Utils::GetArgValue(args, i, num);
		}
		else if (arg == "--ambient")
		{
			const std::string& hexStr = Utils::GetArgValue(args, i, num);
			const int32_t r = std::stoi(hexStr.substr(0, 2), nullptr, 16);
			const int32_t g = std::stoi(hexStr.substr(2, 2), nullptr, 16);
			const int32_t b = std::stoi(hexStr.substr(4, 2), nullptr, 16);

			res.m_ambient = glm::vec3(r / 255.0f, g / 255.0f, b / 255.0f);
		}
		else if (arg == "--rayBiasBase")
		{
			res.m_rayBiasBase = (std::max)(0.0f, (float)atof(Utils::GetArgValue(args, i, num).c_str()));
		}
		else if (arg == "--rayBiasScale")
		{
			res.m_rayBiasScale = (std::max)(0.0f, (float)atof(Utils::GetArgValue(args, i, num).c_str()));
		}
	}
}

bool PathTracer::InitializeScene(const TVector<TLASInstance>& instances,
	const TVector<MaterialPtr>& materials,
	const TVector<LightProxy>& lightProxies,
	bool bAddDefaultLightIfEmpty,
	const ScenePreparationProgressCallback& progress,
	bool bSkipUnresolvedMaterialInstances,
	const ScenePreparationWarningCallback& warning)
{
	m_tlasInstances = instances;
	m_lightProxies = lightProxies;
	m_bAddDefaultLightIfEmpty = bAddDefaultLightIfEmpty;
	m_tlasOctree.Clear();
	m_emissiveTriangles.Clear();
	m_totalEmissiveWeight = 0.0f;
	m_lastScenePreparationStats = {};
	m_lastScenePreparationStats.m_instanceCount = m_tlasInstances.Num();

	auto reportGeometryProgress = [&](size_t completed) -> bool
	{
		if (!progress)
		{
			return true;
		}
		ScenePreparationProgress update;
		update.m_stage = EScenePreparationStage::Geometry;
		update.m_completed = completed;
		update.m_total = m_tlasInstances.Num();
		return progress(update);
	};
	if (!reportGeometryProgress(0u))
	{
		return false;
	}

	TMap<const TVector<Math::Triangle>*, TSharedPtr<BVH>> preparedBlas;
	for (size_t i = 0; i < m_tlasInstances.Num(); i++)
	{
		auto& instance = m_tlasInstances[i];
		const auto* triangles = instance.m_triangles.GetRawPtr();
		if (triangles && instance.m_blas &&
			!preparedBlas.ContainsKey(triangles))
		{
			preparedBlas.Add(triangles, instance.m_blas);
		}
		if (!ResolveInstanceBlas(instance) && triangles &&
			!triangles->IsEmpty() && instance.m_worldBounds.IsValid())
		{
			TSharedPtr<BVH>* cachedBlas = nullptr;
			if (preparedBlas.Find(triangles, cachedBlas) &&
				cachedBlas && *cachedBlas)
			{
				instance.m_blas = *cachedBlas;
				++m_lastScenePreparationStats.m_reusedBlasCount;
			}
			else
			{
				if (triangles->Num() >
					(std::numeric_limits<uint32_t>::max)())
				{
					return false;
				}
				instance.m_blas = TSharedPtr<BVH>::Make(
					static_cast<uint32_t>(triangles->Num()));
				instance.m_blas->BuildBVH(*triangles);
				preparedBlas.Add(triangles, instance.m_blas);
				++m_lastScenePreparationStats.m_builtBlasCount;
			}
		}

		const size_t completed = i + 1u;
		if ((completed % 64u == 0u ||
			completed == m_tlasInstances.Num()) &&
			!reportGeometryProgress(completed))
		{
			return false;
		}
	}

	const size_t materialsSignature = ComputeMaterialsSignature(materials);
	const bool bNeedRebuildMaterials = m_materials.Num() == 0 ||
		m_cachedMaterialsCount != (uint32_t)materials.Num() ||
		m_cachedMaterialsSignature != materialsSignature ||
		!m_bMaterialsFullyResolved;

	if (bNeedRebuildMaterials)
	{
		m_materials.Clear();
		m_resolvedMaterialSlots.Clear();
		m_textures.Clear();
		m_textureMapping.Clear();
		bool bAllTexturesResolved = true;
		if (!BuildRaytracingMaterialsFromRuntimeMaterials(
				materials,
				m_materials,
				m_resolvedMaterialSlots,
				m_textures,
				m_textureMapping,
				progress,
				warning,
				m_lastScenePreparationStats,
				bAllTexturesResolved))
		{
			m_bMaterialsFullyResolved = false;
			return false;
		}
		m_bMaterialsFullyResolved = bAllTexturesResolved;
		m_cachedMaterialsSignature = materialsSignature;
		m_cachedMaterialsCount = (uint32_t)materials.Num();
	}
	else
	{
		m_lastScenePreparationStats.m_materialSlotCount = materials.Num();
		if (progress)
		{
			ScenePreparationProgress update;
			update.m_stage = EScenePreparationStage::Materials;
			update.m_completed = materials.Num();
			update.m_total = materials.Num();
			if (!progress(update))
			{
				return false;
			}
		}
	}

	if (m_materials.Num() == 0)
	{
		m_materials.Add(Material{});
		m_resolvedMaterialSlots.Add(1u);
		m_bMaterialsFullyResolved = true;
	}

	bool bHasGeometry = false;
	TMap<const TVector<Math::Triangle>*, TVector<uint32_t>>
		referencedMaterialSlots;
	for (size_t i = 0u; i < m_tlasInstances.Num(); ++i)
	{
		const TLASInstance& instance = m_tlasInstances[i];
		if (!HasInstanceGeometry(instance))
		{
			if (bSkipUnresolvedMaterialInstances)
			{
				++m_lastScenePreparationStats.m_skippedInstanceCount;
				if (warning)
				{
					warning(
						"skipped " +
						(instance.m_debugName.empty() ?
							"mesh instance " + std::to_string(i) :
							instance.m_debugName) +
						": CPU raytracing geometry is unavailable");
				}
			}
			continue;
		}

		bool bSkipInstance = false;
		uint32_t unresolvedLocalSlot = 0u;
		int64_t unresolvedGlobalSlot = -1;
		if (bSkipUnresolvedMaterialInstances)
		{
			const auto* triangles = ResolveInstanceTriangles(instance);
			auto& localMaterialSlots = referencedMaterialSlots[triangles];
			if (localMaterialSlots.IsEmpty())
			{
				for (const Math::Triangle& triangle : *triangles)
				{
					if (std::find(
							localMaterialSlots.begin(),
							localMaterialSlots.end(),
							triangle.m_materialIndex) ==
						localMaterialSlots.end())
					{
						localMaterialSlots.Add(triangle.m_materialIndex);
					}
				}
			}
			for (uint32_t localMaterialSlot : localMaterialSlots)
			{
				const int64_t globalSlot =
					static_cast<int64_t>(instance.m_materialBaseOffset) +
					static_cast<int64_t>(localMaterialSlot);
				if (globalSlot < 0 ||
					globalSlot >= static_cast<int64_t>(
						m_resolvedMaterialSlots.Num()) ||
					m_resolvedMaterialSlots[static_cast<size_t>(globalSlot)] == 0u)
				{
					bSkipInstance = true;
					unresolvedLocalSlot = localMaterialSlot;
					unresolvedGlobalSlot = globalSlot;
					break;
				}
			}
		}

		if (bSkipInstance)
		{
			++m_lastScenePreparationStats.m_skippedInstanceCount;
			if (warning)
			{
				std::string materialIdentity =
					"material slot " + std::to_string(unresolvedLocalSlot);
				if (unresolvedGlobalSlot >= 0 &&
					unresolvedGlobalSlot < static_cast<int64_t>(materials.Num()))
				{
					const MaterialPtr& material =
						materials[static_cast<size_t>(unresolvedGlobalSlot)];
					const std::string fileId = material ?
						material->GetFileId().ToString() : std::string();
					if (!fileId.empty())
					{
						materialIdentity += " ('" + fileId + "')";
					}
				}
				warning(
					"skipped " +
					(instance.m_debugName.empty() ?
						"mesh instance " + std::to_string(i) :
						instance.m_debugName) +
					" because " + materialIdentity +
					" could not be prepared");
			}
			continue;
		}

		bHasGeometry = true;
		++m_lastScenePreparationStats.m_geometryInstanceCount;
		m_lastScenePreparationStats.m_triangleCount +=
			ResolveInstanceTriangles(instance)->Num();
		const glm::ivec3 integerMin =
			glm::ivec3(glm::floor(instance.m_worldBounds.m_min));
		const glm::ivec3 integerMax =
			glm::ivec3(glm::ceil(instance.m_worldBounds.m_max));
		const glm::ivec3 integerCenter =
			(integerMin + integerMax) / 2;
		const glm::ivec3 integerExtents = glm::max(
			integerCenter - integerMin,
			integerMax - integerCenter);
		m_tlasOctree.Update(
			integerCenter,
			glm::max(integerExtents, glm::ivec3(1)),
			i);
		AppendEmissiveTriangles(static_cast<uint32_t>(i));
	}
	m_lastScenePreparationStats.m_emissiveTriangleCount =
		m_emissiveTriangles.Num();
	m_lastScenePreparationStats.m_emissiveSamplingWeight =
		m_totalEmissiveWeight;

	if (m_bAddDefaultLightIfEmpty && m_lightProxies.Num() == 0)
	{
		DirectionalLight sun{};
		sun.m_direction = glm::normalize(vec3(-0.7f, -1.0f, -0.2f));
		sun.m_intensity = vec3(1.0f);
		m_lightProxies.Add(MakeDirectionalLightProxy(sun));
	}

	return bHasGeometry;
}

void PathTracer::SetRuntimeEnvironment(const TVector<u8vec4>& image, const glm::uvec2& extent)
{
	if (image.Num() == 0 || extent.x == 0 || extent.y == 0 || image.Num() < (size_t)extent.x * (size_t)extent.y)
	{
		return;
	}

	m_runtimeEnvironment.Initialize<vec3>(extent.x, extent.y, 3, SamplerClamping::Repeat);
	for (uint32_t y = 0; y < extent.y; y++)
	{
		for (uint32_t x = 0; x < extent.x; x++)
		{
			const u8vec4 src = image[x + y * extent.x];
			const vec3 linear = Utils::SRGBToLinear(vec3(src) * (1.0f / 255.0f));
			m_runtimeEnvironment.SetPixel(x, y, linear);
		}
	}

	m_bHasRuntimeEnvironment = true;
	RebuildRuntimeEnvironmentImportance();
}

void PathTracer::SetRuntimeEnvironmentLinear(const TVector<vec4>& image, const glm::uvec2& extent)
{
	if (image.Num() == 0 || extent.x == 0 || extent.y == 0 || image.Num() < (size_t)extent.x * (size_t)extent.y)
	{
		return;
	}

	m_runtimeEnvironment.Initialize<vec3>(extent.x, extent.y, 3, SamplerClamping::Repeat);
	for (uint32_t y = 0; y < extent.y; y++)
	{
		for (uint32_t x = 0; x < extent.x; x++)
		{
			const vec4 src = image[x + y * extent.x];
			m_runtimeEnvironment.SetPixel(x, y, vec3(src));
		}
	}

	m_bHasRuntimeEnvironment = true;
	RebuildRuntimeEnvironmentImportance();
}

void PathTracer::SetRuntimeDiffuseEnvironmentLinear(const TVector<vec4>& image, const glm::uvec2& extent)
{
	if (image.Num() == 0 || extent.x == 0 || extent.y == 0 || image.Num() < (size_t)extent.x * (size_t)extent.y)
	{
		return;
	}

	m_runtimeDiffuseEnvironment.Initialize<vec3>(extent.x, extent.y, 3, SamplerClamping::Repeat);
	for (uint32_t y = 0; y < extent.y; y++)
	{
		for (uint32_t x = 0; x < extent.x; x++)
		{
			const vec4 src = image[x + y * extent.x];
			m_runtimeDiffuseEnvironment.SetPixel(x, y, vec3(src));
		}
	}

	m_bHasRuntimeDiffuseEnvironment = true;
	RebuildRuntimeEnvironmentImportance();
}

void PathTracer::ClearRuntimeEnvironment()
{
	m_runtimeEnvironment.m_data.Clear();
	m_runtimeEnvironment.m_width = 0;
	m_runtimeEnvironment.m_height = 0;
	m_runtimeEnvironment.m_channels = 3;
	m_runtimeEnvironment.m_clamping = SamplerClamping::Clamp;
	m_bHasRuntimeEnvironment = false;

	m_runtimeDiffuseEnvironment.m_data.Clear();
	m_runtimeDiffuseEnvironment.m_width = 0;
	m_runtimeDiffuseEnvironment.m_height = 0;
	m_runtimeDiffuseEnvironment.m_channels = 3;
	m_runtimeDiffuseEnvironment.m_clamping = SamplerClamping::Clamp;
	m_bHasRuntimeDiffuseEnvironment = false;
	m_runtimeEnvironmentImportanceCdf.Clear();
	m_runtimeEnvironmentImportancePdf.Clear();
	m_bUseRuntimeEnvironmentImportance = false;
}

vec3 PathTracer::SampleRuntimeEnvironment(const vec3& direction) const
{
	if (!m_bHasRuntimeEnvironment || m_runtimeEnvironment.m_width <= 0 || m_runtimeEnvironment.m_height <= 0)
	{
		return vec3(0.0f);
	}

	const vec3 dir = glm::normalize(direction);
	const float phi = atan2(dir.z, dir.x);
	const float theta = acos(glm::clamp(dir.y, -1.0f, 1.0f));
	const float u = (phi + Pi) / (2.0f * Pi);
	const float v = theta / Pi;

	return m_runtimeEnvironment.Sample<vec3>(vec2(u, v));
}

vec3 PathTracer::SampleRuntimeDiffuseEnvironment(const vec3& direction) const
{
	if (!m_bHasRuntimeDiffuseEnvironment || m_runtimeDiffuseEnvironment.m_width <= 0 || m_runtimeDiffuseEnvironment.m_height <= 0)
	{
		return SampleRuntimeEnvironment(direction);
	}

	const vec3 dir = glm::normalize(direction);
	const float phi = atan2(dir.z, dir.x);
	const float theta = acos(glm::clamp(dir.y, -1.0f, 1.0f));
	const float u = (phi + Pi) / (2.0f * Pi);
	const float v = theta / Pi;

	return m_runtimeDiffuseEnvironment.Sample<vec3>(vec2(u, v));
}

vec3 PathTracer::SampleRuntimeDirectEnvironment(const vec3& direction) const
{
	return m_bHasRuntimeEnvironment ?
		SampleRuntimeEnvironment(direction) :
		SampleRuntimeDiffuseEnvironment(direction);
}

void PathTracer::RebuildRuntimeEnvironmentImportance()
{
	m_runtimeEnvironmentImportanceCdf.Clear();
	m_runtimeEnvironmentImportancePdf.Clear();
	m_bUseRuntimeEnvironmentImportance = false;

	const CombinedSampler2D* source = m_bHasRuntimeEnvironment ?
		&m_runtimeEnvironment :
		(m_bHasRuntimeDiffuseEnvironment ?
			&m_runtimeDiffuseEnvironment : nullptr);
	if (!source || source->m_width <= 0 || source->m_height <= 0)
	{
		return;
	}

	const size_t pixelCount = static_cast<size_t>(source->m_width) *
		static_cast<size_t>(source->m_height);
	if (source->m_data.Num() < pixelCount * sizeof(vec3))
	{
		return;
	}

	const vec3* pixels = reinterpret_cast<const vec3*>(
		source->m_data.GetData());
	TVector<double> solidAngleWeights;
	solidAngleWeights.Resize(pixelCount);
	double totalLuminanceWeight = 0.0;
	double maximumLuminance = 0.0;
	const double deltaPhi = 2.0 * static_cast<double>(Pi) /
		static_cast<double>(source->m_width);
	for (int32_t y = 0; y < source->m_height; ++y)
	{
		const double theta0 = static_cast<double>(Pi) *
			static_cast<double>(y) /
			static_cast<double>(source->m_height);
		const double theta1 = static_cast<double>(Pi) *
			static_cast<double>(y + 1) /
			static_cast<double>(source->m_height);
		const double pixelSolidAngle = deltaPhi *
			(std::cos(theta0) - std::cos(theta1));
		for (int32_t x = 0; x < source->m_width; ++x)
		{
			const size_t index = static_cast<size_t>(x) +
				static_cast<size_t>(y) *
				static_cast<size_t>(source->m_width);
			const vec3 radiance = SanitizeRadiance(pixels[index]);
			const double luminance = static_cast<double>(glm::dot(
				radiance,
				vec3(0.2126f, 0.7152f, 0.0722f)));
			solidAngleWeights[index] = luminance * pixelSolidAngle;
			totalLuminanceWeight += solidAngleWeights[index];
			maximumLuminance = (std::max)(maximumLuminance, luminance);
		}
	}

	if (!std::isfinite(totalLuminanceWeight) ||
		totalLuminanceWeight <= 0.0)
	{
		return;
	}

	const double sphereSolidAngle = 4.0 * static_cast<double>(Pi);
	const double averageLuminance =
		totalLuminanceWeight / sphereSolidAngle;
	// A nearly uniform map is sampled more efficiently by the receiver-aligned
	// hemisphere technique. Importance sampling is reserved for concentrated
	// HDR features such as the solar aureole and bright emissive texels.
	if (maximumLuminance <= averageLuminance * 4.0)
	{
		return;
	}

	const double uniformFloor = averageLuminance * 0.01;
	double totalWeight = totalLuminanceWeight +
		uniformFloor * sphereSolidAngle;
	if (!std::isfinite(totalWeight) || totalWeight <= 0.0)
	{
		return;
	}

	m_runtimeEnvironmentImportanceCdf.Resize(pixelCount);
	m_runtimeEnvironmentImportancePdf.Resize(pixelCount);
	double cumulativeProbability = 0.0;
	for (int32_t y = 0; y < source->m_height; ++y)
	{
		const double theta0 = static_cast<double>(Pi) *
			static_cast<double>(y) /
			static_cast<double>(source->m_height);
		const double theta1 = static_cast<double>(Pi) *
			static_cast<double>(y + 1) /
			static_cast<double>(source->m_height);
		const double pixelSolidAngle = deltaPhi *
			(std::cos(theta0) - std::cos(theta1));
		for (int32_t x = 0; x < source->m_width; ++x)
		{
			const size_t index = static_cast<size_t>(x) +
				static_cast<size_t>(y) *
				static_cast<size_t>(source->m_width);
			const double probability =
				(solidAngleWeights[index] +
					uniformFloor * pixelSolidAngle) / totalWeight;
			cumulativeProbability += probability;
			m_runtimeEnvironmentImportanceCdf[index] =
				static_cast<float>(cumulativeProbability);
			m_runtimeEnvironmentImportancePdf[index] =
				static_cast<float>(probability / pixelSolidAngle);
		}
	}
	m_runtimeEnvironmentImportanceCdf[pixelCount - 1u] = 1.0f;
	m_bUseRuntimeEnvironmentImportance = true;
}

float PathTracer::RuntimeEnvironmentImportancePdf(
	const vec3& direction) const
{
	if (!m_bUseRuntimeEnvironmentImportance ||
		m_runtimeEnvironmentImportancePdf.IsEmpty())
	{
		return 0.0f;
	}

	const CombinedSampler2D* source = m_bHasRuntimeEnvironment ?
		&m_runtimeEnvironment :
		(m_bHasRuntimeDiffuseEnvironment ?
			&m_runtimeDiffuseEnvironment : nullptr);
	if (!source || source->m_width <= 0 || source->m_height <= 0)
	{
		return 0.0f;
	}

	const vec3 normalizedDirection = glm::normalize(direction);
	const float phi = std::atan2(
		normalizedDirection.z,
		normalizedDirection.x);
	const float theta = std::acos(glm::clamp(
		normalizedDirection.y,
		-1.0f,
		1.0f));
	const float u = (phi + Pi) / (2.0f * Pi);
	const float v = theta / Pi;
	const uint32_t x = (std::min)(
		static_cast<uint32_t>(u * static_cast<float>(source->m_width)),
		static_cast<uint32_t>(source->m_width - 1));
	const uint32_t y = (std::min)(
		static_cast<uint32_t>(v * static_cast<float>(source->m_height)),
		static_cast<uint32_t>(source->m_height - 1));
	return m_runtimeEnvironmentImportancePdf[
		static_cast<size_t>(x) + static_cast<size_t>(y) *
			static_cast<size_t>(source->m_width)];
}

bool PathTracer::SampleRuntimeEnvironmentImportance(
	uint32_t& randomState,
	vec3& outDirection,
	float& outPdf) const
{
	outDirection = vec3(0.0f);
	outPdf = 0.0f;
	if (!m_bUseRuntimeEnvironmentImportance ||
		m_runtimeEnvironmentImportanceCdf.IsEmpty())
	{
		return false;
	}

	const CombinedSampler2D* source = m_bHasRuntimeEnvironment ?
		&m_runtimeEnvironment :
		(m_bHasRuntimeDiffuseEnvironment ?
			&m_runtimeDiffuseEnvironment : nullptr);
	if (!source || source->m_width <= 0 || source->m_height <= 0)
	{
		return false;
	}

	const float selection = NextRandom01(randomState);
	size_t first = 0u;
	size_t count = m_runtimeEnvironmentImportanceCdf.Num();
	while (count > 0u)
	{
		const size_t step = count / 2u;
		const size_t middle = first + step;
		if (m_runtimeEnvironmentImportanceCdf[middle] < selection)
		{
			first = middle + 1u;
			count -= step + 1u;
		}
		else
		{
			count = step;
		}
	}
	const size_t index = (std::min)(
		first,
		m_runtimeEnvironmentImportanceCdf.Num() - 1u);
	const uint32_t x = static_cast<uint32_t>(
		index % static_cast<size_t>(source->m_width));
	const uint32_t y = static_cast<uint32_t>(
		index / static_cast<size_t>(source->m_width));
	const float theta0 = Pi * static_cast<float>(y) /
		static_cast<float>(source->m_height);
	const float theta1 = Pi * static_cast<float>(y + 1u) /
		static_cast<float>(source->m_height);
	const float cosTheta = glm::mix(
		std::cos(theta0),
		std::cos(theta1),
		NextRandom01(randomState));
	const float sinTheta = std::sqrt((std::max)(
		0.0f,
		1.0f - cosTheta * cosTheta));
	const float phi = -Pi + 2.0f * Pi *
		(static_cast<float>(x) + NextRandom01(randomState)) /
		static_cast<float>(source->m_width);
	outDirection = vec3(
		std::cos(phi) * sinTheta,
		cosTheta,
		std::sin(phi) * sinTheta);
	outPdf = RuntimeEnvironmentImportancePdf(outDirection);
	return std::isfinite(outPdf) && outPdf > 0.0f;
}

float PathTracer::DirectEnvironmentPdf(
	const vec3& worldNormal,
	const vec3& direction) const
{
	const float hemispherePdf = glm::dot(worldNormal, direction) > 0.0f ?
		1.0f / (2.0f * Pi) : 0.0f;
	if (!m_bUseRuntimeEnvironmentImportance)
	{
		return hemispherePdf;
	}
	constexpr float ImportanceMix = 0.5f;
	return hemispherePdf * (1.0f - ImportanceMix) +
		RuntimeEnvironmentImportancePdf(direction) * ImportanceMix;
}

bool PathTracer::SampleDirectEnvironment(
	const vec3& worldNormal,
	uint32_t& randomState,
	vec3& outDirection,
	float& outPdf) const
{
	outDirection = vec3(0.0f);
	outPdf = 0.0f;
	constexpr float ImportanceMix = 0.5f;
	if (m_bUseRuntimeEnvironmentImportance &&
		NextRandom01(randomState) < ImportanceMix)
	{
		if (!SampleRuntimeEnvironmentImportance(
				randomState,
				outDirection,
				outPdf))
		{
			return false;
		}
	}

	if (glm::dot(outDirection, outDirection) <= 0.0f)
	{
		outDirection = LightingModel::ImportanceSampleHemisphere(
			NextRandomVec2_01(randomState),
			worldNormal);
	}
	outPdf = DirectEnvironmentPdf(worldNormal, outDirection);
	return std::isfinite(outPdf) && outPdf > 0.0f;
}

bool PathTracer::RenderPreparedScene(const PathTracer::Params& params)
{
	m_lastRaytraceTimeMs = 0.0;
	m_lastRenderedImage.Clear();
	m_lastRenderedExtent = glm::uvec2(0, 0);

	if (m_tlasInstances.Num() == 0)
	{
		SAILOR_LOG_ERROR("PathTracer scene has no TLASInstances.");
		return false;
	}

	Math::AABB bounds{};
	for (const auto& instance : m_tlasInstances)
	{
		if (instance.m_worldBounds.IsValid())
		{
			bounds.Extend(instance.m_worldBounds);
		}
	}

	const vec3 sceneCenter = 0.5f * (bounds.m_min + bounds.m_max);
	const float sceneRadius = glm::distance(bounds.m_max, sceneCenter);

	vec3 cameraPos = sceneCenter + vec3(0.0f, std::max(sceneRadius, 0.1f) * 0.6f, std::max(sceneRadius, 0.1f) * 2.5f);
	vec3 cameraForward = glm::normalize(sceneCenter - cameraPos);
	vec3 cameraUp = vec3(0, 1, 0);
	float aspectRatio = std::max(DefaultAspectRatio, 0.1f);
	float hFov = std::max(DefaultHfov, glm::radians(10.0f));

	if (params.m_bUseRuntimeCamera)
	{
		cameraPos = params.m_runtimeCameraPos;

		if (glm::length(params.m_runtimeCameraForward) > 0.001f)
		{
			cameraForward = glm::normalize(params.m_runtimeCameraForward);
		}

		if (glm::length(params.m_runtimeCameraUp) > 0.001f)
		{
			cameraUp = glm::normalize(params.m_runtimeCameraUp);
		}

		if (params.m_runtimeAspectRatio > 0.1f)
		{
			aspectRatio = params.m_runtimeAspectRatio;
		}

		if (params.m_runtimeHFov > 0.0f)
		{
			hFov = std::max(params.m_runtimeHFov, glm::radians(10.0f));
		}
	}

	if (abs(dot(cameraForward, cameraUp)) > 0.99f || length(cameraForward) < 0.001f)
	{
		cameraUp = vec3(0.0f, 0.0f, 1.0f);
		cameraForward = glm::normalize(vec3(0, 0, -1.0f));
	}

	const uint32_t height = params.m_height;
	const uint32_t width = (std::max)(1u, (uint32_t)std::lround((double)height * (double)aspectRatio));
	const float vFov = 2.0f * atan(tan(hFov * 0.5f) * (1.0f / aspectRatio));

	if (m_bAddDefaultLightIfEmpty && m_lightProxies.Num() == 0)
	{
		DirectionalLight sun{};
		sun.m_direction = glm::normalize(vec3(-0.7f, -1.0f, -0.2f));
		sun.m_intensity = vec3(1.0f);
		m_lightProxies.Add(MakeDirectionalLightProxy(sun));
	}

	CombinedSampler2D outputTex;
	outputTex.Initialize<vec3>(width, height);
	CombinedSampler2D alphaTex;
	alphaTex.Initialize<float>(width, height);

	float h = tan(vFov / 2);
	const float ViewportHeight = 2.0f * h;
	const float ViewportWidth = aspectRatio * ViewportHeight;

	vec3 _u = normalize(cross(cameraUp, -cameraForward));
	vec3 _v = cross(-cameraForward, _u);

	const vec3 ViewportU = ViewportWidth * _u;
	const vec3 ViewportV = ViewportHeight * _v;
	const vec3 ViewportPivot = cameraPos - (ViewportU + ViewportV) * 0.5f + cameraForward;

	const vec3 _pixelDeltaU = ViewportU / (float)width;
	const vec3 _pixelDeltaV = ViewportV / (float)height;
	const vec3 _pixel00Dir = ViewportPivot + 0.5f * (_pixelDeltaU + _pixelDeltaV) - cameraPos;

	Utils::Timer raytracingTimer;
	raytracingTimer.Start();

	{
		TVector<Tasks::ITaskPtr> tasks;
		TVector<Tasks::ITaskPtr> tasksThisThread;

		std::atomic<uint32_t> finishedTasks = 0;
		const uint32_t numTasksX = (width + DefaultGroupSize - 1) / DefaultGroupSize;
		const uint32_t numTasksY = (height + DefaultGroupSize - 1) / DefaultGroupSize;
		const uint32_t numTasks = std::max(1u, numTasksX * numTasksY);

		tasks.Reserve(numTasks);
		tasksThisThread.Reserve(numTasks / 32);

		for (uint32_t y = 0; y < height; y += DefaultGroupSize)
		{
			for (uint32_t x = 0; x < width; x += DefaultGroupSize)
			{
				auto task = Tasks::CreateTask("Calculate raytracing",
					[=, &finishedTasks, &outputTex, &alphaTex, this]() mutable
					{
						Ray ray;
						ray.SetOrigin(cameraPos);

						for (uint32_t v = 0; (v < DefaultGroupSize) && (y + v) < height; v++)
						{
							for (uint32_t u = 0; u < DefaultGroupSize && (u + x) < width; u++)
							{
								vec3 accumulator = vec3(0);
								float alphaCoverage = 0.0f;
								for (uint32_t sample = 0; sample < params.m_msaa; sample++)
								{
									uint32_t randomState = NextRandomU32();
									const vec2 offset = sample == 0 ?
										vec2(0.5f, 0.5f) :
										NextRandomVec2_01(randomState);
									const vec3 pixelDir = _pixel00Dir + ((float)(u + x) + offset.x) * _pixelDeltaU + ((float)(y + v) - offset.y) * _pixelDeltaV;
									ray.SetDirection(glm::normalize(pixelDir));
									TLASHit primaryHit{};
									alphaCoverage += IntersectScene(ray, primaryHit, std::numeric_limits<float>::max(), (uint32_t)(-1), (uint32_t)(-1)) ? 1.0f : 0.0f;
									accumulator += Raytrace(ray, params.m_maxBounces, (uint32_t)(-1), (uint32_t)(-1), std::numeric_limits<float>::max(), params, 1.0f, randomState, true);
								}

								vec3 res = accumulator / (float)params.m_msaa;
								outputTex.SetPixel(x + u, height - (y + v) - 1, res);
								alphaTex.SetPixel(x + u, height - (y + v) - 1, alphaCoverage / (float)params.m_msaa);
							}
						}

						finishedTasks++;
					}, EThreadType::Worker);

				if (params.m_bRunTasksInline ||
					((x + y) / DefaultGroupSize) % 32 == 0)
				{
					tasksThisThread.Emplace(task);
				}
				else
				{
					task->Run();
					tasks.Emplace(std::move(task));
				}
			}
		}

		for (auto& task : tasksThisThread)
		{
			task->Execute();
		}

		for (auto& task : tasks)
		{
			if (!task->IsFinished())
			{
				task->Wait();
			}
		}
	}

	raytracingTimer.Stop();
	m_lastRaytraceTimeMs = static_cast<double>(raytracingTimer.ResultAccumulatedMs());

	TVector<u8vec4> outSrgb(width * height);
	const float aberrationAmount = (0.5f / width);

	for (uint32_t y = 0; y < height; y++)
	{
		for (uint32_t x = 0; x < width; x++)
		{
			vec2 uv = vec2((float)x / width, (float)y / height);
			vec3 greenColor = outputTex.Sample<vec3>(uv + vec2(aberrationAmount, 0));
			vec3 blueColor = outputTex.Sample<vec3>(uv + vec2(aberrationAmount, aberrationAmount));
			vec3 redColor = outputTex.Sample<vec3>(uv + vec2(-aberrationAmount, -aberrationAmount));
			vec3 chromaAberratedColor = vec3(redColor.r, greenColor.g, blueColor.b);
			const float alpha = glm::clamp(alphaTex.Sample<float>(uv), 0.0f, 1.0f);
			const u8vec3 rgb = alpha > 0.0f ?
				u8vec3(glm::clamp(Utils::LinearToSRGB(chromaAberratedColor) * 255.0f, 0.0f, 255.0f)) :
				u8vec3(0, 0, 0);
			outSrgb[x + y * width] = u8vec4(rgb, (uint8_t)glm::round(alpha * 255.0f));
		}
	}

	m_lastRenderedImage = outSrgb;
	m_lastRenderedExtent = glm::uvec2(width, height);

	if (!params.m_output.empty())
	{
		const uint32_t Channels = 4;
		if (!stbi_write_png(params.m_output.string().c_str(), width, height, Channels, outSrgb.GetData(), width * Channels))
		{
			SAILOR_LOG_ERROR("Raytracing WriteImage error");
			return false;
		}
	}

	return true;
}

bool PathTracer::SamplePreparedSceneRay(
	const vec3& origin,
	const vec3& direction,
	float maxDistance,
	const Params& params,
	uint32_t randomSeed,
	PreparedRaySample& outSample) const
{
	if (!SamplePreparedSceneVisibility(
			origin,
			direction,
			maxDistance,
			outSample))
	{
		return false;
	}

	const float directionLength = glm::length(direction);
	const vec3 normalizedDirection = direction / directionLength;
	const Math::Ray ray(origin, normalizedDirection);
	if (outSample.m_bHit)
	{
		uint32_t randomState = randomSeed ^ 0xa511e9b3u;
		if (randomState == 0u)
		{
			randomState = 0x6d2b79f5u;
		}
		outSample.m_radiance = Raytrace(
			ray,
			params.m_maxBounces,
			(uint32_t)-1,
			(uint32_t)-1,
			maxDistance,
			params,
			1.0f,
			randomState,
			true);
	}
	else if (params.m_bIncludeEnvironment)
	{
		outSample.m_radiance = m_bHasRuntimeEnvironment ?
			SampleRuntimeEnvironment(normalizedDirection) :
			m_bHasRuntimeDiffuseEnvironment ?
				SampleRuntimeDiffuseEnvironment(normalizedDirection) :
				params.m_ambient;
	}
	return true;
}

bool PathTracer::SamplePreparedSceneVisibility(
	const vec3& origin,
	const vec3& direction,
	float maxDistance,
	PreparedRaySample& outSample) const
{
	outSample = {};
	const float directionLength = glm::length(direction);
	if (m_tlasInstances.IsEmpty() ||
		!std::isfinite(directionLength) ||
		directionLength <= 1e-6f ||
		!std::isfinite(maxDistance) ||
		maxDistance <= 0.0f)
	{
		return false;
	}

	const vec3 normalizedDirection = direction / directionLength;
	const Math::Ray ray(origin, normalizedDirection);
	TLASHit hit{};
	outSample.m_bHit = IntersectScene(
		ray,
		hit,
		maxDistance,
		(uint32_t)-1,
		(uint32_t)-1);
	outSample.m_distance = outSample.m_bHit ?
		hit.m_hit.m_rayLenght : maxDistance;
	outSample.m_bBackFace = outSample.m_bHit &&
		glm::dot(hit.m_geometricNormal, normalizedDirection) > 0.0f;
	return true;
}

void PathTracer::Run(const PathTracer::Params& params)
{
	SAILOR_PROFILE_FUNCTION();
	m_lastRaytraceTimeMs = 0.0;
	m_lastRenderedImage.Clear();
	m_lastRenderedExtent = glm::uvec2(0, 0);

	auto* pAssetRegistry = App::GetSubmodule<AssetRegistry>();
	auto* pModelImporter = App::GetSubmodule<ModelImporter>();
	auto* pMaterialImporter = App::GetSubmodule<MaterialImporter>();

	if (!pAssetRegistry || !pModelImporter || !pMaterialImporter)
	{
		SAILOR_LOG_ERROR("PathTracer requires initialized AssetRegistry/ModelImporter/MaterialImporter.");
		return;
	}

	ModelAssetInfoPtr pModelAssetInfo = pAssetRegistry->GetAssetInfoPtr<ModelAssetInfoPtr>(params.m_pathToModel.string());

	if (!pModelAssetInfo && std::filesystem::exists(params.m_pathToModel))
	{
		const FileId& modelFileId = pAssetRegistry->GetOrLoadFile(params.m_pathToModel.string());
		pModelAssetInfo = pAssetRegistry->GetAssetInfoPtr<ModelAssetInfoPtr>(modelFileId);
	}

	if (!pModelAssetInfo)
	{
		SAILOR_LOG_ERROR("Cannot resolve model asset: %s", params.m_pathToModel.string().c_str());
		return;
	}

	if (pModelAssetInfo->ShouldGenerateMaterials() && pModelAssetInfo->GetDefaultMaterials().Num() == 0)
	{
		pModelImporter->OnImportAsset(pModelAssetInfo);
	}

	m_directionalLights.Clear();
	m_lightProxies.Clear();
	m_tlasInstances.Clear();
	m_tlasOctree.Clear();
	m_materials.Clear();
	m_textures.Clear();
	m_textureMapping.Clear();

	if (!pModelAssetInfo->ShouldKeepCpuBuffers())
	{
		SAILOR_LOG_ERROR("Path tracer requires bShouldKeepCpuBuffers=true in model asset info: %s", params.m_pathToModel.string().c_str());
		return;
	}

	ModelPtr pModel;
	Tasks::TaskPtr<ModelPtr> pLoadModelTask = pModelImporter->LoadModel(pModelAssetInfo->GetFileId(), pModel);
	if (!pLoadModelTask.IsValid())
	{
		SAILOR_LOG_ERROR("Cannot start model loading for path tracing: %s", params.m_pathToModel.string().c_str());
		return;
	}

	pLoadModelTask->Wait();
	pModel = pLoadModelTask->GetResult();
	if (!pModel || !pModel->IsReady() || !pModel->HasCpuMeshes())
	{
		SAILOR_LOG_ERROR("Path tracer requires CPU model buffers. Enable bShouldKeepCpuBuffers for model: %s", params.m_pathToModel.string().c_str());
		return;
	}

	const Math::Sphere boundsSphere = pModel->GetBoundsSphere();

	auto defaultMaterials = pModelAssetInfo->GetDefaultMaterials();
	m_materials.Resize(defaultMaterials.Num());

	if (!pModel->HasBLAS() || pModel->GetBLASTriangles().Num() == 0)
	{
		SAILOR_LOG_ERROR("PathTracer requires model BLAS/TLAS-ready scene data: %s", params.m_pathToModel.string().c_str());
		return;
	}

	TVector<MaterialPtr> runtimeMaterials(defaultMaterials.Num());
	for (size_t i = 0; i < defaultMaterials.Num(); i++)
	{
		const FileId materialFileId = defaultMaterials[i];
		if (!materialFileId)
		{
			continue;
		}

		MaterialPtr pMaterial;
		if (pMaterialImporter->LoadMaterial_Immediate(materialFileId, pMaterial) && pMaterial)
		{
			runtimeMaterials[i] = pMaterial;
		}
	}

	m_lastScenePreparationStats = {};
	bool bAllTexturesResolved = true;
	BuildRaytracingMaterialsFromRuntimeMaterials(
		runtimeMaterials,
		m_materials,
		m_resolvedMaterialSlots,
		m_textures,
		m_textureMapping,
		{},
		{},
		m_lastScenePreparationStats,
		bAllTexturesResolved);
	m_bMaterialsFullyResolved = bAllTexturesResolved;
	if (m_materials.Num() == 0)
	{
		m_materials.Add(Material{});
		m_resolvedMaterialSlots.Add(1u);
		m_bMaterialsFullyResolved = true;
	}

	Raytracing::PathTracer::TLASInstance instance{};
	instance.m_model = pModel;
	instance.m_worldMatrix = glm::mat4(1.0f);
	instance.m_inverseWorldMatrix = glm::mat4(1.0f);
	instance.m_worldBounds = Math::AABB(boundsSphere.m_center - vec3(boundsSphere.m_radius), boundsSphere.m_center + vec3(boundsSphere.m_radius));
	instance.m_materialBaseOffset = 0;
	m_tlasInstances.Add(instance);

	PathTracerView view{};
	tinygltf::Model gltfModel;
	{
		tinygltf::TinyGLTF loader;
		std::string err;
		std::string warn;
		const bool bIsGlb = Utils::GetFileExtension(pModelAssetInfo->GetAssetFilepath().c_str()) == "glb";
		const bool bParsed = bIsGlb ?
			loader.LoadBinaryFromFile(&gltfModel, &err, &warn, pModelAssetInfo->GetAssetFilepath().c_str()) :
			loader.LoadASCIIFromFile(&gltfModel, &err, &warn, pModelAssetInfo->GetAssetFilepath().c_str());

		if (bParsed)
		{
			ResolveViewAndDirectionalLights(gltfModel, params, boundsSphere, view, m_directionalLights);
		}
	}

	Utils::Timer raytracingTimer;
	raytracingTimer.Start();

	const vec3 cameraPos = view.m_cameraPos;
	vec3 cameraForward = view.m_cameraForward;
	vec3 cameraUp = view.m_cameraUp;
	if (abs(dot(cameraForward, cameraUp)) > 0.99f || length(cameraForward) < 0.001f)
	{
		cameraUp = vec3(0.0f, 0.0f, 1.0f);
		cameraForward = glm::normalize(vec3(0, 0, -1.0f));
	}

	const float aspectRatio = std::max(view.m_aspectRatio, 0.1f);
	const uint32_t height = params.m_height;
	const uint32_t width = (std::max)(1u, (uint32_t)std::lround((double)height * (double)aspectRatio));

	const float hFov = std::max(view.m_hFov, glm::radians(10.0f));

	const float vFov = 2.0f * atan(tan(hFov * 0.5f) * (1.0f / aspectRatio));
	if (m_directionalLights.Num() == 0)
	{
		DirectionalLight sun{};
		sun.m_direction = glm::normalize(vec3(-0.7f, -1.0f, -0.2f));
		sun.m_intensity = vec3(1.0f);
		m_directionalLights.Add(sun);
	}

	m_lightProxies.Reserve(m_directionalLights.Num());
	for (const auto& directional : m_directionalLights)
	{
		m_lightProxies.Add(MakeDirectionalLightProxy(directional));
	}

	CombinedSampler2D outputTex;
	outputTex.Initialize<vec3>(width, height);
	CombinedSampler2D alphaTex;
	alphaTex.Initialize<float>(width, height);

	float h = tan(vFov / 2);
	const float ViewportHeight = 2.0f * h;
	const float ViewportWidth = aspectRatio * ViewportHeight;
	
	vec3 _u = normalize(cross(cameraUp, -cameraForward));
	vec3 _v = cross(-cameraForward, _u);

	const vec3 ViewportU = ViewportWidth * _u;
	const vec3 ViewportV = ViewportHeight * _v;
	const vec3 ViewportPivot = cameraPos - (ViewportU + ViewportV) * 0.5f + cameraForward;

	const vec3 _pixelDeltaU = ViewportU / (float)width;
	const vec3 _pixelDeltaV = ViewportV / (float)height;
	const vec3 _pixel00Dir = ViewportPivot + 0.5f * (_pixelDeltaU + _pixelDeltaV) - cameraPos;

	// Raytracing
	{
		SAILOR_PROFILE_SCOPE("Prepare raytracing tasks");

		TVector<Tasks::ITaskPtr> tasks;
		TVector<Tasks::ITaskPtr> tasksThisThread;

		std::atomic<uint32_t> finishedTasks = 0;
		const uint32_t numTasksX = (width + DefaultGroupSize - 1) / DefaultGroupSize;
		const uint32_t numTasksY = (height + DefaultGroupSize - 1) / DefaultGroupSize;
		const uint32_t numTasks = std::max(1u, numTasksX * numTasksY);

		tasks.Reserve(numTasks);
		tasksThisThread.Reserve(numTasks / 32);

		for (uint32_t y = 0; y < height; y += DefaultGroupSize)
		{
			for (uint32_t x = 0; x < width; x += DefaultGroupSize)
			{
				auto task = Tasks::CreateTask("Calculate raytracing",
					[=,
					&finishedTasks,
					&outputTex,
					&alphaTex,
					this]() mutable
					{
						Ray ray;
						ray.SetOrigin(cameraPos);

						for (uint32_t v = 0; (v < DefaultGroupSize) && (y + v) < height; v++)
						{
							for (uint32_t u = 0; u < DefaultGroupSize && (u + x) < width; u++)
							{
								vec3 accumulator = vec3(0);
								float alphaCoverage = 0.0f;
								for (uint32_t sample = 0; sample < params.m_msaa; sample++)
								{
									uint32_t randomState = NextRandomU32();
									const vec2 offset = sample == 0 ?
										vec2(0.5f, 0.5f) :
										NextRandomVec2_01(randomState);
									const vec3 pixelDir = _pixel00Dir + ((float)(u + x) + offset.x) * _pixelDeltaU + ((float)(y + v) - offset.y) * _pixelDeltaV;

									ray.SetDirection(glm::normalize(pixelDir));
									TLASHit primaryHit{};
									alphaCoverage += IntersectScene(ray, primaryHit, std::numeric_limits<float>::max(), (uint32_t)(-1), (uint32_t)(-1)) ? 1.0f : 0.0f;

									accumulator += Raytrace(ray, params.m_maxBounces, (uint32_t)(-1), (uint32_t)(-1), std::numeric_limits<float>::max(), params, 1.0f, randomState, true);
								}

								vec3 res = accumulator / (float)params.m_msaa;
								outputTex.SetPixel(x + u, height - (y + v) - 1, res);
								alphaTex.SetPixel(x + u, height - (y + v) - 1, alphaCoverage / (float)params.m_msaa);
								}
							}

						finishedTasks++;

					}, EThreadType::Worker);

				if (((x + y) / DefaultGroupSize) % 32 == 0)
				{
					tasksThisThread.Emplace(task);
				}
				else
				{
					task->Run();
					tasks.Emplace(std::move(task));
				}
			}
		}

		float lastPrg = 0.0f;
		{
			SAILOR_PROFILE_SCOPE("Calcs on Main thread");
			float eta = 0.0f;
			for (auto& task : tasksThisThread)
			{
				task->Execute();

				const float progress = finishedTasks.load() / (float)numTasks;

				if (progress - lastPrg > 0.05f)
				{
					if (eta == 0.0f)
					{
						eta = raytracingTimer.ResultAccumulatedMs() * 20.0f * 0.001f * 1.5f;
						SAILOR_LOG("PathTracer ETA: ~%.2fsec (%.2fmin)", eta, round(eta / 60.0f));
					}

					SAILOR_LOG("PathTracer Progress: %.2f", progress);
					lastPrg = progress;
				}
			}
		}

		{
			SAILOR_PROFILE_SCOPE("Wait all calcs");
			for (auto& task : tasks)
			{
				if (!task->IsFinished())
				{
					task->Wait();

					const float progress = finishedTasks.load() / (float)numTasks;
					if (progress - lastPrg > 0.05f)
					{
						SAILOR_LOG("PathTracer Progress: %.2f", progress);
						lastPrg = progress;
					}
				}
			}
		}
	}

	raytracingTimer.Stop();
	m_lastRaytraceTimeMs = static_cast<double>(raytracingTimer.ResultAccumulatedMs());

	{
		SAILOR_PROFILE_SCOPE("Write Image");

		TVector<u8vec4> outSrgb(width * height);
		const float aberrationAmount = (0.5f / width);

		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				vec2 uv = vec2((float)x / width, (float)y / height);

				// Fetch the color values from the offset positions
				vec3 greenColor = outputTex.Sample<vec3>(uv + vec2(aberrationAmount, 0));
				vec3 blueColor = outputTex.Sample<vec3>(uv + vec2(aberrationAmount, aberrationAmount));
				vec3 redColor = outputTex.Sample<vec3>(uv + vec2(-aberrationAmount, -aberrationAmount));

				// Combine the shifted channels
				vec3 chromaAberratedColor = vec3(redColor.r, greenColor.g, blueColor.b);
				const float alpha = glm::clamp(alphaTex.Sample<float>(uv), 0.0f, 1.0f);
				const u8vec3 rgb = alpha > 0.0f ?
					u8vec3(glm::clamp(Utils::LinearToSRGB(chromaAberratedColor) * 255.0f, 0.0f, 255.0f)) :
					u8vec3(0, 0, 0);

				// Write the result back to the output array
				outSrgb[x + y * width] = u8vec4(rgb, (uint8_t)glm::round(alpha * 255.0f));
			}
		}

		m_lastRenderedImage = outSrgb;
		m_lastRenderedExtent = glm::uvec2(width, height);

		if (!params.m_output.empty())
		{
			const uint32_t Channels = 4;
			if (!stbi_write_png(params.m_output.string().c_str(), width, height, Channels, outSrgb.GetData(), width * Channels))
			{
				SAILOR_LOG("Raytracing WriteImage error");
			}
		}
	}
}

bool PathTracer::IntersectSceneGeometry(const Math::Ray& worldRay,
	TLASHit& outHit,
	float maxRayLength,
	uint32_t ignoreInstance,
	uint32_t ignoreTriangle) const
{
	outHit = TLASHit{};

	if (m_tlasInstances.Num() > 0)
	{
		TLASHit bestHit{};
		float bestDistance = maxRayLength;

		m_tlasOctree.TraceRay(worldRay, maxRayLength, [&](size_t idx)
		{
			if (idx >= m_tlasInstances.Num())
			{
				return;
			}
			const auto& instance = m_tlasInstances[idx];
			const Raytracing::BVH* blas = ResolveInstanceBlas(instance);
			const TVector<Math::Triangle>* triangles =
				ResolveInstanceTriangles(instance);
			if (!blas || !triangles || !instance.m_worldBounds.IsValid())
			{
				return;
			}

			if (Math::IntersectRayAABB(worldRay, instance.m_worldBounds.m_min, instance.m_worldBounds.m_max, bestDistance) == FLT_MAX)
			{
				return;
			}

			const vec3 localOrigin = vec3(instance.m_inverseWorldMatrix * vec4(worldRay.GetOrigin(), 1.0f));
			const vec3 localDirRaw = vec3(instance.m_inverseWorldMatrix * vec4(worldRay.GetDirection(), 0.0f));
			if (glm::length(localDirRaw) <= 1e-8f)
			{
				return;
			}

			Math::Ray localRay(localOrigin, glm::normalize(localDirRaw));
			Math::RaycastHit localHit{};
			const uint32_t ignoredLocalTriangle = idx == ignoreInstance ?
				ignoreTriangle : (uint32_t)-1;
			if (!blas->IntersectBVH(
					localRay,
					localHit,
					0,
					FLT_MAX,
					ignoredLocalTriangle))
			{
				return;
			}

			const vec3 worldPoint = vec3(instance.m_worldMatrix * vec4(localHit.m_point, 1.0f));
			const float worldDistance = glm::length(worldPoint - worldRay.GetOrigin());
			if (worldDistance >= bestDistance || worldDistance > maxRayLength)
			{
				return;
			}

			if (localHit.m_triangleIndex >= triangles->Num())
			{
				return;
			}
			const auto& localTri = (*triangles)[localHit.m_triangleIndex];
			const glm::mat3 normalMatrix = glm::mat3(glm::transpose(instance.m_inverseWorldMatrix));
			const vec3 localNormal = localHit.m_barycentricCoordinate.x * localTri.m_normals[0] +
				localHit.m_barycentricCoordinate.y * localTri.m_normals[1] +
				localHit.m_barycentricCoordinate.z * localTri.m_normals[2];

			bestHit.m_hit = localHit;
			bestHit.m_hit.m_point = worldPoint;
			bestHit.m_hit.m_normal = glm::normalize(normalMatrix * localNormal);
			bestHit.m_hit.m_rayLenght = worldDistance;
			const vec3 worldVertex0 = vec3(
				instance.m_worldMatrix * vec4(localTri.m_vertices[0], 1.0f));
			const vec3 worldVertex1 = vec3(
				instance.m_worldMatrix * vec4(localTri.m_vertices[1], 1.0f));
			const vec3 worldVertex2 = vec3(
				instance.m_worldMatrix * vec4(localTri.m_vertices[2], 1.0f));
			bestHit.m_geometricNormal = Math::SafeNormalize(
				glm::cross(
					worldVertex1 - worldVertex0,
					worldVertex2 - worldVertex0),
				bestHit.m_hit.m_normal);
			bestHit.m_instanceIndex = (uint32_t)idx;
			bestHit.m_triangleIndex = localHit.m_triangleIndex;
			bestHit.m_materialIndex = ResolveMaterialIndex(bestHit);
			bestDistance = worldDistance;
		});

		if (bestHit.m_triangleIndex != (uint32_t)-1)
		{
			outHit = bestHit;
			return true;
		}

		return false;
	}

	return false;
}

bool PathTracer::IntersectScene(const Math::Ray& worldRay,
	TLASHit& outHit,
	float maxRayLength,
	uint32_t ignoreInstance,
	uint32_t ignoreTriangle) const
{
	outHit = TLASHit{};
	const vec3 direction = worldRay.GetDirection();
	float traveledDistance = 0.0f;
	constexpr uint32_t MaxMaskedIntersections = 256u;

	for (uint32_t intersectionIndex = 0u;
		intersectionIndex < MaxMaskedIntersections;
		++intersectionIndex)
	{
		const float remainingDistance = std::isfinite(maxRayLength) ?
			maxRayLength - traveledDistance : maxRayLength;
		if (remainingDistance <= 0.0f)
		{
			return false;
		}

		const vec3 currentOrigin =
			worldRay.GetOrigin() + direction * traveledDistance;
		TLASHit candidate{};
		if (!IntersectSceneGeometry(
				Math::Ray(currentOrigin, direction),
				candidate,
				remainingDistance,
				ignoreInstance,
				ignoreTriangle))
		{
			return false;
		}

		const uint32_t materialIndex = ResolveMaterialIndex(candidate);
		const Material& material = m_materials[materialIndex];
		bool bAcceptHit = material.m_blendMode != BlendMode::Mask;
		if (!bAcceptHit)
		{
			const Math::Triangle& triangle = GetTriangle(candidate);
			const vec2 uv = ResolveHitTextureCoordinates(
				triangle,
				candidate.m_hit);
			const vec2 transformedUv = glm::vec2(
				material.m_uvTransform * glm::vec3(uv, 1.0f));
			const glm::vec4 layerWeights =
				candidate.m_hit.m_barycentricCoordinate.x *
					triangle.m_colors[0] +
				candidate.m_hit.m_barycentricCoordinate.y *
					triangle.m_colors[1] +
				candidate.m_hit.m_barycentricCoordinate.z *
					triangle.m_colors[2];
			const glm::vec4 baseColor = SampleMaterialBaseColor(
				materialIndex,
				transformedUv,
				layerWeights);
			bAcceptHit = baseColor.a >= material.m_alphaCutoff;
		}

		const float localDistance = candidate.m_hit.m_rayLenght;
		if (bAcceptHit)
		{
			candidate.m_hit.m_rayLenght =
				traveledDistance + localDistance;
			outHit = candidate;
			return true;
		}

		const vec3 absolutePoint = glm::abs(candidate.m_hit.m_point);
		const float coordinateScale = (std::max)({
			absolutePoint.x,
			absolutePoint.y,
			absolutePoint.z,
			1.0f });
		const float advance = (std::max)(
			1e-5f,
			coordinateScale * std::numeric_limits<float>::epsilon() * 8.0f);
		traveledDistance += localDistance + advance;
	}

	return false;
}

float PathTracer::TraceDirectLightTransmittance(
	const Math::Ray& worldRay,
	float maxRayLength,
	uint32_t ignoreInstance,
	uint32_t ignoreTriangle) const
{
	const vec3 direction = worldRay.GetDirection();
	float transmittance = 1.0f;
	float traveledDistance = 0.0f;
	constexpr uint32_t MaxBlendedIntersections = 256u;

	for (uint32_t intersectionIndex = 0u;
		intersectionIndex < MaxBlendedIntersections;
		++intersectionIndex)
	{
		const float remainingDistance = std::isfinite(maxRayLength) ?
			maxRayLength - traveledDistance : maxRayLength;
		if (remainingDistance <= 0.0f)
		{
			return transmittance;
		}

		const vec3 currentOrigin =
			worldRay.GetOrigin() + direction * traveledDistance;
		TLASHit candidate{};
		if (!IntersectScene(
				Math::Ray(currentOrigin, direction),
				candidate,
				remainingDistance,
				ignoreInstance,
				ignoreTriangle))
		{
			return transmittance;
		}

		const uint32_t materialIndex = ResolveMaterialIndex(candidate);
		const Material& material = m_materials[materialIndex];
		if (material.m_blendMode != BlendMode::Blend)
		{
			return 0.0f;
		}

		const Math::Triangle& triangle = GetTriangle(candidate);
		const vec2 uv = ResolveHitTextureCoordinates(
			triangle,
			candidate.m_hit);
		const vec2 transformedUv = glm::vec2(
			material.m_uvTransform * glm::vec3(uv, 1.0f));
		const glm::vec4 layerWeights =
			candidate.m_hit.m_barycentricCoordinate.x *
				triangle.m_colors[0] +
			candidate.m_hit.m_barycentricCoordinate.y *
				triangle.m_colors[1] +
			candidate.m_hit.m_barycentricCoordinate.z *
				triangle.m_colors[2];
		const float alpha = glm::clamp(
			SampleMaterialBaseColor(
				materialIndex,
				transformedUv,
				layerWeights).a,
			0.0f,
			1.0f);
		transmittance *= 1.0f - alpha;
		if (transmittance <= 0.0001f)
		{
			return 0.0f;
		}

		const float localDistance = candidate.m_hit.m_rayLenght;
		const vec3 absolutePoint = glm::abs(candidate.m_hit.m_point);
		const float coordinateScale = (std::max)({
			absolutePoint.x,
			absolutePoint.y,
			absolutePoint.z,
			1.0f });
		const float advance = (std::max)(
			1e-5f,
			coordinateScale * std::numeric_limits<float>::epsilon() * 8.0f);
		traveledDistance += localDistance + advance;
	}

	return 0.0f;
}

const Math::Triangle& PathTracer::GetTriangle(const TLASHit& hit) const
{
	const auto& instance = m_tlasInstances[hit.m_instanceIndex];
	return (*ResolveInstanceTriangles(instance))[hit.m_triangleIndex];
}

void PathTracer::GetShadingBasis(const TLASHit& hit, vec3& outNormal, vec3& outTangent, vec3& outBitangent) const
{
	const auto& tri = GetTriangle(hit);

	const vec3 localNormal = hit.m_hit.m_barycentricCoordinate.x * tri.m_normals[0] +
		hit.m_hit.m_barycentricCoordinate.y * tri.m_normals[1] +
		hit.m_hit.m_barycentricCoordinate.z * tri.m_normals[2];
	const vec3 localTangent = hit.m_hit.m_barycentricCoordinate.x * tri.m_tangent[0] +
		hit.m_hit.m_barycentricCoordinate.y * tri.m_tangent[1] +
		hit.m_hit.m_barycentricCoordinate.z * tri.m_tangent[2];
	const vec3 localBitangent = hit.m_hit.m_barycentricCoordinate.x * tri.m_bitangent[0] +
		hit.m_hit.m_barycentricCoordinate.y * tri.m_bitangent[1] +
		hit.m_hit.m_barycentricCoordinate.z * tri.m_bitangent[2];

	const auto& instance = m_tlasInstances[hit.m_instanceIndex];
	const glm::mat3 linearMatrix = glm::mat3(instance.m_worldMatrix);
	const glm::mat3 normalMatrix = glm::mat3(glm::transpose(instance.m_inverseWorldMatrix));
	outNormal = Math::SafeNormalize(
		normalMatrix * localNormal,
		Math::vec3_Up);

	const vec3 transformedTangent = linearMatrix * localTangent;
	outTangent = Math::SafeNormalize(
		transformedTangent - outNormal * glm::dot(outNormal, transformedTangent));
	if (outTangent == Math::vec3_Zero)
	{
		const vec3 fallbackAxis = std::abs(outNormal.y) < 0.999f ?
			Math::vec3_Up : Math::vec3_Right;
		outTangent = Math::SafeNormalize(
			glm::cross(fallbackAxis, outNormal),
			Math::vec3_Right);
	}

	const vec3 transformedBitangent = linearMatrix * localBitangent;
	const vec3 canonicalBitangent = Math::SafeNormalize(
		glm::cross(outNormal, outTangent),
		Math::vec3_Forward);
	const float handedness =
		glm::dot(canonicalBitangent, transformedBitangent) < 0.0f ?
		-1.0f : 1.0f;
	outBitangent = canonicalBitangent * handedness;
}

bool PathTracer::OrientShadingBasisToGeometricSurface(
	const vec3& rayDirection,
	const vec3& geometricNormal,
	vec3& inOutNormal,
	vec3& inOutBitangent,
	vec3& outOrientedGeometricNormal)
{
	outOrientedGeometricNormal = Math::SafeNormalize(
		geometricNormal,
		inOutNormal);
	const bool bFrontFace =
		glm::dot(outOrientedGeometricNormal, rayDirection) < 0.0f;
	if (!bFrontFace)
	{
		outOrientedGeometricNormal *= -1.0f;
	}

	// Winding determines the physical side of a triangle. Imported vertex
	// normals may be smoothed, mirrored, or malformed and must not change face
	// culling, volume entry/exit, or one-sided emission.
	if (glm::dot(inOutNormal, outOrientedGeometricNormal) < 0.0f)
	{
		inOutNormal *= -1.0f;
		inOutBitangent *= -1.0f;
	}

	return bFrontFace;
}

uint32_t PathTracer::ResolveMaterialIndex(const TLASHit& hit) const
{
	if (m_materials.Num() == 0)
	{
		return 0;
	}

	const auto& instance = m_tlasInstances[hit.m_instanceIndex];
	const auto& tri =
		(*ResolveInstanceTriangles(instance))[hit.m_triangleIndex];
	const int32_t idx = instance.m_materialBaseOffset + (int32_t)tri.m_materialIndex;
	return (uint32_t)(std::max)(0, (std::min)(idx, (int32_t)m_materials.Num() - 1));
}

bool PathTracer::IsThickVolumeAtHit(
	const TLASHit& hit,
	uint32_t materialIndex) const
{
	const auto& material = m_materials[materialIndex];
	const vec2 uv = ResolveHitTextureCoordinates(
		GetTriangle(hit),
		hit.m_hit);
	const vec2 uvTransformed =
		material.m_uvTransform * vec3(uv, 1.0f);

	float transmission = material.m_transmissionFactor;
	if (material.HasTransmissionTexture())
	{
		transmission *=
			m_textures[material.m_transmissionIndex]->Sample<vec3>(
				uvTransformed).r;
	}

	if (transmission <= 0.0f)
	{
		return false;
	}

	return material.m_thicknessFactor > 0.0f;
}

void PathTracer::AppendEmissiveTriangles(uint32_t instanceIndex)
{
	if (instanceIndex >= m_tlasInstances.Num())
	{
		return;
	}

	const TLASInstance& instance = m_tlasInstances[instanceIndex];
	const TVector<Math::Triangle>* triangles =
		ResolveInstanceTriangles(instance);
	if (!triangles)
	{
		return;
	}

	for (uint32_t triangleIndex = 0u;
		triangleIndex < triangles->Num();
		++triangleIndex)
	{
		const Math::Triangle& triangle = (*triangles)[triangleIndex];
		const int64_t materialIndex =
			static_cast<int64_t>(instance.m_materialBaseOffset) +
			static_cast<int64_t>(triangle.m_materialIndex);
		if (materialIndex < 0 ||
			materialIndex >= static_cast<int64_t>(m_materials.Num()) ||
			materialIndex >= static_cast<int64_t>(
				m_resolvedMaterialSlots.Num()) ||
			m_resolvedMaterialSlots[static_cast<size_t>(materialIndex)] == 0u)
		{
			continue;
		}

		const glm::vec3 emissiveFactor = glm::max(
			m_materials[static_cast<size_t>(materialIndex)].m_emissiveFactor,
			glm::vec3(0.0f));
		const float emissivePower = glm::dot(
			emissiveFactor,
			glm::vec3(0.2126f, 0.7152f, 0.0722f));
		if (!std::isfinite(emissivePower) || emissivePower <= 0.0f)
		{
			continue;
		}

		EmissiveTriangle source;
		for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
		{
			source.m_vertices[vertexIndex] = glm::vec3(
				instance.m_worldMatrix *
					glm::vec4(triangle.m_vertices[vertexIndex], 1.0f));
			source.m_uvs[vertexIndex] = triangle.m_uvs[vertexIndex];
			source.m_colors[vertexIndex] = triangle.m_colors[vertexIndex];
		}
		const glm::vec3 areaVector = glm::cross(
			source.m_vertices[1] - source.m_vertices[0],
			source.m_vertices[2] - source.m_vertices[0]);
		source.m_area = glm::length(areaVector) * 0.5f;
		if (!std::isfinite(source.m_area) || source.m_area <= 1e-8f)
		{
			continue;
		}

		source.m_instanceIndex = instanceIndex;
		source.m_triangleIndex = triangleIndex;
		source.m_materialIndex = static_cast<uint32_t>(materialIndex);

		const Material& material = m_materials[source.m_materialIndex];
		constexpr glm::vec3 WeightSamples[] = {
			glm::vec3(1.0f / 3.0f),
			glm::vec3(0.6f, 0.2f, 0.2f),
			glm::vec3(0.2f, 0.6f, 0.2f),
			glm::vec3(0.2f, 0.2f, 0.6f)
		};
		constexpr float WeightSampleCount = 4.0f;
		float sampledEmissivePower = 0.0f;
		for (const glm::vec3& barycentric : WeightSamples)
		{
			const glm::vec2 uv =
				barycentric.x * source.m_uvs[0] +
				barycentric.y * source.m_uvs[1] +
				barycentric.z * source.m_uvs[2];
			const glm::vec2 transformedUv = glm::vec2(
				material.m_uvTransform * glm::vec3(uv, 1.0f));
			const glm::vec4 layerWeights =
				barycentric.x * source.m_colors[0] +
				barycentric.y * source.m_colors[1] +
				barycentric.z * source.m_colors[2];
			const LightingModel::SampledData sample = GetMaterialData(
				source.m_materialIndex,
				transformedUv,
				layerWeights);
			const float alpha = sample.m_bIsOpaque ?
				1.0f : sample.m_baseColor.a;
			sampledEmissivePower += glm::dot(
				glm::max(sample.m_emissive * alpha, glm::vec3(0.0f)),
				glm::vec3(0.2126f, 0.7152f, 0.0722f));
		}
		sampledEmissivePower /= WeightSampleCount;
		const float conservativeWeightFloor = emissivePower * 0.01f;
		source.m_weight = source.m_area * (std::max)(
			sampledEmissivePower,
			conservativeWeightFloor);
		m_totalEmissiveWeight += source.m_weight;
		source.m_cumulativeWeight = m_totalEmissiveWeight;
		m_emissiveTriangles.Add(std::move(source));
	}
}

vec3 PathTracer::SampleDirectEmissive(
	const TLASHit& receiverHit,
	const LightingModel::SampledData& receiverMaterial,
	const vec3& viewDirection,
	const vec3& worldNormal,
	float fromIor,
	float toIor,
	const Params& params,
	uint32_t& randomState) const
{
	if (m_emissiveTriangles.IsEmpty() ||
		!std::isfinite(m_totalEmissiveWeight) ||
		m_totalEmissiveWeight <= 0.0f)
	{
		return vec3(0.0f);
	}

	const float selectedWeight =
		NextRandom01(randomState) * m_totalEmissiveWeight;
	const auto selectedIt = std::lower_bound(
		m_emissiveTriangles.begin(),
		m_emissiveTriangles.end(),
		selectedWeight,
		[](const EmissiveTriangle& source, float value)
		{
			return source.m_cumulativeWeight < value;
		});
	const EmissiveTriangle& source = selectedIt !=
		m_emissiveTriangles.end() ?
		*selectedIt :
			m_emissiveTriangles[m_emissiveTriangles.Num() - 1u];
	if (source.m_instanceIndex == receiverHit.m_instanceIndex &&
		source.m_triangleIndex == receiverHit.m_triangleIndex)
	{
		return vec3(0.0f);
	}

	const vec2 randomSample = NextRandomVec2_01(randomState);
	const float sqrtSample = std::sqrt(randomSample.x);
	const glm::vec3 barycentric(
		1.0f - sqrtSample,
		sqrtSample * (1.0f - randomSample.y),
		sqrtSample * randomSample.y);
	const glm::vec3 target =
		barycentric.x * source.m_vertices[0] +
		barycentric.y * source.m_vertices[1] +
		barycentric.z * source.m_vertices[2];
	const glm::vec3 receiverToTarget = target - receiverHit.m_hit.m_point;
	const float distanceSquared = glm::dot(
		receiverToTarget,
		receiverToTarget);
	if (!std::isfinite(distanceSquared) || distanceSquared <= 1e-8f)
	{
		return vec3(0.0f);
	}

	const float distance = std::sqrt(distanceSquared);
	const glm::vec3 direction = receiverToTarget / distance;
	const glm::vec3 emitterAreaVector = glm::cross(
		source.m_vertices[1] - source.m_vertices[0],
		source.m_vertices[2] - source.m_vertices[0]);
	const float emitterAreaVectorLength = glm::length(emitterAreaVector);
	if (emitterAreaVectorLength <= 1e-8f)
	{
		return vec3(0.0f);
	}
	const glm::vec3 emitterNormal =
		emitterAreaVector / emitterAreaVectorLength;
	const float signedEmitterCosine =
		glm::dot(emitterNormal, -direction);
	if (!IsMaterialFaceVisible(
			m_materials[source.m_materialIndex],
			signedEmitterCosine >= 0.0f))
	{
		return vec3(0.0f);
	}
	const float emitterCosine = std::abs(signedEmitterCosine);
	if (emitterCosine <= 1e-6f)
	{
		return vec3(0.0f);
	}

	const float selectionProbability =
		source.m_weight / m_totalEmissiveWeight;
	const float solidAnglePdf =
		selectionProbability / source.m_area *
		distanceSquared / emitterCosine;
	if (!std::isfinite(solidAnglePdf) || solidAnglePdf <= 1e-8f)
	{
		return vec3(0.0f);
	}

	const Material& emitterMaterial =
		m_materials[source.m_materialIndex];
	const glm::vec2 uv =
		barycentric.x * source.m_uvs[0] +
		barycentric.y * source.m_uvs[1] +
		barycentric.z * source.m_uvs[2];
	const glm::vec2 transformedUv = glm::vec2(
		emitterMaterial.m_uvTransform * glm::vec3(uv, 1.0f));
	const glm::vec4 layerWeights =
		barycentric.x * source.m_colors[0] +
		barycentric.y * source.m_colors[1] +
		barycentric.z * source.m_colors[2];
	LightingModel::SampledData emitterSample = GetMaterialData(
		source.m_materialIndex,
		transformedUv,
		layerWeights);
	TLASHit emitterHit;
	emitterHit.m_instanceIndex = source.m_instanceIndex;
	emitterHit.m_triangleIndex = source.m_triangleIndex;
	emitterHit.m_geometricNormal = emitterNormal;
	emitterHit.m_hit.m_barycentricCoordinate = barycentric;
	vec3 emitterVertexNormal{};
	vec3 emitterTangent{};
	vec3 emitterBitangent{};
	GetShadingBasis(
		emitterHit,
		emitterVertexNormal,
		emitterTangent,
		emitterBitangent);
	vec3 orientedEmitterGeometricNormal{};
	OrientShadingBasisToGeometricSurface(
		direction,
		emitterNormal,
		emitterVertexNormal,
		emitterBitangent,
		orientedEmitterGeometricNormal);
	const mat3 emitterTangentBasis(
		emitterTangent,
		emitterBitangent,
		emitterVertexNormal);
	const vec3 emitterWorldNormal = ResolveWorldShadingNormal(
		emitterTangentBasis,
		emitterSample.m_normal,
		emitterVertexNormal,
		orientedEmitterGeometricNormal);
	emitterSample.m_clearcoatNormal = ResolveWorldShadingNormal(
		emitterTangentBasis,
		emitterSample.m_clearcoatNormal,
		emitterVertexNormal,
		orientedEmitterGeometricNormal);
	const float alpha = emitterSample.m_bIsOpaque ?
		1.0f : emitterSample.m_baseColor.a;
	const glm::vec3 emittedRadiance = glm::max(
		LightingModel::CalculateEmittedRadiance(
			emitterSample,
			emitterWorldNormal,
			-direction) * alpha,
		glm::vec3(0.0f));
	if (glm::dot(emittedRadiance, emittedRadiance) <= 0.0f)
	{
		return vec3(0.0f);
	}

	const glm::vec3 visibilityOrigin = OffsetRayOrigin(
		receiverHit.m_hit.m_point,
		receiverHit.m_geometricNormal,
		direction,
		params.m_rayBiasBase,
		params.m_rayBiasScale);
	const glm::vec3 visibilityVector = target - visibilityOrigin;
	const float visibilityDistance = glm::length(visibilityVector);
	if (!std::isfinite(visibilityDistance) || visibilityDistance <= 1e-5f)
	{
		return vec3(0.0f);
	}
	const float endpointTolerance = (std::max)(
		1e-4f,
		visibilityDistance * 1e-4f);
	Math::Ray visibilityRay(
		visibilityOrigin,
		visibilityVector / visibilityDistance);
	const float transmittance = TraceDirectLightTransmittance(
		visibilityRay,
		(std::max)(0.0f, visibilityDistance - endpointTolerance),
		receiverHit.m_instanceIndex,
		receiverHit.m_triangleIndex);
	if (transmittance <= 0.0f)
	{
		return vec3(0.0f);
	}

	return LightingModel::CalculateBRDFCosineWeighted(
		viewDirection,
		worldNormal,
		direction,
		receiverMaterial,
		fromIor,
		toIor) * emittedRadiance *
		transmittance / solidAnglePdf;
}

vec3 PathTracer::Raytrace(
	const Math::Ray& ray,
	uint32_t bounceLimit,
	uint32_t ignoreInstance,
	uint32_t ignoreTriangle,
	float maxRayDistance,
	const PathTracer::Params& params,
	float environmentIor,
	uint32_t& randomState,
	bool bAllowEmissiveHit) const
{

	uint32_t randSeedX = NextRandomRange(randomState, 681u);
	uint32_t randSeedY = NextRandomRange(randomState, 681u);

	vec3 res = vec3(0);
	TLASHit hit{};
	const bool bHitScene = IntersectScene(
		ray,
		hit,
		maxRayDistance,
		ignoreInstance,
		ignoreTriangle);
	if (bHitScene)
	{
		const bool bIsFirstIntersection = bounceLimit == params.m_maxBounces;

		const Math::Triangle& tri = GetTriangle(hit);
		vec3 faceNormal{}, tangent{}, bitangent{};
		GetShadingBasis(hit, faceNormal, tangent, bitangent);

		vec3 orientedGeometricNormal{};
		const bool bFrontFace = OrientShadingBasisToGeometricSurface(
			ray.GetDirection(),
			hit.m_geometricNormal,
			faceNormal,
			bitangent,
			orientedGeometricNormal);

		const mat3 tbn(tangent, bitangent, faceNormal);

		const vec2 uv = ResolveHitTextureCoordinates(tri, hit.m_hit);

		const uint32_t materialIndex = ResolveMaterialIndex(hit);
		const auto material = m_materials[materialIndex];
		const vec2 uvTransformed = (material.m_uvTransform * vec3(uv, 1));

		const vec4 layerWeights =
			hit.m_hit.m_barycentricCoordinate.x * tri.m_colors[0] +
			hit.m_hit.m_barycentricCoordinate.y * tri.m_colors[1] +
			hit.m_hit.m_barycentricCoordinate.z * tri.m_colors[2];
		LightingModel::SampledData sample = GetMaterialData(
			materialIndex,
			uvTransformed,
			layerWeights);
		const vec3 viewDirection = -normalize(ray.GetDirection());
		const vec3 worldNormal = ResolveWorldShadingNormal(
			tbn,
			sample.m_normal,
			faceNormal,
			orientedGeometricNormal);
		sample.m_clearcoatNormal = ResolveWorldShadingNormal(
			tbn,
			sample.m_clearcoatNormal,
			faceNormal,
			orientedGeometricNormal);

		const bool bHasAlphaBlending = !sample.m_bIsOpaque && sample.m_baseColor.a < 1.0f;
		const uint32_t numSamples = bHasAlphaBlending ? std::max(1u, (uint32_t)round(sample.m_baseColor.a * (float)params.m_numSamples)) : params.m_numSamples;
		const uint32_t numAmbientSamples = bHasAlphaBlending ? std::max(1u, (uint32_t)round(sample.m_baseColor.a * (float)params.m_numAmbientSamples)) : params.m_numAmbientSamples;

		const bool bFullMetallic = sample.m_orm.z == 1.0f;
		const bool bOnlySpecularRay =
			bFullMetallic && sample.m_orm.y <= 0.001f;
		const bool bHasTransmission = !bFullMetallic && sample.m_transmission > 0.0f;
		const bool bThickVolume = bHasTransmission && material.m_thicknessFactor > 0.0f;
		const float interfaceFromIor = bThickVolume ?
			environmentIor : 1.0f;
		const float interfaceToIor = bThickVolume ?
			(bFrontFace ? sample.m_ior : 1.0f) : sample.m_ior;

		// Analytic local-light range is a surface-lighting cutoff, not emissive
		// geometry in otherwise empty space.
		if (params.m_bIncludeDirectLighting)
		{
			for (uint32_t i = 0; i < m_lightProxies.Num(); i++)
			{
				vec3 toLight{};
				vec3 radiance{};
				float maxDistanceToLight = std::numeric_limits<float>::max();

				if (!EvaluateDirectLight(m_lightProxies[i], hit.m_hit.m_point, toLight, radiance, maxDistanceToLight))
				{
					continue;
				}

				float directTransmittance = 1.0f;
				if (m_lightProxies[i].m_bCastShadows)
				{
					const vec3 visibilityOrigin = OffsetRayOrigin(
						hit.m_hit.m_point,
						hit.m_geometricNormal,
						toLight,
						params.m_rayBiasBase,
						params.m_rayBiasScale);
					vec3 visibilityDirection = toLight;
					float visibilityDistance = maxDistanceToLight;
					if (m_lightProxies[i].m_type != ELightType::Directional)
					{
						const vec3 visibilityVector =
							m_lightProxies[i].m_worldPosition - visibilityOrigin;
						const float distance = glm::length(visibilityVector);
						if (!std::isfinite(distance) || distance <= 1e-5f)
						{
							directTransmittance = 0.0f;
						}
						else
						{
							visibilityDirection = visibilityVector / distance;
							const float endpointTolerance = (std::max)(
								1e-4f,
								distance * 1e-4f);
							visibilityDistance = (std::max)(
								0.0f,
								distance - endpointTolerance);
						}
					}
					if (directTransmittance <= 0.0f)
					{
						continue;
					}
					Ray rayToLight(
						visibilityOrigin,
						visibilityDirection);
					directTransmittance = TraceDirectLightTransmittance(
						rayToLight,
						visibilityDistance,
						hit.m_instanceIndex,
						hit.m_triangleIndex);
				}
				if (directTransmittance > 0.0f)
				{
					res += LightingModel::CalculateBRDFCosineWeighted(
						viewDirection,
						worldNormal,
						toLight,
						sample,
						interfaceFromIor,
						interfaceToIor) * radiance *
						directTransmittance;
				}
			}
		}
		if (params.m_bIncludeEmissive && !bOnlySpecularRay)
		{
			res += SampleDirectEmissive(
				hit,
				sample,
				viewDirection,
				worldNormal,
				interfaceFromIor,
				interfaceToIor,
				params,
				randomState);
		}

		// Ambient / environment lighting and recursive indirect bounces.
		// Recursive bounces must remain available when the environment is disabled:
		// a bake may intentionally capture only lights and emissive materials.
		const bool bHasEnvironmentLighting = params.m_bIncludeEnvironment &&
			(params.m_ambient.x + params.m_ambient.y + params.m_ambient.z > 0.0f ||
				m_bHasRuntimeEnvironment || m_bHasRuntimeDiffuseEnvironment);
		if (bHasEnvironmentLighting || bounceLimit > 0)
		{
			// Random ray
			vec3 ambient1 = vec3(0, 0, 0);

			const uint32_t ambientNumSamples = bIsFirstIntersection ? numAmbientSamples : 1u;
			const uint32_t numExtraSamples = bIsFirstIntersection ? numSamples : 1;

			// Hemisphere sampling loop
			if (bHasEnvironmentLighting &&
				!bThickVolume &&
				!bOnlySpecularRay)
			{
				for (uint32_t i = 0; i < ambientNumSamples; i++)
				{
					vec3 toLight{};
					float environmentPdf = 0.0f;
					if (!SampleDirectEnvironment(
							worldNormal,
							randomState,
							toLight,
							environmentPdf))
					{
						continue;
					}
					const float angle = glm::max(
						0.0f,
						glm::dot(toLight, worldNormal));
					if (angle <= 0.0f)
					{
						continue;
					}
					Ray skyRay(OffsetRayOrigin(hit.m_hit.m_point, hit.m_geometricNormal, toLight, params.m_rayBiasBase, params.m_rayBiasScale), toLight);
					const float skyTransmittance =
						TraceDirectLightTransmittance(
							skyRay,
							std::numeric_limits<float>::max(),
							hit.m_instanceIndex,
							hit.m_triangleIndex);
					if (skyTransmittance > 0.0f)
					{
						const vec3 env =
							(m_bHasRuntimeEnvironment ||
								m_bHasRuntimeDiffuseEnvironment) ?
							SampleRuntimeDirectEnvironment(toLight) :
							params.m_ambient;
						const vec3 value = LightingModel::CalculateBRDFCosineWeighted(
							viewDirection,
							worldNormal,
							toLight,
							sample,
							interfaceFromIor,
							interfaceToIor) * env;
						const float bsdfPdf = LightingModel::ReflectionPdf(
							sample,
							worldNormal,
							viewDirection,
							toLight);
						const float misWeight = LightingModel::PowerHeuristic(
							static_cast<int32_t>(ambientNumSamples),
							environmentPdf,
							static_cast<int32_t>(numExtraSamples),
							bsdfPdf);
						ambient1 += SanitizeRadiance(
							(value * skyTransmittance) /
								environmentPdf) * misWeight;
					}
				}
			}

			if (bHasEnvironmentLighting)
			{
				ambient1 /= (float)ambientNumSamples;
			}

			// Importance sampling ray
			vec3 ambient2 = vec3(0, 0, 0);

			// Indirect lighting
			vec3 indirect = vec3(0.0f, 0.0f, 0.0f);
			float indirectContribution = 0.0f;

			// Importance sampling loop
			for (uint32_t i = 0; i < numExtraSamples; i++)
			{
				vec3 term{};
				float pdf = 0.0f;
				bool bTransmissionRay = false;
				vec3 direction = vec3(0);
				const vec2 randomSample = NextVec2_BlueNoise(
					randSeedX,
					randSeedY,
					randomState);
				const vec2 selectionSample = NextRandomVec2_01(randomState);
				const bool bSample = LightingModel::Sample(
					sample,
					worldNormal,
					viewDirection,
					interfaceFromIor,
					interfaceToIor,
					term,
					pdf,
					bTransmissionRay,
					direction,
					randomSample,
					selectionSample);
				if (!bSample)
				{
					// Rejection is a zero-valued sample from the original BSDF
					// distribution. Retrying until a direction is accepted samples a
					// conditional distribution while retaining the original PDF and
					// systematically overestimates indirect lighting.
					indirectContribution += 1.0f;
					continue;
				}

				float newEnvironmentIor = environmentIor;

				if (bFrontFace && bTransmissionRay && bThickVolume)
				{
					newEnvironmentIor = sample.m_ior;
				}
				else if (!bFrontFace && bTransmissionRay && bThickVolume)
				{
					newEnvironmentIor = 1.0f;
				}

				Ray rayToLight(OffsetRayOrigin(hit.m_hit.m_point, hit.m_geometricNormal, direction, params.m_rayBiasBase, params.m_rayBiasScale), direction);

				TLASHit hitLight{};
				if (!IntersectScene(rayToLight, hitLight, std::numeric_limits<float>().max(), hit.m_instanceIndex, hit.m_triangleIndex))
				{
					const vec3 env = bHasEnvironmentLighting ?
						(m_bHasRuntimeEnvironment ? SampleRuntimeEnvironment(direction) :
							(m_bHasRuntimeDiffuseEnvironment ? SampleRuntimeDiffuseEnvironment(direction) : params.m_ambient)) :
						vec3(0.0f);
					const vec3 value = SanitizeRadiance(term * env);
					const float directEnvironmentPdf =
						DirectEnvironmentPdf(worldNormal, direction);
					const bool bEnvironmentTechniqueSupportsDirection =
						bHasEnvironmentLighting &&
						!bThickVolume &&
						!bOnlySpecularRay &&
						!bTransmissionRay &&
						directEnvironmentPdf > 0.0f;
					const float misWeight = bEnvironmentTechniqueSupportsDirection ?
						LightingModel::PowerHeuristic(
							static_cast<int32_t>(numExtraSamples),
							pdf,
							static_cast<int32_t>(ambientNumSamples),
							directEnvironmentPdf) : 1.0f;

					// Ambient lighting
					ambient2 += value * misWeight;

					// A miss reaches the environment directly from this surface.
					// It already belongs to the MIS ambient estimator above; adding
					// it to the recursive estimator would count the same sky path
					// twice.
				}
				else if (bounceLimit > 0)
				{
					// Indirect
					vec3 lightAttenuation = vec3(1, 1, 1);
					if (bFrontFace && bTransmissionRay && bThickVolume)
					{
						const float distance = glm::length(hitLight.m_hit.m_point - hit.m_hit.m_point);
						lightAttenuation = CalculateVolumeAttenuation(
							material.m_attenuationColor,
							material.m_attenuationDistance,
							distance);
					}

					// Non-delta emitter paths are sampled explicitly above. Keeping
					// their BSDF-hit estimator as well would count the same path twice.
					const bool bAllowNextEmissiveHit =
						bOnlySpecularRay || bTransmissionRay ||
						m_emissiveTriangles.IsEmpty();
					const vec3 raytraced = Raytrace(
						rayToLight,
						bounceLimit - 1,
						hit.m_instanceIndex,
						hit.m_triangleIndex,
						std::numeric_limits<float>::max(),
						params,
						newEnvironmentIor,
						randomState,
						bAllowNextEmissiveHit);

					const vec3 value = SanitizeRadiance(
						term * lightAttenuation * raytraced);

					// Indirect lighting with bounces in case of hit
					indirect += value;

				}
				indirectContribution += 1.0f;
			}

			ambient2 /= (float)indirectContribution;
			if (bHasEnvironmentLighting)
			{
				res += ambient1 + ambient2;
			}

			if (indirectContribution > 0.0f)
			{
				res += (indirect / indirectContribution);
			}
		}

		if (params.m_bIncludeEmissive &&
			bAllowEmissiveHit &&
			IsMaterialFaceVisible(material, bFrontFace))
		{
			res += LightingModel::CalculateEmittedRadiance(
				sample,
				worldNormal,
				viewDirection);
		}

		// Alpha Blending
		if (bounceLimit > 0 && bHasAlphaBlending)
		{
			Math::Ray newRay{};
			newRay.SetDirection(ray.GetDirection());
			newRay.SetOrigin(hit.m_hit.m_point + ray.GetDirection() * 0.0001f);

			PathTracer::Params p = params;
			p.m_maxBounces = std::max(0u, params.m_maxBounces - 1);
			p.m_numSamples = std::max(1u, params.m_numSamples - numSamples);
			p.m_numAmbientSamples = std::max(1u, params.m_numAmbientSamples - numAmbientSamples);

			res = res * sample.m_baseColor.a +
				Raytrace(newRay, bounceLimit - 1, hit.m_instanceIndex, hit.m_triangleIndex, std::numeric_limits<float>::max(), p, environmentIor, randomState, bAllowEmissiveHit) * (1.0f - sample.m_baseColor.a);
		}
	}
	else
	{
		if (params.m_bIncludeEnvironment &&
			(m_bHasRuntimeEnvironment || m_bHasRuntimeDiffuseEnvironment))
		{
			res = m_bHasRuntimeEnvironment ? SampleRuntimeEnvironment(ray.GetDirection()) : SampleRuntimeDiffuseEnvironment(ray.GetDirection());
		}
		else
		{
			res = params.m_bIncludeEnvironment ? params.m_ambient : vec3(0.0f);
		}
	}

	return res;
}

glm::vec4 PathTracer::SampleMaterialBaseColor(
	const size_t& materialIndex,
	glm::vec2 uv,
	glm::vec4 layerWeights) const
{
	const auto& material = m_materials[materialIndex];
	glm::vec4 baseColor = material.m_baseColorFactor;

	if (material.HasLayerColorTextures())
	{
		layerWeights = glm::max(layerWeights, glm::vec4(0.0f));
		const float weightSum = glm::dot(layerWeights, glm::vec4(1.0f));
		if (!std::isfinite(weightSum) || weightSum <= 0.0001f)
		{
			layerWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
		}
		else
		{
			layerWeights /= weightSum;
		}

		glm::vec4 layeredColor(0.0f);
		float resolvedWeight = 0.0f;
		for (glm::length_t layer = 0; layer < 4; ++layer)
		{
			if (!material.HasLayerColorTexture(layer) ||
				layerWeights[layer] <= 0.0f)
			{
				continue;
			}
			layeredColor += layerWeights[layer] *
				m_textures[material.m_layerColorIndices[layer]]->Sample<vec4>(
					uv * material.m_layerUvScale[layer]);
			resolvedWeight += layerWeights[layer];
		}
		if (resolvedWeight > 0.0001f)
		{
			baseColor *= layeredColor / resolvedWeight;
		}
	}
	else
	{
		if (material.HasBaseTexture())
		{
			baseColor *=
				m_textures[material.m_baseColorIndex]->Sample<vec4>(uv);
		}
		baseColor *= layerWeights;
	}

	return baseColor;
}

LightingModel::SampledData PathTracer::GetMaterialData(
	const size_t& materialIndex,
	glm::vec2 uv,
	glm::vec4 layerWeights) const
{
	const auto& material = m_materials[materialIndex];

	LightingModel::SampledData res{};
	res.m_baseColor = SampleMaterialBaseColor(
		materialIndex,
		uv,
		layerWeights);
	res.m_normal = vec3(0, 0, 1.0f);
	res.m_orm = vec3(0.0f, material.m_roughnessFactor, material.m_metallicFactor);
	res.m_emissive = material.m_emissiveFactor;
	res.m_clearcoatNormal = vec3(0.0f, 0.0f, 1.0f);
	res.m_clearcoatFactor = material.m_clearcoatFactor;
	res.m_clearcoatRoughness = material.m_clearcoatRoughnessFactor;
	res.m_sheenColor = material.m_sheenColorFactor;
	res.m_sheenRoughness = material.m_sheenRoughnessFactor;
	res.m_transmission = material.m_transmissionFactor;
	res.m_bIsOpaque = material.m_blendMode == BlendMode::Opaque;
	res.m_thicknessFactor = material.m_thicknessFactor;
	res.m_ior = material.m_indexOfRefraction;

	if (material.HasEmissiveTexture())
	{
		res.m_emissive *= m_textures[material.m_emissiveIndex]->Sample<vec3>(uv);
	}

	if (material.HasMetallicRoughnessTexture())
	{
		const vec3 ormSample = m_textures[material.m_metallicRoughnessIndex]->Sample<vec3>(uv);
		res.m_orm = vec3(ormSample.r, res.m_orm.g * ormSample.g, res.m_orm.b * ormSample.b);
	}
	else
	{
		if (material.HasRoughnessTexture())
		{
			const float roughness = m_textures[material.m_roughnessIndex]->Sample<vec3>(uv).r;
			res.m_orm.y *= roughness;
		}

		if (material.HasMetallicTexture())
		{
			const float metallic = m_textures[material.m_metallicIndex]->Sample<vec3>(uv).r;
			res.m_orm.z *= metallic;
		}
	}

	if (material.HasNormalTexture())
	{
		res.m_normal = m_textures[material.m_normalIndex]->Sample<vec3>(uv);
		res.m_normal.x *= material.m_normalScale;
		res.m_normal.y *= material.m_normalScale;
	}

	if (material.HasClearcoatTexture())
	{
		res.m_clearcoatFactor *=
			m_textures[material.m_clearcoatIndex]->Sample<vec3>(uv).r;
	}

	if (material.HasClearcoatRoughnessTexture())
	{
		res.m_clearcoatRoughness *=
			m_textures[material.m_clearcoatRoughnessIndex]->Sample<vec4>(uv).g;
	}

	if (material.HasClearcoatNormalTexture())
	{
		res.m_clearcoatNormal =
			m_textures[material.m_clearcoatNormalIndex]->Sample<vec3>(uv);
		res.m_clearcoatNormal.x *= material.m_clearcoatNormalScale;
		res.m_clearcoatNormal.y *= material.m_clearcoatNormalScale;
	}

	if (material.HasSheenColorTexture())
	{
		res.m_sheenColor *=
			m_textures[material.m_sheenColorIndex]->Sample<vec3>(uv);
	}

	if (material.HasSheenRoughnessTexture())
	{
		res.m_sheenRoughness *=
			m_textures[material.m_sheenRoughnessIndex]->Sample<vec4>(uv).a;
	}

	res.m_clearcoatFactor = glm::clamp(
		res.m_clearcoatFactor,
		0.0f,
		1.0f);
	res.m_clearcoatRoughness = glm::clamp(
		res.m_clearcoatRoughness,
		0.0f,
		1.0f);
	res.m_sheenColor = glm::clamp(
		res.m_sheenColor,
		vec3(0.0f),
		vec3(1.0f));
	res.m_sheenRoughness = glm::clamp(
		res.m_sheenRoughness,
		0.0f,
		1.0f);

	if (material.HasTransmissionTexture())
	{
		res.m_transmission *= m_textures[material.m_transmissionIndex]->Sample<vec3>(uv).r;
	}

	if (material.HasThicknessTexture())
	{
		res.m_thicknessFactor *= m_textures[material.m_thicknessIndex]->Sample<vec3>(uv).g;
	}

	if (material.m_blendMode == BlendMode::Mask)
	{
		res.m_baseColor.a = res.m_baseColor.a >= material.m_alphaCutoff;
	}

	return res;
}

vec2 PathTracer::NextVec2_Linear(uint32_t& randomState)
{
	return NextRandomVec2_01(randomState);
}

vec2 PathTracer::NextVec2_BlueNoise(
	uint32_t& randSeedX,
	uint32_t& randSeedY,
	uint32_t& randomState)
{
	/*
	static const vec2 BlueNoiseInDisk[64] = {
   vec2(0.478712, 0.875764),
   vec2(-0.337956, -0.793959),
   vec2(-0.955259, -0.028164),
   vec2(0.864527, 0.325689),
   vec2(0.209342, -0.395657),
   vec2(-0.106779, 0.672585),
   vec2(0.156213, 0.235113),
   vec2(-0.413644, -0.082856),
   vec2(-0.415667, 0.323909),
   vec2(0.141896, -0.939980),
   vec2(0.954932, -0.182516),
   vec2(-0.766184, 0.410799),
   vec2(-0.434912, -0.458845),
   vec2(0.415242, -0.078724),
   vec2(0.728335, -0.491777),
   vec2(-0.058086, -0.066401),
   vec2(0.202990, 0.686837),
   vec2(-0.808362, -0.556402),
   vec2(0.507386, -0.640839),
   vec2(-0.723494, -0.229240),
   vec2(0.489740, 0.317826),
   vec2(-0.622663, 0.765301),
   vec2(-0.010640, 0.929347),
   vec2(0.663146, 0.647618),
   vec2(-0.096674, -0.413835),
   vec2(0.525945, -0.321063),
   vec2(-0.122533, 0.366019),
   vec2(0.195235, -0.687983),
   vec2(-0.563203, 0.098748),
   vec2(0.418563, 0.561335),
   vec2(-0.378595, 0.800367),
   vec2(0.826922, 0.001024),
   vec2(-0.085372, -0.766651),
   vec2(-0.921920, 0.183673),
   vec2(-0.590008, -0.721799),
   vec2(0.167751, -0.164393),
   vec2(0.032961, -0.562530),
   vec2(0.632900, -0.107059),
   vec2(-0.464080, 0.569669),
   vec2(-0.173676, -0.958758),
   vec2(-0.242648, -0.234303),
   vec2(-0.275362, 0.157163),
   vec2(0.382295, -0.795131),
   vec2(0.562955, 0.115562),
   vec2(0.190586, 0.470121),
   vec2(0.770764, -0.297576),
   vec2(0.237281, 0.931050),
   vec2(-0.666642, -0.455871),
   vec2(-0.905649, -0.298379),
   vec2(0.339520, 0.157829),
   vec2(0.701438, -0.704100),
   vec2(-0.062758, 0.160346),
   vec2(-0.220674, 0.957141),
   vec2(0.642692, 0.432706),
   vec2(-0.773390, -0.015272),
   vec2(-0.671467, 0.246880),
   vec2(0.158051, 0.062859),
   vec2(0.806009, 0.527232),
   vec2(-0.057620, -0.247071),
   vec2(0.333436, -0.516710),
   vec2(-0.550658, -0.315773),
   vec2(-0.652078, 0.589846),
   vec2(0.008818, 0.530556),
   vec2(-0.210004, 0.519896)
	};*/

	static const float BlueNoiseData[] = { 0.1025390625, 0.4072265625, 0.6240234375, 0.2763671875, 0.7021484375, 0.3701171875, 0.7666015625, 0.9755859375, 0.4619140625, 0.8173828125, 0.5048828125, 0.3427734375,
		0.9677734375, 0.7431640625, 0.4287109375, 0.6875, 0.5458984375, 0.19140625, 0.47265625, 0.109375, 0.2578125, 0.509765625, 0.01171875, 0.3232421875, 0.158203125, 0.6904296875, 0.2236328125,
		0.9375, 0.1396484375, 0.8095703125, 0.4375, 0.568359375, 0.318359375, 0.787109375, 0.8837890625, 0.5234375, 0.14453125, 0.6064453125, 0.2109375, 0.11328125, 0.6865234375, 0.1708984375, 0.8984375,
		0.0341796875, 0.537109375, 0.1455078125, 0.2802734375, 0.919921875, 0.001953125, 0.3017578125, 0.9736328125, 0.58203125, 0.6943359375, 0.9306640625, 0.767578125, 0.65234375, 0.9091796875, 0.3916015625,
		0.7568359375, 0.57421875, 0.3525390625, 0.6298828125, 0.1796875, 0.8623046875, 0.7080078125, 0.2080078125, 0.0302734375, 0.4453125, 0.9267578125, 0.79296875, 0.4833984375, 0.8759765625, 0.3759765625,
		0.599609375, 0.2919921875, 0.7763671875, 0.6279296875, 0.3857421875, 0.7900390625, 0.498046875, 0.6328125, 0.8408203125, 0.7216796875, 0.4150390625, 0.0556640625, 0.208984375, 0.453125, 0.1123046875,
		0.2509765625, 0.5146484375, 0.095703125, 0.8828125, 0.0400390625, 0.98046875, 0.27734375, 0.478515625, 0.95703125, 0.5771484375, 0.3349609375, 0.734375, 0.23828125, 0.05859375, 0.306640625, 0.5517578125,
		0.01953125, 0.9609375, 0.4404296875, 0.193359375, 0.8603515625, 0.060546875, 0.9951171875, 0.2138671875, 0.1015625, 0.369140625, 0.142578125, 0.244140625, 0.853515625, 0.62109375, 0.361328125, 0.9814453125,
		0.587890625, 0.8505859375, 0.310546875, 0.4501953125, 0.7158203125, 0.525390625, 0.662109375, 0.0517578125, 0.40234375, 0.125, 0.9130859375, 0.634765625, 0.3876953125, 0.9833984375, 0.6640625, 0.8193359375,
		0.25390625, 0.751953125, 0.1171875, 0.673828125, 0.4873046875, 0.3330078125, 0.5634765625, 0.693359375, 0.44921875, 0.77734375, 0.5693359375, 0.91015625, 0.4794921875, 0.7939453125, 0.17578125, 0.74609375, 0.04296875,
		0.681640625, 0.1484375, 0.80859375, 0.2294921875, 0.376953125, 0.15625, 0.8271484375, 0.2431640625, 0.46875, 0.8076171875, 0.1787109375, 0.5322265625, 0.1103515625, 0.427734375, 0.1640625, 0.6259765625, 0.5107421875,
		0.3603515625, 0.943359375, 0.2490234375, 0.7548828125, 0.1513671875, 0.8662109375, 0.263671875, 0.958984375, 0.025390625, 0.6767578125, 0.3115234375, 0.0888671875, 0.5478515625, 0.28125, 0.4716796875, 0.3720703125, 0.966796875,
		0.609375, 0.06640625, 0.923828125, 0.7392578125, 0.6044921875, 0.873046875, 0.6708984375, 0.0205078125, 0.298828125, 0.849609375, 0.7060546875, 0.8896484375, 0.326171875, 0.91796875, 0.0693359375, 0.822265625, 0.5498046875,
		0.0126953125, 0.9052734375, 0.4130859375, 0.08203125, 0.650390625, 0.337890625, 0.529296875, 0.1611328125, 0.404296875, 0.9521484375, 0.6494140625, 0.89453125, 0.8203125, 0.2041015625, 0.5380859375, 0.2861328125, 0.76953125,
		0.4990234375, 0.302734375, 0.07421875, 0.34375, 0.5224609375, 0.748046875, 0.435546875, 0.583984375, 0.2265625, 0.0439453125, 0.763671875, 0.45703125, 0.2373046875, 0.6552734375, 0.177734375, 0.703125, 0.3076171875, 0.591796875,
		0.7978515625, 0.5029296875, 0.2001953125, 0.8876953125, 0.7109375, 0.8310546875, 0.236328125, 0.0078125, 0.4228515625, 0.1240234375, 0.720703125, 0.02734375, 0.869140625, 0.4111328125, 0.1884765625, 0.5673828125, 0.9921875, 0.134765625,
		0.21484375, 0.90625, 0.0986328125, 0.9638671875, 0.3623046875, 0.5078125, 0.60546875, 0.123046875, 0.98828125, 0.341796875, 0.4345703125, 0.837890625, 0.4853515625, 0.189453125, 0.978515625, 0.046875, 0.7470703125, 0.43359375, 0.0625,
		0.5966796875, 0.5068359375, 0.7333984375, 0.2880859375, 0.6162109375, 0.9482421875, 0.4541015625, 0.65625, 0.1162109375, 0.8359375, 0.689453125, 0.4326171875, 0.5986328125, 0.828125, 0.3828125, 0.640625, 0.181640625, 0.798828125,
		0.6787109375, 0.279296875, 0.87109375, 0.53125, 0.71875, 0.041015625, 0.9453125, 0.103515625, 0.677734375, 0.283203125, 0.3740234375, 0.619140625, 0.9423828125, 0.2998046875, 0.365234375, 0.150390625, 0.9990234375, 0.7919921875,
		0.3583984375, 0.5654296875, 0.224609375, 0.322265625, 0.9345703125, 0.275390625, 0.005859375, 0.7646484375, 0.2529296875, 0.0458984375, 0.7177734375, 0.2724609375, 0.4951171875, 0.015625, 0.9326171875, 0.1591796875, 0.3994140625,
		0.076171875, 0.7998046875, 0.2958984375, 0.6025390625, 0.38671875, 0.900390625, 0.5302734375, 0.8544921875, 0.130859375, 0.228515625, 0.779296875, 0.865234375, 0.6650390625, 0.072265625, 0.4736328125, 0.1748046875, 0.05078125,
		0.888671875, 0.7314453125, 0.5830078125, 0.50390625, 0.3642578125, 0.9462890625, 0.53515625, 0.455078125, 0.9716796875, 0.5732421875, 0.876953125, 0.3369140625, 0.4423828125, 0.7353515625, 0.8369140625, 0.6220703125, 0.19921875,
		0.505859375, 0.1474609375, 0.775390625, 0.216796875, 0.0048828125, 0.705078125, 0.462890625, 0.572265625, 0.0869140625, 0.439453125, 0.5537109375, 0.248046875, 0.8779296875, 0.7001953125, 0.810546875, 0.3798828125, 0.0927734375,
		0.1826171875, 0.806640625, 0.1083984375, 0.642578125, 0.896484375, 0.33984375, 0.1630859375, 0.0712890625, 0.7587890625, 0.658203125, 0.1044921875, 0.5712890625, 0.24609375, 0.357421875, 0.9501953125, 0.69921875, 0.87890625, 0.4462890625,
		0.6513671875, 0.3408203125, 0.8125, 0.26953125, 0.9765625, 0.73046875, 0.197265625, 0.931640625, 0.3935546875, 0.6181640625, 0.296875, 0.54296875, 0.4638671875, 0.982421875, 0.7041015625, 0.4267578125, 0.8642578125, 0.1982421875, 0.0849609375,
		0.6982421875, 0.8154296875, 0.423828125, 0.2099609375, 0.2978515625, 0.9853515625, 0.482421875, 0.037109375, 0.8583984375, 0.419921875, 0.056640625, 0.3134765625, 0.0966796875, 0.5439453125, 0.927734375, 0.111328125, 0.4013671875, 0.6396484375,
		0.0244140625, 0.3291015625, 0.7861328125, 0.1357421875, 0.0615234375, 0.9560546875, 0.2158203125, 0.013671875, 0.6484375, 0.2568359375, 0.5595703125, 0.3095703125, 0.7529296875, 0.501953125, 0.2666015625, 0.626953125, 0.9365234375, 0.5283203125,
		0.8525390625, 0.146484375, 0.6103515625, 0.744140625, 0.171875, 0.66015625, 0.578125, 0.7841796875, 0.9970703125, 0.251953125, 0.7373046875, 0.4970703125, 0.185546875, 0.8427734375, 0.513671875, 0.6669921875, 0.4560546875, 0.8486328125, 0.708984375,
		0.4169921875, 0.5927734375, 0.8251953125, 0.35546875, 0.12890625, 0.9296875, 0.0263671875, 0.4052734375, 0.998046875, 0.564453125, 0.1259765625, 0.3505859375, 0.0009765625, 0.6630859375, 0.390625, 0.8232421875, 0.3193359375, 0.9619140625, 0.265625,
		0.484375, 0.1494140625, 0.384765625, 0.630859375, 0.033203125, 0.890625, 0.5869140625, 0.2841796875, 0.9189453125, 0.099609375, 0.255859375, 0.5205078125, 0.3173828125, 0.880859375, 0.16015625, 0.73828125, 0.9013671875, 0.517578125, 0.724609375,
		0.623046875, 0.8349609375, 0.0576171875, 0.220703125, 0.8935546875, 0.7509765625, 0.466796875, 0.7822265625, 0.2412109375, 0.064453125, 0.4580078125, 0.55078125, 0.0224609375, 0.9072265625, 0.69140625, 0.83203125, 0.2021484375, 0.44140625,
		0.333984375, 0.796875, 0.13671875, 0.373046875, 0.75390625, 0.9541015625, 0.1845703125, 0.646484375, 0.048828125, 0.494140625, 0.2744140625, 0.0771484375, 0.41796875, 0.2333984375, 0.15234375, 0.3544921875, 0.6845703125, 0.4521484375,
		0.638671875, 0.2822265625, 0.09375, 0.9697265625, 0.5810546875, 0.904296875, 0.71484375, 0.205078125, 0.8017578125, 0.41015625, 0.30078125, 0.08984375, 0.5625, 0.9228515625, 0.6689453125, 0.0498046875, 0.712890625, 0.4599609375, 0.6005859375,
		0.00390625, 0.5615234375, 0.8037109375, 0.994140625, 0.37890625, 0.5703125, 0.6748046875, 0.791015625, 0.9658203125, 0.4892578125, 0.7734375, 0.9443359375, 0.1650390625, 0.8115234375, 0.3984375, 0.541015625, 0.1923828125, 0.3310546875,
		0.1298828125, 0.380859375, 0.8798828125, 0.603515625, 0.140625, 0.5126953125, 0.962890625, 0.7197265625, 0.2548828125, 0.1552734375, 0.5390625, 0.974609375, 0.2119140625, 0.86328125, 0.3251953125, 0.412109375, 0.234375, 0.1279296875, 0.7578125,
		0.921875, 0.18359375, 0.33203125, 0.044921875, 0.59765625, 0.2626953125, 0.52734375, 0.359375, 0.0234375, 0.88671875, 0.732421875, 0.6474609375, 0.8330078125, 0.490234375, 0.6826171875, 0.078125, 0.31640625, 0.7724609375, 0.64453125, 0.0068359375,
		0.3681640625, 0.4765625, 0.8701171875, 0.4091796875, 0.28515625, 0.6455078125, 0.10546875, 0.7880859375, 0.908203125, 0.7099609375, 0.4609375, 0.30859375, 0.0146484375, 0.6337890625, 0.83984375, 0.4384765625, 0.8720703125, 0.1005859375,
		0.7412109375, 0.9150390625, 0.5947265625, 0.25, 0.12109375, 0.4248046875, 0.0361328125, 0.939453125, 0.2646484375, 0.5263671875, 0.955078125, 0.4365234375, 0.2392578125, 0.884765625, 0.755859375, 0.107421875, 0.625, 0.8212890625, 0.0751953125,
		0.76171875, 0.49609375, 0.173828125, 0.5341796875, 0.083984375, 0.611328125, 0.8681640625, 0.5009765625, 0.240234375, 0.544921875, 0.1435546875, 0.669921875, 0.3154296875, 0.431640625, 0.0908203125, 0.68359375, 0.4697265625, 0.9892578125,
		0.314453125, 0.759765625, 0.595703125, 0.1533203125, 0.7275390625, 0.0478515625, 0.8291015625, 0.1669921875, 0.55859375, 0.3046875, 0.990234375, 0.201171875, 0.3466796875, 0.56640625, 0.953125, 0.39453125, 0.3037109375, 0.671875, 0.97265625,
		0.3662109375, 0.1953125, 0.6884765625, 0.951171875, 0.771484375, 0.3779296875, 0.986328125, 0.1904296875, 0.560546875, 0.8466796875, 0.29296875, 0.1572265625, 0.814453125, 0.5546875, 0.22265625, 0.400390625, 0.8447265625, 0.32421875,
		0.615234375, 0.3896484375, 0.6962890625, 0.4658203125, 0.0546875, 0.66796875, 0.5087890625, 0.736328125, 0.0322265625, 0.2314453125, 0.6201171875, 0.875, 0.021484375, 0.2607421875, 0.82421875, 0.0654296875, 0.421875, 0.119140625, 0.294921875,
		0.029296875, 0.61328125, 0.794921875, 0.0537109375, 0.388671875, 0.947265625, 0.5166015625, 0.0087890625, 0.70703125, 0.9248046875, 0.0830078125, 0.4814453125, 0.9775390625, 0.1943359375, 0.8916015625, 0.1201171875, 0.7802734375, 0.9140625,
		0.3837890625, 0.1318359375, 0.93359375, 0.4482421875, 0.8388671875, 0.1142578125, 0.7421875, 0.40625, 0.4931640625, 0.5751953125, 0.7607421875, 0.916015625, 0.5244140625, 0.7255859375, 0.8564453125, 0.4921875, 0.2685546875, 0.716796875,
		0.2177734375, 0.7490234375, 0.62890625, 0.3388671875, 0.447265625, 0.1767578125, 0.7958984375, 0.6533203125, 0.017578125, 0.533203125, 0.267578125, 0.5859375, 0.349609375, 0.212890625, 0.576171875, 0.84765625, 0.2705078125, 0.6806640625,
		0.3212890625, 0.5185546875, 0.1962890625, 0.90234375, 0.701171875, 0.1328125, 0.232421875, 0.6376953125, 0.34765625, 0.1806640625, 0.4140625, 0.126953125, 0.8974609375, 0.59375, 0.474609375, 0.1064453125, 0.8671875, 0.2470703125, 0.8994140625,
		0.6015625, 0.3671875, 0.2783203125, 0.7626953125, 0.4189453125, 0.7265625, 0.9599609375, 0.0419921875, 0.81640625, 0.443359375, 0.0673828125, 0.6142578125, 0.169921875, 0.7890625, 0.9873046875, 0.5849609375, 0.0810546875, 0.3125, 0.96875,
		0.46484375, 0.0029296875, 0.833984375, 0.556640625, 0.9580078125, 0.654296875, 0.3515625, 0.9794921875, 0.1728515625, 0.8056640625, 0.4208984375, 0.0595703125, 0.6953125, 0.09765625, 0.5400390625, 0.94140625, 0.138671875, 0.8515625, 0.091796875,
		0.4755859375, 0.6416015625, 0.291015625, 0.7236328125, 0.9033203125, 0.3564453125, 0.5, 0.009765625, 0.416015625, 0.26171875, 0.6591796875, 0.841796875, 0.3818359375, 0.78125, 0.287109375, 0.6796875, 0.0791015625, 0.23046875, 0.783203125, 0.0185546875,
		0.271484375, 0.6728515625, 0.330078125, 0.552734375, 0.970703125, 0.4912109375, 0.818359375, 0.21875, 0.67578125, 0.3447265625, 0.607421875, 0.2216796875, 0.375, 0.92578125, 0.154296875, 0.5361328125, 0.203125, 0.8046875, 0.9287109375, 0.6923828125,
		0.1416015625, 0.7744140625, 0.4775390625, 0.0380859375, 0.6123046875, 0.1689453125, 0.9404296875, 0.48828125, 0.8955078125, 0.3203125, 0.458984375, 0.546875, 0.4033203125, 0.9169921875, 0.03515625, 0.7294921875, 0.1865234375, 0.3056640625,
		0.3974609375, 0.8818359375, 0.0283203125, 0.470703125, 0.9912109375, 0.7119140625, 0.548828125, 0.826171875, 0.0166015625, 0.6611328125, 0.392578125, 0.0947265625, 0.2939453125, 0.5791015625, 0.859375, 0.345703125, 0.9384765625, 0.20703125,
		0.5419921875, 0.7138671875, 0.115234375, 0.396484375, 0.58984375, 0.740234375, 0.1337890625, 0.8740234375, 0.080078125, 0.5908203125, 0.2275390625, 0.85546875, 0.63671875, 0.1181640625, 0.75, 0.521484375, 0.162109375, 0.78515625, 0.2890625,
		0.052734375, 0.1875, 0.328125, 0.765625, 0.4677734375, 0.984375, 0.7451171875, 0.4443359375, 0.2197265625, 0.515625, 0.0703125, 0.728515625, 0.4306640625, 0.857421875, 0.353515625, 0.2421875, 0.845703125, 0.0634765625, 0.2060546875, 0.6318359375,
		0.802734375, 0.486328125, 0.7705078125, 0.3486328125, 0.51171875, 0.4296875, 0.935546875, 0.2451171875, 0.6572265625, 0.912109375, 0.408203125, 0.5888671875, 0.861328125, 0.451171875, 0.9111328125, 0.580078125, 0.259765625, 0.1376953125,
		0.6083984375, 0.0390625, 0.666015625, 0.96484375, 0.166015625, 0.6357421875, 0.2734375, 0.087890625, 0.9931640625, 0.6171875, 0.7685546875, 0.51953125, 0.9208984375, 0.37109375, 0.2587890625, 0.685546875, 0.16796875, 0.99609375, 0.0859375,
		0.830078125, 0.0, 0.5556640625, 0.3271484375, 0.068359375, 0.2353515625, 0.72265625, 0.1220703125, 0.6435546875, 0.2255859375, 0.0732421875, 0.84375, 0.36328125, 0.892578125, 0.7783203125, 0.3359375, 0.8134765625, 0.3955078125, 0.8857421875,
		0.5576171875, 0.80078125, 0.48046875, 0.03125, 0.42578125, 0.2900390625, 0.697265625, 0.0107421875, 0.94921875 };


	if (randSeedX >= 688)
	{
		randSeedX = NextRandomRange(randomState, 681u);
	}

	if (randSeedY >= 688)
	{
		randSeedY = NextRandomRange(randomState, 681u);
	}

	vec2 res = vec2(BlueNoiseData[randSeedX++], BlueNoiseData[randSeedY++]);

	return res;
}
