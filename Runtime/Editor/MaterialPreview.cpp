#include "MaterialPreview.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "ECS/CameraECS.h"
#include "FrameGraph/ClearNode.h"
#include "FrameGraph/LightCullingNode.h"
#include "FrameGraph/PostProcessNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/RenderSceneNode.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Cubemap.h"
#include "RHI/Readback.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include "RHI/VertexDescription.h"
#include "YamlExceptionBoundary.h"
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;
using namespace Sailor::EditorRuntime;

namespace
{
	std::mutex g_previewMutex;
	MaterialPreviewResult g_previewResult;
	std::optional<MaterialPreviewRequest> g_pendingPreview;
	std::deque<uint64_t> g_closedPreviewSessions;
	bool g_previewWorkerRunning = false;
	std::atomic<uint64_t> g_previewRenderCount{};

	void RememberClosedSession(uint64_t sessionId)
	{
		if (!sessionId || std::find(g_closedPreviewSessions.begin(), g_closedPreviewSessions.end(), sessionId) != g_closedPreviewSessions.end()) return;
		g_closedPreviewSessions.push_back(sessionId);
		if (g_closedPreviewSessions.size() > 128) g_closedPreviewSessions.pop_front();
	}

	bool IsCurrent(const MaterialPreviewRequest& request)
	{
		const std::lock_guard lock(g_previewMutex);
		return g_previewResult.m_sessionId == request.m_sessionId &&
			g_previewResult.m_revision == request.m_revision &&
			g_previewResult.m_state == EMaterialPreviewState::Pending;
	}

	bool OnRender(std::function<bool()> operation)
	{
		auto task = Tasks::CreateTask<bool>("Render material preview"_h, [operation = std::move(operation)]()
		{
			try { return operation(); }
			catch (...) { return false; }
		}, EThreadType::Render);
		task->Run();
		task->Wait();
		return task->GetResult();
	}

	bool WaitForGpu(const RHIFencePtr& fence)
	{
		if (!fence) return false;
		const auto status = fence->Wait(30000000000ull);
		Renderer::GetDriver()->TrackResources_ThreadSafe();
		return status == EFenceStatus::Finished;
	}

	void MakeGeometry(EMaterialPreviewPrimitive primitive,
		TVector<VertexP3N3T3B3UV2C4I4W4>& vertices, TVector<uint32_t>& indices)
	{
		const auto addVertex = [&](glm::vec3 position, glm::vec3 normal, glm::vec3 tangent, glm::vec2 uv)
		{
			VertexP3N3T3B3UV2C4I4W4 vertex{};
			vertex.m_position = position;
			vertex.m_normal = normal;
			vertex.m_tangent = tangent;
			vertex.m_bitangent = glm::cross(normal, tangent);
			vertex.m_texcoord = uv;
			vertex.m_color = glm::vec4(1.0f);
			vertex.m_boneIds = glm::ivec4(0);
			vertex.m_boneWeights = glm::vec4(0.0f);
			vertices.Add(vertex);
		};
		if (primitive == EMaterialPreviewPrimitive::Sphere)
		{
			constexpr uint32_t rings = 32, sectors = 64;
			for (uint32_t ring = 0; ring <= rings; ++ring)
			for (uint32_t sector = 0; sector <= sectors; ++sector)
			{
				const float u = float(sector) / sectors, v = float(ring) / rings;
				const float phi = u * glm::two_pi<float>(), theta = v * glm::pi<float>();
				const glm::vec3 normal(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
				addVertex(normal, normal, { -std::sin(phi), 0.0f, std::cos(phi) }, { u, v });
			}
			for (uint32_t ring = 0; ring < rings; ++ring)
			for (uint32_t sector = 0; sector < sectors; ++sector)
			{
				const uint32_t first = ring * (sectors + 1) + sector, next = first + sectors + 1;
				for (auto index : { first, first + 1, next, next, first + 1, next + 1 }) indices.Add(index);
			}
			return;
		}
		const auto addFace = [&](glm::vec3 normal, glm::vec3 tangent, float offset)
		{
			const glm::vec3 bitangent = glm::cross(normal, tangent);
			const uint32_t first = static_cast<uint32_t>(vertices.Num());
			for (const auto uv : { glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1), glm::vec2(1, 1) })
				addVertex(normal * offset + tangent * (uv.x * 2 - 1) + bitangent * (uv.y * 2 - 1), normal, tangent, uv);
			for (const auto index : { 0u, 1u, 2u, 2u, 1u, 3u }) indices.Add(first + index);
		};
		addFace({ 0, 0, 1 }, { 1, 0, 0 }, primitive == EMaterialPreviewPrimitive::Plane ? 0.0f : 1.0f);
		if (primitive == EMaterialPreviewPrimitive::Box)
		{
			addFace({ 0, 0, -1 }, { -1, 0, 0 }, 1);
			addFace({ 1, 0, 0 }, { 0, 0, -1 }, 1);
			addFace({ -1, 0, 0 }, { 0, 0, 1 }, 1);
			addFace({ 0, 1, 0 }, { 1, 0, 0 }, 1);
			addFace({ 0, -1, 0 }, { 1, 0, 0 }, 1);
		}
		const float scale = 1.0f / std::sqrt(primitive == EMaterialPreviewPrimitive::Box ? 3.0f : 2.0f);
		for (auto& vertex : vertices) vertex.m_position *= scale;
	}

	struct PreviewJob final : RHIResource
	{
		~PreviewJob()
		{
			if (m_material) m_material.DestroyObject(m_allocator);
		}
		Memory::ObjectAllocatorPtr m_allocator = Memory::ObjectAllocatorPtr::Make();
		MaterialPtr m_material;
		RHIFrameGraphPtr m_graph = RHIFrameGraphPtr::Make();
		TRefPtr<RenderSceneNode> m_draw;
		RHISceneViewPtr m_view = RHISceneViewPtr::Make();
		RHISceneInstanceRecord m_record;
		TRefPtr<RHISceneProxyResource> m_proxy;
		RHIMeshPtr m_mesh;
		TSharedPtr<ReadbackFrame> m_readback;
	};

	bool LoadPreviewShader(const char* path, ShaderSetPtr& shader, const TVector<std::string>& defines = {})
	{
		auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(path);
		return info && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(info->GetFileId(), shader, defines) && shader && shader->IsReady();
	}

	bool InitializePreview(TRefPtr<PreviewJob> job, const MaterialPreviewRequest& request,
		const ShaderSetPtr& brdfShader, RHIFencePtr& completion)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const glm::ivec2 extent(request.m_width, request.m_height);
		auto color = driver->CreateRenderTarget(extent, 1, EFormat::R16G16B16A16_SFLOAT);
		auto depth = driver->CreateRenderTarget(extent, 1, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		auto motion = driver->CreateRenderTarget(extent, 1, EFormat::R16G16_SFLOAT);
		auto output = driver->CreateRenderTarget(extent, 1, EFormat::R16G16B16A16_SFLOAT);
		auto exposure = driver->CreateRenderTarget(glm::ivec2(1), 1, EFormat::R16G16B16A16_SFLOAT);
		if (!color || !depth || !motion || !output || !exposure) return false;
		auto colorSurface = RHISurfacePtr::Make(color, color, false);
		auto depthSurface = RHISurfacePtr::Make(depth, depth, false);
		auto motionSurface = RHISurfacePtr::Make(motion, motion, false);
		auto graph = job->m_graph;
		graph->SetSurface("Main"_h, colorSurface);
		graph->SetSurface("PreviewDepth"_h, depthSurface);
		graph->SetSurface("PreviewMotion"_h, motionSurface);
		graph->SetRenderTarget("LinearDepth"_h, color);
		graph->SetRenderTarget("PreviewOutput"_h, output);

		auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(upload, true);
		commands->ImageMemoryBarrier(upload, exposure, EImageLayout::TransferDstOptimal);
		commands->ClearImage(upload, exposure, glm::vec4(1.0f / 9.6f));
		commands->ImageMemoryBarrier(upload, exposure, EImageLayout::ShaderReadOnlyOptimal);
		TVector<VertexP3N3T3B3UV2C4I4W4> vertices;
		TVector<uint32_t> indices;
		MakeGeometry(request.m_primitive, vertices, indices);
		job->m_mesh = driver->CreateMesh();
		job->m_mesh->m_vertexDescription = driver->GetOrAddVertexDescription<VertexP3N3T3B3UV2C4I4W4>();
		job->m_mesh->m_bounds = Math::AABB(glm::vec3(-1), glm::vec3(1));
		job->m_mesh->m_vertexBuffer = driver->CreateBuffer(upload, vertices.GetData(), vertices.Num() * sizeof(vertices[0]), EBufferUsageBit::VertexBuffer_Bit);
		job->m_mesh->m_indexBuffer = driver->CreateBuffer(upload, indices.GetData(), indices.Num() * sizeof(indices[0]), EBufferUsageBit::IndexBuffer_Bit);

		const auto usage = ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Storage_Bit;
		auto environment = driver->CreateCubemap(glm::ivec2(1), 1, EFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Linear, ETextureClamping::Clamp, usage);
		auto irradiance = driver->CreateCubemap(glm::ivec2(1), 1, EFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Linear, ETextureClamping::Clamp, usage);
		auto brdf = driver->CreateRenderTarget(glm::ivec2(64), 1, EFormat::R16G16B16A16_SFLOAT,
			ETextureFiltration::Linear, ETextureClamping::Clamp, usage);
		if (!job->m_mesh->m_vertexBuffer || !job->m_mesh->m_indexBuffer || !environment || !irradiance || !brdf) return false;
		for (const auto& texture : { environment, irradiance })
		{
			commands->ImageMemoryBarrier(upload, texture, EImageLayout::TransferDstOptimal);
			commands->ClearImage(upload, texture, glm::vec4(glm::vec3(texture == irradiance ? 0.08f * glm::pi<float>() : 0.08f), 1));
			commands->ImageMemoryBarrier(upload, texture, EImageLayout::ShaderReadOnlyOptimal);
		}
		auto brdfBindings = driver->CreateShaderBindings();
		driver->AddStorageImageToShaderBindings(brdfBindings, "dst"_h, brdf, 0);
		commands->ImageMemoryBarrier(upload, brdf, EImageLayout::ComputeWrite);
		commands->Dispatch(upload, brdfShader->GetComputeShaderRHI(), 2, 2, 1, { brdfBindings }, nullptr, 0);
		commands->ImageMemoryBarrier(upload, brdf, EImageLayout::ShaderReadOnlyOptimal);
		graph->SetSampler("g_envCubemap"_h, environment);
		graph->SetSampler("g_irradianceCubemap"_h, irradiance);
		graph->SetSampler("g_sheenEnvCubemap"_h, environment);
		graph->SetSampler("g_brdfSampler"_h, brdf);
		commands->EndCommandList(upload);
		completion = RHIFencePtr::Make();
		completion->AddDependency(job);
		if (!driver->SubmitCommandList(upload, completion)) return false;

		const auto clear = [&](RHIResourcePtr target, glm::vec4 value)
		{
			auto node = TRefPtr<ClearNode>::Make();
			node->SetRHIResource("target"_h, target);
			node->SetVec4("clearColor"_h, value);
			node->SetFloat("clearDepth"_h, 0);
			node->SetFloat("clearStencil"_h, 0);
			graph->GetGraph().Add(node);
		};
		clear(colorSurface, glm::vec4(0.055f, 0.055f, 0.055f, 1));
		clear(depthSurface, glm::vec4(0));
		clear(motionSurface, glm::vec4(0));
		graph->GetGraph().Add(TRefPtr<LightCullingNode>::Make());
		auto draw = TRefPtr<RenderSceneNode>::Make();
		job->m_draw = draw;
		draw->SetString("Tag"_h, job->m_material->GetRenderState().GetTag() == "Transparent"_h.GetHash() ? "Transparent" :
			job->m_material->GetRenderState().GetTag() == "Masked"_h.GetHash() ? "Masked" : "Opaque");
		draw->SetString("GPUCulling"_h, "false");
		draw->SetString("VirtualizeInstancePayloads"_h, "false");
		draw->SetRHIResource("color"_h, colorSurface);
		draw->SetRHIResource("depthStencil"_h, depthSurface);
		draw->SetRHIResource("motionVectors"_h, motionSurface);
		graph->GetGraph().Add(draw);
		auto tonemap = TRefPtr<PostProcessNode>::Make();
		tonemap->SetString("shader"_h, "Shaders/Tonemapping.shader");
		tonemap->SetString("defines"_h, "ACES");
		tonemap->SetVec4("data.whitePoint"_h, glm::vec4(11.2f));
		tonemap->SetVec4("data.exposureCompensation"_h, glm::vec4(0));
		tonemap->SetRHIResource("color"_h, output);
		tonemap->SetRHIResource("colorSampler"_h, color);
		tonemap->SetRHIResource("averageLuminanceSampler"_h, exposure);
		tonemap->PreloadShader();
		graph->GetGraph().Add(tonemap);

		RHISceneViewProxy proxy;
		proxy.m_meshes.Add(job->m_mesh);
		proxy.m_meshModelMatrices.Add(glm::mat4(1));
		proxy.m_overrideMaterials.Add(job->m_material->GetOrAddRHI(job->m_mesh->m_vertexDescription));
		proxy.m_renderQueueTags.Add(job->m_material->GetRenderState().GetTag());
#if defined(__APPLE__)
		proxy.m_materialTextureSamplers.Add(job->m_material->GetRenderMetadata().m_textureSamplers);
#endif
		job->m_proxy = TRefPtr<RHISceneProxyResource>::Make(std::move(proxy));
		job->m_record.m_worldBounds = job->m_mesh->m_bounds;
		job->m_record.m_topology = job->m_proxy;
		job->m_view->m_snapshots.Resize(1);
		auto& view = job->m_view->m_snapshots[0];
		view.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		view.m_submissionContext->BeginSubmission(request.m_revision, 0, Material::GetGlobalContentRevision(), request.m_sessionId);
		view.m_submissionCompletionToken = RHISubmissionCompletionTokenPtr::Make();
		view.m_proxies.Emplace(RenderInstanceHandle{ 0, 1 }, job->m_record, *job->m_proxy);
		view.m_bGlobalIlluminationEnabled = false;
		view.m_camera = TUniquePtr<CameraData>::Make();
		const glm::vec3 position(request.m_distance * std::cos(request.m_pitch) * std::sin(request.m_yaw),
			request.m_distance * std::sin(request.m_pitch), request.m_distance * std::cos(request.m_pitch) * std::cos(request.m_yaw));
		view.m_cameraTransform.m_position = glm::vec4(position, 1);
		view.m_camera->SetFov(45);
		view.m_camera->SetAspect(float(request.m_width) / request.m_height);
		view.m_camera->SetZNear(0.05f);
		view.m_camera->SetZFar(50);
		view.m_camera->SetViewMatrix(glm::lookAtRH(position, glm::vec3(0), glm::vec3(0, 1, 0)));
		view.m_camera->SetProjectionMatrix(Math::PerspectiveRH(glm::radians(45.0f), view.m_camera->GetAspect(), 0.05f, 50));
		auto lights = TSharedPtr<TVector<RHILightShaderData>>::Make();
		for (const auto direction : { glm::vec3(-0.5f, -0.7f, -0.8f), glm::vec3(0.8f, -0.2f, -0.4f), glm::vec3(-0.2f, -0.3f, 1.0f) })
		{
			RHILightShaderData light;
			light.m_type = static_cast<uint32_t>(ELightType::Directional);
			light.m_direction = glm::normalize(direction);
			light.m_intensity = glm::vec3(lights->IsEmpty() ? 2.2f : 0.65f);
			lights->Add(light);
		}
		view.m_cpuLightsData = lights;
		view.m_totalNumLights = static_cast<uint32_t>(lights->Num());
		view.m_lightingRevision = 1;
		job->m_readback = TSharedPtr<ReadbackFrame>::Make();
		job->m_readback->m_extent = extent;
		job->m_readback->m_format = color->GetFormat();
		job->m_readback->m_bytesPerRow = request.m_width * 8;
		job->m_readback->m_buffer = driver->CreateBuffer(size_t(request.m_width) * request.m_height * 8,
			EBufferUsageBit::BufferTransferDst_Bit, EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
		return job->m_readback->m_buffer.IsValid();
	}

	bool SubmitPreview(TRefPtr<PreviewJob> job, RHIFencePtr& completion)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		TVector<RHICommandListPtr> transfers, draws;
		RHISemaphorePtr ready;
		bool success = job->m_graph->Process(job->m_view, transfers, draws, {}, ready, false);
		// A graph can finish while an unusable surface pipeline skipped the mesh.
		success = success && job->m_draw && job->m_draw->GetDrawCallStats().m_numInstances > 0;
		for (size_t i = 0; success && i < transfers.Num(); ++i)
		for (const auto& command : { transfers[i], draws[i] })
		{
			auto next = driver->CreateWaitSemaphore();
			auto fence = RHIFencePtr::Make();
			fence->AddDependency(job);
			if (!driver->SubmitCommandList(command, fence, next, ready)) { success = false; break; }
			ready = next;
		}
		// Also retire successful internal graph submissions when a later submit fails.
		auto read = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(read, true);
		if (success)
		{
			auto color = job->m_graph->GetRenderTarget("PreviewOutput"_h);
			commands->ImageMemoryBarrier(read, color, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(read, color, job->m_readback->m_buffer);
			commands->MemoryBarrier(read, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit), static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		}
		commands->EndCommandList(read);
		completion = job->m_view->m_snapshots[0].m_submissionContext->GetOrCreateFrameCompletion();
		completion->AddDependency(job);
		job->m_readback->m_completion = completion;
		if (!driver->SubmitCommandList(read, completion, {}, ready)) return false;
		if (success) ++g_previewRenderCount;
		return success;
	}

	MaterialPreviewResult RenderPreview(const MaterialPreviewRequest& request)
	{
		MaterialPreviewResult result;
		result.m_sessionId = request.m_sessionId;
		result.m_revision = request.m_revision;
		result.m_width = request.m_width;
		result.m_height = request.m_height;
		result.m_state = EMaterialPreviewState::Error;
		YAML::Node document;
		auto asset = TSharedPtr<MaterialAsset>::Make();
		if (!External::TryLoadYaml(request.m_materialYaml, document, result.m_diagnostic) ||
			!External::GuardYamlExceptions([&]() { asset->Deserialize(document); }, result.m_diagnostic)) return result;
		if (!asset->GetShader() || (asset->GetRenderQueue() != "Opaque" && asset->GetRenderQueue() != "Masked" && asset->GetRenderQueue() != "Transparent"))
		{
			result.m_diagnostic = "Preview requires a surface shader and an Opaque, Masked or Transparent render queue.";
			return result;
		}
		auto shaderAsset = App::GetSubmodule<ShaderCompiler>()->LoadShaderAsset(asset->GetShader());
		if (!shaderAsset || shaderAsset->GetGlslVertexCode().empty() || shaderAsset->GetGlslFragmentCode().empty())
		{
			result.m_diagnostic = "The selected shader does not provide a surface vertex and fragment program.";
			return result;
		}
		auto job = TRefPtr<PreviewJob>::Make();
		auto materialTask = App::GetSubmodule<MaterialImporter>()->CreateTransientMaterial(asset, job->m_allocator, job->m_material);
		if (materialTask) materialTask->Wait();
		if (!materialTask || !materialTask->GetResult())
		{
			result.m_diagnostic = "The material shader or one of its textures could not be loaded.";
			return result;
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!job->m_material->IsReady() && IsCurrent(request) && std::chrono::steady_clock::now() < deadline)
		{
			Renderer::GetDriver()->TrackResources_ThreadSafe();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		if (!IsCurrent(request)) return result;
		if (!job->m_material->IsReady())
		{
			result.m_diagnostic = "Material GPU initialization did not complete.";
			return result;
		}
		ShaderSetPtr brdf, lightCulling, tonemap;
		if (!LoadPreviewShader("Shaders/ComputeBrdfLut.shader", brdf) || !LoadPreviewShader("Shaders/ComputeLightCulling.shader", lightCulling) ||
			!LoadPreviewShader("Shaders/Tonemapping.shader", tonemap, { "ACES" }))
		{
			result.m_diagnostic = "Preview lighting shaders are unavailable.";
			return result;
		}
		RHIFencePtr initialized;
		const bool initializedOk = OnRender([&]() { return InitializePreview(job, request, brdf, initialized); });
		const bool initializedComplete = initialized ? WaitForGpu(initialized) : false;
		if (!initializedOk || !initializedComplete)
		{
			result.m_diagnostic = "Preview GPU resources could not be initialized.";
			return result;
		}
		if (!IsCurrent(request)) return result;
		for (const auto& task : job->m_graph->Prepare(job->m_view)) { task->Run(); task->Wait(); }
		RHIFencePtr completed;
		const bool submitted = OnRender([&]() { return SubmitPreview(job, completed); });
		const bool finished = completed ? WaitForGpu(completed) : false;
		if (!submitted || !finished)
		{
			result.m_diagnostic = job->m_draw && job->m_draw->GetDrawCallStats().m_numInstances == 0 ?
				"The material shader could not draw the preview primitive." : "Preview GPU rendering or readback failed.";
			return result;
		}
		if (!IsCurrent(request)) return result;
		TVector<glm::u8vec4> pixels;
		if (!job->m_readback->CopySrgbPixels(pixels))
		{
			result.m_diagnostic = "Preview pixels could not be read.";
			return result;
		}
		const auto append = [](void* context, void* bytes, int count)
		{
			auto& png = *static_cast<std::vector<uint8_t>*>(context);
			const auto* first = static_cast<const uint8_t*>(bytes);
			png.insert(png.end(), first, first + count);
		};
		if (!stbi_write_png_to_func(append, &result.m_png, request.m_width, request.m_height, 4, pixels.GetData(), request.m_width * 4))
		{
			result.m_png.clear();
			result.m_diagnostic = "Preview PNG encoding failed.";
			return result;
		}
		result.m_state = EMaterialPreviewState::Ready;
		return result;
	}

	void RunPreviewWorker()
	{
		for (;;)
		{
			MaterialPreviewRequest request;
			{
				const std::lock_guard lock(g_previewMutex);
				if (!g_pendingPreview) { g_previewWorkerRunning = false; return; }
				request = std::move(*g_pendingPreview);
				g_pendingPreview.reset();
			}
			MaterialPreviewResult result;
			result.m_sessionId = request.m_sessionId;
			result.m_revision = request.m_revision;
			result.m_state = EMaterialPreviewState::Error;
			try { result = RenderPreview(request); }
			catch (const std::exception& error) { result.m_diagnostic = error.what(); }
			catch (...) { result.m_diagnostic = "Unexpected material preview failure."; }
			if (result.m_diagnostic.size() > 8192) result.m_diagnostic.resize(8192);
			const std::lock_guard lock(g_previewMutex);
			if (g_previewResult.m_sessionId == request.m_sessionId && g_previewResult.m_revision == request.m_revision &&
				g_previewResult.m_state == EMaterialPreviewState::Pending) g_previewResult = std::move(result);
		}
	}
}

bool Sailor::EditorRuntime::UpdateMaterialPreview(MaterialPreviewRequest request, MaterialPreviewResult& result, std::string& diagnostic)
{
	if (!request.m_sessionId || !request.m_revision || request.m_materialYaml.empty() || request.m_materialYaml.size() > 1024 * 1024 ||
		!std::isfinite(request.m_yaw) || !std::isfinite(request.m_pitch) || !std::isfinite(request.m_distance) ||
		request.m_primitive < EMaterialPreviewPrimitive::Sphere || request.m_primitive > EMaterialPreviewPrimitive::Box)
	{
		diagnostic = "Invalid material preview session, revision, material or camera controls.";
		return false;
	}
	if (!request.m_width) request.m_width = 384;
	if (!request.m_height) request.m_height = 384;
	if (request.m_width < 64 || request.m_height < 64 || request.m_width > 512 || request.m_height > 512)
	{
		diagnostic = "Material preview dimensions must be between 64 and 512 pixels.";
		return false;
	}
	if (!App::GetSubmodule<Tasks::Scheduler>() || !App::IsRendererInitialized())
	{
		diagnostic = "The engine renderer is not running.";
		return false;
	}
	request.m_yaw = std::remainder(request.m_yaw, glm::two_pi<float>());
	request.m_pitch = std::clamp(request.m_pitch, -1.45f, 1.45f);
	request.m_distance = std::clamp(request.m_distance == 0 ? 3.0f : request.m_distance, 1.5f, 12.0f);
	const std::lock_guard lock(g_previewMutex);
	if (std::find(g_closedPreviewSessions.begin(), g_closedPreviewSessions.end(), request.m_sessionId) != g_closedPreviewSessions.end())
	{
		result = {};
		result.m_sessionId = request.m_sessionId;
		result.m_revision = request.m_revision;
		return true;
	}
	if (g_previewResult.m_sessionId == request.m_sessionId && g_previewResult.m_revision >= request.m_revision)
	{
		result = g_previewResult;
		result.m_png.clear();
		return true;
	}
	if (g_previewResult.m_sessionId != request.m_sessionId) RememberClosedSession(g_previewResult.m_sessionId);
	g_previewResult = {};
	g_previewResult.m_sessionId = request.m_sessionId;
	g_previewResult.m_revision = request.m_revision;
	g_previewResult.m_state = EMaterialPreviewState::Pending;
	g_previewResult.m_width = request.m_width;
	g_previewResult.m_height = request.m_height;
	g_pendingPreview = std::move(request);
	result = g_previewResult;
	if (!g_previewWorkerRunning)
	{
		g_previewWorkerRunning = true;
		Tasks::CreateTask("Material preview worker"_h, RunPreviewWorker, EThreadType::Background)->Run();
	}
	return true;
}

MaterialPreviewResult Sailor::EditorRuntime::GetMaterialPreview(uint64_t sessionId, uint64_t afterRevision)
{
	const std::lock_guard lock(g_previewMutex);
	if (sessionId != g_previewResult.m_sessionId)
	{
		MaterialPreviewResult result;
		result.m_sessionId = sessionId;
		return result;
	}
	auto result = g_previewResult;
	if (result.m_revision <= afterRevision) result.m_png.clear();
	return result;
}

MaterialPreviewResult Sailor::EditorRuntime::CloseMaterialPreview(uint64_t sessionId)
{
	const std::lock_guard lock(g_previewMutex);
	MaterialPreviewResult result;
	result.m_sessionId = sessionId;
	RememberClosedSession(sessionId);
	if (sessionId == g_previewResult.m_sessionId)
	{
		g_pendingPreview.reset();
		result.m_revision = g_previewResult.m_revision;
		g_previewResult = result;
	}
	return result;
}

void Sailor::EditorRuntime::ResetMaterialPreviewForAppLifecycle()
{
	const std::lock_guard lock(g_previewMutex);
	g_pendingPreview.reset();
	g_closedPreviewSessions.clear();
	g_previewResult = {};
	g_previewWorkerRunning = false;
}

uint64_t Sailor::EditorRuntime::GetMaterialPreviewRenderCount() { return g_previewRenderCount.load(); }
