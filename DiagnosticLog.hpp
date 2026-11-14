#pragma once

std::mutex g_logMutex;
fs::path g_logPath;

void InitializeDiagnosticLog()
{
	g_logPath = GetModuleFsPath(g_hInst).remove_filename() / L"logs" / L"AudioPlaybackConnector.log";
	std::error_code error;
	fs::create_directories(g_logPath.parent_path(), error);
}

void DiagnosticLog(const std::wstring& message) noexcept
{
	try
	{
		std::lock_guard<std::mutex> guard(g_logMutex);
		if (g_logPath.empty()) return;
		WIN32_FILE_ATTRIBUTE_DATA attributes{};
		if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &attributes) &&
			(attributes.nFileSizeHigh || attributes.nFileSizeLow > 2 * 1024 * 1024))
			MoveFileExW(g_logPath.c_str(), (g_logPath.wstring() + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
		SYSTEMTIME time{};
		GetSystemTime(&time);
		wchar_t prefix[80];
		swprintf_s(prefix, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [thread=%lu] ",
			time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, GetCurrentThreadId());
		auto line = std::wstring(prefix) + message;
		std::replace(line.begin(), line.end(), L'\r', L' ');
		std::replace(line.begin(), line.end(), L'\n', L' ');
		line += L"\r\n";
		OutputDebugStringW(line.c_str());
		const auto bytes = Utf16ToUtf8(line);
		wil::unique_hfile file(CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (file)
		{
			DWORD written = 0;
			WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
		}
	}
	catch (...) { }
}

std::wstring HResultText(HRESULT result)
{
	wchar_t text[16];
	swprintf_s(text, L"0x%08X", static_cast<uint32_t>(result));
	return text;
}
