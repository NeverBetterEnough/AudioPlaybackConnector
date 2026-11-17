#pragma once

#include <atomic>
#include <unordered_set>

// An API acknowledgement is not evidence of audio delivery. Remain pending
// through an idle phone and early successful pulses, until real audio arrives.
class SessionInitialization
{
public:
	bool verified = false;
	unsigned attempts = 0;
	unsigned failures = 0;
	uint64_t nextAttemptAt = 0;
	bool Observe(bool meterValid, float peak)
	{
		if (!meterValid || peak <= 0.0001f || verified) return false;
		verified = true;
		return true;
	}
	bool Due(uint64_t now) const { return !verified && failures < 3 && now >= nextAttemptAt; }
	void Attempted(uint64_t now, HRESULT result)
	{
		++attempts;
		failures = FAILED(result) ? failures + 1 : 0;
		nextAttemptAt = now + (FAILED(result) ? 2000 : (attempts < 3 ? 250 : 1000));
	}
};

struct AudioActivationContext
{
	std::atomic<bool> current{ true };
	uint64_t epoch = 0;
	std::unordered_map<std::wstring, SessionInitialization> sessions; // persistent MTA worker only
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
struct AudioSessionInbox
{
	std::mutex mutex;
	std::vector<winrt::com_ptr<IAudioSessionControl>> created;
};

struct AudioSessionReadyEvents : winrt::implements<AudioSessionReadyEvents, IAudioSessionNotification, IAudioSessionEvents>
{
	AudioSessionReadyEvents(HWND window, std::shared_ptr<AudioSessionInbox> inbox) : window_(window), inbox_(std::move(inbox)) { }
	HRESULT __stdcall OnSessionCreated(IAudioSessionControl* session) noexcept override
	{
		if (!session) return S_OK;
		try
		{
			winrt::com_ptr<IAudioSessionControl> reference;
			reference.copy_from(session);
			std::lock_guard<std::mutex> guard(inbox_->mutex);
			// Keep the callback's reference: enumeration can lag session creation.
			if (inbox_->created.size() >= 64) inbox_->created.erase(inbox_->created.begin());
			inbox_->created.push_back(std::move(reference));
		}
		catch (...) { }
		Notify(1, 0); return S_OK;
	}
	HRESULT __stdcall OnStateChanged(AudioSessionState state) noexcept override { Notify(2, state); return S_OK; }
	HRESULT __stdcall OnSessionDisconnected(AudioSessionDisconnectReason reason) noexcept override { Notify(3, reason); return S_OK; }
	HRESULT __stdcall OnDisplayNameChanged(LPCWSTR, LPCGUID) noexcept override { return S_OK; }
	HRESULT __stdcall OnIconPathChanged(LPCWSTR, LPCGUID) noexcept override { return S_OK; }
	HRESULT __stdcall OnSimpleVolumeChanged(float, BOOL, LPCGUID context) noexcept override
	{
		if (!context || *context != AUDIO_ACTIVATION_EVENT) Notify(4, 0);
		return S_OK;
	}
	HRESULT __stdcall OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) noexcept override { return S_OK; }
	HRESULT __stdcall OnGroupingParamChanged(LPCGUID, LPCGUID) noexcept override { return S_OK; }
private:
	void Notify(WPARAM reason, LPARAM state) const { PostMessageW(window_, WM_AUDIO_SESSION_READY, reason, state); }
	HWND window_;
	std::shared_ptr<AudioSessionInbox> inbox_;
};

class PhoneAudioNotifications
{
public:
	PhoneAudioNotifications(IAudioSessionManager2* manager, HWND window) :
		inbox_(std::make_shared<AudioSessionInbox>()), events_(winrt::make_self<AudioSessionReadyEvents>(window, inbox_))
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
	std::vector<winrt::com_ptr<IAudioSessionControl>> KnownSessions()
	{
		std::vector<winrt::com_ptr<IAudioSessionControl>> result;
		{
			std::lock_guard<std::mutex> guard(inbox_->mutex);
			result.swap(inbox_->created);
		}
		for (const auto& session : sessions_) result.push_back(session.second);
		return result;
	}
private:
	winrt::com_ptr<IAudioSessionManager2> manager_;
	std::shared_ptr<AudioSessionInbox> inbox_;
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

std::vector<winrt::com_ptr<IAudioSessionControl>> NotifiedPhoneSessions()
{
	std::lock_guard<std::mutex> guard(g_audioNotificationsMutex);
	return g_audioNotifications ? g_audioNotifications->KnownSessions() : std::vector<winrt::com_ptr<IAudioSessionControl>>{};
}
