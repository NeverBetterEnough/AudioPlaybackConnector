#include "../pch.h"
#include "../AudioPlaybackConnector.h"
#include <cstdio>
#include <stdexcept>

void Require(bool condition, const char* message)
{
	if (!condition) throw std::runtime_error(message);
}

void WriteConfig(const char* content)
{
	const auto path = GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME;
	wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	THROW_LAST_ERROR_IF(!file);
	DWORD written = 0;
	THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content, static_cast<DWORD>(strlen(content)), &written, nullptr));
}

void CleanupTestRegistry(const std::wstring& testKey)
{
	// Delete only the fixed, empty test keys from the leaves up.
	for (const auto suffix : { L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"\\Software\\Microsoft\\Windows\\CurrentVersion", L"\\Software\\Microsoft\\Windows", L"\\Software\\Microsoft", L"\\Software", L"" })
		RegDeleteKeyW(HKEY_CURRENT_USER, (testKey + suffix).c_str());
}

void TestAudioHealthPolicy()
{
	AudioHealthFacts facts;
	Require(ClassifyAudioHealth(facts) == AudioHealthKind::Disconnected, "Disconnected is not a silent-stream failure");
	facts.connected = true;
	Require(ClassifyAudioHealth(facts) == AudioHealthKind::OutputUnavailable, "Missing output must be distinguished");
	facts.outputAvailable = true;
	Require(ClassifyAudioHealth(facts) == AudioHealthKind::SessionMissing, "Missing phone session must be distinguished");
	facts.sessionFound = true;
	Require(ClassifyAudioHealth(facts) == AudioHealthKind::WrongOutput, "Other-output phone sessions must be distinguished");
	facts.sessionOnDefaultOutput = true;
	Require(ClassifyAudioHealth(facts) == AudioHealthKind::Unmeasurable, "Unavailable meters must not be classified as silence");
	facts.meterValid = true;
	facts.muted = true;
	Require(ClassifyAudioHealth(facts) == AudioHealthKind::Muted, "Muted audio must be distinguished from silence");
	facts.muted = false;
	Require(ClassifyAudioHealth(facts) == AudioHealthKind::Quiet, "A valid zero meter is quiet, not proof of phone failure");
	facts.peak = 0.2f;
	Require(ClassifyAudioHealth(facts) == AudioHealthKind::Signal, "A nonzero phone signal must be detected");
	AudioRepairPolicy repair;
	Require(repair.Observe(AudioHealthKind::Quiet, 60000) == AudioRepairDecision::None, "Quiet playback must not reconnect without an explicit request");
	repair.Request(1000);
	for (uint64_t time = 3000; time < 11000; time += 2000)
		Require(repair.Observe(AudioHealthKind::Quiet, time) == AudioRepairDecision::None, "Repair must collect a bounded observation window");
	Require(repair.Observe(AudioHealthKind::Quiet, 11000) == AudioRepairDecision::Reconnect, "Explicit repair may reconnect once");
	repair.OnConnected(12000);
	for (uint64_t time = 14000; time < 22000; time += 2000) repair.Observe(AudioHealthKind::Quiet, time);
	Require(repair.Observe(AudioHealthKind::Quiet, 22000) == AudioRepairDecision::NeedsUserAction && !repair.Active(), "Repeated silence must stop rather than reconnect indefinitely");
	repair.Request(1000);
	Require(repair.Observe(AudioHealthKind::Signal, 2000) == AudioRepairDecision::SignalDetected && !repair.Active(), "Signal must end repair successfully");
	repair.Request(1000);
	Require(repair.Observe(AudioHealthKind::Muted, 2000) == AudioRepairDecision::NeedsUserAction, "Repair must not unmute or change system volume");
	repair.Request(1000);
	Require(repair.Observe(AudioHealthKind::Disconnected, 91000) == AudioRepairDecision::NeedsUserAction, "Connection failures must have a deadline");
	repair.Request(1000);
	repair.Cancel();
	Require(repair.Observe(AudioHealthKind::Quiet, 60000) == AudioRepairDecision::None, "Pause must cancel repair");
	std::puts("PASS: audio classification, invalid meters, silence without reconnect, single repair budget, signal completion, mute preservation, deadline, cancellation");
}

void PumpTransportTest(winrt::Windows::Foundation::IAsyncAction action)
{
	const auto deadline = GetTickCount64() + 2000;
	while (action.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
	{
		Require(GetTickCount64() < deadline, "Transport readiness test must not hang");
		MSG message;
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
		Sleep(1);
	}
	action.GetResults();
}

void TestReceiverLifetime()
{
	ReceiverLifecycle receiver;
	receiver.Created();
	receiver.OpenFailed();
	Require(!receiver.KeepListening(), "A failed registration must be released");
	receiver.Created();
	receiver.Registered();
	receiver.Opened();
	Require(receiver.TransportReady(), "A registered and opened receiver is ready for audio observation");
	for (int returnHome = 0; returnHome < 5; ++returnHome)
	{
		receiver.TransportClosed();
		Require(receiver.KeepListening() && !receiver.TransportReady(), "Leaving radio range must retain incoming support without claiming an open transport");
		for (int offlineRetry = 0; offlineRetry < 50; ++offlineRetry)
		{
			receiver.OpenFailed();
			Require(receiver.KeepListening() && !receiver.TransportReady(), "Failed offline opens must never unregister a started receiver");
		}
		receiver.Opened();
		Require(receiver.TransportReady(), "An incoming reconnection must reuse the registered receiver");
	}
	receiver.Released();
	Require(!receiver.KeepListening() && !receiver.TransportReady(), "Explicit disable must release both incoming registration and transport");
	receiver.Created();
	receiver.Registered();
	Require(receiver.KeepListening() && !receiver.TransportReady(), "Manual enable must prepare incoming support before claiming transport readiness");
	std::puts("PASS: receiver survives repeated range loss and 250 failed offline opens; incoming reconnect, explicit disable, failed registration, manual enable");
}

struct TestSessionVolume
{
	BOOL muted = FALSE;
	float volume = 0.37f;
	bool denyMute = false;
	bool failFirstRestore = false;
	std::vector<BOOL> writes;
	HRESULT GetMute(BOOL* value) { *value = muted; return S_OK; }
	HRESULT GetMasterVolume(float* value) { *value = volume; return S_OK; }
	HRESULT SetMute(BOOL value, LPCGUID)
	{
		writes.push_back(value);
		if (value && denyMute) return E_ACCESSDENIED;
		if (!value && failFirstRestore) { failFirstRestore = false; return E_FAIL; }
		muted = value;
		return S_OK;
	}
};

void TestSessionActivation()
{
	TestSessionVolume volume;
	int waits = 0;
	Require(ActivateUnmutedPhoneSession(&volume, [&] { ++waits; }) == S_OK, "A new unmuted phone session must be initialized");
	Require(waits == 1 && volume.writes == std::vector<BOOL>({ TRUE, FALSE }) && !volume.muted && volume.volume == 0.37f,
		"Session initialization must restore mute and preserve its volume");
	volume.writes.clear();
	volume.muted = TRUE;
	Require(ActivateUnmutedPhoneSession(&volume, [&] { ++waits; }) == S_FALSE && volume.writes.empty() && waits == 1,
		"A user-muted session must not be unmuted or toggled");
	volume.muted = FALSE;
	volume.volume = 0;
	Require(ActivateUnmutedPhoneSession(&volume, [] {}) == S_FALSE && volume.writes.empty(), "A zero-volume session must be left unchanged");
	volume.volume = 0.37f;
	volume.denyMute = true;
	Require(ActivateUnmutedPhoneSession(&volume, [] {}) == E_ACCESSDENIED && !volume.muted, "A denied activation must leave the original state intact");
	volume.denyMute = false;
	volume.writes.clear();
	try { ActivateUnmutedPhoneSession(&volume, [] { throw std::runtime_error("cancel"); }); }
	catch (const std::runtime_error&) { }
	Require(!volume.muted && volume.writes == std::vector<BOOL>({ TRUE, FALSE }), "An interrupted initialization must still restore mute");
	volume.failFirstRestore = true;
	Require(FAILED(ActivateUnmutedPhoneSession(&volume, [] {})) && !volume.muted, "A failed restore must be reported and restoration retried by the scope guard");
	std::puts("PASS: session activation without transport operations; user mute and zero volume preserved; failure and interruption restore original state");
}

void TestVerifiedInitialization()
{
	SessionInitialization initialization;
	Require(initialization.Due(1000), "A new audio session must initialize promptly");
	initialization.Attempted(1000, S_OK);
	Require(!initialization.verified && !initialization.Due(1100), "S_OK without audio must remain unverified and rate limited");
	Require(initialization.Due(1250), "An early successful activation must be retried when it produces no audio");
	initialization.Attempted(1250, S_OK);
	Require(!initialization.Observe(false, 1.0f) && !initialization.verified, "Invalid meter values cannot verify playback");
	Require(initialization.Due(1200000), "Starting phone playback many minutes after reconnect must still have pending initialization");
	initialization.Attempted(1200000, S_OK);
	Require(!initialization.verified && !initialization.Due(1200999) && initialization.Due(1201000),
		"Idle retries must be rate limited without declaring success or exhausting on silence");
	Require(initialization.Observe(true, 0.2f) && initialization.verified, "Only real phone signal must complete initialization");
	Require(!initialization.Due(2000000) && !initialization.Observe(true, 0.2f), "Verified playback must stop all activation pulses and duplicate completions");
	Require(!initialization.Observe(true, 0) && !initialization.Due(3000000), "A quiet passage in verified playback must not restart initialization");
	SessionInitialization failed;
	for (uint64_t now = 0; now < 6000; now += 2000) failed.Attempted(now, E_ACCESSDENIED);
	Require(!failed.Due(90000), "Persistent API errors must stop instead of changing mute indefinitely");
	std::puts("PASS: early S_OK remains pending, delayed playback, rate limiting, invalid meters, real-signal verification, no pulses after success, API failure bound");
}

void TestPersistentAudioWorker()
{
	std::vector<DWORD> threads;
	std::vector<APTTYPE> apartments;
	AudioSurveyWorker worker(nullptr, [&](const std::wstring&, std::shared_ptr<AudioActivationContext>) {
		APTTYPE type = APTTYPE_CURRENT;
		APTTYPEQUALIFIER qualifier{};
		AudioSurvey survey;
		survey.error = CoGetApartmentType(&type, &qualifier);
		threads.push_back(GetCurrentThreadId());
		apartments.push_back(type);
		return survey;
	});
	auto context = std::make_shared<AudioActivationContext>();
	auto awaitResult = [&]() {
		const auto deadline = GetTickCount64() + 2000;
		for (;;)
		{
			if (auto result = worker.TakeResult()) return result;
			Require(GetTickCount64() < deadline, "The persistent audio worker must not hang");
			Sleep(1);
		}
	};
	worker.Request({ 1, 3, L"test", context });
	auto first = awaitResult();
	Require(SUCCEEDED(first->survey.error) && first->job.serial == 1, "Audio worker must publish the matching result");
	Sleep(30);
	worker.Request({ 2, 3, L"test", context });
	auto second = awaitResult();
	Require(SUCCEEDED(second->survey.error) && second->job.serial == 2 && threads.size() == 2 && threads[0] == threads[1],
		"Both surveys must use one continuously owned native thread");
	Require(apartments[0] == APTTYPE_MTA && apartments[1] == APTTYPE_MTA, "Core Audio must retain a live MTA between surveys");
	context->current = false;
	worker.Request({ 3, 3, L"test", context });
	auto canceled = awaitResult();
	Require(canceled->survey.error == E_ABORT && threads.size() == 2, "An obsolete connection must not execute queued audio mutations");
	worker.Stop();
	worker.Stop();
	std::puts("PASS: persistent MTA and thread identity across surveys, result identity, canceled job suppression, safe repeated shutdown");
}

void TestTransportReadiness()
{
	auto signal = std::make_shared<ConnectionStateSignal>();
	bool opened = false;
	bool current = true;
	auto operation = AwaitTransportReadiness(signal, [&] { return opened; }, [&] { return current; });
	Require(operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started, "An accepted request with initial Closed state must wait, not dispose or finish");
	SetEvent(signal->event.get());
	MSG message;
	for (int i = 0; i < 20; ++i)
	{
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
		Sleep(1);
	}
	if (operation.Status() != winrt::Windows::Foundation::AsyncStatus::Started)
		std::printf("READINESS unexpected status=%d error=0x%08X\n", static_cast<int>(operation.Status()), static_cast<uint32_t>(operation.ErrorCode()));
	Require(operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started, "An initial Closed notification must not finish the open operation");
	opened = true;
	SetEvent(signal->event.get());
	PumpTransportTest(operation);
	operation = AwaitTransportReadiness(signal, [&] { return opened; }, [&] { return current; });
	Require(operation.Status() == winrt::Windows::Foundation::AsyncStatus::Completed, "Already Opened transport must return immediately without a fixed delay");
	opened = false;
	operation = AwaitTransportReadiness(signal, [&] { return opened; }, [&] { return current; });
	current = false;
	SetEvent(signal->event.get());
	PumpTransportTest(operation);
	current = true;
	bool timedOut = false;
	try { PumpTransportTest(AwaitTransportReadiness(signal, [&] { return opened; }, [&] { return current; }, std::chrono::milliseconds(20))); }
	catch (const winrt::hresult_error& error) { timedOut = error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
	Require(timedOut, "A transport that never opens must still have a real connection timeout");
	std::puts("PASS: asynchronous Closed-to-Opened ordering, early Closed event, immediate ready path, cancellation, real transport timeout");
}

int wmain()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	winrt::init_apartment(winrt::apartment_type::single_threaded);
	g_hInst = GetModuleHandleW(nullptr);
	const auto path = GetModuleFsPath(g_hInst).remove_filename() / CONFIG_NAME;
	const auto testKey = std::wstring(L"Software\\AudioPlaybackConnector.SettingsTests.") + std::to_wstring(GetCurrentProcessId());
	try
	{
		TestAudioHealthPolicy();
		TestTransportReadiness();
		TestReceiverLifetime();
		TestSessionActivation();
		TestVerifiedInitialization();
		TestPersistentAudioWorker();
		Require(!fs::exists(path), "Tests require a clean, dedicated output directory");
		LoadSettings();
		Require(g_connectionEnabled && g_targetDeviceId.empty(), "Missing configuration must use defaults");
		g_connectionEnabled = false;
		g_targetDeviceId = L"device-with-quote-\"-and-\\-slash";
		g_targetDeviceName = L"\u5c0f\u7c73 14";
		SaveSettings();
		DefaultSettings();
		LoadSettings();
		Require(!g_connectionEnabled && g_targetDeviceId == L"device-with-quote-\"-and-\\-slash" && g_targetDeviceName == L"\u5c0f\u7c73 14", "Paused state and Unicode phone selection must survive restart");
		g_connectionEnabled = true;
		SaveSettings();
		DefaultSettings();
		LoadSettings();
		Require(g_connectionEnabled && !g_targetDeviceId.empty(), "Re-enabling must preserve the remembered phone");
		Require(!fs::exists(path.wstring() + L".tmp"), "Atomic save must not leave a pending temporary file");
		WriteConfig("{\"reconnect\":true,\"lastDevices\":[\"legacy-phone\"]}");
		LoadSettings();
		Require(g_connectionEnabled && g_targetDeviceId == L"legacy-phone", "Previous-version device lists must migrate");
		std::puts("Checking corrupt configuration recovery...");
		WriteConfig("invalid JSON");
		LoadSettings();
		Require(g_connectionEnabled && g_targetDeviceId.empty(), "Corrupt configuration must recover to defaults");
		std::puts("PASS: defaults, pause persistence, Unicode/escaping, resume persistence, atomic replacement, legacy migration, corrupt configuration");

		HKEY rawKey = nullptr;
		std::puts("Checking isolated autostart registration...");
		THROW_IF_WIN32_ERROR(RegCreateKeyExW(HKEY_CURRENT_USER, testKey.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &rawKey, nullptr));
		wil::unique_hkey root(rawKey);
		std::puts("  Read initial state");
		Require(!IsStartupEnabled(root.get()), "Autostart must default to off");
		std::puts("  Disable absent entry");
		SetStartupEnabled(false, root.get());
		std::puts("  Enable entry");
		SetStartupEnabled(true, root.get());
		std::puts("  Read enabled entry");
		Require(IsStartupEnabled(root.get()), "Enabling must register the current executable");
		const auto command = StartupCommand();
		Require(command.front() == L'\"' && command.back() == L'\"', "Executable paths must be quoted");
		SetStartupEnabled(true, root.get());
		std::puts("  Disable existing entry");
		Require(IsStartupEnabled(root.get()), "Enabling twice must remain enabled");
		SetStartupEnabled(false, root.get());
		std::puts("  Disabled once");
		SetStartupEnabled(false, root.get());
		std::puts("  Disabled twice");
		Require(!IsStartupEnabled(root.get()), "Disabling twice must remain disabled");
		std::puts("  Closing isolated root");
		root.reset();
		CleanupTestRegistry(testKey);
		DeleteFileW(path.c_str());
		std::puts("PASS: isolated autostart enable/disable, idempotence, executable quoting");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
	}
	catch (const winrt::hresult_error& error)
	{
		std::fprintf(stderr, "FAIL: HRESULT 0x%08X\n", static_cast<uint32_t>(error.code()));
	}
	CleanupTestRegistry(testKey);
	DeleteFileW(path.c_str());
	return 1;
}
