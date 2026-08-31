#pragma once
#include "Core/Defines.h"
#include "Math/Bounds.h"
#include "Containers/Vector.h"
#include "Containers/Map.h"
#include "Containers/Octree.h"
#include "Engine/Types.h"
#include "Raytracing/BVH.h"

#include "MaterialUtils.h"
#include "LightingModel.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

using namespace Sailor;

namespace Sailor::Raytracing
{
	class PathTracer
	{
		friend class GIProbesPathTracer;

	public:
		enum class EScenePreparationStage : uint8_t
		{
			Geometry = 0u,
			Materials
		};

		struct ScenePreparationProgress final
		{
			EScenePreparationStage m_stage =
				EScenePreparationStage::Geometry;
			size_t m_completed = 0u;
			size_t m_total = 0u;
		};

			struct ScenePreparationStats final
		{
			size_t m_instanceCount = 0u;
			size_t m_geometryInstanceCount = 0u;
			size_t m_triangleCount = 0u;
			size_t m_emissiveTriangleCount = 0u;
			size_t m_skippedInstanceCount = 0u;
			size_t m_builtBlasCount = 0u;
			size_t m_reusedBlasCount = 0u;
			size_t m_materialSlotCount = 0u;
			size_t m_uniqueMaterialCount = 0u;
			size_t m_reusedMaterialCount = 0u;
			size_t m_textureReferenceCount = 0u;
			size_t m_uniqueTextureCount = 0u;
			size_t m_decodedTextureCount = 0u;
			double m_emissiveSamplingWeight = 0.0;
		};

		using ScenePreparationProgressCallback =
			std::function<bool(const ScenePreparationProgress&)>;
		using ScenePreparationWarningCallback =
			std::function<void(const std::string&)>;

		struct TLASInstance
		{
			ModelPtr m_model{};
			// Optional immutable geometry snapshot. GI probes baking uses it so
			// model hot reloads cannot mutate an in-flight bake.
			TSharedPtr<BVH> m_blas{};
			TSharedPtr<TVector<Math::Triangle>> m_triangles{};
			int32_t m_meshIndex = -1;
			Math::AABB m_worldBounds{};
			glm::mat4 m_worldMatrix{ 1.0f };
			glm::mat4 m_inverseWorldMatrix{ 1.0f };
			int32_t m_materialBaseOffset = 0;
			std::string m_debugName{};
		};

		struct Params
		{
			std::filesystem::path m_pathToModel;
			std::filesystem::path m_output;
			std::string m_camera;
			uint32_t m_height;
			uint32_t m_numSamples;
			uint32_t m_numAmbientSamples;
			uint32_t m_maxBounces;
			uint32_t m_msaa;
			vec3 m_ambient;
			float m_rayBiasBase = 0.0f;
			float m_rayBiasScale = 0.0f;

			bool m_bUseRuntimeCamera = false;
			vec3 m_runtimeCameraPos = vec3(0.0f, 0.75f, 5.0f);
			vec3 m_runtimeCameraForward = vec3(0.0f, 0.0f, -1.0f);
			vec3 m_runtimeCameraUp = vec3(0.0f, 1.0f, 0.0f);
			float m_runtimeAspectRatio = 0.0f;
			float m_runtimeHFov = 0.0f;
			bool m_bRunTasksInline = false;
			bool m_bIncludeDirectLighting = true;
			bool m_bIncludeEnvironment = true;
			bool m_bIncludeEmissive = true;
		};

		struct PreparedRaySample final
		{
			vec3 m_radiance{};
			float m_distance = 0.0f;
			bool m_bHit = false;
			bool m_bBackFace = false;
		};

		static void ParseCommandLineArgs(Params& params, const char** args, int32_t num);

		SAILOR_SHARED_API bool InitializeScene(const TVector<TLASInstance>& instances,
			const TVector<MaterialPtr>& materials,
			const TVector<LightProxy>& lightProxies,
			bool bAddDefaultLightIfEmpty = true,
			const ScenePreparationProgressCallback& progress = {},
			bool bSkipUnresolvedMaterialInstances = false,
			const ScenePreparationWarningCallback& warning = {});
		void SetRuntimeEnvironment(const TVector<u8vec4>& image, const glm::uvec2& extent);
		SAILOR_SHARED_API void SetRuntimeEnvironmentLinear(const TVector<vec4>& image, const glm::uvec2& extent);
		void SetRuntimeDiffuseEnvironmentLinear(const TVector<vec4>& image, const glm::uvec2& extent);
		void ClearRuntimeEnvironment();
		bool RenderPreparedScene(const Params& params);
		SAILOR_SHARED_API bool SamplePreparedSceneRay(
			const vec3& origin,
			const vec3& direction,
			float maxDistance,
			const Params& params,
			uint32_t randomSeed,
			PreparedRaySample& outSample) const;
		SAILOR_SHARED_API bool SamplePreparedSceneVisibility(
			const vec3& origin,
			const vec3& direction,
			float maxDistance,
			PreparedRaySample& outSample) const;
		bool ArePreparedMaterialsFullyResolved() const
		{
			return m_bMaterialsFullyResolved;
		}
		double GetLastRaytraceTimeMs() const { return m_lastRaytraceTimeMs; }
		const ScenePreparationStats& GetLastScenePreparationStats() const
		{
			return m_lastScenePreparationStats;
		}
		const TVector<u8vec4>& GetLastRenderedImage() const { return m_lastRenderedImage; }
		glm::uvec2 GetLastRenderedExtent() const { return m_lastRenderedExtent; }

		void Run(const Params& params);

	protected:

		static vec2 NextVec2_BlueNoise(
			uint32_t& randSeedX,
			uint32_t& randSeedY,
			uint32_t& randomState);
		__forceinline static vec2 NextVec2_Linear(uint32_t& randomState);

		SAILOR_SHARED_API LightingModel::SampledData GetMaterialData(
			const size_t& materialIndex,
			glm::vec2 uv,
			glm::vec4 layerWeights = glm::vec4(1.0f)) const;
		glm::vec4 SampleMaterialBaseColor(
			const size_t& materialIndex,
			glm::vec2 uv,
			glm::vec4 layerWeights) const;

		struct TLASHit
		{
			Math::RaycastHit m_hit{};
			vec3 m_geometricNormal{};
			uint32_t m_instanceIndex = (uint32_t)-1;
			uint32_t m_triangleIndex = (uint32_t)-1;
			uint32_t m_materialIndex = 0;
		};

		SAILOR_SHARED_API bool IntersectScene(const Math::Ray& worldRay, TLASHit& outHit, float maxRayLength = FLT_MAX,
			uint32_t ignoreInstance = (uint32_t)-1, uint32_t ignoreTriangle = (uint32_t)-1) const;
		SAILOR_SHARED_API float TraceDirectLightTransmittance(
			const Math::Ray& worldRay,
			float maxRayLength = FLT_MAX,
			uint32_t ignoreInstance = (uint32_t)-1,
			uint32_t ignoreTriangle = (uint32_t)-1) const;
		bool IntersectSceneGeometry(const Math::Ray& worldRay, TLASHit& outHit,
			float maxRayLength, uint32_t ignoreInstance,
			uint32_t ignoreTriangle) const;
		const Math::Triangle& GetTriangle(const TLASHit& hit) const;
		SAILOR_SHARED_API void GetShadingBasis(const TLASHit& hit, vec3& outNormal, vec3& outTangent, vec3& outBitangent) const;
		SAILOR_SHARED_API static bool OrientShadingBasisToGeometricSurface(
			const vec3& rayDirection,
			const vec3& geometricNormal,
			vec3& inOutNormal,
			vec3& inOutBitangent,
			vec3& outOrientedGeometricNormal);
		uint32_t ResolveMaterialIndex(const TLASHit& hit) const;
		bool IsThickVolumeAtHit(
			const TLASHit& hit,
			uint32_t materialIndex) const;

		struct EmissiveTriangle final
		{
			glm::vec3 m_vertices[3]{};
			glm::vec2 m_uvs[3]{};
			glm::vec4 m_colors[3]{};
			uint32_t m_instanceIndex = 0u;
			uint32_t m_triangleIndex = 0u;
			uint32_t m_materialIndex = 0u;
			float m_area = 0.0f;
			float m_weight = 0.0f;
			float m_cumulativeWeight = 0.0f;
		};

		void AppendEmissiveTriangles(uint32_t instanceIndex);
		vec3 SampleDirectEmissive(
			const TLASHit& receiverHit,
			const LightingModel::SampledData& receiverMaterial,
			const vec3& viewDirection,
			const vec3& worldNormal,
			float fromIor,
			float toIor,
			const Params& params,
			uint32_t& randomState) const;

		vec3 Raytrace(const Math::Ray& r, uint32_t bounceLimit, uint32_t ignoreInstance, uint32_t ignoreTriangle,
			float maxRayDistance, const Params& params, float environmentIor,
			uint32_t& randomState, bool bAllowEmissiveHit) const;
		vec3 SampleRuntimeEnvironment(const vec3& direction) const;
		vec3 SampleRuntimeDiffuseEnvironment(const vec3& direction) const;
		vec3 SampleRuntimeDirectEnvironment(const vec3& direction) const;
		void RebuildRuntimeEnvironmentImportance();
		float RuntimeEnvironmentImportancePdf(
			const vec3& direction) const;
		bool SampleRuntimeEnvironmentImportance(
			uint32_t& randomState,
			vec3& outDirection,
			float& outPdf) const;
		float DirectEnvironmentPdf(
			const vec3& worldNormal,
			const vec3& direction) const;
		bool SampleDirectEnvironment(
			const vec3& worldNormal,
			uint32_t& randomState,
			vec3& outDirection,
			float& outPdf) const;


		TVector<DirectionalLight> m_directionalLights{};
		TVector<LightProxy> m_lightProxies{};
		TVector<TLASInstance> m_tlasInstances{};
		TOctree<size_t> m_tlasOctree{ glm::ivec3(0, 0, 0), 16536 * 16, 4 };
		TVector<Material> m_materials{};
		TVector<uint8_t> m_resolvedMaterialSlots{};
		TVector<EmissiveTriangle> m_emissiveTriangles{};
		float m_totalEmissiveWeight = 0.0f;
		TVector<TSharedPtr<CombinedSampler2D>> m_textures{};
		TMap<std::string, uint32_t> m_textureMapping{};
		size_t m_cachedMaterialsSignature = 0;
		uint32_t m_cachedMaterialsCount = 0;
		bool m_bMaterialsFullyResolved = false;
		ScenePreparationStats m_lastScenePreparationStats{};
		double m_lastRaytraceTimeMs = 0.0;
		TVector<u8vec4> m_lastRenderedImage{};
		glm::uvec2 m_lastRenderedExtent{ 0, 0 };
		CombinedSampler2D m_runtimeEnvironment{};
		CombinedSampler2D m_runtimeDiffuseEnvironment{};
		TVector<float> m_runtimeEnvironmentImportanceCdf{};
		TVector<float> m_runtimeEnvironmentImportancePdf{};
		bool m_bHasRuntimeEnvironment = false;
		bool m_bHasRuntimeDiffuseEnvironment = false;
		bool m_bUseRuntimeEnvironmentImportance = false;
		bool m_bAddDefaultLightIfEmpty = true;
	};
}
