#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Texture.h"

#include <iostream>
#include <stdexcept>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	RHITexturePtr MakeResource()
	{
		return RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
	}

	void TestRejectedSubmissionReleasesInitializationOwners()
	{
		VulkanGraphicsDriver driver;
		auto resource = MakeResource();
		auto fence = RHIFencePtr::Make();
		driver.TrackDelayedInitialization(resource.GetRawPtr(), fence);
		Require(!resource->IsReady() && resource.NumRefs() == 2u && fence.NumRefs() == 2u,
			"pending initialization must retain both owners");

		const bool submitted = driver.SubmitCommandList({}, fence);
		const bool released = resource.NumRefs() == 1u && fence.NumRefs() == 1u;
		const bool ready = resource->IsReady();
		// Keep a failing predecessor run from leaking the cycle under test.
		resource->ClearDependencies();
		fence->ClearObservables();
		Require(!submitted, "an unavailable command/device must reject submission");
		Require(released, "rejected submission retained the resource/fence ownership cycle");
		Require(!ready, "failed initialization must not become ready after releasing its fence");
		Require(resource->HasInitializationFailed() && fence->HasFailed() && !fence->IsFinished(),
			"submission failure must be observable without pretending that the GPU signaled");
		Require(fence->Wait() == EFenceStatus::Failed && !fence->Reset(),
			"wait/reset must report terminal failure without a native fence");
		Require(fence->HasFailed() && !fence->IsFinished(), "a failed fence is terminal, including after reset");
		driver.TrackResources_ThreadSafe();
		Require(!resource->IsReady(), "garbage collection must not turn a rejected upload into success");
	}

	void TestFailureOnlyRemovesItsOwnDependencies()
	{
		VulkanGraphicsDriver driver;
		auto shared = MakeResource();
		auto firstOnly = MakeResource();
		auto secondOnly = MakeResource();
		auto first = RHIFencePtr::Make();
		auto second = RHIFencePtr::Make();
		driver.TrackDelayedInitialization(shared.GetRawPtr(), first);
		driver.TrackDelayedInitialization(firstOnly.GetRawPtr(), first);
		driver.TrackDelayedInitialization(shared.GetRawPtr(), second);
		driver.TrackDelayedInitialization(secondOnly.GetRawPtr(), second);
		Require(!driver.SubmitCommandList({}, first), "the first submission must be refused");
		Require(first.NumRefs() == 1u && firstOnly.NumRefs() == 1u &&
			shared.NumRefs() == 2u && second.NumRefs() == 3u && secondOnly.NumRefs() == 2u,
			"failure must release only the failed fence and its observers, not another pending upload");
		Require(shared->HasInitializationFailed() && firstOnly->HasInitializationFailed() &&
			!secondOnly->HasInitializationFailed() && !second->HasFailed(),
			"failure must reach every observer without poisoning an unrelated resource");
		Require(!shared->IsReady() && !firstOnly->IsReady() && !secondOnly->IsReady(),
			"failed and still-pending resources must remain unavailable");
		Require(!driver.SubmitCommandList({}, second), "the second submission must be refused");
		Require(shared.NumRefs() == 1u && firstOnly.NumRefs() == 1u && secondOnly.NumRefs() == 1u &&
			first.NumRefs() == 1u && second.NumRefs() == 1u,
			"the last refusal must release all resource/fence ownership edges");
	}

	void TestFailedFenceReleasesRecordedDependencies()
	{
		VulkanGraphicsDriver driver;
		auto command = RHICommandListPtr::Make(ECommandListQueue::Transfer);
		auto retained = MakeResource();
		auto resource = MakeResource();
		auto fence = RHIFencePtr::Make();
		fence->AddDependency(command);
		fence->AddDependency(retained);
		driver.TrackDelayedInitialization(resource.GetRawPtr(), fence);
		Require(command.NumRefs() == 2u && retained.NumRefs() == 2u, "the fixture must retain both dependencies");
		Require(!driver.SubmitCommandList(command, fence), "a command without a native buffer must be refused");
		Require(command.NumRefs() == 1u && retained.NumRefs() == 1u && resource.NumRefs() == 1u && fence.NumRefs() == 1u,
			"failed submission must release command dependencies as well as initialization observers");
	}

	void TestRepeatedTrackingAndFailureStayTerminal()
	{
		VulkanGraphicsDriver driver;
		auto resource = MakeResource();
		auto fence = RHIFencePtr::Make();
		for (uint32_t i = 0; i < 3u; ++i) driver.TrackDelayedInitialization(resource.GetRawPtr(), fence);
		Require(!driver.SubmitCommandList({}, fence), "submission must fail");
		Require(resource.NumRefs() == 1u && fence.NumRefs() == 1u,
			"each registered dependency must be removed even when an owner was registered more than once");
		Require(!driver.SubmitCommandList({}, fence) && !driver.SubmitCommandList({}, {}),
			"repeated refusal and a missing fence must remain harmless");
		Require(resource->HasInitializationFailed() && !resource->IsReady() &&
			resource.NumRefs() == 1u && fence.NumRefs() == 1u,
			"repeated cleanup must not resurrect or retain a failed resource");
		auto replacement = MakeResource();
		Require(replacement->IsReady() && !replacement->HasInitializationFailed(),
			"failure belongs to one resource, not the driver or future resource instances");
	}

	void TestUnsubmittedFenceStaysPending()
	{
		auto fence = RHIFencePtr::Make();
		Require(fence->GetStatus() == EFenceStatus::Pending && fence->Wait(0u) == EFenceStatus::Pending &&
			!fence->IsFinished() && !fence->HasFailed() && !fence->Reset(),
			"a fence without a submission cannot report successful completion");
	}
}

int main()
{
	try
	{
		TestRejectedSubmissionReleasesInitializationOwners();
		TestFailureOnlyRemovesItsOwnDependencies();
		TestFailedFenceReleasesRecordedDependencies();
		TestRepeatedTrackingAndFailureStayTerminal();
		TestUnsubmittedFenceStaysPending();
		std::cout << "RHI submission tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
