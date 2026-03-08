# Seamless Cross-Server Transfer in UE 5.7 MultiServerReplication

## Executive Summary

After an independent audit of the full UE 5.7 engine source, the MultiServerReplication plugin, and the DSTM (Distributed State Transfer Machine) framework in CoreUObject, this document presents a complete analysis of how to achieve **truly seamless** cross-server player migration — meaning the client experiences zero visual disruption, no camera loss, no input gap, and no ghost actors.

The core problem is well-understood: the manual `SwapPlayerControllers` path creates a **new** PlayerController and Pawn on the destination server, which the client receives as distinct actors. This causes a visible destroy/create cycle regardless of how much client-side code is written to compensate.

This document proposes **three concrete approaches** ordered by increasing ambition and decreasing fragility, with the recommended "golden path" being **Approach 2** — completing the DSTM network layer using the existing MultiServer beacon infrastructure. All implementation code is framed as a **custom plugin** that extends the engine's existing infrastructure without modifying engine source (beyond enabling a compile-time define).

---

## The Problem In One Diagram

```
CURRENT FLOW (Manual SwapPC):

  Server-A                  Proxy Shared World              Client
  ────────                  ──────────────────              ──────
  PC_A (GUID=100004)  ───►  PC_proxy (GUID=100004) ───►    PC_local (GUID=100004)
  Pawn_A (GUID=100006) ──►  Pawn_proxy (GUID=100006) ──►   Pawn_local (GUID=100006)
        │                          │                              │
   [Release]                       │                              │
   Destroy Pawn                    │                              │
   Swap → NoPawnPC                 │                              │
        │                          │                              │
        │                [PC_proxy becomes NoPawnPC]        [Camera loses target]
        │                [Pawn_proxy destroyed]             [Pawn_local destroyed]
        │                          │                              │
  Server-B                         │                              │
  ────────                         │                              │
  PC_B (GUID=200004)  ────►  PC_proxy_new (GUID=200004) ──►  PC_local_2 (GUID=200004)
  Pawn_B (GUID=200006) ───►  Pawn_proxy_new (GUID=200006)►   Pawn_local_2 (GUID=200006)
        │                          │                              │
   [Claim + Possess]         [Route flip]                   [New PC, new Pawn]
                                                            [Must rebind everything]
                                                            [≈200ms visible seam]
```

**Root cause**: Different GUIDs → different actor channels → client destroys old + creates new. All client-side fixes (OnRep_Controller re-bind, camera integrity timers, ghost pawn cleanup) are **compensating** for this fundamental problem.

---

## Approach 1: Hardened Manual Path (No Engine Changes)

### What It Is
Improve the current `SwapPlayerControllers` + `EChannelCloseReason::Migrated` approach to minimize the visible seam to a single frame.

### What's Already Working
- Closing pawn channels with `EChannelCloseReason::Migrated` — the proxy's `ShouldClientDestroyActor` returns `false`, so the **pawn stays alive on the client** during the transition window
- Cross-server coordination signals Server-B to claim authority shortly after Server-A's release
- The proxy's `FinalizePlayerControllerReassignment` correctly flips the primary route

### Remaining Client-Side Issues

1. **The PC is still a new actor** — even with pawn preservation, the client gets a new `PlayerController` from Server-B. Camera manager, HUD, and input contexts are bound to the **old** PC and lost.

2. **Two pawns coexist** — the Migrated-close pawn (frozen at boundary) + the new pawn from Server-B. The old pawn is never explicitly destroyed because only the proxy can close frontend channels.

3. **Post-migration proxy disruption** — ~5s after migration, the proxy's `AddNetworkActor` may call `SetReplicates(false)` on the new pawn if it briefly has `ROLE_Authority`, breaking the PC→Pawn link.

### What Would Make This Work Reliably

**A. Pawn channel cleanup on proxy side:**
The old pawn (Migrated-close from Server-A) lives forever in the proxy's shared world because `ShouldClientDestroyActor` returned `false`. But Server-A will never re-replicate it. The proxy needs to detect this "orphaned Migrated actor" and eventually close the frontend channel. Currently it doesn't.

**Proposed fix**: After `FinalizePlayerControllerReassignment`, iterate the proxy's network object list. Any actor with `EChannelCloseReason::Migrated` on the backend side that hasn't been re-associated with a new backend channel within N seconds should be destroyed from the shared world (which closes the frontend channel → client destroys it cleanly).

This requires **plugin modification** — touching `MultiServerProxy.cpp`.

**B. Single-frame PC transition on client:**
Bundle all state transfer into one deferred action:
```
Frame N:   New PC arrives via replication (OnRep_Controller fires)
           → Capture old PC's camera transform, input mapping contexts, HUD class
           → Apply to new PC: SetViewTarget, input contexts, spawn HUD
           → Hide old pawn, enable new pawn visibility
Frame N+1: Client sees seamless transition
```

This is essentially what the current `OnRep_Controller` code tries to do, but race conditions with the pawn's `RetryInputSetup` timer and the proxy's actor role manipulation cause it to fail intermittently.

**C. Deterministic pawn GUID (the abandoned approach, revisited):**
The previous attempt at `MakeMigrationGUID` failed because `ClientHandshakeId` differs per backend server (the proxy increments a counter per ConnectToGameServer call — see `MultiServerProxy.cpp:311`). 

But the proxy also propagates `ProxyConnection->PlayerId` to each backend server's `ULocalPlayer` (line 375: `NewPlayer->SetCachedUniqueNetId(ProxyConnection->PlayerId)`). This `PlayerId` is a `FUniqueNetIdRepl` — a stable cross-server identifier for the same physical player. If the game servers can access this PlayerId, it can be used as a deterministic GUID seed that produces the same value on all servers:

```cpp
// Concept: Deterministic GUID from player's stable network identity
uint64 DeterministicSeed = GetTypeHash(NetConnection->PlayerId.ToString());
FNetworkGUID PawnGUID = FNetworkGUID::CreateFromIndex((DeterministicSeed << 4) | SlotIndex, false);
```

However, this still has the **channel continuity problem**: the proxy creates per-`UObject*` channels, not per-GUID channels. A new object with the same GUID gets a new channel. The client still sees destroy+create.

### Assessment

**Approach 1 is inherently limited.** No amount of client-side code can prevent the destroy/create cycle for the PlayerController because the proxy's frontend channel system is per-object. The pawn can be preserved via Migrated close, but the PC cannot — and the PC is what owns camera, input, and HUD.

**Estimated effort**: Medium (game code only, no engine/plugin changes)  
**Seamlessness**: Partial — ~1 frame seam for pawn, ≈200ms seam for PC/camera/input  
**Reliability**: Fragile — dependent on timing of replication, prone to edge cases

---

## Approach 2: Complete the DSTM Network Layer (Recommended)

### The Key Insight

The DSTM framework is **architecturally complete**. Every piece of the migration lifecycle exists in shipped UE 5.7 source:

| Component | Location | Status |
|-----------|----------|--------|
| Object serialization (`FArchiveRemoteObjectWriter/Reader`) | `RemoteObjectSerialization.cpp` | Complete |
| Migration queue with priority arbitration | `RemoteObjectTransfer.cpp:118-319` | Complete |
| `AActor::PostMigrate()` — world removal, channel close with `Migrated`, tick teardown | `Actor.cpp:1264-1400` | Complete |
| `APlayerController::PostMigrate()` — NoPawnPC swap, connection rebind | `PlayerController.cpp:5029-5180` | Complete |
| Transport delegates (pluggable interface) | `RemoteObjectTransfer.h:147-170` | **Default to disk I/O** |
| Proxy `ShouldClientDestroyActor(Migrated)` | `MultiServerProxy.cpp:658` | Complete |
| Proxy GUID continuity (shared backend cache) | `MultiServerProxy.cpp:93-148` | Complete |
| `FRemoteServerId` identity system | `RemoteObjectTypes.h` | Complete |
| `FRemoteObjectId` cross-server identity | `RemoteObjectTypes.h` | Complete |
| Beacon-based inter-server connections | `MultiServerBeaconClient/Node/PeerConnection` | Complete |

The **only missing piece** is binding `RemoteObjectTransferDelegate` and `RequestRemoteObjectDelegate` to actual inter-server network transport instead of local disk I/O.

### Why This Is Feasible

1. **The delegates are designed to be pre-bound.** `RemoteObject.cpp:317` checks `!IsBound()` before applying disk defaults. Any code that binds the delegates **before** `InitRemoteObjects()` is called wins. Game module `StartupModule()` or a custom engine subsystem can pre-bind.

2. **The MultiServer beacon infrastructure provides the network channel.** `AMultiServerBeaconClient` already:
   - Establishes reliable connections between servers via `UMultiServerNode`
   - Supports custom RPCs (subclass beacon, add `UFUNCTION(Server/Client, Reliable)`)
   - Has `SetUnlimitedBunchSizeAllowed(true)` (`MultiServerBeaconClient.cpp:60`) — can handle arbitrarily large serialized object data
   - Provides `GetBeaconClientForRemotePeer(PeerId)` for routing to specific servers

3. **The serialization format is self-contained.** `FRemoteObjectData` contains `Tables` (name tables, remote IDs), `PathNames`, and `Bytes` (serialized object data in <64KB chunks). This is already designed to be sent over the wire — the disk format and network format would be identical.

### Implementation Plan

#### Step 1: Enable `UE_WITH_REMOTE_OBJECT_HANDLE`

In the engine source, `Engine/Source/Runtime/Core/Public/Misc/CoreMiscDefines.h`, line 620:

```cpp
#define UE_WITH_REMOTE_OBJECT_HANDLE 1  // was 0
```

**This requires a full engine rebuild from source.** This is a major prerequisite but enables:
- `AActor::PostMigrate()` — the complete send/receive lifecycle
- `APlayerController::PostMigrate()` — NoPawnPC swap, CachedConnectionPlayerId save/restore
- `FRemoteObjectTransferQueue::TickSubsystem()` — migration queue processing
- `FRemoteObjectId` based cross-server object identity
- `NetDriver->SetUsingRemoteObjectReferences(true)` on beacon connections

**ABI impact**: This changes `FObjectHandle` from a simple pointer to `FRemoteObjectHandlePrivate` (tagged pointer union). Every `TObjectPtr<>` in the engine changes size/layout. All modules (engine, plugins, game) must be compiled against the same setting. This requires building from engine source — a pre-built/installed engine cannot be used.

#### Step 2: Initialize Server Identity

Each game server needs a unique `FRemoteServerId`. In a custom plugin subsystem or the game module's `StartupModule()`:

```cpp
// Parse server ID from command line: -DedicatedServerId=server-1
FString ServerIdStr;
FParse::Value(FCommandLine::Get(), TEXT("-DedicatedServerId="), ServerIdStr);
if (!ServerIdStr.IsEmpty())
{
    uint32 ServerId = GetTypeHash(ServerIdStr); // or parse a numeric ID
    FRemoteServerId::InitGlobalServerId(FRemoteServerId(ServerId));
}
```

Note: `InitGlobalServerId` can only be called once (it asserts on re-initialization). It should be called before any UObjects are allocated, ideally in a plugin module's `StartupModule()`.

#### Step 3: Create the DSTM Network Transport

In a custom plugin, subclass `AMultiServerBeaconClient` with RPCs for sending/receiving serialized object data:

```cpp
// Custom plugin: DSTMTransport/Source/DSTMTransport/Public/DSTMBeaconClient.h
UCLASS()
class ADSTMBeaconClient : public AMultiServerBeaconClient
{
    GENERATED_BODY()

    UFUNCTION(Server, Reliable)
    void ServerReceiveMigratedObject(
        FRemoteObjectId ObjectId,
        FRemoteServerId OwnerServerId,
        FRemoteServerId PhysicsServerId,
        uint32 PhysicsLocalIslandId,
        FRemoteServerId SenderServerId,
        const TArray<uint8>& SerializedData);

    UFUNCTION(Client, Reliable)
    void ClientReceiveMigratedObject(/* same params */);

    // For pull-requests (server B requesting an object from server A):
    UFUNCTION(Server, Reliable)
    void ServerRequestMigrateObject(
        FRemoteObjectId ObjectId,
        FRemoteServerId RequestingServerId);
};
```

#### Step 4: Bind the Transport Delegates

In the custom plugin module's startup (before `InitRemoteObjects()` is called):

```cpp
// Bind the send delegate
UE::RemoteObject::Transfer::RemoteObjectTransferDelegate.BindLambda(
    [this](const UE::RemoteObject::Transfer::FMigrateSendParams& Params)
    {
        FMigrationRoutingInfo Info = UE::RemoteObject::Transfer::GetMigrationRoutingInfo(Params);
        
        // Serialize FRemoteObjectData to TArray<uint8>
        TArray<uint8> SerializedData;
        FMemoryWriter Writer(SerializedData);
        // ... serialize Params.ObjectData.Tables, PathNames, Bytes ...
        
        // Route to destination server via beacon
        ADSTMBeaconClient* Beacon = Cast<ADSTMBeaconClient>(
            MultiServerNode->GetBeaconClientForRemotePeer(
                ServerIdToPeerId(Info.DestinationServerId)));
        if (Beacon)
        {
            if (Beacon->IsAuthorityBeacon())
                Beacon->ClientReceiveMigratedObject(Info.ObjectId, Info.OwnerServerId, ...);
            else
                Beacon->ServerReceiveMigratedObject(Info.ObjectId, Info.OwnerServerId, ...);
        }
    });

// Bind the request delegate (for pull-migration)
UE::RemoteObject::Transfer::RequestRemoteObjectDelegate.BindLambda(
    [this](FRemoteWorkPriority Priority, FRemoteObjectId ObjectId, 
           FRemoteServerId LastKnownServerId, FRemoteServerId DestServerId)
    {
        ADSTMBeaconClient* Beacon = Cast<ADSTMBeaconClient>(
            MultiServerNode->GetBeaconClientForRemotePeer(
                ServerIdToPeerId(LastKnownServerId)));
        if (Beacon)
        {
            Beacon->ServerRequestMigrateObject(ObjectId, DestServerId);
        }
    });
```

On the receiving end:
```cpp
void ADSTMBeaconClient::ServerReceiveMigratedObject_Implementation(
    FRemoteObjectId ObjectId, FRemoteServerId OwnerServerId,
    FRemoteServerId PhysicsServerId, uint32 PhysicsLocalIslandId,
    FRemoteServerId SenderServerId, const TArray<uint8>& SerializedData)
{
    // Deserialize FRemoteObjectData from TArray<uint8>
    FRemoteObjectData ObjectData;
    FMemoryReader Reader(SerializedData);
    // ... deserialize ...
    
    // Feed into the DSTM receive pipeline
    UE::RemoteObject::Transfer::OnObjectDataReceived(
        OwnerServerId, PhysicsServerId, PhysicsLocalIslandId,
        ObjectId, SenderServerId, ObjectData);
}
```

#### Step 5: Trigger Migration Instead of Manual Swap

Replace the current manual release/claim authority dance with a single DSTM call:

```cpp
// Game mode or custom plugin: Replace manual SwapPC with DSTM transfer
void AMyGameMode::OnPlayerCrossesBoundary(APlayerController* PC, ACharacter* PlayerChar)
{
    FRemoteServerId DestServerId = GetServerIdForZone(PlayerChar->GetActorLocation());
    
    // This one call does everything:
    // 1. Serializes PC + all subobjects (including owned components)
    // 2. Calls AActor::PostMigrate(Send) — which calls NotifyActorDestroyed(Migrated)
    // 3. Calls APlayerController::PostMigrate(Send) — spawns NoPawnPC, transfers connection
    // 4. Invokes RemoteObjectTransferDelegate → our beacon sends data to Server-B
    UE::RemoteObject::Transfer::TransferObjectOwnershipToRemoteServer(PC, DestServerId);
    
    // Also transfer the pawn
    UE::RemoteObject::Transfer::TransferObjectOwnershipToRemoteServer(PlayerChar, DestServerId);
}
```

Server-B doesn't need explicit claim code — the DSTM receive pipeline handles everything:
1. `OnObjectDataReceived()` → `FRemoteObjectTransferQueue::FulfillReceiveRequest()`
2. Deserializes the PC and Pawn objects (**same C++ objects, same FRemoteObjectId**)
3. Calls `AActor::PostMigrate(Receive)` → adds to world, starts replicating
4. Calls `APlayerController::PostMigrate(Receive)` → finds connection by `CachedConnectionPlayerId`, binds PC to it, destroys the NoPawnPC placeholder

### Why This Achieves True Seamlessness

```
DSTM FLOW:

  Server-A                  Proxy Shared World              Client
  ────────                  ──────────────────              ──────
  PC_A (RemoteId=X)   ───►  PC_proxy (GUID=G)    ───►     PC_local (GUID=G)
  Pawn_A (RemoteId=Y) ──►   Pawn_proxy (GUID=H)  ──►      Pawn_local (GUID=H)
        │                          │                              │
   [TransferOwnership]             │                              │
   PostMigrate(Send):              │                              │
    - Close channels(Migrated)     │                              │
    - Spawn NoPawnPC               │                              │
    - Serialize PC+Pawn            │                              │
        │                   [ShouldDestroy=false]           [No destroy!]
        │                   [PC_proxy stays alive]          [PC_local stays]
        │                   [Pawn_proxy stays alive]        [Pawn_local stays]
        │                          │                              │
        ├──── Beacon RPC ─────────►│                              │
        │                          │                              │
  Server-B                         │                              │
  ────────                         │                              │
   [OnObjectDataReceived]          │                              │
   Deserialize PC → same RemoteId  │                              │
   PostMigrate(Receive):           │                              │
    - Bind to connection           │                              │
    - Add to world                 │                              │
    - Start replicating      [Same PC_proxy, same GUID]    [Property updates on
        │                    [Same Pawn_proxy, same GUID]   same PC_local, same
        │                          │                        Pawn_local - seamless!]
        │                          │                              │
                              [Replication continues]       [Zero disruption ✓]
```

The critical difference: **same `FRemoteObjectId` → same proxy UObject → same frontend GUID → same client actor channel → client sees property updates, not destroy/create.**

### Risks & Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Engine rebuild breaks something | Medium | Build from source incrementally; test with simple actors first |
| `FObjectHandle` ABI change causes crashes | High if mixed | Never mix pre-built engine with source-built; always use `D:\UnrealEngine` |
| Beacon RPC size limits for large actors | Low | `SetUnlimitedBunchSizeAllowed(true)` already set; chunk data if needed |
| `CachedConnectionPlayerId` doesn't match on Server-B | Low | Proxy propagates `PlayerId` to all backends; `PostMigrate(Receive)` uses `FindConnection(CachedConnectionPlayerId)` which searches by `PlayerId` |
| Missing `DstmPhysicsMigration.cpp` (referenced in comments) | Low | Only relevant for physics simulation handoff, not player migration |
| DSTM's `RemoteObjectSupportCompiledIn` early-exit | None | Setting the define to 1 makes this `true` |
| Interaction with GUID seed offsets | Low | DSTM uses `FRemoteObjectId` (distinct from `FNetworkGUID`); GUID seed offsets still prevent backend GUID collisions |

### Estimated Effort

| Task | Effort | Status |
|------|--------|--------|
| Engine rebuild with `UE_WITH_REMOTE_OBJECT_HANDLE=1` | 2-4 hours (compile time) | ✅ Done |
| `ADSTMBeaconClient` custom plugin implementation | 1-2 days | ✅ Done (`Plugins/DSTMTransport/`) |
| Delegate binding + `FRemoteObjectData` serialization | 1 day | ✅ Done |
| Server ID initialization | 2 hours | ✅ Done |
| Replace release/claim with `TransferObjectOwnershipToRemoteServer` | 4 hours | ✅ Done (game code integration) |
| Testing + iteration | 2-3 days | ⏳ Pending runtime testing |
| **Total** | **~1-2 weeks** | |

---

## Approach 3: Shared-Directory Transport (Simpler DSTM, Same Engine Rebuild)

### What It Is

A simpler variant of Approach 2 that sidesteps the beacon-based network transport by using a **shared filesystem** or **network share** between servers. Since the default DSTM transport already serializes to disk, this just requires making all servers read/write to the same directory.

### How It Works

The default disk transport uses per-server filenames:
```cpp
// RemoteObjectSerialization.cpp:87
FString GenerateRemoteObjectFilename(FRemoteObjectId ObjectId, FRemoteServerId OwnerServerId)
{
    return FPaths::Combine(*FPaths::ProjectSavedDir(), 
        *(FString::Printf(TEXT("%s-%s_%s.remote"), 
            *FRemoteServerId::GetLocalServerId().ToString(),
            *ObjectId.ToString(ERemoteIdToStringVerbosity::Id), 
            *OwnerServerId.ToString())));
}
```

The problem: Server-A writes `{Server-A-Id}-{ObjectId}_{OwnerServerId}.remote`, but Server-B looks for `{Server-B-Id}-{ObjectId}_{OwnerServerId}.remote`. They use different `GetLocalServerId()` prefixes.

**Fix**: Override `RemoteObjectTransferDelegate` and `RequestRemoteObjectDelegate` with a version that uses a **shared path** without the local server ID prefix:

```cpp
FString SharedFilename = FPaths::Combine(
    TEXT("\\\\SharedDrive\\DSTMMigration"),  // or a local temp dir if same machine
    FString::Printf(TEXT("%s_%s.remote"), 
        *ObjectId.ToString(), *OwnerServerId.ToString()));
```

Then add a **file watcher** or **polling loop** on Server-B that detects new `.remote` files addressed to it and calls `OnObjectDataReceived()`.

### Pros and Cons vs Approach 2

| Aspect | Approach 2 (Beacon) | Approach 3 (Shared Disk) |
|--------|---------------------|--------------------------|
| Latency | Low (~1ms LAN RPC) | Higher (~10-50ms file I/O + polling) |
| Reliability | Built on UE reliable channel | Filesystem operations can fail; no guaranteed ordering |
| Scalability | Works across machines/networks | Requires shared filesystem access |
| Code complexity | More code (beacon subclass + RPCs) | Less code (delegate rebind + file watcher) |
| Same-machine dev | Works | Easier (just use a local temp dir) |
| Production | Production-ready | Not suitable for production |

### Assessment

**Good for prototyping** the DSTM pipeline quickly to validate that it actually solves the client-side issues before investing in the beacon transport. In development (all servers on one machine), this "just works" with a shared temp directory.

**Not suitable for production** — file I/O is too slow for responsive migration, there's no reliable delivery guarantee, and it requires filesystem coordination that doesn't scale.

---

## Key Discoveries From the Source Audit

### 1. The Proxy Was Designed For DSTM

The comment on `ShouldClientDestroyActor` (`MultiServerProxy.cpp:660-663`) is the most important text in the entire plugin:

> "When the actor arrives on the destination server it will be added to that server's replication system and **the actor will be re-used on the proxy since it's still referenced in the shared backend NetGUID cache**."

This is explicitly describing DSTM flow. The proxy's architecture — shared backend GUID cache, Migrated close reason handling, GUID passthrough from backend to frontend — **was built for DSTM**. The manual SwapPC path is a fallback that the proxy happens to also support via its type-based `ReassignPlayerController` dispatch.

### 2. `FMigrationRoutingInfo` + `GetMigrationRoutingInfo()` Already Exist

The `RemoteObjectTransfer.h` declares `FMigrationRoutingInfo` (a public struct containing `ObjectId`, `OwnerServerId`, `DestinationServerId`, `PhysicsServerId`, `PhysicsLocalIslandId`) and `GetMigrationRoutingInfo()` (a `COREUOBJECT_API` function that extracts this from the opaque `FMigrateSendParams`). 

This was clearly designed to be consumed by custom transport implementations — it provides exactly the routing metadata needed to address the serialized data to the correct destination server.

### 3. Beacon Connections Support Unlimited Bunch Sizes

`MultiServerBeaconClient.cpp:60`:
```cpp
Connection->SetUnlimitedBunchSizeAllowed(true);
```

UE5's default reliable RPC max bunch size is 64KB, which would limit the serialized object payload. But the MultiServer beacons explicitly remove this limit, suggesting they were designed to carry large payloads like serialized migration data.

### 4. `RemoteObjectSupportCompiledIn` Controls Runtime Behavior

`FRemoteObjectId::RemoteObjectSupportCompiledIn` is set to `UE_WITH_REMOTE_OBJECT_HANDLE`. When this is `0`, `InitRemoteObjects()` returns early after setting an invalid `GlobalServerId`. This means none of the remote object infrastructure is initialized — no stubs, no transfer queue, no delegates. The define must be `1` for any DSTM path.

### 5. The Proxy Does NOT Use Iris-style Replication

The proxy uses traditional `ServerReplicateActors()` (which calls `UNetDriver::ServerReplicateActors_ProcessPrioritizedActors`). It does NOT use the Iris `FReplicationSystemParams::bUseRemoteObjectReferences` path. This means the proxy's replication to clients is standard actor-channel based, and DSTM's benefits (same RemoteObjectId → same proxy object → same client channel) flow through the traditional GUID cache, not through Iris net serializers.

### 6. `CachedConnectionPlayerId` Is The Connection Identifier

`APlayerController::PostMigrate(Receive)` at line 5085:
```cpp
NetConnection = GetWorld()->GetNetDriver()->FindConnection(CachedConnectionPlayerId);
```

This is how the migrated PC finds its connection on Server-B. The `CachedConnectionPlayerId` is serialized in `APlayerController::Serialize()` (line 5197-5200) and restored during deserialization. The proxy ensures that each backend server's connection has the same `PlayerId` (set from `ProxyConnection->PlayerId` at `MultiServerProxy.cpp:375`).

**This is the linchpin of DSTM migration working with the proxy.** As long as each game server's `UNetConnection::PlayerId` matches for the same physical client, `FindConnection(CachedConnectionPlayerId)` will find the correct connection on Server-B.

### 7. The `check(!ObjectLookup.Contains(NetGUID))` Assertion

The previous analysis identified `RegisterNetGUID_Server`'s assertion as a blocker for GUID-forcing approaches. With DSTM, this is not an issue because:
- The proxy uses `RegisterNetGUID_Client` (backend side, non-authority) which **allows reassignment** with a warning
- The frontend `FProxyNetGUIDCache::AssignNewNetGUID_Server` does a lookup, not a direct register — it finds the existing GUID in the shared backend cache

### 8. MultiServer Plugin Has Zero DSTM Integration

Confirmed by exhaustive search: the plugin never references `RemoteObjectTransferDelegate`, `FRemoteObjectTransferQueue`, `TransferObjectOwnershipToRemoteServer`, `PostMigrate`, or any includes of `RemoteObjectTransfer.h`. The only touchpoint is `SetUsingRemoteObjectReferences()` gated by the compile-time define.

This confirms that **Epic has not shipped the transport integration** — it's likely in an internal-only plugin or still in development.

---

## Recommended Path Forward

### Phase 1: Validate DSTM with Shared-Disk Transport (1 week) — SKIPPED

> Skipped in favor of going directly to Phase 2 (beacon-based transport).

### Phase 2: Replace Disk Transport with Beacon RPCs (1 week) — ✅ COMPLETE

1. ✅ Create `ADSTMBeaconClient` subclass with migration RPCs (in `Plugins/DSTMTransport/`)
2. ✅ Set up `UMultiServerNode` on each game server with peer connections (via `UDSTMSubsystem`)
3. ✅ Bind `RemoteObjectTransferDelegate` to beacon-based transport (in `FDSTMTransportModule`)
4. ✅ Bind `RequestRemoteObjectDelegate` to beacon-based pull-request
5. ✅ Game code integration (`NyxGameMode::MigratePlayerDSTM()` calls `UDSTMSubsystem::TransferActorToServer()`)
6. ⏳ Stress test with rapid back-and-forth boundary crossing — pending runtime testing

### Phase 3: Production Hardening (1-2 weeks) — PENDING

1. Handle edge cases: player disconnects during migration, rapid boundary crossing, network partitions
2. Integrate with external persistence layer for durable state (save/load on disconnect or crash)
3. Test with multiple simultaneous migrations
4. Profile and optimize serialization payload size
5. Add monitoring / logging for migration success/failure rates

---

## Why Common Approaches Fail

Based on analysis of the MultiServerReplication plugin architecture, here's why each common approach doesn't achieve seamlessness:

### Attempt: Deterministic GUID Forcing
- **Why it failed**: `ClientHandshakeId` differs per backend server. The proxy assigns a new `ClientHandshakeId` for each `ConnectToGameServer` call (line 311: `const uint32 ClientHandshakeId = ProxyNetDriver->GetNextClientHandshakeId()`). Server-1 might have HandshakeId=1 while Server-2 has HandshakeId=2 for the same player. Using HandshakeId as a GUID seed produces different GUIDs → different proxy objects → different client channels.
- **Why DSTM avoids this**: DSTM doesn't use GUIDs for object identity across servers. It uses `FRemoteObjectId`, which is assigned once and travels with the object. The GUID mapping on each server is local and irrelevant — what matters is that the proxy's shared backend cache maps the RemoteObjectId to the same `UObject*`.

### Attempt: `EChannelCloseReason::Migrated` Without DSTM
- **Why it partially works**: The pawn survives on the proxy and client (ShouldClientDestroyActor returns false). But the new PC from Server-B gets a different GUID → new proxy object → new client channel → PC destroy/create still happens.
- **Why DSTM completes this**: `PostMigrate(Send)` on Server-A calls `NotifyActorDestroyed(Migrated)` internally (Actor.cpp:1338). On Server-B, `PostMigrate(Receive)` adds the **same object** (same RemoteObjectId) to the world and starts replicating. The proxy finds the existing object in its shared backend cache → no new frontend channel needed.

### Attempt: Client-Side Re-binding (OnRep_Controller, Camera Integrity)
- **Why it's fragile**: Compensating for a PC swap that shouldn't happen. The proxy's `AddNetworkActor` may interfere (setting `SetReplicates(false)`), the timing of `OnRep_Controller` relative to pawn replication is unpredictable, and the camera integrity timer is a poll-based repair mechanism that can't guarantee single-frame transitions.
- **Why DSTM eliminates this**: No PC swap on the client → no re-binding needed. The client's existing PlayerController receives property updates via normal replication from the new authoritative server. Camera, input, and HUD remain continuously bound.

---

## Appendix: Source File Reference

> All paths below are relative to the UE 5.7 engine source root.

### DSTM Framework (CoreUObject)
| File | Key Content |
|------|-------------|
| `Engine/Source/Runtime/Core/Public/Misc/CoreMiscDefines.h:620` | `#define UE_WITH_REMOTE_OBJECT_HANDLE 0` — the master switch |
| `Engine/Source/Runtime/CoreUObject/Public/UObject/RemoteObjectTransfer.h` | Transport delegates, `TransferObjectOwnershipToRemoteServer`, `OnObjectDataReceived`, `FMigrationRoutingInfo` |
| `Engine/Source/Runtime/CoreUObject/Private/UObject/RemoteObjectTransfer.cpp` | `FRemoteObjectTransferQueue`, `SendRemoteObject()`, `FulfillReceiveRequest()` |
| `Engine/Source/Runtime/CoreUObject/Public/UObject/RemoteObject.h` | `FRemoteObjectStub`, `IsRemote()`, `IsOwned()`, `GetOwnerServerId()` |
| `Engine/Source/Runtime/CoreUObject/Private/UObject/RemoteObject.cpp:292-360` | `InitRemoteObjects()` — default delegate bindings to disk I/O |
| `Engine/Source/Runtime/CoreUObject/Private/UObject/RemoteObjectSerialization.cpp` | `SaveObjectToDisk()`, `LoadObjectFromDisk()`, `SerializeObjectData()` |
| `Engine/Source/Runtime/CoreUObject/Public/UObject/RemoteObjectTypes.h` | `FRemoteServerId`, `FRemoteObjectId`, `RemoteObjectSupportCompiledIn` |

### Actor Migration (Engine)
| File | Key Content |
|------|-------------|
| `Engine/Source/Runtime/Engine/Private/Actor.cpp:1264-1400` | `AActor::PostMigrate()` — world removal, `NotifyActorDestroyed(Migrated)`, tick teardown |
| `Engine/Source/Runtime/Engine/Private/PlayerController.cpp:5029-5180` | `APlayerController::PostMigrate()` — NoPawnPC swap, `CachedConnectionPlayerId`, connection rebind |
| `Engine/Source/Runtime/Engine/Private/PlayerController.cpp:6665-6718` | `ANoPawnPlayerController` implementation |

### MultiServer Plugin
| File | Key Content |
|------|-------------|
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerProxy.cpp:93-148` | `FProxyNetGUIDCache` — GUID lookup from shared backend cache |
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerProxy.cpp:658-664` | `ShouldClientDestroyActor(Migrated)` — actor preservation |
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerProxy.cpp:920-960` | `AddNetworkActor()` / `RemoveNetworkActor()` — role management + GUID cleanup |
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerProxy.cpp:1001-1005` | `ServerReplicateActors()` — proxy's re-encode to clients |
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerProxy.cpp:1214-1250` | `ReassignPlayerController()` — migration detection via type dispatch |
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerProxy.cpp:1389-1478` | Reassignment finalization — route state flip |
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerProxy.cpp:1538-1655` | `PrepareStateForRelevancy()`, `SetRemoteViewTarget()` |
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerBeaconClient.cpp` | Beacon connections, `SetUnlimitedBunchSizeAllowed`, `SetUsingRemoteObjectReferences` |
| `Engine/Plugins/Runtime/MultiServerReplication/.../MultiServerNode.h` | `UMultiServerNode` — peer-to-peer server mesh, beacon management |

### DSTMTransport Plugin (Custom — `Plugins/DSTMTransport/`)
| File | Key Content |
|------|-------------|
| `DSTMTransport.uplugin` | Plugin descriptor — depends on MultiServerReplication |
| `DSTMTransportModule.h/cpp` | Module: `InitGlobalServerId()` from `-DedicatedServerId=`, pre-binds transport delegates |
| `DSTMSubsystem.h/cpp` | Subsystem: creates DSTM beacon mesh (port +1000), routes migration data |
| `DSTMBeaconClient.h/cpp` | Beacon: `ServerReceiveMigratedObject`, `ClientReceiveMigratedObject`, `ServerRequestMigrateObject` RPCs |

**Implementation Status:** Complete. Pending runtime testing with `UE_WITH_REMOTE_OBJECT_HANDLE=1` engine build.
