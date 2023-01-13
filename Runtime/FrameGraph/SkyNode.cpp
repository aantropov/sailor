#include "SkyNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Texture.h"
#include "RHI/VertexDescription.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Math/Math.h"
#include "Math/Noise.h"
#include "glm/glm/gtx/quaternion.hpp"
#include "AssetRegistry/Texture/TextureImporter.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* SkyNode::m_name = "Sky";
#endif

glm::vec3 SkyNode::s_rgbTemperatures[s_maxRgbTemperatures];

using TParseRes = TPair<TVector<VertexP3C4>, TVector<uint32_t>>;

Tasks::TaskPtr<RHI::RHIMeshPtr, TParseRes> SkyNode::CreateStarsMesh()
{
	auto task = Tasks::Scheduler::CreateTaskWithResult<TParseRes>("Parse Stars Mesh",
		[=]() -> TParseRes
		{
			std::string temperatures;
	if (!AssetRegistry::ReadAllTextFile(AssetRegistry::ContentRootFolder + std::string("StarsColor.yaml"), temperatures))
	{
		return TParseRes();
	}

	YAML::Node temperaturesNode = YAML::Load(temperatures.c_str());
	if (temperaturesNode["colors"])
	{
		TVector<TVector<float>> temperaturesData = temperaturesNode["colors"].as<TVector<TVector<float>>>();

		for (const auto& line : temperaturesData)
		{
			const auto temperatureK = line[0];

			uint32_t index = uint32_t((temperatureK / 100.0f) - 10.0f); // 1000 / 100 = 10
			index = glm::clamp(index, 0u, s_maxRgbTemperatures);

			// Should we inverse gamma correction?
			const vec3 color = vec3(line[5], line[6], line[7]);

			s_rgbTemperatures[index] = color;
		}
	}

	TVector<uint8_t> starCatalogueData;
	AssetRegistry::ReadBinaryFile(std::filesystem::path(AssetRegistry::ContentRootFolder + std::string("BSC5")), starCatalogueData);

	BrighStarCatalogue_Header* header = (BrighStarCatalogue_Header*)starCatalogueData.GetData();

	const size_t starCount = abs(header->m_starCount);
	BrighStarCatalogue_Entry* starCatalogue = (BrighStarCatalogue_Entry*)(starCatalogueData.GetData() + sizeof(BrighStarCatalogue_Header));

	TVector<VertexP3C4> vertices(starCount);
	TVector<uint32_t> indices(starCount);

	for (uint32_t i = 0; i < starCount; i++)
	{
		const BrighStarCatalogue_Entry& entry = starCatalogue[i];

		vertices[i].m_position = Utils::ConvertToEuclidean((float)entry.m_SRA0, (float)entry.m_SDEC0, 1.0f);
		vertices[i].m_position /= (entry.m_mag / 100.0f) + 0.4f;

		vertices[i].m_position.x *= 5000.0f;
		vertices[i].m_position.y *= 5000.0f;
		vertices[i].m_position.z *= 5000.0f;

		vertices[i].m_color = glm::vec4(MorganKeenanToColor(entry.m_IS[0], entry.m_IS[1]), 1.0f);

		indices[i] = i;
	}

	return TPair<TVector<VertexP3C4>, TVector<uint32_t>>(std::move(vertices), std::move(indices));
		})->Then<RHI::RHIMeshPtr, TParseRes>([](const TParseRes& res) mutable
			{
				const VkDeviceSize bufferSize = sizeof(res.m_first[0]) * res.m_first.Num();
		const VkDeviceSize indexBufferSize = sizeof(res.m_second[0]) * res.m_second.Num();

		auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();

		RHI::RHIMeshPtr mesh = driver->CreateMesh();

		mesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3C4>();
		mesh->m_bounds = Math::AABB(vec3(0), vec3(1000, 1000, 1000));
		driver->UpdateMesh(mesh, &res.m_first[0], bufferSize, &res.m_second[0], indexBufferSize);

		return mesh;
			}, "Create Stars Mesh", Tasks::EThreadType::RHI);

		task->Run();

		return task;
}

float Remap(float value, float minValue, float maxValue, float newMinValue, float newMaxValue)
{
	return newMinValue + (value - minValue) / (maxValue - minValue) * (newMaxValue - newMinValue);
}

void SmoothBorders(TVector<uint8_t>& noise3d, uint32_t dimensions, float percentage)
{
	uint32_t border = uint32_t(percentage * dimensions);

	for (uint32_t z = 0; z < border - 1; z++)
	{
		for (uint32_t y = 0; y < border - 1; y++)
		{
			for (uint32_t x = 0; x < border - 1; x++)
			{
				uint8_t& value = noise3d[x + y * dimensions + z * dimensions * dimensions];

				vec3 weight = 1.0f - vec3(((float)x + 1.0f) / border, ((float)y + 1.0f) / border, ((float)z + 1.0f) / border);

				vec3 values = vec3(noise3d[(dimensions - 1) + y * dimensions + z * dimensions * dimensions],
					noise3d[x + (dimensions - 1) * dimensions + z * dimensions * dimensions],
					noise3d[x + y * dimensions + (dimensions - 1) * dimensions * dimensions]);

				float mix = glm::dot(weight, values);

				value = uint8_t(mix);
			}
		}
	}
}

TVector<uint8_t> SkyNode::GenerateCloudsNoiseLow() const
{
	TVector<uint8_t> res;
	res.Resize(CloudsNoiseLowResolution * CloudsNoiseLowResolution * CloudsNoiseLowResolution);

	TVector<Tasks::ITaskPtr> tasks;
	tasks.Reserve(CloudsNoiseLowResolution);

	for (uint32_t z = 0; z < CloudsNoiseLowResolution; z++)
	{
		auto pTask = Tasks::Scheduler::CreateTask("Generate Clouds Noise Low", [=, &res]()
			{
				for (uint32_t y = 0; y < CloudsNoiseLowResolution; y++)
				{
					for (uint32_t x = 0; x < CloudsNoiseLowResolution; x++)
					{
						uint8_t& value = res[x + y * CloudsNoiseLowResolution + z * CloudsNoiseLowResolution * CloudsNoiseLowResolution];

						vec3 uv = vec3((float)x / CloudsNoiseLowResolution, (float)y / CloudsNoiseLowResolution, (float)z / CloudsNoiseLowResolution) + (0.5f / CloudsNoiseLowResolution);

						const float tiling = 5.0f;

						const float perlinNoiseLow = (Math::fBmTiledPerlin(uv * tiling, 4, (int32_t)tiling) + 1) * 0.5f;
						const float cellularNoiseLow = Math::fBmTiledWorley(uv * tiling, 4, (int32_t)tiling);
						const float cellularNoiseMid = Math::fBmTiledWorley(uv * tiling * 2.0f, 4, (int32_t)tiling * 2);
						const float cellularNoiseHigh = Math::fBmTiledWorley(uv * tiling * 3.0f, 4, (int32_t)tiling * 3);

						const float noise = Remap(perlinNoiseLow, (cellularNoiseLow * 0.625f + cellularNoiseMid * 0.25f + cellularNoiseHigh * 0.125f) - 1.0f, 1.0f, 0.0f, 1.0f);

						value = uint8_t(noise * 255.0f);
					}
				}

			})->Run();

			tasks.Add(pTask);
	}

	for (uint32_t i = 0; i < tasks.Num(); i++)
	{
		tasks[i]->Wait();
	}

	return res;
}

TVector<uint8_t> SkyNode::GenerateCloudsNoiseHigh() const
{
	TVector<uint8_t> res;
	res.Resize(CloudsNoiseHighResolution * CloudsNoiseHighResolution * CloudsNoiseHighResolution);

	for (uint32_t z = 0; z < CloudsNoiseHighResolution; z++)
	{
		for (uint32_t y = 0; y < CloudsNoiseHighResolution; y++)
		{
			for (uint32_t x = 0; x < CloudsNoiseHighResolution; x++)
			{
				uint8_t& value = res[x + y * CloudsNoiseHighResolution + z * CloudsNoiseHighResolution * CloudsNoiseHighResolution];

				vec3 uv = vec3((float)x / CloudsNoiseHighResolution, (float)y / CloudsNoiseHighResolution, (float)z / CloudsNoiseHighResolution);

				const float tiling = 5.0f;

				float noise = 0.5f * (Math::fBmTiledPerlin(uv * tiling, 4, (int32_t)tiling) + 1) * 0.625f +
					(Math::fBmTiledWorley(uv * tiling * 2.0f, 4, (int32_t)tiling * 2)) * 0.25f +
					(Math::fBmTiledWorley(uv * tiling * 3.0f, 4, (int32_t)tiling * 3)) * 0.125f;

				value = uint8_t(noise * 255.0f);
			}
		}
	}

	return res;
}

void SkyNode::SetLocation(float latitudeDegrees, float longitudeDegrees)
{
	float latitudeRad = glm::radians(latitudeDegrees);
	float longitudeRad = glm::radians(longitudeDegrees);

	const double jdn2022 = Utils::CalculateJulianDate(2022, 12, 29, 12, 0, 0);

	// Calculate rotation matrix based on time, latitude and longitude
	double localMeanSiderealTime = 4.894961f + 230121.675315f * jdn2022 + longitudeRad;

	// Exploration of different rotations
	glm::quat rotation = angleAxis(-(float)localMeanSiderealTime, Math::vec3_Backward) * angleAxis(latitudeRad - pi<float>() * 0.5f, Math::vec3_Up);

	glm::quat precessionRotationZ = angleAxis(0.01118f, Math::vec3_Backward);
	glm::quat precession = (precessionRotationZ * angleAxis(-0.00972f, Math::vec3_Right)) * precessionRotationZ;

	m_starsModelView = glm::toMat4(precession);
}

void SkyNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdPostProcess);

	if (!m_pSkyShader)
	{
		const std::string shaderPath = "Shaders/Sky.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pSkyShader, { "FILL" });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pSunShader, { "SUN" });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pComposeShader, { "COMPOSE" });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pCloudsShader, { "CLOUDS", "DITHER" });
		}
	}

	if (!m_pBlitShader)
	{
		const std::string shaderPath = "Shaders/Blit.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pBlitShader, {});
		}
	}

	if (!m_pCloudsMapTexture)
	{
		const std::string path = "Textures/CloudsMap.png";

		if (auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(path))
		{
			TexturePtr clouds;
			if (App::GetSubmodule<TextureImporter>()->LoadTexture_Immediate(info->GetUID(), clouds))
			{
				m_pCloudsMapTexture = clouds->GetRHI();
			}
		}
	}

	if (m_createNoiseLow && !m_createNoiseLow->IsFinished() ||
		m_createNoiseHigh && !m_createNoiseHigh->IsFinished())
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	if (!m_pCloudsNoiseHighTexture)
	{
		bool bShouldReturn = false;

		TVector<uint8_t> noiseHigh;
		auto pathNoiseHigh = std::filesystem::path(AssetRegistry::CacheRootFolder + std::string("CloudsNoiseHigh.bin"));
		if (!AssetRegistry::ReadBinaryFile(pathNoiseHigh, noiseHigh))
		{
			m_createNoiseHigh = Tasks::Scheduler::CreateTask("Generate Clouds Noise High",
				[=]()
				{
					auto cache = GenerateCloudsNoiseHigh();
					AssetRegistry::WriteBinaryFile(pathNoiseHigh, cache);
				})->Run();

			bShouldReturn = true;
		}
		
		TVector<uint8_t> noiseLow;
		auto pathNoiseLow = std::filesystem::path(AssetRegistry::CacheRootFolder + std::string("CloudsNoiseLow.bin"));
		if (!AssetRegistry::ReadBinaryFile(pathNoiseLow, noiseLow))
		{
			m_createNoiseLow = Tasks::Scheduler::CreateTask("Generate Clouds Noise Low",
				[=]()
				{
					auto cache = GenerateCloudsNoiseLow();
					AssetRegistry::WriteBinaryFile(pathNoiseLow, cache);
				})->Run();

			bShouldReturn = true;
		}

		if (bShouldReturn)
		{
			return;
		}

		m_pCloudsNoiseHighTexture = driver->CreateTexture(noiseHigh.GetData(), noiseHigh.Num() * sizeof(uint8_t),
			glm::ivec3(CloudsNoiseHighResolution, CloudsNoiseHighResolution, CloudsNoiseHighResolution),
			1,
			ETextureType::Texture3D,
			ETextureFormat::R8_UNORM,
			ETextureFiltration::Linear,
			ETextureClamping::Repeat,
			ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);

		driver->SetDebugName(m_pCloudsNoiseHighTexture, "CloudsNoiseHigh");

		m_pCloudsNoiseLowTexture = driver->CreateTexture(noiseLow.GetData(), noiseLow.Num() * sizeof(uint8_t),
			glm::ivec3(CloudsNoiseLowResolution, CloudsNoiseLowResolution, CloudsNoiseLowResolution),
			1,
			ETextureType::Texture3D,
			ETextureFormat::R8_UNORM,
			ETextureFiltration::Linear,
			ETextureClamping::Repeat,
			ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);

		driver->SetDebugName(m_pCloudsNoiseLowTexture, "CloudsNoiseLow");
	}

	if (!m_pStarsShader)
	{
		const std::string shaderPath = "Shaders/Stars.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetUID(), m_pStarsShader);
		}
	}

	if (!m_pSkyTexture)
	{
		m_pSkyTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(SkyResolution, SkyResolution),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Bicubic,
			ETextureClamping::Repeat,
			ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit);

		driver->SetDebugName(m_pSunTexture, "Sky");
	}

	if (!m_pSunTexture)
	{
		m_pSunTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(SunResolution, SunResolution),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Bicubic,
			ETextureClamping::Clamp,
			ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit);

		driver->SetDebugName(m_pSunTexture, "Sun");
	}

	if (!m_pCloudsTexture)
	{
		m_pCloudsTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(CloudsResolution, CloudsResolution),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Bicubic,
			ETextureClamping::Clamp,
			ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit);

		driver->SetDebugName(m_pSunTexture, "Clouds");
	}

	if (!m_starsMesh && !m_loadMeshTask)
	{
		m_loadMeshTask = CreateStarsMesh();
	}
	else if (!m_starsMesh && m_loadMeshTask->IsFinished())
	{
		m_starsMesh = m_loadMeshTask->GetResult();
		m_loadMeshTask.Clear();
	}

	if (!m_pBlitShader || !m_pBlitShader->IsReady() ||
		!m_pStarsShader || !m_pStarsShader->IsReady() ||
		!m_pCloudsShader || !m_pCloudsShader->IsReady() ||
		!m_pSkyShader || !m_pSkyShader->IsReady() ||
		!m_pSunShader || !m_pSunShader->IsReady() ||
		!m_pComposeShader || !m_pComposeShader->IsReady() ||
		!m_starsMesh || !m_starsMesh->IsReady())
	{
		commands->EndDebugRegion(commandList);
		return;
	}

	if (!m_pSkyMaterial)
	{
		m_pShaderBindings = driver->CreateShaderBindings();

		// Firstly we must assign the correct layout
		driver->FillShadersLayout(m_pShaderBindings, { m_pSkyShader->GetDebugVertexShaderRHI(), m_pSkyShader->GetDebugFragmentShaderRHI() }, 1);

		// That should be enough to handle all the uniforms 
		const size_t uniformsSize = std::max(sizeof(SkyParams), m_vectorParams.Num() * sizeof(glm::vec4));
		RHIShaderBindingPtr data = driver->AddBufferToShaderBindings(m_pShaderBindings, "data", uniformsSize, 0, RHI::EShaderBindingType::UniformBuffer);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "skySampler", m_pSkyTexture, 1);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "sunSampler", m_pSunTexture, 2);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "cloudsMapSampler", m_pCloudsMapTexture, 3);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "cloudsNoiseLowSampler", m_pCloudsNoiseLowTexture, 4);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "cloudsNoiseHighSampler", m_pCloudsNoiseHighTexture, 5);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "cloudsSampler", m_pCloudsTexture, 6);

		auto ditherPattern = frameGraph->GetSampler("g_ditherPatternSampler")->GetRHI();
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "g_ditherPatternSampler", ditherPattern, 7);

		auto noise = frameGraph->GetSampler("g_noiseSampler")->GetRHI();
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "g_noiseSampler", noise, 8);

		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, false };
		m_pSkyMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSkyShader, m_pShaderBindings);
		m_pSunMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSunShader, m_pShaderBindings);
		m_pComposeMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pComposeShader, m_pShaderBindings);
		m_pCloudsMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pCloudsShader, m_pShaderBindings);

		const SkyParams params{};
		commands->UpdateShaderBinding(transferCommandList, data, &params, sizeof(SkyParams));
	}

	if (!m_pBlitMaterial)
	{
		m_pBlitBindings = driver->CreateShaderBindings();

		// Firstly we must assign the correct layout
		driver->FillShadersLayout(m_pBlitBindings, { m_pBlitShader->GetDebugVertexShaderRHI(), m_pBlitShader->GetDebugFragmentShaderRHI() }, 1);

		// That should be enough to handle all the uniforms 
		const size_t uniformsSize = std::max(sizeof(SkyParams), m_vectorParams.Num() * sizeof(glm::vec4));
		driver->AddSamplerToShaderBindings(m_pBlitBindings, "colorSampler", m_pCloudsTexture, 0);

		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::AlphaBlending, EFillMode::Fill, 0, false };
		m_pBlitMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlitShader, m_pBlitBindings);
	}

	if (!m_pStarsMaterial)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::Back, EBlendMode::Additive, EFillMode::Point, 0, false };
		m_pStarsMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::PointList, renderState, m_pStarsShader);
	}

	RHI::RHITexturePtr target = GetResolvedAttachment("color");
	RHI::RHITexturePtr depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");

	auto mesh = frameGraph->GetFullscreenNdcQuad();

	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

	commands->BeginDebugRegion(commandList, "Sky", DebugContext::Color_CmdPostProcess);
	{
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), m_pSkyTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

		commands->BindMaterial(commandList, m_pSkyMaterial);
		commands->BindShaderBindings(commandList, m_pSkyMaterial, { sceneView.m_frameBindings, m_pShaderBindings });

		commands->SetViewport(commandList,
			0, 0,
			(float)m_pSkyTexture->GetExtent().x, (float)m_pSkyTexture->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(m_pSkyTexture->GetExtent().x, m_pSkyTexture->GetExtent().y),
			0, 1.0f);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{m_pSkyTexture},
			depthAttachment,
			glm::vec4(0, 0, m_pSkyTexture->GetExtent().x, m_pSkyTexture->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pSkyTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->BeginDebugRegion(commandList, "Clouds", DebugContext::Color_CmdPostProcess);
	{
		commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), m_pSkyTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), m_pCloudsTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

		commands->BindMaterial(commandList, m_pCloudsMaterial);
		commands->BindShaderBindings(commandList, m_pCloudsMaterial, { sceneView.m_frameBindings, m_pShaderBindings });
		commands->PushConstants(commandList, m_pCloudsMaterial, sizeof(uint32_t), &m_ditherPatternIndex);

		commands->SetViewport(commandList,
			0, 0,
			(float)m_pCloudsTexture->GetExtent().x, (float)m_pCloudsTexture->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(m_pCloudsTexture->GetExtent().x, m_pCloudsTexture->GetExtent().y),
			0, 1.0f);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{m_pCloudsTexture},
			depthAttachment,
			glm::vec4(0, 0, m_pCloudsTexture->GetExtent().x, m_pCloudsTexture->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSkyTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pCloudsTexture->GetDefaultLayout());

	}
	commands->EndDebugRegion(commandList);

	commands->BeginDebugRegion(commandList, "Sun", DebugContext::Color_CmdPostProcess);
	{
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), m_pSunTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), m_pCloudsTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);

		commands->BindMaterial(commandList, m_pSunMaterial);
		commands->BindShaderBindings(commandList, m_pSunMaterial, { sceneView.m_frameBindings, m_pShaderBindings });

		commands->SetViewport(commandList,
			0, 0,
			(float)m_pSunTexture->GetExtent().x, (float)m_pSunTexture->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(m_pSunTexture->GetExtent().x, m_pSunTexture->GetExtent().y),
			0, 1.0f);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{m_pSunTexture},
			depthAttachment,
			glm::vec4(0, 0, m_pSunTexture->GetExtent().x, m_pSunTexture->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pCloudsTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pSunTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->BeginDebugRegion(commandList, "Compose", DebugContext::Color_CmdPostProcess);
	{
		commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), m_pSkyTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), m_pSunTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);

		commands->BindMaterial(commandList, m_pComposeMaterial);
		commands->BindShaderBindings(commandList, m_pComposeMaterial, { sceneView.m_frameBindings, m_pShaderBindings });

		commands->SetViewport(commandList,
			0, 0,
			(float)target->GetExtent().x, (float)target->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(target->GetExtent().x, target->GetExtent().y),
			0, 1.0f);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{target},
			depthAttachment,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSkyTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSunTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	// http://www.geomidpoint.com/example.html
	// Longitude in radians (east positive)
	// Latitude in radians (north positive)
	//
	// New York lat	40° 42' 51.6708" N	40.7143528	0.710599509
	// New York lon    74° 0' 21.5022" W	-74.0059731	-1.291647896
	// Rome lat 41.8919300 degrees
	// Rome lon 12.5113300 degrees
	SetLocation(41.8919300f, 12.5113300f);

	commands->BeginDebugRegion(commandList, "Stars & Clouds", DebugContext::Color_CmdGraphics);
	{
		commands->BindVertexBuffer(commandList, m_starsMesh->m_vertexBuffer, m_starsMesh->m_vertexBuffer->GetOffset());
		commands->BindIndexBuffer(commandList, m_starsMesh->m_indexBuffer, m_starsMesh->m_indexBuffer->GetOffset());

		PushConstants pushConstants{};
		pushConstants.m_starsModelView = glm::translate(glm::mat4(1), glm::vec3(sceneView.m_cameraPosition)) * m_starsModelView;

		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), m_pCloudsTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);

		commands->BindMaterial(commandList, m_pStarsMaterial);
		commands->BindShaderBindings(commandList, m_pStarsMaterial, { sceneView.m_frameBindings, m_pShaderBindings });
		commands->PushConstants(commandList, m_pStarsMaterial, sizeof(PushConstants), &pushConstants);

		commands->SetDefaultViewport(commandList);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{target},
			depthAttachment,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			false);

		commands->DrawIndexed(commandList, (uint32_t)m_starsMesh->m_indexBuffer->GetSize() / sizeof(uint32_t), 1u, 0u, 0u, 0u);

		// Bind Post Effect
		commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

		commands->BindMaterial(commandList, m_pBlitMaterial);
		commands->BindShaderBindings(commandList, m_pBlitMaterial, { sceneView.m_frameBindings, m_pBlitBindings });
		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);

		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pCloudsTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->EndDebugRegion(commandList);

	m_ditherPatternIndex++;
}

void SkyNode::Clear()
{
	m_pSkyTexture.Clear();
	m_pSunTexture.Clear();
	m_pSkyShader.Clear();
	m_pSkyMaterial.Clear();
	m_pShaderBindings.Clear();
}

uint32_t SkyNode::MorganKeenanToTemperature(char spectralType, char subType)
{
	// Morgan-Keenan classification
	// https://starparty.com/topics/astronomy/stars/the-morgan-keenan-system/

	// Letters are for star categories.
	// Numbers (0..9) are for further subdivision: 0 hottest, 9 colder.
	static constexpr uint32_t s_maxStarTypes = 'z' - 'a';

	// Temperature ranges (in Kelvin) of the different MK spectral types.
	static constexpr glm::vec2 s_starTemperatureRanges[s_maxStarTypes] =
	{
		// A0-A9            B                   C                 D                 E           F               G
		{ 7300, 10000 }, { 10000, 30000 }, { 2400, 3200 }, { 100000, 1000000 }, { 0, 0 }, { 6000, 7300 }, { 5300, 6000 }, { 0, 0 }, { 0, 0 },
		//  J          K                    L           M                           O           P           Q        R          S               T
		{ 0, 0 }, { 3800, 5300 }, { 1300, 2100 }, { 2500, 3800 }, { 0, 0 }, { 30000, 40000 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 2400, 3500 }, { 600, 1300 },
		// U         V          W              X            Y
		{ 0, 0 }, { 0, 0 }, { 25000, 40000 }, { 0, 0 }, { 0, 600 }
	};

	const glm::vec2& temperatureRange = s_starTemperatureRanges[spectralType - 'A'];
	const uint32_t rangeStep = (uint32_t)((temperatureRange.y - temperatureRange.x) / 9);

	const uint32_t subIndex = '9' - subType;
	return (uint32_t)(temperatureRange.x + subIndex * rangeStep);
}

const glm::vec3& SkyNode::TemperatureToColor(uint32_t temperature)
{
	int32_t index = (temperature / 100) - 10; // 1000 / 100 = 10
	index = glm::clamp(index, 0, (int32_t)s_maxRgbTemperatures);

	return s_rgbTemperatures[index];
}

const glm::vec3& SkyNode::MorganKeenanToColor(char spectralType, char subType)
{
	uint32_t temperature_kelvin = MorganKeenanToTemperature(spectralType, subType);
	return TemperatureToColor(temperature_kelvin);
}