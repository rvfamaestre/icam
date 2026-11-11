# Security and limits

This source is iCam 0.2.1, iOS build 6. Build and install both sides from the same version to get its consent and shared-memory checks.

- Video travels through Apple's USB tunnel. iCam has no cloud upload service or telemetry. An app using the virtual camera, such as Teams, can send that video as part of a call.
- Each new connection needs approval on the iPhone before video is encoded or sent. Permission expires on disconnect. The displayed PC name is supplied by the client and is not verified, so only approve connections you started.
- Windows camera registration uses `CurrentUser`. Shared-memory access is limited to the installing user's SID, SYSTEM and LocalService, with owner and ACL checks.
- Complete isolation from other Windows accounts remains unverified. Another account creating a separate camera source through Frame Server may still be able to receive frames. Use a PC with trusted local users.
- Programs running under the same user, administrators and compromised system services are outside that isolation boundary. Consent is not cryptographic host authentication.
- A local process can reserve the shared-memory/event names first and make the camera unavailable. Handle checks reject those objects rather than trusting them.
- Keep Apple credentials, signing keys, provisioning profiles, device pairing records, personal logs and certificate exports outside Git. The export helper's file permissions do not protect copies made elsewhere.

Protocol, consent, buffer bounds, shared-memory ACLs and COM lifetime have regression tests. The simulator exercises video without a phone. Real multi-user isolation, the new consent UI on a phone and long thermal runs still need device testing. Automated secret scanning does not prove the absence of every privacy or security issue.

To scan your own checkout with [Gitleaks](https://github.com/gitleaks/gitleaks):

```powershell
scripts\security-scan.ps1 -Gitleaks <path-to-gitleaks.exe>
```

Reports redact detected values and stay under ignored `build/security/`. Do not attach logs, pairing files or signing files to a public issue without reviewing them.
