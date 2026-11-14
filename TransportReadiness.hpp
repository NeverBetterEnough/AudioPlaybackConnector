#pragma once

struct ConnectionStateSignal
{
	wil::unique_handle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
};

struct ApartmentReturn
{
	winrt::apartment_context context;
	bool await_ready() const noexcept { return false; }
	void await_resume() const noexcept { }
	template<typename Handle> void await_suspend(Handle handle) const
	{
		// This old C++/WinRT version runs signal callbacks on an uninitialized
		// threadpool thread. IContextCallback requires COM on that calling thread.
		const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(result) && result != RPC_E_CHANGED_MODE) winrt::check_hresult(result);
		struct Uninitialize
		{
			HRESULT result;
			~Uninitialize() { if (SUCCEEDED(result)) CoUninitialize(); }
		} uninitialize{ result };
		// Keep the destination and COM guard on this callback's stack: resuming
		// the coroutine may destroy its awaiter or suspend on another thread.
		auto destination = context;
		destination.await_suspend(handle);
	}
};

// OpenAsync acknowledgement and actual transport readiness are separate events.
// An initial Closed value must not dispose a connection still becoming Opened.
template<typename StateReader, typename CurrentAttempt>
winrt::Windows::Foundation::IAsyncAction AwaitTransportReadiness(
	std::shared_ptr<ConnectionStateSignal> signal, StateReader isOpened, CurrentAttempt isCurrent,
	std::chrono::milliseconds timeout = std::chrono::seconds(30))
{
	const auto context = ApartmentReturn{};
	const auto deadline = GetTickCount64() + static_cast<uint64_t>(timeout.count());
	bool loggedWaiting = false;
	while (isCurrent())
	{
		ResetEvent(signal->event.get());
		if (isOpened()) co_return;
		if (!loggedWaiting)
		{
			DiagnosticLog(L"open request accepted; waiting for real Opened event; preserving the same connection object");
			loggedWaiting = true;
		}
		const auto now = GetTickCount64();
		if (now >= deadline) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		const bool signaled = co_await winrt::resume_on_signal(signal->event.get(), std::chrono::milliseconds(deadline - now));
		co_await context;
		if (!isCurrent()) co_return;
		if (!signaled) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
	}
}
