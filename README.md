# Nyx

An open-world MMO built with **Unreal Engine 5.7**, **SpacetimeDB 2.0**, and **Epic Online Services (EOS)**.

## Structure

| Directory | Description |
|-----------|-------------|
| `Source/Nyx/` | UE5 C++ game module — networking, entity management, physics sidecar |
| `server/nyx-server/` | SpacetimeDB Rust server module (game logic, tables, reducers) |
| `Plugins/SpacetimeDbSdk/` | SpacetimeDB Unreal SDK plugin |
| `Config/` | UE5 project configuration |
| `Content/` | UE5 assets and maps |

## Documentation

See **[RESEARCH.md](RESEARCH.md)** for the full Phase 0 research log — nine completed spikes covering plugin integration, Rust server module, round-trip validation, EOS auth, spatial interest management, client-side prediction, WASM benchmarks, physics sidecar architecture, and scalability analysis. Also includes reference architecture reviews of BitCraft, MultiServer Replication, and OWS.
