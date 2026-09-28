#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include "MacViewportPresentation.h"
#include "Sailor.h"
#include "Editor/EditorRuntimeBridge.h"
#include "EditorEngineProtocolInternal.h"
#include "EditorEngineProtocolLifecycle.h"
#include "Protocol/Generated/editor_engine.pb.h"
#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

extern "C" SAILOR_SHARED_API void SailorProtocolFreeBuffer(uint8_t* buffer) noexcept;

// Observe the queue created by the real App binding without replacing its work.
@interface ViewportQueueDevice : NSProxy
{
@public
	id<MTLDevice> m_device;
	id<MTLCommandQueue> m_queue;
	BOOL m_failNextQueue;
}
- (id)initWithDevice:(id<MTLDevice>)device;
@end

@implementation ViewportQueueDevice
- (id)initWithDevice:(id<MTLDevice>)device
{
	m_device = [device retain];
	return self;
}
- (id<MTLCommandQueue>)newCommandQueue
{
	if (std::exchange(m_failNextQueue, NO)) return nil;
	id<MTLCommandQueue> queue = [m_device newCommandQueue];
	[m_queue release];
	m_queue = [queue retain];
	return queue;
}
- (NSMethodSignature*)methodSignatureForSelector:(SEL)selector
{
	return [(NSObject*)m_device methodSignatureForSelector:selector];
}
- (void)forwardInvocation:(NSInvocation*)invocation { [invocation invokeWithTarget:m_device]; }
- (void)dealloc
{
	[m_queue release];
	[m_device release];
	[super dealloc];
}
@end

// Keep the actual presented drawable alive long enough to inspect the blit.
@interface ReadbackPresentationLayer : CAMetalLayer
{
@public
	id<CAMetalDrawable> m_lastDrawable;
	double m_drawableCpuUs;
	ViewportQueueDevice* m_queueDevice;
}
@end

@implementation ReadbackPresentationLayer
- (id<MTLDevice>)device { return m_queueDevice ? (id<MTLDevice>)m_queueDevice : [super device]; }
- (void)setDevice:(id<MTLDevice>)device
{
	[super setDevice:m_queueDevice && device == (id<MTLDevice>)m_queueDevice ? m_queueDevice->m_device : device];
}
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
	[m_queueDevice release];
	[super dealloc];
}
@end

namespace Sailor::Tests
{
	using namespace EditorRemote;

	void CheckMacVulkanTexturePresentation(uintptr_t texture, uint32_t width, uint32_t height, uint32_t expectedPixel)
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
				MacRendererFrameSource m_frame;
				Failure AcquireFrameSource(const MacViewportSurfaceState&, FrameIndex, MacRendererFrameSource& out) override
				{
					out = m_frame;
					return Failure::Ok();
				}
			} input;
			input.m_frame.m_kind = MacRendererFrameSourceKind::RendererOwnedMetalTexture;
			input.m_frame.m_textureObject = texture;
			input.m_frame.m_width = width;
			input.m_frame.m_height = height;
			input.m_frame.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			MacLoopbackIOSurfaceProvider provider(&input);
			MacLoopbackViewportPresenter presenter;
			ViewportDescriptor viewport;
			viewport.m_viewportId = 204;
			viewport.m_width = width;
			viewport.m_height = height;
			viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			viewport.m_colorSpace = ColorSpace::Srgb;
			viewport.m_presentMode = PresentMode::Mailbox;
			MacViewportLoopbackBinding binding(viewport, provider, presenter);
			ReadbackPresentationLayer* layer = [ReadbackPresentationLayer layer];
			presenter.BindHostHandle(viewport.m_viewportId,
				{ MacNativeHostHandleKind::CAMetalLayer, reinterpret_cast<uintptr_t>(layer) });
			require(binding.Create().IsOk(), "native Vulkan export needs an actual viewport binding");
			auto allocation = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 1)->m_nativeAllocation;
			const auto queue = allocation->m_producerCommandQueueObject;
			for (uint32_t i = 0; i < 8; ++i)
			{
				require(binding.PumpFrame().IsOk(), "an exported Vulkan texture must start an asynchronous Metal copy");
				[(id<MTLCommandBuffer>)allocation->m_copyCommandBufferObject waitUntilCompleted];
				if (binding.GetRuntimeSession().GetLastPublishedFrameIndex() == i)
					require(binding.PumpFrame().IsOk(), "completed Vulkan texture copy must become presentable");
				id<MTLCommandBuffer> presented = (id<MTLCommandBuffer>)allocation->m_presentCommandBufferObject;
				[presented waitUntilCompleted];
				require(presented && presented.status == MTLCommandBufferStatusCompleted &&
					binding.GetRuntimeSession().GetLastPublishedFrameIndex() == i + 1u,
					"native Vulkan texture must complete a real presentation without a CPU readback payload");
				require(allocation->m_cpuUploadedBytes == 0 && allocation->m_producerCommandQueueObject == queue,
					"native texture transfer must reuse its queue without CPU uploads");
			}
			const auto native = presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr();
			id<MTLBuffer> pixel = [[(id<MTLDevice>)native->m_deviceObject newBufferWithLength:256 options:MTLResourceStorageModeShared] autorelease];
			id<MTLCommandBuffer> read = [(id<MTLCommandQueue>)native->m_commandQueueObject commandBuffer];
			id<MTLBlitCommandEncoder> blit = [read blitCommandEncoder];
			require(pixel && blit && layer->m_lastDrawable, "native Vulkan presentation needs actual drawable evidence");
			[blit copyFromTexture:layer->m_lastDrawable.texture sourceSlice:0 sourceLevel:0
				sourceOrigin:MTLOriginMake(width / 2u, height / 2u, 0) sourceSize:MTLSizeMake(1, 1, 1)
				toBuffer:pixel destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
			[blit endEncoding];
			[read commit];
			[read waitUntilCompleted];
			require(read.status == MTLCommandBufferStatusCompleted && std::memcmp(pixel.contents, &expectedPixel, 4) == 0,
				"presented drawable must match the actual exported Vulkan image");
			require(binding.Destroy().IsOk(), "native Vulkan export viewport must release after copy and presentation completion");
		}
	}

	void CheckMacVulkanTextureRetirement(const MacRendererFrameSource& first, uint32_t firstPixel,
		const MacRendererFrameSource& resized, uint32_t resizedPixel, const std::function<void()>& churn)
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
				MacRendererFrameSource m_frame;
				id<MTLSharedEvent> m_gate = nil;
				uint64_t m_waitValue = 1;
				uint32_t m_calls = 0;
				Failure AcquireFrameSource(const MacViewportSurfaceState&, FrameIndex, MacRendererFrameSource& out) override
				{
					++m_calls;
					out = m_frame;
					out.m_crossApiSharedEventObject = m_waitValue ? reinterpret_cast<uintptr_t>([m_gate retain]) : 0;
					out.m_crossApiAcquireValue = m_waitValue;
					out.m_crossApiSyncKind = m_waitValue ? CrossApiSyncKind::MetalSharedEvent : CrossApiSyncKind::None;
					return Failure::Ok();
				}
			} input;
			input.m_frame = first;
			MacLoopbackIOSurfaceProvider provider(&input);
			MacLoopbackViewportPresenter presenter;
			ViewportDescriptor viewport;
			viewport.m_viewportId = 205;
			viewport.m_width = first.m_width;
			viewport.m_height = first.m_height;
			viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
			viewport.m_colorSpace = ColorSpace::Srgb;
			viewport.m_presentMode = PresentMode::Mailbox;
			MacViewportLoopbackBinding binding(viewport, provider, presenter);
			ReadbackPresentationLayer* layer = [ReadbackPresentationLayer layer];
			layer->m_queueDevice = [[ViewportQueueDevice alloc] initWithDevice:[MTLCreateSystemDefaultDevice() autorelease]];
			presenter.BindHostHandle(viewport.m_viewportId,
				{ MacNativeHostHandleKind::CAMetalLayer, reinterpret_cast<uintptr_t>(layer) });
			require(binding.Create().IsOk(), "Vulkan-source retirement needs an actual native viewport");
			auto allocation = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 1)->m_nativeAllocation;
			input.m_gate = [[(id<MTLDevice>)allocation->m_producerDeviceObject newSharedEvent] autorelease];
			require(input.m_gate != nil, "Vulkan-source retirement needs a real GPU gate");
			NSMutableArray<id<MTLCommandBuffer>>* copies = [NSMutableArray array];
			std::promise<void> finished;
			auto finish = finished.get_future();
			std::thread watchdog([&]()
				{
					if (finish.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
						input.m_gate.signaledValue = UINT64_MAX;
				});
			try
			{
				require(binding.PumpFrame().IsOk(), "Vulkan-source copy must submit without waiting for its gate");
				id<MTLCommandBuffer> oldCopy = (id<MTLCommandBuffer>)allocation->m_copyCommandBufferObject;
				[copies addObject:oldCopy];
				id<MTLTexture> oldPixels = [[(id<MTLTexture>)allocation->m_producerTextureObject retain] autorelease];
				for (uint32_t i = 0; i < 100; ++i)
					require(binding.PumpFrame().IsOk(), "pending Vulkan-source copies must remain nonblocking");
				require(input.m_gate.signaledValue == 0 && input.m_calls == 1 &&
					binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 0,
					"pending native copy must not reacquire or publish the Vulkan source");
				layer->m_queueDevice->m_failNextQueue = YES;
				require(!binding.Resize(resized.m_width, resized.m_height).IsOk() &&
					binding.GetRuntimeSession().GetGeneration() == 1 && presenter.FindImportedState(viewport.m_viewportId)->m_generation == 1 &&
					provider.GetLiveAllocationCount() == 1 && allocation.IsShared() && input.m_gate.signaledValue == 0,
					"native import failure must preserve the old generation and its pending Vulkan-source copy");
				require(binding.Resize(resized.m_width, resized.m_height).IsOk() && !allocation.IsShared(),
					"resize must retire the old allocation while its copy remains pending");
				allocation.Clear();
				churn();
				require(input.m_gate.signaledValue == 0 && oldCopy.status != MTLCommandBufferStatusCompleted,
					"renderer allocation churn must proceed independently of the retired Metal copy");
				input.m_frame = resized;
				input.m_waitValue = 0;
				id<MTLSharedEvent> gate = input.m_gate;
				require(binding.PumpFrame().IsOk(), "new extent must submit its own completed Vulkan source");
				auto replacement = std::as_const(binding.GetTransportBackend()).FindSurface(viewport.m_viewportId, 1, 2)->m_nativeAllocation;
				id<MTLCommandBuffer> newCopy = (id<MTLCommandBuffer>)replacement->m_copyCommandBufferObject;
				if (newCopy) [copies addObject:newCopy];
				[newCopy waitUntilCompleted];
				if (binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 0)
					require(binding.PumpFrame().IsOk(), "new extent must publish after its own copy finishes");
				require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1 && input.m_calls == 2 &&
					gate.signaledValue == 0 && oldCopy.status != MTLCommandBufferStatusCompleted && replacement->m_cpuUploadedBytes == 0,
					"resized presentation must not wait for, upload or publish the retired source");
				[(id<MTLCommandBuffer>)replacement->m_presentCommandBufferObject waitUntilCompleted];
				uint32_t pixel = 0;
				[(id<MTLTexture>)replacement->m_producerTextureObject getBytes:&pixel bytesPerRow:4
					fromRegion:MTLRegionMake2D(11, 7, 1, 1) mipmapLevel:0];
				require(pixel == resizedPixel, "new viewport must contain the new Vulkan image's pixels");
				gate.signaledValue = 1;
				[oldCopy waitUntilCompleted];
				[oldPixels getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(11, 7, 1, 1) mipmapLevel:0];
				require(oldCopy.status == MTLCommandBufferStatusCompleted && pixel == firstPixel,
					"retired copy must retain the original Vulkan pixels across allocation churn and resize");
				input.m_waitValue = 2;
				require(binding.PumpFrame().IsOk(), "viewport destruction test needs another pending Vulkan-source copy");
				id<MTLCommandBuffer> finalCopy = (id<MTLCommandBuffer>)replacement->m_copyCommandBufferObject;
				[copies addObject:finalCopy];
				id<MTLTexture> finalPixels = [[(id<MTLTexture>)replacement->m_producerTextureObject retain] autorelease];
				require(binding.Destroy().IsOk() && !replacement.IsShared() && provider.GetLiveAllocationCount() == 0 &&
					presenter.FindImportedState(viewport.m_viewportId) == nullptr,
					"viewport shutdown must unregister and unimport its pending allocation");
				replacement.Clear();
				churn();
				require(gate.signaledValue == 1 && finalCopy.status != MTLCommandBufferStatusCompleted,
					"viewport destruction and renderer churn must not synchronously drain the native copy");
				gate.signaledValue = 2;
				[finalCopy waitUntilCompleted];
				[finalPixels getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(11, 7, 1, 1) mipmapLevel:0];
				require(finalCopy.status == MTLCommandBufferStatusCompleted && pixel == resizedPixel,
					"copy retired by viewport shutdown must preserve the caller-owned Vulkan image");
			}
			catch (...)
			{
				input.m_gate.signaledValue = UINT64_MAX;
				for (id<MTLCommandBuffer> copy in copies) [copy waitUntilCompleted];
				finished.set_value();
				watchdog.join();
				throw;
			}
			finished.set_value();
			watchdog.join();
		}
	}

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
			layer->m_queueDevice = [[ViewportQueueDevice alloc] initWithDevice:[MTLCreateSystemDefaultDevice() autorelease]];
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

			auto checkPresentedPixel = [&](const auto* nativeBinding)
			{
				id<MTLDevice> device = (id<MTLDevice>)nativeBinding->m_deviceObject;
				id<MTLBuffer> pixel = [[device newBufferWithLength:256 options:MTLResourceStorageModeShared] autorelease];
				id<MTLCommandBuffer> read = [(id<MTLCommandQueue>)nativeBinding->m_commandQueueObject commandBuffer];
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
			};
			checkPresentedPixel(native);
			require(binding.GetRuntimeSession().GetLastPublishedFrameIndex() == repeats + 1u && native->m_presentToken == repeats + 1u,
				"texture reuse must preserve normal export and native presentation");
			require(reused, "repeated presentation of an actual Vulkan readback must reuse its IOSurface texture import");

			for (uint32_t attempt = 0; attempt < 3; ++attempt)
			{
				layer->m_queueDevice->m_failNextQueue = YES;
				require(!binding.Resize(source.m_width + 1, source.m_height + 1).IsOk() &&
					binding.GetRuntimeSession().GetDescriptor() == viewport && binding.GetRuntimeSession().GetGeneration() == 1 &&
					binding.GetRuntimeSession().IsReady() && presenter.FindImportedState(viewport.m_viewportId)->m_generation == 1 &&
					presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr() == native &&
					layer.drawableSize.width == source.m_width && layer.drawableSize.height == source.m_height &&
					provider.GetLiveAllocationCount() == 1 && binding.GetTransportBackend().GetSurfaceCount() == 1,
					"native queue creation failure must preserve the imported layer and release the rejected IOSurface");
				require(binding.PumpFrame().IsOk(), "old viewport must still present after a failed native resize");
				checkPresentedPixel(native);
			}
			allocation.Clear();
			require(binding.Resize(source.m_width, source.m_height).IsOk() && binding.PumpFrame().IsOk() &&
				binding.GetRuntimeSession().GetGeneration() == 2 && provider.GetLiveAllocationCount() == 1 &&
				presenter.FindImportedState(viewport.m_viewportId)->m_generation == 2,
				"native import retry must replace the generation without leaking its predecessor");
			checkPresentedPixel(presenter.FindImportedState(viewport.m_viewportId)->m_layerBinding.GetRawPtr());
			require(binding.GetRuntimeSession().MarkFailure(Failure::FromDomain(ErrorDomain::Session, 2, "recreate native viewport")).IsOk() &&
				binding.PumpFrame().IsOk(), "a real native viewport must recover through its ordinary pump");
			const auto* recovered = presenter.FindImportedState(viewport.m_viewportId);
			require(recovered && recovered->m_epoch == 2 && recovered->m_layerBinding &&
				binding.GetRuntimeSession().GetLastPublishedFrameIndex() == 1 && provider.GetLiveAllocationCount() == 1 &&
				binding.GetTransportBackend().GetSurfaceCount() == 1,
				"recovery must replace the native surface and present the first frame of the new epoch");
			checkPresentedPixel(recovered->m_layerBinding.GetRawPtr());
			binding.Destroy();
			require(provider.GetLiveAllocationCount() == 0, "readback presentation fixture must release its native registration");
		}
	}

	void CheckMacAppViewportPump(const MacRendererFrameSource& initial,
		const std::function<MacRendererFrameSource()>& captureNextFrame)
	{
		using sailor::editor::v1::ProtocolRequest;
		using sailor::editor::v1::ProtocolResponse;
		auto require = [](bool value, const char* message)
			{
				if (!value) throw std::runtime_error(message);
			};
		// This fixture initializes App directly, without the local protocol host.
		Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		require(gate.TryBeginInitialization(error), "App viewport protocol gate must initialize");
		gate.CompleteInitialization(true);
		Protocol::EditorEngineProtocolDependencies dependencies;
		dependencies.m_lifecycleGate = &gate;
		uint64_t requestId = 0;
		auto invoke = [&](ProtocolRequest& request)
		{
			request.set_protocol_version(1);
			request.set_request_id(++requestId);
			const auto bytes = request.SerializeAsString();
			uint8_t* data = nullptr;
			uint32_t size = 0;
			const auto status = Protocol::InvokeEditorEngineProtocol(reinterpret_cast<const uint8_t*>(bytes.data()),
				static_cast<uint32_t>(bytes.size()), &data, &size, dependencies);
			ProtocolResponse response;
			const bool decoded = response.ParseFromArray(data, static_cast<int>(size));
			SailorProtocolFreeBuffer(data);
			require(status == static_cast<int32_t>(Protocol::EEditorEngineTransportStatus::Ok) && decoded &&
				response.protocol_version() == 1 && response.request_id() == requestId && response.success(),
				"viewport protobuf request must reach the native handler and return its correlated response");
			require(request.has_get_remote_viewport_state() ? response.has_uint32_result() : response.has_bool_result(),
				"viewport response must contain the result type required by its command");
			return response;
		};
		@autoreleasepool
		{
			constexpr uint64_t viewportId = 203;
			ReadbackPresentationLayer* layer = [ReadbackPresentationLayer layer];
			layer->m_queueDevice = [[ViewportQueueDevice alloc] initWithDevice:[MTLCreateSystemDefaultDevice() autorelease]];
			try
			{
				ProtocolRequest hostRequest;
				auto* host = hostRequest.mutable_set_remote_viewport_mac_host_handle();
				host->set_viewport_id(viewportId);
				host->set_host_handle_kind(static_cast<uint32_t>(MacNativeHostHandleKind::CAMetalLayer));
				host->set_host_handle_value(reinterpret_cast<uint64_t>(layer));
				require(invoke(hostRequest).bool_result().value(), "protobuf host binding must accept the actual Metal layer");
				ProtocolRequest updateRequest;
				auto* update = updateRequest.mutable_upsert_remote_viewport();
				update->set_viewport_id(viewportId);
				update->set_width(initial.m_width);
				update->set_height(initial.m_height);
				update->set_visible(true);
				require(invoke(updateRequest).bool_result().value(), "protobuf create must import the actual App viewport");
				require(layer->m_queueDevice->m_queue != nil, "actual App binding must create a native presentation queue");
				ProtocolRequest stateRequest;
				stateRequest.mutable_get_remote_viewport_state()->set_viewport_id(viewportId);
				auto state = invoke(stateRequest);
				require(state.has_uint32_result() && state.uint32_result().value() == static_cast<uint32_t>(SessionState::Active),
					"protobuf state must describe the created live binding");
				update->set_visible(false);
				require(invoke(updateRequest).bool_result().value() &&
					invoke(stateRequest).uint32_result().value() == static_cast<uint32_t>(SessionState::Paused),
					"protobuf visibility must pause that same live binding");
				EditorRuntime::PumpEditorRemoteViewportsOnEngineThread();
				require(layer->m_lastDrawable == nil, "hidden App viewport must not acquire or present a native drawable");
				update->set_visible(true);
				require(invoke(updateRequest).bool_result().value() &&
					invoke(stateRequest).uint32_result().value() == static_cast<uint32_t>(SessionState::Active),
					"protobuf visibility must resume the live binding before its frame pump");
				auto source = initial;
				double captureUs = 0, pumpUs = 0, drawableUs = 0, completeUs = 0, maxPumpUs = 0, repeatPumpUs = 0;
				constexpr uint32_t frames = 24;
				constexpr uint32_t repeats = 4;
				for (uint32_t i = 0; i < frames + repeats; ++i)
				{
					const auto begin = std::chrono::steady_clock::now();
					if (i != 0 && i < frames) source = captureNextFrame();
					const auto captured = std::chrono::steady_clock::now();
					EditorRuntime::PumpEditorRemoteViewportsOnEngineThread();
					const auto submitted = std::chrono::steady_clock::now();
					id<MTLCommandQueue> queue = layer->m_queueDevice->m_queue;
					id<MTLCommandBuffer> completion = [queue commandBuffer];
					[completion commit];
					[completion waitUntilCompleted];
					const auto completed = std::chrono::steady_clock::now();
					require(completion.status == MTLCommandBufferStatusCompleted && layer->m_lastDrawable,
						"App native presentation must submit and finish before inspecting its drawable");
					const auto cpu = std::chrono::duration<double, std::micro>(submitted - captured).count();
					if (i != 0 && i < frames)
					{
						captureUs += std::chrono::duration<double, std::micro>(captured - begin).count();
						pumpUs += cpu;
						maxPumpUs = std::max(maxPumpUs, cpu);
						drawableUs += layer->m_drawableCpuUs;
						completeUs += std::chrono::duration<double, std::micro>(completed - begin).count();
					}
					else if (i >= frames) repeatPumpUs += cpu;
					id<MTLBuffer> pixel = [[layer->m_queueDevice->m_device newBufferWithLength:256 options:MTLResourceStorageModeShared] autorelease];
					id<MTLCommandBuffer> read = [queue commandBuffer];
					id<MTLBlitCommandEncoder> blit = [read blitCommandEncoder];
					require(pixel && blit, "actual App drawable verification requires native readback resources");
					[blit copyFromTexture:layer->m_lastDrawable.texture sourceSlice:0 sourceLevel:0
						sourceOrigin:MTLOriginMake(source.m_width / 2u, source.m_height / 2u, 0) sourceSize:MTLSizeMake(1, 1, 1)
						toBuffer:pixel destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
					[blit endEncoding];
					[read commit];
					[read waitUntilCompleted];
					const auto* expected = source.GetCpuBytes() + (source.m_height / 2u) * source.m_bytesPerRow + (source.m_width / 2u) * 4u;
					require(read.status == MTLCommandBufferStatusCompleted && std::memcmp(pixel.contents, expected, 4) == 0,
						"actual App drawable must follow each new Vulkan capture, not the prior native image");
				}
				std::cout << "App viewport " << initial.m_width << 'x' << initial.m_height << ": " << frames - 1 <<
					" new frames, capture/publication mean " << captureUs / (frames - 1) <<
					" us, Main pump mean " << pumpUs / (frames - 1) << " us, max " << maxPumpUs <<
					" us, nextDrawable mean " << drawableUs / (frames - 1) <<
					" us, capture-to-queue-completion mean " << completeUs / (frames - 1) <<
					" us, repeated-frame Main pump mean " << repeatPumpUs / repeats << " us\n";
				char* text = nullptr;
				const auto length = App::GetEditorRemoteViewportDiagnostics(viewportId, &text);
				require(text != nullptr && length != 0, "actual App viewport must expose its upload accounting");
				const std::string diagnostics(text, length);
				delete[] text;
				constexpr std::string_view key = "cpuUploadedBytes=";
				const auto offset = diagnostics.find(key);
				require(offset != std::string::npos, "native upload bytes must be present in viewport diagnostics");
				const auto uploaded = std::stoull(diagnostics.substr(offset + key.size()));
				require(uploaded == static_cast<uint64_t>(frames) * initial.m_width * initial.m_height * 4u,
					"each new App frame must upload once; repeated presentation must not upload the same pixels again");
				std::cout << "App native upload bytes " << initial.m_width << 'x' << initial.m_height << ": " << uploaded << '\n';
				ProtocolRequest destroyRequest;
				destroyRequest.mutable_destroy_remote_viewport()->set_viewport_id(viewportId);
				require(invoke(destroyRequest).bool_result().value(), "protobuf destroy must release the actual App viewport after GPU completion");
				require(invoke(stateRequest).uint32_result().value() == static_cast<uint32_t>(SessionState::Created) &&
					!invoke(destroyRequest).bool_result().value(), "native binding must be absent after protobuf destroy");
				host->set_host_handle_kind(0);
				host->set_host_handle_value(0);
				require(invoke(hostRequest).bool_result().value(), "protobuf cleanup must clear the native host reference");
			}
			catch (...)
			{
				App::DestroyEditorRemoteViewport(viewportId);
				App::SetEditorRemoteViewportMacHostHandle(viewportId, 0, 0);
				throw;
			}
		}
	}
}
