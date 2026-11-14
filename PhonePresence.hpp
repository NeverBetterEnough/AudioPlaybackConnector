#pragma once

struct PhonePresence
{
	winrt::guid container;
	bool connected;
};

DeviceWatcher g_phoneWatcher = nullptr;
DeviceWatcher g_sourceWatcher = nullptr;
winrt::event_token g_phoneAdded{}, g_phoneUpdated{}, g_phoneRemoved{};
winrt::event_token g_sourceAdded{}, g_sourceRemoved{};
std::unordered_map<std::wstring, PhonePresence> g_phonePresence;
uint64_t g_presenceSerial = 0;
winrt::guid g_presenceTarget{};
bool g_presenceKnown = false;
bool g_presenceConnected = false;

void RefreshPhonePresence()
{
	if (g_targetContainerId == winrt::guid{}) return;
	if (g_presenceTarget != g_targetContainerId)
	{
		g_presenceTarget = g_targetContainerId;
		g_presenceKnown = false;
	}
	for (const auto& item : g_phonePresence)
	{
		if (item.second.container != g_targetContainerId) continue;
		const bool changed = g_presenceKnown && g_presenceConnected != item.second.connected;
		if (!g_presenceKnown || changed)
			DiagnosticLog(L"phone Bluetooth presence connected=" + std::to_wstring(item.second.connected) +
				L" transition=" + std::to_wstring(changed));
		g_presenceKnown = true;
		g_presenceConnected = item.second.connected;
		if (changed && g_presenceConnected && g_connectionEnabled && !g_stopping)
		{
			// A real ACL reconnection takes precedence over a previous offline backoff.
			// OpenAsync is idempotent on the same receiver and does not tear it down.
			g_reopenOnArrival = true;
			g_retryDelay = 5000;
			KillTimer(g_hWnd, RECONNECT_TIMER);
			DiagnosticLog(L"phone returned; immediately request OpenAsync on retained receiver; no silence detection");
			PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
		}
		return;
	}
}

winrt::fire_and_forget ApplyPhoneAdded(ApartmentReturn context, uint64_t serial, DeviceInformation device)
{
	try
	{
		co_await context;
		if (g_stopping || serial != g_presenceSerial) co_return;
		const auto properties = device.Properties();
		if (!properties.HasKey(L"System.Devices.Aep.ContainerId")) co_return;
		const auto container = winrt::unbox_value_or<winrt::guid>(properties.Lookup(L"System.Devices.Aep.ContainerId"), winrt::guid{});
		const bool connected = properties.HasKey(L"System.Devices.Aep.IsConnected") &&
			winrt::unbox_value_or<bool>(properties.Lookup(L"System.Devices.Aep.IsConnected"), false);
		g_phonePresence[std::wstring(device.Id())] = { container, connected };
		RefreshPhonePresence();
	}
	catch (const winrt::hresult_error& error) { DiagnosticLog(L"phone watcher Added failed hresult=" + HResultText(error.code())); }
	catch (...) { DiagnosticLog(L"phone watcher Added failed"); }
}

winrt::fire_and_forget ApplyPhoneUpdated(ApartmentReturn context, uint64_t serial, DeviceInformationUpdate update, bool removed)
{
	try
	{
		co_await context;
		if (g_stopping || serial != g_presenceSerial) co_return;
		const auto found = g_phonePresence.find(std::wstring(update.Id()));
		if (found == g_phonePresence.end()) co_return;
		const auto properties = update.Properties();
		if (removed) found->second.connected = false;
		else if (properties.HasKey(L"System.Devices.Aep.IsConnected"))
			found->second.connected = winrt::unbox_value_or<bool>(properties.Lookup(L"System.Devices.Aep.IsConnected"), false);
		else co_return;
		RefreshPhonePresence();
		if (removed) g_phonePresence.erase(found);
	}
	catch (const winrt::hresult_error& error) { DiagnosticLog(L"phone watcher update failed hresult=" + HResultText(error.code())); }
	catch (...) { DiagnosticLog(L"phone watcher update failed"); }
}

winrt::fire_and_forget ApplySourcePresence(ApartmentReturn context, uint64_t serial, std::wstring id, bool removed)
{
	try
	{
		co_await context;
		if (g_stopping || serial != g_presenceSerial || id != g_targetDeviceId) co_return;
		DiagnosticLog(removed ? L"paired audio source removed; release invalid receiver" : L"paired audio source available");
		if (removed)
		{
			DisconnectConnections();
			if (g_connectionEnabled)
			{
				SetConnectionStatus(_(L"Waiting for the phone; will retry automatically"));
				ScheduleReconnect();
			}
		}
		else if (g_connectionEnabled)
		{
			g_retryDelay = 5000;
			PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
		}
	}
	catch (const winrt::hresult_error& error) { DiagnosticLog(L"source watcher failed hresult=" + HResultText(error.code())); }
	catch (...) { DiagnosticLog(L"source watcher failed"); }
}

void StopPhonePresenceWatcher()
{
	++g_presenceSerial;
	try
	{
		if (g_phoneWatcher)
		{
			g_phoneWatcher.Added(g_phoneAdded);
			g_phoneWatcher.Updated(g_phoneUpdated);
			g_phoneWatcher.Removed(g_phoneRemoved);
			if (g_phoneWatcher.Status() == DeviceWatcherStatus::Started || g_phoneWatcher.Status() == DeviceWatcherStatus::EnumerationCompleted)
				g_phoneWatcher.Stop();
		}
		if (g_sourceWatcher)
		{
			g_sourceWatcher.Added(g_sourceAdded);
			g_sourceWatcher.Removed(g_sourceRemoved);
			if (g_sourceWatcher.Status() == DeviceWatcherStatus::Started || g_sourceWatcher.Status() == DeviceWatcherStatus::EnumerationCompleted)
				g_sourceWatcher.Stop();
		}
	}
	CATCH_LOG();
	g_phoneWatcher = nullptr;
	g_sourceWatcher = nullptr;
	g_phonePresence.clear();
	g_presenceKnown = false;
}

void StartPhonePresenceWatcher()
{
	try
	{
		const auto context = ApartmentReturn{};
		const auto serial = ++g_presenceSerial;
		auto properties = winrt::single_threaded_vector<winrt::hstring>({ L"System.Devices.Aep.ContainerId", L"System.Devices.Aep.IsConnected" });
		g_phoneWatcher = DeviceInformation::CreateWatcher(
			winrt::Windows::Devices::Bluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(true),
			properties, DeviceInformationKind::AssociationEndpoint);
		g_phoneAdded = g_phoneWatcher.Added([context, serial](const auto&, const auto& device) { ApplyPhoneAdded(context, serial, device); });
		g_phoneUpdated = g_phoneWatcher.Updated([context, serial](const auto&, const auto& update) { ApplyPhoneUpdated(context, serial, update, false); });
		g_phoneRemoved = g_phoneWatcher.Removed([context, serial](const auto&, const auto& update) { ApplyPhoneUpdated(context, serial, update, true); });
		g_sourceWatcher = DeviceInformation::CreateWatcher(AudioPlaybackConnection::GetDeviceSelector());
		g_sourceAdded = g_sourceWatcher.Added([context, serial](const auto&, const auto& device) { ApplySourcePresence(context, serial, std::wstring(device.Id()), false); });
		g_sourceRemoved = g_sourceWatcher.Removed([context, serial](const auto&, const auto& device) { ApplySourcePresence(context, serial, std::wstring(device.Id()), true); });
		g_phoneWatcher.Start();
		g_sourceWatcher.Start();
		DiagnosticLog(L"phone and audio-source watchers started; offline polling is fallback only");
	}
	catch (const winrt::hresult_error& error)
	{
		DiagnosticLog(L"presence watcher unavailable; polling fallback hresult=" + HResultText(error.code()));
		StopPhonePresenceWatcher();
	}
}
