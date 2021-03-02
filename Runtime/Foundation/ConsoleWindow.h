#pragma once
#include <cstdio>

// This class redirects stdout to the current console window, if there is one.
// It can also open a new console window on demand if the application is
// started from windows but wants console output anyway.
class ConsoleWindow {
public:
	// Setup singleton access.
	static void setup(bool to_attach);
	// Remove singleton.
	static void shutdown();
	// Get singleton instance.
	static ConsoleWindow& get();

	// Opens a new console window and redirects stdout to the new window.
	void open_window(const char* title);
	// Closes the console window if open.
	void close_window();

	// Fill a buffer with data read from the console window. If this
	// returns 0 or a number less than `buffer_size`, there is no
	// more to read right now.
	unsigned int read(char* buffer, unsigned int buffer_size);

	// Call this every frame.
	void update();

	// Returns true if the console window has been closed.
	bool exit_requested();
private:
	ConsoleWindow(bool to_attach);
	~ConsoleWindow();

	// Attach to default console window, if any.
	void attach();
	// Close files and console window.
	void free();

	// Write a unicode character to the console.
	void write(wchar_t c);

	static ConsoleWindow* __instance;
	FILE* _stdout_file;
	FILE* _stderr_file;
	FILE* _stdin_file;
	bool _attach;

	static const unsigned int LINE_BUFFER_SIZE = 256;
	unsigned int _buffer_size; // Number of characters in the buffer.
	wchar_t _buffer[LINE_BUFFER_SIZE]; // Buffer for unfinished lines.
};
