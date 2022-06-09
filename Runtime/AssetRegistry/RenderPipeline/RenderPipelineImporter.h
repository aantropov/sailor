#pragma once
#include "Core/Defines.h"
#include <string>
#include "Containers/Vector.h"
#include <nlohmann_json/include/nlohmann/json.hpp>
#include "Core/Submodule.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/WeakPtr.hpp"
#include "AssetRegistry/RenderPipeline/RenderPipelineAssetInfo.h"
#include "RHI/Renderer.h"
#include "RHI/RenderPipeline/RenderPipeline.h"

namespace Sailor
{
	class RenderPipeline : public Object
	{
	public:

		SAILOR_API virtual bool IsReady() const override { return true; }

	protected:

		RHI::RHIRenderPipelinePtr m_renderPipeline;
	};

	using RenderPipelinePtr = TObjectPtr<RenderPipeline>;

	class SAILOR_API RenderPipelineImporter final : public TSubmodule<RenderPipelineImporter>, public IAssetInfoHandlerListener
	{
	public:
		using ByteCode = TVector<uint8_t>;

		RenderPipelineImporter(RenderPipelineAssetInfoHandler* infoHandler);
		virtual ~RenderPipelineImporter() override;

		virtual void OnImportAsset(AssetInfoPtr assetInfo) override;
		virtual void OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired) override;

		bool LoadRenderPipeline_Immediate(UID uid, RenderPipelinePtr& outRenderPipeline);

	protected:

	};
}
