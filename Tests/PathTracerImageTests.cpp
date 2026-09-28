#include "FrameGraph/CPUPathTracerNode.h"
#include "RHI/SceneView.h"

#include <iostream>
#include <stdexcept>

using namespace Sailor;
using namespace Sailor::Raytracing;

namespace
{
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
		using CPUPathTracerNode::AccumulateImage;
		using CPUPathTracerNode::m_accumulatedImage;
		using CPUPathTracerNode::m_accumulatedSamples;
	};

	void TestLinearAccumulationAndDisplayExport()
	{
		ImageNode node;
		TVector<u8vec4> display;
		uvec2 extent(0);
		Require(!node.GetLastRenderedImage(display, extent), "an empty accumulator has no display image");
		node.AccumulateImage({ vec4(8, 0.0625f, 0.25f, 1), vec4(0) }, uvec2(2, 1), 2);
		node.AccumulateImage({ vec4(0, 0.3125f, 0.25f, 0), vec4(0, 4, 0, 0.5f) }, uvec2(2, 1), 6);
		Require(node.m_accumulatedSamples == 8, "sample counts, not frame counts, must weight the mean");
		RequireColor(node.m_accumulatedImage[0], vec4(2, 0.25f, 0.25f, 0.25f));
		RequireColor(node.m_accumulatedImage[1], vec4(0, 3, 0, 0.375f));
		Require(node.GetLastRenderedImage(display, extent) && extent == uvec2(2, 1) && display.Num() == 2,
			"display export must preserve dimensions");
		Require(display[0] == u8vec4(255, 136, 136, 64) && display[1] == u8vec4(0, 255, 0, 96),
			"only display RGB is sRGB-encoded and clamped; alpha remains linear coverage");
		RequireColor(node.m_accumulatedImage[0], vec4(2, 0.25f, 0.25f, 0.25f));
		Require(node.GetLastRenderedImage(display, extent) && display[0] == u8vec4(255, 136, 136, 64),
			"repeated display export must not apply another transfer function");

		node.AccumulateImage({ vec4(4, 2, 3, 1), vec4(0) }, uvec2(1, 2), 3);
		Require(node.m_accumulatedSamples == 3, "changing shape must restart accumulation even with the same pixel count");
		RequireColor(node.m_accumulatedImage[0], vec4(4, 2, 3, 1));
		Require(node.GetLastRenderedImage(display, extent) && extent == uvec2(1, 2), "display extent must follow the new image");
		node.Clear();
		Require(node.m_accumulatedSamples == 0 && node.m_accumulatedImage.IsEmpty() && !node.GetLastRenderedImage(display, extent),
			"Clear must discard accumulated radiance and display availability");
		node.AccumulateImage({ vec4(0, 0, 0, 1) }, uvec2(1), 1);
		node.AccumulateImage({ vec4(1) }, uvec2(1), 1);
		RequireColor(node.m_accumulatedImage[0], vec4(0.5f, 0.5f, 0.5f, 1));
		Require(node.GetLastRenderedImage(display, extent) && display[0] == u8vec4(187, 187, 187, 255),
			"black and white must average to linear 0.5, then encode once to sRGB 187, not average display bytes");
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
}

int main()
{
	try
	{
		TestLinearAccumulationAndDisplayExport();
		TestPreparedLinearImage();
		std::cout << "Path tracer linear HDR image tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
