#pragma once

#include "resource.h"
#include "AudioHealthPolicy.hpp"
#include "ReceiverLifecycle.hpp"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_SHOWSETTINGS = WM_APP + 3;
constexpr UINT WM_CONNECTIONCHANGED = WM_APP + 4;
constexpr UINT WM_REPAIR_AUDIO = WM_APP + 5;
constexpr UINT WM_SET_AUDIO_ENABLED = WM_APP + 6;
constexpr UINT WM_AUDIO_SESSION_READY = WM_APP + 7;
constexpr UINT_PTR RECONNECT_TIMER = 1;
constexpr UINT_PTR AUDIO_HEALTH_TIMER = 2;
constexpr UINT_PTR CONNECTION_TIMEOUT_TIMER = 3;

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
Flyout g_xamlFlyout = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
DevicePicker g_devicePicker = nullptr;
std::unordered_map<std::wstring, std::pair<DeviceInformation, AudioPlaybackConnection>> g_audioPlaybackConnections;
std::unordered_map<std::wstring, winrt::event_token> g_connectionStateTokens;
struct ConnectionStateSignal;
std::unordered_map<std::wstring, std::shared_ptr<ConnectionStateSignal>> g_connectionStateSignals;
HICON g_hIconLight = nullptr;
HICON g_hIconDark = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;
bool g_connectionEnabled = true;
bool g_stopping = false;
bool g_connecting = false;
uint64_t g_connectionGeneration = 0;
uint64_t g_operationSerial = 0;
UINT g_retryDelay = 5000;
std::wstring g_targetDeviceId;
std::wstring g_targetDeviceName;
std::wstring g_connectionStatus;
HWND g_settingsWnd = nullptr;
std::vector<DeviceInformation> g_settingsDevices;
winrt::Windows::Foundation::IAsyncInfo g_pendingOperation = nullptr;
std::vector<winrt::Windows::Foundation::IAsyncInfo> g_drainingOperations;
bool g_verifyAfterResume = false;
ReceiverLifecycle g_receiverLifecycle;
bool g_reopenOnArrival = false;
winrt::guid g_targetContainerId{};
AudioRepairPolicy g_audioRepair;
bool g_repairOnStartup = false;
void OnAudioConnectionOpened();
void BeginAudioRepair();
void StartPhonePresenceWatcher();
void StopPhonePresenceWatcher();
void RefreshPhonePresence();
void InvalidateAudioActivation();

#include "Util.hpp"
#include "DiagnosticLog.hpp"
#include "TransportReadiness.hpp"
#include "AudioSessionActivation.hpp"
#include "AudioSurvey.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
#include "Direct2DSvg.hpp"
