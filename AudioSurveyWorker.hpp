#pragma once

#include <condition_variable>
#include <functional>
#include <thread>

struct AudioSurveyJob
{
	uint64_t serial = 0, generation = 0;
	std::wstring phone;
	std::shared_ptr<AudioActivationContext> activation;
};

struct AudioSurveyResult
{
	AudioSurveyJob job;
	AudioSurvey survey;
};

// Keep Core Audio objects, registrations and COM's MTA alive between samples.
// UI timers enqueue snapshots; callbacks only wake the UI, never run COM work.
class AudioSurveyWorker
{
public:
	using Provider = std::function<AudioSurvey(const std::wstring&, std::shared_ptr<AudioActivationContext>)>;
	explicit AudioSurveyWorker(HWND window, Provider provider = SurveyPhoneAudio) :
		window_(window), provider_(std::move(provider)), thread_([this] { Run(); }) { }
	~AudioSurveyWorker() { Stop(); }
	void Request(AudioSurveyJob job)
	{
		std::lock_guard<std::mutex> guard(mutex_);
		if (stopping_) return;
		pending_ = std::make_unique<AudioSurveyJob>(std::move(job));
		wake_.notify_one();
	}
	std::unique_ptr<AudioSurveyResult> TakeResult()
	{
		std::lock_guard<std::mutex> guard(mutex_);
		return std::move(result_);
	}
	void Stop()
	{
		{
			std::lock_guard<std::mutex> guard(mutex_);
			stopping_ = true;
			pending_.reset();
		}
		wake_.notify_one();
		if (thread_.joinable()) thread_.join();
	}
private:
	void Run() noexcept
	{
		const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		DiagnosticLog(L"audio worker started; persistent MTA thread=" + std::to_wstring(GetCurrentThreadId()) + L" hresult=" + HResultText(initialized));
		for (;;)
		{
			std::unique_ptr<AudioSurveyJob> job;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				wake_.wait(lock, [this] { return stopping_ || pending_; });
				if (stopping_) break;
				job = std::move(pending_);
			}
			auto result = std::make_unique<AudioSurveyResult>();
			result->job = std::move(*job);
			if (FAILED(initialized)) result->survey.error = initialized;
			else if (!result->job.activation || !result->job.activation->current) result->survey.error = E_ABORT;
			else
			{
				try { result->survey = provider_(result->job.phone, result->job.activation); }
				catch (const winrt::hresult_error& error) { result->survey.error = error.code(); }
				catch (...) { result->survey.error = E_FAIL; }
			}
			{
				std::lock_guard<std::mutex> guard(mutex_);
				if (stopping_) break;
				result_ = std::move(result);
			}
			if (window_) PostMessageW(window_, WM_AUDIO_SURVEY_RESULT, 0, 0);
		}
		if (SUCCEEDED(initialized))
		{
			StopAudioNotifications();
			CoUninitialize();
		}
		DiagnosticLog(L"audio worker stopped; native subscriptions released before MTA shutdown");
	}
	HWND window_;
	Provider provider_;
	std::mutex mutex_;
	std::condition_variable wake_;
	bool stopping_ = false;
	std::unique_ptr<AudioSurveyJob> pending_;
	std::unique_ptr<AudioSurveyResult> result_;
	std::thread thread_;
};
