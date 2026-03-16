#pragma once
#include "Core/Defines.h"
#include "Math/Bounds.h"
#include "Containers/Vector.h"
#include "Containers/Map.h"
#include "Containers/Octree.h"
#include "Engine/Types.h"

#include "MaterialUtils.h"
#include "LightingModel.h"

#include <filesystem>

using namespace Sailor;

namespace Sailor::Raytracing
{
	class PathTracer
	{
	public:

		struct TLASInstance
		{
			ModelPtr m_model{};
			Math::AABB m_worldBounds{};
			glm::mat4 m_worldMatrix{ 1.0f };
			glm::mat4 m_inverseWorldMatrix{ 1.0f };
			int32_t m_materialBaseOffset = 0;
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
		};

		static void ParseCommandLineArgs(Params& params, const char** args, int32_t num);

		bool InitializeScene(const TVector<TLASInstance>& instances,
			const TVector<MaterialPtr>& materials,
			const TVector<LightProxy>& lightProxies);
		void SetRuntimeEnvironment(const TVector<u8vec4>& image, const glm::uvec2& extent);
		void SetRuntimeEnvironmentLinear(const TVector<vec4>& image, const glm::uvec2& extent);
		void SetRuntimeDiffuseEnvironmentLinear(const TVector<vec4>& image, const glm::uvec2& extent);
		void ClearRuntimeEnvironment();
		bool RenderPreparedScene(const Params& params);
		double GetLastRaytraceTimeMs() const { return m_lastRaytraceTimeMs; }
		const TVector<u8vec4>& GetLastRenderedImage() const { return m_lastRenderedImage; }
		glm::uvec2 GetLastRenderedExtent() const { return m_lastRenderedExtent; }

		void Run(const Params& params);

	protected:

		static vec2 NextVec2_BlueNoise(uint32_t& randSeedX, uint32_t& randSeedY);
		__forceinline static vec2 NextVec2_Linear();

		__forceinline LightingModel::SampledData GetMaterialData(const size_t& materialIndex, glm::vec2 uv) const;

		struct TLASHit
		{
			Math::RaycastHit m_hit{};
			uint32_t m_instanceIndex = (uint32_t)-1;
			uint32_t m_triangleIndex = (uint32_t)-1;
			uint32_t m_materialIndex = 0;
		};

		bool IntersectScene(const Math::Ray& worldRay, TLASHit& outHit, float maxRayLength = FLT_MAX,
			uint32_t ignoreInstance = (uint32_t)-1, uint32_t ignoreTriangle = (uint32_t)-1) const;
		const Math::Triangle& GetTriangle(const TLASHit& hit) const;
		void GetShadingBasis(const TLASHit& hit, vec3& outNormal, vec3& outTangent, vec3& outBitangent) const;
		uint32_t ResolveMaterialIndex(const TLASHit& hit) const;

		vec3 TraceSky(vec3 startPoint, vec3 toLight, const PathTracer::Params& params, float currentIor,
			uint32_t ignoreInstance, uint32_t ignoreTriangle) const;
		vec3 Raytrace(const Math::Ray& r, uint32_t bounceLimit, uint32_t ignoreInstance, uint32_t ignoreTriangle,
			const Params& params, float inAcc, float environmentIor = 1.0f) const;
		vec3 SampleRuntimeEnvironment(const vec3& direction) const;
		vec3 SampleRuntimeDiffuseEnvironment(const vec3& direction) const;


		TVector<DirectionalLight> m_directionalLights{};
		TVector<LightProxy> m_lightProxies{};
		TVector<TLASInstance> m_tlasInstances{};
		TOctree<size_t> m_tlasOctree{ glm::ivec3(0, 0, 0), 16536 * 16, 4 };
		TVector<Material> m_materials{};
		TVector<TSharedPtr<CombinedSampler2D>> m_textures{};
		TMap<std::string, uint32_t> m_textureMapping{};
		size_t m_cachedMaterialsSignature = 0;
		uint32_t m_cachedMaterialsCount = 0;
		bool m_bMaterialsFullyResolved = false;
		double m_lastRaytraceTimeMs = 0.0;
		TVector<u8vec4> m_lastRenderedImage{};
		glm::uvec2 m_lastRenderedExtent{ 0, 0 };
		CombinedSampler2D m_runtimeEnvironment{};
		CombinedSampler2D m_runtimeDiffuseEnvironment{};
		bool m_bHasRuntimeEnvironment = false;
		bool m_bHasRuntimeDiffuseEnvironment = false;
	};
}
