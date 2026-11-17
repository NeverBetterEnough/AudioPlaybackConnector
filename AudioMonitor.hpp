#pragma once

bool g_audioSamplePending = false;
bool g_audioResampleRequested = false;
uint64_t g_audioSampleSerial = 0;
uint64_t g_audioOpenedAt = 0;
uint64_t g_lastHealthLogAt = 0;
uint64_t g_lastRouteRecoveryAt = 0;
AudioHealthKind g_previousAudioHealth = AudioHealthKind::Disconnected;
std::wstring g_lastDefaultOutput;
bool g_audioRepairNeedsAttention = false;
bool g_resumeSignalPending = false;
std::shared_ptr<AudioActivationContext> g_audioActivation;
uint64_t g_audioActivationEpoch = 0;
std::unique_ptr<AudioSurveyWorker> g_audioWorker;

void StartAudioWorker()
{
	g_audioWorker = std::make_unique<AudioSurveyWorker>(g_hWnd);
}

void StopAudioWorker()
{
	if (g_audioWorker) g_audioWorker->Stop();
	g_audioWorker.reset();
}

void InvalidateAudioActivation()
{
	if (g_audioActivation) g_audioActivation->current = false;
	g_audioActivation.reset();
	KillTimer(g_hWnd, AUDIO_INITIALIZATION_TIMER);
}

void OnAudioConnectionOpened()
{
	InvalidateAudioActivation();
	g_audioActivation = std::make_shared<AudioActivationContext>();
	g_audioActivation->epoch = ++g_audioActivationEpoch;
	g_audioOpenedAt = GetTickCount64();
	g_audioRepairNeedsAttention = false;
	g_lastDefaultOutput.clear();
	g_audioRepair.OnConnected(g_audioOpenedAt);
	DiagnosticLog(L"connection open; audio initialization pending until real signal; epoch=" + std::to_wstring(g_audioActivation->epoch));
	PostMessageW(g_hWnd, WM_AUDIO_SESSION_READY, 0, 0);
	if (g_repairOnStartup)
	{
		g_repairOnStartup = false;
		BeginAudioRepair();
	}
	else if (g_verifyAfterResume)
	{
		g_verifyAfterResume = false;
		g_resumeSignalPending = true;
		DiagnosticLog(L"resume transport ready; initialization pending until real signal; no automatic Bluetooth teardown");
	}
}

void RebuildAudioConnection(const std::wstring& reason)
{
	DiagnosticLog(L"recovery reconnect reason=" + reason);
	DisconnectConnections();
	SetConnectionStatus(_(L"Rebuilding the phone audio connection"));
	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
}

void BeginAudioRepair()
{
	if (!g_connectionEnabled)
	{
		SetConnectionStatus(_(L"Enable the phone connection before checking sound"));
		return;
	}
	g_audioRepair.Request(GetTickCount64());
	InvalidateAudioActivation();
	g_audioActivation = std::make_shared<AudioActivationContext>();
	g_audioActivation->epoch = ++g_audioActivationEpoch;
	PostMessageW(g_hWnd, WM_AUDIO_SESSION_READY, 0, 0);
	g_audioRepairNeedsAttention = false;
	DiagnosticLog(L"repair requested by user; one reconnect permitted; deadline_ms=90000");
	SetConnectionStatus(_(L"Checking phone audio; keep music playing on the phone"));
	if (!g_receiverLifecycle.TransportReady() && !g_connecting)
		PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);
}

std::wstring AudioHealthMessage(AudioHealthKind kind)
{
	switch (kind)
	{
	case AudioHealthKind::Signal: return _(L"Receiving phone audio");
	case AudioHealthKind::OutputUnavailable: return _(L"No available PC audio output");
	case AudioHealthKind::Muted: return _(L"Phone audio or PC output is muted; check the volume");
	case AudioHealthKind::WrongOutput: return _(L"Phone audio is routed to a different output");
	case AudioHealthKind::Unmeasurable: return _(L"Connected; audio signal measurement is unavailable");
	case AudioHealthKind::SessionMissing: return _(L"Connected; phone audio session has not appeared");
	default: return _(L"Connected; waiting for phone audio");
	}
}

void ApplyAudioSurvey(AudioSurvey survey)
{
	const auto now = GetTickCount64();
	KillTimer(g_hWnd, AUDIO_INITIALIZATION_TIMER);
	if (survey.nextInitializationMs && g_audioActivation && g_audioActivation->current)
		SetTimer(g_hWnd, AUDIO_INITIALIZATION_TIMER, (std::max)(survey.nextInitializationMs, UINT(50)), nullptr);
	survey.facts.connected = g_receiverLifecycle.TransportReady() && !g_connecting;
	const auto kind = FAILED(survey.error) && !survey.facts.outputAvailable && survey.error != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ?
		AudioHealthKind::Unmeasurable : ClassifyAudioHealth(survey.facts);
	if (kind == AudioHealthKind::Signal && g_resumeSignalPending)
	{
		g_resumeSignalPending = false;
		DiagnosticLog(L"resume verification passed; phone signal confirmed; no Bluetooth teardown");
	}
	// Do not reset backoff for an OpenAsync success that immediately closes.
	if (kind == AudioHealthKind::Signal || now - g_audioOpenedAt >= 30000)
		g_retryDelay = 5000;
	if (kind != g_previousAudioHealth || now - g_lastHealthLogAt >= 10000)
	{
		std::wostringstream log;
		log << L"audio health=" << AudioHealthName(kind) << L" output=" << survey.defaultOutputName
			<< L" output_volume=" << survey.outputVolume << L" output_mute=" << survey.outputMuted
			<< L" phone_sessions=" << survey.matchingSessions << L" on_default_output=" << survey.facts.sessionOnDefaultOutput
			<< L" phone_volume=" << survey.phoneVolume << L" muted=" << survey.facts.muted
			<< L" valid_meter_samples=" << survey.validMeters << L" peak=" << std::fixed << std::setprecision(6) << survey.facts.peak
			<< L" hardware_meter_flags=" << survey.hardwareMeterSupport << L" hresult=" << HResultText(survey.error);
		DiagnosticLog(log.str());
		g_lastHealthLogAt = now;
	}
	g_previousAudioHealth = kind;
	const bool outputChanged = !g_lastDefaultOutput.empty() && !survey.defaultOutputId.empty() && g_lastDefaultOutput != survey.defaultOutputId;
	g_lastDefaultOutput = survey.defaultOutputId;
	if (outputChanged && (g_lastRouteRecoveryAt == 0 || now - g_lastRouteRecoveryAt > 30000) && now - g_audioOpenedAt > 5000)
	{
		g_lastRouteRecoveryAt = now;
		RebuildAudioConnection(L"default output changed");
		return;
	}
	const auto decision = g_audioRepair.Observe(kind, now);
	if (decision == AudioRepairDecision::Reconnect)
	{
		RebuildAudioConnection(L"user requested silent-stream repair");
		return;
	}
	if (decision == AudioRepairDecision::NeedsUserAction)
	{
		g_audioRepairNeedsAttention = true;
		DiagnosticLog(L"repair stopped: no verified signal; phone playback/volume/routing or driver investigation required");
		SetConnectionStatus(kind == AudioHealthKind::Muted || kind == AudioHealthKind::OutputUnavailable ? AudioHealthMessage(kind) :
			_(L"No phone audio detected. Check phone media volume and output, then pause and play again."));
		return;
	}
	if (g_audioRepair.Active())
		SetConnectionStatus(_(L"Checking phone audio; keep music playing on the phone"));
	else if (g_audioRepairNeedsAttention && kind != AudioHealthKind::Signal)
		return;
	else if (decision == AudioRepairDecision::SignalDetected || now - g_audioOpenedAt >= 5000)
	{
		g_audioRepairNeedsAttention = false;
		if (decision == AudioRepairDecision::SignalDetected)
			DiagnosticLog(L"repair verification passed; phone signal confirmed");
		SetConnectionStatus(AudioHealthMessage(kind));
	}
}

void PollAudioHealth()
{
	if (g_stopping || !g_connectionEnabled || g_connecting || !g_receiverLifecycle.TransportReady())
	{
		if (g_audioRepair.Observe(AudioHealthKind::Disconnected, GetTickCount64()) == AudioRepairDecision::NeedsUserAction)
			SetConnectionStatus(_(L"Could not establish the phone audio connection. See the diagnostic log."));
		return;
	}
	if (g_audioSamplePending)
	{
		g_audioResampleRequested = true;
		return;
	}
	g_audioSamplePending = true;
	if (!g_audioWorker) { g_audioSamplePending = false; return; }
	g_audioWorker->Request({ ++g_audioSampleSerial, g_connectionGeneration, g_targetDeviceName, g_audioActivation });
}

void ReceiveAudioSurvey()
{
	if (!g_audioWorker) return;
	auto result = g_audioWorker->TakeResult();
	if (!result) return;
	if (result->job.serial == g_audioSampleSerial) g_audioSamplePending = false;
	if (IsCurrentConnectionAttempt(result->job.generation) && result->job.activation == g_audioActivation &&
		!g_connecting && g_receiverLifecycle.TransportReady())
		ApplyAudioSurvey(std::move(result->survey));
	else if (!g_stopping && g_receiverLifecycle.TransportReady()) g_audioResampleRequested = true;
	if (g_audioResampleRequested)
	{
		g_audioResampleRequested = false;
		PostMessageW(g_hWnd, WM_AUDIO_SESSION_READY, 0, 0);
	}
}
