# Phase 0: Research Spikes — SpacetimeDB 2.0 + UE5 + EOS MMO

> **Project:** Nyx MMO  
> **Engine:** Unreal Engine 5.7  
> **Server/DB:** SpacetimeDB 2.0  
> **Auth:** Epic Online Services (EOS)  
> **IDE:** Visual Studio Code  

---

## Overview

Before writing production game logic, we must validate every integration point between SpacetimeDB 2.0, Unreal Engine 5.7, and Epic Online Services. Each spike below is a time-boxed investigation with a clear deliverable. No spike should exceed **3 days**. If a spike reveals a blocker, we pivot before wasting time on downstream work.

---

## Spike 1: SpacetimeDB 2.0 Unreal Plugin Integration ✅

**Goal:** Confirm the official SpacetimeDB Unreal SDK plugin compiles and connects from our UE5.7 project.

**Duration:** 1–2 days (actual: ~2 days)

**Status:** COMPLETE — Full round-trip verified (connect → subscribe → reducer → DB row confirmed)

### Tasks

1. **Install SpacetimeDB CLI** ✅
   - Installed SpacetimeDB CLI v2.0.2 via PowerShell installer
   - Installs to `%LOCALAPPDATA%\SpacetimeDB\` — must add to PATH each terminal session:
     ```powershell
     $installDir = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "SpacetimeDB"
     $env:PATH = "$installDir;$env:PATH"
     ```
   - `spacetime version` → `spacetimedb tool version 2.0.2`
   - `spacetime start` boots local instance on `127.0.0.1:3000`

2. **Install Rust Toolchain** ✅
   - Installed Rust 1.93.1 (stable-x86_64-pc-windows-msvc)
   - Added `wasm32-unknown-unknown` target: `rustup target add wasm32-unknown-unknown`
   - Required for building SpacetimeDB server modules to WASM

3. **Clone the SpacetimeDB Unreal Plugin** ✅
   - Plugin lives in main SpacetimeDB repo at `sdks/unreal/src/SpacetimeDbSdk/` (NOT `crates/sdk/`)
   - Cloned and copied to `C:\UE\Nyx\Plugins\SpacetimeDbSdk\`
   - **Module name is `SpacetimeDbSdk`** (not `SpacetimeDB`)

4. **Add Plugin to Project** ✅
   - Added to `Nyx.uproject`:
     ```json
     {
       "Name": "SpacetimeDbSdk",
       "Enabled": true
     }
     ```
   - Added `"SpacetimeDbSdk"` to `Nyx.Build.cs` PrivateDependencyModuleNames
   - Also added `bEnableExceptions = true` and `CppStandard = CppStandardVersion.Cpp20` to Build.cs

5. **Compile and Verify** ✅
   - Build `NyxEditor Win64 Development` — clean compile
   - `UDbConnection`, `URemoteTables`, `USubscriptionBuilder`, `UDbConnectionBuilder` all accessible
   - Plugin DLL: `UnrealEditor-SpacetimeDbSdk.dll` (1.2 MB)

### Deliverable
- [x] Plugin compiles with UE5.7 — no patches needed
- [x] Can `#include` SpacetimeDB headers in Nyx source
- [x] No UE5.7-specific patches required

### Answers to Key Questions
- **Does the plugin support UE5.7 out of the box?** Yes, compiled without any patches.
- **What is the plugin's module name?** `SpacetimeDbSdk` (note the casing and `Sdk` suffix)
- **Does it conflict with any default UE5 plugins?** No conflicts observed.

### Critical Discovery: Auto-Ticking
The SDK's `UDbConnectionBase` inherits `FTickableGameObject` but `bIsAutoTicking` defaults to `false`.
`BuildConnection()` does NOT enable it. You **must** call `SetAutoTicking(true)` after `Build()` or
incoming WebSocket messages will queue up and never be processed (no callbacks fire).
```cpp
SpacetimeDBConnection = Builder->...->Build();
SpacetimeDBConnection->SetAutoTicking(true); // REQUIRED!
```

---

## Spike 2: SpacetimeDB 2.0 Module (Server-Side) Hello World ✅

**Goal:** Write, deploy, and interact with a minimal SpacetimeDB module.

**Duration:** 1–2 days (actual: completed same session as Spike 1)

**Status:** COMPLETE — Module published, bindings generated and compiling

### Tasks

1. **Create a Rust Module** ✅
   ```bash
   mkdir server
   cd server
   spacetime init --lang rust nyx-server
   ```
   - Actual path: `C:\UE\Nyx\server\nyx-server\`

2. **Define a Minimal Schema** ✅
   ```rust
   // server/nyx-server/spacetimedb/src/lib.rs
   use spacetimedb::{ReducerContext, Table, Identity, Timestamp};

   #[spacetimedb::table(name = player, public)]
   pub struct Player {
       #[primary_key]
       identity: Identity,
       display_name: String,
       pos_x: f32,
       pos_y: f32,
       pos_z: f32,
       rot_yaw: f32,
       last_update: Timestamp,
   }

   #[spacetimedb::reducer]
   pub fn create_player(ctx: &ReducerContext, display_name: String) {
       ctx.db.player().insert(Player {
           identity: ctx.sender(),
           display_name,
           pos_x: 0.0, pos_y: 0.0, pos_z: 100.0,
           rot_yaw: 0.0,
           last_update: ctx.timestamp,
       });
   }

   #[spacetimedb::reducer]
   pub fn move_player(ctx: &ReducerContext, x: f32, y: f32, z: f32, yaw: f32) {
       if let Some(mut p) = ctx.db.player().identity().find(ctx.sender()) {
           p.pos_x = x; p.pos_y = y; p.pos_z = z;
           p.rot_yaw = yaw;
           p.last_update = ctx.timestamp;
           ctx.db.player().identity().update(p);
       }
   }
   ```
   - **Key SpacetimeDB 2.0 API differences from docs:**
     - `ctx.sender()` is a method (not a field)
     - `ctx.timestamp` is a field (not a method)
     - Primary key is `Identity` type (not auto-inc u64)
     - Lifecycle reducers: `init`, `client_connected`, `client_disconnected`

3. **Publish Locally** ✅
   ```bash
   spacetime publish nyx --project-path server/nyx-server --server local
   ```
   - Published to database name `nyx` (not `nyx-world`)
   - Running on `127.0.0.1:3000`

4. **Generate UE5 Bindings** ✅
   ```bash
   spacetime generate --lang unrealcpp --project-path server/nyx-server --out-dir Source/Nyx
   ```
   - Output goes to:
     - `Source/Nyx/Public/ModuleBindings/` — headers (SpacetimeDBClient.g.h, types, etc.)
     - `Source/Nyx/Private/ModuleBindings/` — implementations (SpacetimeDBClient.g.cpp, etc.)
   - **Generated ~849 lines** in SpacetimeDBClient.g.h including:
     - `UDbConnection` (extends `UDbConnectionBase`)
     - `UDbConnectionBuilder` (builder pattern: `WithUri`, `WithDatabaseName`, `OnConnect`, etc.)
     - `URemoteTables` (has `UPlayerTable* Player`)
     - `URemoteReducers` (`CreatePlayer`, `MovePlayer` methods)
     - `USubscriptionBuilder` (`OnApplied`, `OnError`, `Subscribe`)
     - `FEventContext`, `FReducerEventContext`, `FSubscriptionEventContext`, `FErrorContext`

5. **Verify Generated Code** ✅
   - All generated code compiles cleanly within the Nyx module
   - No manual edits needed on generated files

### Deliverable
- [x] Module publishes to local SpacetimeDB without errors
- [x] `spacetime generate` produces Unreal C++ bindings
- [x] Generated code compiles within the Nyx project
- [x] Codegen command and output directory documented above

### Answers to Key Questions
- **What is the exact codegen output structure?** Headers in `Public/ModuleBindings/`, implementations in `Private/ModuleBindings/`
- **Does codegen produce a separate UE module?** No, files go into the existing Nyx module
- **What's the iteration loop?** Edit Rust → `spacetime publish` → `spacetime generate` → rebuild UE5. Consider a batch script.

---

## Spike 3: UE5 ↔ SpacetimeDB Round-Trip Connection ✅

**Goal:** Connect from UE5, call a reducer, subscribe to a table, and verify round-trip data flow.

**Duration:** 2–3 days (actual: completed same session as Spikes 1–2)

**Status:** COMPLETE — Full round-trip verified: connect → subscribe → create_player → row in DB

### What We Actually Built (instead of a test actor)

Instead of a standalone test actor, the round-trip was integrated into the existing subsystem architecture:

1. **NyxNetworkSubsystem** — owns the `UDbConnection`, handles connect/subscribe/reducer calls
2. **NyxGameInstance** — console commands (`Nyx.Connect`, `Nyx.ConnectMock`, `Nyx.Disconnect`, `Nyx.StartGame`)
3. **NyxGameMode** — auto-login support via `bAutoLoginMock`

### Connection Flow (verified working)
```cpp
// NyxNetworkSubsystem::ConnectToServer() — real SpacetimeDB path
FOnConnectDelegate OnConnect;
OnConnect.BindDynamic(this, &UNyxNetworkSubsystem::HandleSpacetimeDBConnect);

UDbConnectionBuilder* Builder = UDbConnection::Builder();
SpacetimeDBConnection = Builder
    ->WithUri(FString::Printf(TEXT("ws://%s"), *Host))
    ->WithDatabaseName(DatabaseName)
    ->OnConnect(OnConnect)
    ->OnConnectError(OnConnectError)
    ->OnDisconnect(OnDisconnect)
    ->Build();

SpacetimeDBConnection->SetAutoTicking(true); // REQUIRED — see Spike 1 notes

// HandleSpacetimeDBConnect callback:
USubscriptionBuilder* SubBuilder = Connection->SubscriptionBuilder();
SubBuilder->OnApplied(OnApplied);
SubBuilder->OnError(OnError);
SubBuilder->Subscribe({TEXT("SELECT * FROM player")});

// HandleSubscriptionApplied callback:
Context.Reducers->CreatePlayer(TEXT("NyxTestPlayer"));
```

### Console Commands (for editor testing)
| Command | Description |
|---------|-------------|
| `Nyx.Connect [Host] [Database]` | Connect to real SpacetimeDB (default: 127.0.0.1:3000 / nyx) |
| `Nyx.ConnectMock` | Connect using mock backend |
| `Nyx.Disconnect` | Disconnect current connection |
| `Nyx.StartGame [mock]` | Full login flow (auth → connect) |

### Verified Output Log (editor PIE session)
```
LogSpacetimeDb_Connection: UWebsocketManager: WebSocket Connected.
LogNyxNet: SpacetimeDB connected! Token length=386
LogNyxNet: Connection state changed to: 2
LogNyxNet: Subscribed to player table
LogNyxNet: SpacetimeDB subscription applied successfully!
LogNyxNet: Calling create_player reducer...
```

### Database Verification
```sql
spacetime sql --server local nyx "SELECT * FROM player"
-- Result: 1 row with identity, display_name="NyxTestPlayer", pos=(0,0,100)
```

### Deliverable
- [x] UE5 connects to local SpacetimeDB
- [x] Reducer calls work (create_player verified, move_player defined)
- [ ] OnInsert / OnUpdate / OnDelete callbacks — not yet wired to table events (deferred)
- [ ] Subscription queries with filters — not tested yet
- [ ] Latency numbers — not measured yet
- [ ] Subscription update (change query at runtime) — not tested yet

### Answers to Key Questions
- **What is the round-trip latency?** Not formally measured yet. Qualitatively instant on localhost.
- **Can we update subscriptions dynamically?** SDK supports it via `USubscriptionHandleBase`. Not tested yet.
- **What SQL subset is supported?** `SELECT * FROM table` works. `WHERE` clauses supported per server docs.
- **Does `UDbConnection` auto-tick?** **NO!** `bIsAutoTicking` defaults to `false`. Must call `SetAutoTicking(true)` after `Build()`. This was the biggest gotcha — without it, no callbacks fire.

### Configuration Notes
- `DefaultEngine.ini` must set custom GameInstance and GameMode:
  ```ini
  [/Script/EngineSettings.GameMapsSettings]
  GameInstanceClass=/Script/Nyx.NyxGameInstance
  GlobalDefaultGameMode=/Script/Nyx.NyxGameMode
  ```
- Dynamic delegate callbacks (used by `AddDynamic`) require `UFUNCTION()` on the target method. Missing this causes a runtime `ensureMsgf` crash at `DelegateSignatureImpl.inl:1144`.
- Live Coding blocks external builds. Close the editor before building, or use Ctrl+Alt+F11 for hot-reload.

---

## Spike 4: EOS Authentication → SpacetimeDB Token Flow ✅

**Goal:** Authenticate via EOS, obtain an OIDC-compliant JWT, and pass it to SpacetimeDB's `WithToken()`.

**Duration:** 2–3 days (actual: ~1 day)

**Status:** COMPLETE — Full two-phase auth flow implemented and end-to-end verified in editor

### Tasks

1. **Set Up EOS Developer Portal** ✅
   - Created Product, Sandbox, Deployment, Application in Epic Developer Portal
   - Obtained all credentials: `ProductId`, `SandboxId`, `DeploymentId`, `ClientId`, `ClientSecret`
   - Generated 64-char hex encryption key via PowerShell
   - Configured Client Policy allowing "Epic Games" login type

2. **Enable EOS Plugins in UE5** ✅
   - Plugins `OnlineServices`, `OnlineServicesEOS`, `OnlineServicesEOSGS` already enabled in `Nyx.uproject`
   - Configured `DefaultEngine.ini` with full EOS section:
     ```ini
     [OnlineServices]
     DefaultServices=Epic

     [OnlineServices.EOS]
     ProductId=<configured>
     SandboxId=<configured>
     DeploymentId=<configured>
     ClientId=<configured>
     ClientSecret=<configured>
     ClientEncryptionKey=<64-char hex>
     ```
   - Added `OnlineServicesInterface` and `CoreOnline` to `Nyx.Build.cs` PrivateDependencyModuleNames

3. **Implement EOS Login** ✅
   - Used UE5.7 `UE::Online` API (NOT the legacy `IOnlineSubsystem`):
     ```cpp
     TSharedPtr<IOnlineServices> Services = GetServices(EOnlineServices::Epic);
     IAuthPtr Auth = Services->GetAuthInterface();
     Auth->Login(Params).OnComplete(...);
     ```
   - **Critical includes needed** (not just `Online/Auth.h`):
     ```cpp
     #include "Online/Auth.h"
     #include "Online/OnlineAsyncOpHandle.h"  // TOnlineAsyncOpHandle
     #include "Online/OnlineResult.h"         // TOnlineResult
     #include "Online/OnlineError.h"          // FOnlineError
     ```
   - `FAccountId` has NO `.ToString()` method — use free function: `UE::Online::ToLogString(AccountId)`
   - Supports multiple login types via `LoginCredentialsType::` FName constants:
     - `AccountPortal` — opens browser for Epic Account login
     - `Developer` — DevAuth tool at localhost:6300 (best for editor testing)
     - `PersistentAuth` — uses cached refresh token
   - Display name extracted from: `AccountInfo->Attributes.Find(AccountAttributeData::DisplayName)->GetString()`

4. **Token Flow: Decided on Anonymous + Reducer Auth (Option B)** ✅
   - `WithToken()` is SpacetimeDB's own reconnection token, NOT for external OIDC JWTs
   - Two-phase auth flow implemented:
     1. **Phase 1:** EOS Login → obtain AccountId, display name, optional id_token
     2. **Phase 2:** Anonymous SpacetimeDB connect → call `authenticate_with_eos` reducer
   - `QueryExternalAuthToken` attempted for JWT — may fail gracefully (not fatal)
   - SpacetimeDB connection state change fires via `OnConnectionStateChangedBP` delegate

5. **Server-Side Authentication Reducer** ✅
   - Added `PlayerAccount` table to Rust module:
     ```rust
     #[table(name = player_account, public)]
     pub struct PlayerAccount {
         #[primary_key]
         identity: Identity,
         eos_product_user_id: String,
         display_name: String,
         platform: String,
         created_at: Timestamp,
         last_login: Timestamp,
     }
     ```
   - Added `authenticate_with_eos` reducer:
     - Links `ctx.sender` (SpacetimeDB Identity) to EOS ProductUserId
     - Creates new record on first auth, updates `last_login` on subsequent
   - Published module to local SpacetimeDB, regenerated Unreal bindings

6. **Client Wiring** ✅
   - `NyxAuthSubsystem` rewritten with full real EOS login implementation
   - `NyxNetworkSubsystem` updated:
     - Exposes `GetSpacetimeDBConnection()` and `IsMockConnection()` accessors
     - Subscribes to both `player` and `player_account` tables
     - Removed test `create_player` call from `HandleSubscriptionApplied`
   - Auth subsystem calls `Conn->Reducers->AuthenticateWithEos(...)` directly
   - Reducer callback via `OnAuthenticateWithEos` delegate checks `Status.IsCommitted()`
   - Both mock and real SpacetimeDB paths work (mock uses interface, real uses generated reducers)

### Deliverable
- [x] EOS login compiles and runs in UE5 editor (Development configuration)
- [x] EOS display name and AccountId obtained successfully
- [x] Two-phase auth flow: EOS → anonymous SpacetimeDB → reducer auth
- [x] `authenticate_with_eos` reducer links SpacetimeDB Identity to EOS PUID
- [x] `player_account` table created with identity ↔ PUID mapping
- [x] Full build passes clean (NyxEditor Win64 Development)
- [x] End-to-end tested in editor — `player_account` row confirmed in DB

### Key Findings

| Question | Answer |
|----------|--------|
| Does `WithToken()` accept EOS JWTs? | **No.** It's SpacetimeDB's own reconnection token only. |
| Auth flow pattern? | **Anonymous connect + `authenticate_with_eos` reducer** (Option B from Fallback Plan) |
| What EOS login types work in editor? | `Developer` (DevAuth tool on localhost:6300) and `AccountPortal` (browser popup) |
| UE5 Online Services API? | Use `UE::Online::GetServices(EOnlineServices::Epic)` — NOT legacy `IOnlineSubsystem` |
| FAccountId to string? | Free function `UE::Online::ToLogString(id)` — no `.ToString()` member |
| include gotcha? | `Online/Auth.h` only forward-declares `TOnlineAsyncOpHandle`, `TOnlineResult`, `FOnlineError` — must include their headers separately |
| Dynamic delegate gotcha? | `AddDynamic()` returns void — cannot capture FDelegateHandle from it |
| AddDynamic lifecycle? | Must call `AddDynamic()` once (e.g. in `Init()`), NOT on every action — duplicate bindings cause `ensureMsgf` crash |

### End-to-End Verification (AccountPortal Login)

Tested via `Nyx.StartGame` console command in editor PIE session:

1. EOS AccountPortal login triggers browser popup → user "Jota 2RZ" authenticated
2. `QueryExternalAuthToken` returns 1069-byte JWT
3. Anonymous SpacetimeDB WebSocket connects to `ws://127.0.0.1:3000`
4. `authenticate_with_eos` reducer called → status `Committed`
5. Auth state machine transitions: `NotAuthenticated → AuthenticatingEOS → EOSAuthenticated → ConnectingSpacetimeDB → FullyAuthenticated`

**Database confirmation:**
```sql
spacetime sql nyx "SELECT * FROM player_account" --server local
-- identity=0xc200d12d..., eos_product_user_id="Epic:1 (EAS=[...] EOS=[...])",
-- display_name="Jota 2RZ", platform="Windows", created_at=2026-02-27T04:15:15
```

### Runtime Fix: AddDynamic Ensure Crash

`NyxGameInstance::StartGame()` originally called `AddDynamic()` for `OnLoginCompleteBP` on every invocation.
Duplicate dynamic delegate bindings cause a runtime `ensureMsgf` crash.

**Fix:** Moved `AddDynamic()` to `Init()` (runs once). Added `AuthSub->Logout()` call before re-login
if already authenticated, so `StartGame` is safely re-entrant.

### Known Issues / TODO

- **EOS Product User ID format:** `eos_product_user_id` currently stores the verbose `UE::Online::ToLogString()` output
  (e.g. `"Epic:1 (EAS=[80686cc6...] EOS=[00028b63...])"`) rather than a clean PUID string.
  This is non-blocking for the spike but should be cleaned up before production — extract just the raw
  EOS Product User ID portion (the `EOS=[...]` value) or use a different API to get the bare ID.
- **Legacy config warning:** Engine logs `"Using legacy config from OnlineServices.EOS, use EOSShared named config instead"`.
  Non-blocking — migrate to named config format when convenient.

### Files Modified
- `Config/DefaultEngine.ini` — EOS config sections
- `Source/Nyx/Nyx.Build.cs` — Added OnlineServicesInterface, CoreOnline
- `Source/Nyx/Online/NyxAuthSubsystem.h` — Full EOS auth API integration
- `Source/Nyx/Online/NyxAuthSubsystem.cpp` — Two-phase login implementation
- `Source/Nyx/Core/NyxNetworkSubsystem.h` — GetSpacetimeDBConnection(), IsMockConnection()
- `Source/Nyx/Core/NyxNetworkSubsystem.cpp` — Subscribe player_account, remove test reducer call
- `server/nyx-server/spacetimedb/src/lib.rs` — PlayerAccount table, authenticate_with_eos reducer
- Generated bindings: `AuthenticateWithEos.g.h`, `PlayerAccountTable.g.h`, updated `SpacetimeDBClient.g.h`

---

## Spike 5: Spatial Interest Management at Scale ✅

**Goal:** Determine how to partition the game world so clients only receive data about nearby entities.

**Duration:** 2–3 days (actual: ~half day)

**Status:** COMPLETE — Chunk-based spatial subscriptions working with 10K entities

### Design

World is divided into **chunks** of 10,000 UE units (100 meters) each. Every entity (player or world object) has `chunk_x`/`chunk_y` columns derived from position. The client uses two independent subscriptions:

1. **Global subscription** — `SELECT * FROM player_account` (always active, no spatial filter)
2. **Spatial subscription** — `SELECT * FROM player WHERE chunk_x/chunk_y BETWEEN ...` and `SELECT * FROM world_entity WHERE ...` (re-subscribes when player changes chunk)

### Implementation

#### Server Changes
- Added `chunk_x: i32, chunk_y: i32` to `Player` table
- Created `WorldEntity` table with `id` (auto_inc), `entity_type`, position, chunk coords, `data`
- `move_player` reducer recomputes chunk coords: `pos_to_chunk(pos) = (pos / CHUNK_SIZE).floor() as i32`
- Added `seed_entities(count)` and `clear_entities()` reducers for stress testing

#### Client Changes
- `NyxNetworkSubsystem` uses `USubscriptionHandle*` for each subscription
- `HandleSpacetimeDBConnect` creates two independent subscriptions (global + spatial)
- `UpdateSpatialSubscription(FVector)` detects chunk change → `Unsubscribe()` old → `SubscriptionBuilder->Subscribe()` new
- `HandleSpatialSubscriptionApplied` logs cache contents via `UPlayerTable::Count()` / `UWorldEntityTable::Count()`
- New console commands: `Nyx.Seed <count>`, `Nyx.ClearEntities`, `Nyx.Move <x> <y> <z>`

### Test Results

#### Subscription Switching (1,000 entities, ~32×32 chunk grid)
| Test | Result |
|------|--------|
| Origin (0,0) → chunk (5,5) | Sub switch applied immediately, 25 entities in cache |
| Chunk (5,5) → origin (0,0) | Sub switch applied immediately, 25 entities in cache |
| Same-chunk move (0 → 1 within chunk 0) | No subscription change (correct optimization) |

#### Stress Test (10,000 entities, ~101×101 chunk grid, chunks -50..+50)
| Test | Entities in Cache | Notes |
|------|-------------------|-------|
| Chunk (0,0), radius=2, 5×5 area | **25** of 10,000 | Correct — 1 entity per chunk in seed |
| Chunk (5,5), radius=2, 5×5 area | **25** of 10,000 | Instant switch from distant position |
| Chunk (-5,-5), radius=2, 5×5 area | **25** of 10,000 | Works in negative chunk space |
| Subscription switch latency | **< 1 frame** | No measurable delay in logs |
| Audio buffer underrun during `seed_entities(10000)` | One-time stall | Heavy write on server side |

### Deliverable
- [x] Chunk-based spatial subscription working (5×5 chunk area per player)
- [x] Entity count verified: only nearby entities received (25 of 10,000)
- [x] Subscription switching: instant, no visible latency on localhost
- [x] WHERE clauses in `Subscribe()` confirmed working (not parameterized — build SQL with `FString::Printf`)
- [x] `Unsubscribe()` + re-`Subscribe()` pattern validated for chunk transitions
- [ ] Multi-client stress test (100 clients) — deferred to production testing
- [ ] SpacetimeDB CPU/memory metrics — not instrumented yet

### Key Findings

| Question | Answer |
|----------|--------|
| Can subscriptions use WHERE clauses? | **Yes.** `SELECT * FROM table WHERE chunk_x >= N AND chunk_x <= M` works perfectly. |
| Parameterized queries? | **No.** Build SQL strings with `FString::Printf`. Not parameterized but functionally identical. |
| Max active subscriptions per client? | At least 2 independent subscriptions tested (global + spatial). Limit not reached. |
| Subscription switch latency? | **Sub-frame on localhost.** Unsubscribe + subscribe completes before next `HandleSpatialSubscriptionApplied` callback fires. |
| Spatial filtering effective? | **Yes.** 25 of 10,000 entities received — exactly the 5×5 chunk area. |
| SpacetimeDB SQL limitations? | No `ORDER BY`, `LIMIT`, `COUNT(*)`, or column aliases. `WHERE` with `>=`/`<=`/`AND` works fine. |
| Entity distribution? | `seed_entities` distributes 1 entity per chunk across a sqrt(N)×sqrt(N) grid. |

### Architecture Notes

```
┌─────────────────────────────────────────────────────────────┐
│ NyxNetworkSubsystem                                         │
│                                                             │
│  GlobalSubscriptionHandle ──► SELECT * FROM player_account  │
│  (always active)                                            │
│                                                             │
│  SpatialSubscriptionHandle ──► SELECT * FROM player         │
│                                  WHERE chunk_x/y BETWEEN... │
│                              ──► SELECT * FROM world_entity │
│                                  WHERE chunk_x/y BETWEEN... │
│  (re-subscribes on chunk change)                            │
│                                                             │
│  UpdateSpatialSubscription(PlayerPos)                       │
│    → chunk = floor(pos / 10000)                             │
│    → if chunk changed: Unsubscribe() → new Subscribe()     │
│                                                             │
│  Constants: ChunkSize=10000cm (100m), Radius=2 (5×5 area)  │
└─────────────────────────────────────────────────────────────┘
```

### Files Modified
- `server/nyx-server/spacetimedb/src/lib.rs` — Added chunk_x/chunk_y to Player, WorldEntity table, seed/clear reducers
- `Source/Nyx/Core/NyxNetworkSubsystem.h` — USubscriptionHandle* for global/spatial, SubscribeToSpatialArea()
- `Source/Nyx/Core/NyxNetworkSubsystem.cpp` — Split subscriptions, spatial sub switching with Unsubscribe/Subscribe
- `Source/Nyx/Core/NyxGameInstance.cpp` — Added Nyx.Seed, Nyx.ClearEntities, Nyx.Move console commands
- Generated bindings: WorldEntityTable, SeedEntities, ClearEntities reducers

---

## Spike 6: Client-Side Prediction & Reconciliation

**Goal:** Build smooth movement without UE5's dedicated server replication.

**Duration:** 3–5 days

### Tasks

1. **Design the Prediction Model**
   - Client applies input immediately (prediction)
   - Client sends `move_player` reducer with position + input sequence number
   - Server validates and updates row
   - Client receives `OnUpdate` with server-authoritative position
   - Client reconciles: if server position ≠ predicted, snap/interpolate to server state

2. **Implement in UE5**
   - Custom `UNyxMovementComponent` (NOT `UCharacterMovementComponent`)
   - Maintains a buffer of predicted moves
   - On server update: compare, discard confirmed moves, replay unconfirmed

3. **Test Edge Cases**
   - High latency (simulate 200ms+ round-trip)
   - Packet loss (WebSocket drops — does the SDK reconnect?)
   - Rapid direction changes
   - Collision with server-authoritative obstacles

4. **Other Players: Interpolation**
   - For remote players, interpolate between received positions
   - Buffer 2–3 updates and lerp between them
   - Handle gaps (no update for 500ms+) gracefully

### Deliverable
- [ ] Local player moves smoothly with prediction
- [ ] Server reconciliation works without visible jitter
- [ ] Remote players interpolate smoothly
- [ ] System handles 100ms–300ms latency gracefully
- [ ] Document the prediction buffer size and interpolation settings

---

## Spike 7: SpacetimeDB WASM Module Performance Limits

**Goal:** Determine what game logic can feasibly run inside SpacetimeDB WASM modules.

**Duration:** 1–2 days

### Tasks

1. **Benchmark Reducer Throughput**
   - How many reducer calls per second can a single module handle?
   - Test with: simple (update 1 row), medium (read 10 rows + update 1), complex (iterate 1000 rows)

2. **Benchmark Scheduled Reducers**
   - SpacetimeDB supports `#[spacetimedb::reducer(repeat = "100ms")]` for ticking
   - How many entities can a 100ms tick update?
   - Is 100ms fast enough for game AI? Do we need 50ms? 16ms?

3. **Memory Limits**
   - How much memory does a WASM module get?
   - Can we load navigation data (simplified) for pathfinding?
   - Can we store spatial indexes (quadtree/octree) in module memory?

4. **Identify What Must Stay Outside WASM**
   - UE5 navmesh generation (too complex for WASM)
   - Physics simulation (too CPU-intensive)
   - Audio processing
   - Asset streaming

### Deliverable
- [ ] Reducer throughput numbers (calls/sec)
- [ ] Tick-based scheduled reducer capacity (entities/tick)
- [ ] Memory limits documented
- [ ] Clear list of what runs in WASM vs. what stays client-side

---

## Decision Points After Spikes

After completing these spikes, we'll have clarity on:

| Decision | Options | Depends On |
|----------|---------|------------|
| **Auth flow** | A) EOS JWT → WithToken direct, B) Anonymous + reducer auth | Spike 4 ✅ → **Option B** |
| **Spatial partitioning** | A) Single DB + spatial subs, B) Sharded DBs | Spike 5 ✅ → **Option A** (chunk-based WHERE queries) |
| **Movement model** | A) Full server-auth, B) Client-auth with validation | Spike 3, 6 |
| **AI system** | A) WASM module, B) Sidecar UE5 process, C) Hybrid | Spike 7 |
| **Module language** | A) Rust (performance), B) C# (familiarity) | Spike 2, 7 |

---

## Timeline

| Week | Spikes | Status |
|------|--------|--------|
| Week 1 | Spike 1 (Plugin) ✅, Spike 2 (Module) ✅, Spike 3 (Round-Trip) ✅ | **DONE** — completed in ~2 days |
| Week 1–2 | Spike 4 (EOS Auth) ✅ | **DONE** — anonymous connect + reducer auth, end-to-end verified |
| Week 2 | Spike 5 (Spatial) ✅ | **DONE** — chunk-based subscriptions working with 10K entities |
| Week 2 | Spike 6 (Prediction) | TODO |
| Week 2–3 | Spike 7 (WASM Perf) | TODO |

**Total estimated time: 2–3 weeks**

---

## References

- SpacetimeDB 2.0 Unreal Reference: https://spacetimedb.com/docs/2.0.0-rc1/clients/unreal
- SpacetimeDB Client Codegen: https://spacetimedb.com/docs/2.0.0-rc1/clients/codegen
- SpacetimeDB GitHub: https://github.com/clockworklabs/SpacetimeDB
- EOS Online Services Config (UE5): https://dev.epicgames.com/documentation/en-us/unreal-engine/enable-and-configure-online-services-eos-in-unreal-engine
- EOS Developer Portal: https://dev.epicgames.com/portal/
- UE5.7 Documentation: https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-7-documentation
- BitCraft (reference game built with SpacetimeDB): https://www.bitcraftonline.com/
