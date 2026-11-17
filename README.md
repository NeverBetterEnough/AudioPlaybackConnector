# AudioPlaybackConnector
**English** | [简体中文](https://github.com/ysc3839/AudioPlaybackConnector/blob/master/README.zh_CN.md)

Bluetooth audio playback (A2DP Sink) connector for Windows 10 2004+.

Microsoft added Bluetooth A2DP Sink to Windows 10 2004. However, a third-party app is required to manage connection.\
There is already an app can do this job. However it can't hide to notification area and it's not open-source.\
So I write this app, provide a simple, modern and open-source alternative.

# Preview
![Preview](https://cdn.jsdelivr.net/gh/ysc3839/AudioPlaybackConnector@master/AudioPlaybackConnector.gif)

# Usage
* Download and run AudioPlaybackConnector from [releases](https://github.com/ysc3839/AudioPlaybackConnector/releases).
* Add a bluetooth device in system bluetooth settings. You can right click AudioPlaybackConnector icon in notification area and select "Bluetooth Settings".
* Click AudioPlaybackConnector icon and select the device you want to connect.
* Enjoy!

Right-click the tray icon and select **Settings** to enable **Start with Windows**, choose a paired phone, or set the phone audio connection to **Enabled** / **Temporarily disabled**. Click **Apply** to save and apply changes. Disabling closes this app's audio connections and stops retries; enabling reconnects the selected phone automatically. Settings survive restarts. The registered receiver remains available while the phone is absent or transport opens fail. Bluetooth arrival events bypass offline backoff; polling with a delay of up to one minute is a fallback. Bluetooth pairing and authentication must already work. Autostart runs after the current user signs in; keep the executable at the same path.

Some Windows A2DP capture-monitor sessions appear active after reconnecting but deliver no frames. A successful activation call is not proof of audio delivery, particularly when phone playback starts later. A persistent MTA worker owns Core Audio objects and registrations between surveys, retaining sessions returned by creation callbacks rather than relying exclusively on enumeration snapshots.

Each opened transport remains pending until a real phone signal is measured. Already-flowing sessions finish immediately. An unmuted, nonzero-volume phone session can otherwise be re-armed with an approximately 80 ms mute edge, restoring its original state. Successful calls with no signal remain pending and retry with bounded backoff (up to about one second), including delayed playback. Real signal stops the retries; later quiet passages do not restart them. Three consecutive API errors stop further pulses. This does not close Bluetooth or change volume. User-muted, output-muted and zero-volume sessions are left unchanged. Disabling or losing the connection cancels obsolete initialization work.

For a still-silent phone, select **Check and repair silent audio** while music is playing. The app first tries session activation and checks mute/volume and valid peak measurements, then permits at most one targeted reconnect if necessary. Silence alone does not trigger repeated reconnects. Default-output changes rebuild this app's connection; pending connection operations time out after 30 seconds. **View diagnostic log** opens `logs/AudioPlaybackConnector.log` next to the executable. Logs contain UTC timestamps, connection results and level measurements, not recordings or track contents, with one rolling backup at about 2 MB. Run `python translate/generate_resources.py` before building to generate Chinese resources without external Python dependencies.

Re-enabling waits for the actual Opened transport state using StateChanged events. It does not insert a fixed release delay or run silence-triggered reconnects. OpenAsync acknowledgement may precede the real Opened event, so an initial Closed value must not dispose the connection. Transport loss, open failure and open timeout reuse a started receiver. Explicit disable, target changes, device removal, failed registration and exit release it. A real Bluetooth disconnect may pause the phone player; reconnecting alone does not resume that player. Old event handlers are revoked and notifications are scoped to the current connection. Only measured audio signal verifies that music is arriving.
