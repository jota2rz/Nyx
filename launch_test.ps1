# Launch test: 2 dedicated servers + 1 proxy + 2 clients
# Usage: powershell -ExecutionPolicy Bypass -File C:\UE\Nyx\launch_test.ps1
$proj   = "C:\UE\Nyx\Nyx.uproject"
$server = "C:\UE\Nyx\Binaries\Win64\NyxServer.exe"
$editor = "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"

# Kill existing
Get-Process -Name "NyxServer","UnrealEditor" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 3

# Clean old UE logs
$logDir = "C:\UE\Nyx\Saved\Logs"
Remove-Item "$logDir\Nyx*.log" -ErrorAction SilentlyContinue

$commonArgs = "-log -NOSTEAM -DisableGarbageElimination -NOSPLASH -NOSOUND"

# Proxy first: listens for game server registrations via beacon on port 17000
$proxyArgs = "`"$proj`" -port=7780 $commonArgs -DedicatedServerId=proxy-1 -ProxyRegistrationPort=17000"
Start-Process $server -ArgumentList $proxyArgs
Write-Host "[1/5] Proxy on port 7780, registration beacon on port 17000"
Start-Sleep -Seconds 10

# Server-1: West zone (X < 0), port 7777
# Registers with proxy, gets peer list for mesh/DSTM via beacon
$s1Args = "`"$proj`" -port=7777 $commonArgs -DedicatedServerId=server-1 -ZoneSide=west -NyxMultiServerListenPort=15000 -DSTMListenPort=16000 -JoinProxy=127.0.0.1:17000"
Start-Process $server -ArgumentList $s1Args
Write-Host "[2/5] Server-1 (West) on port 7777 → JoinProxy 127.0.0.1:17000"
Start-Sleep -Seconds 5

# Server-2: East zone (X >= 0), port 7778
$s2Args = "`"$proj`" -port=7778 $commonArgs -DedicatedServerId=server-2 -ZoneSide=east -NyxMultiServerListenPort=15001 -DSTMListenPort=16001 -JoinProxy=127.0.0.1:17000"
Start-Process $server -ArgumentList $s2Args
Write-Host "[3/5] Server-2 (East) on port 7778 → JoinProxy 127.0.0.1:17000"

# Wait for servers to register with proxy and connect
Write-Host "Waiting 25s for servers to register and proxy to connect..."
Start-Sleep -Seconds 25

# Client 1: windowed game connecting to proxy
Start-Process $editor -ArgumentList "`"$proj`" 127.0.0.1:7780 -game -WINDOWED -ResX=800 -ResY=600 -WinX=50 -WinY=50 -NOSTEAM -ABSLOG=`"C:\UE\Nyx\client1_log.txt`""
Write-Host "[4/5] Client 1 connecting to 127.0.0.1:7780"

Start-Sleep -Seconds 5

# Client 2: windowed game connecting to proxy (offset window)
Start-Process $editor -ArgumentList "`"$proj`" 127.0.0.1:7780 -game -WINDOWED -ResX=800 -ResY=600 -WinX=900 -WinY=50 -NOSTEAM -ABSLOG=`"C:\UE\Nyx\client2_log.txt`""
Write-Host "[5/5] Client 2 connecting to 127.0.0.1:7780"

Start-Sleep -Seconds 3
$serverCount = @(Get-Process -Name "NyxServer" -ErrorAction SilentlyContinue).Count
$editorCount = @(Get-Process -Name "UnrealEditor" -ErrorAction SilentlyContinue).Count
Write-Host "All launched ($serverCount server(s) + $editorCount editor(s))."
