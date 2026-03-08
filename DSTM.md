# DSTMTransport Plugin

A plugin for Unreal Engine 5.7 that completes the engine's built-in DSTM (Distributed State Transfer Machine) framework for seamless cross-server actor migration. It replaces the default disk-based transport with a real-time beacon mesh, enabling servers to push serialized actors directly to each other over the network without the client disconnecting.

## Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Architecture](#architecture)
- [Adding the Plugin to Your Project](#adding-the-plugin-to-your-project)
- [Engine Build Requirement](#engine-build-requirement)
- [Command-Line Arguments](#command-line-arguments)
- [Initialization](#initialization)
- [Migrating an Actor](#migrating-an-actor)
- [Migration Flow Reference](#migration-flow-reference)
- [Pull Migration](#pull-migration)
- [Beacon Mesh and Port Offset](#beacon-mesh-and-port-offset)
- [Logging](#logging)
- [Troubleshooting](#troubleshooting)

---

## Overview

Unreal Engine 5.7 ships a DSTM framework (`UE::RemoteObject::Transfer`) that can serialize any actor—including its player controller, possessed pawn, and all subobjects—and reconstitute it on another server without the client noticing a disconnect. By default, the engine expects a disk- or platform-specific transport layer to move the serialized blob between servers. **DSTMTransport** provides that transport layer on top of the `MultiServerReplication` plugin's beacon mesh.

The plugin consists of three cooperating classes:

| Class | Responsibility |
|-------|---------------|
| `FDSTMTransportModule` | Module startup: initializes the server's `FRemoteServerId` and pre-binds the engine transport delegates |
| `UDSTMSubsystem` | Runtime: manages the DSTM beacon mesh, routes outgoing and incoming migration data, handles pull-requests |
| `ADSTMBeaconClient` | Network: extends `AMultiServerBeaconClient` with reliable RPCs that carry serialized `FRemoteObjectData` |

---

## Prerequisites

- Unreal Engine 5.7 (custom build with `UE_WITH_REMOTE_OBJECT_HANDLE=1`)
- `MultiServerReplication` plugin (ships with UE 5.7)
- A dedicated-server topology where each server process has a unique string ID

---

## Architecture

```
Server-A                        Beacon Mesh                      Server-B
────────                        ───────────                      ────────
TransferActorToServer(PC)
  └─► TransferObjectOwnership   [serialize Actor + subobjects]
        ToRemoteServer()
            │
            ▼
  RemoteObjectTransferDelegate
  (bound by DSTMTransportModule)
            │
            ▼
  HandleOutgoingMigration()     ──Serialize FRemoteObjectData──►
  [FMemoryWriter → TArray<u8>]     ServerReceiveMigratedObject()
                                   or ClientReceiveMigratedObject()
                                        │
                                        ▼
                                  HandleIncomingMigrationData()
                                  [FMemoryReader ← TArray<u8>]
                                        │
                                        ▼
                                  OnObjectDataReceived()
                                  [engine DSTM receive pipeline]
                                        │
                                        ▼
                                  AActor::PostMigrate(Receive)
                                  APlayerController::PostMigrate(Receive)
```

The DSTM beacon mesh is a separate `UMultiServerNode` instance from any game-level multi-server mesh. It listens on a port offset (+1000 by default) from the main MultiServer mesh, keeping the transport concern isolated inside the plugin.

---

## Adding the Plugin to Your Project

1. Copy the `Plugins/DSTMTransport` folder into your project's `Plugins/` directory.

2. Add `DSTMTransport` to the plugins list in your `.uproject` file:

   ```json
   {
     "Plugins": [
       { "Name": "MultiServerReplication", "Enabled": true },
       { "Name": "DSTMTransport", "Enabled": true }
     ]
   }
   ```

3. Add `DSTMTransport` to your game module's `PublicDependencyModuleNames` if you call subsystem methods directly, or `PrivateDependencyModuleNames` if you only call through the game instance:

   ```cs
   // YourGame.Build.cs
   PrivateDependencyModuleNames.AddRange(new string[]
   {
       "DSTMTransport",
   });
   ```

4. Rebuild your project.

---

## Engine Build Requirement

The plugin requires an engine build where `UE_WITH_REMOTE_OBJECT_HANDLE` is set to `1`. Verify this in your engine's build configuration or by checking whether `RemoteObjectTypes.h` and `RemoteObjectTransfer.h` exist under `Engine/Source/Runtime/CoreUObject/Public/UObject/`.

If the define is absent, the plugin compiles but remains inert: the module logs a warning, skips delegate binding, and the subsystem reports `IsMeshActive() == false`.

---

## Command-Line Arguments

Each server process that participates in the DSTM mesh must receive these arguments:

| Argument | Required | Description |
|----------|----------|-------------|
| `-DedicatedServerId=<string>` | Yes | Unique string identifier for this server (e.g. `server-1`). Hashed to a `uint32` via `GetTypeHash()` to produce the `FRemoteServerId`. Must be the same string used by your game code to identify this server. |
| `-MultiServerLocalId=<string>` | Yes | Peer ID used in the MultiServer beacon mesh. Should match `-DedicatedServerId=` for correct peer routing. |
| `-MultiServerListenPort=<int>` | Yes | Base port for the main MultiServer mesh. The DSTM mesh listens on this port + 1000. |
| `-MultiServerListenIp=<ip>` | No | IP address to bind the DSTM beacon listener. Defaults to `0.0.0.0`. |
| `-MultiServerPeers=<ip:port,...>` | Yes (multi-server) | Comma-separated list of `host:port` pairs for other servers' **main** MultiServer mesh ports. The plugin automatically adds +1000 to each port for the DSTM mesh. |
| `-MultiServerNumServers=<int>` | No | Total number of servers in the cluster. Used to determine when `AreAllPeersConnected()` returns true. |

### Example (two-server cluster)

```
# Server 1
-DedicatedServerId=server-1
-MultiServerLocalId=server-1
-MultiServerListenPort=15000
-MultiServerPeers=127.0.0.1:15001
-MultiServerNumServers=2

# Server 2
-DedicatedServerId=server-2
-MultiServerLocalId=server-2
-MultiServerListenPort=15001
-MultiServerPeers=127.0.0.1:15000
-MultiServerNumServers=2
```

With these arguments:
- Server 1 DSTM beacon listens on port **16000** (15000 + 1000)
- Server 2 DSTM beacon listens on port **16001** (15001 + 1000)
- Each server connects its DSTM beacon to the other's DSTM port

---

## Initialization

### Why `Initialize()` does not auto-start the mesh

`UGameInstanceSubsystem::Initialize()` runs during `GameInstance` creation, before any `UWorld` exists. `GetWorld()` returns `nullptr` at that point. Creating a beacon mesh requires a valid world, so the subsystem starts inert and waits to be told when to initialize.

### Calling from your Game Mode

Call `InitializeFromCommandLine()` from your game mode's `StartPlay()` (or equivalent) once the world is ready:

```cpp
// YourGameMode.cpp
#include "DSTMSubsystem.h"

void AYourGameMode::StartPlay()
{
    Super::StartPlay();

    if (UGameInstance* GI = GetGameInstance())
    {
        if (UDSTMSubsystem* DSTM = GI->GetSubsystem<UDSTMSubsystem>())
        {
            DSTM->InitializeFromCommandLine();
        }
    }
}
```

`InitializeFromCommandLine()` reads the command-line arguments described above, computes the DSTM port, and calls `InitializeDSTMMesh()` internally. It returns `true` if the mesh was created or `false` if the process is not in multi-server mode (no `-MultiServerLocalId=` present).

### Explicit initialization (without command-line args)

```cpp
UDSTMSubsystem* DSTM = GI->GetSubsystem<UDSTMSubsystem>();

TArray<FString> Peers = { TEXT("192.168.1.20:16001") };
DSTM->InitializeDSTMMesh(
    TEXT("server-1"),   // LocalPeerId
    TEXT("0.0.0.0"),    // ListenIp
    16000,              // ListenPort  (already offset)
    2,                  // NumServers
    Peers               // PeerAddresses (already offset)
);
```

When providing peer addresses explicitly, include the DSTM port offset yourself (i.e. the actual DSTM port, not the base MultiServer port).

### Checking readiness

```cpp
if (DSTM->IsMeshActive() && DSTM->AreAllPeersConnected())
{
    // Safe to call TransferActorToServer()
}
```

`IsMeshActive()` — returns `true` once `InitializeDSTMMesh()` succeeds.  
`AreAllPeersConnected()` — returns `true` when every expected peer has an established beacon connection.

---

## Migrating an Actor

```cpp
// Get the subsystem
UDSTMSubsystem* DSTM = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();

// Resolve the destination server's FRemoteServerId from its string ID
FRemoteServerId DestId = UDSTMSubsystem::GetRemoteServerIdFromString(TEXT("server-2"));

// Transfer the actor — serializes PC + all subobjects including possessed Pawn.
// Do NOT call this separately for the Pawn; it is included automatically.
DSTM->TransferActorToServer(PlayerController, DestId);
```

`TransferActorToServer()` calls `UE::RemoteObject::Transfer::TransferObjectOwnershipToRemoteServer()`, which:

1. Serializes the actor and all its subobjects into `FRemoteObjectData`
2. Calls `AActor::PostMigrate(Send)` — removes the actor from the world, closes the replication channel with the `Migrated` flag
3. For player controllers: calls `APlayerController::PostMigrate(Send)` — swaps in a `NoPawnPC`, saves the connection handle
4. Invokes `RemoteObjectTransferDelegate` → `HandleOutgoingMigration()` → sends via beacon RPC

> **Important:** Only pass the `PlayerController`. The possessed `Pawn` is automatically included as a subobject. Passing both separately causes a double-transfer and will corrupt the migration.

### Convenience: first connected peer

For a two-server setup where there is exactly one peer:

```cpp
FRemoteServerId PeerId;
if (DSTM->GetFirstPeerServerId(PeerId))
{
    DSTM->TransferActorToServer(PlayerController, PeerId);
}
```

---

## Migration Flow Reference

### Push (source server sends the actor)

```
Source server calls TransferActorToServer(Actor, DestServerId)
  │
  ├─ Engine: TransferObjectOwnershipToRemoteServer()
  │    ├─ Serialize Actor + subobjects → FRemoteObjectData
  │    └─ Call RemoteObjectTransferDelegate
  │
  └─ DSTMTransportModule::OnRemoteObjectTransfer()
       └─ UDSTMSubsystem::HandleOutgoingMigration()
            ├─ Serialize FRemoteObjectData → TArray<uint8> via FMemoryWriter
            ├─ Look up ADSTMBeaconClient for DestServerId
            └─ Send via RPC:
                 HasAuthority() == true  → ClientReceiveMigratedObject()
                 HasAuthority() == false → ServerReceiveMigratedObject()

Destination server receives RPC
  └─ ADSTMBeaconClient fires OnMigrationDataReceived
       └─ UDSTMSubsystem::HandleIncomingMigrationData()
            ├─ Deserialize TArray<uint8> → FRemoteObjectData via FMemoryReader
            └─ UE::RemoteObject::Transfer::OnObjectDataReceived()
                 ├─ Deserialize Actor + subobjects into existing C++ object
                 ├─ AActor::PostMigrate(Receive) → add to world, begin replication
                 └─ APlayerController::PostMigrate(Receive) → bind to connection
```

### RPC direction

Each server-to-server beacon connection has one side with authority (`HasAuthority() == true`) and one without. The plugin checks authority at send time to select the correct RPC direction so that the UE networking stack accepts the call:

- Server side of beacon → sends via **Client RPC** to reach the other process
- Client side of beacon → sends via **Server RPC** to reach the other process

This applies identically to both data-transfer RPCs and pull-request RPCs.

---

## Pull Migration

A "pull" migration happens when a destination server requests an object that still lives on another server—for example, when the engine's DSTM scheduler determines that an object should move before the source server has initiated it.

The engine calls `RequestRemoteObjectDelegate` on the destination server. The plugin handles this with `HandleObjectRequest()`:

1. Looks up the beacon for `LastKnownServerId`
2. Sends `ServerRequestMigrateObject()` or `ClientRequestMigrateObject()` depending on beacon authority

The source server receives the request, fires `OnMigrationRequested`, and `HandleIncomingMigrationRequest()`:

1. Iterates all world actors, matching `FRemoteObjectHandle.GetRemoteObjectId()` against the requested `FRemoteObjectId`
2. Calls `TransferActorToServer(FoundActor, RequestingServerId)` — which triggers the normal push flow

---

## Beacon Mesh and Port Offset

The plugin creates a dedicated `UMultiServerNode` separate from any game-level multi-server mesh. This keeps the DSTM transport concern fully inside the plugin.

The DSTM mesh port is computed as:

```
DSTMListenPort = MultiServerListenPort + DSTMPortOffset
```

where `DSTMPortOffset = 1000` (a compile-time constant in `UDSTMSubsystem`).

Peer addresses supplied via `-MultiServerPeers=` use the **base** MultiServer port. The plugin rewrites them automatically by adding `DSTMPortOffset` to each port before creating the mesh.

If you initialize the mesh explicitly (not via command-line), supply the already-offset DSTM ports in `PeerAddresses`.

### Server identity hashing

`FRemoteServerId` is a `uint32`. The plugin derives it from a human-readable string (`DedicatedServerId`) using `GetTypeHash(FString)`:

```cpp
FRemoteServerId id = FRemoteServerId(GetTypeHash(TEXT("server-1")));
```

`GetRemoteServerIdFromString()` performs this hash publicly so your game code can produce the same value when specifying migration targets.

---

## Logging

| Log category | Used in |
|---|---|
| `LogDSTM` | `FDSTMTransportModule` — module startup, delegate binding, server identity |
| `LogDSTMSub` | `UDSTMSubsystem` — mesh lifecycle, peer connections, migration send/receive |

Enable verbose output:

```ini
; DefaultEngine.ini
[Core.Log]
LogDSTM=Verbose
LogDSTMSub=Verbose
```

---

## Troubleshooting

### `UE_WITH_REMOTE_OBJECT_HANDLE is disabled` warning at startup

Your engine build does not have DSTM support compiled in. The plugin requires a custom UE 5.7 build with `UE_WITH_REMOTE_OBJECT_HANDLE=1` defined in `RemoteObjectHandle.h` or the build environment.

### `No -DedicatedServerId= on command line` — migration never starts

Each server process must receive `-DedicatedServerId=<unique-string>`. Without it, `FRemoteServerId::InitGlobalServerId()` is never called, delegate binding is skipped, and the engine has no server identity for routing.

### `No beacon connection to destination server N! Migration data lost`

The DSTM beacon mesh has not finished connecting to the target server. Ensure:
- Both servers are running and have received `-MultiServerPeers=` pointing to each other's **base** ports
- `AreAllPeersConnected()` returns `true` before initiating the first migration
- Firewall rules allow traffic on both the base port and the base port + 1000

### Double-transfer: PlayerController and Pawn both passed separately

`TransferObjectOwnershipToRemoteServer()` serializes the passed actor **and all its subobjects**, including the possessed `Pawn`. Passing the `Pawn` to `TransferActorToServer()` in addition to the `PlayerController` causes two migration payloads and results in undefined behavior on the destination server. Pass only the `PlayerController`.

### `Failed to deserialize FRemoteObjectData` on receive

A serialization version mismatch between the two servers. Both server binaries must be built from the same source. This error can also occur if the network payload was truncated—ensure the beacon allows unlimited bunch sizes (`SetUnlimitedBunchSizeAllowed(true)` is inherited from `AMultiServerBeaconClient`).

### DSTM mesh created but `AreAllPeersConnected()` never returns `true`

Verify that `-MultiServerNumServers=` matches the actual number of servers minus one (or the number of distinct peers each server should connect to). If you omit this argument, the default is `1`, so a two-server cluster will report all peers connected as soon as one peer connects—even if additional peers are expected.
