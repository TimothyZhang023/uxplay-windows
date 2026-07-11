```diff
+ 4 may 2026
+ new version is finally out! check it out in the releases section!
```

# FREE AirPlay to your Windows PC
Free as both in "freedom" and "free beer"!

## Installation
Download the latest version of uxplay-windows from [**releases**](https://github.com/TimothyZhang023/uxplay-windows/releases/latest).

After installing, control uxplay-windows from it's [tray icon](https://www.odu.edu/sites/default/files/documents/win10-system-tray.pdf)! Right-click it to start or stop AirPlay. \
You can also set it to run automatically when your PC starts

## FAQ — Please Read!
> [!NOTE]
> *What is uxplay-windows?*
> 
> [uxplay-windows](.) is a software that allows you to video stream with AirPlay to your windows PC. \
> It turns [UxPlay](https://github.com/FDH2/UxPlay/) into a fully featured App for Windows 10/11 users, making it easier for those who may find compiling UxPlay challenging.
> 
> Most other software achieving the same functionality as `uxplay-windows` is usually paid and non-free.


> [!TIP]
> *My \<apple device\> can't connect to my PC!!!*
> 1. Check the discovery status shown in the app. If it is degraded, choose `Retry Bonjour Discovery` from the tray menu.
> 2. Toggle Wi-Fi and Bluetooth OFF on your iPhone/iPad/Mac, wait a couple of seconds and reconnect. It might take a few attempts.
> 3. Use `Open Diagnostic Logs` from the app or tray menu. Attach both
>    `uxplay-windows.log` and `uxplay-engine.log` when reporting a problem.
>
> Bonjour failure no longer stops the receiver. It continues with Bluetooth fallback and shows a warning. The BLE helper cycles through active IPv4 interfaces, which improves discovery on VPN, Wi-Fi + Ethernet and direct-cable networks.
>
> If the UxPlay engine cannot initialize, the app records its complete output,
> Windows crash code and restart history. Automatic retries pause after three
> consecutive startup failures instead of continuously switching between
> `starting` and `stopped`; use `Retry UxPlay Engine` after checking the logs.

### Windows video performance

- The default profile requests 1920×1080 at up to 60 FPS and lets GStreamer
  select the renderer. Explicit paired D3D11/D3D12 hardware modes, 1440p60,
  4K60 HEVC and a 1080p30 compatibility profile are available in the main
  window. A failed hardware pipeline automatically falls back to software
  decoding once instead of entering a restart loop.
- Low latency mode disables timestamp-delayed video presentation, lowers the
  advertised audio latency and enables a 1 ms Windows timer resolution while
  the engine is running. It is experimental and defaults to off. Compressed
  H.264/H.265 frames are never discarded, because doing so corrupts inter-frame
  references and causes macroblocking.
- Double-click the video or press `F11` to enter or leave fullscreen.
  `Alt+Enter` is disabled. Input and fullscreen changes are handled by the
  GStreamer-owned window rather than by cross-process Win32 window changes.
- Video-setting changes are saved during an active AirPlay session and applied
  after the session ends, avoiding an unexpected renderer shutdown mid-stream.

> [!IMPORTANT]
> *Why is Windows Defender complaining during installation?*
> 
> ![alt text](https://raw.githubusercontent.com/leapbtw/uxplay-windows/refs/heads/main/stuff/defender.png "defender")
>
> Just click on `More info` and it will let you install. It complains because the executable is not signed. If you don't trust this software you can always build it yourself! See below.
>
> The MSI installer adds local-network firewall rules for `uxplay-engine.exe`
> and Bonjour UDP 5353 discovery. Portable ZIP users must allow `uxplay-engine.exe`
> and `mDNSResponder.exe` when prompted by Windows Firewall.


> [!NOTE]
>  *How do I build this software myself?*
> 
> Please see [BUILDING.md](./docs/BUILDING.md)
<br>

## TODO
- make an update checker

## Known Issues
~~uxplay bugs out when waking PC from Sleep~~
~~you can fix this by killing uxplay-windows.exe and restarting Bonjour Service, and restarting uxplay.exe. Also restarting your PC might fix this.~~  \
Apparently moving from Bonjour PS to mDNSResponder fixed it? :)

## Reporting Issues
Please report issues related to the build system created with GitHub Actions in this repository. For issues related to other parts of this software, report them in their respective repositories.

## License
Please take a look at the [LICENSE](./docs/LICENSE.rtf).
