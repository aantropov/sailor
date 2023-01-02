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
#include "glm/glm/gtx/quaternion.hpp"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

#ifndef _SAILOR_IMPORT_
const char* SkyNode::m_name = "Sky";
#endif

glm::vec3 SkyNode::s_rgbTemperatures[s_maxRgbTemperatures];

void SkyNode::CreateStarsMesh(RHI::RHICommandListPtr transferCommandList)
{
	SAILOR_PROFILE_FUNCTION();

	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
	commands->BeginDebugRegion(transferCommandList, GetName(), DebugContext::Color_CmdTransfer);

	m_starsMesh = driver->CreateMesh();
	m_starsMesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
	m_starsMesh->m_bounds = Math::AABB(vec3(0), vec3(1000, 1000, 1000));

	std::string temperatures;
	if (!AssetRegistry::ReadAllTextFile(AssetRegistry::ContentRootFolder + std::string("StarsColor.yaml"), temperatures))
	{
		return;
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

	// http://www.geomidpoint.com/example.html
	// Longitude in radians (east positive)
	// Latitude in radians (north positive)
	//
	// New York lat	40° 42' 51.6708" N	40.7143528	0.710599509
	// New York lon    74° 0' 21.5022" W	-74.0059731	-1.291647896
	// Rome lat 41.8919300 degrees
	// Rome lon 12.5113300 degrees

	constexpr float latitudeRad = glm::radians(41.8919300f);
	constexpr float longitudeRad = glm::radians(12.5113300f);

	const double jdn2022 = Utils::CalculateJulianDate(2022, 12, 29, 12, 0, 0);

	// Calculate rotation matrix based on time, latitude and longitude
	double localMeanSiderealTime = 4.894961f + 230121.675315f * jdn2022 + longitudeRad;

	// Exploration of different rotations
	glm::quat rotation = angleAxis(-(float)localMeanSiderealTime, Math::vec3_Backward) * angleAxis(latitudeRad - pi<float>() * 0.5f, Math::vec3_Up);

	glm::quat precessionRotationZ = angleAxis(0.01118f, Math::vec3_Backward);
	glm::quat precession = (precessionRotationZ * angleAxis(-0.00972f, Math::vec3_Right)) * precessionRotationZ;

	m_starsModelView = glm::toMat4(precession);

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

	const VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.Num();
	const VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.Num();

	// Handle both buffers in memory closier each other
	const EBufferUsageFlags flags = EBufferUsageBit::VertexBuffer_Bit | EBufferUsageBit::IndexBuffer_Bit;

	m_starsMesh->m_vertexBuffer = driver->CreateBuffer(transferCommandList,
		&vertices[0],
		bufferSize,
		flags);

	m_starsMesh->m_indexBuffer = driver->CreateBuffer(transferCommandList,
		&indices[0],
		indexBufferSize,
		flags);

	commands->EndDebugRegion(transferCommandList);
}

void SkyNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

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
		}
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

		return;
	}

	if (!m_pSunTexture)
	{
		m_pSunTexture = driver->CreateRenderTarget(
			commandList,
			glm::ivec2(SkyResolution, SkyResolution),
			1,
			ETextureFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Bicubic,
			ETextureClamping::Clamp,
			ETextureUsageBit::Sampled_Bit | ETextureUsageBit::ColorAttachment_Bit);

		driver->SetDebugName(m_pSunTexture, "Sun");

		return;
	}

	if (!m_starsMesh)
	{
		CreateStarsMesh(transferCommandList);
	}

	if (!m_pStarsShader || !m_pStarsShader->IsReady() ||
		!m_pSkyShader || !m_pSkyShader->IsReady() ||
		!m_pSunShader || !m_pSunShader->IsReady() ||
		!m_pComposeShader || !m_pComposeShader->IsReady() ||
		!m_starsMesh || !m_starsMesh->IsReady())
	{
		return;
	}

	if (!m_pSkyMaterial)
	{
		m_pShaderBindings = driver->CreateShaderBindings();

		// Firstly we must assign the correct layout
		driver->FillShadersLayout(m_pShaderBindings, { m_pSkyShader->GetDebugVertexShaderRHI(), m_pSkyShader->GetDebugFragmentShaderRHI() }, 1);

		// That should be enough to handle all the uniforms 
		const size_t uniformsSize = std::max(256ull, m_vectorParams.Num() * sizeof(glm::vec4));
		RHIShaderBindingPtr data = driver->AddBufferToShaderBindings(m_pShaderBindings, "data", uniformsSize, 0, RHI::EShaderBindingType::UniformBuffer);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "skySampler", m_pSkyTexture, 1);
		driver->AddSamplerToShaderBindings(m_pShaderBindings, "sunSampler", m_pSunTexture, 2);

		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		RenderState renderState{ false, false, 0, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0, false };
		m_pSkyMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSkyShader, m_pShaderBindings);
		m_pSunMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pSunShader, m_pShaderBindings);
		m_pComposeMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, renderState, m_pComposeShader, m_pShaderBindings);

		const glm::vec4 initLightDirection = normalize(glm::vec4(0, -1, 1, 0));
		commands->UpdateShaderBindingVariable(transferCommandList, data, "lightDirection", &initLightDirection, sizeof(glm::vec4));
	}

	if (!m_pStarsMaterial)
	{
		RHI::RHIVertexDescriptionPtr vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3C4>();
		RenderState renderState{ true, false, 0, false, ECullMode::Back, EBlendMode::AlphaBlending, EFillMode::Point, 0, false };
		m_pStarsMaterial = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::PointList, renderState, m_pStarsShader);
	}

	RHI::RHITexturePtr target = GetResolvedAttachment("color");
	RHI::RHITexturePtr depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");

	auto mesh = frameGraph->GetFullscreenNdcQuad();

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

		const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ColorAttachmentOptimal, m_pSkyTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->BeginDebugRegion(commandList, "Sun", DebugContext::Color_CmdPostProcess);
	{
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), m_pSunTexture->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

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

		const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

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

		const uint32_t firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();

		commands->DrawIndexed(commandList, 6, 1, firstIndex, vertexOffset, 0);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, m_pSkyTexture, m_pSkyTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSkyTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, m_pSunTexture, m_pSunTexture->GetFormat(), EImageLayout::ShaderReadOnlyOptimal, m_pSunTexture->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->BeginDebugRegion(commandList, "Stars", DebugContext::Color_CmdGraphics);
	{
		commands->BindVertexBuffer(commandList, m_starsMesh->m_vertexBuffer, m_starsMesh->m_vertexBuffer->GetOffset());
		commands->BindIndexBuffer(commandList, m_starsMesh->m_indexBuffer, m_starsMesh->m_indexBuffer->GetOffset());

		PushConstants pushConstants{};
		pushConstants.m_starsModelView = glm::translate(glm::mat4(1), glm::vec3(sceneView.m_cameraPosition)) * m_starsModelView;

		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), depthAttachment->GetDefaultLayout(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal);
		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), target->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

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

		const uint32_t firstIndex = (uint32_t)m_starsMesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
		const uint32_t vertexOffset = (uint32_t)m_starsMesh->m_vertexBuffer->GetOffset() / (uint32_t)m_starsMesh->m_vertexDescription->GetVertexStride();

		commands->DrawIndexed(commandList, (uint32_t)m_starsMesh->m_indexBuffer->GetSize() / sizeof(uint32_t), 1u, firstIndex, vertexOffset, 0u);
		commands->EndRenderPass(commandList);

		commands->ImageMemoryBarrier(commandList, target, target->GetFormat(), EImageLayout::ColorAttachmentOptimal, target->GetDefaultLayout());
		commands->ImageMemoryBarrier(commandList, depthAttachment, depthAttachment->GetFormat(), EImageLayout::DepthAttachmentStencilReadOnlyOptimal, depthAttachment->GetDefaultLayout());
	}
	commands->EndDebugRegion(commandList);

	commands->EndDebugRegion(commandList);
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