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
#include "Core/Utils.h"
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

CPUPathTracerNode::CameraState& CPUPathTracerNode::GetCameraState(uint32_t cameraIndex)
{
	auto& camera = m_cameras[cameraIndex];
	if (!camera) camera = TUniquePtr<CameraState>::Make();
	m_lastCameraIndex = cameraIndex;
	return *camera;
}

bool CPUPathTracerNode::ApplyCompletedReadback(CameraState& camera,
	const RHICubemapPtr& environment, const RHICubemapPtr& diffuseEnvironment)
{
	if (!camera.m_pendingReadback) return false;
	auto& resources = *camera.m_pendingReadback;
	const auto status = resources.m_readbackCompletion->GetStatus();
	if (status == EFenceStatus::Pending) return false;

	const bool bCurrent = status == EFenceStatus::Finished &&
		resources.m_environment.m_source == environment &&
		resources.m_diffuseEnvironment.m_source == diffuseEnvironment;
	if (bCurrent)
	{
		camera.m_pathTracer.ClearRuntimeEnvironment();
		auto apply = [&](const CubemapReadbackState& readback, bool bDiffuse)
		{
			if (!readback.m_source) return;
			TVector<TVector<vec4>> faces;
			faces.Resize(6);
			const size_t facePixels = static_cast<size_t>(readback.m_extent.x) * readback.m_extent.y;
			for (uint32_t face = 0; face < 6; ++face)
			{
				const auto* src = static_cast<const uint8_t*>(readback.m_faceBuffers[face]->GetPointer());
				faces[face].Resize(facePixels);
				for (size_t i = 0; i < facePixels; ++i)
					faces[face][i] = DecodeR16G16B16A16_SFLOAT(src + i * 4 * sizeof(uint16_t));
			}
			const auto image = ConvertCubemapFacesToEquirect(faces, readback.m_extent);
			const uvec2 extent(readback.m_extent.x * 2u, readback.m_extent.y);
			if (bDiffuse) camera.m_pathTracer.SetRuntimeDiffuseEnvironmentLinear(image, extent);
			else camera.m_pathTracer.SetRuntimeEnvironmentLinear(image, extent);
		};
		apply(resources.m_environment, false);
		apply(resources.m_diffuseEnvironment, true);
	}
	else if (status == EFenceStatus::Failed)
	{
		camera.m_environmentSource.Clear();
		camera.m_diffuseEnvironmentSource.Clear();
	}
	camera.m_pendingReadback.Clear();
	return bCurrent;
}

void CPUPathTracerNode::QueueEnvironmentReadback(CameraState& camera, TRefPtr<SubmissionResources> resources,
	RHICommandListPtr commandList, const RHISceneViewSnapshot& sceneView,
	RHICubemapPtr environment, RHICubemapPtr diffuseEnvironment)
{
	if (!environment && !diffuseEnvironment)
	{
		camera.m_pendingReadback.Clear();
		camera.m_environmentSource.Clear();
		camera.m_diffuseEnvironmentSource.Clear();
		camera.m_pathTracer.ClearRuntimeEnvironment();
		return;
	}
	if (camera.m_pendingReadback) return;
	if (camera.m_environmentSource == environment && camera.m_diffuseEnvironmentSource == diffuseEnvironment &&
		sceneView.m_frame - camera.m_lastQueuedFrame < 8u)
	{
		return;
	}

	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	auto queue = [&](CubemapReadbackState& readback, RHICubemapPtr cubemap)
	{
		readback.m_source = cubemap;
		if (!cubemap) return;
		readback.m_mipLevel = 0;
		auto mip = cubemap;
		while (mip->GetExtent().x > 64)
		{
			auto next = cubemap->GetMipLevel(readback.m_mipLevel + 1);
			if (!next) break;
			++readback.m_mipLevel;
			mip = next;
		}
		readback.m_extent = uvec2(mip->GetExtent());
		readback.m_faceBuffers.Resize(6);
		const size_t size = static_cast<size_t>(readback.m_extent.x) * readback.m_extent.y * 4 * sizeof(uint16_t);
		for (uint32_t face = 0; face < 6; ++face)
		{
			auto& buffer = readback.m_faceBuffers[face];
			if (!buffer || buffer->GetSize() != size)
				buffer = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit,
					EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
			auto texture = cubemap->GetFace(face, readback.m_mipLevel);
			commands->ImageMemoryBarrier(commandList, texture, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(commandList, texture, buffer);
		}
	};
	queue(resources->m_environment, environment);
	queue(resources->m_diffuseEnvironment, diffuseEnvironment);
	commands->MemoryBarrier(commandList, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
		static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
	resources->m_readbackCompletion = sceneView.m_submissionContext->GetOrCreateFrameCompletion();
	camera.m_pendingReadback = resources;
	camera.m_environmentSource = environment;
	camera.m_diffuseEnvironmentSource = diffuseEnvironment;
	camera.m_lastQueuedFrame = sceneView.m_frame;
}

void CPUPathTracerNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();
	ResetDrawCallStats();

	auto getFloatParam = [this](const char* name, float defaultValue) -> float
	{
		const std::string key = name;
		return m_floatParams.ContainsKey(key) ? m_floatParams[key] : defaultValue;
	};

	if (getFloatParam("enabled", 0.0f) <= 0.5f || IsSceneViewDebugVisualization(sceneView.m_renderMode) ||
		!sceneView.m_submissionContext || !sceneView.m_camera || sceneView.m_pathTracerProxies.IsEmpty())
	{
		return;
	}

	auto colorResource = GetRHIResource("color");
	const auto dstSurface = colorResource.DynamicCast<RHISurface>();
	const auto dst = dstSurface ? dstSurface->GetResolved() : colorResource.DynamicCast<RHITexture>();
	const bool bUseMsaaTarget = dstSurface && dstSurface->NeedsResolve();
	if (!dst) return;

	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdTransfer);
	auto& camera = GetCameraState(sceneView.m_cameraIndex);
	auto resources = sceneView.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, sceneView.m_cameraIndex, 0);
	const auto environment = frameGraph->GetSampler("g_rawEnvCubemap").DynamicCast<RHICubemap>();
	const auto diffuseEnvironment = frameGraph->GetSampler("g_irradianceCubemap").DynamicCast<RHICubemap>();
	ApplyCompletedReadback(camera, environment, diffuseEnvironment);
	QueueEnvironmentReadback(camera, resources, commandList, sceneView, environment, diffuseEnvironment);

	const uint32_t spp = (std::max)(1u, (uint32_t)std::lround(getFloatParam("samplesPerFrame", 1.0f)));
	const uint32_t maxBounces = (std::max)(0u, (uint32_t)std::lround(getFloatParam("maxBounces", 2.0f)));
	const uint64_t maxAccumulatedSamples = (uint64_t)(std::max)(0.0f, getFloatParam("maxAccumulatedSamples", 0.0f));
	const float blend = glm::clamp(getFloatParam("blend", 1.0f), 0.0f, 1.0f);
	const float rayBiasBase = (std::max)(0.0f, getFloatParam("rayBiasBase", getFloatParam("shadowBias", 0.0f)));
	const float rayBiasScale = (std::max)(0.0f, getFloatParam("rayBiasScale", 3e-4f));
#ifdef __APPLE__
	constexpr uint64_t maxPixels = 225000ull;
	const float targetAspect = (std::max)(0.1f, (float)dst->GetExtent().x / (float)(std::max)(1, dst->GetExtent().y));
	const uint32_t maxHeight = (std::max)(1u, (uint32_t)std::floor(std::sqrt((double)maxPixels / (double)targetAspect)));
	const uint32_t runtimeHeight = (std::min)((uint32_t)(std::max)(1, dst->GetExtent().y), maxHeight);
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

	const bool bCameraChanged = !camera.m_bHasAccumulationState ||
		!NearlyEqual(camera.m_lastCameraPosition, params.m_runtimeCameraPos) ||
		!NearlyEqual(camera.m_lastCameraForward, params.m_runtimeCameraForward) ||
		!NearlyEqual(camera.m_lastCameraUp, params.m_runtimeCameraUp) ||
		!NearlyEqual(camera.m_lastCameraAspect, params.m_runtimeAspectRatio) ||
		!NearlyEqual(camera.m_lastCameraHFov, params.m_runtimeHFov);

	if (bCameraChanged)
	{
		camera.m_accumulatedImage.Clear();
		camera.m_accumulatedSamples = 0ull;
	}

	const bool bHasAccumulatedResult = camera.m_extent.x > 0u && camera.m_extent.y > 0u && camera.m_accumulatedImage.Num() > 0;
	const bool bReachedAccumulationLimit = maxAccumulatedSamples > 0ull && camera.m_accumulatedSamples >= maxAccumulatedSamples;
	const bool bShouldRenderNewSamples = !bReachedAccumulationLimit || !bHasAccumulatedResult;

	if (bShouldRenderNewSamples)
	{
		if (sceneView.m_pathTracerTLASInstances.Num() == 0 ||
			!camera.m_pathTracer.InitializeScene(sceneView.m_pathTracerTLASInstances, sceneView.m_pathTracerMaterials, sceneView.m_pathTracerLights) ||
			!camera.m_pathTracer.RenderPreparedScene(params))
		{
			commands->EndDebugRegion(commandList);
			return;
		}

		const auto& image = camera.m_pathTracer.GetLastRenderedImageLinear();
		const glm::uvec2 imageExtent = camera.m_pathTracer.GetLastRenderedExtent();
		if (image.Num() == 0 || imageExtent.x == 0 || imageExtent.y == 0)
		{
			commands->EndDebugRegion(commandList);
			return;
		}

		AccumulateImage(camera, image, imageExtent, spp);
		if (maxAccumulatedSamples > 0ull)
		{
			camera.m_accumulatedSamples = (std::min)(camera.m_accumulatedSamples, maxAccumulatedSamples);
		}
	}

	if (resources->m_imageRevision != camera.m_imageRevision)
	{
		const size_t uploadSizeRequired = camera.m_accumulatedImage.Num() * sizeof(vec4);

		if (!resources->m_uploadBuffer || resources->m_uploadBuffer->GetSize() != uploadSizeRequired)
		{
			resources->m_uploadBuffer = driver->CreateBuffer(uploadSizeRequired,
				EBufferUsageBit::BufferTransferSrc_Bit,
				EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
		}
		uint8_t* uploadPtr = reinterpret_cast<uint8_t*>(resources->m_uploadBuffer->GetPointer());

		std::memcpy(uploadPtr, camera.m_accumulatedImage.GetData(), uploadSizeRequired);

		if (!resources->m_runtimeTexture ||
			(uint32_t)resources->m_runtimeTexture->GetExtent().x != camera.m_extent.x ||
			(uint32_t)resources->m_runtimeTexture->GetExtent().y != camera.m_extent.y)
		{
			resources->m_runtimeTexture = driver->CreateTexture(
				nullptr,
				0,
				glm::ivec3((int32_t)camera.m_extent.x, (int32_t)camera.m_extent.y, 1),
				1,
				ETextureType::Texture2D,
				ETextureFormat::R32G32B32A32_SFLOAT,
				ETextureFiltration::Nearest,
				ETextureClamping::Clamp,
				ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);
			if (!resources->m_runtimeTexture)
			{
				commands->EndDebugRegion(commandList);
				return;
			}
		}

		commands->ImageMemoryBarrier(commandList, resources->m_runtimeTexture, EImageLayout::TransferDstOptimal);
		commands->CopyBufferToImage(commandList, resources->m_uploadBuffer, resources->m_runtimeTexture);
		commands->ImageMemoryBarrier(commandList, resources->m_runtimeTexture, EImageLayout::ShaderReadOnlyOptimal);
		resources->m_imageRevision = camera.m_imageRevision;
	}

	if (!resources->m_runtimeTexture || camera.m_extent.x == 0u || camera.m_extent.y == 0u || camera.m_accumulatedImage.Num() == 0)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	camera.m_lastCameraPosition = params.m_runtimeCameraPos;
	camera.m_lastCameraForward = params.m_runtimeCameraForward;
	camera.m_lastCameraUp = params.m_runtimeCameraUp;
	camera.m_lastCameraAspect = params.m_runtimeAspectRatio;
	camera.m_lastCameraHFov = params.m_runtimeHFov;
	camera.m_bHasAccumulationState = true;

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

	if (!resources->m_shaderBindings)
	{
		resources->m_shaderBindings = driver->CreateShaderBindings();
		driver->FillShadersLayout(resources->m_shaderBindings, { m_pShader->GetDebugVertexShaderRHI(), m_pShader->GetDebugFragmentShaderRHI() }, 1);
		driver->AddBufferToShaderBindings(resources->m_shaderBindings, "data", 32, 1, RHI::EShaderBindingType::UniformBuffer);
		driver->AddSamplerToShaderBindings(resources->m_shaderBindings, "currentSampler", resources->m_runtimeTexture, 0);
		resources->m_shaderBindings->RecalculateCompatibility();
	}

	if (!m_overlayMaterial || !m_overlayMaterialMsaa)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState overlayState{ false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending, EFillMode::Fill, 0, false };
		RenderState overlayMsaaState{ false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending, EFillMode::Fill, 0, true };
		if (!m_overlayMaterial) { m_overlayMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, overlayState, m_pShader, resources->m_shaderBindings); }
		if (!m_overlayMaterialMsaa) { m_overlayMaterialMsaa = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, overlayMsaaState, m_pShader, resources->m_shaderBindings); }
	}
	if (!m_overlayMaterial || !m_overlayMaterialMsaa)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	driver->UpdateShaderBinding(resources->m_shaderBindings, "currentSampler", resources->m_runtimeTexture, 0);
	commands->SetMaterialParameter(commandList, resources->m_shaderBindings, "data.fitScaleOffset", glm::vec4(1.0f, 1.0f, 0.0f, 0.0f));
	commands->SetMaterialParameter(commandList, resources->m_shaderBindings, "data.blend", blend);
	commands->MemoryBarrier(commandList, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
		static_cast<EAccessFlags>(EAccessBit::UniformRead_Bit));

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
	if (commands->BindShaderBindings(commandList, bUseMsaaTarget ? m_overlayMaterialMsaa : m_overlayMaterial, { sceneView.m_frameBindings, resources->m_shaderBindings }))
	{
		commands->SetViewport(commandList,
			0.0f, 0.0f,
			(float)dst->GetExtent().x, (float)dst->GetExtent().y,
			glm::vec2(0.0f, 0.0f),
			glm::vec2((float)dst->GetExtent().x, (float)dst->GetExtent().y),
			0.0f, 1.0f);
		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		RecordDrawCallStats(1);
	}
	commands->EndRenderPass(commandList);
	commands->EndDebugRegion(commandList);
}

void CPUPathTracerNode::AccumulateImage(CameraState& camera, const TVector<glm::vec4>& image, glm::uvec2 extent, uint32_t samples)
{
	camera.m_imageRevision = ++m_nextImageRevision;
	if (camera.m_extent != extent || camera.m_accumulatedSamples == 0ull)
	{
		camera.m_extent = extent;
		camera.m_accumulatedImage = image;
		camera.m_accumulatedSamples = samples;
		return;
	}

	const float currentSamples = static_cast<float>(camera.m_accumulatedSamples);
	const float newSamples = static_cast<float>(samples);
	for (size_t i = 0; i < image.Num(); ++i)
	{
		camera.m_accumulatedImage[i] = (camera.m_accumulatedImage[i] * currentSamples + image[i] * newSamples) / (currentSamples + newSamples);
	}
	camera.m_accumulatedSamples += samples;
}

bool CPUPathTracerNode::GetLastRenderedImage(TVector<glm::u8vec4>& outImage, glm::uvec2& outExtent) const
{
	const auto found = m_cameras.Find(m_lastCameraIndex);
	if (found == m_cameras.end()) return false;
	const auto& camera = *found.Value();
	const auto& image = camera.m_accumulatedImage;
	const glm::uvec2 extent = camera.m_extent;
	if (extent.x == 0 || extent.y == 0 || image.Num() == 0)
	{
		return false;
	}

	if (image.Num() < (size_t)extent.x * (size_t)extent.y)
	{
		return false;
	}

	outExtent = extent;
	outImage.Resize(image.Num());
	for (size_t i = 0; i < image.Num(); ++i)
	{
		outImage[i] = Utils::LinearToSRGB8(image[i]);
	}
	return true;
}

void CPUPathTracerNode::Clear()
{
	m_cameras.Clear();
	m_lastCameraIndex = 0;
	m_overlayMaterial.Clear();
	m_overlayMaterialMsaa.Clear();
	m_pShader.Clear();
}
