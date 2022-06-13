#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/FrameGraph/FrameGraphAssetInfo.h"
#include "RHI/Renderer.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/BaseFrameGraphNode.h"

namespace Sailor
{
	class FrameGraph : public Object
	{
	public:

		SAILOR_API virtual bool IsReady() const override { return true; }

	protected:

		RHIFrameGraphPtr m_FrameGraph;
	};

	using FrameGraphPtr = TObjectPtr<FrameGraph>;

	class SAILOR_API FrameGraphImporter final : public TSubmodule<FrameGraphImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = TVector<uint8_t>;

		FrameGraphImporter(FrameGraphAssetInfoHandler* infoHandler);
		virtual ~FrameGraphImporter() override;

		virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		bool LoadFrameGraph_Immediate(UID uid, FrameGraphPtr& outFrameGraph);

		static void RegisterFrameGraphNode(const std::string& nodeName, std::function<FrameGraphNodePtr(void)> factoryMethod);

	protected:

		FrameGraphNodePtr CreateNode(const std::string& nodeName) const;
		static TUniquePtr<TMap<std::string, std::function<FrameGraphNodePtr(void)>>> s_pNodeFactoryMethods;
	};
}
