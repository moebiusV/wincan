<#
.SYNOPSIS
    Removes wincan for the current user: the program, its shortcuts and registrations.

.DESCRIPTION
    The settings and the trade log (%APPDATA%\wincan) stay, so that a later install picks up where this left
    off, unless -Purge is given; -Purge also deletes this computer's key from the personal certificate store.

.PARAMETER InstallDir
    Where wincan.exe was put.  Default: %LOCALAPPDATA%\Programs\wincan

.PARAMETER SettingsDir
    Where wincan.ini and the trade log are.  Default: %APPDATA%\wincan

.PARAMETER Purge
    Also remove the settings, the trade log and this computer's key.

.PARAMETER StopRunning
    Close a running wincan first.

.PARAMETER DryRun
    Say what would be done, and do nothing.
#>
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\wincan'),
    [string]$SettingsDir = (Join-Path $env:APPDATA 'wincan'),
    [switch]$Purge,
    [switch]$StopRunning,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
function Say([string]$m) { Write-Host $m }
function Step([string]$what, [scriptblock]$do) { if ($DryRun) { Say "would: $what" } else { Say $what; & $do } }

$exe = Join-Path $InstallDir 'wincan.exe'
$settingsDir = $SettingsDir
$ini = Join-Path $settingsDir 'wincan.ini'
$running = Get-Process -Name wincan -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
if ($running) {
    if ($StopRunning) { Step 'closing the running wincan' { $running | Stop-Process -Force; Start-Sleep -Seconds 1 } }
    else { throw "wincan is running. Quit it (right-click its tray icon, Exit), or run this again with -StopRunning." }
}

if ($Purge -and (Test-Path $ini)) {
    Add-Type -Namespace Win -Name Ini2 -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern uint GetPrivateProfileString(string section, string key, string dflt, System.Text.StringBuilder buf, uint size, string file);
'@
    $b = New-Object System.Text.StringBuilder 512
    [void][Win.Ini2]::GetPrivateProfileString('wincan', 'client_cert', '', $b, 512, $ini)
    $thumb = $b.ToString().ToUpper()
    if ($thumb) {
        $cert = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $thumb } | Select-Object -First 1
        if ($cert) { Step "removing this computer's key ($thumb) from the certificate store" { Remove-Item -Path $cert.PSPath -DeleteKey -Force } }
    }
}

Step "removing the Start Menu shortcut" { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\wincan.lnk') }
Step "removing the Desktop shortcut, if any" { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path ([Environment]::GetFolderPath('Desktop')) 'wincan.lnk') }
Step "removing the Win+R registration" { Remove-Item -Force -ErrorAction SilentlyContinue 'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\wincan.exe' }
Step "removing start at sign-in, if set" { Remove-ItemProperty -ErrorAction SilentlyContinue -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'wincan' }
Step "removing $InstallDir" { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $InstallDir }
if ($Purge) { Step "removing $settingsDir (settings and trade log)" { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $settingsDir } }
else { Say "kept $settingsDir (settings and trade log); -Purge removes it and this computer's key." }
if ($DryRun) { Say '(a dry run: nothing was changed)' } else { Say 'wincan is uninstalled.' }
