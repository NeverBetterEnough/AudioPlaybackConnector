#pragma once

void SetConnectionStatus(const std::wstring& status)
{
	if (g_connectionStatus != status)
		DiagnosticLog(L"status " + status);
	g_connectionStatus = status;
	if (g_settingsWnd)
		SetDlgItemTextW(g_settingsWnd, IDC_CONNECTION_STATUS, status.c_str());
	const auto tooltip = std::wstring(L"AudioPlaybackConnector - ") + status;
	wcsncpy_s(g_nid.szTip, tooltip.c_str(), _TRUNCATE);
	if (g_nid.hWnd)
		Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

std::wstring ConnectionError(const winrt::hresult_error& error)
{
	wchar_t code[16];
	swprintf_s(code, L" (0x%08X)", static_cast<uint32_t>(error.code()));
	return std::wstring(error.message()) + code;
}

void SetDeviceStatus(DevicePicker picker, DeviceInformation device, const std::wstring& text, DevicePickerDisplayStatusOptions options)
{
	try { picker.SetDisplayStatus(device, text, options); }
	CATCH_LOG();
}

void ScheduleReconnect()
{
	if (g_connectionEnabled && !g_stopping)
	{
		SetTimer(g_hWnd, RECONNECT_TIMER, g_retryDelay, nullptr);
		DiagnosticLog(L"retry scheduled delay_ms=" + std::to_wstring(g_retryDelay));
		g_retryDelay = (std::min)(g_retryDelay * 2, 60000u);
	}
}

void RevokeConnectionEvents(const std::wstring& id, AudioPlaybackConnection connection)
{
	const auto signal = g_connectionStateSignals.find(id);
	if (signal != g_connectionStateSignals.end())
	{
		SetEvent(signal->second->event.get());
		g_connectionStateSignals.erase(signal);
	}
	const auto token = g_connectionStateTokens.find(id);
	if (token == g_connectionStateTokens.end()) return;
	try { connection.StateChanged(token->second); }
	catch (const winrt::hresult_error& error) { DiagnosticLog(L"event cleanup hresult=" + HResultText(error.code())); }
	g_connectionStateTokens.erase(token);
}

bool ConnectionReleaseComplete()
{
	for (auto it = g_drainingOperations.begin(); it != g_drainingOperations.end();)
	{
		bool running = false;
		try { running = it->Status() == winrt::Windows::Foundation::AsyncStatus::Started; }
		catch (...) { }
		if (running) ++it;
		else it = g_drainingOperations.erase(it);
	}
	if (g_drainingOperations.empty()) return true;
	SetTimer(g_hWnd, RECONNECT_TIMER, 25, nullptr);
	SetConnectionStatus(_(L"Preparing phone audio; waiting for the previous connection to close"));
	return false;
}

void DisconnectConnections()
{
	KillTimer(g_hWnd, RECONNECT_TIMER);
	KillTimer(g_hWnd, CONNECTION_TIMEOUT_TIMER);
	++g_connectionGeneration;
	++g_operationSerial;
	DiagnosticLog(L"disconnect generation=" + std::to_wstring(g_connectionGeneration) + L" connections=" + std::to_wstring(g_audioPlaybackConnections.size()));
	if (g_pendingOperation)
	{
		g_drainingOperations.push_back(g_pendingOperation);
		try { g_pendingOperation.Cancel(); }
		CATCH_LOG();
		g_pendingOperation = nullptr;
	}
	g_connecting = false;
	InvalidateAudioActivation();
	g_receiverLifecycle.Released();
	g_reopenOnArrival = false;
	// Remove entries before Close raises StateChanged, including pending opens.
	auto connections = std::move(g_audioPlaybackConnections);
	g_audioPlaybackConnections.clear();
	for (const auto& item : connections)
	{
		RevokeConnectionEvents(item.first, item.second.second);
		try { item.second.second.Close(); }
		CATCH_LOG();
		SetDeviceStatus(g_devicePicker, item.second.first, {}, DevicePickerDisplayStatusOptions::None);
	}
	DiagnosticLog(L"release requested; no fixed delay; draining_operations=" + std::to_wstring(g_drainingOperations.size()));
}

bool IsCurrentConnectionAttempt(uint64_t generation, uint64_t operation = 0)
{
	return !g_stopping && g_connectionEnabled && generation == g_connectionGeneration &&
		(operation == 0 || operation == g_operationSerial);
}

void CancelTimedOutConnection()
{
	KillTimer(g_hWnd, CONNECTION_TIMEOUT_TIMER);
	++g_operationSerial;
	if (g_pendingOperation)
	{
		g_drainingOperations.push_back(g_pendingOperation);
		try { g_pendingOperation.Cancel(); }
		CATCH_LOG();
		g_pendingOperation = nullptr;
	}
	for (const auto& signal : g_connectionStateSignals) SetEvent(signal.second->event.get());
	g_connecting = false;
	if (g_receiverLifecycle.KeepListening())
	{
		g_receiverLifecycle.OpenFailed();
		InvalidateAudioActivation();
		DiagnosticLog(L"open timeout; canceled attempt only; receiver remains registered for incoming connections");
	}
	else DisconnectConnections();
	SetConnectionStatus(_(L"The request timed out"));
	ScheduleReconnect();
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device, bool reuseExisting = false)
{
	const std::wstring id(device.Id());
	if (!g_connectionEnabled || g_stopping || g_connecting || (!reuseExisting && g_audioPlaybackConnections.count(id)))
		co_return;
	if (!ConnectionReleaseComplete()) co_return;
	const auto generation = g_connectionGeneration;
	const auto operation = ++g_operationSerial;
	const auto attemptStarted = GetTickCount64();
	std::wstring stage = L"TryCreateFromId";
	g_connecting = true;
	SetTimer(g_hWnd, CONNECTION_TIMEOUT_TIMER, 30000, nullptr);
	g_reopenOnArrival = false;
	DiagnosticLog(L"connect begin generation=" + std::to_wstring(generation) + L" reuse_receiver=" + std::to_wstring(reuseExisting));
	SetConnectionStatus(_(L"Connecting"));
	SetDeviceStatus(picker, device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);
	bool success = false;
	std::wstring error;
	try
	{
		auto connection = reuseExisting ? g_audioPlaybackConnections.at(id).second : AudioPlaybackConnection::TryCreateFromId(id);
		if (!connection)
			winrt::throw_hresult(E_FAIL);
		std::shared_ptr<ConnectionStateSignal> signal;
		if (!reuseExisting)
		{
			g_receiverLifecycle.Created();
			g_audioPlaybackConnections.emplace(id, std::pair(device, connection));
			signal = std::make_shared<ConnectionStateSignal>();
			THROW_LAST_ERROR_IF(!signal->event);
			g_connectionStateSignals[id] = signal;
			g_connectionStateTokens[id] = connection.StateChanged([generation, signal](const auto& sender, const auto&) {
				int state = -1;
				try { state = static_cast<int>(sender.State()); }
				catch (...) { }
				SetEvent(signal->event.get());
				PostMessageW(g_hWnd, WM_CONNECTIONCHANGED, static_cast<WPARAM>(generation), state + 1);
			});
			stage = L"StartAsync";
			DiagnosticLog(L"connect stage=StartAsync begin");
			auto start = connection.StartAsync();
			g_pendingOperation = start.as<winrt::Windows::Foundation::IAsyncInfo>();
			co_await start;
			if (!IsCurrentConnectionAttempt(generation, operation)) co_return;
			g_receiverLifecycle.Registered();
			DiagnosticLog(L"receiver registered; remains listening across transport loss and failed opens");
			DiagnosticLog(L"connect stage=StartAsync complete elapsed_ms=" + std::to_wstring(GetTickCount64() - attemptStarted));
		}
		else signal = g_connectionStateSignals.at(id);
		stage = L"OpenAsync";
		auto open = connection.OpenAsync();
		g_pendingOperation = open.as<winrt::Windows::Foundation::IAsyncInfo>();
		const auto result = co_await open;
		if (!IsCurrentConnectionAttempt(generation, operation))
			co_return;
		DiagnosticLog(L"connect stage=OpenAsync complete status=" + std::to_wstring(static_cast<int>(result.Status())) +
			L" hresult=" + HResultText(result.ExtendedError()) + L" state=" + std::to_wstring(static_cast<int>(connection.State())) + L" elapsed_ms=" + std::to_wstring(GetTickCount64() - attemptStarted));
		switch (result.Status())
		{
		case AudioPlaybackConnectionOpenResultStatus::Success:
		{
			stage = L"WaitForOpenedState";
			auto ready = AwaitTransportReadiness(signal, [connection] { return connection.State() == AudioPlaybackConnectionState::Opened; },
				[generation, operation] { return IsCurrentConnectionAttempt(generation, operation); });
			g_pendingOperation = ready.as<winrt::Windows::Foundation::IAsyncInfo>();
			co_await ready;
			if (!IsCurrentConnectionAttempt(generation, operation)) co_return;
			DiagnosticLog(L"transport confirmed Opened elapsed_ms=" + std::to_wstring(GetTickCount64() - attemptStarted));
			success = true;
			break;
		}
		case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
			error = _(L"The request timed out");
			break;
		case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
			error = _(L"The operation was denied by the system");
			break;
		default:
		{
			const auto extendedError = result.ExtendedError();
			winrt::throw_hresult(FAILED(extendedError) ? extendedError : winrt::hresult(E_FAIL));
		}
		}
	}
	catch (const winrt::hresult_error& ex)
	{
		error = ConnectionError(ex);
		DiagnosticLog(L"connect failed stage=" + stage + L" hresult=" + HResultText(ex.code()) +
			L" generation=" + std::to_wstring(generation));
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
		error = _(L"Unknown error");
		LOG_CAUGHT_EXCEPTION();
	}
	if (!IsCurrentConnectionAttempt(generation, operation))
		co_return;
	g_connecting = false;
	g_pendingOperation = nullptr;
	KillTimer(g_hWnd, CONNECTION_TIMEOUT_TIMER);
	if (success)
	{
		if (g_reopenOnArrival) DiagnosticLog(L"phone arrival coalesced into successful in-flight open; no duplicate OpenAsync");
		g_reopenOnArrival = false;
		g_receiverLifecycle.Opened();
		KillTimer(g_hWnd, RECONNECT_TIMER);
		SetConnectionStatus(_(L"Connected; waiting for phone audio"));
		SetDeviceStatus(picker, device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
		OnAudioConnectionOpened();
	}
	else
	{
		g_receiverLifecycle.OpenFailed();
		InvalidateAudioActivation();
		const auto item = g_audioPlaybackConnections.find(id);
		if (!g_receiverLifecycle.KeepListening() && item != g_audioPlaybackConnections.end())
		{
			auto connection = item->second.second;
			RevokeConnectionEvents(id, connection);
			g_audioPlaybackConnections.erase(item);
			try { connection.Close(); }
			CATCH_LOG();
		}
		else if (g_receiverLifecycle.KeepListening())
			DiagnosticLog(L"open failed; keeping started receiver available for incoming phone connections");
		SetDeviceStatus(picker, device, error, DevicePickerDisplayStatusOptions::ShowRetryButton);
		SetConnectionStatus(std::wstring(_(L"Will retry automatically: ")) + error);
		ScheduleReconnect();
	}
	// An arrival received during an outstanding open must not be lost to backoff.
	if (g_reopenOnArrival)
	{
		KillTimer(g_hWnd, RECONNECT_TIMER);
		PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
	}
}

winrt::fire_and_forget ConnectRememberedDevice()
{
	if (!g_connectionEnabled || g_stopping || g_connecting)
		co_return;
	if (!ConnectionReleaseComplete()) co_return;
	if (!g_audioPlaybackConnections.empty())
	{
		if (!g_receiverLifecycle.TransportReady() || g_reopenOnArrival)
		{
			const auto device = g_audioPlaybackConnections.begin()->second.first;
			ConnectDevice(g_devicePicker, device, true);
		}
		co_return;
	}
	const auto generation = g_connectionGeneration;
	const auto operation = ++g_operationSerial;
	g_connecting = true;
	SetTimer(g_hWnd, CONNECTION_TIMEOUT_TIMER, 30000, nullptr);
	SetConnectionStatus(_(L"Looking for the phone"));
	DeviceInformation target = nullptr;
	try
	{
		auto properties = winrt::single_threaded_vector<winrt::hstring>({ L"System.Devices.ContainerId" });
		auto enumerate = DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector(), properties);
		g_pendingOperation = enumerate.as<winrt::Windows::Foundation::IAsyncInfo>();
		const auto devices = co_await enumerate;
		if (!IsCurrentConnectionAttempt(generation, operation))
			co_return;
		DiagnosticLog(L"enumerate supported_devices=" + std::to_wstring(devices.Size()));
		for (const auto& device : devices)
		{
			if (device.Id() == g_targetDeviceId || (g_targetDeviceId.empty() && devices.Size() == 1))
			{
				target = device;
				break;
			}
		}
		if (target)
		{
			g_targetDeviceId = target.Id();
			g_targetDeviceName = target.Name();
			g_targetContainerId = {};
			if (target.Properties().HasKey(L"System.Devices.ContainerId"))
				g_targetContainerId = winrt::unbox_value_or<winrt::guid>(target.Properties().Lookup(L"System.Devices.ContainerId"), winrt::guid{});
			RefreshPhonePresence();
			SaveSettings();
		}
		else
		{
			SetConnectionStatus(g_targetDeviceId.empty() && devices.Size() > 1 ? _(L"Choose a phone in Settings") : _(L"Waiting for the phone; will retry automatically"));
		}
	}
	catch (const winrt::hresult_error& ex)
	{
		if (IsCurrentConnectionAttempt(generation, operation))
			SetConnectionStatus(ConnectionError(ex));
		LOG_CAUGHT_EXCEPTION();
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}
	if (!IsCurrentConnectionAttempt(generation, operation))
		co_return;
	g_connecting = false;
	g_pendingOperation = nullptr;
	KillTimer(g_hWnd, CONNECTION_TIMEOUT_TIMER);
	if (target)
		ConnectDevice(g_devicePicker, target);
	else
		ScheduleReconnect();
}

void CheckClosedConnections()
{
	if (g_connecting || g_stopping)
		return;
	for (const auto& item : g_audioPlaybackConnections)
	{
		bool isClosed = true;
		try { isClosed = item.second.second.State() == AudioPlaybackConnectionState::Closed; }
		catch (const winrt::hresult_error& error) { DiagnosticLog(L"connection state query failed hresult=" + HResultText(error.code())); }
		if (isClosed)
		{
			const bool wasOpened = g_receiverLifecycle.TransportReady();
			g_receiverLifecycle.TransportClosed();
			InvalidateAudioActivation();
			if (!wasOpened) continue;
			DiagnosticLog(L"transport Closed; retaining registered receiver; reopen same receiver without teardown");
			SetDeviceStatus(g_devicePicker, item.second.first, {}, DevicePickerDisplayStatusOptions::None);
			SetConnectionStatus(_(L"Waiting for the phone; will retry automatically"));
			g_retryDelay = 5000;
			PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
		}
		else if (!g_receiverLifecycle.TransportReady())
		{
			g_receiverLifecycle.Opened();
			KillTimer(g_hWnd, RECONNECT_TIMER);
			DiagnosticLog(L"incoming transport Opened on retained receiver");
			SetDeviceStatus(g_devicePicker, item.second.first, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			SetConnectionStatus(_(L"Connected; waiting for phone audio"));
			OnAudioConnectionOpened();
		}
	}
}

void ApplyConnectionPreferenceChange(bool wasEnabled, bool targetChanged)
{
	DiagnosticLog(L"settings connection_enabled=" + std::to_wstring(g_connectionEnabled) +
		L" was_enabled=" + std::to_wstring(wasEnabled) + L" target_changed=" + std::to_wstring(targetChanged));
	g_retryDelay = 5000;
	if (!g_connectionEnabled)
	{
		g_audioRepair.Cancel();
		g_verifyAfterResume = false;
		DisconnectConnections();
		SetConnectionStatus(_(L"Temporarily disabled"));
	}
	else if (!wasEnabled || targetChanged)
	{
		if (!g_audioPlaybackConnections.empty() || g_pendingOperation) DisconnectConnections();
		g_targetContainerId = {};
		g_verifyAfterResume = true;
		SetConnectionStatus(_(L"Preparing phone audio; waiting for the previous connection to close"));
		PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
		DiagnosticLog(L"resume requested; event-driven connection; no fixed delay; no silence-triggered repair");
	}
	else if (!g_receiverLifecycle.TransportReady() && !g_connecting)
		PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
}

void SetPhoneConnectionEnabled(bool enabled)
{
	const auto previous = g_connectionEnabled;
	g_connectionEnabled = enabled;
	try { SaveSettings(); }
	catch (...) { g_connectionEnabled = previous; throw; }
	ApplyConnectionPreferenceChange(previous, false);
	if (g_settingsWnd)
		SendDlgItemMessageW(g_settingsWnd, IDC_CONNECTION_MODE, CB_SETCURSEL, enabled ? 0 : 1, 0);
}
