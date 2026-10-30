@echo off
rem Stop PlayShell on the PS5: connects to PlayShell's stop port (TCP 9059) on the console.
rem After this a new playshell.elf always starts (a stuck older one is also killed by the new one itself).
rem   stop_playshell.bat 192.168.1.50      (the PS5's IP address; asked for when not given)
setlocal
set HOST=%1
if "%HOST%"=="" set /p HOST=PS5 IP address:
echo Stopping PlayShell on %HOST%...
powershell -NoProfile -Command "$c = New-Object Net.Sockets.TcpClient; try { $c.Connect('%HOST%', 9059); $r = New-Object IO.StreamReader($c.GetStream()); Write-Host $r.ReadLine(); $c.Close() } catch { Write-Host 'PlayShell not running (port 9059 refused)' }"
echo Done.
pause
