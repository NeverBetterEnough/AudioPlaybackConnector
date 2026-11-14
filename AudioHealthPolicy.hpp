#pragma once
#include <cstdint>

enum class AudioHealthKind { Disconnected, OutputUnavailable, SessionMissing, Unmeasurable, Muted, Quiet, Signal, WrongOutput };
enum class AudioRepairDecision { None, Reconnect, SignalDetected, NeedsUserAction };

inline const wchar_t* AudioHealthName(AudioHealthKind kind)
{
	switch (kind)
	{
	case AudioHealthKind::Disconnected: return L"disconnected";
	case AudioHealthKind::OutputUnavailable: return L"output_unavailable";
	case AudioHealthKind::SessionMissing: return L"phone_session_missing";
	case AudioHealthKind::Unmeasurable: return L"meter_unavailable";
	case AudioHealthKind::Muted: return L"muted";
	case AudioHealthKind::Quiet: return L"quiet_not_proof_of_fault";
	case AudioHealthKind::Signal: return L"phone_signal_detected";
	default: return L"phone_on_other_output";
	}
}

struct AudioHealthFacts
{
	bool connected = false;
	bool outputAvailable = false;
	bool sessionFound = false;
	bool sessionOnDefaultOutput = false;
	bool meterValid = false;
	bool muted = false;
	float peak = 0;
};

inline AudioHealthKind ClassifyAudioHealth(const AudioHealthFacts& facts)
{
	if (!facts.connected) return AudioHealthKind::Disconnected;
	if (!facts.outputAvailable) return AudioHealthKind::OutputUnavailable;
	if (!facts.sessionFound) return AudioHealthKind::SessionMissing;
	if (!facts.sessionOnDefaultOutput) return AudioHealthKind::WrongOutput;
	if (facts.muted) return AudioHealthKind::Muted;
	if (!facts.meterValid) return AudioHealthKind::Unmeasurable;
	return facts.peak > 0.0001f ? AudioHealthKind::Signal : AudioHealthKind::Quiet;
}

// Silence alone is never a fault. A user-requested repair gets one reconnect,
// then stops with guidance instead of repeatedly interrupting a paused phone.
class AudioRepairPolicy
{
	bool active = false;
	bool reconnected = false;
	uint64_t requestedAt = 0;
	uint64_t probeStartedAt = 0;
	unsigned observations = 0;
public:
	bool Active() const { return active; }
	void Request(uint64_t now)
	{
		active = true; reconnected = false; requestedAt = now;
		OnConnected(now);
	}
	void Cancel() { active = false; }
	void OnConnected(uint64_t now) { probeStartedAt = now; observations = 0; }
	AudioRepairDecision Observe(AudioHealthKind kind, uint64_t now)
	{
		if (!active) return AudioRepairDecision::None;
		if (now - requestedAt >= 90000) { active = false; return AudioRepairDecision::NeedsUserAction; }
		if (kind == AudioHealthKind::Disconnected) return AudioRepairDecision::None;
		if (kind == AudioHealthKind::Signal) { active = false; return AudioRepairDecision::SignalDetected; }
		if (kind == AudioHealthKind::Muted || kind == AudioHealthKind::OutputUnavailable)
		{ active = false; return AudioRepairDecision::NeedsUserAction; }
		if (++observations < 5 || now - probeStartedAt < 10000) return AudioRepairDecision::None;
		if (kind == AudioHealthKind::Unmeasurable || reconnected)
		{ active = false; return AudioRepairDecision::NeedsUserAction; }
		reconnected = true;
		OnConnected(now);
		return AudioRepairDecision::Reconnect;
	}
};
