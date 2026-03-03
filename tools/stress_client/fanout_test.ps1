<#
.SYNOPSIS
    Spike 9 Test 3: Subscription Fan-Out Stress Test

.DESCRIPTION
    Seeds N fanout entities, starts a scheduled tick at M Hz,
    then subscribes and measures OnUpdate callback throughput.

    Uses spacetime CLI to control the server side, and
    spacetime subscribe to measure client-side fan-out.

.PARAMETER EntityCount
    Number of fan-out entities to seed (simulated "players").

.PARAMETER TickHz
    Tick frequency in Hz (e.g. 10 = every 100ms).

.PARAMETER DurationSec
    How long to let the tick run before stopping.

.PARAMETER Server
    SpacetimeDB server URL.

.PARAMETER Database
    SpacetimeDB database name.

.EXAMPLE
    .\fanout_test.ps1 -EntityCount 50 -TickHz 10 -DurationSec 15
    .\fanout_test.ps1 -EntityCount 200 -TickHz 10 -DurationSec 15
#>

param(
    [int]$EntityCount = 50,
    [int]$TickHz = 10,
    [int]$DurationSec = 15,
    [string]$Server = "local",
    [string]$Database = "nyx"
)

$installDir = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "SpacetimeDB"
$env:PATH = "$installDir;$env:USERPROFILE\.cargo\bin;$env:PATH"

$intervalMs = [math]::Floor(1000 / $TickHz)

Write-Host "=== Spike 9 Test 3: Subscription Fan-Out Stress ===" -ForegroundColor Cyan
Write-Host "Entities: $EntityCount | Tick: ${TickHz} Hz (${intervalMs}ms) | Duration: ${DurationSec}s"
Write-Host "Expected updates/tick: $EntityCount"
Write-Host "Expected updates/sec: $($EntityCount * $TickHz)"
Write-Host "If 1 subscriber: $($EntityCount * $TickHz) OnUpdate callbacks/sec"
Write-Host "If N subscribers: $($EntityCount * $TickHz) * N total events/sec (O(N^2) at N=$EntityCount)"
Write-Host ""

# Step 1: Reset any previous fan-out data
Write-Host "Resetting fan-out data..." -ForegroundColor Yellow
spacetime call $Database fanout_reset --server $Server --anonymous -y 2>&1 | Out-Null

# Step 2: Seed entities
Write-Host "Seeding $EntityCount fan-out entities..." -ForegroundColor Yellow
spacetime call $Database fanout_seed $EntityCount --server $Server --anonymous -y 2>&1

# Step 3: Start the fan-out tick
Write-Host "Starting fan-out tick at ${intervalMs}ms interval..." -ForegroundColor Yellow
spacetime call $Database fanout_start $intervalMs --server $Server --anonymous -y 2>&1

# Step 4: Start a subscriber in the background to measure fan-out
Write-Host "Starting subscriber (measuring for ${DurationSec}s)..." -ForegroundColor Green
$subscriberJob = Start-Job -ScriptBlock {
    param($installDir, $Database, $Server, $DurationSec)
    $env:PATH = "$installDir;$env:USERPROFILE\.cargo\bin;$env:PATH"
    $output = spacetime subscribe $Database "SELECT * FROM fanout_entity" --server $Server --timeout $DurationSec --anonymous -y 2>&1
    return $output
} -ArgumentList $installDir, $Database, $Server, $DurationSec

# Step 5: Wait for the measurement window
Start-Sleep -Seconds $DurationSec

# Step 6: Stop the fan-out tick
Write-Host "Stopping fan-out tick..." -ForegroundColor Yellow
spacetime call $Database fanout_stop --server $Server --anonymous -y 2>&1 | Out-Null

# Step 7: Query tick logs for server-side timing
Write-Host ""
Write-Host "=== Server-Side Tick Log (last 20) ===" -ForegroundColor Cyan
spacetime sql $Database "SELECT * FROM fanout_tick_log ORDER BY tick_number DESC LIMIT 20" --server $Server 2>&1

# Step 8: Get the subscriber output
Write-Host ""
Write-Host "=== Subscriber Output ===" -ForegroundColor Cyan
$subResult = Receive-Job -Job $subscriberJob -Wait -AutoRemoveJob -ErrorAction SilentlyContinue
if ($subResult) {
    # Count update lines as a rough measure
    $lines = ($subResult | Out-String).Split("`n")
    $updateCount = ($lines | Where-Object { $_ -match "update|Update|row" }).Count
    Write-Host "Subscriber received ~$updateCount update events in ${DurationSec}s"
    Write-Host "Rate: ~$([math]::Round($updateCount / $DurationSec, 1)) events/sec"
} else {
    Write-Host "No subscriber output captured (subscriber may have timed out or errored)"
}

# Step 9: Summary
Write-Host ""
Write-Host "=== Summary ===" -ForegroundColor Cyan
$tickCount = spacetime sql $Database "SELECT COUNT(*) FROM fanout_tick_log" --server $Server 2>&1
Write-Host "Total ticks recorded: $tickCount"
Write-Host "Expected ticks: $($TickHz * $DurationSec)"
Write-Host ""
Write-Host "To see SpacetimeDB server logs: spacetime logs nyx --server $Server"
Write-Host "Done!" -ForegroundColor Green
