#include "PathTracerPassNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
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
#include "Containers/Hash.h"
#include <cmath>
#include <cstring>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* PathTracerPassNode::m_name = "PathTracerPass";
#endif

void PathTracerPassNode::ResetAccumulation()
{
	m_accumulatedFrames = 0;
	m_accumulatedSampleWeight = 0.0;
	m_upload.Clear();
	m_extent = glm::uvec2(0u, 0u);
	m_historyReadIndex = 0;
}

void PathTracerPassNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdTransfer);
	(void)transferCommandList;

	auto getFloatParam = [this](const char* name, float defaultValue) -> float
	{
		const std::string key = name;
		return m_floatParams.ContainsKey(key) ? m_floatParams[key] : defaultValue;
	};

	const bool bEnabled = getFloatParam("enabled", 0.0f) > 0.5f;
	const bool bDebugResets = getFloatParam("debugResets", 0.0f) > 0.5f;
	const uint32_t selectedCameraIndex = (std::max)(0u, (uint32_t)std::lround(getFloatParam("cameraIndex", 0.0f)));
	if (!bEnabled)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	if (sceneView.m_cameraIndex != selectedCameraIndex)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	RHI::RHIResourcePtr colorResource = GetRHIResource("color");
	if (!colorResource)
	{
		for (const auto& r : m_unresolvedResourceParams)
		{
			if (r.First() == "color")
			{
				if (auto surface = frameGraph->GetSurface(*r.Second()))
				{
					colorResource = surface;
				}
				else
				{
					colorResource = frameGraph->GetRenderTarget(*r.Second());
				}
				break;
			}
		}
	}

	RHI::RHISurfacePtr dstSurface = colorResource.DynamicCast<RHISurface>();
	RHI::RHITexturePtr dst = dstSurface ? dstSurface->GetResolved() : colorResource.DynamicCast<RHI::RHITexture>();
	const bool bUseMsaaTarget = dstSurface && dstSurface->NeedsResolve();

	if (!dst || sceneView.m_pathTracerProxies.Num() == 0)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	auto resolveEnvironmentResource = [&](const char* name, RHI::RHITexturePtr& outTexture, RHI::RHICubemapPtr& outCubemap)
	{
		RHI::RHIResourcePtr resource = GetRHIResource(name);
		if (!resource)
		{
			for (const auto& r : m_unresolvedResourceParams)
			{
				if (r.First() == name)
				{
					if (auto surface = frameGraph->GetSurface(*r.Second()))
					{
						resource = surface;
					}
					else
					{
						resource = frameGraph->GetRenderTarget(*r.Second());
						if (!resource)
						{
							resource = frameGraph->GetSampler(*r.Second());
						}
					}
					break;
				}
			}
		}

		if (auto envSurface = resource.DynamicCast<RHISurface>())
		{
			outTexture = envSurface->GetResolved();
		}
		else
		{
			outTexture = resource.DynamicCast<RHI::RHITexture>();
			outCubemap = resource.DynamicCast<RHICubemap>();
		}
	};

	RHI::RHITexturePtr environmentSpecularTexture{};
	RHI::RHICubemapPtr environmentSpecularCubemap{};
	resolveEnvironmentResource("environment", environmentSpecularTexture, environmentSpecularCubemap);

	RHI::RHITexturePtr environmentDiffuseTexture{};
	RHI::RHICubemapPtr environmentDiffuseCubemap{};
	resolveEnvironmentResource("environmentDiffuse", environmentDiffuseTexture, environmentDiffuseCubemap);

	if (!environmentDiffuseTexture && !environmentDiffuseCubemap)
	{
		environmentDiffuseTexture = environmentSpecularTexture;
		environmentDiffuseCubemap = environmentSpecularCubemap;
	}

	const bool bHasAnyEnvironment =
		environmentSpecularTexture || environmentSpecularCubemap ||
		environmentDiffuseTexture || environmentDiffuseCubemap;

	if (!bHasAnyEnvironment)
	{
		m_pathTracer.ClearRuntimeEnvironment();
	}

	auto consumeCapturedEnvironment = [&](RHI::RHIBufferPtr& captureBuffer, const glm::uvec2& captureExtent, bool& bPending, uint64_t submittedFrame, bool bDiffuse)
	{
		if (!bPending || !captureBuffer || captureExtent.x == 0 || captureExtent.y == 0 || sceneView.m_frame <= (submittedFrame + 1ull))
		{
			return;
		}

		driver->WaitIdle();

		const size_t pixelsCount = (size_t)captureExtent.x * (size_t)captureExtent.y;
		TVector<vec4> environmentPixels;
		environmentPixels.Resize(pixelsCount);
		std::memcpy(environmentPixels.GetData(), captureBuffer->GetPointer(), pixelsCount * sizeof(vec4));
		if (bDiffuse)
		{
			m_pathTracer.SetRuntimeDiffuseEnvironmentLinear(environmentPixels, captureExtent);
		}
		else
		{
			m_pathTracer.SetRuntimeEnvironmentLinear(environmentPixels, captureExtent);
		}

		bPending = false;
	};

	consumeCapturedEnvironment(m_environmentSpecularCaptureBuffer, m_environmentSpecularCaptureExtent,
		m_bHasPendingEnvironmentSpecularCapture, m_environmentSpecularCaptureSubmittedFrame, false);
	consumeCapturedEnvironment(m_environmentDiffuseCaptureBuffer, m_environmentDiffuseCaptureExtent,
		m_bHasPendingEnvironmentDiffuseCapture, m_environmentDiffuseCaptureSubmittedFrame, true);

	auto captureEnvironment = [&](const RHI::RHITexturePtr& environmentTexture,
		const RHI::RHICubemapPtr& environmentCubemap,
		RHI::RHITexturePtr& equirectTexture,
		RHI::RHIBufferPtr& captureBuffer,
		glm::uvec2& captureExtentOut,
		bool& bPendingCapture,
		uint64_t& submittedFrame) -> bool
	{
		if (!environmentTexture && !environmentCubemap)
		{
			return false;
		}

		bool bCanCaptureEnvironment = true;
		const uint32_t requestedEnvironmentHeight = (std::max)(16u, (uint32_t)std::lround(getFloatParam("environmentCaptureHeight", 256.0f)));
		const uint32_t requestedEnvironmentWidth = (std::max)(32u, (uint32_t)std::lround(getFloatParam("environmentCaptureWidth", (float)requestedEnvironmentHeight * 2.0f)));
		const glm::uvec2 captureExtent(requestedEnvironmentWidth, requestedEnvironmentHeight);

		if (!equirectTexture || m_environmentEquirectExtent != captureExtent)
		{
			equirectTexture = driver->CreateTexture(
				nullptr,
				0,
				glm::ivec3((int32_t)captureExtent.x, (int32_t)captureExtent.y, 1),
				1,
				ETextureType::Texture2D,
				ETextureFormat::R32G32B32A32_SFLOAT,
				ETextureFiltration::Nearest,
				ETextureClamping::Repeat,
				ETextureUsageBit::TextureTransferSrc_Bit |
				ETextureUsageBit::TextureTransferDst_Bit |
				ETextureUsageBit::Sampled_Bit |
				ETextureUsageBit::Storage_Bit);
		}

		if (!equirectTexture)
		{
			return false;
		}

		if (environmentCubemap)
		{
			if (!m_pCubeToEquirectShader)
			{
				if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/ComputeCube2Equirect.shader"))
				{
					App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderInfo->GetFileId(), m_pCubeToEquirectShader, {});
				}
			}

			if (m_pCubeToEquirectShader && m_pCubeToEquirectShader->IsReady())
			{
				m_cubeToEquirectBindings = driver->CreateShaderBindings();
				driver->AddSamplerToShaderBindings(m_cubeToEquirectBindings, "src", environmentCubemap, 0);
				driver->AddStorageImageToShaderBindings(m_cubeToEquirectBindings, "dst", equirectTexture, 1);
				m_cubeToEquirectBindings->RecalculateCompatibility();

				commands->ImageMemoryBarrier(commandList, environmentCubemap, EImageLayout::ShaderReadOnlyOptimal);
				commands->ImageMemoryBarrier(commandList, equirectTexture, EImageLayout::ComputeWrite);
				const uint32_t groupsX = (std::max)(1u, (uint32_t)std::ceil((double)captureExtent.x / 16.0));
				const uint32_t groupsY = (std::max)(1u, (uint32_t)std::ceil((double)captureExtent.y / 16.0));
				commands->Dispatch(commandList, m_pCubeToEquirectShader->GetComputeShaderRHI(),
					groupsX, groupsY, 1u,
					{ m_cubeToEquirectBindings },
					nullptr, 0);
			}
			else
			{
				bCanCaptureEnvironment = false;
			}
		}
		else if (environmentTexture)
		{
			commands->ImageMemoryBarrier(commandList, environmentTexture, EImageLayout::TransferSrcOptimal);
			commands->ImageMemoryBarrier(commandList, equirectTexture, EImageLayout::TransferDstOptimal);
			commands->BlitImage(commandList, environmentTexture, equirectTexture,
				glm::ivec4(0, 0, environmentTexture->GetExtent().x, environmentTexture->GetExtent().y),
				glm::ivec4(0, 0, (int32_t)captureExtent.x, (int32_t)captureExtent.y),
				ETextureFiltration::Linear);
		}

		if (!bCanCaptureEnvironment)
		{
			return false;
		}

		commands->ImageMemoryBarrier(commandList, equirectTexture, EImageLayout::TransferSrcOptimal);
		const size_t envBufferSize = (size_t)captureExtent.x * (size_t)captureExtent.y * sizeof(vec4) + 512u;
		if (!captureBuffer || captureBuffer->GetSize() < envBufferSize)
		{
			captureBuffer = driver->CreateBuffer(envBufferSize,
				RHI::EBufferUsageBit::BufferTransferDst_Bit,
				RHI::EMemoryPropertyBit::HostCoherent | RHI::EMemoryPropertyBit::HostVisible);
		}

		if (!captureBuffer)
		{
			return false;
		}

		commands->CopyImageToBuffer(commandList, equirectTexture, captureBuffer);
		captureExtentOut = captureExtent;
		submittedFrame = sceneView.m_frame;
		bPendingCapture = true;
		m_environmentEquirectExtent = captureExtent;
		return true;
	};

	const uint32_t environmentCaptureInterval = (std::max)(1u, (uint32_t)std::lround(getFloatParam("environmentCaptureInterval", 8.0f)));
	if (bHasAnyEnvironment && (sceneView.m_frame % environmentCaptureInterval == 0u))
	{
		captureEnvironment(environmentSpecularTexture, environmentSpecularCubemap,
			m_environmentSpecularEquirectTexture, m_environmentSpecularCaptureBuffer,
			m_environmentSpecularCaptureExtent, m_bHasPendingEnvironmentSpecularCapture, m_environmentSpecularCaptureSubmittedFrame);

		captureEnvironment(environmentDiffuseTexture, environmentDiffuseCubemap,
			m_environmentDiffuseEquirectTexture, m_environmentDiffuseCaptureBuffer,
			m_environmentDiffuseCaptureExtent, m_bHasPendingEnvironmentDiffuseCapture, m_environmentDiffuseCaptureSubmittedFrame);
	}

	const bool bResetOnCameraMove = getFloatParam("resetOnCameraMove", 1.0f) > 0.5f;
	size_t sceneHash = 1469598103934665603ull;
	auto hashVec2 = [&sceneHash](const vec2& v) { HashCombine(sceneHash, std::hash<float>()(v.x), std::hash<float>()(v.y)); };
	auto hashVec3 = [&sceneHash](const vec3& v) { HashCombine(sceneHash, std::hash<float>()(v.x), std::hash<float>()(v.y), std::hash<float>()(v.z)); };
	auto hashMat4 = [&sceneHash](const mat4& m)
	{
		for (int32_t c = 0; c < 4; c++) for (int32_t r = 0; r < 4; r++) { HashCombine(sceneHash, std::hash<float>()(m[c][r])); }
	};
	HashCombine(sceneHash, sceneView.m_pathTracerProxies.Num(), sceneView.m_pathTracerLights.Num());

	for (const auto& proxy : sceneView.m_pathTracerProxies)
	{
		HashCombine(sceneHash, proxy.m_model ? proxy.m_model->GetFileId().GetHash() : 0ull, (size_t)proxy.m_frameLastChange);
		hashMat4(proxy.m_worldMatrix);
		hashMat4(proxy.m_inverseWorldMatrix);
		hashVec3(proxy.m_worldBounds.m_min);
		hashVec3(proxy.m_worldBounds.m_max);
		for (const auto& material : proxy.m_materials) { HashCombine(sceneHash, material.GetHash()); }
	}

	for (const auto& light : sceneView.m_pathTracerLights)
	{
		HashCombine(sceneHash, (size_t)light.m_type);
		hashVec3(light.m_worldPosition);
		hashVec3(light.m_direction);
		hashVec3(light.m_intensity);
		hashVec3(light.m_attenuation);
		hashVec3(light.m_bounds);
		hashVec2(light.m_cutOff);
	}

	if (sceneView.m_camera)
	{
		HashCombine(sceneHash, std::hash<float>()(sceneView.m_camera->GetAspect()));
		HashCombine(sceneHash, std::hash<float>()(sceneView.m_camera->GetFov()));
	}

	HashCombine(sceneHash, bHasAnyEnvironment ? 1u : 0u);

	if (sceneHash != m_sceneHash)
	{
		if (bDebugResets)
		{
			SAILOR_LOG("PathTracerPass reset: scene hash changed old=%zu new=%zu frame=%llu", m_sceneHash, sceneHash, sceneView.m_frame);
		}
		ResetAccumulation();
		m_idleFrames = 0;
		m_qualityBudget = 0.6f;
		m_sceneHash = sceneHash;
	}

	const float movePosThreshold = (std::max)(0.0f, getFloatParam("movePosThreshold", 1e-4f));
	const float moveRotThreshold = (std::max)(0.0f, getFloatParam("moveRotThreshold", 1e-5f));
	bool bCameraMoving = !m_bHasPrevCamera;

	if (m_bHasPrevCamera)
	{
		const vec3 prevPos = vec3(m_prevCamera.m_position);
		const vec3 currPos = vec3(sceneView.m_cameraTransform.m_position);
		const float posDelta = glm::length(currPos - prevPos);
		const quat prevRot = glm::normalize(m_prevCamera.m_rotation);
		const quat currRot = glm::normalize(sceneView.m_cameraTransform.m_rotation);
		const float rotDot = glm::clamp(glm::abs(glm::dot(currRot, prevRot)), 0.0f, 1.0f);
		const float rotDelta = 1.0f - rotDot;
		bCameraMoving = (posDelta > movePosThreshold) || (rotDelta > moveRotThreshold);
		if (bResetOnCameraMove && bCameraMoving)
		{
			if (bDebugResets)
			{
				SAILOR_LOG("PathTracerPass reset: camera moved posDelta=%.7f rotDelta=%.7f frame=%llu", posDelta, rotDelta, sceneView.m_frame);
			}
			ResetAccumulation();
		}
	}
	m_prevCamera = sceneView.m_cameraTransform;
	m_bHasPrevCamera = true;

	const uint32_t spp = (std::max)(1u, (uint32_t)std::lround(getFloatParam("samplesPerFrame", 1.0f)));
	const uint32_t maxBounces = (std::max)(0u, (uint32_t)std::lround(getFloatParam("maxBounces", 2.0f)));
	const int32_t maxAccumFramesParam = (int32_t)std::lround(getFloatParam("maxAccumFrames", 0.0f));
	const bool bUseAccumFrameLimit = maxAccumFramesParam > 0;
	const uint32_t maxAccumFrames = bUseAccumFrameLimit ? (uint32_t)maxAccumFramesParam : (uint32_t)-1;
	const float blend = glm::clamp(getFloatParam("blend", 1.0f), 0.0f, 1.0f);

	const bool bAdaptiveQuality = getFloatParam("adaptiveQuality", 1.0f) > 0.5f;
	const uint32_t movingSpp = (std::max)(1u, (uint32_t)std::lround(getFloatParam("movingSamplesPerFrame", 1.0f)));
	const uint32_t movingBounces = (std::max)(0u, (uint32_t)std::lround(getFloatParam("movingMaxBounces", 0.0f)));
	const float movingResolutionScale = glm::clamp(getFloatParam("movingResolutionScale", 0.08f), 0.02f, 1.0f);
	const bool bAdaptiveResolution = getFloatParam("adaptiveResolution", 0.0f) > 0.5f;
	const uint32_t idleRampFrames = (std::max)(1u, (uint32_t)std::lround(getFloatParam("idleRampFrames", 24.0f)));
	const float maxQualityFps = (std::max)(1.0f, getFloatParam("maxQualityFps", 30.0f));
	const float maxQualityCap = glm::clamp(getFloatParam("maxQualityCap", 0.65f), 0.05f, 1.0f);
	const float minQualityCap = glm::clamp(getFloatParam("minQualityCap", 0.04f), 0.01f, maxQualityCap);
	const float qualityRampUp = glm::clamp(getFloatParam("qualityRampUp", 0.08f), 0.001f, 1.0f);
	const float qualityRampDown = glm::clamp(getFloatParam("qualityRampDown", 0.35f), 0.01f, 2.0f);

	if (bAdaptiveQuality)
	{
		if (bCameraMoving)
		{
			m_idleFrames = 0;
		}
		else
		{
			m_idleFrames = (std::min)(idleRampFrames, m_idleFrames + 1u);
		}
	}
	else
	{
		m_idleFrames = idleRampFrames;
		m_qualityBudget = maxQualityCap;
	}

	if (bAdaptiveQuality)
	{
		const float targetFrameMs = 1000.0f / maxQualityFps;
		const float lastRaytraceMs = (float)m_pathTracer.GetLastRaytraceTimeMs();

		if (lastRaytraceMs > 0.0f)
		{
			const float error = (lastRaytraceMs - targetFrameMs) / targetFrameMs;
			if (error > 0.0f)
			{
				m_qualityBudget = (std::max)(minQualityCap, m_qualityBudget - error * qualityRampDown);
			}
			else
			{
				m_qualityBudget = (std::min)(maxQualityCap, m_qualityBudget + (-error) * qualityRampUp);
			}
		}
		else
		{
			m_qualityBudget = (std::min)(m_qualityBudget, maxQualityCap);
		}
	}

	const float idleProgress = glm::clamp((float)m_idleFrames / (float)idleRampFrames, 0.0f, 1.0f);
	const float idleQuality = sqrt(idleProgress);
	const float qualityT = bAdaptiveQuality ? (std::min)(idleQuality, m_qualityBudget) : maxQualityCap;
	const float renderScale = bAdaptiveResolution ? glm::mix(movingResolutionScale, 1.0f, qualityT) : (bCameraMoving ? movingResolutionScale : 1.0f);
	const uint32_t activeSpp = (uint32_t)std::lround(glm::mix((float)movingSpp, (float)spp, qualityT));
	const uint32_t activeBounces = (uint32_t)std::lround(glm::mix((float)movingBounces, (float)maxBounces, qualityT));
	const float rayBiasBase = (std::max)(0.0f, getFloatParam("rayBiasBase", getFloatParam("shadowBias", 0.0f)));
	const float rayBiasScale = (std::max)(0.0f, getFloatParam("rayBiasScale", 3e-4f));
	const uint64_t maxUploadBytes = (uint64_t)(std::max)(262144.0f, getFloatParam("maxUploadBytes", 900000.0f));
	const float targetAspect = (std::max)(0.1f, (float)dst->GetExtent().x / (float)(std::max)(1, dst->GetExtent().y));
	const uint64_t maxPixelsByUpload = (std::max)(1ull, maxUploadBytes / (uint64_t)sizeof(u8vec4));
	const uint32_t maxHeightByUpload = (std::max)(1u, (uint32_t)std::floor(std::sqrt((double)maxPixelsByUpload / (double)targetAspect)));
	const uint32_t targetHeight = (uint32_t)(std::max)(1, (int32_t)std::lround((float)dst->GetExtent().y * renderScale));
	const uint32_t runtimeHeight = (std::min)(targetHeight, maxHeightByUpload);

	Raytracing::PathTracer::Params params{};
	params.m_height = runtimeHeight;
	params.m_maxBounces = activeBounces;
	params.m_output.clear();
	params.m_msaa = activeSpp <= 32 ? (std::min)(4u, activeSpp) : 8u;
	params.m_numSamples = (std::max)(1u, (uint32_t)std::lround(activeSpp / (float)params.m_msaa));
	params.m_numAmbientSamples = params.m_numSamples;
	params.m_rayBiasBase = rayBiasBase;
	params.m_rayBiasScale = rayBiasScale;
	if (sceneView.m_camera)
	{
		params.m_bUseRuntimeCamera = true;
		params.m_runtimeCameraPos = vec3(sceneView.m_cameraTransform.m_position);
		params.m_runtimeCameraForward = glm::normalize(sceneView.m_cameraTransform.GetForward());
		params.m_runtimeCameraUp = glm::normalize(sceneView.m_cameraTransform.GetUp());
		const float fallbackAspect = (std::max)(0.1f, (float)dst->GetExtent().x / (float)(std::max)(1, dst->GetExtent().y));
		const float aspect = (std::max)(0.1f, sceneView.m_camera->GetAspect() > 0.0f ? sceneView.m_camera->GetAspect() : fallbackAspect);
		const float verticalFov = glm::radians(sceneView.m_camera->GetFov());
		params.m_runtimeAspectRatio = aspect;
		params.m_runtimeHFov = 2.0f * atan(tan(verticalFov * 0.5f) * aspect);
	}

	TVector<Raytracing::PathTracer::SceneInstance> instances;
	TVector<MaterialPtr> materials;
	int32_t materialOffset = 0;
	instances.Reserve(sceneView.m_pathTracerProxies.Num());
	for (const auto& proxy : sceneView.m_pathTracerProxies)
	{
		if (!proxy.m_model || !proxy.m_model->HasBLAS() || proxy.m_model->GetBLASTriangles().Num() == 0) { continue; }

		Raytracing::PathTracer::SceneInstance instance{};
		instance.m_model = proxy.m_model;
		instance.m_worldBounds = proxy.m_worldBounds;
		instance.m_worldMatrix = proxy.m_worldMatrix;
		instance.m_inverseWorldMatrix = proxy.m_inverseWorldMatrix;
		instance.m_materialBaseOffset = materialOffset;

		if (proxy.m_materials.Num() == 0)
		{
			materials.Add(MaterialPtr());
			materialOffset += 1;
		}
		else
		{
			for (const auto& material : proxy.m_materials) { materials.Add(material); }
			materialOffset += (int32_t)proxy.m_materials.Num();
		}

		instances.Add(std::move(instance));
	}

	if (instances.Num() == 0 || !m_pathTracer.InitializeScene(instances, materials, sceneView.m_pathTracerLights) || !m_pathTracer.RenderPreparedScene(params))
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

	if (m_extent != imageExtent || m_upload.Num() != image.Num())
	{
		if (bDebugResets)
		{
			SAILOR_LOG("PathTracerPass reset: runtime image extent changed old=%ux%u new=%ux%u frame=%llu",
				m_extent.x, m_extent.y, imageExtent.x, imageExtent.y, sceneView.m_frame);
		}
		ResetAccumulation();
		m_extent = imageExtent;
		m_upload.Resize(image.Num());
	}

	for (size_t i = 0; i < image.Num(); i++) { m_upload[i] = image[i]; }

	const size_t uploadSize = m_upload.Num() * sizeof(u8vec4);
	if (!m_uploadBuffer || m_uploadBuffer->GetSize() < uploadSize)
	{
		m_uploadBuffer = driver->CreateBuffer(uploadSize,
			EBufferUsageBit::BufferTransferSrc_Bit | EBufferUsageBit::BufferTransferDst_Bit);
		m_uploadBufferSize = m_uploadBuffer ? m_uploadBuffer->GetSize() : 0;
	}
	if (!m_uploadBuffer || m_uploadBuffer->GetSize() < uploadSize)
	{
		SAILOR_LOG_ERROR("PathTracerPass: invalid persistent upload buffer. Need=%zu bytes, Have=%zu bytes", uploadSize, m_uploadBuffer ? m_uploadBuffer->GetSize() : 0ull);
		commands->EndDebugRegion(commandList);
		return;
	}
	commands->UpdateBuffer(commandList, m_uploadBuffer, m_upload.GetData(), uploadSize, 0);

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
	if (m_historyExtent != dstExtent || !m_historyTextures[0] || !m_historyTextures[1] || !m_currentFrameTexture)
	{
		if (bDebugResets && (m_historyExtent != dstExtent))
		{
			SAILOR_LOG("PathTracerPass reset: history extent changed old=%ux%u new=%ux%u frame=%llu",
				m_historyExtent.x, m_historyExtent.y, dstExtent.x, dstExtent.y, sceneView.m_frame);
		}
		m_historyExtent = dstExtent;
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
		m_historyTextures[0] = driver->CreateRenderTarget(glm::ivec2((int32_t)dstExtent.x, (int32_t)dstExtent.y), 1, ETextureFormat::R8G8B8A8_UNORM, ETextureFiltration::Linear, ETextureClamping::Clamp);
		m_historyTextures[1] = driver->CreateRenderTarget(glm::ivec2((int32_t)dstExtent.x, (int32_t)dstExtent.y), 1, ETextureFormat::R8G8B8A8_UNORM, ETextureFiltration::Linear, ETextureClamping::Clamp);
		ResetAccumulation();
	}
	if (!m_currentFrameTexture || !m_historyTextures[0] || !m_historyTextures[1])
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	commands->ImageMemoryBarrier(commandList, m_runtimeTexture, EImageLayout::TransferSrcOptimal);
	commands->ImageMemoryBarrier(commandList, m_currentFrameTexture, EImageLayout::TransferDstOptimal);
	const glm::ivec4 srcRegion(0, 0, (int32_t)m_extent.x, (int32_t)m_extent.y);
	const glm::ivec4 dstRegion(0, 0, (int32_t)m_historyExtent.x, (int32_t)m_historyExtent.y);
	commands->BlitImage(commandList, m_runtimeTexture, m_currentFrameTexture, srcRegion, dstRegion, ETextureFiltration::Nearest);
	commands->ImageMemoryBarrier(commandList, m_runtimeTexture, EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, m_currentFrameTexture, EImageLayout::ShaderReadOnlyOptimal);

	if (!m_shaderBindings)
	{
		m_shaderBindings = driver->CreateShaderBindings();
		driver->FillShadersLayout(m_shaderBindings, { m_pShader->GetDebugVertexShaderRHI(), m_pShader->GetDebugFragmentShaderRHI() }, 1);
		driver->AddBufferToShaderBindings(m_shaderBindings, "data", 64, 2, RHI::EShaderBindingType::UniformBuffer);
		driver->AddSamplerToShaderBindings(m_shaderBindings, "prevAccumSampler", m_historyTextures[m_historyReadIndex], 0);
		driver->AddSamplerToShaderBindings(m_shaderBindings, "currentSampler", m_currentFrameTexture, 1);
		m_shaderBindings->RecalculateCompatibility();
	}

	if (!m_accumulateMaterial || !m_overlayMaterial || !m_overlayMaterialMsaa)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState accumulateState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, false };
		RenderState overlayState{ false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending, EFillMode::Fill, 0, false };
		RenderState overlayMsaaState{ false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending, EFillMode::Fill, 0, true };
		if (!m_accumulateMaterial) { m_accumulateMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, accumulateState, m_pShader, m_shaderBindings); }
		if (!m_overlayMaterial) { m_overlayMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, overlayState, m_pShader, m_shaderBindings); }
		if (!m_overlayMaterialMsaa) { m_overlayMaterialMsaa = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, overlayMsaaState, m_pShader, m_shaderBindings); }
	}
	if (!m_accumulateMaterial || !m_overlayMaterial || !m_overlayMaterialMsaa)
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	const uint32_t writeIndex = (m_historyReadIndex + 1u) & 1u;
	driver->UpdateShaderBinding(m_shaderBindings, "prevAccumSampler", m_historyTextures[m_historyReadIndex], 0);

	driver->UpdateShaderBinding(m_shaderBindings, "currentSampler", m_currentFrameTexture, 1);

	const double frameSampleWeight = (double)activeSpp * (double)renderScale * (double)renderScale;
	const double safeFrameSampleWeight = (std::max)(frameSampleWeight, 1e-6);

	double oldSampleWeight = m_accumulatedSampleWeight;
	if (bUseAccumFrameLimit && m_accumulatedFrames >= maxAccumFrames && maxAccumFrames > 0u)
	{
		// Keep a bounded temporal history if explicit limit is set.
		const double maxWeight = safeFrameSampleWeight * (double)maxAccumFrames;
		oldSampleWeight = (std::min)(oldSampleWeight, maxWeight);
	}

	const double totalSampleWeight = oldSampleWeight + safeFrameSampleWeight;
	const float oldWeight = totalSampleWeight > 0.0 ? (float)(oldSampleWeight / totalSampleWeight) : 0.0f;
	const float newWeight = totalSampleWeight > 0.0 ? (float)(safeFrameSampleWeight / totalSampleWeight) : 1.0f;

	const auto setCompositeData = [&](float mode)
	{
		commands->SetMaterialParameter(commandList, m_shaderBindings, "data.fitScaleOffset", glm::vec4(1.0f, 1.0f, 0.0f, 0.0f));
		commands->SetMaterialParameter(commandList, m_shaderBindings, "data.oldWeight", oldWeight);
		commands->SetMaterialParameter(commandList, m_shaderBindings, "data.newWeight", newWeight);
		commands->SetMaterialParameter(commandList, m_shaderBindings, "data.blend", blend);
		commands->SetMaterialParameter(commandList, m_shaderBindings, "data.mode", mode);
	};

	auto mesh = frameGraph->GetFullscreenNdcQuad();
	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->ImageMemoryBarrier(commandList, m_historyTextures[m_historyReadIndex], EImageLayout::ShaderReadOnlyOptimal);
	commands->ImageMemoryBarrier(commandList, m_historyTextures[writeIndex], EImageLayout::ColorAttachmentOptimal);
	setCompositeData(0.0f);
	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{m_historyTextures[writeIndex]},
		nullptr,
		glm::vec4(0, 0, m_historyExtent.x, m_historyExtent.y),
		glm::ivec2(0, 0),
		false,
		glm::vec4(0.0f),
		0.0f,
		false);
	commands->BindMaterial(commandList, m_accumulateMaterial);
	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);
	commands->BindShaderBindings(commandList, m_accumulateMaterial, { sceneView.m_frameBindings, m_shaderBindings });
	commands->SetViewport(commandList,
		0.0f, 0.0f,
		(float)m_historyExtent.x, (float)m_historyExtent.y,
		glm::vec2(0.0f, 0.0f),
		glm::vec2((float)m_historyExtent.x, (float)m_historyExtent.y),
		0.0f, 1.0f);
	commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
	commands->EndRenderPass(commandList);

	driver->UpdateShaderBinding(m_shaderBindings, "prevAccumSampler", m_historyTextures[writeIndex], 0);
	setCompositeData(1.0f);
	commands->ImageMemoryBarrier(commandList, m_historyTextures[writeIndex], EImageLayout::ShaderReadOnlyOptimal);
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

	m_historyReadIndex = writeIndex;
	if (bUseAccumFrameLimit)
	{
		m_accumulatedFrames = (std::min)(maxAccumFrames, m_accumulatedFrames + 1u);
		const double maxWeight = safeFrameSampleWeight * (double)maxAccumFrames;
		m_accumulatedSampleWeight = (std::min)(oldSampleWeight + safeFrameSampleWeight, maxWeight);
	}
	else
	{
		m_accumulatedFrames += 1u;
		m_accumulatedSampleWeight = oldSampleWeight + safeFrameSampleWeight;
	}
	commands->EndDebugRegion(commandList);
}

void PathTracerPassNode::Clear()
{
	ResetAccumulation();
	m_runtimeTexture.Clear();
	m_currentFrameTexture.Clear();
	m_environmentSpecularEquirectTexture.Clear();
	m_environmentDiffuseEquirectTexture.Clear();
	m_historyTextures[0].Clear();
	m_historyTextures[1].Clear();
	m_uploadBuffer.Clear();
	m_environmentSpecularCaptureBuffer.Clear();
	m_environmentDiffuseCaptureBuffer.Clear();
	m_shaderBindings.Clear();
	m_uploadBufferSize = 0;
	m_historyExtent = glm::uvec2(0u, 0u);
	m_environmentSpecularCaptureExtent = glm::uvec2(0u, 0u);
	m_environmentDiffuseCaptureExtent = glm::uvec2(0u, 0u);
	m_environmentEquirectExtent = glm::uvec2(0u, 0u);
	m_bHasPendingEnvironmentSpecularCapture = false;
	m_bHasPendingEnvironmentDiffuseCapture = false;
	m_environmentSpecularCaptureSubmittedFrame = 0ull;
	m_environmentDiffuseCaptureSubmittedFrame = 0ull;
	m_accumulateMaterial.Clear();
	m_overlayMaterial.Clear();
	m_overlayMaterialMsaa.Clear();
	m_pShader.Clear();
	m_pCubeToEquirectShader.Clear();
	m_cubeToEquirectBindings.Clear();
	m_bHasPrevCamera = false;
	m_sceneHash = 0;
	m_qualityBudget = 0.6f;
	m_accumulatedSampleWeight = 0.0;
	m_pathTracer.ClearRuntimeEnvironment();
}
