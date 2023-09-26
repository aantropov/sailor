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
			uint32_t m_height;
			uint32_t m_numSamples;
			uint32_t m_numBounces;
			uint32_t m_msaa;
		};

		static void ParseParamsFromCommandLineArgs(Params& params, const char** args, int32_t num);

		void Run(const Params& params);

	protected:

		__forceinline LightingModel::SampledData GetSampledData(const size_t& materialIndex, glm::vec2 uv) const;

		vec3 Raytrace(const Math::Ray& r, const BVH& bvh, uint32_t bounceLimit, uint32_t ignoreTriangle, const Params& params) const;

		TVector<DirectionalLight> m_directionalLights{};
		TVector<Math::Triangle> m_triangles{};
		TVector<Material> m_materials{};
		TVector<TSharedPtr<Texture2D>> m_textures{};
		TMap<std::string, uint32_t> m_textureMapping{};
	};
}