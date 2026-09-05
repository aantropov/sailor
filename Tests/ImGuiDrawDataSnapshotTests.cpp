#include "Submodules/ImGuiDrawDataSnapshot.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	int copiedCallbackValue = 0;
	void ReadCopiedCallback(const ImDrawList*, const ImDrawCmd* command)
	{
		std::memcpy(&copiedCallbackValue, command->UserCallbackData, sizeof(int));
	}

	void CheckRetainedFrame()
	{
		ImGui::NewFrame();
		auto* background = ImGui::GetBackgroundDrawList();
		background->AddRectFilled({ 11, 17 }, { 95, 105 }, IM_COL32_WHITE);
		auto* foreground = ImGui::GetForegroundDrawList();
		foreground->PushClipRect({ 7, 9 }, { 411, 277 }, false);
		foreground->AddText({ 20, 20 }, IM_COL32_WHITE, "Retained HUD");
		int payload = 31415;
		foreground->AddCallback(ReadCopiedCallback, &payload, sizeof(payload));
		foreground->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
		// Force 16-bit index rollover: offsets in the retained commands must
		// still address the same uploaded vertices after the source is reused.
		for (int i = 0; i < 20000; ++i)
		{
			foreground->AddRectFilled({ 30, 40 }, { 31, 41 }, IM_COL32_WHITE);
		}
		foreground->PopClipRect();
		ImGui::Render();

		const ImDrawData* source = ImGui::GetDrawData();
		Sailor::ImGuiDrawDataSnapshot snapshot(source);
		const ImDrawData& retained = snapshot.GetDrawData();
		Require(retained.Valid && retained.CmdListsCount == 2, "Both draw layers must be captured");
		Require(retained.TotalVtxCount == source->TotalVtxCount && retained.TotalVtxCount > 65536,
			"Large vertex payload must survive capture");
		Require(retained.TotalIdxCount == source->TotalIdxCount, "Index counts must agree");
		Require(retained.FramebufferScale.x == 2 && retained.DisplaySize.x == 640,
			"Retina projection must be captured with geometry");
		bool hasLargeOffset = false;
		for (int i = 0; i < retained.CmdListsCount; ++i)
		{
			const auto* list = retained.CmdLists[i];
			Require(list != source->CmdLists[i], "Draw lists must not alias the context");
			Require(std::memcmp(list->VtxBuffer.Data, source->CmdLists[i]->VtxBuffer.Data,
				list->VtxBuffer.Size * sizeof(ImDrawVert)) == 0, "Vertex bytes must match the upload");
			Require(std::memcmp(list->IdxBuffer.Data, source->CmdLists[i]->IdxBuffer.Data,
				list->IdxBuffer.Size * sizeof(ImDrawIdx)) == 0, "Index bytes must match the upload");
			for (const ImDrawCmd& command : list->CmdBuffer)
			{
				hasLargeOffset |= command.VtxOffset > 0;
			}
		}
		Require(hasLargeOffset, "Fixture must exercise nonzero vertex offsets");
		const int expectedVertices = retained.TotalVtxCount;
		const float expectedX = retained.CmdLists[0]->VtxBuffer[0].pos.x;

		// The render consumer starts only after a later NewFrame has invalidated
		// the context's draw data, then reads concurrently with more UI frames.
		ImGui::NewFrame();
		ImGui::GetForegroundDrawList()->AddText({ 50, 60 }, IM_COL32_WHITE, "Replacement");
		ImGui::Render();
		std::atomic<bool> changed{ false };
		std::jthread consumer([&]
			{
				for (int i = 0; i < 100000; ++i)
				{
					if (retained.TotalVtxCount != expectedVertices ||
						retained.CmdLists[0]->VtxBuffer[0].pos.x != expectedX)
					{
						changed.store(true);
					}
				}
			});
		for (int i = 0; i < 30; ++i)
		{
			ImGui::GetIO().DisplaySize = { float(400 + i), 300 };
			ImGui::NewFrame();
			ImGui::GetForegroundDrawList()->AddRectFilled({ 0, 0 }, { 10, 10 }, IM_COL32_WHITE);
			ImGui::Render();
		}
		consumer.join();
		Require(!changed.load(), "A newer CPU frame must not change the retained frame");
		Require(retained.DisplaySize.x == 640, "Resize must not alter an older projection");
		bool readCallback = false;
		for (const ImDrawList* list : retained.CmdLists)
		{
			for (const ImDrawCmd& command : list->CmdBuffer)
			{
				if (command.UserCallback == ReadCopiedCallback)
				{
					command.UserCallback(list, &command);
					readCallback = true;
				}
			}
		}
		Require(readCallback && copiedCallbackValue == payload, "Copied callback payload must outlive its source frame");
	}
}

int main()
{
	ImGui::CreateContext();
	try
	{
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = { 640, 480 };
		io.DisplayFramebufferScale = { 2, 2 };
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		CheckRetainedFrame();
		Sailor::ImGuiDrawDataSnapshot empty(nullptr);
		Require(!empty.GetDrawData().Valid && empty.GetDrawData().CmdListsCount == 0,
			"Missing draw data must produce an empty snapshot");
		ImGui::DestroyContext();
		std::cout << "PASS: immutable multi-list HUD, large offsets, resize, delayed reader and copied callbacks\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		ImGui::DestroyContext();
		std::cerr << error.what() << '\n';
		return 1;
	}
}
