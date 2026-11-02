@echo off
setlocal EnableExtensions
title Reverse SOCKS tunnel
rem ---------------------------------------------------------------------------
rem Keeps a reverse SOCKS tunnel open from this PC to a server, reconnecting
rem forever. On the server, 127.0.0.1:<tunnel port> becomes a SOCKS5 proxy that
rem exits to the internet from THIS PC (e.g. a provider proxy of
rem socks5h://127.0.0.1:1080 for a geo-blocked origin).
rem
rem First run: asks for server, ports and user, saves them to a config file next
rem to this script (reverse-socks-tunnel.cfg - same name, .cfg), creates an SSH
rem key, and asks the server password ONCE (typed at ssh's own prompt) to
rem install that key. Later runs read the config file without asking anything,
rem so the logon task brings the tunnel back after a reboot unattended.
rem
rem To change settings, edit or delete the .cfg file, or run:
rem     reverse-socks-tunnel.bat /setup
rem A second tunnel from the same PC (e.g. port 1888): copy this script under
rem another name (reverse-socks-tunnel-1888.bat); it gets its own .cfg and its
rem own logon task.
rem
rem Needs the Windows OpenSSH client (built into Windows 10 1809+ / 11).
rem ---------------------------------------------------------------------------

where ssh >nul 2>&1 || (
    echo The OpenSSH client is not installed.
    echo Settings ^> Apps ^> Optional features ^> Add a feature ^> OpenSSH Client
    pause
    exit /b 1
)

set "SSHDIR=%USERPROFILE%\.ssh"
set "KEY=%SSHDIR%\reverse_tunnel_ed25519"
rem Settings live next to this script, named after it: copies under other names
rem keep their own settings.
set "CFG=%~dpn0.cfg"
rem Where older versions kept the settings, one file per Windows user.
set "OLDCFG=%SSHDIR%\reverse_tunnel.cfg"
set "TASK=ReverseSocksTunnel"
if /i not "%~n0"=="reverse-socks-tunnel" set "TASK=ReverseSocksTunnel-%~n0"
if not exist "%SSHDIR%" mkdir "%SSHDIR%"
echo Config file:   "%CFG%"
echo Config folder: "%~dp0"

if /i "%~1"=="/setup" goto :ask
if not exist "%CFG%" if exist "%OLDCFG%" (
    copy /y "%OLDCFG%" "%CFG%" >nul && echo Copied the saved settings to "%CFG%".
)
rem Kept outside a ( ) block: cmd expands %VAR% in a block before it runs.
if not exist "%CFG%" goto :ask
set "TUN_HOST="
set "TUN_SSHPORT=22"
set "TUN_PORT=1080"
set "TUN_USER=root"
for /f "usebackq eol=# tokens=1,* delims==" %%A in ("%CFG%") do set "%%A=%%B"
if not defined TUN_HOST (
    echo "%CFG%" has no TUN_HOST - asking for the settings again.
    goto :ask
)
echo Using saved settings: %TUN_USER%@%TUN_HOST% ssh port %TUN_SSHPORT%, tunnel port %TUN_PORT%
goto :haveconfig

:ask
set "TUN_HOST="
set /p "TUN_HOST=Server IP or hostname: "
if not defined TUN_HOST goto :ask
set "TUN_SSHPORT=22"
set /p "TUN_SSHPORT=SSH port [22]: "
set "TUN_PORT=1080"
set /p "TUN_PORT=Tunnel (SOCKS) port on the server [1080]: "
set "TUN_USER=root"
set /p "TUN_USER=SSH user [root]: "
set /a "_chk=TUN_PORT" 2>nul
if not "%_chk%"=="%TUN_PORT%" (echo Tunnel port must be a number. & goto :ask)
set /a "_chk=TUN_SSHPORT" 2>nul
if not "%_chk%"=="%TUN_SSHPORT%" (echo SSH port must be a number. & goto :ask)

> "%CFG%" (
    echo # Reverse SOCKS tunnel settings, read by %~nx0 on every start.
    echo # Edit to change them, or delete this file to be asked again.
    echo TUN_HOST=%TUN_HOST%
    echo TUN_SSHPORT=%TUN_SSHPORT%
    echo TUN_PORT=%TUN_PORT%
    echo TUN_USER=%TUN_USER%
) || (
    echo Could not write "%CFG%" - move this script to a folder you can write to.
    pause
    exit /b 1
)
echo Saved the settings to "%CFG%".

:haveconfig
set "SSHOPTS=-i "%KEY%" -p %TUN_SSHPORT% -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes"

if not exist "%KEY%" (
    echo Creating SSH key %KEY% ...
    ssh-keygen -q -t ed25519 -N "" -C "reverse-tunnel@%COMPUTERNAME%" -f "%KEY%" || (pause & exit /b 1)
)

rem Already authorised? Then no password is needed. -n and <nul: with the
rem console as stdin, Windows ssh can stay open after the remote "exit".
echo Checking the key login on %TUN_HOST% ...
ssh %SSHOPTS% -n -o BatchMode=yes -o ConnectTimeout=15 %TUN_USER%@%TUN_HOST% exit <nul >nul 2>&1
if not errorlevel 1 goto :autostart

echo.
echo Installing the key on %TUN_HOST%. Type the server password when ssh asks
echo (nothing shows while typing). This is needed only once.
set /p PUBKEY=<"%KEY%.pub"
ssh %SSHOPTS% -o ConnectTimeout=15 %TUN_USER%@%TUN_HOST% "umask 077; mkdir -p ~/.ssh; grep -qxF '%PUBKEY%' ~/.ssh/authorized_keys 2>/dev/null || echo '%PUBKEY%' >> ~/.ssh/authorized_keys"
ssh %SSHOPTS% -n -o BatchMode=yes -o ConnectTimeout=15 %TUN_USER%@%TUN_HOST% exit <nul >nul 2>&1
if errorlevel 1 (
    echo Key login still fails - wrong password, user or port? Run "%~nx0 /setup" to re-enter settings.
    pause
    exit /b 1
)
echo Key installed.

:autostart
rem Start this script at every logon of this user (re-created each run, so a moved
rem script stays correct). Runs unattended: the .cfg is read without asking.
rem <nul: if schtasks wants a password it fails instead of waiting unseen.
schtasks /create /tn "%TASK%" /tr "\"%~f0\"" /sc onlogon /delay 0000:30 /f <nul >nul 2>&1 && echo Autostart enabled at logon ^(task %TASK%^). || echo Could not enable autostart - try running as administrator.

:loop
echo [%date% %time%] Connecting: %TUN_HOST% 127.0.0.1:%TUN_PORT% ^<- SOCKS via this PC
rem ServerAlive*: notice a dead link in ~45s instead of hanging.
rem ExitOnForwardFailure: if the port is still held on the server, exit and
rem retry rather than sit connected with no tunnel.
ssh %SSHOPTS% -N -o BatchMode=yes -o ConnectTimeout=20 ^
    -o ServerAliveInterval=15 -o ServerAliveCountMax=3 -o TCPKeepAlive=yes ^
    -o ExitOnForwardFailure=yes ^
    -R 127.0.0.1:%TUN_PORT% %TUN_USER%@%TUN_HOST%
echo [%date% %time%] Tunnel dropped (exit %errorlevel%). Reconnecting in 5s - close this window to stop.
timeout /t 5 /nobreak >nul
goto :loop
