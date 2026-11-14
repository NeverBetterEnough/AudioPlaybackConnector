#pragma once

#include <atomic>
#include <unordered_set>

// A new context belongs to one Opened transport. Native callbacks and duplicate
// health polls may observe the same session, but must only initialize it once.
struct AudioActivationContext
{
	std::atomic<bool> current{ true };
	std::unordered_set<std::wstring> initializedSessions; // MTA survey worker only
};

constexpr GUID AUDIO_ACTIVATION_EVENT{ 0x638bf60a, 0x7df2, 0x49a4, {0x9d, 0xb1, 0x8d, 0xb4, 0xb0, 0x57, 0x12, 0x81} };

// Windows' A2DP capture monitor can report Active/Opened but deliver no frames
// after reconnecting. A mute edge re-arms that session without closing A2DP.
// Never touch a session the user has muted or set to zero volume.
template<typename Volume, typename Settle>
HRESULT ActivateUnmutedPhoneSession(Volume* volume, Settle settle)
{
	BOOL originalMute = FALSE;
	float originalVolume = 0;
	auto result = volume->GetMute(&originalMute);
	if (FAILED(result)) return result;
	result = volume->GetMasterVolume(&originalVolume);
	if (FAILED(result)) return result;
	if (originalMute || originalVolume == 0) return S_FALSE;
	result = volume->SetMute(TRUE, &AUDIO_ACTIVATION_EVENT);
	if (FAILED(result)) return result;
	struct Restore
	{
		Volume* volume;
		BOOL originalMute;
		bool restored = false;
		~Restore() { if (!restored) volume->SetMute(originalMute, &AUDIO_ACTIVATION_EVENT); }
	} restore{ volume, originalMute };
	settle();
	result = volume->SetMute(originalMute, &AUDIO_ACTIVATION_EVENT);
	restore.restored = SUCCEEDED(result);
	return result;
}

// Notifications only enqueue work. Core Audio forbids blocking or registering
// other callbacks from inside its notification callbacks.
struct AudioSessionReadyEvents : winrt::implements<AudioSessionReadyEvents, IAudioSessionNotification, IAudioSessionEvents>
{
	explicit AudioSessionReadyEvents(HWND window) : window_(window) { }
	HRESULT __stdcall OnSessionCreated(IAudioSessionControl*) noexcept override { Notify(); return S_OK; }
	HRESULT __stdcall OnStateChanged(AudioSessionState) noexcept override { Notify(); return S_OK; }
	HRESULT __stdcall OnSessionDisconnected(AudioSessionDisconnectReason) noexcept override { Notify(); return S_OK; }
	HRESULT __stdcall OnDisplayNameChanged(LPCWSTR, LPCGUID) noexcept override { return S_OK; }
	HRESULT __stdcall OnIconPathChanged(LPCWSTR, LPCGUID) noexcept override { return S_OK; }
	HRESULT __stdcall OnSimpleVolumeChanged(float, BOOL, LPCGUID) noexcept override { return S_OK; }
	HRESULT __stdcall OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) noexcept override { return S_OK; }
	HRESULT __stdcall OnGroupingParamChanged(LPCGUID, LPCGUID) noexcept override { return S_OK; }
private:
	void Notify() const { PostMessageW(window_, WM_AUDIO_SESSION_READY, 0, 0); }
	HWND window_;
};

class PhoneAudioNotifications
{
public:
	PhoneAudioNotifications(IAudioSessionManager2* manager, HWND window) : events_(winrt::make_self<AudioSessionReadyEvents>(window))
	{
		manager_.copy_from(manager);
		winrt::check_hresult(manager_->RegisterSessionNotification(events_.get()));
	}
	~PhoneAudioNotifications()
	{
		manager_->UnregisterSessionNotification(events_.get());
		for (const auto& session : sessions_) session.second->UnregisterAudioSessionNotification(events_.get());
	}
	void Observe(const std::wstring& instance, IAudioSessionControl* session)
	{
		if (sessions_.count(instance)) return;
		winrt::com_ptr<IAudioSessionControl> reference;
		reference.copy_from(session);
		if (SUCCEEDED(session->RegisterAudioSessionNotification(events_.get()))) sessions_.emplace(instance, std::move(reference));
	}
	void Prune()
	{
		for (auto it = sessions_.begin(); it != sessions_.end();)
		{
			AudioSessionState state = AudioSessionStateExpired;
			if (FAILED(it->second->GetState(&state)) || state == AudioSessionStateExpired)
			{
				it->second->UnregisterAudioSessionNotification(events_.get());
				it = sessions_.erase(it);
			}
			else ++it;
		}
	}
private:
	winrt::com_ptr<IAudioSessionManager2> manager_;
	winrt::com_ptr<AudioSessionReadyEvents> events_;
	std::unordered_map<std::wstring, winrt::com_ptr<IAudioSessionControl>> sessions_;
};

std::mutex g_audioNotificationsMutex;
std::unique_ptr<PhoneAudioNotifications> g_audioNotifications;
std::wstring g_notificationOutput, g_notificationPhone;

void StopAudioNotifications()
{
	std::lock_guard<std::mutex> guard(g_audioNotificationsMutex);
	g_audioNotifications.reset();
	g_notificationOutput.clear();
	g_notificationPhone.clear();
}

void ObservePhoneSession(IAudioSessionManager2* manager, IAudioSessionControl* session,
	const std::wstring& output, const std::wstring& phone, const std::wstring& instance)
{
	std::lock_guard<std::mutex> guard(g_audioNotificationsMutex);
	try
	{
		if (!g_audioNotifications || g_notificationOutput != output || g_notificationPhone != phone)
		{
			g_audioNotifications.reset();
			g_audioNotifications = std::make_unique<PhoneAudioNotifications>(manager, g_hWnd);
			g_notificationOutput = output;
			g_notificationPhone = phone;
		}
		g_audioNotifications->Prune();
		if (session) g_audioNotifications->Observe(instance, session);
	}
	catch (const winrt::hresult_error& error) { DiagnosticLog(L"audio session notifications unavailable; timer fallback hresult=" + HResultText(error.code())); }
}
