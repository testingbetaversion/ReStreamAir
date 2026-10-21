@echo off
setlocal EnableExtensions
title Reverse SOCKS tunnel
rem ---------------------------------------------------------------------------
rem Keeps a reverse SOCKS tunnel open from this PC to a server, reconnecting
rem forever. On the server, 127.0.0.1:<tunnel port> becomes a SOCKS5 proxy that
rem exits to the internet from THIS PC (e.g. a provider proxy of
rem socks5h://127.0.0.1:1080 for a geo-blocked origin).
rem
rem First run: asks for server, ports and user, creates an SSH key, and asks the
rem server password ONCE (typed at ssh's own prompt) to install that key.
rem Later runs reuse the saved settings, so a shortcut in the Startup folder
rem (Win+R, shell:startup) brings the tunnel back after a reboot unattended.
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
set "CFG=%SSHDIR%\reverse_tunnel.cfg"
if not exist "%SSHDIR%" mkdir "%SSHDIR%"

rem Kept outside a ( ) block: cmd expands %VAR% in a block before it runs.
if not exist "%CFG%" goto :ask
for /f "usebackq tokens=1,* delims==" %%A in ("%CFG%") do set "%%A=%%B"
echo Saved settings: %TUN_USER%@%TUN_HOST% ssh port %TUN_SSHPORT%, tunnel port %TUN_PORT%
choice /c YN /t 8 /d Y /m "Use them"
if not errorlevel 2 goto :haveconfig

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
    echo TUN_HOST=%TUN_HOST%
    echo TUN_SSHPORT=%TUN_SSHPORT%
    echo TUN_PORT=%TUN_PORT%
    echo TUN_USER=%TUN_USER%
)

:haveconfig
set "SSHOPTS=-i "%KEY%" -p %TUN_SSHPORT% -o StrictHostKeyChecking=accept-new -o IdentitiesOnly=yes"

if not exist "%KEY%" (
    echo Creating SSH key %KEY% ...
    ssh-keygen -q -t ed25519 -N "" -C "reverse-tunnel@%COMPUTERNAME%" -f "%KEY%" || (pause & exit /b 1)
)

rem Already authorised? Then no password is needed.
ssh %SSHOPTS% -o BatchMode=yes -o ConnectTimeout=15 %TUN_USER%@%TUN_HOST% exit >nul 2>&1
if not errorlevel 1 goto :autostart

echo.
echo Installing the key on %TUN_HOST%. Type the server password when ssh asks
echo (nothing shows while typing). This is needed only once.
set /p PUBKEY=<"%KEY%.pub"
ssh %SSHOPTS% -o ConnectTimeout=15 %TUN_USER%@%TUN_HOST% "umask 077; mkdir -p ~/.ssh; grep -qxF '%PUBKEY%' ~/.ssh/authorized_keys 2>/dev/null || echo '%PUBKEY%' >> ~/.ssh/authorized_keys"
ssh %SSHOPTS% -o BatchMode=yes -o ConnectTimeout=15 %TUN_USER%@%TUN_HOST% exit >nul 2>&1
if errorlevel 1 (
    echo Key login still fails - wrong password, user or port? Delete "%CFG%" to re-enter settings.
    pause
    exit /b 1
)
echo Key installed.

:autostart
rem Start this script at every logon of this user (re-created each run, so a moved
rem script stays correct). Runs unattended: saved settings auto-accept after 8s.
schtasks /create /tn ReverseSocksTunnel /tr "\"%~f0\"" /sc onlogon /delay 0000:30 /f >nul 2>&1 && echo Autostart enabled at logon. || echo Could not enable autostart - try running as administrator.

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
