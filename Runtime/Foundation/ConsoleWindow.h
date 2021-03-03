#pragma once
#include <cstdio>

// This class redirects stdout to the current console window, if there is one.
// It can also open a new console window on demand if the application is
// started from windows but wants console output anyway.
class ConsoleWindow
{
public:
	// Setup singleton access.
	static void Initialize(bool bInShouldAttach);

	// Remove singleton.
	static void Release();

	// Get singleton instance.
	static ConsoleWindow& GetInstance();

	// Opens a new console window and redirects stdout to the new window.
	void OpenWindow(const wchar_t* Title);

	// Closes the console window if open.
	void CloseWindow();

	// Fill a buffer with data read from the console window. If this
	// returns 0 or a number less than `buffer_size`, there is no
	// more to read right now.
	unsigned int Read(char* OutBuffer, unsigned int BufferSize);

	// Call this every frame.
	void Update();

	// Returns true if the console window has been closed.
	bool IsExitRequested();
	
private:
	
	ConsoleWindow(bool bInShouldAttach);
	~ConsoleWindow();

	// Attach to default console window, if any.
	void Attach();
	
	// Close files and console window.
	void Free();

	// Write a unicode character to the console.
	void Write(wchar_t c);

	static ConsoleWindow* instance;
	
	FILE* stdout_file;
	FILE* stderr_file;
	FILE* stdin_file;
	
	bool bShouldAttach;

	static const unsigned int LINE_BUFFER_SIZE = 256;

	// Number of characters in the buffer.
	unsigned int bufferSize;

	// Buffer for unfinished lines.
	wchar_t buffer[LINE_BUFFER_SIZE]; 
};
