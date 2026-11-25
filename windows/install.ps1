<#
.SYNOPSIS
    Installs wincan, the candle window, for the current user (no administrator rights needed).

.DESCRIPTION
    - copies wincan.exe to a folder of its own (default %LOCALAPPDATA%\Programs\wincan);
    - adds a Start Menu shortcut, and registers the program so that Win+R "wincan" starts it;
    - optionally a Desktop shortcut (-Desktop) and start when you sign in (-Startup);
    - for an https:// server, makes this computer's key (an ECDSA P-256 certificate in your personal
      certificate store, the private key not exportable), writes the settings to
      %APPDATA%\wincan\wincan.ini and prints the key's SHA-256 fingerprint, which the server's
      administrator authorizes:   candled --authorize SHA256:...
    Settings and the trade log (%APPDATA%\wincan) are never touched by an upgrade.

.PARAMETER Server
    The candle supply: https://HOST:PORT (encrypted, with this computer's key; the usual case) or HOST:PORT
    (plain HTTP, for the loopback address).  Kept in wincan.ini.  Without it the existing setting stays.

.PARAMETER ServerPin
    The fingerprint of the server's certificate, as `candled --fingerprint` prints it (SHA256:...).  Without it
    wincan trusts the certificate it sees at the first contact, and says so once.

.PARAMETER InstallDir
    Where to put wincan.exe.  Default: %LOCALAPPDATA%\Programs\wincan

.PARAMETER Desktop
    Also make a shortcut on the Desktop.

.PARAMETER Startup
    Start wincan when you sign in.

.PARAMETER NoKey
    Do not make this computer's key.

.PARAMETER StopRunning
    Close a running wincan so that it can be replaced.

.PARAMETER Launch
    Start wincan when done.

.PARAMETER DryRun
    Say what would be done, and do nothing.

.EXAMPLE
    .\install.ps1 -Server https://localhost:8766 -ServerPin SHA256:0123...cdef -Launch

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File install.ps1 -Server https://candles.example.org:8766
#>
[CmdletBinding()]
param(
    [string]$Server,
    [string]$ServerPin,
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\wincan'),
    [switch]$Desktop,
    [switch]$Startup,
    [switch]$NoKey,
    [switch]$StopRunning,
    [switch]$Launch,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'

function Say([string]$m) { Write-Host $m }
function Step([string]$what, [scriptblock]$do) {
    if ($DryRun) { Say "would: $what" } else { Say $what; & $do }
}

# ---- settings file access (the same INI file that wincan itself reads and writes)
Add-Type -Namespace Win -Name Ini -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern uint GetPrivateProfileString(string section, string key, string dflt, System.Text.StringBuilder buf, uint size, string file);
'@
function Get-Ini([string]$file, [string]$key) {
    $b = New-Object System.Text.StringBuilder 512
    [void][Win.Ini]::GetPrivateProfileString('wincan', $key, '', $b, 512, $file)
    $b.ToString()
}
function Set-Ini([string]$file, [string]$key, [string]$value) { [void][Win.Ini]::WritePrivateProfileString('wincan', $key, $value, $file) }

# ---- where the files are
$here = $PSScriptRoot
$exeSrc = @((Join-Path $here 'wincan.exe'), (Join-Path $here '..\wincan.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exeSrc) { throw "wincan.exe is not next to this script ($here). Unpack the whole release, or build it (make)." }
$exeSrc = (Resolve-Path $exeSrc).Path
$exeDst = Join-Path $InstallDir 'wincan.exe'
$settingsDir = Join-Path $env:APPDATA 'wincan'
$ini = Join-Path $settingsDir 'wincan.ini'
$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\wincan.lnk'
$appPaths = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\wincan.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

# ---- the files must be the ones that were released
$sums = Join-Path (Split-Path $exeSrc) 'SHA256SUMS'
if (Test-Path $sums) {
    $want = Select-String -Path $sums -Pattern '\swincan\.exe\s*$' | ForEach-Object { ($_.Line -split '\s+')[0] } | Select-Object -First 1
    $have = (Get-FileHash -Algorithm SHA256 $exeSrc).Hash.ToLower()
    if ($want -and $want.ToLower() -ne $have) { throw "wincan.exe does not match SHA256SUMS (expected $want, got $have). Download it again." }
    Say "wincan.exe matches SHA256SUMS."
}

# ---- is it running from where it will be replaced?
$running = Get-Process -Name wincan -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exeDst }
if ($running) {
    if ($StopRunning) { Step 'closing the running wincan' { $running | Stop-Process -Force; Start-Sleep -Seconds 1 } }
    else { throw "wincan is running from $InstallDir. Quit it (right-click its tray icon, Exit), or run this again with -StopRunning." }
}

# ---- the program, and the ways to start it
Step "copying wincan.exe to $InstallDir" {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -Force $exeSrc $exeDst
}
function New-Shortcut([string]$path) {
    $s = (New-Object -ComObject WScript.Shell).CreateShortcut($path)
    $s.TargetPath = $exeDst; $s.WorkingDirectory = $InstallDir; $s.Description = 'wincan: candles on your desktop'
    $s.Save()
}
Step "adding the Start Menu shortcut" { New-Shortcut $startMenu }
Step "registering wincan.exe for Win+R" {
    New-Item -Path $appPaths -Force | Out-Null
    Set-ItemProperty -Path $appPaths -Name '(default)' -Value $exeDst
    Set-ItemProperty -Path $appPaths -Name 'Path' -Value $InstallDir
}
if ($Desktop) { Step "adding a Desktop shortcut" { New-Shortcut (Join-Path ([Environment]::GetFolderPath('Desktop')) 'wincan.lnk') } }
if ($Startup) { Step "starting wincan when you sign in" { Set-ItemProperty -Path $runKey -Name 'wincan' -Value ('"' + $exeDst + '"') } }

# ---- the settings, and this computer's key
$fingerprint = $null
Step "making sure $settingsDir exists" { New-Item -ItemType Directory -Force -Path $settingsDir | Out-Null }
if ($Server) {
    $hostport = ($Server -replace '^https?://', '')
    if ($hostport -notmatch ':\d+$') { $hostport += ':8766' }
    $secure = $Server -match '^https://'
    $stored = $(if ($secure) { "https://$hostport" } else { $hostport })
    Step "setting the server to $stored" { Set-Ini $ini 'server' $stored }
    if ($ServerPin) {
        $hex = ($ServerPin -replace '(?i)^sha256[:=]?', '' -replace '[:\s]', '').ToLower()
        if ($hex -notmatch '^[0-9a-f]{64}$') { throw "-ServerPin is not a SHA-256 fingerprint (SHA256: and 64 hexadecimal digits)." }
        Step "pinning the server's certificate" { Set-Ini $ini 'server_pin' "SHA256:$hex"; Set-Ini $ini 'server_pin_for' $hostport }
    }
    if ($secure -and -not $NoKey) {
        $thumb = Get-Ini $ini 'client_cert'
        $cert = $null
        if ($thumb) { $cert = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $thumb.ToUpper() } | Select-Object -First 1 }
        if ($cert) { Say "using this computer's existing key (thumbprint $($cert.Thumbprint))." }
        elseif ($DryRun) { Say "would: make this computer's key (an ECDSA P-256 certificate in Cert:\CurrentUser\My, not exportable)" }
        else {
            Say "making this computer's key..."
            $cert = New-SelfSignedCertificate -Subject "CN=wincan client ($env:USERNAME@$env:COMPUTERNAME)" -KeyAlgorithm ECDSA_nistP256 `
                -KeyExportPolicy NonExportable -KeyUsage DigitalSignature -CertStoreLocation Cert:\CurrentUser\My `
                -NotAfter (Get-Date).AddYears(20) -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.2')
            Set-Ini $ini 'client_cert' $cert.Thumbprint
        }
        if ($cert) {
            $sha = [System.Security.Cryptography.SHA256]::Create()
            $fingerprint = 'SHA256:' + (([BitConverter]::ToString($sha.ComputeHash($cert.RawData))) -replace '-', '').ToLower()
            Set-Content -Path (Join-Path $settingsDir 'client-fingerprint.txt') -Value $fingerprint -Encoding ASCII -ErrorAction SilentlyContinue
        }
    }
}

# ---- done
Say ''
if ($DryRun) { Say "(a dry run: nothing was changed)"; return }
Say "wincan is installed: $exeDst"
Say "Start it from the Start Menu, or press Win+R and type  wincan"
if ($fingerprint) {
    Say ''
    Say "This computer's key has the fingerprint"
    Say "    $fingerprint"
    Say "Have the server's administrator authorize it, on the machine that runs candled:"
    Say "    candled --authorize $fingerprint"
    Say "(it is also in $settingsDir\client-fingerprint.txt, and  wincan --fingerprint  prints it)"
}
if ($Launch) { Start-Process $exeDst }
