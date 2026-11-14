#pragma once

constexpr auto CONFIG_NAME = L"AudioPlaybackConnector.json";
constexpr auto STARTUP_KEY = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";
constexpr auto STARTUP_NAME = L"AudioPlaybackConnector";

void DefaultSettings()
{
	g_connectionEnabled = true;
	g_targetDeviceId.clear();
	g_targetDeviceName.clear();
}

void LoadSettings()
{
	DefaultSettings();
	try
	{
		const auto path = GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME;
		wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!file && GetLastError() == ERROR_FILE_NOT_FOUND)
			return;
		THROW_LAST_ERROR_IF(!file);
		std::string data;
		char buffer[4096];
		DWORD read = 0;
		do
		{
			THROW_IF_WIN32_BOOL_FALSE(ReadFile(file.get(), buffer, sizeof(buffer), &read, nullptr));
			data.append(buffer, read);
		} while (read);
		const auto json = JsonObject::Parse(Utf8ToUtf16(data));
		g_connectionEnabled = json.GetNamedBoolean(L"connectionEnabled", true);
		g_targetDeviceId = json.GetNamedString(L"targetDeviceId", L"");
		g_targetDeviceName = json.GetNamedString(L"targetDeviceName", L"");
		// Import the previous version's remembered device list.
		if (g_targetDeviceId.empty() && json.HasKey(L"lastDevices"))
		{
			const auto devices = json.GetNamedArray(L"lastDevices");
			if (devices.Size() && devices.GetAt(0).ValueType() == JsonValueType::String)
				g_targetDeviceId = devices.GetStringAt(0);
		}
	}
	CATCH_LOG();
}

void SaveSettings()
{
	JsonObject json;
	json.Insert(L"connectionEnabled", JsonValue::CreateBooleanValue(g_connectionEnabled));
	json.Insert(L"targetDeviceId", JsonValue::CreateStringValue(g_targetDeviceId));
	json.Insert(L"targetDeviceName", JsonValue::CreateStringValue(g_targetDeviceName));
	const auto path = GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME;
	const auto temporary = path.wstring() + L".tmp";
	const auto data = Utf16ToUtf8(json.Stringify());
	wil::unique_hfile file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	THROW_LAST_ERROR_IF(!file);
	DWORD written = 0;
	THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr));
	THROW_HR_IF(E_FAIL, written != data.size());
	THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(file.get()));
	file.reset();
	THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}

std::wstring StartupCommand()
{
	return L"\"" + GetModuleFsPath(g_hInst).wstring() + L"\"";
}

bool IsStartupEnabled(HKEY root = HKEY_CURRENT_USER)
{
	DWORD bytes = 0;
	if (RegGetValueW(root, STARTUP_KEY, STARTUP_NAME, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
		return false;
	std::wstring command(bytes / sizeof(wchar_t), L'\0');
	if (RegGetValueW(root, STARTUP_KEY, STARTUP_NAME, RRF_RT_REG_SZ, nullptr, command.data(), &bytes) != ERROR_SUCCESS)
		return false;
	command.resize(wcsnlen(command.c_str(), command.size()));
	return command == StartupCommand();
}

void SetStartupEnabled(bool enabled, HKEY root = HKEY_CURRENT_USER)
{
	if (enabled)
	{
		HKEY rawKey = nullptr;
		THROW_IF_WIN32_ERROR(RegCreateKeyExW(root, STARTUP_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &rawKey, nullptr));
		wil::unique_hkey key(rawKey);
		const auto command = StartupCommand();
		THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), STARTUP_NAME, 0, REG_SZ,
			reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))));
	}
	else
	{
		HKEY rawKey = nullptr;
		const auto result = RegOpenKeyExW(root, STARTUP_KEY, 0, KEY_SET_VALUE, &rawKey);
		if (result == ERROR_FILE_NOT_FOUND)
			return;
		THROW_IF_WIN32_ERROR(result);
		wil::unique_hkey key(rawKey);
		const auto deleted = RegDeleteValueW(key.get(), STARTUP_NAME);
		if (deleted != ERROR_FILE_NOT_FOUND)
			THROW_IF_WIN32_ERROR(deleted);
	}
}
