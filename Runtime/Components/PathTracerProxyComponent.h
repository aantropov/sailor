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

		SAILOR_API bool IsEnabled() const { return GetData().GetOptions().m_bEnabled; }
		SAILOR_API void SetEnabled(bool bEnabled);

		SAILOR_API bool ShouldRebuildEveryFrame() const { return GetData().GetOptions().m_bRebuildEveryFrame; }
		SAILOR_API void SetRebuildEveryFrame(bool bValue);

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
	func(SetRebuildEveryFrame, property("rebuildEveryFrame"), SkipCDO())
)
