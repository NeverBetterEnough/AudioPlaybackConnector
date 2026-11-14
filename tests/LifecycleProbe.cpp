#include "../pch.h"
#include "../AudioPlaybackConnector.h"
#include <cstdio>
#include <clocale>
#include <memory>
#include <atomic>
#include <winrt/Windows.Media.Control.h>

void PumpFor(DWORD milliseconds)
{
	const auto until = GetTickCount64() + milliseconds;
	do
	{
		MSG message;
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		Sleep(5);
	} while (GetTickCount64() < until);
}

template<typename Operation> void WaitForOperation(Operation operation)
{
	const auto started = GetTickCount64();
	while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
	{
		if (GetTickCount64() - started > 30000) { operation.Cancel(); winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); }
		PumpFor(5);
	}
}

struct ProbeContext
{
	int cycle;
	ULONGLONG started;
};

int wmain(int argc, wchar_t** argv)
{
	std::setlocale(LC_ALL, ".UTF8");
	setvbuf(stdout, nullptr, _IONBF, 0);
	const bool sta = argc > 1 && std::wstring_view(argv[1]) == L"sta";
	winrt::init_apartment(sta ? winrt::apartment_type::single_threaded : winrt::apartment_type::multi_threaded);
	std::wprintf(L"PROBE apartment=%ls auto_repair=disabled synthetic_state_check=disabled\n", sta ? L"STA" : L"MTA");
	try
	{
		auto enumerate = DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector());
		WaitForOperation(enumerate);
		const auto devices = enumerate.GetResults();
		if (devices.Size() != 1) { std::wprintf(L"Expected one audio source, found %u\n", devices.Size()); return 2; }
		const auto device = devices.GetAt(0);
		int passed = 0;
		for (int cycle = 1; cycle <= 3; ++cycle)
		{
			auto context = std::make_shared<ProbeContext>(ProbeContext{cycle, GetTickCount64()});
			auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
			if (!connection) winrt::throw_hresult(E_FAIL);
			const auto token = connection.StateChanged([context](const auto& sender, const auto&) {
				try { std::wprintf(L"EVENT cycle=%d elapsed_ms=%llu state=%d\n", context->cycle, GetTickCount64() - context->started, static_cast<int>(sender.State())); }
				catch (...) { std::wprintf(L"EVENT cycle=%d state_query_failed\n", context->cycle); }
			});
			auto start = connection.StartAsync();
			WaitForOperation(start);
			start.GetResults();
			std::wprintf(L"START cycle=%d elapsed_ms=%llu state=%d\n",cycle,GetTickCount64()-context->started,static_cast<int>(connection.State()));
			auto open = connection.OpenAsync();
			WaitForOperation(open);
			const auto result = open.GetResults();
			std::wprintf(L"OPEN cycle=%d elapsed_ms=%llu status=%d state=%d hr=0x%08X\n",cycle,GetTickCount64()-context->started,static_cast<int>(result.Status()),static_cast<int>(connection.State()),static_cast<uint32_t>(result.ExtendedError()));
			if (cycle == 1)
			{
				try
				{
					auto request = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
					WaitForOperation(request);
					for (const auto& media : request.GetResults().GetSessions())
					{
						const auto playback = media.GetPlaybackInfo();
						std::wprintf(L"MEDIA source=%ls playback_state=%d play_enabled=%d\n",media.SourceAppUserModelId().c_str(),static_cast<int>(playback.PlaybackStatus()),playback.Controls().IsPlayEnabled());
					}
				}
				catch(const winrt::hresult_error& error) { std::wprintf(L"MEDIA unavailable hr=0x%08X\n",static_cast<uint32_t>(error.code())); }
			}
			bool signal = false;
			const auto openedAt = GetTickCount64();
			while (GetTickCount64() - openedAt < 3500)
			{
				auto survey = SurveyPhoneAudio(std::wstring(device.Name()));
				if (survey.facts.peak > 0.0001f)
				{
					std::wprintf(L"SIGNAL cycle=%d elapsed_ms=%llu peak=%.6f state=%d\n",cycle,GetTickCount64()-context->started,survey.facts.peak,static_cast<int>(connection.State()));
					signal = true; ++passed; break;
				}
				PumpFor(25);
			}
			if (!signal)
			{
				std::wprintf(L"NO_SIGNAL cycle=%d state=%d; reopening same object without Close\n",cycle,static_cast<int>(connection.State()));
				auto reopen = connection.OpenAsync();
				WaitForOperation(reopen);
				const auto second = reopen.GetResults();
				std::wprintf(L"REOPEN cycle=%d elapsed_ms=%llu status=%d state=%d\n",cycle,GetTickCount64()-context->started,static_cast<int>(second.Status()),static_cast<int>(connection.State()));
				const auto reopenedAt = GetTickCount64();
				while (GetTickCount64() - reopenedAt < 2500)
				{
					const auto survey = SurveyPhoneAudio(std::wstring(device.Name()));
					if (survey.facts.peak > 0.0001f) { std::wprintf(L"SIGNAL_AFTER_REOPEN cycle=%d elapsed_ms=%llu peak=%.6f\n",cycle,GetTickCount64()-context->started,survey.facts.peak); break; }
					PumpFor(25);
				}
			}
			connection.StateChanged(token);
			std::wprintf(L"CLOSE cycle=%d elapsed_ms=%llu\n",cycle,GetTickCount64()-context->started);
			connection.Close();
			connection = nullptr;
			PumpFor(100);
		}
		std::wprintf(L"RESULT apartment=%ls first_open_signal_cycles=%d/3\n",sta ? L"STA" : L"MTA",passed);
		return 0;
	}
	catch (const winrt::hresult_error& error) { std::wprintf(L"PROBE_FAILED hr=0x%08X\n",static_cast<uint32_t>(error.code())); return 1; }
}
