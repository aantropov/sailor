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
			uint32_t m_numBounces;
			uint32_t m_msaa;
			vec3 m_ambient;
		};

		static void ParseParamsFromCommandLineArgs(Params& params, const char** args, int32_t num);

		void Run(const Params& params);

	protected:

		static vec2 NextVec2_BlueNoise(uint32_t& randSeedX, uint32_t& randSeedY);
		__forceinline static vec2 NextVec2_Linear();

		__forceinline LightingModel::SampledData GetMaterialData(const size_t& materialIndex, glm::vec2 uv) const;

		vec3 Raytrace(const Math::Ray& r, const BVH& bvh, uint32_t bounceLimit, uint32_t ignoreTriangle, const Params& params, float environmentIor = 1.0f) const;

		TVector<DirectionalLight> m_directionalLights{};
		TVector<Math::Triangle> m_triangles{};
		TVector<Material> m_materials{};
		TVector<TSharedPtr<CombinedSampler2D>> m_textures{};
		TMap<std::string, uint32_t> m_textureMapping{};
	};
}