# Nyx

A research on how to build an open-world MMO with **Unreal Engine 5.7**, **SpacetimeDB 2.0**, and **Epic Online Services (EOS)**.

## Current State

**Working end-to-end:** Client connects through a proxy to two dedicated servers (west/east zones). Player spawns, moves, fights, and crosses the zone boundary — pawn authority migrates seamlessly between servers without the client disconnecting.

**Key achievements:**
- Proxy-based multi-server architecture with transparent zone migration
- SpacetimeDB persistence (character state, HP, position)
- EOS authentication
- Custom replication graph with spatial interest management
- GUID collision prevention across servers (`-NyxGuidSeed=`)
- Client-side prediction with server reconciliation (custom CMC)
- Canvas-based HUD showing server, zone, position, HP/MP

**Known issues being worked on:**
- After migration, the client briefly sees N/A on HUD (new PC from server-2 needs HUD re-creation)
- Camera can get stuck if migration timing is unlucky (race between Server-1 release and Server-2 claim)
- Pre-migration ghost pawn from non-primary server is visible on client before migration completes

## Structure

| Directory | Description |
|-----------|-------------|
| `Source/Nyx/Core/` | Game mode, game instance — zone boundaries, server lifecycle |
| `Source/Nyx/Player/` | Character, movement component — replication, stats, input |
| `Source/Nyx/UI/` | HUD — server/zone/HP/MP overlay |
| `Source/Nyx/Networking/` | ReplicationGraph, MultiServer beacon/subsystem |
| `Source/Nyx/Server/` | Server-side SpacetimeDB subsystem (persistence, combat) |
| `Source/Nyx/Sidecar/` | Physics sidecar subsystem |
| `Source/Nyx/Online/` | EOS authentication subsystem |
| `Source/Nyx/World/` | Zone boundary visuals |
| `Source/Nyx/Test/` | Smoke test commandlet |
| `Source/Nyx/Data/` | Shared types and enums |
| `Source/Nyx/Public/ModuleBindings/` | Auto-generated SpacetimeDB C++ bindings |
| `Source/Nyx/Private/ModuleBindings/` | Auto-generated SpacetimeDB C++ bindings (impl) |
| `server/nyx-server/` | SpacetimeDB Rust server module (tables, reducers) |
| `Plugins/SpacetimeDbSdk/` | SpacetimeDB Unreal SDK plugin |
| `Config/` | UE5 project configuration |
| `Content/` | UE5 assets and maps |

## Test Launch

```powershell
$ue = "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe"
$proj = "C:\UE\Nyx\Nyx.uproject"

# Server-1 (west zone)
Start-Process $ue "$proj -server -port=7777 -log -NOSTEAM -DedicatedServerId=server-1 -ZoneSide=west -NyxGuidSeed=100000 -abslog=C:\UE\Nyx\server1_log.txt"

# Server-2 (east zone)
Start-Process $ue "$proj -server -port=7778 -log -NOSTEAM -DedicatedServerId=server-2 -ZoneSide=east -NyxGuidSeed=200000 -abslog=C:\UE\Nyx\server2_log.txt"

# Wait ~15s for servers to start

# Proxy (connects to both servers)
Start-Process $ue "$proj -server -port=7780 -log -NOSTEAM -ProxyGameServers=127.0.0.1:7777,127.0.0.1:7778 -abslog=C:\UE\Nyx\proxy1_log.txt"

# Wait ~20s for proxy to connect to backends

# Client
Start-Process $ue "$proj 127.0.0.1:7780 -game -WINDOWED -ResX=800 -ResY=600 -abslog=C:\UE\Nyx\client1_log.txt -NOSTEAM"
```

Walk east to cross the zone boundary at X=0. Cyan pillars = west/server-1, orange pillars = east/server-2.

## Documentation

| File | Contents |
|------|----------|
| [RESEARCH.md](RESEARCH.md) | Full Phase 0 research log — 21 spikes covering plugin integration, Rust server module, round-trip validation, EOS auth, spatial interest management, client-side prediction, WASM benchmarks, physics sidecar, Docker deployment, cross-server transfer, MultiServer proxy routing, and seamless pawn authority migration |
| [MULTISERVER.md](MULTISERVER.md) | MultiServer Replication Plugin analysis — GUID coordination, proxy routing, migration protocol, and client-side migration issues |
