# Nyx

A research on how to build an open-world MMO with **Unreal Engine 5.7**, **SpacetimeDB 2.0**, and **Epic Online Services (EOS)**.

## Current State

**Working end-to-end:** Client connects through a proxy to two dedicated servers (west/east zones). Player spawns, moves, fights, and crosses the zone boundary — player migration is handled by the [MultiServerReplicationEx](https://github.com/jota2rz/MultiServerReplicationEx) plugin using the engine's built-in DSTM framework.

**Key achievements:**
- Proxy-based multi-server architecture with DSTM seamless zone migration
- SpacetimeDB persistence (character state, HP, position)
- EOS authentication
- Custom replication graph with spatial interest management
- GUID collisions structurally prevented by DSTM's `FRemoteObjectId` (10-bit `ServerId` embedded in every `FNetworkGUID`)
- Canvas-based HUD showing server, zone, position, HP/MP

## Structure

| Directory | Description |
|-----------|-------------|
| `Source/Nyx/Core/` | Game mode, game instance — zone boundaries, server lifecycle |
| `Source/Nyx/Player/` | Character — replication, stats, input |
| `Source/Nyx/UI/` | HUD — server/zone/HP/MP overlay |
| `Source/Nyx/Networking/` | ReplicationGraph, MultiServer beacon/subsystem |
| `Source/Nyx/Server/` | Server-side SpacetimeDB subsystem (persistence, combat) |
| `Source/Nyx/Online/` | EOS authentication subsystem |
| `Source/Nyx/World/` | Zone boundary visuals |
| `Source/Nyx/Test/` | Smoke test commandlet |
| `Source/Nyx/Data/` | Shared types and enums |
| `Source/Nyx/Public/ModuleBindings/` | Auto-generated SpacetimeDB C++ bindings |
| `Source/Nyx/Private/ModuleBindings/` | Auto-generated SpacetimeDB C++ bindings (impl) |
| `Source/Nyx/Sidecar/` | *(deprecated)* Physics sidecar subsystem — no longer used |
| `server/nyx-server/` | *(deprecated)* SpacetimeDB Rust server module — no longer used |
| `Plugins/MultiServerReplicationEx/` | *(submodule)* [MultiServerReplicationEx](https://github.com/jota2rz/MultiServerReplicationEx) — beacon-based DSTM transport plugin for cross-server actor migration |
| `Plugins/SpacetimeDbSdk/` | SpacetimeDB Unreal SDK plugin |
| `Config/` | UE5 project configuration |
| `Content/` | UE5 assets and maps |

## Test Launch

```powershell
# Use launch_test.ps1 for a ready-to-run 5-process test (proxy + 2 servers + 2 clients):
powershell -ExecutionPolicy Bypass -File "C:\UE\Nyx\launch_test.ps1"
```

Or launch manually:

```powershell
$server = "C:\UE\Nyx\Binaries\Win64\NyxServer.exe"
$editor = "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
$proj = "C:\UE\Nyx\Nyx.uproject"

# Proxy (registration beacon on 17000, clients connect on 7780)
Start-Process $server "`"$proj`" -server -port=7780 -log -NOSTEAM -DisableGarbageElimination -DedicatedServerId=proxy-1 -ProxyRegistrationPort=17000"

# Wait ~10s for proxy to start

# Server-1 (west zone) — registers with proxy via beacon
Start-Process $server "`"$proj`" -server -port=7777 -log -NOSTEAM -DisableGarbageElimination -DedicatedServerId=server-1 -ZoneSide=west -NyxMultiServerListenPort=15000 -DSTMListenPort=16000 -JoinProxy=127.0.0.1:17000"

# Server-2 (east zone) — registers with proxy via beacon
Start-Process $server "`"$proj`" -server -port=7778 -log -NOSTEAM -DisableGarbageElimination -DedicatedServerId=server-2 -ZoneSide=east -NyxMultiServerListenPort=15001 -DSTMListenPort=16001 -JoinProxy=127.0.0.1:17000"

# Wait ~25s for servers to register and meshes to connect

# Client
Start-Process $editor "`"$proj`" 127.0.0.1:7780 -game -WINDOWED -ResX=800 -ResY=600 -NOSTEAM"
```

Walk east to cross the zone boundary at X=0. The DSTM framework handles the transfer automatically — the engine serializes the PlayerController + Pawn, sends via beacon RPC to the destination server, and rebinds the connection without the client disconnecting.

See the [MultiServerReplicationEx plugin documentation](https://github.com/jota2rz/MultiServerReplicationEx) for command-line argument reference and architecture details.

## Documentation

| File | Contents |
|------|----------|
| [RESEARCH.md](RESEARCH.md) | Full Phase 0 research log — 21 spikes covering plugin integration, Rust server module, round-trip validation, EOS auth, spatial interest management, client-side prediction, WASM benchmarks, physics sidecar, Docker deployment, cross-server transfer, MultiServer proxy routing, and seamless pawn authority migration |
| [MULTISERVER.md](MULTISERVER.md) | MultiServer Replication Plugin analysis — GUID coordination, proxy routing, migration protocol |
| [SEAMLESS.md](SEAMLESS.md) | Seamless cross-server migration analysis — DSTM framework audit, three implementation approaches, and recommended path using beacon-based transport |
| [DSTM.md](DSTM.md) | Redirect → [MultiServerReplicationEx plugin repo](https://github.com/jota2rz/MultiServerReplicationEx) |
