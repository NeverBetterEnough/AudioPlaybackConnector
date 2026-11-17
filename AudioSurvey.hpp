#pragma once

struct AudioSurvey
{
	AudioHealthFacts facts;
	HRESULT error = S_OK;
	std::wstring defaultOutputId;
	std::wstring defaultOutputName;
	float outputVolume = -1;
	bool outputMuted = false;
	float phoneVolume = -1;
	unsigned matchingSessions = 0;
	unsigned validMeters = 0;
	DWORD hardwareMeterSupport = 0;
	UINT nextInitializationMs = 0;
	unsigned pendingInitializations = 0;
};

std::wstring AudioDeviceId(IMMDevice* device)
{
	wil::unique_cotaskmem_string id;
	if (FAILED(device->GetId(id.put()))) return {};
	return id.get();
}

std::wstring AudioDeviceName(IMMDevice* device)
{
	winrt::com_ptr<IPropertyStore> properties;
	if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) return {};
	PROPVARIANT value{};
	std::wstring name;
	if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
		name = value.pwszVal;
	PropVariantClear(&value);
	return name;
}

bool IsPhoneAudioSession(IAudioSessionControl* session, const std::wstring& phoneName)
{
	if (phoneName.empty()) return false;
	wil::unique_cotaskmem_string rawName;
	if (FAILED(session->GetDisplayName(rawName.put())) || !rawName) return false;
	std::wstring name(rawName.get());
	return name.find(phoneName) != std::wstring::npos && name.find(L"A2DP SNK") != std::wstring::npos;
}

AudioSurvey SurveyPhoneAudio(const std::wstring& phoneName, std::shared_ptr<AudioActivationContext> activation = {})
{
	AudioSurvey survey;
	winrt::com_ptr<IMMDeviceEnumerator> enumerator;
	survey.error = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
		__uuidof(IMMDeviceEnumerator), enumerator.put_void());
	if (FAILED(survey.error)) return survey;
	winrt::com_ptr<IMMDevice> defaultOutput;
	survey.error = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, defaultOutput.put());
	if (FAILED(survey.error)) return survey;
	survey.defaultOutputId = AudioDeviceId(defaultOutput.get());
	survey.defaultOutputName = AudioDeviceName(defaultOutput.get());
	survey.facts.outputAvailable = true;
	winrt::com_ptr<IAudioEndpointVolume> outputVolume;
	if (SUCCEEDED(defaultOutput->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, outputVolume.put_void())))
	{
		BOOL muted = FALSE;
		outputVolume->GetMute(&muted);
		outputVolume->GetMasterVolumeLevelScalar(&survey.outputVolume);
		survey.outputMuted = muted != FALSE;
	}
	winrt::com_ptr<IAudioMeterInformation> outputMeter;
	if (SUCCEEDED(defaultOutput->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, outputMeter.put_void())))
		outputMeter->QueryHardwareSupport(&survey.hardwareMeterSupport);
	winrt::com_ptr<IMMDeviceCollection> outputs;
	survey.error = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, outputs.put());
	if (FAILED(survey.error)) return survey;
	UINT count = 0;
	outputs->GetCount(&count);
	for (UINT i = 0; i < count; ++i)
	{
		winrt::com_ptr<IMMDevice> output;
		if (FAILED(outputs->Item(i, output.put()))) continue;
		const bool isDefault = AudioDeviceId(output.get()) == survey.defaultOutputId;
		winrt::com_ptr<IAudioSessionManager2> manager;
		if (FAILED(output->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void()))) continue;
		winrt::com_ptr<IAudioSessionEnumerator> sessions;
		if (FAILED(manager->GetSessionEnumerator(sessions.put()))) continue;
		int size = 0;
		sessions->GetCount(&size);
		// Register even when no phone session exists yet: creation must wake the
		// worker immediately when playback starts, rather than waiting for a timer.
		if (activation && isDefault && activation->current)
			ObservePhoneSession(manager.get(), nullptr, survey.defaultOutputId, phoneName, {});
		auto controls = activation && isDefault && activation->current ? NotifiedPhoneSessions() : std::vector<winrt::com_ptr<IAudioSessionControl>>{};
		for (int j = 0; j < size; ++j)
		{
			winrt::com_ptr<IAudioSessionControl> session;
			if (SUCCEEDED(sessions->GetSession(j, session.put()))) controls.push_back(std::move(session));
		}
		std::unordered_set<std::wstring> seen;
		for (const auto& session : controls)
		{
			if (!IsPhoneAudioSession(session.get(), phoneName)) continue;
			std::wstring instance;
			if (auto session2 = session.try_as<IAudioSessionControl2>())
			{
				wil::unique_cotaskmem_string value;
				if (SUCCEEDED(session2->GetSessionInstanceIdentifier(value.put())) && value) instance = value.get();
			}
			if (!instance.empty() && !seen.insert(instance).second) continue;
			if (activation && isDefault && !instance.empty() && activation->current)
				ObservePhoneSession(manager.get(), session.get(), survey.defaultOutputId, phoneName, instance);
			AudioSessionState state = AudioSessionStateExpired;
			if (FAILED(session->GetState(&state)) || state != AudioSessionStateActive) continue;
			++survey.matchingSessions;
			survey.facts.sessionFound = true;
			if (!isDefault) continue;
			survey.facts.sessionOnDefaultOutput = true;
			auto volume = session.try_as<ISimpleAudioVolume>();
			if (volume)
			{
				BOOL muted = FALSE;
				volume->GetMute(&muted);
				volume->GetMasterVolume(&survey.phoneVolume);
				survey.facts.muted = survey.facts.muted || muted != FALSE || survey.phoneVolume == 0;
			}
			auto meter = session.try_as<IAudioMeterInformation>();
			float sessionPeak = 0;
			bool sessionMeterValid = false;
			auto measure = [&]
			{
				if (!meter) return;
				// Take a short bounded window; a single instant can miss a brief sound.
				for (int sample = 0; sample < 5; ++sample)
				{
					float peak = 0;
					const auto result = meter->GetPeakValue(&peak);
					if (SUCCEEDED(result))
					{
						++survey.validMeters;
						survey.facts.meterValid = true;
						sessionMeterValid = true;
						sessionPeak = (std::max)(sessionPeak, peak);
						survey.facts.peak = (std::max)(survey.facts.peak, peak);
					}
					else survey.error = result;
					Sleep(20);
				}
			};
			measure();
			if (activation && activation->current && volume && !instance.empty())
			{
				auto& initialization = activation->sessions[instance];
				auto verify = [&]
				{
					if (initialization.Observe(sessionMeterValid, sessionPeak))
						DiagnosticLog(L"audio initialization verified epoch=" + std::to_wstring(activation->epoch) +
							L" attempts=" + std::to_wstring(initialization.attempts) + L" real_signal=1; stop activation retries");
				};
				verify();
				const bool canActivate = !survey.outputMuted && survey.outputVolume > 0 && !survey.facts.muted && survey.phoneVolume > 0;
				if (canActivate && initialization.Due(GetTickCount64()) && activation->current)
				{
					const auto started = GetTickCount64();
					const auto result = ActivateUnmutedPhoneSession(volume.get(), [] { Sleep(80); });
					if (result != S_FALSE)
					{
						initialization.Attempted(GetTickCount64(), result);
						if (initialization.attempts <= 3 || initialization.attempts % 30 == 0 || FAILED(result))
							DiagnosticLog(L"audio initialization pulse epoch=" + std::to_wstring(activation->epoch) +
								L" attempt=" + std::to_wstring(initialization.attempts) + L" hresult=" + HResultText(result) +
								L" elapsed_ms=" + std::to_wstring(GetTickCount64() - started) + L" verified=0 transport_unchanged=1");
						measure();
						verify();
					}
				}
				if (!initialization.verified && canActivate && initialization.failures < 3 && activation->current)
				{
					++survey.pendingInitializations;
					const auto now = GetTickCount64();
					const auto delay = initialization.nextAttemptAt > now ? initialization.nextAttemptAt - now : 50;
					const auto bounded = static_cast<UINT>((std::min)(delay, uint64_t(1000)));
					if (!survey.nextInitializationMs || bounded < survey.nextInitializationMs) survey.nextInitializationMs = bounded;
				}
			}
		}
	}
	survey.facts.muted = survey.facts.muted || survey.outputMuted || survey.outputVolume == 0;
	return survey;
}
