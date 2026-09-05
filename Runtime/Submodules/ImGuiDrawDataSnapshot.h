#pragma once
#include <imgui.h>

namespace Sailor
{
	// Capture and release on the ImGui owner thread. Once published, render tasks
	// may read this data without consulting the context mutated by NewFrame().
	class ImGuiDrawDataSnapshot final
	{
	public:

		explicit ImGuiDrawDataSnapshot(const ImDrawData* source)
		{
			if (!source || !source->Valid)
			{
				return;
			}

			m_data.Valid = true;
			m_data.DisplayPos = source->DisplayPos;
			m_data.DisplaySize = source->DisplaySize;
			m_data.FramebufferScale = source->FramebufferScale;
			m_data.CmdLists.reserve(source->CmdListsCount);
			for (const ImDrawList* sourceList : source->CmdLists)
			{
				ImDrawList* list = sourceList->CloneOutput();
				// CloneOutput copies geometry/commands, not the owned callback
				// payload. Pointers into the source list must not escape the frame.
				list->_CallbacksDataBuf = sourceList->_CallbacksDataBuf;
				for (ImDrawCmd& command : list->CmdBuffer)
				{
					if (command.UserCallbackDataSize > 0)
					{
						command.UserCallbackData = list->_CallbacksDataBuf.Data +
							command.UserCallbackDataOffset;
					}
				}
				m_data.CmdLists.push_back(list);
				m_data.TotalIdxCount += list->IdxBuffer.Size;
				m_data.TotalVtxCount += list->VtxBuffer.Size;
			}
			m_data.CmdListsCount = m_data.CmdLists.Size;
		}

		~ImGuiDrawDataSnapshot()
		{
			for (ImDrawList* list : m_data.CmdLists)
			{
				IM_DELETE(list);
			}
		}

		ImGuiDrawDataSnapshot(const ImGuiDrawDataSnapshot&) = delete;
		ImGuiDrawDataSnapshot& operator=(const ImGuiDrawDataSnapshot&) = delete;

		const ImDrawData& GetDrawData() const { return m_data; }

	private:

		ImDrawData m_data;
	};
}
