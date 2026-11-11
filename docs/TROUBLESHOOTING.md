# Troubleshooting

Logs: agent `%LOCALAPPDATA%\iCam\logs\agent.log` (tray › Abrir registros); camera
`%LOCALAPPDATA%\iCam\logs\vcam.log` of the process identity. For Frame Server (LocalService), this
is normally `C:\Windows\ServiceProfiles\LocalService\AppData\Local\iCam\logs\vcam.log` (admin may be
needed to read it). Install logs are `%ProgramFiles%\iCam\install-*.log`. Legacy ProgramData logs
are not used by 0.2.1. Quick state dump: `"C:\Program Files\iCam\icam-vcamctl.exe" status`.

| What you see (in Teams / on the card) | Cause | Fix |
|---|---|---|
| Camera connects but video does not start | consent is pending on the phone | Tap **Permitir esta conexión** in iCam; a rejected or expired request needs a new connection |
| Shared memory access denied after updating | installer has not configured the owning SID, or old binaries remain loaded | Re-run `windows\scripts\install.ps1` as your normal user, close camera clients and restart Windows if needed |
| "iPhone Camera" missing in Teams | camera not registered, or Teams cached the device list | `icam-vcamctl list`; re-run `windows\scripts\install.ps1`; restart Teams |
| Card "La aplicación iCam no se está ejecutando" | agent not running | Start menu › iCam, or run `C:\Program Files\iCam\icam-agent.exe` |
| Card "Falta el servicio de dispositivos Apple" | Apple Mobile Device Service missing/stopped | Install Apple Devices (Store) or iTunes; `services.msc` › Apple Mobile Device Service › Start |
| Card "Conecta tu iPhone" | no iPhone on USB, charge-only cable, not trusted | data cable, unlock, tap Trust; try another port without hub |
| Card "El dispositivo USB de Apple está detenido" | Windows lists the USB device but its driver is not started | unplug the cable from the PC, reconnect while the iPhone is unlocked; restart Windows if it stays stopped |
| Card "El servicio de Apple no encuentra el iPhone" | USB is started but Apple's device list is empty | replug; if it persists, restart Apple Mobile Device Service as administrator |
| Card "Abre iCam en el iPhone" | app closed, or iOS hasn't brought it to the foreground | open the app; keep it in front |
| Card "Cámara en pausa en el iPhone" | app in background, phone locked, another app using the camera, overheating | bring iCam to the front; unlock; close the other camera app; let it cool |
| Image upside down / sideways | phone orientation lock or flat on a table | stand the phone up in landscape; the picture follows the phone |
| Mirrored text for the other side | "Espejo horizontal" on | turn it off (Teams mirrors only your own preview) |
| Status message appears twice, one copy reflected | mirror-safe status cards | one copy is readable in the local self-view and the other in the remote video; tray > "Avisos compatibles con vista espejo" switches this off |
| Portrait phone but horizontal video wanted | use a crop rather than rotating the person | framing menu > "Llenar encuadre 16:9"; some of the top and bottom are cropped, so move the phone further away if needed |
| Mirror button changes Teams video but not phone preview | old iOS build | install 0.2.0 build 4 or later; preview and transmitted frames use the same mirror choice |
| iCam won't open on the phone after a week | free Apple ID signing expired (7 days) | `scripts\reinstall-ios.ps1` |
| Black image only in Teams calls (preview fine) | Teams/virtual-camera issue (test the same camera in another app) | report with both logs; try Teams on the web as a check |
| Camera privacy | Settings › Privacy & security › Camera: camera access and "Let desktop apps access your camera" must be on | |

Reset everything: `windows\scripts\uninstall.ps1 -Purge`, then install again.

Hotspot is not required for video. If the phone disappears after a USB or hotspot change, reconnect it while unlocked. Restart the PC and iPhone if the Apple device service still cannot see it.
