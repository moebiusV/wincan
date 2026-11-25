wincan for Windows
==================

wincan.exe is one file with no dependency: it needs only what Windows has.  install.ps1 puts it in a folder
of its own, adds the Start Menu entry and Win+R, and sets up the encrypted connection to a candle server.

Install (PowerShell, no administrator rights needed):

    powershell -ExecutionPolicy Bypass -File install.ps1 -Server https://HOST:8766 -ServerPin SHA256:... -Launch

or double-click install.cmd (it asks nothing: give the options on its command line).  -Server and -ServerPin are
what the server's administrator tells you (candled --fingerprint prints the pin).  The installer makes this
computer's key (an ECDSA P-256 certificate in your personal certificate store, not exportable) and prints its
fingerprint; the administrator authorizes it with   candled --authorize SHA256:...   and wincan connects.

Other options: -InstallDir, -SettingsDir, -Desktop, -Startup, -NoKey, -StopRunning, -Launch, -DryRun (say what would be done).
Remove it with uninstall.ps1 (-Purge also removes the settings, the trade log and the key).

For a server on the same computer in plain HTTP (a quick test), -Server 127.0.0.1:8766 needs no key.
Settings: %APPDATA%\wincan\wincan.ini.  The manual pages (wincan.1, wincan.5, candled.8) are in the source.
