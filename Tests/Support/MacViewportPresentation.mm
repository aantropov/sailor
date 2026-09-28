#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include "MacViewportPresentation.h"
#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

// Keep the actual presented drawable alive long enough to inspect the blit.
@interface ReadbackPresentationLayer : CAMetalLayer
{
@public
	id<CAMetalDrawable> m_lastDrawable;
	double m_drawableCpuUs;
}
@end

@implementation ReadbackPresentationLayer
- (id<CAMetalDrawable>)nextDrawable
{
	[m_lastDrawable release];
	m_lastDrawable = nil;
	const auto start = std::chrono::steady_clock::now();
	id<CAMetalDrawable> drawable = [super nextDrawable];
	m_drawableCpuUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
	m_lastDrawable = [drawable retain];
	return drawable;
}
- (void)dealloc
{
	[m_lastDrawable release];
	[super dealloc];
}
@end

namespace Sailor::Tests
{
	using namespace EditorRemote;

	void CheckMacReadbackPresentation(const MacRendererFrameSource& source)
	{
		auto require = [](bool value, const char* message)
			{
				if (!value) throw std::runtime_error(message);
			};
		@autoreleasepool
		{
			class Source final : public IMacRendererFrameSourceProvider
			{
			public:
				explicit Source(const MacRendererFrameSource& frame) : m_frame(frame) {}
				Failure AcquireFrameSource(const MacViewportSurfaceState&, FrameIndex, MacRendererFrameSource& out) override
				{
					out = m_frame;
					return Failure::Ok();
				}
				const MacRendererFrameSource& m_frame;
			} input(source);
			MacLoopbackIOSurfaceProvider provider(&input);
			MacLoopbackViewportPresenter presenter;
			ViewportDescriptor viewport;
			viewport.m_viewportId = 202;
			viewport.m_width = source.m_width;
			viewport.m_height = source.m_height;
			viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			viewport.m_colorSpace = ColorSpace::Srgb;
			viewport.m_presentMode = PresentMode::Mailbox;
			MacViewportLoopbackBinding binding(viewport, provider, presenter);
			ReadbackPresentationLayer* layer = [ReadbackPresentationLayer layer];
			presenter.BindHostHandle(viewport.m_viewportId,
				{ MacNativeHostHandleKind::CAMetalLayer, reinterpret_cast<uintptr_t>(layer) });
			require(binding.Create().IsOk(), "actual readback presentation must create a native loopback binding");
			auto allocation = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 1)->m_nativeAllocation;
			const auto native = presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr();
			id<MTLTexture> firstImport = nil;
			uint64_t firstCopy = 0;
			bool reused = true;
			double firstPumpUs = 0, warmPumpUs = 0, maxPumpUs = 0, drawableUs = 0, gpuUs = 0;
			constexpr uint32_t repeats = 8;
			for (uint32_t i = 0; i <= repeats; ++i)
			{
				const auto start = std::chrono::steady_clock::now();
				const auto result = binding.PumpFrame();
				const double elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
				require(result.IsOk(), "actual Vulkan pixels must pass through native upload and CAMetalLayer presentation");
				if (i == 0)
				{
					firstPumpUs = elapsed;
					firstCopy = allocation->m_currentCopyToken;
					firstImport = [[(id<MTLTexture>)native->m_lastSourceTextureObject retain] autorelease];
				}
				else
				{
					warmPumpUs += elapsed;
					maxPumpUs = std::max(maxPumpUs, elapsed);
					drawableUs += layer->m_drawableCpuUs;
					reused &= native->m_lastSourceTextureObject == reinterpret_cast<uintptr_t>(firstImport);
				}
				require(firstCopy != 0 && allocation->m_currentCopyToken == firstCopy &&
					allocation->m_lastRendererSource.m_readback == source.m_readback,
					"native presentation must retain the actual immutable record without another pixel upload");
				id<MTLCommandBuffer> command = (id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject;
				require(command != nil, "native presentation must submit an actual GPU command");
				[command waitUntilCompleted];
				require(command.status == MTLCommandBufferStatusCompleted, "native presentation command must complete");
				if (i != 0) gpuUs += (command.GPUEndTime - command.GPUStartTime) * 1000000.0;
				bool completed = false;
				require(PollMacIOSurfaceReadCompletion(*allocation, completed).IsOk() && completed,
					"completed presentation must retire its IOSurface reader");
			}
			std::cout << "Readback presentation " << source.m_width << 'x' << source.m_height <<
				": first PumpFrame " << firstPumpUs << " us, warm mean " << warmPumpUs / repeats <<
				" us, warm max " << maxPumpUs << " us, nextDrawable mean " << drawableUs / repeats <<
				" us, GPU mean " << gpuUs / repeats << " us\n";

			id<MTLDevice> device = (id<MTLDevice>)native->m_deviceObject;
			id<MTLBuffer> pixel = [[device newBufferWithLength:256 options:MTLResourceStorageModeShared] autorelease];
			id<MTLCommandBuffer> read = [(id<MTLCommandQueue>)native->m_commandQueueObject commandBuffer];
			id<MTLBlitCommandEncoder> blit = [read blitCommandEncoder];
			require(pixel && blit && layer->m_lastDrawable, "presented-pixel evidence needs real native readback resources");
			[blit copyFromTexture:layer->m_lastDrawable.texture sourceSlice:0 sourceLevel:0
				sourceOrigin:MTLOriginMake(source.m_width / 2u, source.m_height / 2u, 0) sourceSize:MTLSizeMake(1, 1, 1)
				toBuffer:pixel destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
			[blit endEncoding];
			[read commit];
			[read waitUntilCompleted];
			const auto* expected = source.GetCpuBytes() + (source.m_height / 2u) * source.m_bytesPerRow + (source.m_width / 2u) * 4u;
			require(read.status == MTLCommandBufferStatusCompleted && std::memcmp(pixel.contents, expected, 4) == 0,
				"the actual presented drawable must contain the Vulkan readback's channels and alpha");
			require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == repeats + 1u && native->m_presentToken == repeats + 1u,
				"texture reuse must preserve normal export and native presentation");
			require(reused, "repeated presentation of an actual Vulkan readback must reuse its IOSurface texture import");
			binding.Destroy();
			require(provider.GetLiveAllocationCount() == 0, "readback presentation fixture must release its native registration");
		}
	}
}
