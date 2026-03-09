# Launch test: 2 dedicated servers + 1 proxy + 1 client
# Usage: powershell -ExecutionPolicy Bypass -File C:\UE\Nyx\launch_test.ps1
$proj = "C:\UE\Nyx\Nyx.uproject"
$ue   = "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"

# Kill existing
Get-Process -Name "UnrealEditor*" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 3

# Clean logs
Remove-Item "C:\UE\Nyx\server1_log.txt","C:\UE\Nyx\server2_log.txt","C:\UE\Nyx\proxy1_log.txt","C:\UE\Nyx\client1_log.txt" -ErrorAction SilentlyContinue

# Server-1: West zone (X < 0), port 7777
# Game mesh: port 15000, DSTM beacon: port 16000
Start-Process $ue -ArgumentList "`"$proj`" -server -port=7777 -log -NOSTEAM -DedicatedServerId=server-1 -ZoneSide=west -NyxMultiServerListenPort=15000 -MultiServerPeers=127.0.0.1:15001 -DSTMListenPort=16000 -DSTMPeers=127.0.0.1:16001 -ABSLOG=`"C:\UE\Nyx\server1_log.txt`""
Write-Host "[1/4] Server-1 (West) on port 7777, game mesh 15000, DSTM beacon 16000"
Start-Sleep -Seconds 2

# Server-2: East zone (X >= 0), port 7778
# Game mesh: port 15001, DSTM beacon: port 16001
Start-Process $ue -ArgumentList "`"$proj`" -server -port=7778 -log -NOSTEAM -DedicatedServerId=server-2 -ZoneSide=east -NyxMultiServerListenPort=15001 -MultiServerPeers=127.0.0.1:15000 -DSTMListenPort=16001 -DSTMPeers=127.0.0.1:16000 -ABSLOG=`"C:\UE\Nyx\server2_log.txt`""
Write-Host "[2/4] Server-2 (East) on port 7778, game mesh 15001, DSTM beacon 16001"

# Wait for servers
Write-Host "Waiting 15s for servers..."
Start-Sleep -Seconds 15

# Proxy: uses -ProxyGameServers= to activate via NyxGameInstance
Start-Process $ue -ArgumentList "`"$proj`" -server -port=7780 -log -NOSTEAM -ProxyGameServers=127.0.0.1:7777,127.0.0.1:7778 -ABSLOG=`"C:\UE\Nyx\proxy1_log.txt`""
Write-Host "[3/4] Proxy on port 7780"

# Wait for proxy
Write-Host "Waiting 25s for proxy..."
Start-Sleep -Seconds 25

# Client: windowed game connecting to proxy
Start-Process $ue -ArgumentList "`"$proj`" 127.0.0.1:7780 -game -WINDOWED -ResX=800 -ResY=600 -WinX=50 -WinY=50 -NOSTEAM -ABSLOG=`"C:\UE\Nyx\client1_log.txt`""
Write-Host "[4/4] Client connecting to 127.0.0.1:7780"

Start-Sleep -Seconds 3
$count = @(Get-Process -Name "UnrealEditor*" -ErrorAction SilentlyContinue).Count
Write-Host "All launched ($count processes). Logs in C:\UE\Nyx\"
