#include "pch.h"
#include "AudioPlaybackConnector.h"
#include "ConnectionController.hpp"
#include "PhonePresence.hpp"
#include "AudioMonitor.hpp"
#include "SettingsWindow.hpp"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(nCmdShow);
	const std::wstring_view arguments(lpCmdLine);

	wil::unique_handle instance(CreateMutexW(nullptr, FALSE, L"Local\\AudioPlaybackConnector"));
	if (instance && GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if (const auto existing = FindWindowW(L"AudioPlaybackConnector", nullptr))
		{
			const bool modeCommand = arguments == L"--pause-audio" || arguments == L"--resume-audio";
			PostMessageW(existing, modeCommand ? WM_SET_AUDIO_ENABLED : (arguments == L"--repair-audio" ? WM_REPAIR_AUDIO : WM_SHOWSETTINGS),
				modeCommand && arguments == L"--resume-audio", 0);
		}
		return EXIT_SUCCESS;
	}
	g_hInst = hInstance;
	InitializeDiagnosticLog();
	DiagnosticLog(L"application starting; diagnostics v7; persistent MTA, verified audio initialization, no Bluetooth teardown for silent startup");
	g_repairOnStartup = std::wstring_view(lpCmdLine) == L"--repair-audio";
	LoadTranslateData();
	winrt::init_apartment(winrt::apartment_type::single_threaded);

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	if (arguments == L"--pause-audio" || arguments == L"--resume-audio")
	{
		g_connectionEnabled = arguments == L"--resume-audio";
		g_verifyAfterResume = g_connectionEnabled;
		try { SaveSettings(); }
		CATCH_LOG();
	}
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	SetConnectionStatus(g_connectionEnabled ? _(L"Looking for the phone") : _(L"Temporarily disabled"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	StartAudioWorker();
	StartPhonePresenceWatcher();
	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
	SetTimer(g_hWnd, AUDIO_HEALTH_TIMER, 2000, nullptr);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		if (g_settingsWnd && IsDialogMessageW(g_settingsWnd, &msg))
			continue;
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		g_stopping = true;
		DiagnosticLog(L"application stopping");
		g_audioRepair.Cancel();
		StopPhonePresenceWatcher();
		InvalidateAudioActivation();
		StopAudioWorker();
		KillTimer(hWnd, AUDIO_HEALTH_TIMER);
		try { SaveSettings(); }
		CATCH_LOG();
		DisconnectConnections();
		if (g_settingsWnd)
			DestroyWindow(g_settingsWnd);
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_QUERYENDSESSION:
		try { SaveSettings(); }
		CATCH_LOG();
		return TRUE;
	case WM_ENDSESSION:
		if (wParam)
			DestroyWindow(hWnd);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			if (!g_connectionEnabled)
			{
				ShowSettingsWindow();
				break;
			}
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_SHOWSETTINGS:
		ShowSettingsWindow();
		break;
	case WM_REPAIR_AUDIO:
		BeginAudioRepair();
		break;
	case WM_SET_AUDIO_ENABLED:
		try { SetPhoneConnectionEnabled(wParam != 0); }
		CATCH_LOG();
		break;
	case WM_CONNECTDEVICE:
		ConnectRememberedDevice();
		break;
	case WM_AUDIO_SESSION_READY:
		if (wParam && g_connectionEnabled)
			DiagnosticLog(L"audio native notification reason=" + std::to_wstring(wParam) + L" state=" + std::to_wstring(lParam));
		PollAudioHealth();
		break;
	case WM_AUDIO_SURVEY_RESULT:
		ReceiveAudioSurvey();
		break;
	case WM_CONNECTIONCHANGED:
		if (wParam == static_cast<WPARAM>(g_connectionGeneration))
		{
			DiagnosticLog(L"transport StateChanged event state=" + std::to_wstring(static_cast<int>(lParam) - 1) +
				L" opening=" + std::to_wstring(g_connecting));
			CheckClosedConnections();
		}
		break;
	case WM_TIMER:
		if (wParam == AUDIO_INITIALIZATION_TIMER)
		{
			KillTimer(hWnd, AUDIO_INITIALIZATION_TIMER);
			PollAudioHealth();
			break;
		}
		if (wParam == AUDIO_HEALTH_TIMER)
		{
			PollAudioHealth();
			break;
		}
		if (wParam == CONNECTION_TIMEOUT_TIMER && g_connecting)
		{
			DiagnosticLog(L"connect timeout after 30000ms; cancelling pending operation");
			CancelTimedOutConnection();
			break;
		}
		if (wParam == RECONNECT_TIMER)
		{
			KillTimer(hWnd, RECONNECT_TIMER);
			ConnectRememberedDevice();
		}
		break;
	case WM_POWERBROADCAST:
		if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
			PostMessageW(hWnd, WM_CONNECTDEVICE, 0, 0);
		return TRUE;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([](const auto&, const auto&) {
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	FontIcon appSettingsIcon;
	appSettingsIcon.Glyph(L"\xE713");
	MenuFlyoutItem appSettingsItem;
	appSettingsItem.Text(_(L"Settings"));
	appSettingsItem.Icon(appSettingsIcon);
	appSettingsItem.Click([](const auto&, const auto&) {
		PostMessageW(g_hWnd, WM_SHOWSETTINGS, 0, 0);
	});

	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (g_audioPlaybackConnections.size() == 0)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(appSettingsItem);
	menu.Items().Append(settingsItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto&, const auto& args) {
		if (!g_connectionEnabled)
			return;
		const auto device = args.SelectedDevice();
		const bool newTarget = g_targetDeviceId != device.Id() || g_audioPlaybackConnections.empty();
		g_targetDeviceId = device.Id();
		g_targetDeviceName = device.Name();
		g_retryDelay = 5000;
		try { SaveSettings(); }
		CATCH_LOG();
		ApplyConnectionPreferenceChange(true, newTarget);
	});
	g_devicePicker.DisconnectButtonClicked([](const auto&, const auto&) {
		try { SetPhoneConnectionEnabled(false); }
		CATCH_LOG();
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
