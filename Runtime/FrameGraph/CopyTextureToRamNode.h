#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class CopyTextureToRamNode : public TFrameGraphNode<CopyTextureToRamNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		SAILOR_API RHI::RHIBufferPtr GetBuffer() { return m_cpuBuffer; }
		SAILOR_API RHI::RHITexturePtr GetTexture() { return m_texture; }

		SAILOR_API void DoOneCapture() { m_captureThisFrame.exchange(true); }

	protected:

		RHI::RHIBufferPtr m_cpuBuffer;
		RHI::RHITexturePtr m_texture;

		std::atomic<bool> m_captureThisFrame;
		static const char* m_name;
	};

	template class TFrameGraphNode<CopyTextureToRamNode>;
};
