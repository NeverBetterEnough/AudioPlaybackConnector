#pragma once

uint64_t g_settingsGeneration = 0;

winrt::fire_and_forget LoadSettingsDevices(HWND dialog)
{
	const auto generation = ++g_settingsGeneration;
	try
	{
		const auto devices = co_await DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector());
		if (dialog != g_settingsWnd || generation != g_settingsGeneration)
			co_return;
		g_settingsDevices.clear();
		for (const auto& device : devices)
			g_settingsDevices.push_back(device);
		const auto combo = GetDlgItem(dialog, IDC_PHONE);
		SendMessageW(combo, CB_RESETCONTENT, 0, 0);
		const auto placeholder = g_targetDeviceId.empty() ? std::wstring(_(L"Choose a phone")) :
			g_targetDeviceName + _(L" (currently unavailable)");
		SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(placeholder.c_str()));
		SendMessageW(combo, CB_SETITEMDATA, 0, -1);
		int selected = 0;
		for (size_t i = 0; i < g_settingsDevices.size(); ++i)
		{
			const auto& device = g_settingsDevices[i];
			const auto index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.Name().c_str()));
			SendMessageW(combo, CB_SETITEMDATA, index, static_cast<LPARAM>(i));
			if (device.Id() == g_targetDeviceId || (g_targetDeviceId.empty() && g_settingsDevices.size() == 1))
				selected = static_cast<int>(index);
		}
		SendMessageW(combo, CB_SETCURSEL, selected, 0);
		EnableWindow(combo, !g_settingsDevices.empty());
	}
	catch (const winrt::hresult_error& ex)
	{
		if (dialog == g_settingsWnd && generation == g_settingsGeneration)
			SetDlgItemTextW(dialog, IDC_CONNECTION_STATUS, ConnectionError(ex).c_str());
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}
}

void ApplySettings(HWND dialog)
{
	const auto wasEnabled = g_connectionEnabled;
	const auto previousId = g_targetDeviceId;
	const auto previousName = g_targetDeviceName;
	const auto previousStartup = IsStartupEnabled();
	const auto startup = IsDlgButtonChecked(dialog, IDC_STARTUP) == BST_CHECKED;
	try
	{
		const auto combo = GetDlgItem(dialog, IDC_PHONE);
		const auto selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
		const auto index = selected == CB_ERR ? -1 : SendMessageW(combo, CB_GETITEMDATA, selected, 0);
		if (index >= 0 && static_cast<size_t>(index) < g_settingsDevices.size())
		{
			g_targetDeviceId = g_settingsDevices[index].Id();
			g_targetDeviceName = g_settingsDevices[index].Name();
		}
		g_connectionEnabled = SendDlgItemMessageW(dialog, IDC_CONNECTION_MODE, CB_GETCURSEL, 0, 0) == 0;
		if (startup != previousStartup)
			SetStartupEnabled(startup);
		SaveSettings();
	}
	catch (const winrt::hresult_error& ex)
	{
		g_connectionEnabled = wasEnabled;
		g_targetDeviceId = previousId;
		g_targetDeviceName = previousName;
		try { if (startup != previousStartup) SetStartupEnabled(previousStartup); }
		CATCH_LOG();
		MessageBoxW(dialog, ConnectionError(ex).c_str(), _(L"Could not save settings"), MB_OK | MB_ICONERROR);
		return;
	}
	const bool targetChanged = previousId != g_targetDeviceId;
	ApplyConnectionPreferenceChange(wasEnabled, targetChanged);
	SetDlgItemTextW(dialog, IDC_APPLY_SETTINGS, _(L"Saved"));
}

INT_PTR CALLBACK SettingsDlgProc(HWND dialog, UINT message, WPARAM wParam, LPARAM)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		g_settingsWnd = dialog;
		SetWindowTextW(dialog, _(L"Settings"));
		SetDlgItemTextW(dialog, IDC_STARTUP, _(L"Start with Windows"));
		SetDlgItemTextW(dialog, IDC_STARTUP_DESCRIPTION, _(L"Start in the system tray after signing in to Windows."));
		SetDlgItemTextW(dialog, IDC_CONNECTION_LABEL, _(L"Phone audio connection"));
		SetDlgItemTextW(dialog, IDC_PHONE_LABEL, _(L"Phone to connect automatically"));
		SetDlgItemTextW(dialog, IDC_SETTINGS_DESCRIPTION, _(L"Disconnecting may pause the phone player. After reconnecting, resume playback on the phone if needed."));
		SetDlgItemTextW(dialog, IDC_CONNECTION_STATUS, g_connectionStatus.c_str());
		SetDlgItemTextW(dialog, IDC_APPLY_SETTINGS, _(L"Apply"));
		SetDlgItemTextW(dialog, IDCANCEL, _(L"Close"));
		SetDlgItemTextW(dialog, IDC_REPAIR_AUDIO, _(L"Check and repair silent audio"));
		SetDlgItemTextW(dialog, IDC_OPEN_LOG, _(L"View diagnostic log"));
		CheckDlgButton(dialog, IDC_STARTUP, IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);
		SendDlgItemMessageW(dialog, IDC_CONNECTION_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(_(L"Enabled")));
		SendDlgItemMessageW(dialog, IDC_CONNECTION_MODE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(_(L"Temporarily disabled")));
		SendDlgItemMessageW(dialog, IDC_CONNECTION_MODE, CB_SETCURSEL, g_connectionEnabled ? 0 : 1, 0);
		SendDlgItemMessageW(dialog, IDC_PHONE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(_(L"Looking for the phone")));
		SendDlgItemMessageW(dialog, IDC_PHONE, CB_SETCURSEL, 0, 0);
		EnableWindow(GetDlgItem(dialog, IDC_PHONE), FALSE);
		SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR))));
		LoadSettingsDevices(dialog);
		return TRUE;
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_REPAIR_AUDIO)
		{
			BeginAudioRepair();
			return TRUE;
		}
		if (LOWORD(wParam) == IDC_OPEN_LOG)
		{
			DiagnosticLog(L"diagnostic log opened by user");
			ShellExecuteW(dialog, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			return TRUE;
		}
		if (LOWORD(wParam) == IDC_APPLY_SETTINGS)
		{
			try { ApplySettings(dialog); }
			CATCH_LOG();
			return TRUE;
		}
		if (LOWORD(wParam) == IDCANCEL)
		{
			DestroyWindow(dialog);
			return TRUE;
		}
		if (HIWORD(wParam) == CBN_SELCHANGE || (LOWORD(wParam) == IDC_STARTUP && HIWORD(wParam) == BN_CLICKED))
			SetDlgItemTextW(dialog, IDC_APPLY_SETTINGS, _(L"Apply"));
		break;
	case WM_CLOSE:
		DestroyWindow(dialog);
		return TRUE;
	case WM_DESTROY:
		++g_settingsGeneration;
		g_settingsWnd = nullptr;
		g_settingsDevices.clear();
		return TRUE;
	}
	return FALSE;
}

void ShowSettingsWindow()
{
	if (!g_settingsWnd)
		g_settingsWnd = CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, SettingsDlgProc, 0);
	if (g_settingsWnd)
	{
		ShowWindow(g_settingsWnd, SW_SHOWNORMAL);
		// A tray launch may carry SW_HIDE, which overrides the first ShowWindow call.
		SetWindowPos(g_settingsWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
		SetForegroundWindow(g_settingsWnd);
	}
}
