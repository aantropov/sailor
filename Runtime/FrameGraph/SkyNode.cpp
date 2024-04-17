#include "SkyNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/RenderTarget.h"
#include "RHI/Cubemap.h"
#include "RHI/Texture.h"
#include "RHI/VertexDescription.h"
#include "Engine/World.h"
#include "Engine/GameObject.h"
#include "Math/Math.h"
#include "Math/Noise.h"
#include "glm/glm/gtx/quaternion.hpp"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "EnvironmentNode.h"

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
	auto task = Tasks::CreateTaskWithResult<TParseRes>("Parse Stars Mesh",
		[=]() -> TParseRes
		{
			std::string temperatures;
	if (!AssetRegistry::ReadAllTextFile(AssetRegistry::GetContentFolder() + std::string("StarsColor.yaml"), temperatures))
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
	AssetRegistry::ReadBinaryFile(std::filesystem::path(AssetRegistry::GetContentFolder() + std::string("BSC5")), starCatalogueData);

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

		vec4 color = glm::vec4(MorganKeenanToColor(entry.m_IS[0], entry.m_IS[1]), 1.0f);

		vertices[i].m_color.x = pow(color.x, 1 / 2.2f);
		vertices[i].m_color.y = pow(color.y, 1 / 2.2f);
		vertices[i].m_color.z = pow(color.z, 1 / 2.2f);
		vertices[i].m_color.w = pow(color.w, 1 / 2.2f);

		indices[i] = i;
	}

	return TPair<TVector<VertexP3C4>, TVector<uint32_t>>(std::move(vertices), std::move(indices));
		})->Then<RHI::RHIMeshPtr>([](const TParseRes& res) mutable
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

TVector<uint8_t> SkyNode::GenerateCloudsNoiseLow() const
{
	TVector<uint8_t> res;
	res.Resize(CloudsNoiseLowResolution * CloudsNoiseLowResolution * CloudsNoiseLowResolution);

	TVector<Tasks::ITaskPtr> tasks;
	tasks.Reserve(CloudsNoiseLowResolution);

	for (uint32_t z = 0; z < CloudsNoiseLowResolution; z++)
	{
		auto pTask = Tasks::CreateTask("Generate Clouds Noise Low", [=, &res]()
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

void SkyNode::Process(RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(commandList, GetName(), DebugContext::Color_CmdPostProcess);

	if (!m_pSkyShader)
	{
		const std::string shaderPath = "Shaders/Sky.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pSkyShader, { "FILL" });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pSkyEnvShader, { });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pSunShader, { "SUN" });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pComposeShader, { "COMPOSE" });
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pCloudsShader, { "CLOUDS" /*,"DITHER", "DISCARD_BY_DEPTH"*/ });
		}
	}

	if (!m_pSunShaftsShader)
	{
		const std::string shaderPath = "Shaders/SunShafts.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pSunShaftsShader, {});
		}
	}

	if (!m_pBlitShader)
	{
		const std::string shaderPath = "Shaders/Blit.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pBlitShader, {});
		}
	}

	if (!m_pCloudsMapTexture)
	{
		if (!m_clouds)
		{
			const std::string path = "Textures/CloudsMap.png";

			if (auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(path))
			{
				App::GetSubmodule<TextureImporter>()->LoadTexture(info->GetFileId(), m_clouds);
			}
		}
		
		m_pCloudsMapTexture = m_clouds->GetRHI();

		if(!m_pCloudsMapTexture)
		{
			return;
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
		auto pathNoiseHigh = std::filesystem::path(AssetRegistry::GetCacheFolder() + std::string("CloudsNoiseHigh.bin"));
		if (!AssetRegistry::ReadBinaryFile(pathNoiseHigh, noiseHigh))
		{
			m_createNoiseHigh = Tasks::CreateTask("Generate Clouds Noise High",
				[=]()
				{
					auto cache = GenerateCloudsNoiseHigh();
					AssetRegistry::WriteBinaryFile(pathNoiseHigh, cache);
				})->Run();

				bShouldReturn = true;
		}

		TVector<uint8_t> noiseLow;
		auto pathNoiseLow = std::filesystem::path(AssetRegistry::GetCacheFolder() + std::string("CloudsNoiseLow.bin"));
		if (!AssetRegistry::ReadBinaryFile(pathNoiseLow, noiseLow))
		{
			m_createNoiseLow = Tasks::CreateTask("Generate Clouds Noise Low",
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
			(uint32_t)glm::max(1.0f, glm::log2((float)CloudsNoiseHighResolution)),
			ETextureType::Texture3D,
			ETextureFormat::R8_UNORM,
			ETextureFiltration::Linear,
			ETextureClamping::Repeat,
			ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);

		driver->SetDebugName(m_pCloudsNoiseHighTexture, "CloudsNoiseHigh");

		m_pCloudsNoiseLowTexture = driver->CreateTexture(noiseLow.GetData(), noiseLow.Num() * sizeof(uint8_t),
			glm::ivec3(CloudsNoiseLowResolution, CloudsNoiseLowResolution, CloudsNoiseLowResolution),
			(uint32_t)glm::max(1.0f, glm::log2((float)CloudsNoiseLowResolution)),
			ETextureType::Texture3D,
			ETextureFormat::R8_UNORM,
			ETextureFiltration::Linear,
			ETextureClamping::Repeat,
			ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit);

		driver->SetDebugName(m_pCloudsNoiseLowTexture, "CloudsNoiseLow");
	}

	if (!m_pStarsShader)
	{
		const std::string shaderPath = "Shaders/Stars.shader";

		if (auto shaderInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(shaderPath))
		{
			App::GetSubmodule<ShaderCompiler>()->LoadShader(shaderInfo->GetFileId(), m_pStarsShader);
		}
	}

	if (!m_pSkyTexture)
	{
		m_pSkyTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(SkyResolution, SkyResolution),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Linear,
			ETextureClamping::Repeat,
			ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit);

		driver->SetDebugName(m_pSkyTexture, "Sky");
	}

	if (!m_pSunTexture)
	{
		m_pSunTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(SunResolution, SunResolution),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Linear,
			ETextureClamping::Clamp,
			ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit);

		driver->SetDebugName(m_pSunTexture, "Sun");
	}

	if (!m_pCloudsTexture)
	{
		const float size = std::min(App::GetMainWindow()->GetRenderArea().x * CloudsResolutionFactor, App::GetMainWindow()->GetRenderArea().y * CloudsResolutionFactor);

		m_pCloudsTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(size, size),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Linear,
			ETextureClamping::Clamp,
			ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::TextureTransferDst_Bit);

		driver->SetDebugName(m_pCloudsTexture, "Clouds");
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
		!m_pSkyEnvShader || !m_pSkyEnvShader->IsReady() ||
		!m_pSunShader || !m_pSunShader->IsReady() ||
		!m_pSunShaftsShader || !m_pSunShaftsShader->IsReady() ||
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

		auto ditherPattern = frameGraph->GetSampler("g_ditherPatternSampler");
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "g_ditherPatternSampler", ditherPattern, 7);

		auto noise = frameGraph->GetSampler("g_noiseSampler");
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "g_noiseSampler", noise, 8);

		auto linearDepth = GetResolvedAttachment("linearDepth");
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "linearDepth", linearDepth, 9);

		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0.0f, false, ECullMode::Front, EBlendMode::None, EFillMode::Fill, 0, false };
		m_pSkyMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSkyShader, m_pShaderBindings);
		m_pSkyEnvMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSkyEnvShader, m_pShaderBindings);
		m_pSunMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSunShader, m_pShaderBindings);
		m_pComposeMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pComposeShader, m_pShaderBindings);
		m_pCloudsMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pCloudsShader, m_pShaderBindings);

		RenderState renderStateMultiply{ false, false, 0, false, ECullMode::Back, EBlendMode::Multiply, EFillMode::Fill, 0, false };
		m_pSunShaftsMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderStateMultiply, m_pSunShaftsShader, m_pShaderBindings);
	}

	// TODO: Should we update each frame?
	if (auto bindings = GetShaderBindings())
	{
		if (bindings->HasBinding("data"))
		{
			if (auto binding = bindings->GetOrAddShaderBinding("data"))
			{
				commands->UpdateShaderBinding(transferCommandList, binding, &m_skyParams, sizeof(SkyNode::SkyParams));
			}
		}
	}

	if (!m_pBlitCloudsMaterial)
	{
		m_pBlitCloudsBindings = driver->CreateShaderBindings();

		// Firstly we must assign the correct layout
		driver->FillShadersLayout(m_pBlitCloudsBindings, { m_pBlitShader->GetDebugVertexShaderRHI(), m_pBlitShader->GetDebugFragmentShaderRHI() }, 1);

		// That should be enough to handle all the uniforms 
		const size_t uniformsSize = std::max(sizeof(SkyParams), m_vectorParams.Num() * sizeof(glm::vec4));
		driver->AddSamplerToShaderBindings(m_pBlitCloudsBindings, "colorSampler", m_pCloudsTexture, 0);

		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::Back, EBlendMode::AlphaBlending, EFillMode::Fill, 0, false };
		m_pBlitCloudsMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pBlitShader, m_pBlitCloudsBindings);
	}

	if (!m_pEnvCubemapBindings[0])
	{
		const glm::mat prjMatrix = Math::PerspectiveRH(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f);
		const TVector<glm::mat4x4> viewMatrices{
			glm::rotate(glm::mat4(1), glm::radians(-90.0f), Math::vec3_Up),
			glm::rotate(glm::mat4(1), glm::radians(90.0f), Math::vec3_Up),
			glm::rotate(glm::rotate(glm::mat4(1), glm::radians(-90.0f), Math::vec3_Right), glm::radians(180.0f), Math::vec3_Up),
			glm::rotate(glm::rotate(glm::mat4(1), glm::radians(90.0f), Math::vec3_Right), glm::radians(180.0f), Math::vec3_Up),
			glm::rotate(glm::mat4(1), glm::radians(180.0f), Math::vec3_Up),
			glm::rotate(glm::mat4(1), glm::radians(0.0f), Math::vec3_Up),
		};

		for (uint32_t face = 0; face < 6; face++)
		{
			m_pEnvCubemapBindings[face] = driver->CreateShaderBindings();
			Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(m_pEnvCubemapBindings[face], "frameData", sizeof(RHI::UboFrameData), 0, RHI::EShaderBindingType::UniformBuffer);

			RHI::UboFrameData frameData{};
			frameData.m_cameraPosition = sceneView.m_cameraTransform.m_position;
			frameData.m_projection = prjMatrix;
			frameData.m_invProjection = glm::inverse(prjMatrix);

			frameData.m_view = viewMatrices[face];
			frameData.m_viewportSize = glm::ivec2(128, 128);

			RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCommandList,
				m_pEnvCubemapBindings[face]->GetOrAddShaderBinding("frameData"),
				&frameData,
				sizeof(frameData));
		}
	}

	if (!m_pStarsMaterial)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::Back, EBlendMode::Additive, EFillMode::Point, 0, false };
		m_pStarsMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::PointList, renderState, m_pStarsShader);
	}

	RHI::RHITexturePtr target = GetResolvedAttachment("color");
	RHI::RHIRenderTargetPtr depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	const auto depthAttachmentLayout = RHI::IsDepthStencilFormat(depthAttachment->GetFormat()) ? EImageLayout::DepthStencilAttachmentOptimal : EImageLayout::DepthAttachmentOptimal;

	auto mesh = frameGraph->GetFullscreenNdcQuad();

	const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
	const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

	commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
	commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

	commands->BeginDebugRegion(commandList, "Sky", DebugContext::Color_CmdPostProcess);
	{
		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), m_pSkyTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{m_pSkyTexture},
			nullptr,
			glm::vec4(0, 0, m_pSkyTexture->GetExtent().x, m_pSkyTexture->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			0.0f,
			false);

		commands->BindMaterial(commandList, m_pSkyMaterial);
		commands->BindShaderBindings(commandList, m_pSkyMaterial, { sceneView.m_frameBindings, m_pShaderBindings });

		commands->SetViewport(commandList,
			0, 0,
			(float)m_pSkyTexture->GetExtent().x, (float)m_pSkyTexture->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(m_pSkyTexture->GetExtent().x, m_pSkyTexture->GetExtent().y),
			0, 1.0f);

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pSkyTexture->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	if (m_skyParams.m_cloudsDensity > 0.0f)
	{
		commands->BeginDebugRegion(commandList, "Clouds", DebugContext::Color_CmdPostProcess);
		{
			commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
			commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

			commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), m_pSkyTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
			commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), m_pCloudsTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

			commands->SetViewport(commandList,
				0, 0,
				(float)m_pCloudsTexture->GetExtent().x, (float)m_pCloudsTexture->GetExtent().y,
				glm::vec2(0, 0),
				glm::vec2(m_pCloudsTexture->GetExtent().x, m_pCloudsTexture->GetExtent().y),
				0, 1.0f);

			commands->BeginRenderPass(commandList,
				TVector<RHI::RHITexturePtr>{m_pCloudsTexture},
				nullptr,
				glm::vec4(0, 0, m_pCloudsTexture->GetExtent().x, m_pCloudsTexture->GetExtent().y),
				glm::ivec2(0, 0),
				false,
				glm::vec4(0.0f),
				0.0f,
				false);

			commands->BindMaterial(commandList, m_pCloudsMaterial);
			commands->BindShaderBindings(commandList, m_pCloudsMaterial, { sceneView.m_frameBindings, m_pShaderBindings });
			commands->PushConstants(commandList, m_pCloudsMaterial, sizeof(uint32_t), &m_ditherPatternIndex);

			commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
			commands->EndRenderPass(commandList);

			commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSkyTexture->GetDefaultLayout());
			commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pCloudsTexture->GetDefaultLayout());
		}
		commands->EndDebugRegion(commandList);
	}
	else
	{
		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), m_pCloudsTexture->GetDefaultLayout(), EImageLayout::TransferDstOptimal);
		commands->ClearImage(commandList, m_pCloudsTexture, glm::vec4(0, 0, 0, 0));
		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), EImageLayout::TransferDstOptimal, m_pCloudsTexture->GetDefaultLayout());
	}

	commands->BeginDebugRegion(commandList, "Sun", DebugContext::Color_CmdPostProcess);
	{
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), m_pSunTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), m_pCloudsTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);

		commands->SetViewport(commandList,
			0, 0,
			(float)m_pSunTexture->GetExtent().x, (float)m_pSunTexture->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(m_pSunTexture->GetExtent().x, m_pSunTexture->GetExtent().y),
			0, 1.0f);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{m_pSunTexture},
			nullptr,
			glm::vec4(0, 0, m_pSunTexture->GetExtent().x, m_pSunTexture->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			0.0f,
			false);

		commands->BindMaterial(commandList, m_pSunMaterial);
		commands->BindShaderBindings(commandList, m_pSunMaterial, { sceneView.m_frameBindings, m_pShaderBindings });		

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pCloudsTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pSunTexture->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->BeginDebugRegion(commandList, "Compose", DebugContext::Color_CmdPostProcess);
	{
		commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), m_pSkyTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), m_pSunTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);

		commands->SetViewport(commandList,
			0, 0,
			(float)target->GetExtent().x, (float)target->GetExtent().y,
			glm::vec2(0, 0),
			glm::vec2(target->GetExtent().x, target->GetExtent().y),
			0, 1.0f);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{target},
			nullptr,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			0.0f,
			false);

		commands->BindMaterial(commandList, m_pComposeMaterial);
		commands->BindShaderBindings(commandList, m_pComposeMaterial, { sceneView.m_frameBindings, m_pShaderBindings });

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSkyTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSunTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
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
		pushConstants.m_starsModelView = glm::translate(glm::mat4(1), glm::vec3(sceneView.m_cameraTransform.m_position)) * m_starsModelView;

		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), depthAttachmentLayout);
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);
		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), m_pCloudsTexture->GetDefaultLayout(), EImageLayout::ShaderReadOnlyOptimal);

		commands->BeginRenderPass(commandList,
			TVector<RHI::RHITexturePtr>{target},
			depthAttachment,
			glm::vec4(0, 0, target->GetExtent().x, target->GetExtent().y),
			glm::ivec2(0, 0),
			false,
			glm::vec4(0.0f),
			0.0f,
			false);

		commands->BindMaterial(commandList, m_pStarsMaterial);
		commands->BindShaderBindings(commandList, m_pStarsMaterial, { sceneView.m_frameBindings, m_pShaderBindings });
		commands->PushConstants(commandList, m_pStarsMaterial, sizeof(PushConstants), &pushConstants);

		commands->SetDefaultViewport(commandList);
		
		commands->DrawIndexed(commandList, (uint32_t)m_starsMesh->m_indexBuffer->GetSize() / sizeof(uint32_t), 1u, 0u, 0u, 0u);

		commands->BeginDebugRegion(commandList, "Blit Clouds", DebugContext::Color_CmdPostProcess);
		{
			commands->BindVertexBuffer(commandList, mesh->m_vertexBuffer, 0);
			commands->BindIndexBuffer(commandList, mesh->m_indexBuffer, 0);

			commands->BindMaterial(commandList, m_pBlitCloudsMaterial);
			commands->BindShaderBindings(commandList, m_pBlitCloudsMaterial, { sceneView.m_frameBindings, m_pBlitCloudsBindings });
			commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		}
		commands->EndDebugRegion(commandList);

		commands->BeginDebugRegion(commandList, "Sun Shafts", DebugContext::Color_CmdPostProcess);
		{
			commands->BindMaterial(commandList, m_pSunShaftsMaterial);
			commands->BindShaderBindings(commandList, m_pSunShaftsMaterial, { sceneView.m_frameBindings, m_pShaderBindings });
			commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		}
		commands->EndDebugRegion(commandList);

		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pCloudsTexture, m_pCloudsTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pCloudsTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachmentLayout, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	if (m_bIsDirty)
	{
		RHI::RHICubemapPtr cubemap = frameGraph->GetSampler("g_skyCubemap").DynamicCast<RHICubemap>();

		if (!cubemap)
		{
			cubemap = RHI::Renderer::GetDriver()->CreateCubemap(glm::ivec2(EnvCubemapSize, EnvCubemapSize), 8, RHI::EFormat::R16G16B16A16_SFLOAT);
			RHI::Renderer::GetDriver()->SetDebugName(cubemap, "g_skyCubemap");

			frameGraph->SetSampler("g_skyCubemap", cubemap);
		}

		if (cubemap)
		{
			commands->BeginDebugRegion(commandList, "Generate Environment Map", DebugContext::Color_CmdGraphics);

			uint32_t face = m_updateEnvCubemapPattern;
			if (face < 6)
			{
				RHITexturePtr targetFace = cubemap->GetFace(face, 0);

				commands->ImageMemoryBarrier(commandList, targetFace, targetFace->GetFormat(), targetFace->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

				commands->BeginRenderPass(commandList,
					TVector<RHI::RHITexturePtr>{targetFace},
					nullptr,
					glm::vec4(0, 0, targetFace->GetExtent().x, targetFace->GetExtent().y),
					glm::ivec2(0, 0),
					false,
					glm::vec4(0.0f),
					0.0f,
					false);

				commands->BindMaterial(commandList, m_pSkyEnvMaterial);
				commands->BindShaderBindings(commandList, m_pSkyEnvMaterial, { m_pEnvCubemapBindings[face], m_pShaderBindings });

				commands->SetViewport(commandList,
					0, 0,
					(float)targetFace->GetExtent().x, (float)targetFace->GetExtent().y,
					glm::vec2(0, 0),
					glm::vec2(targetFace->GetExtent().x, targetFace->GetExtent().y),
					0, 1.0f);

				commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
				commands->EndRenderPass(commandList);

				commands->ImageMemoryBarrier(commandList, targetFace, targetFace->GetFormat(), EImageLayout::ColorAttachmentOptimal, targetFace->GetDefaultLayout());
			}
			else
			{
				commands->ImageMemoryBarrier(commandList, cubemap, cubemap->GetFormat(), cubemap->GetDefaultLayout(), EImageLayout::TransferDstOptimal);
				commands->GenerateMipMaps(commandList, cubemap);
			}

			commands->EndDebugRegion(commandList);
		}

		if (m_updateEnvCubemapPattern == 7)
		{
			m_bIsDirty = false;

			if (auto node = frameGraph->GetGraphNode("Environment").DynamicCast<EnvironmentNode>())
			{
				node->MarkDirty();
			}

		}
		m_updateEnvCubemapPattern++;
	}

	commands->EndDebugRegion(commandList);

	m_ditherPatternIndex++;
}

void SkyNode::Clear()
{
	m_pSkyTexture.Clear();
	m_pSunTexture.Clear();
	m_pSkyShader.Clear();
	m_pSkyEnvShader.Clear();
	m_pSkyMaterial.Clear();
	m_pSkyEnvMaterial.Clear();
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