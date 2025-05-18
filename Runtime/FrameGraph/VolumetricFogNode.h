#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
    class VolumetricFogNode : public TFrameGraphNode<VolumetricFogNode>
    {
    public:
        SAILOR_API static const char* GetName() { return m_name; }

        SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph,
            RHI::RHICommandListPtr transferCommandList,
            RHI::RHICommandListPtr commandList,
            const RHI::RHISceneViewSnapshot& sceneView) override;
        SAILOR_API virtual void Clear() override;

    protected:
        struct PushConstants
        {
            float stepSize = 0.1f;
            alignas(16) glm::vec3 fogColor = glm::vec3(1.0f);
        };

        static const char* m_name;

        ShaderSetPtr m_pComputeShader{};
        RHI::RHIShaderBindingSetPtr m_shaderBindings{};
        RHI::RHITexturePtr m_densityVolume{};
    };

    template class TFrameGraphNode<VolumetricFogNode>;
}
