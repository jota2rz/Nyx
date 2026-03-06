# The MultiServer Plugin has ZERO GUID coordination

After an exhaustive search of every `.h` and `.cpp` in the plugin, there is **no mechanism** for coordinating GUID ranges between servers. No seed, no offset, no range partitioning, no beacon protocol message for GUID negotiation.

## How GUIDs work (the full chain)

**GUID encoding** - `Engine/Source/Runtime/Core/Public/Misc/NetworkGuid.h` lines 113-119:
```cpp
static FNetworkGUID CreateFromIndex(uint64 NetIndex, bool bIsStatic)
{
    FNetworkGUID NewGuid;
    NewGuid.ObjectId = NetIndex << 1 | (bIsStatic ? 1 : 0);
    return NewGuid;
}
```
A `FNetworkGUID` is just a `uint64` - the index shifted left by 1 bit, with bit 0 indicating static/dynamic. **No server ID bits, no partition bits.** It's a bare counter.

**GUID assignment** - `Engine/Source/Runtime/Engine/Private/PackageMapClient.cpp` line 3294:
```cpp
const FNetworkGUID NewNetGuid = FNetworkGUID::CreateFromIndex(++NetworkGuidIndex[IsStatic], IsStatic != 0);
```
Each server increments its own `NetworkGuidIndex` counter starting from the same seed (default 0). So Server1 and Server2 **both produce GUID 2, 4, 6, 8...** for dynamic objects.

**Proxy shared cache** - `MultiServerProxy.cpp` line 306:
```cpp
GameServerConnectionState->NetDriver->GuidCache = ProxyNetDriver->GetSharedBackendNetGuidCache();
```
ALL backend drivers share a **single** `FProxyBackendNetGUIDCache`. When Server1 sends "GUID 4 = PlayerController_0" and Server2 later sends "GUID 4 = NoPawnPlayerController_0", the collision goes through `RegisterNetGUID_Client` which:
1. Logs a warning: *"Reassigning NetGUID <4> to NoPawnPlayerController_0 (was assigned to PlayerController_0)"*
2. **Removes** the old Object->GUID mapping for PlayerController_0
3. **Overwrites** the GUID->Object mapping with NoPawnPlayerController_0
4. Now Server1's replication data for what it thinks is PlayerController_0 gets applied to NoPawnPlayerController_0 -> type mismatch -> `ObjectReplicatorReceivedBunchFail` -> **crash**

## What the plugin DOES have (and what it doesn't help with)

- **`FProxyNetGUIDCache::AssignNewNetGUID_Server`** - Proxy looks up GUIDs from backend cache, never assigns its own. Helps with GUID collision? **No** - just passes through.

- **`FProxyBackendNetGUIDCache::IsNetGUIDAuthority() = false`** - Backend cache is a receiver, not an assigner. Helps with GUID collision? **No** - can't reject collisions.

- **Migration-aware GUID preservation (line 660)** - Keeps GUID->Object mapping during actor migration between servers. Helps with GUID collision? **Only for migrated actors**, not for independent spawns.

- **`RemoveActorNetGUIDs` cleanup (line 956)** - Removes GUID when actor is destroyed. Helps with GUID collision? **No** - doesn't prevent initial collision.

## Conclusion

The plugin assumes each game server manages a **disjoint set of actors** (zone-based ownership) so that GUIDs naturally don't collide for replicated actors. But it completely ignores that:
- Both servers create **their own** `PlayerController`, `PlayerState`, `Pawn` etc. for each connecting player
- Engine internals like `NoPawnPlayerController` get spawned on both servers
- All of these get sequential GUIDs starting from the same counter

**`-NetworkGuidSeed=` is the correct fix.** The plugin has no alternative.

## Production Fix (Shipping Builds)

The engine's `-NetworkGuidSeed=` command-line parameter is gated by `#if !UE_BUILD_SHIPPING` in `PackageMapClient.cpp:3004`, so it won't work in Shipping builds. However, the `FNetGUIDCache` constructor that accepts the seed is `ENGINE_API` public and the seed application (`NetworkGuidIndex[0] = NetworkGuidIndex[1] = NetworkGuidSeed`) is **unconditional** - it works in all build configs.

**Solution implemented in `NyxGameMode::StartPlay()`:** Replace the NetDriver's `GuidCache` with a new seeded instance before any clients connect. GUIDs are assigned lazily during replication, so the cache is empty at `StartPlay()` time.

```cpp
// NyxGameMode.cpp - StartPlay()
uint64 GuidSeed = 0;
FParse::Value(FCommandLine::Get(), TEXT("-NyxGuidSeed="), GuidSeed);
if (GuidSeed > 0)
{
    UNetDriver* NetDriver = GetWorld()->GetNetDriver();
    if (NetDriver)
    {
        NetDriver->GuidCache = TSharedPtr<FNetGUIDCache>(new FNetGUIDCache(NetDriver, GuidSeed));
    }
}
```

**Usage - launch each server with a unique seed:**
```
Server1: -NyxGuidSeed=100000
Server2: -NyxGuidSeed=200000
Server3: -NyxGuidSeed=300000
```

This replaces the engine's `-NetworkGuidSeed=` parameter with our own `-NyxGuidSeed=` that works in **all build configs including Shipping**. No engine modification required.

---

# Proxy Routing: Primary Server Selection Flags

When a client connects to a proxy, the proxy creates connections to **all** registered game servers. One server is designated the **primary** (spawns the client's pawn + `PlayerController`), while others are **non-primary** (spawns `ANoPawnPlayerController`, no pawn — receives replication only).

## Command-Line Flags

| Flag | Default | Effect |
|------|---------|--------|
| *(none)* | Index 0 | All clients get the first server in `-ProxyGameServers=` list as primary |
| `-ProxyClientPrimaryGameServer=N` | — | All clients get server at index N (0-based) as primary |
| `-ProxyClientPrimaryGameServer=random` | — | First client gets a random primary server (**one-shot**: resets after first client) |
| `-ProxyCyclePrimaryGameServer` | `false` | Round-robin: each new client gets the next server index. Wraps around. |

## Behavior Details

- **`-ProxyClientPrimaryGameServer=random`** is **one-shot** — after the first client connects, `bRandomizePrimaryGameServerForNextClient` is set to `false`. Only the first client gets randomized. Subsequent clients get whatever index the random pick produced (unless cycling is also enabled).
- **Cycling** (`-ProxyCyclePrimaryGameServer`) increments `PrimaryGameServerForNextClient` by 1 after each client join, wrapping via modulo.
- **Combination**: `-ProxyClientPrimaryGameServer=1 -ProxyCyclePrimaryGameServer` starts at index 1 and cycles (1 → 2 → 0 → 1 → ...).

## Source Location

- **Parsing**: `UProxyNetDriver::InitBase()` in `MultiServerProxy.cpp`
- **Selection**: `UProxyListenerNotify::NotifyControlMessage()` — handles `NMT_Join`, selects primary, calls `ConnectToGameServer()` for each backend with appropriate `EJoinFlags`.

## Example: 3 Servers, Force One Proxy to Server-3

```powershell
$ue = "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
$proj = "C:\UE\Nyx\Nyx.uproject"

# Server-1 (Zone-1 West, port 7777, GUID seed 100000)
# Server-2 (Zone-2 East, port 7778, GUID seed 200000)
# Server-3 (Zone-1 West overlapping, port 7779, GUID seed 300000)

# Proxy-1: default → clients get Server-1 (index 0) as primary
Start-Process $ue -ArgumentList "$proj -server -port=7780 -ProxyGameServers=127.0.0.1:7777,127.0.0.1:7778,127.0.0.1:7779 -NOSTEAM"

# Proxy-2: force Server-3 (index 2) as primary
Start-Process $ue -ArgumentList "$proj -server -port=7781 -ProxyGameServers=127.0.0.1:7777,127.0.0.1:7778,127.0.0.1:7779 -ProxyClientPrimaryGameServer=2 -NOSTEAM"

# Client-1 → Proxy-1 → primary on Server-1
Start-Process $ue -ArgumentList "$proj 127.0.0.1:7780 -game -WINDOWED"

# Client-2 → Proxy-2 → primary on Server-3
Start-Process $ue -ArgumentList "$proj 127.0.0.1:7781 -game -WINDOWED"
```

---

# Seamless Pawn Authority Migration (Proxy-Based)

When a proxy-connected player crosses a zone boundary, the pawn's authority transfers seamlessly between game servers. The client never disconnects — the proxy handles the transition transparently.

## How It Works

The proxy's `ReassignPlayerController()` (MultiServerProxy.cpp:1236) monitors for two events:

1. **Server A** (releasing): swaps the real `PlayerController` → `ANoPawnPlayerController` via `SwapPlayerControllers()`
2. **Server B** (claiming): swaps the `ANoPawnPlayerController` → new real `PlayerController` via `SwapPlayerControllers()`

These events can arrive in **any order**. The proxy tracks them in `FPlayerControllerReassignment` and calls `FinalizePlayerControllerReassignment()` once both are received, flipping the primary route from A to B.

## Server-Side Implementation

### Server A — ReleasePawnAuthority

Triggered when `CheckZoneBoundaries()` detects a proxy player's pawn has left this server's zone:

1. Save pawn state to SpacetimeDB (`SaveCharacterState`, `OnPlayerLeft`)
2. Record last position/rotation
3. `PC->UnPossess()` + `NyxChar->Destroy()`
4. Spawn `ANoPawnPlayerController` at last position
5. `SwapPlayerControllers(RealPC, NoPawnPC)` — proxy detects NoPawnPC

### Server B — ClaimPawnAuthority

Triggered when `CheckZoneBoundaries()` finds a `ANoPawnPlayerController` on a `UChildConnection` whose position is inside this server's zone:

1. Get position via `PC->GetViewTarget()->GetActorLocation()`
2. Spawn `ANyxCharacter` at that position
3. Spawn new `APlayerController` (using `PlayerControllerClass`)
4. `SwapPlayerControllers(NoPawnPC, NewPC)` — proxy detects real PC
5. `NewPC->Possess(NewChar)`, set HUD props, register with SpacetimeDB

## Key Technical Details

- **`AController::GetActorLocation()` is hidden** by `HIDE_ACTOR_TRANSFORM_FUNCTIONS()`. Access NoPawnPC position through `GetViewTarget()->GetActorLocation()` instead.
- **`SwapPlayerControllers()`** is built into `AGameModeBase` — transfers `Player`, `NetConnection`, `NetPlayerIndex`. No custom implementation needed.
- **Grace period** (5 seconds) prevents immediate bounce-back after migration.
- **Three player types** in `CheckZoneBoundaries()`: NoPawnPC (claim), real PC with pawn (release/HUD update), direct-connect (ClientTravel fallback).

## Proxy Internals

| Component | Purpose |
|-----------|---------|
| `FPlayerControllerReassignment` | Tracks both migration events per proxy connection |
| `ReceivedReassignedNoPawnPlayerController()` | Called when Server A produces NoPawnPC |
| `ReceivedReassignedGamePlayerController()` | Called when Server B produces real PC |
| `FinalizePlayerControllerReassignment()` | Flips primary: old route → `Connected`, new route → `ConnectedPrimary` |
| `EProxyConnectionState::PendingReassign` | Intermediate state while waiting for both events |
| `EChannelCloseReason::Migrated` | Prevents premature actor destruction during migration |

## Source Locations

| File | Lines | Content |
|------|-------|---------|
| `MultiServerProxy.cpp` | 1214–1250 | `ReassignPlayerController()` — migration detection + dispatch |
| `MultiServerProxy.cpp` | 1389–1416 | `ReceivedReassignedNoPawnPlayerController()` |
| `MultiServerProxy.cpp` | 1418–1452 | `ReceivedReassignedGamePlayerController()` |
| `MultiServerProxy.cpp` | 1454–1478 | `FinalizePlayerControllerReassignment()` |
| `MultiServerProxy.h` | 145–165 | `FPlayerControllerReassignment` struct |
| `PlayerController.h` | 2353+ | `ANoPawnPlayerController` class |
| `PlayerController.cpp` | 6690+ | `ServerSetViewTargetPosition_Implementation` — stores position via `SetActorLocation()` |
| `GameModeBase.cpp` | 561+ | `SwapPlayerControllers()` — transfers Player/NetConnection |
| `NyxGameMode.cpp` | 311+ | `CheckZoneBoundaries()` — three-case detection |
| `NyxGameMode.cpp` | 430+ | `ReleasePawnAuthority()` — Server A side |
| `NyxGameMode.cpp` | 480+ | `ClaimPawnAuthority()` — Server B side |

---

# Client-Side Migration Problems & Fixes

## Problem 1: Ghost Pawn Steals Possession (Race Condition)

**Root cause:** Server-2's `ClaimPawnAuthority` fired before Server-1's `ReleasePawnAuthority`. The `NoPawnClaimGracePeriodSeconds` was only 0.5s, but Server-1's zone check runs every 0.5s — with network latency, Server-2 often claimed first.

**What the player sees:** A second character appears nearby, then the camera teleports to it, the original character disappears, and controls feel wrong or break entirely.

**Timeline of the bug:**
1. Player crosses boundary at X=0 heading east
2. Server-2 detects NoPawnPC in east zone, waits 0.5s grace, claims → spawns NyxCharacter_1 at X=828
3. Client receives NyxCharacter_1 via replication — sees "ghost" character
4. `RetryInputSetup` timer on NyxCharacter_1 grabs the local PC, calls `ClientRestart` → steals camera/input from NyxCharacter_0
5. Server-1 finally detects crossing (1.5s later), releases → destroys NyxCharacter_0
6. Proxy finalizes, sends new PC (PlayerController_2), but camera state is already corrupted

**Fix:** Three changes:

1. **Increased `NoPawnClaimGracePeriodSeconds` from 0.5s → 2.0s** (`NyxGameMode.h`). Server-1 always releases before Server-2 claims.

2. **`RetryInputSetup` guard** (`NyxCharacter.cpp`): Before stealing the local PC, check if it already has an active, `bInputSetupComplete` pawn. If so, bail out — this is a pre-migration ghost pawn and should wait for `OnRep_Controller` to set it up after migration finalizes.

3. **`BeginPlay` timer guard** (`NyxCharacter.cpp`): Don't even start the retry timer if the local PC already has a fully-setup character. Logs `"SKIP RetryInputSetup for NyxCharacter_1 — NyxCharacter_0 already controlled by PlayerController_1"`.

## Problem 2: Black Screen After Migration (Missing SetViewTarget)

**Root cause:** `SetupPlayerInputComponent` was called in the normal possession chain but never called `PC->SetViewTarget(this)`. Only the `RetryInputSetup` fallback path did. Since the normal path succeeded first, `bInputSetupComplete` was set to `true`, the retry timer was cleared, and `SetViewTarget` was never called.

**What the player sees:** Black screen after spawning. Character exists, input works (WASD moves), but camera shows nothing.

**Fix:** Added `PC->SetViewTarget(this)` at the end of `SetupPlayerInputComponent`, right after setting `bInputSetupComplete = true`.

## Problem 3: No Camera/Input/HUD After Migration (New PC Not Re-bound)

**Root cause:** After proxy migration finalization, the client receives a NEW `PlayerController` (e.g. `PlayerController_2` replacing `PlayerController_1`). `OnRep_Controller` fires on the pawn, but `bInputSetupComplete` is already `true` from the initial setup. The old code just returned early — no view target, no input bindings, no HUD on the new PC.

**What the player sees:** "Half terrain and half sky, no character, no camera movement, no HUD."

**Fix:** `OnRep_Controller` now detects the migration case: if `bInputSetupComplete == true` AND a new local PC is being assigned, force full re-bind:
```cpp
PC->SetPawn(this);
PC->ClientRestart(this);
PC->SetViewTarget(this);
SetupInputMappingContexts();
```

## Problem 4: Proxy NyxGameInstance Activates ProxyNetDriver (Context Mismatch)

**Root cause:** `NyxGameInstance::Init()` detects `-ProxyGameServers=` on the command line and swaps `GEngine->NetDriverDefinitions` from `IpNetDriver` to `ProxyNetDriver`. This is the ONLY way proxy mode activates — the engine's `-MultiServerProxy` / `-MultiServerBackendAddresses` flags are separate and don't integrate with Nyx's custom setup.

**What happens if wrong flags are used:** The proxy opens as a full Editor window instead of running headless, or the ProxyNetDriver isn't activated and clients can't connect.

**Correct proxy launch:** Must use `-server -ProxyGameServers=addr1,addr2` together. The `-server` flag makes it headless, and `-ProxyGameServers=` triggers the NetDriver swap in `NyxGameInstance::Init()`.

## Current Status

| Feature | Status |
|---------|--------|
| Server-side migration protocol (Release/Claim) | ✅ Working |
| Proxy finalization (route flip) | ✅ Working |
| GUID collision prevention (`-NyxGuidSeed=`) | ✅ Working |
| Client SetViewTarget after spawn | ✅ Fixed |
| RetryInputSetup ghost pawn guard | ✅ Fixed |
| Post-migration PC re-bind (OnRep_Controller) | ✅ Fixed |
| Claim timing (2s grace period) | ✅ Fixed |
| HUD after migration | ⚠️ Partial — N/A shown (new PC may need HUD re-creation) |
| Camera stability after migration | ⚠️ Under investigation — still occasional issues |
| Pre-migration ghost pawn visibility | ⚠️ Ghost pawn from server-2 is visible to client before migration completes |

## Key Files

| File | What it does for migration |
|------|---------------------------|
| `NyxGameMode.cpp` | `CheckZoneBoundaries()`, `ReleasePawnAuthority()`, `ClaimPawnAuthority()` |
| `NyxGameMode.h` | Grace period constants (`NoPawnClaimGracePeriodSeconds`, `TransferGracePeriodSeconds`) |
| `NyxCharacter.cpp` | `OnRep_Controller()` post-migration re-bind, `RetryInputSetup()` with guard, `SetupPlayerInputComponent()` with SetViewTarget |
| `NyxGameInstance.cpp` | `-ProxyGameServers=` detection, NetDriver swap to ProxyNetDriver |
| `NyxHUD.cpp` | Canvas HUD — needs work for post-migration PC |
