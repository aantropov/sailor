#pragma once
#include "Sailor.h"
#include "ECS/ECS.h"
#include "Engine/Types.h"
#include "Raytracing/BVH.h"
#include "Raytracing/LightingModel.h"
#include "Raytracing/PathTracer.h"
#include "Math/Bounds.h"
#include "Containers/Octree.h"

namespace Sailor
{
	namespace Raytracing
	{
		class PathTracer;
	}

	class PathTracerProxyData final : public ECS::TComponent
	{
	public:

		struct Options
		{
			bool m_bEnabled = false;
			bool m_bRebuildEveryFrame = false;
			uint32_t m_maxBounces = 2;
			uint32_t m_samplesPerPixel = 2;
		};

		SAILOR_API const TVector<MaterialPtr>& GetMaterials() const { return m_materials; }
		SAILOR_API const TVector<TMap<std::string, TexturePtr>>& GetTextureBindings() const { return m_textureBindings; }
		SAILOR_API void SetMaterials(TVector<MaterialPtr>&& materials, TVector<TMap<std::string, TexturePtr>>&& textureBindings)
		{
			m_materials = std::move(materials);
			m_textureBindings = std::move(textureBindings);
			MarkDirty();
		}

		SAILOR_API const Options& GetOptions() const { return m_options; }
		SAILOR_API Options& GetOptions() { return m_options; }
		SAILOR_API const TVector<Raytracing::LightProxy>& GetLightProxies() const { return m_lightProxies; }
		SAILOR_API void SetLightProxies(const TVector<Raytracing::LightProxy>& lightProxies) { m_lightProxies = lightProxies; }

		SAILOR_API const TSharedPtr<Raytracing::BVH>& GetBVH() const { return m_bvh; }
		SAILOR_API const TVector<Math::Triangle>& GetTriangles() const { return m_triangles; }
		SAILOR_API const Math::AABB& GetWorldBounds() const { return m_worldBounds; }
		SAILOR_API const glm::mat4& GetWorldMatrix() const { return m_worldMatrix; }
		SAILOR_API const glm::mat4& GetInverseWorldMatrix() const { return m_inverseWorldMatrix; }

		SAILOR_API virtual void Clear() override
		{
			m_owner = nullptr;
			m_materials.Clear();
			m_textureBindings.Clear();
			m_lightProxies.Clear();
			m_options = Options();
			m_bvh.Clear();
			m_triangles.Clear();
			m_worldBounds = Math::AABB();
			m_worldMatrix = glm::mat4(1.0f);
			m_inverseWorldMatrix = glm::mat4(1.0f);
			m_bNeedsRebuild = true;
			m_frameLastChange = 0;
			m_bIsDirty = false;
		}

	protected:

		TVector<MaterialPtr> m_materials{};
		TVector<TMap<std::string, TexturePtr>> m_textureBindings{};
		TVector<Raytracing::LightProxy> m_lightProxies{};
		Options m_options{};

		TSharedPtr<Raytracing::BVH> m_bvh{};
		TVector<Math::Triangle> m_triangles{}; // local-space BLAS triangles
		Math::AABB m_worldBounds{};
		glm::mat4 m_worldMatrix{ 1.0f };
		glm::mat4 m_inverseWorldMatrix{ 1.0f };
		bool m_bNeedsRebuild = true;

		friend class PathTracerECS;
	};

	class PathTracerECS final : public ECS::TSystem<PathTracerECS, PathTracerProxyData>
	{
	public:

		virtual Tasks::ITaskPtr Tick(float deltaTime) override;
		bool InitializePathTracer(Raytracing::PathTracer& outPathTracer, size_t componentHandle = (size_t)-1);
		bool RenderScene(const Raytracing::PathTracer::Params& params, size_t componentHandle = (size_t)-1);
		bool IntersectProxyRay(size_t componentHandle, const Math::Ray& worldRay, Math::RaycastHit& outHit, float maxRayLength = FLT_MAX) const;
		bool IntersectSceneRay(const Math::Ray& worldRay, Math::RaycastHit& outHit, size_t& outComponentHandle, float maxRayLength = FLT_MAX) const;
		void SetPathTracingEnabled(bool bEnabled) { m_bPathTracingEnabled = bEnabled; }
		bool IsPathTracingEnabled() const { return m_bPathTracingEnabled; }
		virtual uint32_t GetOrder() const override { return 1100; }

	protected:

		bool m_bPathTracingEnabled = false;
		TOctree<size_t> m_proxyOctree{ glm::ivec3(0, 0, 0), 16536 * 16, 4 };
		Raytracing::PathTracer m_pathTracer{};
	};

	template class ECS::TSystem<PathTracerECS, PathTracerProxyData>;
}
