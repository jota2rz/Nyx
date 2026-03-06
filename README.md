# Nyx

A research on how to build an open-world MMO with **Unreal Engine 5.7**, **SpacetimeDB 2.0**, and **Epic Online Services (EOS)**.

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

## Documentation

| File | Contents |
|------|----------|
| [RESEARCH.md](RESEARCH.md) | Full Phase 0 research log — 21 spikes covering plugin integration, Rust server module, round-trip validation, EOS auth, spatial interest management, client-side prediction, WASM benchmarks, physics sidecar, Docker deployment, cross-server transfer, MultiServer proxy routing, and seamless pawn authority migration |
| [MULTISERVER.md](MULTISERVER.md) | MultiServer Replication Plugin analysis — GUID coordination, proxy routing flags, and seamless pawn authority migration protocol |
