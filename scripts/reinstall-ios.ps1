<#
.SYNOPSIS
  Builds the iPhone app with xtool in WSL and installs it on the attached iPhone (also the weekly
  re-install needed with a free Apple ID). Requires the one-time setup in docs/IOS.md.
#>
param([switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$wslRepo = (wsl.exe -d Ubuntu -- wslpath -a ($repo -replace '\\', '/')).Trim()
if ($LASTEXITCODE -or -not $wslRepo) { throw 'No se ha podido abrir Ubuntu en WSL.' }
# Let WSL reach the iPhone through Apple Mobile Device Service on Windows.
if (-not $BuildOnly) {
    $wslUser = (wsl.exe -d Ubuntu -- id -un).Trim()
    if ($LASTEXITCODE -or -not $wslUser) { throw 'No se ha podido identificar el usuario de WSL.' }
    wsl.exe -d Ubuntu -u root -- bash "$wslRepo/scripts/wsl-usbmuxd-relay.sh" --user $wslUser
    if ($LASTEXITCODE) { throw 'No se llega al servicio de Apple. Conecta el iPhone y comprueba Apple Mobile Device Service.' }
    $devices = @(wsl.exe -d Ubuntu -- idevice_id -l)
    if ($LASTEXITCODE -or -not ($devices | Where-Object { $_.Trim() })) {
        throw 'No se detecta el iPhone. Conectalo por USB, desbloquealo y acepta Confiar si lo pide.'
    }
    Write-Host 'Compilando, firmando e instalando iCam. No desconectes el iPhone.'
}
$cmd = if ($BuildOnly) { @('dev', 'build') } else { @('dev', 'run', '--usb') }
# Paths and arguments remain data even when the checkout path contains shell metacharacters.
wsl.exe -d Ubuntu --cd "$wslRepo/ios" -- bash "$wslRepo/scripts/build-ios.sh" @cmd
if ($LASTEXITCODE) { throw "xtool $cmd failed ($LASTEXITCODE)" }
if ($BuildOnly) {
    Write-Host 'Compilacion terminada. No se ha instalado ni renovado la app.'
} else {
    Write-Host 'Instalacion terminada. Abre iCam en el iPhone. No hace falta borrar la app ni reinstalar el programa del PC.'
}
