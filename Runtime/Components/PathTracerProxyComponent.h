#pragma once
#include "Sailor.h"
#include "Components/Component.h"
#include "Engine/Types.h"
#include "ECS/PathTracerECS.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Components/MeshRendererComponent.h"

namespace Sailor
{
	namespace Raytracing
	{
		class PathTracer;
	}

	class PathTracerProxyComponent : public Component
	{
		SAILOR_REFLECTABLE(PathTracerProxyComponent)

	public:

		SAILOR_API virtual void Initialize() override;
		SAILOR_API virtual void EndPlay() override;

		SAILOR_API __forceinline PathTracerProxyData& GetData();
		SAILOR_API __forceinline const PathTracerProxyData& GetData() const;

		SAILOR_API ModelPtr GetModel() const;
		SAILOR_API void SetModel(const ModelPtr& pModel);
		SAILOR_API void LoadModel(const std::string& path);

		SAILOR_API bool IsEnabled() const { return GetData().GetOptions().m_bEnabled; }
		SAILOR_API void SetEnabled(bool bEnabled);

		SAILOR_API bool ShouldRebuildEveryFrame() const { return GetData().GetOptions().m_bRebuildEveryFrame; }
		SAILOR_API void SetRebuildEveryFrame(bool bValue);

		SAILOR_API uint32_t GetMaxBounces() const { return GetData().GetOptions().m_maxBounces; }
		SAILOR_API void SetMaxBounces(uint32_t value);

		SAILOR_API uint32_t GetSamplesPerPixel() const { return GetData().GetOptions().m_samplesPerPixel; }
		SAILOR_API void SetSamplesPerPixel(uint32_t value);

		SAILOR_API const TSharedPtr<Raytracing::BVH>& GetBVH() const { return GetData().GetBVH(); }
		SAILOR_API const TVector<Math::Triangle>& GetTriangles() const { return GetData().GetTriangles(); }
		SAILOR_API const TVector<MaterialPtr>& GetMaterials() const { return GetData().GetMaterials(); }
		SAILOR_API const TVector<TMap<std::string, TexturePtr>>& GetTextureBindings() const { return GetData().GetTextureBindings(); }
		SAILOR_API const TVector<Raytracing::LightProxy>& GetLightProxies() const { return GetData().GetLightProxies(); }
		SAILOR_API bool InitializePathTracer(Raytracing::PathTracer& outPathTracer);
		SAILOR_API bool RenderScene(const Raytracing::PathTracer::Params& params);
		SAILOR_API void SetPathTracingEnabled(bool bEnabled);
		SAILOR_API bool IsPathTracingEnabled() const;

	protected:

		size_t m_handle = (size_t)(-1);
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::PathTracerProxyComponent, bases<Sailor::Component>),

	func(IsEnabled, property("enabled"), SkipCDO()),
	func(SetEnabled, property("enabled"), SkipCDO()),

	func(ShouldRebuildEveryFrame, property("rebuildEveryFrame"), SkipCDO()),
	func(SetRebuildEveryFrame, property("rebuildEveryFrame"), SkipCDO()),

	func(GetMaxBounces, property("maxBounces"), SkipCDO()),
	func(SetMaxBounces, property("maxBounces"), SkipCDO()),

	func(GetSamplesPerPixel, property("samplesPerPixel"), SkipCDO()),
	func(SetSamplesPerPixel, property("samplesPerPixel"), SkipCDO())
)
