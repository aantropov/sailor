#pragma once
#include "Core/Defines.h"
#include "Math/Bounds.h"
#include "Containers/Vector.h"
#include "Containers/Map.h"

#include "BVH.h"
#include "MaterialUtils.h"
#include "LightingModel.h"

#include <filesystem>

using namespace Sailor;

namespace Sailor::Raytracing
{
	class PathTracer
	{
	public:

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

			bool m_bUseRuntimeCamera = false;
			vec3 m_runtimeCameraPos = vec3(0.0f, 0.75f, 5.0f);
			vec3 m_runtimeCameraForward = vec3(0.0f, 0.0f, -1.0f);
			vec3 m_runtimeCameraUp = vec3(0.0f, 1.0f, 0.0f);
			float m_runtimeAspectRatio = 0.0f;
			float m_runtimeHFov = 0.0f;
		};

		static void ParseCommandLineArgs(Params& params, const char** args, int32_t num);

		bool InitializeScene(const TVector<Math::Triangle>& triangles,
			const TVector<MaterialPtr>& materials,
			const TVector<LightProxy>& lightProxies);
		bool RenderPreparedScene(const Params& params);

		void Run(const Params& params);

	protected:

		static vec2 NextVec2_BlueNoise(uint32_t& randSeedX, uint32_t& randSeedY);
		__forceinline static vec2 NextVec2_Linear();

		__forceinline LightingModel::SampledData GetMaterialData(const size_t& materialIndex, glm::vec2 uv) const;


		vec3 TraceSky(vec3 startPoint, vec3 toLight, const BVH& bvh, const PathTracer::Params& params, float currentIor, uint32_t ignoreTriangle) const;
		vec3 Raytrace(const Math::Ray& r, const BVH& bvh, uint32_t bounceLimit, uint32_t ignoreTriangle, const Params& params, float inAcc, float environmentIor = 1.0f) const;


		TVector<DirectionalLight> m_directionalLights{};
		TVector<LightProxy> m_lightProxies{};
		TVector<Math::Triangle> m_triangles{};
		TVector<Material> m_materials{};
		TVector<TSharedPtr<CombinedSampler2D>> m_textures{};
		TMap<std::string, uint32_t> m_textureMapping{};
		size_t m_cachedMaterialsSignature = 0;
		uint32_t m_cachedMaterialsCount = 0;
		bool m_bMaterialsFullyResolved = false;
	};
}
