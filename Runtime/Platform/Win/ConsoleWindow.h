#pragma once
#include <cstdio>
#include "Sailor.h"
#include "Core/Singleton.hpp"

// This class redirects stdout to the current console window, if there is one.
// It can also open a new console window on demand if the application is
// started from windows but wants console output anyway.
class ConsoleWindow : public Singleton<ConsoleWindow>
{
public:
	// Setup singleton access.
	static SAILOR_API void Initialize(bool bInShouldAttach);

	// Opens a new console window and redirects stdout to the new window.
	SAILOR_API void OpenWindow(const wchar_t* pTitle);

	// Closes the console window if open.
	SAILOR_API void CloseWindow();

	// Fill a buffer with data read from the console window. If this
	// returns 0 or a number less than `buffer_size`, there is no
	// more to read right now.
	uint32_t Read(char* pOutBuffer, uint32_t bufferSize);

	// Call this every frame.
	SAILOR_API void Update();

	// Returns true if the console window has been closed.
	bool IsExitRequested();

	virtual SAILOR_API ~ConsoleWindow() override;
	
private:
	
	SAILOR_API ConsoleWindow(bool bInShouldAttach);

	// Attach to default console window, if any.
	SAILOR_API void Attach();
	
	// Close files and console window.
	SAILOR_API void Free();

	// Write a unicode character to the console.
	SAILOR_API void Write(wchar_t c);

	FILE* m_stdout_file;
	FILE* m_stderr_file;
	FILE* m_stdin_file;
	
	bool m_bShouldAttach;

	static const uint32_t LINE_BUFFER_SIZE = 256;

	// Number of characters in the buffer.
	uint32_t m_bufferSize;

	// Buffer for unfinished lines.
	wchar_t m_buffer[LINE_BUFFER_SIZE]; 
};
