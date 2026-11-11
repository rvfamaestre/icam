<#
.SYNOPSIS
  Installs iCam on this PC: copies the binaries to %ProgramFiles%\iCam, registers the virtual camera
  "iPhone Camera" (one UAC prompt), and starts the agent in your session with autostart at logon.

.PARAMETER From
  Folder with the built binaries (default: build\release under the repository).

.NOTES
  COM registration under HKLM and Program Files need elevation. The persistent camera is registered
  in the original user's session with CurrentUser access. The agent always runs as you.
#>
param(
    [string]$From,
    [switch]$Elevated,       # internal: the elevated half
    [string]$AgentUserSid,   # original user, carried across an over-the-shoulder UAC prompt
    [switch]$NoAgent         # don't start the agent / don't touch autostart
)
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell to install the x64 camera.' }
$InstallDir = Join-Path $env:ProgramFiles 'iCam'
$Files = @('icam-vcam.dll', 'icam-agent.exe', 'icam-vcamctl.exe', 'icam-vcamtest.exe', 'icam-fakephone.exe')

function Find-Binaries([string]$root) {
    $map = @{}
    foreach ($f in $Files) {
        $hit = Get-ChildItem -Path $root -Recurse -Filter $f -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { $map[$f] = $hit.FullName }
    }
    return $map
}

if (-not $From) { $From = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'build\release' }
$From = (Resolve-Path $From).Path

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $Elevated -or -not $AgentUserSid) { $AgentUserSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value }
if ($AgentUserSid -notmatch '^S-1-5-21-\d+-\d+-\d+-\d+\z' -and $AgentUserSid -notmatch '^S-1-12-1-\d+-\d+-\d+-\d+\z') {
    throw 'AgentUserSid must identify a Windows user account.'
}

if (-not $Elevated) {
    $bins = Find-Binaries $From
    foreach ($req in @('icam-vcam.dll', 'icam-vcamctl.exe')) {
        if (-not $bins.ContainsKey($req)) { throw "Missing $req under $From. Build first: scripts\build.ps1 -Config Release" }
    }
    # Stop a running agent so its exe can be replaced.
    Get-Process icam-agent -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $shell = (Get-Process -Id $PID).Path
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated', '-From', "`"$From`"")
    $argv += @('-AgentUserSid', $AgentUserSid)
    if ($NoAgent) { $argv += '-NoAgent' }
    Write-Host 'Requesting administrator rights to register the camera (UAC)...'
    $p = Start-Process -FilePath $shell -ArgumentList $argv -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "Elevated install failed (exit $($p.ExitCode)). See $InstallDir\install-*.log" }

    & (Join-Path $InstallDir 'icam-vcamctl.exe') register
    if ($LASTEXITCODE) { throw "Per-user camera registration failed ($LASTEXITCODE)." }
    if (-not $NoAgent -and (Test-Path (Join-Path $InstallDir 'icam-agent.exe'))) {
        $agent = Join-Path $InstallDir 'icam-agent.exe'
        New-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'iCam' -Value "`"$agent`"" -PropertyType String -Force | Out-Null
        $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'iCam.lnk'
        $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
        $sc.TargetPath = $agent
        $sc.Description = 'iCam: iPhone como webcam'
        $sc.Save()
        Start-Process -FilePath $agent -WindowStyle Hidden
        Write-Host 'Agent started and set to start at logon.'
    }
    Write-Host "iCam installed in $InstallDir. In Teams choose the camera 'iPhone Camera'."
    exit 0
}

# ---------------- elevated half ----------------
if (-not $isAdmin) { throw 'The -Elevated step must run as administrator.' }
foreach ($path in @($env:ProgramFiles, $InstallDir)) {
    if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing redirected install directory: $path"
    }
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$acl = New-Object Security.AccessControl.DirectorySecurity
$acl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)(A;OICI;GRGX;;;LS)')
Set-Acl -LiteralPath $InstallDir -AclObject $acl
# A fresh file in Program Files: never append as administrator to a user-writable log.
Start-Transcript -LiteralPath (Join-Path $InstallDir ("install-" + [guid]::NewGuid().ToString('N') + '.log')) | Out-Null
try {
    New-Item -Path 'HKLM:\SOFTWARE\iCam' -Force | Out-Null
    $registryAcl = New-Object Security.AccessControl.RegistrySecurity
    $registryAcl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KR;;;BU)(A;CI;KR;;;LS)')
    Set-Acl -LiteralPath 'HKLM:\SOFTWARE\iCam' -AclObject $registryAcl
    New-ItemProperty -Path 'HKLM:\SOFTWARE\iCam' -Name AgentUserSid -Value $AgentUserSid -PropertyType String -Force | Out-Null

    $bins = Find-Binaries $From
    foreach ($name in $bins.Keys) {
        $dst = Join-Path $InstallDir $name
        if (Test-Path $dst) {
            # A DLL loaded by Frame Server can't be overwritten but can be renamed; it's deleted next time.
            try { Remove-Item $dst -Force } catch { Rename-Item $dst ("$name.old-" + [DateTime]::Now.Ticks) }
        }
        Copy-Item $bins[$name] $dst -Force
        Write-Host "copied $name"
    }
    Get-ChildItem $InstallDir -Filter '*.old-*' | ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
    Copy-Item (Join-Path $PSScriptRoot 'uninstall.ps1') (Join-Path $InstallDir 'uninstall.ps1') -Force

    $dll = Join-Path $InstallDir 'icam-vcam.dll'
    $regsvr32 = Join-Path ([Environment]::GetFolderPath('System')) 'regsvr32.exe'
    $r = Start-Process $regsvr32 -ArgumentList '/s', "`"$dll`"" -WindowStyle Hidden -Wait -PassThru
    if ($r.ExitCode -ne 0) { throw "regsvr32 failed ($($r.ExitCode))" }
    # Remove the legacy all-users camera so other accounts cannot request this user's phone stream.
    & (Join-Path $InstallDir 'icam-vcamctl.exe') remove-legacy
    if ($LASTEXITCODE) { throw "legacy camera removal failed ($LASTEXITCODE)" }

    # Apps & features entry.
    $key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\iCam'
    New-Item -Path $key -Force | Out-Null
    $shell = (Get-Process -Id $PID).Path
    Set-ItemProperty $key DisplayName 'iCam (iPhone Camera for Windows)'
    Set-ItemProperty $key DisplayVersion '0.2.1'
    Set-ItemProperty $key Publisher 'iCam (personal build)'
    Set-ItemProperty $key InstallLocation $InstallDir
    Set-ItemProperty $key UninstallString "`"$shell`" -NoProfile -ExecutionPolicy Bypass -File `"$InstallDir\uninstall.ps1`""
    Set-ItemProperty $key NoModify 1 -Type DWord
    Set-ItemProperty $key NoRepair 1 -Type DWord
    Write-Host 'elevated install done'
} finally {
    Stop-Transcript | Out-Null
}
