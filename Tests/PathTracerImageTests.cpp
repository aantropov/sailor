#include "FrameGraph/CPUPathTracerNode.h"
#include "RHI/SceneView.h"
#include "Support/TaskTestApp.h"

#include <iostream>
#include <latch>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace Sailor;
using namespace Sailor::Raytracing;

namespace
{
	static_assert(std::is_same_v<decltype(std::declval<RHI::RHIPathTracerScenePtr>().GetRawPtr()), const RHI::RHIPathTracerScene*>);

	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void RequireColor(const glm::vec4& actual, const glm::vec4& expected)
	{
		if (!(glm::length(actual - expected) < 1e-5f))
		{
			std::cerr << "RGBA: " << actual.r << ", " << actual.g << ", " << actual.b << ", " << actual.a
				<< "; expected: " << expected.r << ", " << expected.g << ", " << expected.b << ", " << expected.a << '\n';
			throw std::runtime_error("linear RGBA must match the expected radiance and coverage");
		}
	}

	class ImageNode final : public Framegraph::CPUPathTracerNode
	{
	public:
		using CPUPathTracerNode::AccumulationKey;
		using CPUPathTracerNode::ApplyCompletedReadback;
		using CPUPathTracerNode::SubmissionResources;
		using CPUPathTracerNode::CompleteImageRequests;
		CameraState& Camera(uint32_t index = 0) { return GetCameraState(index); }
		size_t NumCameras() const { return m_cameras.Num(); }
		void AccumulateImage(const TVector<vec4>& image, uvec2 extent, uint32_t samples, uint32_t cameraIndex = 0)
		{
			CPUPathTracerNode::AccumulateImage(Camera(cameraIndex), image, extent, samples);
		}
	};

	void TestRequestedImageOwnership()
	{
		Tests::TaskTestApp app;
		auto& scheduler = app.GetScheduler();
		scheduler.Initialize();
		auto node = TRefPtr<ImageNode>::Make();
		auto onRender = [](auto action)
		{
			auto task = Tasks::CreateTask("Path tracer capture fixture"_h, std::move(action), EThreadType::Render);
			task->Run();
			task->Wait();
		};
		onRender([&]() { node->AccumulateImage({ vec4(4, 0, 0, 1) }, uvec2(1), 1); });
		auto first = node->DoOneCapture(0);
		auto otherCamera = node->DoOneCapture(1);
		auto sameCamera = node->DoOneCapture(0);
		scheduler.WaitIdle({ EThreadType::Render });
		scheduler.ProcessTasksOnMainThread();
		Require(!first->IsFinished(), "request admission cannot return the previous CPU image");
		onRender([&]()
			{
				node->AccumulateImage({ vec4(0, 3, 0, 1) }, uvec2(1), 2, 1);
				node->CompleteImageRequests(1, 12, &node->Camera(1));
			});
		Require(!otherCamera->IsFinished(), "Render cannot mutate Main's capture result before the handoff runs");
		scheduler.ProcessTasksOnMainThread();
		Require(otherCamera->IsFinished() && otherCamera->GetResult() && !first->IsFinished(),
			"another camera's completed image must not satisfy the requested view");
		RequireColor(otherCamera->GetResult()->m_pixels[0], vec4(0, 3, 0, 1));
		onRender([&]()
			{
				node->AccumulateImage({ vec4(0, 0, 8, 1) }, uvec2(1), 1);
				node->CompleteImageRequests(0, 13, &node->Camera());
			});
		scheduler.ProcessTasksOnMainThread();
		Require(first->IsFinished() && sameCamera->IsFinished() && first != sameCamera && first->GetResult() == sameCamera->GetResult(),
			"coalesced captures may share a completed image, never the request task");
		const auto retained = first->GetResult();
		Require(retained && retained->m_frame == 13 && retained->m_cameraIndex == 0 && retained->m_extent == uvec2(1) &&
			retained->m_accumulatedSamples == 2, "capture metadata must belong to the processed camera");
		RequireColor(retained->m_pixels[0], vec4(2, 0, 4, 1));
		auto later = node->DoOneCapture();
		scheduler.WaitIdle({ EThreadType::Render });
		scheduler.ProcessTasksOnMainThread();
		Require(!later->IsFinished(), "retaining an earlier capture must not complete a later request");
		onRender([&]()
			{
				node->AccumulateImage({ vec4(1, 2, 3, 1), vec4(0) }, uvec2(2, 1), 4);
				node->CompleteImageRequests(0, 14, &node->Camera());
			});
		scheduler.ProcessTasksOnMainThread();
		Require(later->IsFinished() && later->GetResult() && later->GetResult() != retained &&
			later->GetResult()->m_extent == uvec2(2, 1) && later->GetResult()->m_imageRevision > retained->m_imageRevision,
			"resize must publish a distinct image with its own extent and revision");
		RequireColor(retained->m_pixels[0], vec4(2, 0, 4, 1));
		auto unavailable = node->DoOneCapture();
		scheduler.WaitIdle({ EThreadType::Render });
		onRender([&]() { node->CompleteImageRequests(0, 15); });
		scheduler.ProcessTasksOnMainThread();
		Require(unavailable->IsFinished() && !unavailable->GetResult(), "no new image must be reported as failure, not replaced with stale pixels");
		auto cleared = node->DoOneCapture();
		scheduler.WaitIdle({ EThreadType::Render });
		onRender([&]() { node->Clear(); });
		scheduler.ProcessTasksOnMainThread();
		Require(cleared->IsFinished() && !cleared->GetResult(), "Clear must cancel a pending CPU image request");
		RequireColor(retained->m_pixels[0], vec4(2, 0, 4, 1));
		auto abandoned = node->DoOneCapture();
		scheduler.WaitIdle({ EThreadType::Render });
		onRender([&]() { node.Clear(); });
		scheduler.ProcessTasksOnMainThread();
		Require(abandoned->IsFinished() && !abandoned->GetResult(), "node destruction must complete its pending requests");
	}

	void TestSceneDemandPublication()
	{
		Tests::TaskTestApp app;
		app.GetScheduler().Initialize();
		auto node = TRefPtr<ImageNode>::Make();
		Require(!node->IsEnabled(RHI::ESceneViewRenderMode::Lit), "an unconfigured tracer must not demand scene snapshots");
		Framegraph::BaseFrameGraphNode& base = *node;
		base.SetFloat("enabled"_h, 1);
		Require(node->IsEnabled(RHI::ESceneViewRenderMode::Lit), "base-node configuration must publish tracer demand too");
		std::latch start(2);
		auto update = Tasks::CreateTask("Publish tracer demand"_h, [&]()
			{
				start.arrive_and_wait();
				for (uint32_t i = 0; i < 20000; ++i)
				{
					base.SetFloat("samplesPerFrame"_h, static_cast<float>(i + 1));
					base.SetFloat("enabled"_h, static_cast<float>(i % 2));
				}
				base.SetFloat("enabled"_h, 0);
			}, EThreadType::Render);
		update->Run();
		start.arrive_and_wait();
		uint32_t enabledReads = 0;
		bool bDebugWasEnabled = false;
		for (uint32_t i = 0; i < 20000; ++i)
		{
			enabledReads += node->IsEnabled(RHI::ESceneViewRenderMode::Lit);
			bDebugWasEnabled |= node->IsEnabled(RHI::ESceneViewRenderMode::AmbientOcclusion);
		}
		update->Wait();
		Require(enabledReads <= 20000 && !bDebugWasEnabled && !node->IsEnabled(RHI::ESceneViewRenderMode::Lit),
			"Main may observe changing demand without reading Render's mutable parameter map; debug modes remain disabled");
	}

	void TestLinearAccumulationAndDisplayExport()
	{
		ImageNode node;
		TVector<u8vec4> display;
		uvec2 extent(0);
		Require(!node.GetLastRenderedImage(display, extent), "an empty accumulator has no display image");
		node.AccumulateImage({ vec4(8, 0.0625f, 0.25f, 1), vec4(0) }, uvec2(2, 1), 2);
		node.AccumulateImage({ vec4(0, 0.3125f, 0.25f, 0), vec4(0, 4, 0, 0.5f) }, uvec2(2, 1), 6);
		Require(node.Camera().m_accumulatedSamples == 8, "sample counts, not frame counts, must weight the mean");
		RequireColor(node.Camera().m_accumulatedImage[0], vec4(2, 0.25f, 0.25f, 0.25f));
		RequireColor(node.Camera().m_accumulatedImage[1], vec4(0, 3, 0, 0.375f));
		ImageNode reverse;
		reverse.AccumulateImage({ vec4(0, 0.3125f, 0.25f, 0), vec4(0, 4, 0, 0.5f) }, uvec2(2, 1), 6);
		reverse.AccumulateImage({ vec4(8, 0.0625f, 0.25f, 1), vec4(0) }, uvec2(2, 1), 2);
		RequireColor(reverse.Camera().m_accumulatedImage[0], vec4(2, 0.25f, 0.25f, 0.25f));
		RequireColor(reverse.Camera().m_accumulatedImage[1], vec4(0, 3, 0, 0.375f));
		Require(reverse.Camera().m_accumulatedSamples == 8, "reordering sample batches must preserve weighted radiance and sample count");
		Require(node.GetLastRenderedImage(display, extent) && extent == uvec2(2, 1) && display.Num() == 2,
			"display export must preserve dimensions");
		Require(display[0] == u8vec4(255, 136, 136, 64) && display[1] == u8vec4(0, 255, 0, 96),
			"only display RGB is sRGB-encoded and clamped; alpha remains linear coverage");
		RequireColor(node.Camera().m_accumulatedImage[0], vec4(2, 0.25f, 0.25f, 0.25f));
		Require(node.GetLastRenderedImage(display, extent) && display[0] == u8vec4(255, 136, 136, 64),
			"repeated display export must not apply another transfer function");

		node.AccumulateImage({ vec4(4, 2, 3, 1), vec4(0) }, uvec2(1, 2), 3);
		Require(node.Camera().m_accumulatedSamples == 3, "changing shape must restart accumulation even with the same pixel count");
		RequireColor(node.Camera().m_accumulatedImage[0], vec4(4, 2, 3, 1));
		Require(node.GetLastRenderedImage(display, extent) && extent == uvec2(1, 2), "display extent must follow the new image");
		node.Clear();
		Require(node.Camera().m_accumulatedSamples == 0 && node.Camera().m_accumulatedImage.IsEmpty() && !node.GetLastRenderedImage(display, extent),
			"Clear must discard accumulated radiance and display availability");
		node.AccumulateImage({ vec4(0, 0, 0, 1) }, uvec2(1), 1);
		node.AccumulateImage({ vec4(1) }, uvec2(1), 1);
		RequireColor(node.Camera().m_accumulatedImage[0], vec4(0.5f, 0.5f, 0.5f, 1));
		Require(node.GetLastRenderedImage(display, extent) && display[0] == u8vec4(187, 187, 187, 255),
			"black and white must average to linear 0.5, then encode once to sRGB 187, not average display bytes");
	}

	void TestCameraAndReadbackOwnership()
	{
		ImageNode node;
		node.AccumulateImage({ vec4(2, 0, 0, 1) }, uvec2(1), 2, 0);
		node.AccumulateImage({ vec4(0, 3, 0, 1) }, uvec2(1), 5, 1);
		Require(node.NumCameras() == 2 && node.Camera(0).m_accumulatedSamples == 2 && node.Camera(1).m_accumulatedSamples == 5,
			"camera accumulators must preserve their own samples");
		RequireColor(node.Camera(0).m_accumulatedImage[0], vec4(2, 0, 0, 1));
		RequireColor(node.Camera(1).m_accumulatedImage[0], vec4(0, 3, 0, 1));
		TVector<u8vec4> display;
		uvec2 extent(0);
		Require(node.GetLastRenderedImage(display, extent) && display[0] == u8vec4(0, 255, 0, 255),
			"the existing display API must return the last selected camera");

		auto batch = TRefPtr<ImageNode::SubmissionResources>::Make();
		batch->m_readbackCompletion = RHI::RHIFencePtr::Make();
		node.Camera(0).m_pendingReadback = batch;
		Require(!node.ApplyCompletedReadback(node.Camera(0), {}, {}) && node.Camera(0).m_pendingReadback == batch,
			"unsubmitted readback must remain pending without accessing mapped bytes");
		batch->m_readbackCompletion->MarkSubmissionFailed();
		Require(!node.ApplyCompletedReadback(node.Camera(0), {}, {}) && !node.Camera(0).m_pendingReadback,
			"failed readback must be discarded without decoding its buffers");
		const auto revision = node.Camera(0).m_imageRevision;
		batch->m_readbackCompletion = RHI::RHIFencePtr::Make();
		node.Camera(0).m_pendingReadback = batch;
		node.Clear();
		Require(node.NumCameras() == 0 && !node.GetLastRenderedImage(display, extent),
			"Clear must discard every camera and pending readback publication");
		node.AccumulateImage({ vec4(0, 0, 4, 1) }, uvec2(1), 1);
		Require(node.Camera().m_imageRevision > revision && !node.Camera().m_pendingReadback,
			"a reset camera must not reuse an old flight image or readback");
		batch->m_imageRevision = 7;
		batch->InvalidateSubmission();
		Require(batch->m_imageRevision == 0, "a refused submission must require another upload");
	}

	void TestPreparedLinearImage()
	{
		PathTracer::TLASInstance instance;
		instance.m_triangles = TSharedPtr<TVector<Math::Triangle>>::Make();
		const vec3 vertices[] = { vec3(-1, -1, 0), vec3(1, -1, 0), vec3(1, 1, 0), vec3(-1, 1, 0) };
		const uint32_t indices[] = { 0, 1, 2, 0, 2, 3 };
		for (uint32_t i = 0; i < 6; i += 3)
		{
			Math::Triangle triangle{};
			for (uint32_t j = 0; j < 3; ++j)
			{
				triangle.m_vertices[j] = vertices[indices[i + j]];
				triangle.m_normals[j] = vec3(0, 0, 1);
				triangle.m_tangent[j] = vec3(1, 0, 0);
				triangle.m_bitangent[j] = vec3(0, 1, 0);
				triangle.m_colors[j] = vec4(1);
			}
			triangle.m_centroid = (triangle.m_vertices[0] + triangle.m_vertices[1] + triangle.m_vertices[2]) / 3.0f;
			instance.m_triangles->Add(triangle);
		}
		instance.m_worldBounds = Math::AABB(vec3(0), vec3(1, 1, 0));
		auto material = TSharedPtr<PathTracer::MaterialSnapshot>::Make();
		material->m_parameters.m_baseColorFactor = vec4(0, 0, 0, 1);
		material->m_parameters.m_emissiveFactor = vec3(2, 0.5f, 0.125f);
		PathTracer tracer;
		Require(tracer.InitializeSceneSnapshot({ instance }, { material }, {}, false), "the generated emissive quad must prepare");
		PathTracer::Params params{};
		params.m_height = 16;
		params.m_numSamples = params.m_numAmbientSamples = params.m_maxBounces = params.m_msaa = 1;
		params.m_bUseRuntimeCamera = params.m_bRunTasksInline = true;
		params.m_bIncludeDirectLighting = params.m_bIncludeEnvironment = false;
		params.m_runtimeCameraPos = vec3(0, 0, 3);
		params.m_runtimeAspectRatio = 1;
		params.m_runtimeHFov = 0.8f;
		Require(tracer.RenderPreparedScene(params), "the generated quad must render without a GPU");
		Require(tracer.GetLastRenderedExtent() == uvec2(16) && tracer.GetLastRenderedImageLinear().Num() == 256,
			"linear output must retain the render dimensions");
		const size_t center = 8 * 16 + 8;
		RequireColor(tracer.GetLastRenderedImageLinear()[center], vec4(2, 0.5f, 0.125f, 1));
		RequireColor(tracer.GetLastRenderedImageLinear()[0], vec4(0));
		Require(tracer.GetLastRenderedImage()[center] == u8vec4(255, 187, 99, 255) && tracer.GetLastRenderedImage()[0] == u8vec4(0),
			"byte export must encode known emissive RGB while preserving transparent black");
		RequireColor(tracer.GetLastRenderedImageLinear()[center], vec4(2, 0.5f, 0.125f, 1));

		params.m_bIncludeEmissive = false;
		Require(tracer.RenderPreparedScene(params), "the next render must replace both output representations");
		RequireColor(tracer.GetLastRenderedImageLinear()[center], vec4(0, 0, 0, 1));
		Require(tracer.GetLastRenderedImage()[center] == u8vec4(0, 0, 0, 255), "byte export must not reuse the previous emissive image");
		Require(!tracer.InitializeSceneSnapshot({}, {}, {}, false) && !tracer.RenderPreparedScene(params), "an empty scene must fail rendering");
		Require(tracer.GetLastRenderedExtent() == uvec2(0) && tracer.GetLastRenderedImageLinear().IsEmpty() && tracer.GetLastRenderedImage().IsEmpty(),
			"a failed render must discard both the linear image and the display cache");
	}

	void TestAccumulationIdentity()
	{
		ImageNode::AccumulationKey key;
		key.m_outputExtent = uvec2(64);
		key.m_sceneRevision = key.m_lightingRevision = key.m_environmentHash = 1;
		key.m_samplesPerFrame = key.m_maxBounces = 1;
		Require(key == key, "an unchanged tracing identity must match");
		auto differs = [&](auto edit, const char* message)
		{
			auto changed = key;
			edit(changed);
			Require(!(key == changed) && !(changed == key), message);
		};
		differs([](auto& k) { ++k.m_sceneRevision; }, "scene changes must invalidate accumulation");
		differs([](auto& k) { ++k.m_lightingRevision; }, "light changes must invalidate accumulation");
		differs([](auto& k) { ++k.m_environmentHash; }, "completed environment changes must invalidate accumulation");
		differs([](auto& k) { ++k.m_outputExtent.x; }, "width changes must invalidate accumulation");
		differs([](auto& k) { ++k.m_outputExtent.y; }, "height changes must invalidate accumulation");
		differs([](auto& k) { ++k.m_samplesPerFrame; }, "sampling changes must invalidate accumulation");
		differs([](auto& k) { ++k.m_maxBounces; }, "bounce changes must invalidate accumulation");
		differs([](auto& k) { k.m_rayBiasBase += 0.01f; }, "base bias changes must invalidate accumulation");
		differs([](auto& k) { k.m_rayBiasScale += 0.01f; }, "scaled bias changes must invalidate accumulation");
		differs([](auto& k) { k.m_cameraPosition.x += 1; }, "camera motion must still invalidate accumulation");
		differs([](auto& k) { k.m_cameraForward.x += 1; }, "camera direction must still invalidate accumulation");
		differs([](auto& k) { k.m_cameraUp.x += 1; }, "camera roll must still invalidate accumulation");
		differs([](auto& k) { k.m_cameraAspect += 1; }, "camera aspect must still invalidate accumulation");
		differs([](auto& k) { k.m_cameraHFov += 1; }, "camera FOV must still invalidate accumulation");
		auto jittered = key;
		jittered.m_cameraPosition.x += 1e-5f;
		Require(key == jittered, "the existing camera tolerance must be preserved");
		RHI::RHISceneViewSnapshot snapshot;
		snapshot.m_pathTracerScene = TSharedPtr<RHI::RHIPathTracerScene>::Make();
		snapshot.ResetForReuse();
		Require(!snapshot.m_pathTracerScene, "snapshot reuse must release the previous tracer scene publication");
	}
}

int main()
{
	try
	{
		TestLinearAccumulationAndDisplayExport();
		TestCameraAndReadbackOwnership();
		TestPreparedLinearImage();
		TestAccumulationIdentity();
		TestRequestedImageOwnership();
		TestSceneDemandPublication();
		std::cout << "Path tracer linear HDR image tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
