#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class EditorReadbackNode : public TFrameGraphNode<EditorReadbackNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		SAILOR_API RHI::RHIBufferPtr GetBuffer() const { return m_cpuBuffer; }
		SAILOR_API RHI::RHITexturePtr GetTexture() const { return m_texture; }
		SAILOR_API uint32_t GetBytesPerRow() const { return m_bytesPerRow; }

	protected:
		RHI::RHIBufferPtr m_cpuBuffer;
		RHI::RHITexturePtr m_texture;
		uint32_t m_bytesPerRow = 0;

		static const char* m_name;
	};

	template class TFrameGraphNode<EditorReadbackNode>;
}
