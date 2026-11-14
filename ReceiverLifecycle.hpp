#pragma once

// A started receiver permits incoming A2DP connections even while the phone is
// absent. A closed transport or a failed OpenAsync must not unregister it.
class ReceiverLifecycle
{
public:
	void Created() { registered_ = false; opened_ = false; }
	void Registered() { registered_ = true; }
	void Opened() { opened_ = true; }
	void TransportClosed() { opened_ = false; }
	void OpenFailed() { opened_ = false; }
	void Released() { registered_ = false; opened_ = false; }
	bool KeepListening() const { return registered_; }
	bool TransportReady() const { return registered_ && opened_; }
private:
	bool registered_ = false;
	bool opened_ = false;
};
