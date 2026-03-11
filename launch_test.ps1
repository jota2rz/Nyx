# Launch test: 1 proxy + 2 dedicated servers (dynamic beacon registration) + 1 client
# Usage: powershell -ExecutionPolicy Bypass -File C:\UE\Nyx\launch_test.ps1
$proj   = "C:\UE\Nyx\Nyx.uproject"
$server = "C:\UE\Nyx\Binaries\Win64\NyxServer.exe"
$editor = "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"

# Kill existing
Get-Process -Name "NyxServer","UnrealEditor" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 3

# Clean logs — NyxServer ignores -ABSLOG so we redirect stdout instead
Remove-Item "C:\UE\Nyx\server1_log.txt","C:\UE\Nyx\server2_log.txt","C:\UE\Nyx\proxy1_log.txt","C:\UE\Nyx\client1_log.txt","C:\UE\Nyx\client2_log.txt" -ErrorAction SilentlyContinue

$commonArgs = "-log -NOSTEAM -DisableGarbageElimination -NOSPLASH -NOSOUND"

# Proxy starts FIRST — listens for dynamic game server registrations via beacon
# Registration beacon on port 17000; clients connect to proxy on port 7780
$proxyArgs = "`"$proj`" -port=7780 $commonArgs -DedicatedServerId=proxy-1 -ProxyRegistrationPort=17000"
Start-Process $server -ArgumentList $proxyArgs -RedirectStandardOutput "C:\UE\Nyx\proxy1_log.txt" -RedirectStandardError "C:\UE\Nyx\proxy1_err.txt"
Write-Host "[1/4] Proxy on port 7780, registration beacon on port 17000"

# Wait for proxy to be ready
Write-Host "Waiting 20s for proxy..."
Start-Sleep -Seconds 20

# Server-1: West zone (X < 0), port 7777
# Connects to proxy beacon at 127.0.0.1:17000 to register itself
# Game mesh: port 15000, DSTM beacon: port 16000
$s1Args = "`"$proj`" -port=7777 $commonArgs -DedicatedServerId=server-1 -ZoneSide=west -JoinProxy=127.0.0.1:17000 -GameServerAddress=127.0.0.1:7777 -NyxMultiServerListenPort=15000 -MultiServerPeers=127.0.0.1:15001 -DSTMListenPort=16000"
Start-Process $server -ArgumentList $s1Args -RedirectStandardOutput "C:\UE\Nyx\server1_log.txt" -RedirectStandardError "C:\UE\Nyx\server1_err.txt"
Write-Host "[2/4] Server-1 (West) on port 7777, joining proxy beacon"
Start-Sleep -Seconds 2

# Server-2: East zone (X >= 0), port 7778
# Connects to proxy beacon at 127.0.0.1:17000 to register itself
# Game mesh: port 15001, DSTM beacon: port 16001
$s2Args = "`"$proj`" -port=7778 $commonArgs -DedicatedServerId=server-2 -ZoneSide=east -JoinProxy=127.0.0.1:17000 -GameServerAddress=127.0.0.1:7778 -NyxMultiServerListenPort=15001 -MultiServerPeers=127.0.0.1:15000 -DSTMListenPort=16001"
Start-Process $server -ArgumentList $s2Args -RedirectStandardOutput "C:\UE\Nyx\server2_log.txt" -RedirectStandardError "C:\UE\Nyx\server2_err.txt"
Write-Host "[3/4] Server-2 (East) on port 7778, joining proxy beacon"

# Wait for servers to boot and register with proxy
Write-Host "Waiting 20s for servers to register..."
Start-Sleep -Seconds 20

# Client 1: windowed game connecting to proxy
Start-Process $editor -ArgumentList "`"$proj`" 127.0.0.1:7780 -game -WINDOWED -ResX=800 -ResY=600 -WinX=50 -WinY=50 -NOSTEAM -ABSLOG=`"C:\UE\Nyx\client1_log.txt`""
Write-Host "[4/4] Client 1 connecting to 127.0.0.1:7780"

Start-Sleep -Seconds 3
$serverCount = @(Get-Process -Name "NyxServer" -ErrorAction SilentlyContinue).Count
$editorCount = @(Get-Process -Name "UnrealEditor" -ErrorAction SilentlyContinue).Count
Write-Host "All launched ($serverCount server(s) + $editorCount editor(s))."
