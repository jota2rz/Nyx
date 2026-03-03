<#
.SYNOPSIS
    Spike 9 Test 2: Multi-Client Reducer Contention Stress Test

.DESCRIPTION
    Spawns N parallel processes, each calling stress_move at a target rate.
    After a measurement window, records the results via stress_record.

    Requires: spacetime CLI in PATH, local SpacetimeDB server running,
    nyx database published with Spike 9 reducers.

.PARAMETER ClientCount
    Number of parallel "clients" (processes) to spawn.

.PARAMETER TargetHz
    Target calls per second per client.

.PARAMETER DurationSec
    How long to run the test in seconds.

.PARAMETER Server
    SpacetimeDB server URL.

.PARAMETER Database
    SpacetimeDB database name.

.EXAMPLE
    .\stress_test.ps1 -ClientCount 10 -TargetHz 10 -DurationSec 10
#>

param(
    [int]$ClientCount = 10,
    [int]$TargetHz = 10,
    [int]$DurationSec = 10,
    [string]$Server = "local",
    [string]$Database = "nyx"
)

$installDir = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "SpacetimeDB"
$env:PATH = "$installDir;$env:USERPROFILE\.cargo\bin;$env:PATH"

Write-Host "=== Spike 9 Test 2: Multi-Client Reducer Contention ===" -ForegroundColor Cyan
Write-Host "Clients: $ClientCount | Target: ${TargetHz} Hz/client | Duration: ${DurationSec}s"
Write-Host "Expected total: $($ClientCount * $TargetHz) calls/sec"
Write-Host ""

# Step 1: Seed bench entities (one per client) — synchronous
Write-Host "Seeding $ClientCount bench entities..." -ForegroundColor Yellow
spacetime call $Database bench_seed $ClientCount --server $Server --anonymous -y 2>&1 | Out-Null

# Step 2: Reset stress counter
spacetime call $Database stress_reset --server $Server --anonymous -y 2>&1 | Out-Null

# Step 3: Spawn N background jobs
$sleepMs = [math]::Max(1, [math]::Floor(1000 / $TargetHz))
$totalCalls = $TargetHz * $DurationSec

Write-Host "Spawning $ClientCount clients (${sleepMs}ms between calls, $totalCalls calls each)..." -ForegroundColor Yellow

$jobs = @()
for ($i = 1; $i -le $ClientCount; $i++) {
    $entityId = $i  # Each client targets a different entity
    $job = Start-Job -ScriptBlock {
        param($installDir, $Database, $Server, $entityId, $totalCalls, $sleepMs)
        $env:PATH = "$installDir;$env:USERPROFILE\.cargo\bin;$env:PATH"
        for ($c = 0; $c -lt $totalCalls; $c++) {
            spacetime call $Database stress_move $entityId 1.0 0.5 --server $Server --anonymous -y 2>&1 | Out-Null
            Start-Sleep -Milliseconds $sleepMs
        }
    } -ArgumentList $installDir, $Database, $Server, $entityId, $totalCalls, $sleepMs
    $jobs += $job
}

Write-Host "All $ClientCount clients running. Waiting ${DurationSec}s..." -ForegroundColor Green

# Step 4: Wait for completion (with timeout)
$timeoutSec = $DurationSec + 60  # Extra buffer for slow calls
$completed = Wait-Job -Job $jobs -Timeout $timeoutSec

# Step 5: Record results
$windowMs = $DurationSec * 1000
Write-Host "Recording results..." -ForegroundColor Yellow
spacetime call $Database stress_record $ClientCount $windowMs --server $Server --anonymous -y 2>&1

# Step 6: Query results
Write-Host ""
Write-Host "=== Results ===" -ForegroundColor Cyan
spacetime sql $Database "SELECT * FROM stress_result" --server $Server 2>&1

# Step 7: Cleanup
$jobs | Remove-Job -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done!" -ForegroundColor Green
