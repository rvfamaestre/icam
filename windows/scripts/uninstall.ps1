<#
.SYNOPSIS
  Removes iCam completely: virtual camera, COM registration, files, autostart and the Apps entry.
  Legacy logs are kept. -Purge removes the calling user's settings.
#>
param([switch]$Elevated, [switch]$Purge)
$ErrorActionPreference = 'Continue'
if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell to uninstall the x64 camera.' }
$InstallDir = Join-Path $env:ProgramFiles 'iCam'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

# Per-user parts (run as the user, before/without elevation).
Get-Process icam-agent -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'iCam' -ErrorAction SilentlyContinue
Remove-Item (Join-Path ([Environment]::GetFolderPath('Programs')) 'iCam.lnk') -ErrorAction SilentlyContinue
if ($Purge) { Remove-Item 'HKCU:\Software\iCam' -Recurse -ErrorAction SilentlyContinue }
if (-not $Elevated) {
    $userCtl = Join-Path $InstallDir 'icam-vcamctl.exe'
    if ((Test-Path -LiteralPath $InstallDir) -and ((Get-Item -LiteralPath $InstallDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing redirected uninstall directory: $InstallDir"
    }
    if ((Test-Path -LiteralPath $userCtl) -and ((Get-Item -LiteralPath $userCtl -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing redirected uninstaller tool: $userCtl"
    }
    if (Test-Path -LiteralPath $userCtl) { & $userCtl remove }
}

if (-not $isAdmin) {
    $shell = (Get-Process -Id $PID).Path
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated')
    if ($Purge) { $argv += '-Purge' }
    # PowerShell loads the script before executing it; no predictable writable temp script is needed.
    $p = Start-Process -FilePath $shell -ArgumentList $argv -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    exit $p.ExitCode
}

# Check the absolute deletion targets and reject redirects before running elevated file operations.
foreach ($target in @($InstallDir)) {
    $base = $env:ProgramFiles
    if ([IO.Path]::GetFullPath($target) -ne [IO.Path]::Combine([IO.Path]::GetFullPath($base), 'iCam')) {
        throw "Unexpected uninstall target: $target"
    }
    if ((Test-Path -LiteralPath $target) -and ((Get-Item -LiteralPath $target -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing redirected uninstall directory: $target"
    }
    if (Test-Path -LiteralPath $target) {
        $redirect = Get-ChildItem -LiteralPath $target -Recurse -Force -Attributes ReparsePoint -ErrorAction Stop | Select-Object -First 1
        if ($redirect) { throw "Refusing redirected child: $($redirect.FullName)" }
    }
}
$ctl = Join-Path $InstallDir 'icam-vcamctl.exe'
if (Test-Path $ctl) { & $ctl remove-legacy }
$dll = Join-Path $InstallDir 'icam-vcam.dll'
if (Test-Path $dll) {
    $regsvr32 = Join-Path ([Environment]::GetFolderPath('System')) 'regsvr32.exe'
    Start-Process $regsvr32 -ArgumentList '/u', '/s', "`"$dll`"" -WindowStyle Hidden -Wait | Out-Null
}
Remove-Item 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{4883E02E-087A-4FE6-8468-2339757D3864}' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\iCam' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item 'HKLM:\SOFTWARE\iCam' -Recurse -Force -ErrorAction SilentlyContinue

# Frame Server may still hold the DLL; files it holds go at next reboot.
if (Test-Path $InstallDir) {
    Get-ChildItem $InstallDir -File | ForEach-Object {
        try { Remove-Item $_.FullName -Force -ErrorAction Stop }
        catch {
            Add-Type -Namespace Win32 -Name Native -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool MoveFileEx(string a, string b, int f);'
            [Win32.Native]::MoveFileEx($_.FullName, $null, 4) | Out-Null  # MOVEFILE_DELAY_UNTIL_REBOOT
            Write-Host "$($_.Name) is in use; it will be deleted at next restart"
        }
    }
    Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
}
# Legacy ProgramData logs are left for manual cleanup: they may be owned/replaced by another user.
Write-Host 'iCam removed.'
exit 0
