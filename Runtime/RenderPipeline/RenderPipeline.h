#pragma once
#include "Memory/RefPtr.hpp"
#include "AssetRegistry/RenderPipeline/RenderPipelineImporter.h"
#include "Engine/Object.h"

namespace Sailor
{
	class RenderPipelineContext
	{
		RenderPipelineContext(RenderPipelineContext&) = delete;
		RenderPipelineContext(RenderPipelineContext&&) = delete;
		RenderPipelineContext& operator=(RenderPipelineContext&) = delete;
		RenderPipelineContext& operator=(RenderPipelineContext&&) = delete;

		virtual ~RenderPipelineContext() = default;
	};

	class RenderPipelineNode
	{
	public:

		RenderPipelineNode(RenderPipelineNode&) = delete;
		RenderPipelineNode(RenderPipelineNode&&) = delete;
		RenderPipelineNode& operator=(RenderPipelineNode&) = delete;
		RenderPipelineNode& operator=(RenderPipelineNode&&) = delete;

		virtual bool Execute();
		virtual bool ParseParams(nlohmann::json params);
		virtual ~RenderPipelineNode() = default;
	};

	class RenderPipeline : public Object
	{
	public:

		virtual SAILOR_API bool IsReady() const override { return true; }

		void BuildGraph();

	protected:

		TUniquePtr<RenderPipelineContext> m_context;
		TMap <std::string, TUniquePtr<RenderPipelineNode>> m_nodes;

		TList<TUniquePtr<RenderPipelineNode>> m_graph;
	};
};
