# Build and use

By Rafael Maestre López. Windows 11 x64 and iOS 17 or later are required. This is source code, with no installer binaries or Apple signing files included.

## Windows

Install Apple Mobile Device Support through Apple Devices, iTunes or iCloud. To compile, install Visual Studio Build Tools 2022 with C++, a Windows 11 SDK, CMake and Ninja.

From the repository root in PowerShell:

```powershell
scripts\build.ps1 -Config Release -Test
windows\scripts\e2e-test.ps1
windows\scripts\install.ps1
```

The installer requests administrator access for the files and COM registration. Run it from your normal user session. The virtual camera is registered for that user.

Build and install the phone app separately: [iPhone setup](IOS.md).

## Use

Connect the iPhone with a USB data cable, unlock it and open iCam. Approve the connection on the phone, then select **iPhone Camera** in Teams or another camera app. Audio stays on the PC.

The phone has front/back camera, lens, quality, framing and mirror controls. Keep the app in the foreground. Auto quality follows the camera format requested by the PC app. Choosing 4K on the phone does not make a Teams call 4K.

The Windows tray menu has camera, mirror and log controls. [Troubleshooting](TROUBLESHOOTING.md) covers connection errors. Read the [security limits](SECURITY.md) before using it on a shared PC.

To uninstall:

```powershell
windows\scripts\uninstall.ps1
```

Add `-Purge` to remove local logs and settings as well.
