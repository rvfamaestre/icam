<# Scan all Git refs and the local working tree with Gitleaks; never print detected secret values. #>
param([string]$Gitleaks = 'gitleaks')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$reportDir = Join-Path $root 'build\security'
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
$historyReport = Join-Path $reportDir 'history.json'
$treeReport = Join-Path $reportDir 'worktree.json'
$scanFlags = @('--redact=100', '--no-banner', '--report-format=json')
& $Gitleaks git $root '--log-opts=--all --full-history' @scanFlags '--report-path' $historyReport
$history = $LASTEXITCODE
& $Gitleaks dir $root @scanFlags '--report-path' $treeReport
$tree = $LASTEXITCODE
if ($history -ne 0 -or $tree -ne 0) { throw "Security scan failed or found a secret. Read redacted reports in $reportDir (history=$history, worktree=$tree)." }
Write-Host 'No secrets detected in Git history or local files. Reports are redacted.'
