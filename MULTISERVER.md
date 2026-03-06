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
---

# DSTM vs Manual Migration: Why Our Approach Is a Workaround

## The Intended Migration Path: DSTM (Distributed State Transfer Machine)

The engine contains a fully implemented **seamless** migration system gated behind `UE_WITH_REMOTE_OBJECT_HANDLE`. When enabled, `APlayerController::PostMigrate()` physically transfers the **same PC instance** between servers:

**Send side** (Server-A releasing):
1. Serializes `CachedConnectionPlayerId` via `APlayerController::Serialize()`
2. Nulls out `NetConnection` and `Player` (server-specific)
3. Detaches PC from `UNetConnection`
4. Spawns `ANoPawnPlayerController` placeholder on the connection
5. Transfers the serialized PC object to Server-B via `RemoteObjectTransfer.cpp`

**Receive side** (Server-B claiming):
1. Deserializes the PC object — same net GUID preserved
2. Finds the connection by `CachedConnectionPlayerId`
3. Destroys the existing `ANoPawnPlayerController` placeholder
4. Binds the original PC to the connection (`NetConnection`, `Player`, `NetPlayerIndex`)
5. Verifies `HandshakeId` consistency

**Key benefit**: Same net GUID → the client's existing PlayerController object gets updated via replication from the new server → no disruption to camera, input, or HUD.

## Why DSTM Is Disabled

`UE_WITH_REMOTE_OBJECT_HANDLE` is `#define`d as **0** in `CoreMiscDefines.h` (line 620). This compiles out:
- `APlayerController::PostMigrate()` entirely
- `APlayerController::Serialize()` migration path
- All of `RemoteObjectTransfer.cpp`'s `FRemoteObjectTransferQueue` subsystem

The MultiServerReplication plugin also gates its usage:

```cpp
// MultiServerBeaconClient.cpp:138
NetDriver->SetUsingRemoteObjectReferences(
    !!UE_WITH_REMOTE_OBJECT_HANDLE && GMultiServerAllowRemoteObjectReferences);
```

With the define at 0, this evaluates to `false` always, regardless of the `multiserver.AllowRemoteObjectReferences` CVar.

## Our Manual Path (What We're Doing Instead)

Without DSTM, we manually orchestrate the swap:

1. **Server-A**: Spawn `ANoPawnPlayerController`, call `SwapPlayerControllers(RealPC, NoPawnPC)`, destroy pawn + old PC
2. **Server-B**: Spawn **new** `APlayerController` + new `ANyxCharacter`, call `SwapPlayerControllers(NoPawnPC, NewPC)`, possess

## Why the Proxy Handles Our Approach Correctly

The proxy's `ReassignPlayerController()` doesn't care whether the game PC is the same instance or a new one. It dispatches based solely on type:

```cpp
// MultiServerProxy.cpp:1244
if (PlayerController->IsA(ANoPawnPlayerController::StaticClass()))
    ReceivedReassignedNoPawnPlayerController(Route);
else
    ReceivedReassignedGamePlayerController(Route);
```

Both events (NoPawnPC on old route + game PC on new route) trigger `FinalizePlayerControllerReassignment()` which flips route states: old → `Connected`, new → `ConnectedPrimary`. This works identically for both DSTM and manual paths.

## The Root Cause of All Client-Side Issues

| | DSTM Path | Our Manual Path |
|---|-----------|-----------------|
| **PC on client** | Same object, same net GUID | **New** object, **new** net GUID |
| **Client experience** | Replication updates existing PC seamlessly | Old PC lingers, new PC arrives — two PCs coexist briefly |
| **Camera** | Stays attached | Detaches (old PC had view target, new PC doesn't) |
| **Input** | Stays bound | Lost (old PC's bindings, new PC has none) |
| **HUD** | Stays attached | Lost (HUD was on old PC) |
| **Pawn** | Same pawn, authority transfers | New pawn spawns, old pawn destroyed — ghost visible briefly |

Every client-side fix we've written (`OnRep_Controller` re-bind, `RetryInputSetup` guard, `SetViewTarget` in `SetupPlayerInputComponent`) is compensating for the fact that the client receives a completely new PlayerController instead of keeping the same one.

## Options Forward

1. **Enable DSTM** — Set `UE_WITH_REMOTE_OBJECT_HANDLE 1` in `CoreMiscDefines.h` and rebuild the engine from source. This gives us the seamless `PostMigrate()` migration. Major undertaking (engine source modification + full rebuild) but is the intended design.

2. **Keep manual path, harden client handling** — Continue with spawn-new-PC approach. Improve `OnRep_Controller`, `HandleClientPlayer`, and pawn re-bind to handle the two-PC coexistence window cleanly. This is what we've been doing.

3. **Hybrid: replicate key state** — Instead of spawning a brand new PC on Server-B, serialize the old PC's camera/input/HUD state and apply it to the new PC. Still a new net GUID though, so the client still sees a PC swap — limited benefit over option 2.

## Source Locations (DSTM)

| File | Lines | Content |
|------|-------|---------|
| `CoreMiscDefines.h` | 620 | `#define UE_WITH_REMOTE_OBJECT_HANDLE 0` — the kill switch |
| `PlayerController.cpp` | 5028–5125 | `PostMigrate()` — full Send/Receive logic |
| `PlayerController.cpp` | 5202–5224 | `Serialize()` — migration serialization of `CachedConnectionPlayerId` |
| `PlayerController.cpp` | 6665–6718 | `ANoPawnPlayerController` implementation |
| `RemoteObjectTransfer.cpp` | 97–1433 | `FRemoteObjectTransferQueue` — the DSTM subsystem |
| `MultiServerBeaconClient.cpp` | 138 | `SetUsingRemoteObjectReferences()` — plugin gate |
| `MultiServerBeaconHost.cpp` | 39 | Same gate on host side |

---

# Can We Replicate DSTM Without Engine Rebuild?

**No.** After exhaustive study of the proxy architecture, GUID system, and channel management, replicating DSTM behavior at the game level is not feasible. Here's the full analysis of every approach considered and why each fails.

## Proxy Architecture: Decode-Replicate-Re-encode

The proxy does **NOT** forward raw bytes. It fully materializes actors from all game servers into a **shared `UWorld`**, then runs `ServerReplicateActors()` to replicate from that world to clients:

```
Game Server A ──(UE replication)──► UProxyBackendNetDriver A ──┐
                                                                ├──► Shared UWorld
Game Server B ──(UE replication)──► UProxyBackendNetDriver B ──┘      (decode)
                                                                         │
                                                         ServerReplicateActors()
                                                              (re-encode)
                                                                         │
                                                                         ▼
                                                              UProxyNetDriver
                                                                   │
                                                              UProxyNetConnection
                                                                 (Client)
```

Key proxy behaviors:
- Backend drivers share a **single** `FProxyBackendNetGUIDCache` (all servers' actors in one GUID space)
- Backend `ShouldSkipRepNotifies()` returns `true` (no game logic on proxy)
- Backend `EnableExecuteRPCFunctions(false)` (no RPC execution on proxy)
- Frontend `FProxyNetGUIDCache::AssignNewNetGUID_Server()` **looks up** the GUID from the shared backend cache — same GUID values flow end-to-end from game server → proxy → client
- RPCs are forwarded function-by-function via `ForwardRemoteFunction()`, not raw bytes
- Actor roles are swapped on proxy so clients receive correct `ROLE_SimulatedProxy`/`ROLE_AutonomousProxy`

Source: `MultiServerProxy.cpp` lines 93–148 (GUID lookup), 519–567 (RPC forwarding), 920–928 (role swap), 1010–1015 (ServerReplicateActors)

## Approach 1: Force Net GUIDs on Server-B — FAILS

**Idea:** `FNetGUIDCache` is fully public `ENGINE_API`. We could capture Server-A's PC GUID, transfer it to Server-B, and call `RegisterNetGUID_Server(sameGUID, newPC)` before replication.

**Why it's possible at the API level:**
```cpp
// FNetGUIDCache — all public, all ENGINE_API
ENGINE_API FNetworkGUID GetNetGUID(const UObject* Object) const;
ENGINE_API bool RemoveNetGUID(const UObject* Object);
ENGINE_API void RegisterNetGUID_Server(const FNetworkGUID& NetGUID, UObject* Object);
ENGINE_API void RemoveActorNetGUIDs(const AActor* Actor);
// FNetworkGUID — plain struct, freely constructible
static FNetworkGUID CreateFromIndex(uint64 NetIndex, bool bIsStatic);
```

**Why it fails in practice (4 layered problems):**

1. **No GUID communication channel** — Server-A and Server-B are separate processes. No built-in mechanism to transfer the GUID. Would need SpacetimeDB or shared memory (adds latency and complexity).

2. **Proxy shared cache collision** — The proxy's `FProxyBackendNetGUIDCache` (shared across all backends) uses `RegisterNetGUID_Client` which handles reassignment. This part actually works — it logs a warning and remaps. But the proxy's frontend `FProxyNetGUIDCache` calls `RegisterNetGUID_Server` which asserts:
   ```cpp
   check(!ObjectLookup.Contains(NetGUID));  // CRASHES if GUID already exists
   ```
   If Server-B's new PC arrives while Server-A's old PC is still in the frontend cache → **assertion failure / crash**.

3. **Timing dependency** — For the frontend cache slot to be free, Server-A's PC must be fully removed from the proxy's replication system first (`RemoveNetworkActor` → `RemoveActorNetGUIDs`). This happens asynchronously when the proxy→client channel for the old PC closes. No guarantee it happens before Server-B's PC arrives.

4. **Channel continuity is not GUID-based** — Even with matching GUIDs, actor channels are per-`UObject*`, not per-GUID. The proxy creates a NEW channel for the new PC object. The client receives a channel close (old PC) followed by a channel open (new PC). Same net effect as different GUIDs — the client destroys and recreates the PC.

## Approach 2: Use `EChannelCloseReason::Migrated` — FAILS

**Idea:** The proxy's `ShouldClientDestroyActor` returns `false` for `EChannelCloseReason::Migrated`. If we close the old PC's channel with `Migrated`, the proxy keeps it alive:

```cpp
// MultiServerProxy.cpp:658 — the gate we want to trigger
bool UProxyBackendNetDriver::ShouldClientDestroyActor(AActor* Actor, EChannelCloseReason CloseReason) const
{
    return (CloseReason != EChannelCloseReason::Migrated);
}
```

`NotifyActorDestroyed` IS callable from game code:
```cpp
// NetDriver.h:1781 — ENGINE_API, public, virtual
ENGINE_API virtual void NotifyActorDestroyed(AActor* Actor, bool IsSeamlessTravel = false,
    EChannelCloseReason CloseReason = EChannelCloseReason::Destroyed);
```

**Why it fails (3 problems):**

1. **Proxy keeps actor but client doesn't** — `ShouldClientDestroyActor` controls whether the proxy destroys the actor in ITS shared world. It does NOT control the proxy→client channel close reason. The proxy's `ServerReplicateActors()` determines client channel lifecycle independently. When the old PC becomes irrelevant (no longer anyone's controller), the proxy→client channel closes with `Destroyed` (not `Migrated`) → client destroys its local PC.

2. **Actor irrelevancy kills it anyway** — After route reassignment, the old PC is nobody's `Route->PlayerController`. `APlayerController::IsNetRelevantFor` returns `false` for non-owning connections. The proxy stops including it in `ServerReplicateActors()` → channel closes → client destroys actor.

3. **`PendingSwapConnection` doesn't replicate** — Set by `SwapPlayerControllers` on Server-A, but `UNetConnection*` properties can't replicate (they ARE the replication mechanism). So the proxy's copy of the PC doesn't reflect the swap state.

## Approach 3: Enable `UE_WITH_REMOTE_OBJECT_HANDLE` Without Full Rebuild — FAILS

**Idea:** `#define UE_WITH_REMOTE_OBJECT_HANDLE 1` in our game module before engine includes.

**Why it fails:** ABI mismatch. The engine DLLs were compiled with the define at 0. Enabling it in our module changes class layouts (`APlayerController` gets additional `CachedConnectionPlayerId` serialization, `CharacterMovementComponent` gets remote object members, physics interfaces change). Our module would see different `sizeof()` for engine types → memory corruption → crash.

## Approach 4: Subclass `FNetGUIDCache` (Virtual Override) — FAILS

**Idea:** `AssignNewNetGUID_Server` is `virtual`. We could subclass `FNetGUIDCache`, override it to use our desired GUIDs, and install it on the NetDriver.

**Why it doesn't help:** Even with custom GUID assignment on Server-B, the fundamental channel continuity problem (Approach 1, point 4) remains. The proxy creates per-object channels, not per-GUID channels. A new object with an old GUID still gets a new channel.

## Why DSTM Works and We Can't

DSTM solves the problem at a level we can't reach:

| Layer | What DSTM Does | What We Can Reach |
|-------|----------------|-------------------|
| **UObject identity** | Physically moves the C++ object between processes — same pointer, same net GUID | We spawn a NEW object — different pointer, different GUID |
| **Actor channel** | Same object → same channel → client sees property updates | New object → new channel → client sees destroy + create |
| **Channel close reason** | `AActor::PostMigrate` calls `NotifyActorDestroyed(Migrated)` — proxy preserves actor | We can call this from game code, but proxy→client propagation doesn't use the same reason |
| **Replication continuity** | Server-B starts replicating the same GUID → client's existing channel gets new data | Server-B replicates new GUID → client creates new actor |

The proxy is a **transparent relay** designed for DSTM. Without DSTM, it still works (our SwapPlayerControllers approach is handled correctly), but the client inevitably sees a PC actor swap because object identity can't be preserved across processes.

## What We CAN Do (Without Engine Changes)

Our current approach (spawn new PC, `OnRep_Controller` re-bind) is the correct fallback. To minimize the visible "seam":

1. **Robust state transfer in `OnRep_Controller`** — Copy camera transform, input bindings, HUD state from old PC to new PC within the same frame. Already partially implemented.

2. **Pawn pre-replication** — Server-B's pawn is visible to the client BEFORE migration completes (the proxy replicates world actors from all servers, not just primary). Use this to our advantage: the pawn is already there when the PC swap happens.

3. **Hide ghost pawn** — Set Server-B's pawn invisible until formally possessed (via `OnRep_Controller`), preventing the "ghost" appearance.

4. **Single-frame transition** — Ensure `HandleClientPlayer` → `OnRep_Controller` → `SetViewTarget` → `SetupInputMappingContexts` all execute within one frame. The visual pop becomes imperceptible.

5. **Camera transform continuity** — Store last camera world transform in a replicated property on the pawn. When the new PC takes over, immediately set camera to match — no visual jump.

## Definitive Source Locations

| File | Lines | What |
|------|-------|------|
| `MultiServerProxy.cpp` | 93–148 | `FProxyNetGUIDCache::LookupNetGUIDFromBackendCache` — GUID pass-through |
| `MultiServerProxy.cpp` | 152–157 | `FProxyBackendNetGUIDCache::IsNetGUIDAuthority() = false` |
| `MultiServerProxy.cpp` | 658–664 | `ShouldClientDestroyActor` — Migrated check |
| `MultiServerProxy.cpp` | 920–928 | Role swap on proxy for client replication |
| `MultiServerProxy.cpp` | 1010 | `ServerReplicateActors` — re-encode to clients |
| `MultiServerProxy.h` | 300–323 | Architecture comment: "shared UWorld" model |
| `PackageMapClient.h` | 228–257 | `FNetGUIDCache` public API (all ENGINE_API) |
| `PackageMapClient.cpp` | 3277–3290 | `AssignNewNetGUID_Server` — GUID assignment |
| `PackageMapClient.cpp` | 3374–3396 | `RegisterNetGUID_Server` — `check(!ObjectLookup.Contains)` assertion |
| `NetDriver.h` | 1781 | `NotifyActorDestroyed` — ENGINE_API, takes CloseReason |
| `CoreMiscDefines.h` | 620 | `#define UE_WITH_REMOTE_OBJECT_HANDLE 0` |
| `Actor.cpp` | 1338 | Only place `EChannelCloseReason::Migrated` is set (gated by DSTM) |