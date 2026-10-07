#include "Core/LogMacros.h"
#include "Sailor.h"
#include "Submodules/Editor.h"
#include "Tasks/Tasks.h"

#include <array>
#include <barrier>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

using namespace Sailor;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	class ConsoleCapture
	{
	public:
		ConsoleCapture() : m_oldOutput(std::cout.rdbuf(output.rdbuf())), m_oldErrors(std::cerr.rdbuf(errors.rdbuf())) {}
		~ConsoleCapture()
		{
			std::cout.rdbuf(m_oldOutput);
			std::cerr.rdbuf(m_oldErrors);
		}

		std::ostringstream output, errors;

	private:
		std::streambuf* m_oldOutput;
		std::streambuf* m_oldErrors;
	};

	std::vector<std::string> PullEditorMessages()
	{
		std::vector<std::string> result;
		std::array<char*, 1024> messages{};
		const uint32_t count = App::PullEditorMessages(messages.data(), static_cast<uint32_t>(messages.size()));
		for (uint32_t i = 0; i < count; ++i)
		{
			const std::string message(messages[i]);
			delete[] messages[i];
			Require(message.size() >= 11 && message[0] == '[' && message[3] == ':' &&
				message[6] == ':' && message[9] == ']' && message[10] == ' ', "editor logs must retain their timestamp");
			result.push_back(message.substr(11));
		}
		return result;
	}

	void TestFormatting()
	{
		ConsoleCapture capture;
		int evaluations = 0;
		SAILOR_LOG("value=%d hex=%08x size=%zu float=%.2f percent=%% text=%s",
			++evaluations, 0x2au, size_t(17), 1.25, "UTF-8: \xD0\x9C\xD0\xBE\xD1\x80\xD0\xB5");
		SAILOR_LOG_ERROR("error=%lld text=%s", -1234567890123LL, "%s %n %%");
		SAILOR_LOG("");
		SAILOR_LOG("literal");
		const std::string longMessage(5000, 'x');
		SAILOR_LOG("%s", longMessage.c_str());
		SAILOR_LOG_ERROR("%s", longMessage.c_str());
		Require(evaluations == 1, "log arguments must be evaluated once");
		Require(capture.output.str() ==
			"value=1 hex=0000002a size=17 float=1.25 percent=% text=UTF-8: \xD0\x9C\xD0\xBE\xD1\x80\xD0\xB5\n\nliteral\n" +
			longMessage.substr(0, 4095) + "\n", "normal logs must preserve formatting, UTF-8 bytes and bounded output");
		Require(capture.errors.str() == "error=-1234567890123 text=%s %n %%\n" + longMessage.substr(0, 4095) + "\n",
			"error logs must use stderr and the same formatting bound");
	}

	void TestBorrowedTextFormatting()
	{
		ConsoleCapture capture;
		const auto status = std::string_view("Corrupt::ignored").substr(0, 7);
		const std::string_view empty;
		SAILOR_LOG("status=%.*s: report", static_cast<int>(status.size()), status.data());
		SAILOR_LOG_ERROR("status=%.*s: report", static_cast<int>(empty.size()), empty.empty() ? "" : empty.data());
		Require(capture.output.str() == "status=Corrupt: report\n" && capture.errors.str() == "status=: report\n",
			"logging must respect bounded and default-empty views without requiring a temporary C string");
	}

	void TestQueuedMessages(Tasks::Scheduler& scheduler, EThreadType producer, bool hasEditor)
	{
		ConsoleCapture capture;
		std::vector<std::string> expectedMessages;
		std::string expectedOutput, expectedErrors;
		const std::string prefix = producer == EThreadType::Render ? "Renderer thread: " : "";
		auto task = Tasks::CreateTask("Produce log messages"_h, [&]()
			{
				for (int i = 0; i < 24; ++i)
				{
					std::string message = "queued-" + std::to_string(i) + "-" + std::string(i == 23 ? 5000 : i * 7, 'a' + i % 26);
					expectedMessages.push_back(message.substr(0, 4095));
					if (i % 2 == 0)
					{
						SAILOR_LOG("%s", message.c_str());
						expectedOutput += prefix + expectedMessages.back() + "\n";
					}
					else
					{
						SAILOR_LOG_ERROR("%s", message.c_str());
						expectedErrors += prefix + expectedMessages.back() + "\n";
					}
					message.assign(512, '!');
				}
			}, producer);
		task->Run()->Wait();
		Require(capture.output.str().empty() && capture.errors.str().empty(),
			"worker console output must wait for the Main queue");
		Require(scheduler.GetNumTasks(EThreadType::Main) == expectedMessages.size(),
			"each queued message must have an owned pending delivery");
		const auto messages = PullEditorMessages();
		Require(messages == (hasEditor ? expectedMessages : std::vector<std::string>{}),
			"editor messages must be immediate, ordered and independent of console labels");
		scheduler.ProcessTasksOnMainThread();
		Require(capture.output.str() == expectedOutput && capture.errors.str() == expectedErrors,
			"deferred messages must own their text and retain order, severity and Render labels");
		Require(scheduler.GetNumTasks(EThreadType::Main) == 0 && PullEditorMessages().empty(),
			"draining console delivery must not duplicate editor messages or leave tasks");
	}

	void TestConcurrentProducers(Tasks::Scheduler& scheduler)
	{
		ConsoleCapture capture;
		constexpr std::array producers{ EThreadType::Worker, EThreadType::Render, EThreadType::RHI, EThreadType::GI };
		std::array<Tasks::ITaskPtr, producers.size()> tasks;
		std::barrier start(static_cast<std::ptrdiff_t>(producers.size()));
		for (size_t producer = 0; producer < producers.size(); ++producer)
		{
			tasks[producer] = Tasks::CreateTask("Concurrent log producer"_h, [&, producer]()
				{
					start.arrive_and_wait();
					for (int i = 0; i < 16; ++i)
					{
						if (i % 2 == 0) SAILOR_LOG("concurrent-%zu-%d", producer, i);
						else SAILOR_LOG_ERROR("concurrent-%zu-%d", producer, i);
					}
				}, producers[producer]);
			tasks[producer]->Run();
		}
		for (auto& task : tasks) task->Wait();
		const auto editorMessages = PullEditorMessages();
		Require(editorMessages.size() == producers.size() * 16, "concurrent producers must not lose editor messages");
		scheduler.ProcessTasksOnMainThread();
		for (size_t producer = 0; producer < producers.size(); ++producer)
		{
			const std::string prefix = "concurrent-" + std::to_string(producer) + "-";
			int next = 0;
			for (const auto& message : editorMessages)
			{
				if (message.starts_with(prefix))
					Require(message == prefix + std::to_string(next++), "editor ordering must follow each concurrent producer");
			}
			Require(next == 16, "each concurrent producer must deliver all its editor messages");
			const std::string consolePrefix = (producers[producer] == EThreadType::Render ? "Renderer thread: " : "") + prefix;
			for (int severity = 0; severity < 2; ++severity)
			{
				std::istringstream lines(severity == 0 ? capture.output.str() : capture.errors.str());
				next = severity;
				for (std::string line; std::getline(lines, line);)
				{
					if (line.starts_with(consolePrefix))
					{
						Require(line == consolePrefix + std::to_string(next), "console ordering and severity must follow each producer");
						next += 2;
					}
				}
				Require(next == 16 + severity, "each concurrent producer must deliver all its console messages");
			}
		}
	}
}

namespace Sailor::Tests
{
	void RunEditorMessageViewTests()
	{
		auto* editor = App::GetSubmodule<Editor>();
		Require(editor != nullptr, "borrowed messages must be tested through the real editor queue");
		PullEditorMessages();
		{
			std::string source = "prefix:borrowed editor message:suffix";
			editor->PushMessage(std::string_view(source).substr(7, 23));
			source.assign(1024, 'x');
		}
		editor->PushMessage({});
		Require(PullEditorMessages() == std::vector<std::string>{ "borrowed editor message", "" },
			"queued messages must own bounded input and accept a default-empty view");
		std::cout << "Editor message views: bounded input survived source modification and destruction\n";
	}

	void RunLoggingWithoutAppTests()
	{
		Require(!App::GetInstance(), "standalone logging test must not borrow a live App");
		TestFormatting();
		TestBorrowedTextFormatting();
		{
			ConsoleCapture capture;
			std::thread producer([]()
				{
					SAILOR_LOG("without scheduler: %d", 42);
					SAILOR_LOG_ERROR("without scheduler: %s", "error");
				});
			producer.join();
			Require(capture.output.str() == "without scheduler: 42\n" &&
				capture.errors.str() == "without scheduler: error\n", "logging without App must not depend on a scheduler");
		}
		std::cout << "Logging without App tests passed\n";
	}

	void RunLoggingCommandTests()
	{
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		Require(scheduler && scheduler->IsMainThread() && App::HasEditor(), "logging test needs the real editor and Main owner");
		scheduler->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render,
			EThreadType::Editor, EThreadType::Background, EThreadType::Physics, EThreadType::Audio, EThreadType::GI });
		PullEditorMessages();
		{
			ConsoleCapture capture;
			SAILOR_LOG("main %d", 1);
			SAILOR_LOG_ERROR("main error %d", 2);
			Require(capture.output.str() == "main 1\n" && capture.errors.str() == "main error 2\n" &&
				PullEditorMessages() == std::vector<std::string>{ "main 1", "main error 2" },
				"Main logs must reach both outputs immediately with no duplicate formatting");
		}
		for (const auto producer : { EThreadType::Worker, EThreadType::Render, EThreadType::RHI, EThreadType::Editor,
			EThreadType::Background, EThreadType::Physics, EThreadType::Audio, EThreadType::GI })
		{
			TestQueuedMessages(*scheduler, producer, true);
		}
		TestConcurrentProducers(*scheduler);
		{
			ConsoleCapture capture;
			auto pending = Tasks::CreateTask("Log before editor detach"_h, []()
				{
					SAILOR_LOG("editor-detach-pending");
				}, EThreadType::Worker);
			pending->Run()->Wait();
			Require(PullEditorMessages() == std::vector<std::string>{ "editor-detach-pending" },
				"the editor must receive its message before detaching");
			App::RemoveSubmodule<Editor>();
			scheduler->ProcessTasksOnMainThread();
			Require(capture.output.str() == "editor-detach-pending\n" && capture.errors.str().empty(),
				"pending console delivery must not retain or access the detached editor");
		}
		Require(!App::HasEditor(), "the detached editor must no longer receive messages");
		TestQueuedMessages(*scheduler, EThreadType::Worker, false);
		TestQueuedMessages(*scheduler, EThreadType::Render, false);
		std::cout << "Logging Main/worker/editor delivery tests passed\n";
	}

	void RunLoggingShutdownTests()
	{
		std::string output, errors;
		{
			ConsoleCapture capture;
			auto task = Tasks::CreateTask("Log before shutdown"_h, []()
				{
					SAILOR_LOG("pending-shutdown-message");
					SAILOR_LOG_ERROR("pending-shutdown-error");
				}, EThreadType::Background);
			task->Run()->Wait();
			Require(capture.output.str().empty() && capture.errors.str().empty(), "shutdown test needs pending console deliveries");
			task.Clear();
			Require(App::Shutdown() && !App::GetInstance(), "shutdown must drain logging and release the App");
			output = capture.output.str();
			errors = capture.errors.str();
			const auto first = output.find("pending-shutdown-message\n");
			const auto error = errors.find("pending-shutdown-error\n");
			Require(first != std::string::npos && output.find("pending-shutdown-message\n", first + 1) == std::string::npos &&
				error != std::string::npos && errors.find("pending-shutdown-error\n", error + 1) == std::string::npos,
				"shutdown must deliver each pending message exactly once");
		}
		std::cout << output;
		std::cerr << errors;
		RunLoggingWithoutAppTests();
		std::cout << "Logging shutdown delivery tests passed\n";
	}
}
