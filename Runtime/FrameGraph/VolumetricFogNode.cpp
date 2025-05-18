#include "VolumetricFogNode.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "AssetRegistry/AssetRegistry.h"
#include "RHI/SceneView.h"
#include "Containers/Vector.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

const char* VolumetricFogNode::m_name = "VolumetricFog";

void VolumetricFogNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList,
    RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
    SAILOR_PROFILE_FUNCTION();

    auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
    auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
    commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdCompute);

    if (!m_pComputeShader)
    {
        if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeVolumetricFog.shader"))
        {
            App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComputeShader);
        }
    }

    if (!m_pComputeShader || !m_pComputeShader->IsReady())
    {
        commands->EndDebugRegion(commandList);
        return;
    }

    if (!m_densityVolume)
    {
        constexpr int Size = 64;
        TVector<uint16_t> data;
        data.Resize(Size * Size * Size);

        for (size_t i = 0; i < data.Num(); ++i)
        {
            data[i] = 6553; // ~0.1 in half float
        }

        m_densityVolume = driver->CreateTexture(data.GetData(), data.Num() * sizeof(uint16_t),
            glm::ivec3(Size, Size, Size),
            1,
            ETextureType::Texture3D,
            ETextureFormat::R16_SFLOAT,
            ETextureFiltration::Linear,
            ETextureClamping::Clamp,
            ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit);
        driver->SetDebugName(m_densityVolume, "VolumetricFogVolume");
    }

    RHITexturePtr densityVolume = GetResolvedAttachment("densityVolume");
    if (!densityVolume)
    {
        densityVolume = m_densityVolume;
    }

    RHITexturePtr target = GetResolvedAttachment("target");

    if (!target)
    {
        commands->EndDebugRegion(commandList);
        return;
    }

    if (!m_shaderBindings)
    {
        m_shaderBindings = driver->CreateShaderBindings();
        driver->AddSamplerToShaderBindings(m_shaderBindings, "u_densityVolume", densityVolume, 0);
        driver->AddStorageImageToShaderBindings(m_shaderBindings, "u_output_image", target, 1);
    }
    else
    {
        driver->AddSamplerToShaderBindings(m_shaderBindings, "u_densityVolume", densityVolume, 0);
        driver->AddStorageImageToShaderBindings(m_shaderBindings, "u_output_image", target, 1);
    }

    m_shaderBindings->RecalculateCompatibility();

    PushConstants params{};
    params.stepSize = GetFloat("stepSize");
    params.fogColor = glm::vec3(GetVec4("fogColor"));

    glm::uvec2 dim(target->GetExtent().x, target->GetExtent().y);

    commands->ImageMemoryBarrier(commandList, densityVolume, EImageLayout::ComputeRead);
    commands->ImageMemoryBarrier(commandList, target, EImageLayout::ComputeWrite);

    commands->Dispatch(commandList, m_pComputeShader->GetComputeShaderRHI(),
        (uint32_t)glm::ceil(float(dim.x) / 16.0f),
        (uint32_t)glm::ceil(float(dim.y) / 16.0f),
        1u,
        { m_shaderBindings, sceneView.m_frameBindings },
        &params, sizeof(PushConstants));

    commands->EndDebugRegion(commandList);
}

void VolumetricFogNode::Clear()
{
    m_pComputeShader.Clear();
    m_shaderBindings.Clear();
    m_densityVolume.Clear();
}
