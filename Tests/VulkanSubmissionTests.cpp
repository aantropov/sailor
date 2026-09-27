#include "Sailor.h"
#include "Engine/Frame.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanQueue.h"
#include "GraphicsDriver/Vulkan/VulkanFence.h"
#include "GraphicsDriver/Vulkan/VulkanSemaphore.h"
#include "GraphicsDriver/Vulkan/VulkanSwapchain.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"
#include "Tasks/Tasks.h"

#include <iostream>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanSubmissionTestAccess
	{
	public:
		static PFN_vkQueueSubmit ExchangeSubmit(VulkanQueue& queue, PFN_vkQueueSubmit submit,
			PFN_vkQueueSubmit* forward = nullptr)
		{
			queue.m_lock.Lock();
			auto previous = std::exchange(queue.m_queueSubmit, submit);
			if (forward) *forward = previous;
			queue.m_lock.Unlock();
			return previous;
		}

		static size_t Flight(const VulkanDevice& device) { return device.m_currentFrame; }
		static VulkanFencePtr FlightFence(const VulkanDevice& device) { return device.m_syncFences[device.m_currentFrame]; }
		static VulkanSemaphorePtr AcquireSemaphore(const VulkanDevice& device) { return device.m_imageAvailableSemaphores[device.m_currentFrame]; }
		static VulkanSemaphorePtr PresentSemaphore(const VulkanDevice& device) { return device.m_renderFinishedSemaphores[device.m_currentSwapchainImageIndex]; }
		static uint32_t ImageIndex(const VulkanDevice& device) { return device.m_currentSwapchainImageIndex; }
		static VulkanQueuePtr UploadQueue(VulkanDevice& device)
		{
			const auto graphics = device.m_queueFamilies.m_graphicsFamily;
			if (graphics == device.m_queueFamilies.m_computeFamily) return device.m_computeQueue;
			if (graphics == device.m_queueFamilies.m_transferFamily) return device.m_transferQueue;
			return device.m_graphicsQueue;
		}
		static void CheckSyncCounts(const VulkanDevice& device)
		{
			const size_t images = device.m_swapchain->GetImageViews().Num();
			if (device.m_renderFinishedSemaphores.Num() != images || device.m_syncImages.Num() != images ||
				device.m_swapchainImagesInitialized.Num() != images || device.m_syncFences.Num() != VulkanApi::MaxFramesInFlight ||
				device.m_imageAvailableSemaphores.Num() != VulkanApi::MaxFramesInFlight || device.m_frameDeps.Num() != VulkanApi::MaxFramesInFlight)
				throw std::runtime_error("synchronization storage must follow image/flight ownership");
		}
	};
}

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	thread_local VkResult nextResult = VK_SUCCESS;
	thread_local uint32_t submitCalls = 0u;
	thread_local uint32_t lastSubmitCount = 0u;
	thread_local uint32_t lastCommandCount = 0u;
	thread_local bool rejectNativeSubmit = false;
	thread_local bool submitBeforeFailure = false;
	thread_local VkSemaphore lastWait = VK_NULL_HANDLE;
	thread_local VkSemaphore lastSignal = VK_NULL_HANDLE;
	PFN_vkQueueSubmit forwardNativeSubmit = nullptr;

	VKAPI_ATTR VkResult VKAPI_CALL StubSubmit(VkQueue, uint32_t count, const VkSubmitInfo*, VkFence)
	{
		++submitCalls;
		lastSubmitCount = count;
		return nextResult;
	}

	VKAPI_ATTR VkResult VKAPI_CALL NativeSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
	{
		if (rejectNativeSubmit)
		{
			StubSubmit(queue, count, info, fence);
			lastCommandCount = count ? info[0].commandBufferCount : 0u;
			lastWait = count && info[0].waitSemaphoreCount ? info[0].pWaitSemaphores[info[0].waitSemaphoreCount - 1u] : VK_NULL_HANDLE;
			lastSignal = count && info[0].signalSemaphoreCount ? info[0].pSignalSemaphores[0] : VK_NULL_HANDLE;
			if (nextResult != VK_SUCCESS && !submitBeforeFailure) return nextResult;
		}
		const VkResult actual = forwardNativeSubmit(queue, count, info, fence);
		return rejectNativeSubmit && actual == VK_SUCCESS ? nextResult : actual;
	}

	void TestQueueDispatch()
	{
		auto native = VulkanQueuePtr::Make(VK_NULL_HANDLE, 5u, 2u);
		Require(native->QueueFamilyIndex() == 5u && native->QueueIndex() == 2u,
			"ordinary queue construction must preserve family and index without exposing loader symbols to clients");
		auto queue = VulkanQueuePtr::Make(VK_NULL_HANDLE, 3u, 1u, StubSubmit);
		VkSubmitInfo info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		for (VkResult result : { VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST })
		{
			nextResult = result;
			const auto before = submitCalls;
			Require(queue->Submit(info) == result && submitCalls == before + 1u && lastSubmitCount == 1u,
				"single submission must forward the actual queue result");
			Require(queue->Submit(TVector<VkSubmitInfo>{ info, info }) == result &&
				submitCalls == before + 2u && lastSubmitCount == 2u,
				"batch submission must forward its complete count and queue result");
		}
	}

	class SubmitOverride
	{
	public:
		SubmitOverride(VulkanQueuePtr queue, VkResult result, bool accepted = false) : m_queue(std::move(queue))
		{
			nextResult = result;
			submitBeforeFailure = accepted;
			rejectNativeSubmit = true;
			m_previous = VulkanSubmissionTestAccess::ExchangeSubmit(*m_queue, NativeSubmit, &forwardNativeSubmit);
		}

		~SubmitOverride()
		{
			VulkanSubmissionTestAccess::ExchangeSubmit(*m_queue, m_previous);
			rejectNativeSubmit = false;
		}

	private:
		VulkanQueuePtr m_queue;
		PFN_vkQueueSubmit m_previous;
	};

	struct RecordedFrame
	{
		RHICommandListPtr command;
		RHIBufferPtr readback;
		std::array<uint32_t, 64> expected;
	};

	RecordedFrame RecordFrame(uint32_t seed)
	{
		RecordedFrame frame;
		for (uint32_t i = 0; i < frame.expected.size(); ++i) frame.expected[i] = seed + 37u * i;
		frame.readback = Renderer::GetDriver()->CreateBuffer(sizeof(frame.expected), EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		std::fill_n(static_cast<uint32_t*>(frame.readback->GetPointer()), frame.expected.size(), 0xdeadbeefu);
		frame.command = Renderer::GetDriver()->CreateCommandList(false, ECommandListQueue::Graphics);
		auto commands = Renderer::GetDriverCommands();
		commands->BeginCommandList(frame.command, true);
		frame.command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		commands->UpdateBuffer(frame.command, frame.readback, frame.expected.data(), sizeof(frame.expected));
		frame.command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		commands->EndCommandList(frame.command);
		return frame;
	}

	void CheckReadback(RecordedFrame& frame, bool submitted)
	{
		const auto* actual = static_cast<const uint32_t*>(frame.readback->GetPointer());
		for (size_t i = 0; i < frame.expected.size(); ++i)
			Require(actual[i] == (submitted ? frame.expected[i] : 0xdeadbeefu),
				"GPU frame readback does not match accepted/rejected submission");
	}

	struct FailedFrame
	{
		RecordedFrame frame;
		VulkanFencePtr fence;
		VulkanSemaphorePtr acquire;
		VulkanSemaphorePtr finished;
	};

	void TestNativeFrameFailure(bool present, VkResult error, FailedFrame& failed)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		Require(device->WaitIdle() == VK_SUCCESS, "native setup must drain successfully");
		VulkanSubmissionTestAccess::CheckSyncCounts(*device);
		if (present)
		{
			uint32_t flight;
			bool hasImage;
			Require(device->BeginRenderSubmission(flight, hasImage) && hasImage,
				"present test must acquire a real swapchain image");
		}
		const auto flight = VulkanSubmissionTestAccess::Flight(*device);
		failed.fence = VulkanSubmissionTestAccess::FlightFence(*device);
		failed.acquire = VulkanSubmissionTestAccess::AcquireSemaphore(*device);
		failed.finished = VulkanSubmissionTestAccess::PresentSemaphore(*device);
		failed.frame = RecordFrame(713u);
		auto& command = failed.frame.command;
		const auto callsBefore = submitCalls;
		bool submitted;
		{
			SubmitOverride override(device->GetGraphicsQueue(), error);
			submitted = present ? device->PresentFrame(Sailor::FrameState{}, { command->m_vulkan.m_commandBuffer }, {}) :
				device->SubmitFrameWithoutPresent({ command->m_vulkan.m_commandBuffer }, {});
		}
		Require(!submitted && submitCalls == callsBefore + 1u, "the actual native queue call must report the injected failure");
		Require(failed.fence->Status() == VK_NOT_READY, "a rejected submission must not signal its real native fence");
		// Stop the predecessor before it can cycle back to an unsignalable fence.
		Require(VulkanSubmissionTestAccess::Flight(*device) == flight,
			"failed submission advanced the flight slot");
		Require(!device->WasLastFrameSubmitSuccessful(), "failed frame must not report successful submission");
		uint32_t nextFlight;
		bool hasNextImage;
		Require(!device->BeginRenderSubmission(nextFlight, hasNextImage),
			"failed flight must be rejected before another native fence wait or acquire");
		Require(!device->PresentFrame(Sailor::FrameState{}, {}, {}) && !device->SubmitFrameWithoutPresent({}, {}),
			"both frame paths must reject further work until recovery");
		Require(command->m_vulkan.m_commandBuffer.NumRefs() == 2u,
			"the failed flight must retain the recorded command until recovery drains it");
		CheckReadback(failed.frame, false);
	}

	void TestRecoveredFrames(bool present, FailedFrame& failed)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		VulkanSubmissionTestAccess::CheckSyncCounts(*device);
		Require(failed.fence->Status() == VK_NOT_READY && VulkanSubmissionTestAccess::FlightFence(*device) != failed.fence,
			"recovery must replace the failed fence, not signal it");
		Require(VulkanSubmissionTestAccess::AcquireSemaphore(*device) != failed.acquire &&
			VulkanSubmissionTestAccess::PresentSemaphore(*device) != failed.finished,
			"recovery must replace possibly signaled semaphores");
		Require(failed.frame.command->m_vulkan.m_commandBuffer.NumRefs() == 1u,
			"drained failed commands must no longer be retained by the device");
		TVector<VkSemaphore> imageSignals;
		imageSignals.Resize(device->GetSwapchain()->GetImageViews().Num());
		for (size_t i = 0; i < 3u * VulkanApi::MaxFramesInFlight; ++i)
		{
			uint32_t flight = static_cast<uint32_t>(VulkanSubmissionTestAccess::Flight(*device));
			bool hasImage = false;
			if (present)
				Require(device->BeginRenderSubmission(flight, hasImage) && hasImage, "recovered frame must acquire an image");
			auto fence = VulkanSubmissionTestAccess::FlightFence(*device);
			auto frame = RecordFrame(19u + static_cast<uint32_t>(i));
			VkSemaphore acquire = *VulkanSubmissionTestAccess::AcquireSemaphore(*device);
			VkSemaphore finished = *VulkanSubmissionTestAccess::PresentSemaphore(*device);
			const uint32_t imageIndex = VulkanSubmissionTestAccess::ImageIndex(*device);
			bool submitted;
			{
				SubmitOverride observe(device->GetGraphicsQueue(), VK_SUCCESS);
				submitted = present ? device->PresentFrame(Sailor::FrameState{}, { frame.command->m_vulkan.m_commandBuffer }, {}) :
					device->SubmitFrameWithoutPresent({ frame.command->m_vulkan.m_commandBuffer }, {});
			}
			Require(submitted && device->WasLastFrameSubmitSuccessful(), "recovered frame must submit real GPU work");
			Require(VulkanSubmissionTestAccess::Flight(*device) == (flight + 1u) % VulkanApi::MaxFramesInFlight,
				"successful frames must advance exactly one flight");
			Require(lastWait == (present ? acquire : VK_NULL_HANDLE) && lastSignal == (present ? finished : VK_NULL_HANDLE),
				"native submission must use the flight acquire and image presentation semaphore");
			if (present)
			{
				Require(!imageSignals[imageIndex] || imageSignals[imageIndex] == finished,
					"a reacquired image must retain its own presentation semaphore");
				imageSignals[imageIndex] = finished;
			}
			Require(fence->Wait(5000000000ull) == VK_SUCCESS, "recovered frame fence did not complete within five seconds");
			CheckReadback(frame, true);
		}
		Require(device->WaitIdle() == VK_SUCCESS, "recovered frames must drain");
	}

	void TestNativeUploadFailure(VkResult error, bool accepted)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		auto frame = RecordFrame(177u);
		auto resource = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto fence = RHIFencePtr::Make();
		driver->TrackDelayedInitialization(resource.GetRawPtr(), fence);
		const auto callsBefore = submitCalls;
		bool submitted;
		{
			SubmitOverride override(VulkanSubmissionTestAccess::UploadQueue(*device), error, accepted);
			submitted = driver->SubmitCommandList(frame.command, fence);
		}
		Require(!submitted && submitCalls == callsBefore + 1u, "upload must reach the injected native submit");
		Require(fence->HasFailed() && !fence->IsFinished() && resource->HasInitializationFailed() && !resource->IsReady(),
			"a failed native upload must not publish successful initialization");
		Require(fence->m_vulkan.m_fence->Status() == (accepted ? VK_SUCCESS : VK_NOT_READY),
			"potentially submitted work must drain before failed-upload dependencies are released");
		Require(resource.NumRefs() == 1u && fence.NumRefs() == 1u && frame.command.NumRefs() == 1u,
			"a native refusal must release the upload's initialization owners");
		CheckReadback(frame, accepted);
		if (error != VK_ERROR_DEVICE_LOST)
		{
			auto replacement = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
			auto retry = RHIFencePtr::Make();
			driver->TrackDelayedInitialization(replacement.GetRawPtr(), retry);
			Require(driver->SubmitCommandList(frame.command, retry), "a rejected upload must be retryable with a fresh fence");
			retry->Wait(5000000000ull);
			Require(retry->IsFinished(), "retried upload must complete on the GPU");
			driver->TrackResources_ThreadSafe();
			Require(replacement->IsReady() && !resource->IsReady(), "retry must not resurrect the failed resource");
			CheckReadback(frame, true);
		}
	}

	template<typename Function>
	void OnRender(Function function)
	{
		auto task = Tasks::CreateTaskWithResult<std::string>("Native frame failure validation", [function = std::move(function)]()
			{
				try { function(); return std::string{}; }
				catch (const std::exception& error) { return std::string(error.what()); }
			}, EThreadType::Render);
		task->Run();
		task->Wait();
		const auto error = task->GetResult();
		if (!error.empty()) throw std::runtime_error(error);
	}

	int RunGpu(int argc, const char** argv, bool present, bool lost, bool upload = false, bool accepted = false)
	{
		App::Initialize(argv, argc);
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "GPU test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				if (upload)
				{
					OnRender([&]() { TestNativeUploadFailure(lost ? VK_ERROR_DEVICE_LOST : error, accepted); });
					if (lost) break;
					continue;
				}
				FailedFrame failed;
				OnRender([&]() { TestNativeFrameFailure(present, lost ? VK_ERROR_DEVICE_LOST : error, failed); });
				if (present && !lost)
				{
					OnRender([&]()
						{
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							const auto before = submitCalls;
							SubmitOverride refusal(device->GetGraphicsQueue(), error);
							Require(!device->FixLostDevice(App::GetMainWindow().GetRawPtr()),
								"failed recovery discarded a pending acquired image");
							Require(submitCalls == before + 1u && lastWait == *failed.acquire &&
								lastSignal == VK_NULL_HANDLE && lastCommandCount == 0u,
								"recovery must consume exactly the outstanding acquisition without signaling a failed flight");
							Require(VulkanSubmissionTestAccess::FlightFence(*device) == failed.fence &&
								failed.frame.command->m_vulkan.m_commandBuffer.NumRefs() == 2u,
								"recovery refusal must retain the old flight and dependencies");
						});
				}
				const bool recovered = Renderer::GetDriver()->FixLostDevice(App::GetMainWindow().GetRawPtr());
				Require(recovered != lost, "only swapchain failure can be recovered without replacing VkDevice");
				if (lost)
				{
					OnRender([&]()
						{
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							Require(VulkanSubmissionTestAccess::FlightFence(*device) == failed.fence &&
								failed.fence->Status() == VK_NOT_READY, "device loss must not replace or signal frame sync objects");
						});
					break;
				}
				OnRender([&]() { TestRecoveredFrames(present, failed); });
				std::cout << "Recovered error " << error << " with verified GPU frame readbacks\n";
			}
			std::cout << "Native frame failure test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		App::Shutdown();
		return result;
	}
}

int main(int argc, const char** argv)
{
	for (int i = 1; i < argc; ++i)
	{
		if (std::string_view(argv[i]) == "--gpu-present") return RunGpu(argc, argv, true, false);
		if (std::string_view(argv[i]) == "--gpu-offscreen") return RunGpu(argc, argv, false, false);
		if (std::string_view(argv[i]) == "--gpu-lost-present") return RunGpu(argc, argv, true, true);
		if (std::string_view(argv[i]) == "--gpu-lost-offscreen") return RunGpu(argc, argv, false, true);
		if (std::string_view(argv[i]) == "--gpu-upload") return RunGpu(argc, argv, false, false, true);
		if (std::string_view(argv[i]) == "--gpu-upload-lost") return RunGpu(argc, argv, false, true, true);
		if (std::string_view(argv[i]) == "--gpu-upload-pending-loss") return RunGpu(argc, argv, false, true, true, true);
	}
	try
	{
		TestQueueDispatch();
		std::cout << "Vulkan submission tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
