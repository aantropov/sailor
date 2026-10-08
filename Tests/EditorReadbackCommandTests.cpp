#include "Sailor.h"
#include "FrameGraph/EditorReadbackNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/SceneView.h"
#include "RHI/Surface.h"
#include <array>
#include <format>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	enum class SourceKind { Target, Surface, Texture };

	struct Source
	{
		RHIResourcePtr m_resource;
		RHITexturePtr m_texture;
	};

	Source CreateSource(SourceKind kind, glm::ivec2 extent, EFormat format)
	{
		auto& driver = Renderer::GetDriver();
		if (kind == SourceKind::Texture)
		{
			const std::vector<uint32_t> pixels(static_cast<size_t>(extent.x) * extent.y, 0);
			auto texture = driver->CreateImage_Immediate(pixels.data(), pixels.size() * sizeof(uint32_t),
				glm::ivec3(extent, 1), 1, ETextureType::Texture2D, format);
			Require(texture.IsValid(), "readback source texture must upload");
			return { texture, texture };
		}
		auto target = driver->CreateRenderTarget(extent, 1, format, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		Require(target.IsValid(), "readback source target must allocate");
		return { kind == SourceKind::Surface ? RHIResourcePtr(driver->CreateSurface(target)) : RHIResourcePtr(target), target };
	}

	void Bind(RHIFrameGraph& graph, StringHash name, SourceKind kind, const Source& source)
	{
		if (kind == SourceKind::Surface) graph.SetSurface(name, source.m_resource.DynamicCast<RHISurface>());
		else if (kind == SourceKind::Target) graph.SetRenderTarget(name, source.m_texture.DynamicCast<RHIRenderTarget>());
		else graph.SetSampler(name, source.m_texture);
	}

	struct Capture
	{
		RHICommandListPtr m_command;
		RHIFencePtr m_completion;
		glm::ivec2 m_extent;
		glm::u8vec4 m_color;
		uint64_t m_generation;
	};

	void CheckPixels(const ReadbackFramePtr& frame, const Capture& capture)
	{
		Require(frame && frame->m_extent == capture.m_extent && frame->m_generation == capture.m_generation &&
			frame->GetBgraBytesPerRow() == uint32_t(capture.m_extent.x) * 4, "readback metadata must match its recorded source");
		const auto* pixels = frame->GetBgraPixels();
		const auto color = capture.m_color;
		for (int pixel = 0; pixel < capture.m_extent.x * capture.m_extent.y; ++pixel)
			Require(pixels[pixel * 4] == color.b && pixels[pixel * 4 + 1] == color.g &&
				pixels[pixel * 4 + 2] == color.r && pixels[pixel * 4 + 3] == color.a,
				"every normalized readback pixel must come from the selected source");
	}

	Capture Record(EditorReadbackNode& node, RHIFrameGraphPtr graph, const Source& source,
		glm::u8vec4 color, uint64_t generation)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		Capture capture{ driver->CreateCommandList(false, ECommandListQueue::Graphics), {}, source.m_texture->GetExtent(), color, generation };
		RHISceneViewSnapshot scene;
		scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		scene.m_submissionContext->BeginSubmission(generation, 0, 0, generation);
		commands->BeginCommandList(capture.m_command, true);
		commands->ImageMemoryBarrier(capture.m_command, source.m_texture, EImageLayout::TransferDstOptimal);
		commands->ClearImage(capture.m_command, source.m_texture, glm::vec4(color) / 255.0f);
		node.Process(graph, {}, capture.m_command, scene);
		commands->EndCommandList(capture.m_command);
		capture.m_completion = scene.m_submissionContext->GetFrameCompletion();
		Require(!node.TakeCompletedFrame(), "recording alone must not publish readback pixels");
		return capture;
	}

	ReadbackFramePtr Complete(EditorReadbackNode& node, const Capture& capture)
	{
		Require(capture.m_completion.IsValid(), "the selected source must record a readback completion");
		Require(Renderer::GetDriver()->SubmitCommandList(capture.m_command, capture.m_completion) &&
			capture.m_completion->Wait(5000000000ull) == EFenceStatus::Finished, "readback copy must finish on the GPU");
		auto frame = node.TakeCompletedFrame();
		CheckPixels(frame, capture);
		Require(!node.TakeCompletedFrame(), "completed readback must publish only once");
		return frame;
	}

	void TestSource(SourceKind kind, bool bNamed, EFormat format)
	{
		auto graph = RHIFrameGraphPtr::Make();
		auto node = TRefPtr<EditorReadbackNode>::Make();
		if (bNamed) node->SetRHIResource_Unresolved("src"_h, "CaptureSource"_h);
		ReadbackFramePtr retained;
		Capture first, pending;
		Source source;
		uint64_t expectedBytes = 0;
		for (uint32_t frame = 0; frame < 6; ++frame)
		{
			if (frame == 5) node->Clear();
			if (frame != 1)
			{
				source = CreateSource(kind, { 5 + int(frame) * 2, 3 + int(frame) }, format);
				if (bNamed) Bind(*graph, "CaptureSource"_h, kind, source);
				else node->SetRHIResource("src"_h, source.m_resource);
			}
			const glm::u8vec4 color(17 + frame * 13, 61 + frame * 7, 211 - frame * 19, 127 + frame * 11);
			// One retained readback leaves just one reusable buffer with a single flight.
			if (frame == 3 && Renderer::GetDriver()->GetMaxFramesInFlight() == 1u)
			{
				Complete(*node, pending);
			}
			auto capture = Record(*node, graph, source, color, 100 + frame);
			expectedBytes += uint64_t(capture.m_extent.x) * capture.m_extent.y * 4;
			if (frame == 2) pending = capture;
			else
			{
				if (frame == 3 && Renderer::GetDriver()->GetMaxFramesInFlight() > 1u)
				{
					Complete(*node, pending);
				}
				auto completed = Complete(*node, capture);
				if (frame == 0) { retained = completed; first = capture; }
			}
			if (retained) CheckPixels(retained, first);
		}
		const auto convertedBytes = format == EFormat::R8G8B8A8_UNORM ? expectedBytes : 0;
		Require(node->GetStats().m_recordedReadbackBytes == expectedBytes &&
			node->GetStats().m_convertedBytes == convertedBytes, "each source must be copied once; only RGBA needs conversion");
		node->Clear();
		CheckPixels(retained, first);
		std::cout << "Editor readback source kind=" << int(kind) << " named=" << bNamed << " format=" << int(format) <<
			": six frames, all pixels, replacement, deferred submission, retained frame and Clear passed\n";
	}

	void TestSourcePolicy()
	{
		const std::array names{ "EditorOutput"_h, "Main"_h, "BackBuffer"_h, "Secondary"_h };
		for (uint32_t scenario = 0; scenario < 12; ++scenario)
		{
			auto graph = RHIFrameGraphPtr::Make();
			auto node = TRefPtr<EditorReadbackNode>::Make();
			std::array<Source, 4> sources;
			const auto first = scenario < 4 ? scenario : 0;
			for (uint32_t i = first; i < sources.size(); ++i)
			{
				sources[i] = CreateSource(SourceKind::Target, { 7 + int(i), 3 + int(i) }, EFormat::B8G8R8A8_UNORM);
				Bind(*graph, names[i], SourceKind::Target, sources[i]);
			}
			uint32_t selected = first;
			bool bExpected = true;
			if (scenario == 4 || scenario == 5)
			{
				auto unsupported = CreateSource(SourceKind::Surface, { 4, 4 }, EFormat::R32_SFLOAT);
				Bind(*graph, names[0], SourceKind::Surface, unsupported);
				if (scenario == 4) { graph->SetRenderTarget(names[0], {}); selected = 1; }
			}
			else if (scenario == 6)
			{
				node->SetRHIResource_Unresolved("src"_h, "MissingSource"_h);
				bExpected = false;
			}
			else if (scenario == 7)
			{
				auto unsupported = CreateSource(SourceKind::Target, { 4, 4 }, EFormat::R32_SFLOAT);
				node->SetRHIResource("src"_h, unsupported.m_resource);
				bExpected = false;
			}
			else if (scenario == 8)
			{
				sources[0] = CreateSource(SourceKind::Texture, { 11, 7 }, EFormat::B8G8R8A8_UNORM);
				graph->SetRenderTarget(names[0], {});
				Bind(*graph, names[0], SourceKind::Texture, sources[0]);
			}
			else if (scenario == 9)
			{
				const auto unsupported = CreateSource(SourceKind::Target, { 4, 4 }, EFormat::R32_SFLOAT);
				Bind(*graph, "UnsupportedSource"_h, SourceKind::Target, unsupported);
				node->SetRHIResource_Unresolved("src"_h, "UnsupportedSource"_h);
				bExpected = false;
			}
			else if (scenario == 10)
			{
				const auto unsupported = CreateSource(SourceKind::Surface, { 4, 4 }, EFormat::R32_SFLOAT);
				Bind(*graph, names[0], SourceKind::Surface, unsupported);
				node->SetRHIResource_Unresolved("src"_h, names[0]);
			}
			else if (scenario == 11)
			{
				sources[0] = CreateSource(SourceKind::Surface, { 11, 7 }, EFormat::B8G8R8A8_UNORM);
				const auto target = sources[0].m_resource.DynamicCast<RHISurface>()->GetTarget();
				node->SetRHIResource("src"_h, target);
				bExpected = target->GetMsaaSamples() == EMsaaSamples::Samples_1;
			}
			const auto capture = Record(*node, graph, sources[selected], { 19, 71, 213, 129 }, 200 + scenario);
			if (bExpected) Complete(*node, capture);
			else
			{
				Require(!capture.m_completion && node->GetStats().m_recordedReadbackBytes == 0,
					"an explicit missing or unsupported source must not capture a default target");
				Require(Renderer::GetDriver()->SubmitCommandList_Immediate(capture.m_command) && !node->TakeCompletedFrame(),
					"a skipped capture must not publish a frame");
			}
			std::cout << "Editor readback policy scenario=" << scenario << ": GPU pixels or explicit no-capture passed\n";
		}
	}
}

namespace Sailor::Tests
{
	void RunEditorReadbackCommandTests()
	{
		Require(App::HasEditor(), "readback contracts need the hidden editor-mode renderer");
		auto task = Tasks::CreateTask<std::string>("Editor readback sources"_h, []()
		{
			std::string failures;
			for (auto kind : { SourceKind::Target, SourceKind::Surface, SourceKind::Texture })
			for (bool bNamed : { false, true })
			for (auto format : { EFormat::R8G8B8A8_UNORM, EFormat::B8G8R8A8_UNORM })
			{
				try { TestSource(kind, bNamed, format); }
				catch (const std::exception& error)
				{
					failures += std::format("Editor readback kind={} named={} format={}: {}\n", int(kind), bNamed, int(format), error.what());
				}
			}
			try { TestSourcePolicy(); }
			catch (const std::exception& error) { failures += std::string("Editor readback policy: ") + error.what(); }
			return failures;
		}, EThreadType::Render);
		task->Run();
		task->Wait();
		Require(task->GetResult().empty(), task->GetResult());
	}
}
