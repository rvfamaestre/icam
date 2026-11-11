# iPhone setup

The app uses SwiftPM and [xtool](https://github.com/xtool-org/xtool) in WSL Ubuntu. You need your own Apple account, an iPhone running iOS 17 or later, and an iOS SDK extracted from Xcode. Apple tools and signing identities are not included.

## Setup

From the repository root in PowerShell:

```powershell
wsl -d Ubuntu -u root -- bash scripts/setup-wsl.sh root
wsl -d Ubuntu -- bash scripts/setup-wsl.sh user
```

Download Xcode from [Apple Developer](https://developer.apple.com/download/all/?q=Xcode). In WSL, run `~/bin/xtool setup` and supply your own account and downloaded Xcode path when asked. Keep credentials and signing files outside the checkout.

Choose your own unique bundle ID in `ios/xtool.yml` before signing. Connect the phone, accept Trust This Computer, and follow Apple's Developer Mode and developer-trust prompts if required.

## Build and install

From the repository root in PowerShell:

```powershell
scripts\reinstall-ios.ps1 -BuildOnly
scripts\reinstall-ios.ps1
```

The first command only builds. The second builds, signs and installs on the connected phone. The WSL relay reaches Apple's Windows device service over loopback; the phone stays attached to Windows.

A free Apple signing profile expires after seven days. Rebuild and reinstall when it expires. The Windows program has no weekly signing limit.

To build an IPA for an existing LiveContainer setup, run `~/bin/xtool dev build --ipa` from `ios/` in WSL. Use your own signing identity and import it yourself. The optional certificate-export helper requires a private Linux directory and refuses Windows mounts, existing files and symlinks:

```sh
mkdir -m 700 ~/icam-private
python3 scripts/make-livecontainer-cert.py ~/icam-private/identity.p12 ~/icam-private/password.txt --identity ~/.config/xtool/data/TEAM_ID
```

Replace `TEAM_ID` with your own identity path. Keep the exported key and its password private. Compatibility mode supports older Apple importers; `--modern` selects AES-256/SHA-256 when the importer supports it.

## Tests and limits

The protocol and camera-policy tests run in WSL with `swift test` from `ios/`, without the iOS SDK. Certificate-export tests use a temporary synthetic identity: `python3 -m unittest discover -s scripts/tests -v` from the repository root on Linux.

Keep iCam open and the phone unlocked while using the camera. iOS stops capture when the app enters the background or the phone locks. Every new PC connection needs approval on the phone. See [security limits](SECURITY.md).
