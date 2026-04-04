#include "CPUPathTracerNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/CommandList.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "RHI/Cubemap.h"
#include "RHI/Buffer.h"
#include "RHI/VertexDescription.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "Core/LogMacros.h"
#include "Containers/Hash.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

namespace
{
	static bool NearlyEqual(float a, float b, float epsilon = 1e-4f)
	{
		return std::abs(a - b) <= epsilon;
	}

	static bool NearlyEqual(const vec3& a, const vec3& b, float epsilon = 1e-4f)
	{
		return glm::length(a - b) <= epsilon;
	}

	static u8vec4 LinearToU8(const vec4& color)
	{
		const vec4 clamped = glm::clamp(color, vec4(0.0f), vec4(1.0f));
		return u8vec4(
			(uint8_t)glm::round(clamped.r * 255.0f),
			(uint8_t)glm::round(clamped.g * 255.0f),
			(uint8_t)glm::round(clamped.b * 255.0f),
			(uint8_t)glm::round(clamped.a * 255.0f));
	}

	static float HalfToFloat(uint16_t h)
	{
		const uint32_t sign = (uint32_t)(h & 0x8000u) << 16u;
		uint32_t exp = (h >> 10u) & 0x1Fu;
		uint32_t mant = h & 0x03FFu;

		uint32_t out = 0;
		if (exp == 0)
		{
			if (mant == 0)
			{
				out = sign;
			}
			else
			{
				exp = 1;
				while ((mant & 0x0400u) == 0)
				{
					mant <<= 1u;
					--exp;
				}
				mant &= 0x03FFu;
				out = sign | ((exp + 112u) << 23u) | (mant << 13u);
			}
		}
		else if (exp == 31)
		{
			out = sign | 0x7F800000u | (mant << 13u);
		}
		else
		{
			out = sign | ((exp + 112u) << 23u) | (mant << 13u);
		}

		float result = 0.0f;
		std::memcpy(&result, &out, sizeof(float));
		return result;
	}

	static vec4 DecodeR16G16B16A16_SFLOAT(const uint8_t* src)
	{
		const uint16_t* halfs = reinterpret_cast<const uint16_t*>(src);
		return vec4(HalfToFloat(halfs[0]), HalfToFloat(halfs[1]), HalfToFloat(halfs[2]), HalfToFloat(halfs[3]));
	}

	static vec3 SampleCubemapFaces(const TVector<TVector<vec4>>& faces, const glm::uvec2& extent, const vec3& direction)
	{
		if (faces.Num() < 6 || extent.x == 0 || extent.y == 0)
		{
			return vec3(0.0f);
		}

		const vec3 dir = glm::normalize(direction);
		const vec3 absDir = glm::abs(dir);

		uint32_t face = 0;
		float u = 0.0f;
		float v = 0.0f;

		if (absDir.x >= absDir.y && absDir.x >= absDir.z)
		{
			if (dir.x >= 0.0f)
			{
				face = 0;
				u = -dir.z / absDir.x;
				v = dir.y / absDir.x;
			}
			else
			{
				face = 1;
				u = dir.z / absDir.x;
				v = dir.y / absDir.x;
			}
		}
		else if (absDir.y >= absDir.x && absDir.y >= absDir.z)
		{
			if (dir.y >= 0.0f)
			{
				face = 2;
				u = dir.x / absDir.y;
				v = -dir.z / absDir.y;
			}
			else
			{
				face = 3;
				u = dir.x / absDir.y;
				v = dir.z / absDir.y;
			}
		}
		else
		{
			if (dir.z >= 0.0f)
			{
				face = 4;
				u = dir.x / absDir.z;
				v = dir.y / absDir.z;
			}
			else
			{
				face = 5;
				u = -dir.x / absDir.z;
				v = dir.y / absDir.z;
			}
		}

		const float s = glm::clamp((u + 1.0f) * 0.5f, 0.0f, 1.0f);
		const float t = glm::clamp((1.0f - v) * 0.5f, 0.0f, 1.0f);
		const uint32_t x = (uint32_t)glm::clamp((int32_t)std::lround(s * (float)(extent.x - 1u)), 0, (int32_t)extent.x - 1);
		const uint32_t y = (uint32_t)glm::clamp((int32_t)std::lround(t * (float)(extent.y - 1u)), 0, (int32_t)extent.y - 1);
		return vec3(faces[face][x + y * extent.x]);
	}

	static TVector<vec4> ConvertCubemapFacesToEquirect(const TVector<TVector<vec4>>& faces, const glm::uvec2& faceExtent)
	{
		const glm::uvec2 outExtent((uint32_t)(std::max)(1u, faceExtent.x * 2u), (uint32_t)(std::max)(1u, faceExtent.y));
		TVector<vec4> result(outExtent.x * outExtent.y);

		for (uint32_t y = 0; y < outExtent.y; ++y)
		{
			for (uint32_t x = 0; x < outExtent.x; ++x)
			{
				const float u = ((float)x + 0.5f) / (float)outExtent.x;
				const float v = ((float)y + 0.5f) / (float)outExtent.y;
				const float phi = u * 2.0f * Sailor::Math::Pi - Sailor::Math::Pi;
				const float theta = v * Sailor::Math::Pi;
				const vec3 dir(
					std::cos(phi) * std::sin(theta),
					std::cos(theta),
					std::sin(phi) * std::sin(theta));
				result[x + y * outExtent.x] = vec4(SampleCubemapFaces(faces, faceExtent, dir), 1.0f);
			}
		}

		return result;
	}
}

#ifndef _SAILOR_IMPORT_
const char* CPUPathTracerNode::m_name = "CPUPathTracerNode";
#endif

void CPUPathTracerNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdTransfer);

	auto getFloatParam = [this](const char* name, float defaultValue) -> float
	{
		const std::string key = name;
		return m_floatParams.ContainsKey(key) ? m_floatParams[key] : defaultValue;
	};

	const bool bEnabled = getFloatParam("enabled", 0.0f) > 0.5f;
	if (!bEnabled)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	auto queueCubemapReadback = [&](CubemapReadbackState& state, RHI::RHICubemapPtr cubemap)
	{
		if (!cubemap)
		{
			state = {};
			return;
		}

		const uint32_t EnvUpdateIntervalFrames = 8u;
		const uint32_t TargetEnvResolution = 64u;
		bool bForceRefresh = false;

		if (state.m_source != cubemap)
		{
			state = {};
			state.m_source = cubemap;
			state.m_faceBuffers.Resize(6);
			bForceRefresh = true;
		}

		uint32_t mipLevel = 0u;
		auto mipTexture = cubemap;
		while (mipTexture && (uint32_t)mipTexture->GetExtent().x > TargetEnvResolution)
		{
			auto nextMip = cubemap->GetMipLevel(mipLevel + 1u);
			if (!nextMip)
			{
				break;
			}
			++mipLevel;
			mipTexture = nextMip;
		}

		const glm::uvec2 mipExtent((uint32_t)mipTexture->GetExtent().x, (uint32_t)mipTexture->GetExtent().y);
		if (state.m_mipLevel != mipLevel || state.m_extent != mipExtent)
		{
			state.m_mipLevel = mipLevel;
			state.m_extent = mipExtent;
			state.m_faceBuffers.Resize(6);
			bForceRefresh = true;
		}

		const bool bIntervalRefresh = !bForceRefresh && (sceneView.m_frame - state.m_lastQueuedFrame) >= EnvUpdateIntervalFrames;
		if (!bForceRefresh && !bIntervalRefresh)
		{
			return;
		}

		const uint64_t requiredSize = (uint64_t)state.m_extent.x * (uint64_t)state.m_extent.y * 4ull * sizeof(uint16_t);
		for (uint32_t face = 0; face < 6; ++face)
		{
			if (!state.m_faceBuffers[face] || state.m_faceBuffers[face]->GetSize() != requiredSize)
			{
				state.m_faceBuffers[face] = driver->CreateBuffer(requiredSize,
					EBufferUsageBit::BufferTransferDst_Bit,
					EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
			}

			auto faceTexture = cubemap->GetFace(face, mipLevel);
			commands->ImageMemoryBarrier(commandList, faceTexture, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(commandList, faceTexture, state.m_faceBuffers[face]);
		}

		state.m_lastQueuedFrame = sceneView.m_frame;
		state.m_bPendingGpuReadback = true;
	};

	auto applyCubemapReadback = [&](CubemapReadbackState& state, bool bDiffuse)
	{
		if (!state.m_bPendingGpuReadback)
		{
			return;
		}

		TVector<TVector<vec4>> faces;
		faces.Resize(6);
		const uint64_t pixelStride = 4ull * sizeof(uint16_t);
		const uint64_t facePixels = (uint64_t)state.m_extent.x * (uint64_t)state.m_extent.y;
		for (uint32_t face = 0; face < 6; ++face)
		{
			const uint8_t* src = reinterpret_cast<const uint8_t*>(state.m_faceBuffers[face]->GetPointer());
			faces[face].Resize(facePixels);
			for (uint64_t i = 0; i < facePixels; ++i)
			{
				faces[face][i] = DecodeR16G16B16A16_SFLOAT(src + i * pixelStride);
			}
		}

		const TVector<vec4> equirect = ConvertCubemapFacesToEquirect(faces, state.m_extent);
		const glm::uvec2 equirectExtent((uint32_t)(std::max)(1u, state.m_extent.x * 2u), (uint32_t)(std::max)(1u, state.m_extent.y));
		if (bDiffuse)
		{
			m_pathTracer.SetRuntimeDiffuseEnvironmentLinear(equirect, equirectExtent);
		}
		else
		{
			m_pathTracer.SetRuntimeEnvironmentLinear(equirect, equirectExtent);
		}

		state.m_bPendingGpuReadback = false;
	};

	RHI::RHIResourcePtr colorResource = GetRHIResource("color");
	RHI::RHISurfacePtr dstSurface = colorResource.DynamicCast<RHISurface>();
	RHI::RHITexturePtr dst = dstSurface ? dstSurface->GetResolved() : colorResource.DynamicCast<RHI::RHITexture>();
	const bool bUseMsaaTarget = dstSurface && dstSurface->NeedsResolve();

	if (!dst || sceneView.m_pathTracerProxies.Num() == 0)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	if (!sceneView.m_camera)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	applyCubemapReadback(m_environmentReadback, false);
	applyCubemapReadback(m_diffuseEnvironmentReadback, true);
	queueCubemapReadback(m_environmentReadback, frameGraph->GetSampler("g_rawEnvCubemap").DynamicCast<RHICubemap>());
	queueCubemapReadback(m_diffuseEnvironmentReadback, frameGraph->GetSampler("g_irradianceCubemap").DynamicCast<RHICubemap>());

	const uint32_t spp = (std::max)(1u, (uint32_t)std::lround(getFloatParam("samplesPerFrame", 1.0f)));
	const uint32_t maxBounces = (std::max)(0u, (uint32_t)std::lround(getFloatParam("maxBounces", 2.0f)));
	const uint64_t maxAccumulatedSamples = (uint64_t)(std::max)(0.0f, getFloatParam("maxAccumulatedSamples", 0.0f));
	const float blend = glm::clamp(getFloatParam("blend", 1.0f), 0.0f, 1.0f);
	const float rayBiasBase = (std::max)(0.0f, getFloatParam("rayBiasBase", getFloatParam("shadowBias", 0.0f)));
	const float rayBiasScale = (std::max)(0.0f, getFloatParam("rayBiasScale", 3e-4f));
#ifdef __APPLE__
	const uint64_t maxUploadBytes = 900000ull;
	const float targetAspect = (std::max)(0.1f, (float)dst->GetExtent().x / (float)(std::max)(1, dst->GetExtent().y));
	const uint64_t maxPixelsByUpload = (std::max)(1ull, maxUploadBytes / (uint64_t)sizeof(u8vec4));
	const uint32_t maxHeightByUpload = (std::max)(1u, (uint32_t)std::floor(std::sqrt((double)maxPixelsByUpload / (double)targetAspect)));
	const uint32_t runtimeHeight = (std::min)((uint32_t)(std::max)(1, dst->GetExtent().y), maxHeightByUpload);
#else
	const uint32_t runtimeHeight = (uint32_t)(std::max)(1, dst->GetExtent().y);
#endif
	Raytracing::PathTracer::Params params{};
	params.m_height = runtimeHeight;
	params.m_maxBounces = maxBounces;
	params.m_output.clear();
	params.m_msaa = spp <= 32 ? (std::min)(4u, spp) : 8u;
	params.m_numSamples = (std::max)(1u, (uint32_t)std::lround(spp / (float)params.m_msaa));
	params.m_numAmbientSamples = params.m_numSamples;
	params.m_rayBiasBase = rayBiasBase;
	params.m_rayBiasScale = rayBiasScale;
	params.m_bUseRuntimeCamera = true;
	params.m_runtimeCameraPos = vec3(sceneView.m_cameraTransform.m_position);
	params.m_runtimeCameraForward = glm::normalize(sceneView.m_cameraTransform.GetForward());
	params.m_runtimeCameraUp = glm::normalize(sceneView.m_cameraTransform.GetUp());
	const float fallbackAspect = (std::max)(0.1f, (float)dst->GetExtent().x / (float)(std::max)(1, dst->GetExtent().y));
	const float aspect = (std::max)(0.1f, sceneView.m_camera->GetAspect() > 0.0f ? sceneView.m_camera->GetAspect() : fallbackAspect);
	const float verticalFov = glm::radians(sceneView.m_camera->GetFov());
	params.m_runtimeAspectRatio = aspect;
	params.m_runtimeHFov = 2.0f * atan(tan(verticalFov * 0.5f) * aspect);

	const bool bCameraChanged = !m_bHasAccumulationState ||
		!NearlyEqual(m_lastCameraPosition, params.m_runtimeCameraPos) ||
		!NearlyEqual(m_lastCameraForward, params.m_runtimeCameraForward) ||
		!NearlyEqual(m_lastCameraUp, params.m_runtimeCameraUp) ||
		!NearlyEqual(m_lastCameraAspect, params.m_runtimeAspectRatio) ||
		!NearlyEqual(m_lastCameraHFov, params.m_runtimeHFov);

	if (bCameraChanged)
	{
		m_accumulatedImage.Clear();
		m_accumulatedDisplayImage.Clear();
		m_accumulatedSamples = 0ull;
	}

	const bool bHasAccumulatedResult = m_extent.x > 0u && m_extent.y > 0u && m_accumulatedDisplayImage.Num() > 0;
	const bool bReachedAccumulationLimit = maxAccumulatedSamples > 0ull && m_accumulatedSamples >= maxAccumulatedSamples;
	const bool bShouldRenderNewSamples = !bReachedAccumulationLimit || !bHasAccumulatedResult;

	if (bShouldRenderNewSamples)
	{
		if (sceneView.m_pathTracerTLASInstances.Num() == 0 ||
			!m_pathTracer.InitializeScene(sceneView.m_pathTracerTLASInstances, sceneView.m_pathTracerMaterials, sceneView.m_pathTracerLights) ||
			!m_pathTracer.RenderPreparedScene(params))
		{
			commands->EndDebugRegion(commandList);
			return;
		}

		const auto& image = m_pathTracer.GetLastRenderedImage();
		const glm::uvec2 imageExtent = m_pathTracer.GetLastRenderedExtent();
		if (image.Num() == 0 || imageExtent.x == 0 || imageExtent.y == 0)
		{
			commands->EndDebugRegion(commandList);
			return;
		}

		if (m_extent != imageExtent)
		{
			m_extent = imageExtent;
			m_accumulatedImage.Clear();
			m_accumulatedDisplayImage.Clear();
			m_accumulatedSamples = 0ull;
		}

		if (m_accumulatedImage.Num() != image.Num())
		{
			m_accumulatedImage.Resize(image.Num());
			m_accumulatedDisplayImage.Resize(image.Num());
			std::fill(m_accumulatedImage.begin(), m_accumulatedImage.end(), vec4(0.0f));
			std::fill(m_accumulatedDisplayImage.begin(), m_accumulatedDisplayImage.end(), u8vec4(0u));
			m_accumulatedSamples = 0ull;
		}

		const float currentSamples = (float)m_accumulatedSamples;
		const float newSamples = (float)spp;
		const float totalSamples = currentSamples + newSamples;
		for (size_t i = 0; i < image.Num(); ++i)
		{
			const u8vec4 src = image[i];
			const vec4 newColor(
				(float)src.r / 255.0f,
				(float)src.g / 255.0f,
				(float)src.b / 255.0f,
				(float)src.a / 255.0f);

			if (m_accumulatedSamples == 0ull)
			{
				m_accumulatedImage[i] = newColor;
			}
			else
			{
				m_accumulatedImage[i] = (m_accumulatedImage[i] * currentSamples + newColor * newSamples) / totalSamples;
			}

			m_accumulatedDisplayImage[i] = LinearToU8(m_accumulatedImage[i]);
		}
		m_accumulatedSamples += spp;
		if (maxAccumulatedSamples > 0ull)
		{
			m_accumulatedSamples = (std::min)(m_accumulatedSamples, maxAccumulatedSamples);
		}

		const size_t uploadSizeRequired = m_accumulatedDisplayImage.Num() * sizeof(u8vec4);

		if (!m_uploadBuffer || m_uploadBuffer->GetSize() != uploadSizeRequired)
		{
			m_uploadBuffer = driver->CreateBuffer(uploadSizeRequired,
				EBufferUsageBit::BufferTransferSrc_Bit,
				EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
		}
		uint8_t* uploadPtr = reinterpret_cast<uint8_t*>(m_uploadBuffer->GetPointer());

		std::memcpy(uploadPtr, m_accumulatedDisplayImage.GetData(), uploadSizeRequired);

		if (!m_runtimeTexture ||
			(uint32_t)m_runtimeTexture->GetExtent().x != m_extent.x ||
			(uint32_t)m_runtimeTexture->GetExtent().y != m_extent.y)
		{
			m_runtimeTexture = driver->CreateTexture(
				nullptr,
				0,
				glm::ivec3((int32_t)m_extent.x, (int32_t)m_extent.y, 1),
				1,
				ETextureType::Texture2D,
				ETextureFormat::R8G8B8A8_UNORM,
				ETextureFiltration::Nearest,
				ETextureClamping::Clamp,
				ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);
			if (!m_runtimeTexture)
			{
				commands->EndDebugRegion(commandList);
				return;
			}
		}

		commands->ImageMemoryBarrier(commandList, m_runtimeTexture, EImageLayout::TransferDstOptimal);
		commands->CopyBufferToImage(commandList, m_uploadBuffer, m_runtimeTexture);
		commands->ImageMemoryBarrier(commandList, m_runtimeTexture, EImageLayout::ShaderReadOnlyOptimal);
	}

	if (!m_runtimeTexture || m_extent.x == 0u || m_extent.y == 0u || m_accumulatedDisplayImage.Num() == 0)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	m_lastCameraPosition = params.m_runtimeCameraPos;
	m_lastCameraForward = params.m_runtimeCameraForward;
	m_lastCameraUp = params.m_runtimeCameraUp;
	m_lastCameraAspect = params.m_runtimeAspectRatio;
	m_lastCameraHFov = params.m_runtimeHFov;
	m_bHasAccumulationState = true;

	if (!m_pShader.IsInited())
	{
		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/PathTracerComposite.shader"))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pShader, {});
		}
	}
	if (!m_pShader || !m_pShader->IsReady())
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	const glm::uvec2 dstExtent((uint32_t)dst->GetExtent().x, (uint32_t)dst->GetExtent().y);
	if (!m_currentFrameTexture ||
		(uint32_t)m_currentFrameTexture->GetExtent().x != dstExtent.x ||
		(uint32_t)m_currentFrameTexture->GetExtent().y != dstExtent.y)
	{
		m_currentFrameTexture = driver->CreateTexture(
			nullptr,
			0,
			glm::ivec3((int32_t)dstExtent.x, (int32_t)dstExtent.y, 1),
			1,
			ETextureType::Texture2D,
			ETextureFormat::R8G8B8A8_UNORM,
			ETextureFiltration::Nearest,
			ETextureClamping::Clamp,
			ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);
	}
	if (!m_currentFrameTexture)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	commands->ImageMemoryBarrier(commandList, m_runtimeTexture, EImageLayout::TransferSrcOptimal);
	commands->ImageMemoryBarrier(commandList, m_currentFrameTexture, EImageLayout::TransferDstOptimal);
	const glm::ivec4 srcRegion(0, 0, (int32_t)m_extent.x, (int32_t)m_extent.y);
	const glm::ivec4 dstRegion(0, 0, (int32_t)dstExtent.x, (int32_t)dstExtent.y);
	commands->BlitImage(commandList, m_runtimeTexture, m_currentFrameTexture, srcRegion, dstRegion, ETextureFiltration::Nearest);
	commands->ImageMemoryBarrier(commandList, m_runtimeTexture, EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, m_currentFrameTexture, EImageLayout::ShaderReadOnlyOptimal);

	if (!m_shaderBindings)
	{
		m_shaderBindings = driver->CreateShaderBindings();
		driver->FillShadersLayout(m_shaderBindings, { m_pShader->GetDebugVertexShaderRHI(), m_pShader->GetDebugFragmentShaderRHI() }, 1);
		driver->AddBufferToShaderBindings(m_shaderBindings, "data", 32, 1, RHI::EShaderBindingType::UniformBuffer);
		driver->AddSamplerToShaderBindings(m_shaderBindings, "currentSampler", m_currentFrameTexture, 0);
		m_shaderBindings->RecalculateCompatibility();
	}

	if (!m_overlayMaterial || !m_overlayMaterialMsaa)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState overlayState{ false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending, EFillMode::Fill, 0, false };
		RenderState overlayMsaaState{ false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending, EFillMode::Fill, 0, true };
		if (!m_overlayMaterial) { m_overlayMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, overlayState, m_pShader, m_shaderBindings); }
		if (!m_overlayMaterialMsaa) { m_overlayMaterialMsaa = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, overlayMsaaState, m_pShader, m_shaderBindings); }
	}
	if (!m_overlayMaterial || !m_overlayMaterialMsaa)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	driver->UpdateShaderBinding(m_shaderBindings, "currentSampler", m_currentFrameTexture, 0);
	commands->SetMaterialParameter(commandList, m_shaderBindings, "data.fitScaleOffset", glm::vec4(1.0f, 1.0f, 0.0f, 0.0f));
	commands->SetMaterialParameter(commandList, m_shaderBindings, "data.blend", blend);

	auto mesh = frameGraph->GetFullscreenNdcQuad();
	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	if (bUseMsaaTarget)
	{
		commands->ImageMemoryBarrier(commandList, dstSurface->GetTarget(), EImageLayout::ColorAttachmentOptimal);
		commands->BeginRenderPass(commandList,
			TVector<RHI::RHISurfacePtr>{dstSurface},
			nullptr,
			glm::vec4(0, 0, dst->GetExtent().x, dst->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			0.0f,
			false);
		commands->BindMaterial(commandList, m_overlayMaterialMsaa);
	}
	else
	{
		commands->ImageMemoryBarrier(commandList, dst, EImageLayout::ColorAttachmentOptimal);
		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{dst},
			nullptr,
			glm::vec4(0, 0, dst->GetExtent().x, dst->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			0.0f,
			false);
		commands->BindMaterial(commandList, m_overlayMaterial);
	}
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	commands->BindShaderBindings(commandList, bUseMsaaTarget ? m_overlayMaterialMsaa : m_overlayMaterial, { sceneView.m_frameBindings, m_shaderBindings });
	commands->SetViewport(commandList,
		0.0f, 0.0f,
		(float)dst->GetExtent().x, (float)dst->GetExtent().y,
		glm::vec2(0.0f, 0.0f),
		glm::vec2((float)dst->GetExtent().x, (float)dst->GetExtent().y),
		0.0f, 1.0f);
	commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
	commands->EndRenderPass(commandList);
	commands->EndDebugRegion(commandList);
}

bool CPUPathTracerNode::GetLastRenderedImage(TVector<glm::u8vec4>& outImage, glm::uvec2& outExtent) const
{
	const auto& image = m_accumulatedDisplayImage;
	const glm::uvec2 extent = m_extent;
	if (extent.x == 0 || extent.y == 0 || image.Num() == 0)
	{
		return false;
	}

	if (image.Num() < (size_t)extent.x * (size_t)extent.y)
	{
		return false;
	}

	outExtent = extent;
	outImage = image;
	return true;
}

void CPUPathTracerNode::Clear()
{
	m_extent = glm::uvec2(0u, 0u);
	m_accumulatedImage.Clear();
	m_accumulatedDisplayImage.Clear();
	m_accumulatedSamples = 0ull;
	m_bHasAccumulationState = false;
	m_lastCameraPosition = glm::vec3(0.0f);
	m_lastCameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
	m_lastCameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
	m_lastCameraAspect = 0.0f;
	m_lastCameraHFov = 0.0f;
	m_runtimeTexture.Clear();
	m_currentFrameTexture.Clear();
	m_uploadBuffer.Clear();
	m_shaderBindings.Clear();
	m_overlayMaterial.Clear();
	m_overlayMaterialMsaa.Clear();
	m_pShader.Clear();
	m_pathTracer.ClearRuntimeEnvironment();
}
