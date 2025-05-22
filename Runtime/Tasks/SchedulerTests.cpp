		#include "Sailor.h"
	#include "Tasks/Tasks.h"
	#include "Tasks/Scheduler.h"
	#include <atomic>
	#include <thread>
	
	using namespace Sailor;
	using namespace Sailor::Tasks;
	
	extern "C" {
	
	SAILOR_API bool SchedulerTest_Initialization()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	const unsigned cores = std::thread::hardware_concurrency();
	unsigned expectedWorkers = std::max(1u, cores - 2u - scheduler->GetNumRHIThreads());
	unsigned expectedTotal = 1u + expectedWorkers + scheduler->GetNumRHIThreads();
	bool result = scheduler->GetNumWorkerThreads() == expectedTotal;
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_BasicTaskExecution()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	std::atomic<bool> flag = false;
	auto task = Tasks::CreateTask("BasicTask", [&]() { flag = true; });
	scheduler->Run(task);
	scheduler->WaitIdle(EThreadType::Worker);
	bool result = flag && task->IsFinished();
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_TaskChaining()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	std::atomic<int> value = 0;
	auto first = Tasks::CreateTaskWithResult<int>("First", []() { return 42; });
	auto second = first->Then<void>([&](int v) { value = v; });
	scheduler->Run(first);
	scheduler->WaitIdle(EThreadType::Worker);
	bool result = value == 42 && first->IsFinished() && second->IsFinished();
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_TaskDependencies()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	std::atomic<int> order = 0;
	auto first = Tasks::CreateTask("First", [&]() { order = 1; });
	auto second = Tasks::CreateTask("Second", [&]() { if(order==1) order = 2; else order = -1; });
	second->Join(first);
	scheduler->Run(first);
	scheduler->Run(second);
	scheduler->WaitIdle(EThreadType::Worker);
	bool result = order == 2;
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_ThreadSpecificExecution()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	DWORD renderThreadId = 0;
	EThreadType renderType = EThreadType::Worker;
	SAILOR_ENQUEUE_TASK_RENDER_THREAD("RenderTask", [&]() { renderThreadId = GetCurrentThreadId(); renderType = scheduler->GetCurrentThreadType(); });
	scheduler->WaitIdle(EThreadType::Render);
	DWORD rhiThreadId = 0;
	EThreadType rhiType = EThreadType::Worker;
	SAILOR_ENQUEUE_TASK_RHI_THREAD("RhiTask", [&]() { rhiThreadId = GetCurrentThreadId(); rhiType = scheduler->GetCurrentThreadType(); });
	scheduler->WaitIdle(EThreadType::RHI);
	bool result = renderThreadId == scheduler->GetRendererThreadId() && renderType == EThreadType::Render && rhiType == EThreadType::RHI;
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_WaitIdleRHI()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	std::atomic<int> counter = 0;
	for(int i=0;i<5;i++)
	{
	auto task = Tasks::CreateTask("RhiTask", [&](){ counter++; }, EThreadType::RHI);
	scheduler->Run(task);
	}
	scheduler->WaitIdle(EThreadType::RHI);
	bool result = counter == 5;
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_RunExplicitThread()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	if(scheduler->GetNumWorkerThreads() < 2)
	{
	App::Shutdown();
	return false;
	}
	DWORD workerId = scheduler->GetWorkerThreadId(1);
	std::atomic<DWORD> executedId = 0;
	auto task = Tasks::CreateTask("Explicit", [&]() { executedId = GetCurrentThreadId(); });
	scheduler->Run(task, workerId);
	scheduler->WaitIdle(EThreadType::Worker);
	bool result = executedId == workerId;
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_ConcurrentInsertion()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	std::atomic<int> counter = 0;
	const int tasksPerThread = 50;
	const int numThreads = 4;
	std::vector<std::thread> threads;
	for(int t=0;t<numThreads;t++)
	{
	threads.emplace_back([&]() {
	for(int i=0;i<tasksPerThread;i++)
	{
	auto task = Tasks::CreateTask("Conc", [&]() { counter++; });
	scheduler->Run(task);
	}
	});
	}
	for(auto& th : threads) th.join();
	scheduler->WaitIdle(EThreadType::Worker);
	bool result = counter == tasksPerThread * numThreads;
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_TaskSyncBlockReuse()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	TVector<uint16_t> handles;
	for(int i=0;i<100;i++)
	{
	handles.Add(scheduler->AcquireTaskSyncBlock());
	}
	for(auto h : handles)
	{
	scheduler->ReleaseTaskSyncBlock(h);
	}
	TVector<uint16_t> reacquired;
	for(int i=0;i<100;i++)
	{
	reacquired.Add(scheduler->AcquireTaskSyncBlock());
	}
	bool result = true;
	for(int i=0;i<100;i++)
	{
	if(handles[i]!=reacquired[i]){ result=false; break; }
	scheduler->ReleaseTaskSyncBlock(reacquired[i]);
	}
	App::Shutdown();
	return result;
	}
	
	SAILOR_API bool SchedulerTest_MainThreadProcessing()
	{
	App::Initialize();
	auto scheduler = App::GetSubmodule<Sailor::Tasks::Scheduler>();
	TVector<int> order;
	for(int i=0;i<3;i++)
	{
	auto task = Tasks::CreateTask("Main", [&,i]() { order.Add(i); }, EThreadType::Main);
	scheduler->Run(task);
	}
	scheduler->ProcessTasksOnMainThread();
	bool result = order.Num()==3 && order[0]==0 && order[1]==1 && order[2]==2;
	App::Shutdown();
	return result;
	}
	
	}
