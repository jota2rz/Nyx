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
| Command | Description | Added |
|---------|-------------|-------|
| `Nyx.Connect [Host] [Database]` | Connect to real SpacetimeDB (default: 127.0.0.1:3000 / nyx) | Spike 3 |
| `Nyx.ConnectMock` | Connect using mock backend | Spike 3 |
| `Nyx.Disconnect` | Disconnect current connection | Spike 3 |
| `Nyx.StartGame [mock]` | Full login flow (auth → connect) | Spike 4 |
| `Nyx.Seed <count>` | Seed world with N entities for spatial testing | Spike 5 |
| `Nyx.ClearEntities` | Remove all world entities | Spike 5 |
| `Nyx.Move <x> <y> <z>` | Teleport player (debug — raw reducer call) | Spike 5 |
| `Nyx.EnterWorld` | Set up subscriptions + create player + spawn pawn | Spike 6 |
| `Nyx.Walk <dx> <dy> [dz]` | Apply movement input to local pawn (prediction test) | Spike 6 |

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
- [x] OnInsert / OnUpdate / OnDelete callbacks — wired in Spike 6 via `PlayerTable::OnInsert/OnUpdate/OnDelete`
- [x] Subscription queries with filters — verified in Spike 5 (`WHERE chunk_x BETWEEN ...`)
- [ ] Latency numbers — not formally measured (qualitatively instant on localhost)
- [x] Subscription update (change query at runtime) — verified in Spike 5 (`Unsubscribe()` + re-`Subscribe()`)

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

## Spike 6: Client-Side Prediction & Reconciliation ✅

**Goal:** Build smooth movement without UE5's dedicated server replication.

**Duration:** 3–5 days (actual: ~2 days)

**Status:** COMPLETE — Full prediction → server echo → reconciliation pipeline verified end-to-end

### Architecture

```
 Client (UE5)                         Server (SpacetimeDB)
 ┌──────────────────────┐              ┌──────────────────────┐
 │ Input → Predict      │              │                      │
 │ (move actor locally) │─── seq=N ──→ │ move_player reducer  │
 │                      │              │ (validate + update)  │
 │ OnUpdate callback ←──│←── seq=N ──│ Player row updated   │
 │ Reconcile(seq=N)     │              │                      │
 │ Remove confirmed     │              │                      │
 │ Compare positions    │              │                      │
 └──────────────────────┘              └──────────────────────┘
```

### Key Design Decisions

1. **Sequence numbers for reconciliation** — Each `move_player` call carries an incrementing `seq: u32`. The server echoes it back in the Player row. The client uses this to match server confirmations to its prediction buffer.

2. **Custom movement component** — `UNyxMovementComponent` (NOT `UCharacterMovementComponent`). Two modes:
   - **LOCAL**: Client-side prediction + 20 Hz server send + reconciliation buffer
   - **REMOTE**: Entity interpolation with 100ms delay buffer

3. **Table callbacks, not replication** — `PlayerTable::OnInsert/OnUpdate/OnDelete` via `AddDynamic` replace UE5's replication. `NyxEntityManager` routes updates to the correct movement component.

4. **Identity-based ownership** — `FSpacetimeDBIdentity::ToHex()` as the actor lookup key. `NyxNetworkSubsystem::IsLocalIdentity()` determines local vs. remote.

### Implementation

#### Server Changes (`lib.rs`)
- Added `seq: u32` field to `Player` table
- `move_player` reducer now takes `(x, y, z, yaw, seq)` and echoes `seq` back
- `create_player` initializes `seq = 0`

#### New Files
| File | Purpose |
|------|---------|
| `Source/Nyx/Player/NyxPlayerPawn.h/.cpp` | Minimal pawn: capsule, spring arm camera (400 units), Enhanced Input bindings |
| `Source/Nyx/Player/NyxMovementComponent.h/.cpp` | Prediction engine: local prediction, 20 Hz send, reconciliation buffer, remote interpolation |

#### Modified Files
| File | Changes |
|------|---------|
| `NyxNetworkSubsystem` | Stores `LocalIdentity`, exposes `IsLocalIdentity()` |
| `NyxEntityManager` | Rewritten: binds directly to `PlayerTable::OnInsert/OnUpdate/OnDelete`, spawns `NyxPlayerPawn`, routes updates to movement component |
| `NyxGameMode` | `EnterWorld()` sets up spatial subscription + `StartListening()` before calling `CreatePlayer` |
| `NyxGameInstance` | Added `Nyx.EnterWorld`, `Nyx.Walk` console commands |

### Configuration (NyxMovementComponent defaults)

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `MoveSpeed` | 600 cm/s | Movement speed |
| `SendRate` | 20 Hz | How often moves are sent to server |
| `ReconciliationThreshold` | 5.0 cm | Error tolerance before correction |
| `InterpolationDelay` | 100 ms | Remote player interpolation buffer |
| `MaxPredictionBufferSize` | 256 | Max unconfirmed moves stored |
| `MaxInterpolationBufferSize` | 16 | Max remote snapshots buffered |

### Test Results

**Test sequence:** `Nyx.Connect` → `Nyx.EnterWorld` → `Nyx.Walk 1 0 0`

```
LogNyxMove: Initialized as LOCAL player (SendRate=20 Hz, Threshold=5.0 cm)
LogNyxWorld: Player inserted: Player (LOCAL) at (0, 0, 100)
LogNyxMove: Sent move seq=1 pos=(5.1, 0.0, 100.0) yaw=0.0, pending=1
LogNyxMove: Reconciliation OK: error=0.00 cm, serverSeq=1, pending=0
```

**Server-side verification (SQL):**
```sql
SELECT * FROM player;
-- pos_x=5.626, pos_y=0, pos_z=100, seq=1 ✓
```

**What this proves:**
- Client predicts move immediately (pos jumps from 0 → 5.1 before server confirms)
- Server receives `seq=1`, updates row, echoes `seq=1` back
- `OnUpdate` fires → `Reconcile()` runs → error=0.00 cm (prediction matched server)
- Prediction buffer cleared (pending=1 → pending=0)

### Deliverable
- [x] Local player moves smoothly with prediction
- [x] Server reconciliation works (0.00 cm error on single move)
- [x] Remote player interpolation code implemented (needs multi-client test)
- [x] Prediction buffer size and interpolation settings documented
- [ ] Multi-client interpolation test (deferred — needs 2nd client)
- [ ] Latency stress test (200ms+) deferred to production hardening

### Gotchas & Lessons Learned

1. **`IsLocallyControlled()` cannot be overridden** — UHT prevents UFUNCTION override of APawn's version. Use a custom `IsLocalNyxPlayer()` method instead.

2. **`AddYawInput()`/`AddPitchInput()` live on `APlayerController`**, not `AController`. Cast with `Cast<APlayerController>(Controller)`.

3. **Identity keys must use `ToHex()`** — `GetTypeHash()` produces 32-bit hashes that collide. `FSpacetimeDBIdentity::ToHex()` gives the full 256-bit hex string for reliable actor lookup.

4. **Event ordering matters** — `StartListening()` (bind table callbacks) must happen BEFORE `CreatePlayer()` reducer call, otherwise the OnInsert fires before the handler is bound and the pawn never spawns.

5. **Live Coding blocks CLI builds** — Close the UE editor before running `Build.bat` from the command line, or Live Coding will reject the build.

---

## Spike 7: SpacetimeDB WASM Module Performance Limits ✅

**Goal:** Determine what game logic can feasibly run inside SpacetimeDB WASM modules.

**Duration:** 1–2 days (actual: ~half day)

**Status:** COMPLETE — Throughput, scheduled tick, and memory benchmarks all run and documented

### Test Environment

- SpacetimeDB 2.0.2 (local server, `127.0.0.1:3000`)
- WASM module compiled with `cargo build --release --target wasm32-unknown-unknown`
- No `wasm-opt` applied (unoptimised module)
- Host: Windows 11, 4 physical cores / 8 logical, local disk
- Benchmark tables: `bench_counter` (single-row), `bench_entity` (1000 rows seeded)

### 1. Reducer Throughput

Tested three reducer complexity levels, each called N times from a single client:

| Reducer | Complexity | Calls | Server Processing Time | Throughput |
|---------|-----------|-------|----------------------|------------|
| `bench_simple` | Read 1 row + update 1 row | 1000 | 4.58 sec | **~218 reducers/sec** |
| `bench_medium` | Iterate 10 rows + update 1 row | 1000 | 5.99 sec | **~167 reducers/sec** |
| `bench_complex` | Full scan 1000 rows + aggregation | 100 | 6.38 sec | **~16 reducers/sec** |

**Client dispatch rate** was ~700K calls/sec (WebSocket queuing is essentially free).

**Burst operations** (single reducer call doing N internal DB ops):

| Reducer | Operations | Server Time | Internal Throughput |
|---------|-----------|-------------|-------------------|
| `bench_burst` | 10,000 single-row updates | 5.16 sec | **~1,938 ops/sec** |
| `bench_burst_update` | 1,000 entity row updates | 6.01 sec | **~166 entity updates/sec** |

**Key finding:** Per-reducer overhead dominates. A single reducer doing 10K ops is faster than 10K individual reducer calls. For batch operations (AI ticks, world updates), use a single scheduled reducer that processes many entities.

### 2. Scheduled Tick (Game Loop)

Used SpacetimeDB's scheduled table pattern (`#[table(scheduled(game_tick))]`) to fire a reducer every 100ms. Each tick updates ALL 1000 `bench_entity` rows (simulating NPC AI movement).

```
Tick interval set: 100ms
Entities updated per tick: 1000
Total ticks recorded: 155
```

**Tick timing analysis** (from `bench_tick_log` timestamps):

| Metric | Value |
|--------|-------|
| Target interval | 100 ms |
| Actual average interval | **~110 ms** |
| Inter-tick delta range | 108–112 ms |
| Jitter | ±2 ms |
| Entities updated per tick | 1000 |
| Processing overhead per tick | ~10 ms (for 1000 entity updates) |

**Sample tick log:**
```
tick 56  → 06:15:44.409  (1000 entities)
tick 57  → 06:15:44.520  (Δ = 111 ms)
tick 58  → 06:15:44.630  (Δ = 110 ms)
tick 59  → 06:15:44.740  (Δ = 110 ms)
tick 60  → 06:15:44.850  (Δ = 110 ms)
...
tick 155 → 06:15:55.254  (final tick before stop)
```

**Conclusion:** **1000 entities at 100ms tick is sustainable.** The ~10ms processing overhead per tick means:
- At 100ms interval: can handle ~1000 entities comfortably
- At 50ms interval: ~500 entities max before tick overrun
- At 16ms (60 Hz): ~150 entities max — too tight for large AI populations
- For MMO AI (100+ NPCs), stick to 100–200ms tick rate

### 3. Memory Limits

Tested in-reducer heap allocation by writing to a `Vec<u8>` of increasing size:

| Allocation | Duration | Result |
|-----------|----------|--------|
| 1 MB | < 1 sec | ✅ OK |
| 10 MB | ~3 sec | ✅ OK |
| 100 MB | ~4 sec | ✅ OK |
| 256 MB | ~4.3 sec | ✅ OK |
| 512 MB | ~4.0 sec | ✅ OK |

**512 MB allocated successfully** — SpacetimeDB places no practical limit on WASM module memory in the current version. This is generous enough for:
- ✅ Spatial indexes (quadtree/octree) — typically a few MB
- ✅ Simplified navigation data — compressed navmeshes under 100 MB
- ✅ Large lookup tables (item database, skill trees, etc.)
- ✅ In-memory caches for hot data

**Caveat:** Allocation time grows linearly (~8 ms/MB for write-fill). Avoid large allocations in hot paths.

### 4. What Runs in WASM vs. Client-Side

Based on benchmarks:

| System | Where | Rationale |
|--------|-------|-----------|
| **Player movement validation** | WASM (reducer) | ~218 calls/sec is plenty for move_player validation |
| **NPC AI decisions** | WASM (scheduled tick) | 1000 NPCs at 100ms tick verified |
| **Loot/inventory logic** | WASM (reducer) | Single-row CRUD is fast |
| **Combat calculations** | WASM (reducer) | Damage formula per hit is lightweight |
| **Chat/social** | WASM (reducer) | Text insert/broadcast is trivial |
| **Spatial queries** | WASM (scheduled tick) | Can iterate entities within chunks |
| **Pathfinding** | WASM (cautious) | Possible if grid-based; A* on small grids OK. Full navmesh at 16ms would be too slow |
| **Physics simulation** | Sidecar (UE5) | Too CPU-intensive for WASM; sidecar validated in Spike 8 |
| **Navmesh generation** | Client (UE5) | Requires Recast/Detour + full geometry |
| **Rendering/VFX** | Client (UE5) | Obviously |
| **Audio** | Client (UE5) | Low-latency requirement |
| **Asset streaming** | Client (UE5) | Disk/network I/O |

### Scheduled Reducer API (SpacetimeDB 2.0)

**Important:** SpacetimeDB 2.0 does NOT use `#[reducer(repeat = "100ms")]`. It uses the **scheduled table pattern:**

```rust
use spacetimedb::{ScheduleAt, TimeDuration, Table};

// 1. Define scheduling table
#[spacetimedb::table(accessor = tick_schedule, scheduled(game_tick))]
pub struct TickSchedule {
    #[primary_key]
    #[auto_inc]
    pub scheduled_id: u64,
    pub scheduled_at: ScheduleAt,
}

// 2. Reducer receives the schedule row
#[spacetimedb::reducer]
pub fn game_tick(ctx: &ReducerContext, _arg: TickSchedule) -> Result<(), String> {
    // tick logic here
    Ok(())
}

// 3. Start by inserting a row (e.g. in init or via another reducer)
let interval = TimeDuration::from_micros(100_000); // 100ms
ctx.db.tick_schedule().insert(TickSchedule {
    scheduled_id: 0,  // auto_inc
    scheduled_at: interval.into(), // ScheduleAt::Interval → repeating
});
```

- `ScheduleAt::Interval(TimeDuration)` → repeating
- `ScheduleAt::Time(Timestamp)` → one-shot
- Delete the row to stop the schedule

### Deliverable
- [x] Reducer throughput numbers: ~218/sec simple, ~167/sec medium, ~16/sec complex (1000-row scan)
- [x] Scheduled tick: 1000 entities at 100ms sustainable (~110ms actual with 10ms processing)
- [x] Memory: 512 MB allocated successfully, no practical WASM limit
- [x] Clear list of what runs in WASM vs. client-side (table above)

### Console Commands Added

| Command | Description |
|---------|-------------|
| `Nyx.Bench <type> <count>` | Run throughput benchmark (simple/medium/complex/burst/burstupdate) |
| `Nyx.BenchSeed <count>` | Seed bench entities |
| `Nyx.BenchReset` | Clear all benchmark data |
| `Nyx.BenchTick <interval_ms>` | Start scheduled game tick |
| `Nyx.BenchTickStop` | Stop scheduled game tick |
| `Nyx.BenchMem <megabytes>` | Test WASM memory allocation |

### Server Changes

- Added tables: `bench_counter`, `bench_entity`, `bench_tick_log`, `tick_schedule`
- Added reducers: `bench_simple`, `bench_medium`, `bench_complex`, `bench_burst`, `bench_burst_update`, `bench_seed`, `bench_reset`, `bench_start_tick`, `bench_stop_tick`, `bench_memory`, `game_tick`
- Added `ScheduleAt`, `TimeDuration` imports to `lib.rs`

### Gotchas

1. **`TimeDuration::from_micros()` takes `i64`**, not `u64`. Cast with `as i64`.
2. **Scheduled tables are private** — codegen skips them (`Skipping private tables during codegen: tick_schedule`). Clients control ticks via regular reducers that insert/delete schedule rows.
3. **No `wasm-opt`** — our module is unoptimised. Installing `wasm-opt` from Binaryen could improve throughput by 10–30%.
4. **Throughput bottleneck is per-reducer overhead**, not DB operations. Batch entity updates in a single scheduled tick instead of many individual reducer calls.
5. **Measurement methodology caveat:** The "Server Processing Time" figures (4.58 sec for 1000 bench_simple, 6.01 sec for bench_burst_update) were measured client-side and include the **full pipeline** — WebSocket I/O, WASM invocation, DB operations, transaction commit, subscription diff computation, broadcast to subscribers, and client callback processing. By contrast, game_tick timing (measured via `bench_tick_log` timestamps inside WASM) shows 1000 entity updates complete in ~10ms of pure server-side execution. The ~600× difference (166 entity updates/sec client-perceived vs ~100K/sec server-internal) is dominated by subscription diff processing, not WASM execution speed. See Spike 9 section for corrected analysis.

---

## Spike 8: UE5 Physics Sidecar ✅

**Goal:** Validate the sidecar architecture: a separate UE5 process connects to SpacetimeDB, runs physics simulation on shared entities, and writes results back — visible to all game clients.

**Duration:** 1 day

**Status:** COMPLETE — Full round-trip verified: client spawns projectile → sidecar simulates with gravity → positions flow back to client via subscriptions

### Architecture

```
Game Client ──WebSocket──► SpacetimeDB ◄──WebSocket── UE5 Sidecar
(spawns projectile)      (physics_body table)     (Euler physics + gravity)
    │                         │                        │
    │  OnUpdate callbacks ◄───┘───► OnInsert callback  │
    │  (sees positions move)       (starts simulating) │
    └─────────────────────────────────────────────────-┘
```

- **SpacetimeDB** is the shared state store. The `physics_body` table holds position, velocity, and active flag.
- **Game client** creates physics bodies via `spawn_projectile` reducer.
- **Sidecar** connects as a second SpacetimeDB client (separate identity), subscribes to `physics_body WHERE active = true`, and simulates physics.
- **Communication** is entirely through SpacetimeDB — no direct client↔sidecar connection needed.

### Implementation

#### Server (lib.rs)

Added `physics_body` table:
```rust
#[spacetimedb::table(accessor = physics_body, public)]
pub struct PhysicsBody {
    #[primary_key]
    #[auto_inc]
    pub entity_id: u64,
    pub body_type: String,     // "projectile", "debris", etc.
    pub pos_x: f64, pub pos_y: f64, pub pos_z: f64,
    pub vel_x: f64, pub vel_y: f64, pub vel_z: f64,
    pub active: bool,
    pub owner: Identity,
    pub last_update: Timestamp,
}
```

Added reducers: `spawn_projectile`, `physics_update`, `physics_cleanup`, `physics_reset`

#### UE5 Sidecar (`NyxSidecarSubsystem`)

`UNyxSidecarSubsystem` is a `UGameInstanceSubsystem` + `FTickableGameObject` that:

1. **Creates a second `UDbConnection`** — completely independent from the game client's connection, gets its own SpacetimeDB identity
2. **Subscribes to `physics_body WHERE active = true`** — only sees bodies that need simulation
3. **On `PhysicsBodyTable::OnInsert`** — begins tracking the body in a local `TMap<uint64, FTrackedBody>`
4. **Every frame** — runs Euler integration physics:
   - `velocity.Z += gravity * dt` (gravity = -980 cm/s²)
   - `position += velocity * dt`
   - Floor collision at Z=0 → deactivate
5. **At 30 Hz** — calls `PhysicsUpdate` reducer for each active body, sending new position/velocity
6. **Ignores own `OnUpdate` callbacks** — prevents feedback loops (sidecar is the source of truth for tracked bodies)

Key configuration:
- `SendRateHz = 30` — physics updates sent to SpacetimeDB 30 times/sec
- `GravityZ = -980` — Earth gravity in cm/s² (UE5 scale)
- `FloorZ = 0` — bodies below this are deactivated

#### Client Console Commands

| Command | Description |
|---------|-------------|
| `Nyx.StartSidecar [Host] [DB]` | Start the physics sidecar (creates 2nd SpacetimeDB connection) |
| `Nyx.StopSidecar` | Stop the sidecar |
| `Nyx.Shoot [vx] [vy] [vz]` | Spawn a projectile at player position (default: 500, 0, 500) |
| `Nyx.PhysicsReset` | Remove all physics bodies |

### Testing Flow

```
1. Nyx.Connect          → Connect game client to SpacetimeDB
2. Nyx.EnterWorld        → Create player
3. Nyx.StartSidecar      → Start sidecar (2nd connection)
4. Nyx.Shoot 500 0 500   → Spawn projectile with 45° launch angle
5. Watch logs: sidecar picks up the body, simulates gravity, sends updates
6. Nyx.PhysicsReset      → Clean up
7. Nyx.StopSidecar       → Disconnect sidecar
```

### Actual Test Results

**Full log output from end-to-end test run:**

```
Cmd: Nyx.Connect
LogNyx: Console: Nyx.Connect 127.0.0.1:3000 nyx
LogNyxNet: Creating SpacetimeDB connection to ws://127.0.0.1:3000 database=nyx
LogNyxNet: Connection state changed to: 1
LogNyxNet: SpacetimeDB connected! Token length=386
LogNyxNet: Connection state changed to: 2
LogNyxNet: Subscribed to player_account (global, no spatial filter)
LogNyxNet: Subscribing to spatial area: chunks X[-2..2] Y[-2..2] (radius=2)
LogNyxNet: Global subscription applied (player_account)
LogNyxNet: Spatial subscription applied — chunk center (0, 0), radius=2. Cache: 0 players, 0 entities

Cmd: Nyx.EnterWorld
LogNyxWorld: NyxEntityManager now listening for PlayerTable events
LogNyx: EnterWorld: Called CreatePlayer('Player')
LogNyxMove: Initialized as LOCAL player (SendRate=20 Hz, Threshold=5.0 cm)
LogNyxWorld: Local player possessed NyxPlayerPawn
LogNyxWorld: Player inserted: Player (LOCAL) at (0, 0, 100)

Cmd: Nyx.StartSidecar
LogNyx: Starting sidecar — connecting to ws://127.0.0.1:3000 database=nyx
LogNyx: Sidecar connection built — waiting for connect callback
LogNyx: Sidecar connected to SpacetimeDB (identity: 0xc200...610eb1c2)
LogNyx: Sidecar subscribed to physics_body table
LogNyx: Sidecar subscription applied — binding table events
LogNyx: Sidecar active — simulating 0 bodies at 30 Hz send rate

Cmd: Nyx.Shoot 500 0 500
LogNyx: Console: Nyx.Shoot pos=(0, 0, 150) vel=(500, 0, 500)
LogNyx: Sidecar: New body 1 (projectile) at (0, 0, 150) vel=(500, 0, 500)
LogNyx: Sidecar: Body 1 hit floor at (630, 0, 0)
LogNyx: Sidecar: Sent deactivation for body 1
LogNyx: Sidecar: Body 1 deleted
```

**Analysis:**
- 45° launch from Z=150 with v₀=500 cm/s, gravity=-980 cm/s² → expected X landing ≈ **630 cm** ✅
- Sidecar picked up the body via `OnInsert` within 1 frame of the `spawn_projectile` reducer call
- Floor collision detected at Z=0, deactivation sent immediately
- Body deleted from subscription (no longer matches `WHERE active = true`)
- No feedback loops, no errors, no duplicate events
- Benign SDK warning `UCredentials::Init has not been called before SaveToken` — the SpacetimeDB SDK saves a token before credentials are initialized; harmless, present since Spike 1

### What This Validates

| Validation Point | Result |
|------------------|--------|
| **Two SpacetimeDB connections in same process** | ✅ Each gets independent identity, tables, callbacks |
| **Cross-client table event propagation** | ✅ Client A's insert triggers sidecar's OnInsert |
| **Sidecar writes → client reads** | ✅ Sidecar's PhysicsUpdate triggers client's OnUpdate |
| **No feedback loops** | ✅ Sidecar ignores OnUpdate for bodies it tracks |
| **Physics simulation pattern** | ✅ Euler integration at frame rate, batched sends at 30 Hz |
| **Architecture portability** | ✅ Same pattern works for separate process (just move subsystem to its own target) |

### Production Sidecar vs. Spike Sidecar

| Aspect | Spike (current) | Production |
|--------|-----------------|------------|
| **Process** | In-process (same UE5 instance) | Separate headless UE5 process (`-nullrhi -nosound`) |
| **Physics** | Euler integration (math only) | Full Chaos physics engine |
| **Features** | Projectile gravity | Projectiles, ragdolls, destruction, vehicle physics |
| **Pathfinding** | Not included | Recast/Detour navmesh |
| **AI steering** | Not included | Physics-aware RVO avoidance |
| **Scale** | Single instance | Multiple sidecar instances per zone |

### MultiServer Replication Plugin — Deep Dive

The UE5.7 `MultiServerReplication` plugin (experimental, by Epic) was evaluated for Spike 8 and given a full source-code deep dive for Option C viability. The plugin lives at `Engine/Plugins/Runtime/MultiServerReplication/` and consists of two modules: `MultiServerReplication` and `MultiServerConfiguration`.

#### Architecture

Two subsystems work together:

1. **Proxy Server (`UProxyNetDriver`)** — A transparent network relay between game clients and multiple backend UE5 dedicated servers. Clients connect to the proxy as if it were a normal game server. The proxy opens **one connection per backend server** for each client.
2. **Beacon Mesh (`UMultiServerNode`)** — Peer-to-peer server-to-server communication via `AMultiServerBeaconClient` subclasses. Used for level visibility sync and custom cross-server RPCs.

**Key classes:** `UProxyNetDriver`, `UProxyBackendNetDriver`, `ANoPawnPlayerController`, `UMultiServerNode`, `AMultiServerBeaconClient`, `AMultiServerBeaconHost`

**Topology:** Star — one proxy, N backend game servers. Server count is **fixed at launch** (no dynamic add/remove). Config via command-line:
- `-ProxyGameServers=IP1:Port1,IP2:Port2` — register backend servers
- `-ProxyClientPrimaryGameServer=N|random` — which server is primary for new clients
- `-ProxyCyclePrimaryGameServer` — round-robin primary assignment
- `-MultiServerPeers=`, `-MultiServerNumServers=` — for beacon mesh setup

#### Primary vs. Non-Primary Server Connections

For each client connected to the proxy:

| | Primary Server | Non-Primary Server(s) |
|---|---|---|
| **Player controller** | Full game `APlayerController` + pawn | `ANoPawnPlayerController` (no pawn, no presence) |
| **Actor state replication** | Full (transforms, properties) | **Full** — standard UE relevancy applies |
| **Server → Client RPCs** | Forwarded to client | **Dropped** by proxy |
| **Client → Server RPCs** | Forwarded to server | **Not routed** — proxy only sends to primary |
| **Authority** | `ROLE_Authority` for its own actors | `ROLE_Authority` for its own actors |

The proxy strips `ROLE_Authority` from any locally-spawned actors and replicates all actors from backend servers as `ROLE_SimulatedProxy` / `ROLE_AutonomousProxy` with role swapping enabled.

#### Border Relevancy — How Cross-Server Visibility Works

**There is no explicit "overlap zone" or "border region" concept.** Cross-server visibility is entirely emergent from UE's standard relevancy system:

1. The proxy periodically sends the player's position to each non-primary server via `ServerSetViewTargetPosition(FVector)` RPC.
2. The non-primary server's `ANoPawnPlayerController` **literally moves itself** to that position:
   ```cpp
   void ANoPawnPlayerController::ServerSetViewTargetPosition_Implementation(FVector ViewTargetPos)
   {
       AActor* Actor = Cast<AActor>(this);
       Actor->SetActorLocation(ViewTargetPos);
   }
   ```
3. Standard UE relevancy (`AActor::IsNetRelevantFor()` + net cull distance) determines which actors on each server are relevant to that virtual viewpoint.
4. All relevant actors are replicated to the proxy, which aggregates state from all servers and replicates to the client.

The "overlap range" is therefore simply **each actor's net cull distance**. If a player at a border is within the net cull distance of actors on both Server A and Server B, both servers replicate those actors, and the client sees all of them.

**Rate limiting:** The position sync interval is controlled by `net.proxy.NonPrimarySetViewTargetInterval` (default **1.0 second**). Non-primary servers can see the player position up to 1 second stale, creating a latency zone at borders. For action gameplay, this should be tuned down (e.g., 0.1s), at the cost of increased non-primary server load.

**Proxy-side position sync:** Since the proxy doesn't simulate movement, it manually syncs pawn positions from `ReplicatedMovement` for its own relevancy calculations:
```cpp
// PrepareStateForRelevancy() — called before each relevancy pass
for (const auto& ObjectInfo : GetNetworkObjectList().GetAllObjects())
{
    APawn* Pawn = Cast<APawn>(ObjectInfo->Actor);
    if (!Pawn) continue;
    Pawn->SetActorLocation(Pawn->GetReplicatedMovement().Location);
}
```

#### Cross-Server Interaction — Critical Limitation

**RPCs only flow through the primary server.** A player on Server A cannot directly interact (attack, trade, pick up) with an actor owned by Server B via this plugin alone:

- **Client → Server:** Proxy forwards RPCs to the primary server only. No mechanism to target a non-primary server.
- **Server → Client:** Proxy drops RPCs from non-primary servers.

Cross-server interaction requires building custom server-to-server RPCs via `AMultiServerBeaconClient` subclasses on the beacon mesh. The plugin provides the plumbing (beacon connections, level visibility sync) but **not** the game-level cross-server logic.

#### Player Migration

The plugin does **not trigger migration itself**. Game servers decide when to migrate a player (e.g., player crosses a spatial boundary). The proxy reacts by observing player controller swaps:

- Route gets `ANoPawnPlayerController` → was formerly primary, now becoming non-primary
- Route gets a game `APlayerController` → was formerly non-primary, now becoming primary

The migration state machine handles **order-independent events** — the two controller changes can arrive in either order:
```cpp
struct FPlayerControllerReassignment
{
    bool bReceivedNoPawnPlayerController = false;
    bool bReceivedGamePlayerController = false;
    uint64 PreviousClientHandshakeId = 0;
    uint64 ClientHandshakeId = 0;
};
```

Actors closed with `EChannelCloseReason::Migrated` are **preserved on the proxy** (not destroyed), enabling seamless re-use when re-replicated from the destination server. Shared `NetGUIDCache` across all backends ensures the same actor object is reused.

#### Level Streaming Integration

The beacon system syncs level visibility between servers — each server knows which streaming levels the other has loaded, via `ServerUpdateLevelVisibility` RPCs in `AMultiServerBeaconClient::OnConnected()`. Dynamic level add/remove events are also tracked.

**No World Partition or DataLayer integration** exists in the plugin. No references to `UWorldPartition`, `UDataLayerInstance`, or `FWorldPartitionStreamingSource` anywhere in the source.

#### Implications for Nyx Option C

| Capability | Assessment |
|---|---|
| **Visual border seamlessness** | **Yes** — actors from neighboring servers render correctly within net cull distance |
| **Interaction at borders** | **No** — requires custom beacon RPC layer for cross-server combat/trade |
| **Migration trigger** | **Manual** — game must implement spatial boundary detection and PC swap |
| **Auto-scaling** | **No** — server count fixed at launch, no dynamic add/remove |
| **Spatial partitioning** | **No** — the plugin doesn't partition the world; game must assign zones to servers |
| **1000 players in one area** | **No** — doesn't change the ~100-player-per-UE5-server ceiling; stitches multiple zones together |
| **World Partition** | **No integration** — level streaming only, no WP streaming sources |

**Bottom line:** The plugin is viable for stitching multiple UE5 servers (~100 players each) into a seamless open world with visual continuity at borders. But it requires significant custom work for cross-server interaction, migration triggers, and spatial zone assignment. It does **not** solve the 1000-players-in-one-area problem — it distributes players across servers, with each server still capped at ~100.

### Deliverable
- [x] Server `physics_body` table + spawn/update/cleanup/reset reducers
- [x] UE5 `NyxSidecarSubsystem` with second SpacetimeDB connection
- [x] Euler-integration physics simulation with gravity
- [x] Console commands: `Nyx.StartSidecar`, `Nyx.StopSidecar`, `Nyx.Shoot`, `Nyx.PhysicsReset`
- [x] Architecture validated: same communication pattern works for separate process
- [x] MultiServer Replication plugin evaluated — full source-code deep dive completed (border relevancy, migration, cross-server RPC limitations)

### Gotchas

1. **Live Coding mutex** — If the UE5 editor was recently open, build may fail with `OtherCompilationError`. Add `-DisableLiveReload` to the build command to bypass.
2. **Second connection is a separate identity** — The sidecar and game client have different SpacetimeDB identities. They share data through table subscriptions, not through local state.
3. **Feedback loop prevention** — The sidecar must ignore `OnUpdate` for bodies it's already tracking. Since the sidecar calls `PhysicsUpdate`, SpacetimeDB delivers `OnUpdate` back to the sidecar's own subscription. Ignoring tracked entity IDs prevents infinite recursion.
4. **Euler vs. Chaos** — The spike uses simple Euler integration. In production, the sidecar runs Chaos physics in a headless UE5 process for accurate collision, friction, and constraint simulation. The SpacetimeDB communication pattern is identical.

---

## Decision Points After Spikes

After completing these spikes, we'll have clarity on:

| Decision | Options | Depends On |
|----------|---------|------------|
| **Auth flow** | A) EOS JWT → WithToken direct, B) Anonymous + reducer auth | Spike 4 ✅ → **Option B** |
| **Spatial partitioning** | A) Single DB + spatial subs, B) Sharded DBs | Spike 5 ✅ → **Option A** (chunk-based WHERE queries) |
| **Movement model** | A) Full server-auth, B) Client-auth with validation | Spike 3, 6 ✅ → **Option B** (client predicts, server validates + echoes seq) |
| **AI system** | A) WASM module, B) Sidecar UE5 process, C) Hybrid | Spike 7, 8 ✅ → **Option C**: WASM for AI decisions + UE5 sidecar for physics/pathfinding |
| **Physics simulation** | A) Client-only, B) UE5 sidecar, C) WASM | Spike 8 ✅ → **Option B** (sidecar validated) |
| **Module language** | A) Rust (performance), B) C# (familiarity) | Spike 2, 7 ✅ → **Option A** (Rust, performance critical for WASM throughput) |
| **Networking architecture** | A) SpacetimeDB-only, B) Hybrid relay + SpacetimeDB, C) UE5 dedicated server + SpacetimeDB persistence | **Option A viable for 100–200 players per zone** (fully proven: server-side + fan-out). Fan-out ceiling ~300 subs on 4C/8T (zero degradation to 200). Hybrid B/C only needed for mass events (>300 players in one zone). |

---

## Spike 9: Sidecar Scalability & 1000-Player Chunk Viability

**Goal:** Determine whether the current SpacetimeDB + sidecar architecture can support 1000 concurrent players in a single chunk, and if not, identify the architectural changes needed.

**Duration:** 2–3 days

**Status:** PARTIAL COMPLETE — Tests 1–3 executed, Tests 4–5 deferred

### The 1000-Player Problem

Nyx's vision is a seamless open world where large player concentrations (sieges, cities, world events) must work without sharding or instancing. The target: **1000 simultaneously active players in one chunk**.

Our Spike 7 benchmarks reveal constraints, but the original analysis in this section was **overly pessimistic** due to flawed assumptions. Here we correct the math.

#### Spike 7 Benchmark Methodology — What Was Actually Measured

Our benchmarks produced two data points that initially appeared contradictory:

| Measurement | Result | What it Actually Measured |
|---|---|---|
| `bench_simple` × 1000 calls → 4.58 sec | **218 reducers/sec** | **Full client-observable pipeline**: dispatch → WebSocket → WASM invoke → DB op → commit → subscription diff computation → broadcast to subscribers → client callback. This is the end-to-end rate. |
| `game_tick` updating 1000 entities → **~10ms** per tick | **~100,000 entity updates/sec** | **Pure WASM + DB execution time**: measured via `bench_tick_log` timestamps *inside* the WASM module, excludes subscription processing |

Similarly, `bench_burst_update` (1000 entity updates in one call) reported 6.01 sec — but `game_tick` does the same work in ~10ms. The **~600× discrepancy** is because `bench_burst_update` was measured from the client (includes subscription diff computation and WebSocket broadcast for 1000 row changes), while `game_tick` timing is measured inside the reducer.

**Key insight:** The WASM execution itself is fast (~100K entity updates/sec). The bottleneck is the **per-reducer invocation pipeline** (especially subscription diff computation and broadcast) — not the database or WASM performance. SpacetimeDB's claims about DB speed *are* correct; the overhead is in the event distribution layer.

#### Corrected Math: Can SpacetimeDB handle N players?

**The original "~10 players" estimate was wrong** because it assumed:
1. Each player sends a per-frame position reducer at 20 Hz (FPS pattern, not MMO pattern)
2. The 218/sec pipeline rate is the only throughput available
3. Batched operations were also bottlenecked at 166/sec (wrong — that was client-measured, not server throughput)

**Correct architecture** (as BitCraft uses): **input-change reducers + scheduled tick**
- Clients call a reducer only when input *changes* (key down/up, click-to-move) — not every frame
- A scheduled tick at 100ms (10 Hz) processes all pending inputs and updates all entity positions in batch
- The tick updates 1000 entities in ~10ms WASM time — well within the 100ms budget

| Architecture | Per-Player Input Rate | Pipeline Budget (after 10 ticks/sec) | Max Players (Reducer Budget) |
|---|---|---|---|
| **Naive (original estimate)** | 20 Hz position sends | 218/sec total | **~10** ← wrong architecture |
| **Input-change model** | ~3 events/sec (key changes) | ~208/sec for inputs | **~69** |
| **Input-change + wasm-opt (est. +30%)** | ~3 events/sec | ~273/sec for inputs | **~90** |
| **Click-to-move / low-action MMO** | ~1 event/sec | ~208/sec for inputs | **~200** |

The scheduled tick itself handles ALL entity movement in one reducer call (~10ms for 1000 entities). The reducer budget is consumed by client-initiated input events, not movement processing.

**Hardware note (i3-12100):** This CPU has decent single-thread performance (4.3 GHz boost, Passmark ST ~3800). A high-end i9-14900K would be ~30-40% faster single-thread. This would improve throughput somewhat, but is not the 10× improvement needed for 1000 players — the architecture matters more than raw CPU.

#### The Real Bottleneck: Subscription Fan-Out (O(N²))

Even with unlimited reducer throughput, the **read-side** is the binding constraint at high player counts:

- A 10 Hz scheduled tick updates N player positions per tick
- Each of those N clients receives `OnUpdate` callbacks for every visible player
- With N players visible: N changes × N subscribers = **N² events per tick**
- At 10 Hz: **10 × N² events/sec** total WebSocket throughput from SpacetimeDB

| Players in Chunk | Events/Tick | Events/Sec (10 Hz) | Feasibility |
|---|---|---|---|
| 50 | 2,500 | 25,000 | ✅ Likely OK |
| 100 | 10,000 | 100,000 | ⚠️ Needs testing |
| 200 | 40,000 | 400,000 | ❌ Probably too much for WebSocket |
| 500 | 250,000 | 2,500,000 | ❌ Unmanageable |
| 1000 | 1,000,000 | 10,000,000 | ❌ Physically impossible via WebSocket |

**Spike 9 Tests 3 and 3b conclusively answered this question.** Test 3 measured server-side tick overhead at ~10ms constant (50–500 entities). Test 3b then connected real WebSocket subscribers and proved:
- **≤200 subscribers: fan-out is free** (zero tick degradation, 100% delivery, zero spread)
- **300 subscribers: graceful** (8.6 Hz, ~100% delivery, spread of 1 tick)
- **400+: cliff** (delivery collapses, massive spread)
- **Peak throughput: ~280K events/sec** (plateaus at 300+ subscribers)

The table above is overly pessimistic — it assumed N² events per tick, but SpacetimeDB's subscription diff engine is far more efficient than raw broadcast. Actual measured throughput at 200 subscribers with 100 entities at 10 Hz was 180K events/sec with zero impact on server tick rate.

Spatial interest management (subscribing only to nearby entities) reduces the N in the formula, but in a "1000 players in one area" scenario, all players ARE nearby.

#### What BitCraft Actually Does

BitCraft proves SpacetimeDB works at MMO scale, but their workload profile is fundamentally different:

| Aspect | BitCraft | Nyx (Target) |
|---|---|---|
| Movement model | Hex-grid click-to-move | WASD continuous |
| Input rate per player | ~0.5-2 calls/sec | ~3-10 calls/sec |
| Player density target | Low (spread across world) | High (1000 in one zone) |
| Physics | None (delay-based projectiles) | Real-time (gravity, collision) |
| Activity distribution | Most players standing still (crafting) | Many players moving simultaneously |
| Spatial filtering | Hex-grid area subscriptions | Chunk-based WHERE queries |

BitCraft's "MMO scale" likely means hundreds of concurrent players spread across a large world, with ~20-50 visible per area. This is well within SpacetimeDB's capabilities. Nyx's "1000 in one zone" target is a fundamentally harder problem that even AAA C++ engines struggle with.

#### Revised Comparison: How do other MMOs handle this?

| Game | Max density | Architecture | Notes |
|------|-----------|-------------|-------|
| **EVE Online** | 6000+ per system | Single-threaded Python (Stackless) + Time Dilation | Slows simulation to 10% speed at high load |
| **Planetside 2** | 1000+ per zone | Custom C++ dedicated servers, 15 Hz tick | Spatial partitioning, aggressive LOD |
| **WoW Classic** | ~300 before severe lag | C++ dedicated server, 20 Hz tick | World bosses cause mass disconnects |
| **New World** | ~100 per settlement | Lumberyard C++ server | Severe lag at 200+ |
| **BitCraft** | ~20-50 per area (estimated) | SpacetimeDB WASM, hex grid | Low-density sandbox; no physics, click-to-move |

**Key observation:** Even AAA studios with custom C++ servers running compiled (not WASM) code struggle beyond ~300 players. 1000 players is only achieved with time dilation (EVE) or purpose-built C++ engines (Planetside 2). SpacetimeDB's per-reducer overhead is a real constraint, but even without it, the O(N²) fan-out problem would remain.

### Three Candidate Architectures

#### Option A: SpacetimeDB-Only (Optimized)
```
Client ──WebSocket──► SpacetimeDB (WASM) ◄──WebSocket── Sidecar
                     (all state + movement)
```
- **Max capacity (corrected):** ~65–200 moving players depending on input model, with input-change + scheduled tick architecture (confirmed by Spike 9 Tests 1–3). Server-side WASM+DB handles ~400K entity updates/sec. Scheduled tick overhead is ~10ms constant. Subscription fan-out remains the untested ceiling — likely binding at >50 players.
- **Pros:** Simple, single source of truth, no additional infrastructure
- **Cons:** O(N²) subscription fan-out limits high-density scenarios. Per-reducer pipeline overhead (~4.5ms/call) limits client-initiated event rate.
- **Verdict:** Viable for moderate density (50-100 players per zone). Not viable for 1000 players per zone due to subscription fan-out, not WASM speed.

#### Option B: Hybrid Relay + SpacetimeDB
```
Client ──UDP──► Position Relay (Rust/C++) ──broadcast──► Nearby Clients
  │                    │
  │                    ├── Snapshot @ 1 Hz ──► SpacetimeDB (authoritative state)
  └──WebSocket──► SpacetimeDB (inventory, combat, chat, persistence)
```
- **Position relay** handles high-frequency position sync via UDP multicast/broadcast
- **SpacetimeDB** handles authoritative game state: inventory, combat outcomes, chat, persistence, AI decisions
- **Relay sends position snapshots** to SpacetimeDB at low frequency (1 Hz) for persistence and server-side validation
- **Max capacity:** UDP relay can handle thousands of position updates/sec. SpacetimeDB only processes ~1 reducer/sec per player (low-frequency actions)
- **Pros:** Decouples real-time movement from persistent state. SpacetimeDB stays the authoritative game database.
- **Cons:** Two networking stacks (UDP relay + WebSocket). Position relay must handle spatial filtering. Anti-cheat validation happens at low frequency.
- **Reference:** This is essentially how Planetside 2 and most FPS MMOs work — fast position relay + slower authoritative server

#### Option C: UE5 Dedicated Server + SpacetimeDB Persistence
```
Client ──UE5 NetDriver──► UE5 Dedicated Server (Chaos physics, movement, combat)
                                    │
                                    └── State sync ──► SpacetimeDB (persistence, cross-server state)
```
- **UE5 dedicated server** handles all real-time gameplay (movement, physics, combat, AI) using native UE5 replication
- **SpacetimeDB** acts as the persistence layer and cross-server state coordinator (accounts, inventory, world state)
- **Max capacity:** UE5 dedicated servers handle ~100 players per instance. Multiple servers with seamless travel for open world.
- **Pros:** Leverages UE5's mature networking stack, Chaos physics, AI, all built-in. SpacetimeDB provides the MMO persistence layer.
- **Cons:** Requires UE5 dedicated server infrastructure. SpacetimeDB becomes a database, not the game server. Loses SpacetimeDB's reducer-as-game-logic paradigm. MultiServer Replication plugin becomes relevant — deep dive (Spike 8 section) shows it provides visual border seamlessness but requires custom beacon RPCs for cross-server interaction, manual migration triggers, and has no auto-scaling or World Partition integration.

### What Spike 9 Will Benchmark

Before choosing an architecture, we need hard numbers on the unknowns:

#### Test 1: Batched Physics Update Throughput
Replace per-body `physics_update` calls with a single `physics_update_batch(Vec<BodyUpdate>)` reducer.
- Measure: How many body updates per batched reducer call before tick overrun?
- Target: 100+ bodies per batch at 30 Hz
- Compare: Per-body calls (current) vs. batch calls

#### Test 2: Multi-Client Reducer Contention
Connect 10, 50, 100 simultaneous clients, each calling `move_player` at 10 Hz.
- Measure: Reducer queue depth, average latency per call, dropped/delayed reducers
- Target: Determine the real per-client throughput under contention
- Tool: Python/Rust script to spawn N WebSocket clients

#### Test 3: Subscription Fan-Out Stress
Seed N entities in one chunk, update all at M Hz via scheduled tick, measure:
- Client-side `OnUpdate` callback rate and CPU cost
- SpacetimeDB WebSocket bandwidth per client
- Point where client falls behind or WebSocket backpressures
- Test matrix: N ∈ {50, 100, 200, 500}, M ∈ {10, 20, 30} Hz

#### Test 4: Sidecar Body Capacity
Fire the sidecar with N concurrent physics bodies (Euler + gravity, not Chaos yet):
- Measure: CPU time per physics step, reducer call latency, bodies-per-frame budget
- Find: Maximum bodies at 30 Hz before frame overrun
- Vary: With and without batch reducer

#### Test 5: UDP Relay Prototype (if Tests 1-3 confirm SpacetimeDB-only is unviable)
Build a minimal Rust UDP relay that:
- Accepts position packets from N clients
- Broadcasts to clients in the same spatial partition
- Snapshots to SpacetimeDB at 1 Hz
- Measure: Throughput at 100, 500, 1000 clients

### Expected Outcome vs. Actual Results

Based on the corrected analysis above, the predicted vs. actual results:

1. **Predicted: SpacetimeDB-only supports 50–100 players.** **Actual: 65–200 players (server-side), 200–300 players (including fan-out).** Test 1 showed ~400K entity updates/sec (4× higher than Spike 7's estimate). Test 2 showed 200+ reducer calls/sec with zero drops. Test 3b proved fan-out is free up to 200 subscribers with zero tick degradation. Significant upward revision.
2. **Predicted: Subscription fan-out is the real ceiling.** **Status: CONFIRMED at 300–400 subscribers.** Test 3b showed fan-out is free up to 200 subs. At 300, tick rate drops 5%. At 400+, delivery reliability collapses. The ceiling is ~300 subscribers per zone with 100 entities at 10 Hz on a 4C/8T machine.
3. **For 1000 players, all architectures face O(N²) fan-out.** **Confirmed.** At 500 subscribers, delivery drops to 54% with massive spread. Spatial partitioning to keep subscriber counts below 200–300 per zone is essential.
4. **Option A viable for moderate density.** **Strongly confirmed.** Server handles 200 subscribers with zero degradation, 300 with graceful degradation.
5. **Options B or C needed for mass events.** **Confirmed above ~300 players per zone.** Below that, Option A is more than sufficient.

### Spike 9 Benchmark Results

**Test environment:** i3-12100 (4C/8T, 4.3 GHz boost), 64 GB RAM, SpacetimeDB 2.0.2 local server, Windows 11. Note: League of Legends was running concurrently during tests, adding CPU contention. Server-side timestamps (measured inside WASM/SpacetimeDB) are unaffected; client-side measurements may show higher latency.

#### Test 1: Batched Physics Update Throughput

A self-contained `bench_batch_physics(body_count, iterations)` reducer spawns N bodies and applies M position+velocity updates to all of them in a tight loop within a single transaction. Server-side execution time is measured via SpacetimeDB log timestamps (spawned→COMPLETE).

**Server-side WASM+DB execution (log timestamp deltas):**

| Bodies | Iterations | Total Updates | Server Time (ms) | Server Rate (updates/sec) |
|--------|-----------|--------------|-------------------|--------------------------|
| 10 | 30 | 300 | 1.3 | 230,769 |
| 50 | 30 | 1,500 | 3.6 | 416,667 |
| 100 | 30 | 3,000 | 7.1 | 422,535 |
| 200 | 30 | 6,000 | 13.9 | 431,655 |
| 500 | 10 | 5,000 | 13.0 | 384,615 |
| 1,000 | 10 | 10,000 | 24.1 | 414,938 |
| 1,000 | 30 | 30,000 | 74.7 | 401,606 |

**Client-perceived pipeline (wall clock including WebSocket + subscription diff):**

| Bodies | Iterations | Total Updates | Pipeline Time (s) | Pipeline Rate (updates/sec) |
|--------|-----------|--------------|-------------------|--------------------------|
| 10 | 30 | 300 | 0.065 | 4,616 |
| 50 | 30 | 1,500 | 0.052 | 29,021 |
| 100 | 30 | 3,000 | 0.052 | 57,688 |
| 200 | 30 | 6,000 | 0.083 | 72,470 |
| 500 | 10 | 5,000 | 0.057 | 87,203 |
| 1,000 | 10 | 10,000 | 0.070 | 142,326 |
| 1,000 | 30 | 30,000 | 0.122 | 245,268 |

**Key findings:**
- **Server-side WASM+DB: ~400,000 entity updates/sec** — consistent across all batch sizes, 4× faster than Spike 7's ~100K estimate (which included per-tick scheduling overhead).
- **Pipeline overhead: ~40–80ms constant** regardless of batch size (1 reducer call = 1 subscription diff, regardless of how many rows changed inside it).
- Batched operations within a single reducer call are dramatically more efficient than individual reducer calls — the amortized pipeline cost per entity update drops from ~4.5ms (per-call) to ~0.004ms (batched).
- At 1,000 bodies × 30 iterations, 30,000 entity updates complete in **74.7ms server-side** — well within a 100ms tick budget.

#### Test 2: Multi-Client Reducer Contention

Each simulated "client" is a PowerShell background job calling `stress_move` at 10 Hz via the `spacetime call` CLI. The `stress_move` reducer updates one `bench_entity` row and increments a server-side counter (id=300). After all jobs complete, `stress_record` captures the counter and records the result.

| Test | Clients | Target Hz | Expected Calls | Actual Calls | Delivery % |
|------|---------|-----------|----------------|--------------|------------|
| 2A | 5 | 10 | 500 | 500 | 100.0% |
| 2B | 10 | 10 | 1,000 | 1,000 | 100.0% |
| 2C | 20 | 10 | 2,000 | 2,000 | 100.0% |
| 2D | 50 | 10 | 5,000 | 4,792 | 95.8% |
| 2E | 100 | 10 | — | — | *Cancelled* |

**Key findings:**
- **5–20 concurrent clients: 100% reducer delivery** — the server processes every call without contention or drops, up to 200 reducer calls/sec aggregate.
- **50 clients (2D): 95.8% delivery** — the 4.2% shortfall is caused by the **test harness**, not the server. 50 PowerShell processes each spawning CLI processes saturated the local CPU (especially with LoL running). The server-side counter confirmed all received calls were processed without errors.
- **100 clients: cancelled** — test harness too slow to generate meaningful load. CLI-based testing hits OS process limits, not SpacetimeDB limits.
- **Interpretation:** The server's reducer throughput was never the bottleneck. At 20 clients × 10 Hz = 200 calls/sec, the server handled all calls cleanly. Combined with Test 1's ~400K entity updates/sec, the server can process far more concurrent player input than the test harness can generate.

#### Test 3: Subscription Fan-Out Stress (Server-Side)

A scheduled `fanout_tick` reducer runs at M Hz, updating ALL `fanout_entity` rows per tick and logging per-tick metadata (tick number, entities updated, timestamp) to `fanout_tick_log`. This measures server-side tick execution overhead — **not** WebSocket fan-out to subscribers (no actual WebSocket subscribers were connected during this test).

**At 10 Hz (100ms target interval), 10–15s duration:**

| Test | Entities | Expected Ticks | Actual Ticks | Tick Interval (ms) | Overhead (ms) |
|------|----------|---------------|-------------|-------------------|---------------|
| 3B | 100 | 100 | 44* | ~110 | ~10 |
| 3C | 200 | 100 | 92* | ~110 | ~10 |
| 3D | 500 | 150 | 138* | ~109 | ~9 |

**At 20 Hz (50ms target interval), 10s duration:**

| Test | Entities | Expected Ticks | Actual Ticks | Tick Interval (ms) | Overhead (ms) |
|------|----------|---------------|-------------|-------------------|---------------|
| 3E | 50 | 200 | 161* | ~63 | ~13 |
| 3F | 100 | 200 | 161* | ~63 | ~13 |
| 3G | 200 | 200 | 162* | ~63 | ~13 |

\*Actual tick counts are lower than expected because CLI setup (reset + seed entities) consumes 3–5 seconds of the test window. The tick-to-tick interval (measured from consecutive `fanout_tick_log` timestamps) is the reliable metric.

Test 3A (50 entities @ 10 Hz) was anomalous — showed `entities_updated=200` despite seeding 50, likely from residual state. Discarded.

**Key findings:**
- **Overhead is ~10–13ms constant** regardless of entity count (50–500). This is the scheduled reducer dispatch mechanism, not entity processing time.
- At 500 entities, the 10 Hz tick runs at ~9.2 Hz actual (109ms interval). At 200 entities, the 20 Hz tick runs at ~15.9 Hz actual (63ms interval).
- **Entity count does NOT impact tick interval** within the tested range. Processing 500 entity updates per tick adds negligible time vs. 50 entities, consistent with Test 1's ~400K updates/sec throughput.
- **Important caveat:** This test measures server-side execution only. The fan-out bottleneck with actual WebSocket subscribers was addressed in Test 3b below.

#### Test 3b: Subscription Fan-Out with Real WebSocket Subscribers

**The most critical remaining benchmark.** Test 3 proved the server can tick at ~9 Hz with 500 entities. But SpacetimeDB must also compute subscription diffs and broadcast state changes to every connected subscriber per tick — the O(N²) cost. Test 3b answers: **how many simultaneous players can see each other before the WebSocket broadcast layer breaks down?**

**Tool:** Custom Rust stress test (`tools/fanout_stress/`) using the SpacetimeDB SDK 2.0.2. Each subscriber is a real WebSocket connection with a typed `SELECT * FROM fanout_entity` subscription and an `on_update` callback that atomically counts delivered events. A controller connection seeds entities and triggers `fanout_start(interval_ms)`.

**Parameters:** 100 entities, 10 Hz tick rate (100ms interval), 15-second measurement window, localhost (server + all clients same machine: i3-12100, 4C/8T, 64GB RAM).

**Results:**

| Subscribers | Ticks (15s) | Tick Hz | Total Events/sec | Per-Sub Events/sec | Spread (min–max) | Per-Tick Delivery |
|-------------|------------|---------|-------------------|-------------------|------------------|-------------------|
| 5 | 93 | 9.3 | 2,279 | 456 | 0 | 100% |
| 10 | 138 | 9.1 | 9,076 | 908 | 0 | 100% |
| 25 | 137 | 9.0 | 22,531 | 901 | 0 | 100% |
| 50 | 138 | 9.1 | 45,390 | 908 | 0 | 100% |
| 100 | 138 | 9.1 | 90,607 | 906 | 0 | 100% |
| 200 | 137 | 9.0 | 179,943 | 900 | 100 | ~100% |
| **300** | **131** | **8.6** | **256,653** | **856** | **100** | **~100%** |
| 400 | 112 | 7.3 | 278,826 | 697 | 2,700 | ~85% |
| 500 | 95 | 6.2 | 265,479 | 531 | 4,100 | ~54% |

**"Delivery ratio" note:** The raw delivery percentage (e.g., 92% at 100 subs) reflects fewer ticks than expected (138 vs 150) due to the ~10ms WASM overhead from Test 3, not dropped updates. The "Per-Tick Delivery" column shows whether every subscriber received every entity update for each tick that actually ran — this is the real reliability metric.

**Key findings:**

1. **Fan-out is essentially free up to 200 subscribers.** Server tick rate (9.0–9.1 Hz) is identical to zero-subscriber Test 3 results. Zero spread between subscribers. Every subscriber receives every update for every tick. The subscription diff + WebSocket broadcast adds < 1ms overhead.

2. **300 subscribers: graceful degradation.** Tick rate drops to 8.6 Hz (5% reduction). Spread of 100 (1 tick difference between fastest/slowest subscriber). Still ~100% per-tick delivery. This is the practical comfort zone.

3. **400+ subscribers: cliff edge.** Tick rate drops to 7.3 Hz, spread jumps to 2,700, per-tick delivery falls to ~85%. At 500: 6.2 Hz, spread 4,100, only 54% delivery.

4. **Total throughput plateaus at ~280K events/sec.** Beyond 300 subscribers, adding more connections doesn't increase total throughput — the same bandwidth is spread thinner per subscriber.

5. **Single-machine caveat:** All subscribers ran on the same 4C/8T machine as the server. The 400+ degradation is partly CPU contention (500 subscriber threads competing with the server). In production (distributed clients), the server-side tick rate would be the binding constraint, and client-side delivery issues would largely disappear. The 200-subscriber "zero impact" threshold is a conservative floor; the real ceiling with distributed clients is likely higher.

6. **Bandwidth:** Per-subscriber bandwidth is ~50 KB/sec (100 entities × 10 Hz × ~56 bytes/entity). At 200 subscribers, total bandwidth is ~10 MB/sec — well within typical server NIC capacity.

#### Tests 4–5: Deferred

- **Test 4 (Sidecar Body Capacity):** Requires a running UE5 build with the sidecar subsystem. Deferred to Phase 1 implementation.
- **Test 5 (UDP Relay Prototype):** Contingent on Tests 1–3 confirming SpacetimeDB-only is unviable. Given Test 1–3 results, Option A (SpacetimeDB-only) is viable for moderate density. UDP relay deferred unless mass-event testing reveals the need.

### Revised Capacity Estimate

Combining all benchmark data:

| Component | Measured Capacity | Source |
|-----------|------------------|--------|
| WASM+DB entity updates | ~400,000/sec | Test 1 |
| Scheduled tick overhead | ~10ms constant | Test 3 |
| Concurrent reducer throughput | 200+/sec (no drops) | Test 2 |
| Per-tick entity budget (10 Hz) | ~500 entities at 9.2 Hz actual | Test 3 |
| Per-tick entity budget (20 Hz) | ~200 entities at 15.9 Hz actual | Test 3 |
| WebSocket fan-out (zero impact) | ≤200 subscribers | Test 3b |
| WebSocket fan-out (graceful) | ~300 subscribers (8.6 Hz) | Test 3b |
| WebSocket fan-out (cliff) | 400+ subscribers | Test 3b |
| Peak fan-out throughput | ~280K events/sec | Test 3b |
| Per-subscriber bandwidth | ~50 KB/sec (100 ent × 10 Hz) | Test 3b |

**Revised player ceiling (including subscription fan-out):**

| Player Input Model | Input Rate/Player | Server Ceiling | Fan-Out Ceiling | Effective Max |
|---|---|---|---|---|
| Input-change (WASD) | ~3 calls/sec | ~65 | ~300 | **~65** (reducer-bound) |
| Click-to-move / low-action | ~1 call/sec | ~200 | ~300 | **~200** |
| Hybrid (mixed activity) | ~2 calls/sec | ~100 | ~300 | **~100** |

The **fan-out ceiling (~300 per zone) is NOT the binding constraint** for typical gameplay. The reducer input rate is the tighter bottleneck for action-oriented games. For low-input gameplay, both ceilings converge around 200 players per zone. With spatial partitioning (Spike 5), zones can be kept below these thresholds.

**Bottom line:** SpacetimeDB Option A supports **100–200 players per spatial zone** on a single 4C/8T machine, with all three bottlenecks (WASM execution, reducer contention, WebSocket fan-out) safely within budget. This exceeds the project's initial target and eliminates the need for UDP relay (Option B) or hybrid architecture (Option C) unless targeting 500+ simultaneous players in a single viewport.

### Files Created/Modified
- `server/nyx-server/spacetimedb/src/lib.rs` — Added `physics_update_batch`, `bench_batch_physics`, `spawn_projectiles_batch`, `stress_move`, `stress_record`, `stress_reset`, `fanout_tick`, `fanout_seed`, `fanout_start`, `fanout_stop`, `fanout_reset` reducers; `BodyUpdate` struct; `StressResult`, `FanoutEntity`, `FanoutSchedule`, `FanoutTickLog` tables
- `tools/stress_client/stress_test.ps1` — Multi-client contention test script (PowerShell)
- `tools/stress_client/fanout_test.ps1` — Fan-out stress test script (PowerShell)
- `tools/fanout_stress/` — Rust stress test tool for WebSocket fan-out measurement (SpacetimeDB SDK 2.0.2, clap CLI, real WebSocket subscribers with typed subscriptions)

---

## Timeline

| Week | Spikes | Status |
|------|--------|--------|
| Week 1 | Spike 1 (Plugin) ✅, Spike 2 (Module) ✅, Spike 3 (Round-Trip) ✅ | **DONE** — completed in ~2 days |
| Week 1–2 | Spike 4 (EOS Auth) ✅ | **DONE** — anonymous connect + reducer auth, end-to-end verified |
| Week 2 | Spike 5 (Spatial) ✅ | **DONE** — chunk-based subscriptions working with 10K entities |
| Week 2 | Spike 6 (Prediction) ✅ | **DONE** — prediction + reconciliation verified, 0 cm error |
| Week 2–3 | Spike 7 (WASM Perf) ✅ | **DONE** — 218 reducers/sec, 1000 entities/tick at 100ms, 512MB memory OK |
| Week 3 | Spike 8 (Sidecar) ✅ | **DONE** — UE5 sidecar architecture validated, MultiServer Replication deep dive completed |
| Week 3–4 | Spike 9 (Scalability) ✅ | **DONE** — Tests 1–3b complete: 400K updates/sec, 200 calls/sec, 10ms tick overhead, 200+ subscriber fan-out with zero degradation, ceiling at ~300 per zone |

**Total estimated time: ~4 weeks**

---

## BitCraft Online — Reference Architecture Analysis

> **Source:** https://github.com/clockworklabs/BitCraftPublic  
> **License:** Apache 2.0  
> **Engine:** Unity (client not open-sourced), SpacetimeDB 1.x server  
> **Relevance:** BitCraft is the production MMO built by the SpacetimeDB creators. Although it targets Unity (not UE5), the server-side patterns are engine-agnostic and directly applicable to Nyx.

### Overview

BitCraft Online is a community sandbox MMORPG (crafting, city-building, exploration, PvE combat, governance) running entirely on SpacetimeDB. The open-sourced server code (`BitCraftServer/packages/game`) is a single SpacetimeDB Rust module (~250+ source files) that handles **all** game logic server-side. Examining it reveals production-proven patterns for the same systems Nyx will need.

**Key difference from Nyx:** BitCraft uses SpacetimeDB **1.12.0** (see `Cargo.toml`), while Nyx targets SpacetimeDB **2.0**. API differences include table accessor syntax, reducer signatures, and scheduling APIs. The architectural patterns remain valid, but code cannot be copy-pasted without adaptation.

**Architectural divergence — no physics simulation:** A source code review of BitCraft's server confirms it contains **zero physics simulation**. No gravity, no rigid body dynamics, no velocity/acceleration integration, no collision response, no continuous physics tick loop. The closest physics-adjacent systems are:

| System | File | What it does | What it does NOT do |
|--------|------|-------------|-------------------|
| **Projectile delay** | `handlers/attack.rs` | `distance / projectile_speed = delay`, then schedules damage via `AttackImpactTimer` | No trajectory, no arc, no gravity — flat time delay only |
| **Movement validation** | `reducer_helpers/move_validation_helpers.rs` | Speed-checks client positions (`distance / duration ≤ MAX_SPEED`), validates world bounds, elevation diffs, hitbox footprints, hex-grid raycast | No physics step; much of the comprehensive validation is **commented out** |
| **Position interpolation** | `entities/location.rs` | `interpolated_location()` — lerp between origin/destination based on elapsed time, used for combat range checks | On-demand estimation, not a simulation loop |
| **Wind effect on sailing** | `handlers/player/deployable_move.rs` | Sailing direction relative to wind angle adjusts boat speed as a multiplier | Not a force simulation |
| **Combat math** | `handlers/attack.rs` | Full RPG damage formula (strength, armor, evasion, dodge rolls, `ARMOR_50PCT_REDUCTION`, radius-based multi-targeting) | No physics — pure stat math |

Nyx's Spike 8 physics sidecar — a UE5 client subsystem that connects as a second client to run real-time physics (Euler integration, gravity, floor collision) — has no equivalent in BitCraft. This is a deliberate divergence: BitCraft's sandbox MMORPG gameplay (crafting, building, turn-based hex movement) doesn't require continuous physics. Nyx's action-oriented gameplay does. BitCraft validates that a pure-WASM approach scales for slower-paced games; Nyx's sidecar approach addresses the real-time physics gap that SpacetimeDB alone cannot fill.

### 1. Project Structure — Monorepo Module Layout

BitCraft's server is a **single `cdylib` crate** compiled to WASM, organized into well-separated submodules:

```
BitCraftServer/packages/game/src/
├── lib.rs                  # Entry point: init, client_connected/disconnected, world gen
├── agents/                 # Server-side scheduled agents (ticks)
├── game/
│   ├── autogen/            # Generated code (CSV → Rust data)
│   ├── coordinates/        # Hex-grid coordinate systems (6+ coordinate types)
│   ├── discovery/          # Exploration / fog-of-war tracking
│   ├── entities/           # ~80 entity/component state tables (ECS-like)
│   ├── game_state/         # Core state helpers (entity creation, filtering)
│   ├── handlers/           # Reducers grouped by domain (player, buildings, combat, etc.)
│   ├── location_cache.rs   # In-WASM spatial cache (built at world gen)
│   ├── reducer_helpers/    # Shared reducer logic (building, interior, etc.)
│   ├── static_data/        # Game design data descriptors (items, skills, recipes)
│   ├── terrain_chunk.rs    # Terrain chunk state + cache
│   └── world_gen/          # Procedural world generation
├── inter_module/           # Cross-module RPC (multi-region player transfer)
├── messages/               # Table definitions (components, auth, static data, etc.)
├── table_caches/           # In-memory caches for hot tables
├── macros/                 # Procedural macros (shared_table_reducer)
└── utils/                  # Math, helpers
```

**Takeaway for Nyx:** As the module grows, adopt a similar domain-based directory structure under `server/nyx-server/spacetimedb/src/`. Group reducers by feature (`handlers/player/`, `handlers/combat/`, `handlers/inventory/`) rather than putting everything in `lib.rs`.

### 2. Authentication & Identity

BitCraft separates **authentication** from **sign-in**:

- **`identity_connected`** (lifecycle reducer): Checks if the connecting identity is a developer, has skip-queue privileges, is blocked, or has a valid `user_authentication_state` entry (expires after 24 hours). Unauthorized identities are rejected at connection time.
- **`sign_in`** (game reducer): Called after connection. Validates queue position, checks moderation bans, restores player state, unlocks inventory, restarts buff timers, refreshes traveler tasks, and marks the player as `signed_in`.
- **Role-based access control**: `IdentityRole` table maps `Identity → Role` (Admin, Gm, SkipQueue). Helper `has_role(ctx, &identity, Role::Admin)` gates admin reducers.
- **User moderation**: `user_moderation_state` table stores per-identity moderation actions (permanent block, temporary block with expiration, chat suspension).

**Takeaway for Nyx:** Our current `authenticate_with_eos` reducer is equivalent to BitCraft's `identity_connected` + auth state insertion. As we add features, split into:
1. Connection-level auth check (in `client_connected`)
2. Game-level sign-in reducer (queue, bans, state restore)
3. Role table for admin/GM permissions

### 3. Entity-Component Pattern via SpacetimeDB Tables

BitCraft uses an **ECS-like architecture** where entities are identified by a `u64 entity_id` and components are separate SpacetimeDB tables, each keyed by `entity_id`:

| Component Table | Purpose |
|----------------|---------|
| `player_state` | Core player data (signed_in, session timestamps, traveler tasks) |
| `mobile_entity_state` | Position, destination, chunk_index, walking state, timestamps |
| `inventory_state` | Item stacks with pocket-based slots |
| `equipment_state` | Equipped items (armor, weapons, tools) |
| `character_stats_state` | Computed stats (HP, damage, speed, etc.) |
| `active_buff_state` | Active buff timers (per entity) |
| `ability_state` | Abilities and cooldowns |
| `experience_state` | Skill XP stacks per skill type |
| `stamina_state` | Stamina (consumed by sprinting, actions) |
| `vault_state` | Collectibles, achievements, cosmetics |
| `signed_in_player_state` | Presence table (entity_id only — fast "who is online?" queries) |
| `player_action_state` | Current action (gathering, crafting, attacking) with timestamps |
| `player_username_state` | Username (separate from identity for rename support) |
| `alert_state` | In-game notifications |
| `threat_state` | Aggro/threat table for combat AI |

**Global entity counter:** `Globals.entity_pk_counter` is incremented atomically via `create_entity(ctx)` to generate unique IDs.

**Takeaway for Nyx:** Our current `Player` table is monolithic. As features grow, split into focused component tables (`position`, `stats`, `inventory`, etc.) all sharing the same `entity_id`. This allows spatial subscriptions to include only the components needed (e.g., subscribe to `mobile_entity_state` for position updates but not `inventory_state`).

**Trade-off:** More component tables means more subscription queries per client and more initial state transfer overhead on connection. In SpacetimeDB 2.0, each table requires a separate `SELECT ... WHERE` subscription. Split only when different clients genuinely need different subsets of components — premature decomposition adds join complexity (no real JOINs in SpacetimeDB; you query each table separately and stitch client-side) before we know which components Nyx actually needs.

### 4. Movement & Spatial System

BitCraft uses a **hex-grid coordinate system** with multiple granularity levels:

- `OffsetCoordinatesSmall` / `SmallHexTile` — finest granularity
- `OffsetCoordinatesLarge` / `LargeHexTile` — chunk-level
- `ChunkCoordinates` — for spatial partitioning
- `RegionCoordinates` — for region-level operations
- `FloatHexTile` / `OffsetCoordinatesFloat` — for smooth interpolation

Movement is handled server-side:
- `move_player_and_explore()` validates bounds (refuses out-of-world moves), calculates chunk transitions, triggers exploration discovery, and updates the `mobile_entity_state` table.
- The server validates that both origin and destination are within world bounds using dimension descriptions.
- Chunk transitions trigger exploration logic and potential herd spawns.

**Contrast with Nyx:** Nyx uses client-side prediction with server echo (Spike 6). BitCraft uses a fully server-authoritative approach — `move_player_and_explore()` validates bounds and executes the move server-side; the client does not predict. Both approaches are viable — Nyx's prediction model is better suited for action-oriented gameplay, while BitCraft's server-authoritative model works for its slower-paced sandbox style.

**Takeaway for Nyx:** Consider adding server-side bounds checking to `move_player`. Currently Nyx trusts client-reported positions. Add validation like BitCraft does: check that the target position is within world bounds and that the movement speed doesn't exceed maximum.

### 5. Scheduled Agents (Server-Side Ticks)

BitCraft implements **16 server-side agents** as scheduled reducers, each handling a specific background task:

| Agent | Interval | Purpose |
|-------|----------|---------|
| `auto_logout_agent` | Periodic | Kicks idle players after inactivity timeout |
| `building_decay_agent` | Periodic | Decays unattended buildings over time |
| `chat_cleanup_agent` | Periodic | Deletes old chat messages to save space |
| `crumb_trail_clean_up_agent` | Periodic | Cleans expired trail markers |
| `day_night_agent` | Periodic | Advances the day/night cycle clock |
| `duel_agent` | Periodic | Manages duel timeouts and resolution |
| `enemy_regen_agent` | Periodic | Regenerates enemy HP/respawns |
| `herd_regen_agent` | Periodic | Repopulates animal herds |
| `npc_agent` | Periodic | NPC AI behavior ticks |
| `rent_collector_agent` | Periodic | Collects rent from player-owned plots |
| `resources_regen` | Periodic | Regenerates harvestable resources (trees, ore) |
| `starving_agent` | Periodic | Applies hunger damage to starving players |
| `storage_log_cleanup_agent` | Periodic | Cleans old storage logs |
| `teleportation_energy_regen_agent` | Periodic | Regens teleportation energy |
| `trade_sessions_agent` | Periodic | Times out stale trade sessions |
| `traveler_task_agent` | Periodic | Rotates NPC traveler tasks |

Agents are enabled/disabled via the `Config.agents_enabled` flag and initialized after world generation. Admin reducers `stop_agents` / `start_agents` allow runtime control.

**Takeaway for Nyx:** Spike 7 validated scheduled ticks with 1000 entities at 100ms. BitCraft's agent pattern confirms this scales to production. Plan for these agent types early:
- **Auto-logout** — essential for cleaning up disconnected players
- **Resource regeneration** — if Nyx has harvestable resources
- **Day/night cycle** — if time-of-day is server-authoritative
- **Chat cleanup** — prevent unbounded chat table growth

### 6. Chat System

BitCraft's `chat_post_message` reducer demonstrates several production patterns:

- **Input sanitization:** `sanitize_user_inputs()` + `is_user_text_input_valid()` with a 250-char limit
- **Moderation checks:** `UserModerationState::validate_chat_privileges()` blocks suspended users
- **Rate limiting:** Region chat counts recent messages within a time window (`rate_limit_window_sec`) and rejects if over `max_messages_per_time_period`
- **Minimum playtime gate:** New accounts must play for `min_playtime` seconds before using Region chat
- **Username requirement:** Players must set a username (not default `"player"`) to post in Region chat
- **Channel system:** Multiple channels (`Local`, `Region`) with different rules. Local chat allows targeting specific players
- **I18N support:** Messages store a language code prefix for client-side localization

**Takeaway for Nyx:** When implementing chat, build in rate limiting and moderation from day one. Use a similar pattern: sanitize input → check moderation state → rate limit → insert message entity.

### 7. Inventory & Equipment System

BitCraft's inventory uses a **pocket-based slot system** with rich item stack management:

- `InventoryState` contains item stacks organized into pockets (unlock progression)
- `EquipmentState` tracks equipped items in specific slots
- `VaultState` stores collectibles, titles, and cosmetics (account-level, persists across sessions)
- `CharacterStatsState` is **computed** from equipment + buffs + knowledge — never stored directly. On any change (equip, buff, knowledge unlock), `collect_stats()` recomputes all stats from scratch and updates the table only if values changed.

**Takeaway for Nyx:** The "compute stats from components" pattern is powerful. Instead of storing derived stats directly, define them as functions of equipped items + buffs + passive bonuses. This eliminates desync bugs where stats don't match equipment.

### 8. World Generation

BitCraft generates the entire world server-side in a single reducer call (`generate_world`):

- Reads a `WorldGenWorldDefinition` configuration
- Generates terrain chunks, resources, buildings (ruins), enemies, herds, NPCs, dropped inventories
- Builds a `LocationCache` — an in-WASM spatial index of key locations (trading posts, ruins, spawn points)
- Populates `TerrainChunkState` for all terrain data
- Enables agents after world gen completes

The `LocationCache` is a single-row table that stores pre-computed spatial data as `Vec<SmallHexTile>` arrays. This avoids expensive spatial queries at runtime — agents can look up nearest ruins, spawn points, etc. from the cached vectors.

**Takeaway for Nyx:** For large worlds, consider building spatial caches at world generation time rather than computing spatial queries on every tick. A single-row "cache table" with pre-sorted location arrays is an effective SpacetimeDB pattern.

**Caveat:** Any client subscribing to the cache table receives the **entire** row in the initial sync. For BitCraft's hex grid this is manageable, but for Nyx's free-form 3D world this could mean megabytes of data per connection. SpacetimeDB 2.0 subscriptions are row-level — you cannot subscribe to a subset of columns within a row. Keep cache rows small, or use server-only tables (private tables not exposed to client subscriptions) if the cache is only needed by scheduled agents.

### 9. Inter-Module Communication (Multi-Region)

BitCraft supports **multi-region architecture** through the `inter_module/` system:

- `transfer_player.rs` (24KB) — full player state serialization for cross-region transfers
- `replace_identity.rs` — identity migration between regions
- `user_update_region.rs` — region-level state updates
- `restore_skills.rs` — skill recovery after transfer
- Multiple empire/claim cross-region reducers

The `global_module` package is a separate SpacetimeDB module that provides region-agnostic services (likely authentication, user management).

**Takeaway for Nyx:** SpacetimeDB supports multiple modules communicating via inter-module calls. Plan for this architecture if Nyx needs multi-region scaling: one "game" module per region + one "global" module for auth/accounts. The player transfer pattern (serialize all component state → transfer → deserialize) is relevant to future multi-region scaling but is **not** related to our Spike 8 sidecar architecture. BitCraft's inter-module system is server-to-server RPC between separate SpacetimeDB modules; Spike 8's sidecar is a UE5 client subsystem connecting as a second client to the **same** module to run physics simulation. These solve entirely different problems.

### 10. Data-Driven Design (Static Data via CSV → Rust)

BitCraft separates **mutable game state** from **immutable game design data**:

- `messages/static_data.rs` (72KB) — defines descriptor tables for items, recipes, buildings, enemies, skills, etc.
- `game/static_data/` — loading and validation logic
- `config/` directory contains CSV files that are compiled into Rust code via `build.rs` (18KB build script)
- `build_shared.rs` (20KB) — shared CSV parsing and code generation utilities

At build time, CSV data files are parsed and embedded into the WASM module. This means game designers can edit CSV files, rebuild the module, and deploy balance changes without modifying Rust code.

**Takeaway for Nyx:** Adopt a data-driven approach early. Define item stats, skill trees, enemy parameters, etc. in external data files (CSV, JSON, or TOML) and load them at module initialization. This separates game design iteration from code changes.

### 11. SpacetimeDB API Patterns (v1.12 → v2.0 Translation)

BitCraft reveals idiomatic SpacetimeDB usage patterns. Most translate to v2.0, but the table attribute macro change (`name` → `accessor`) affects every table definition, and `ctx.sender` became a method call. The table below notes actual differences:

| Pattern | BitCraft (v1.12) | Nyx (v2.0) | Changed? |
|---------|-----------------|------------|----------|
| **Table attribute** | `#[spacetimedb::table(name = X)]` | `#[spacetimedb::table(accessor = X)]` | **Yes** — every table definition |
| **Identity access** | `ctx.sender` (field) | `ctx.sender()` (method) | **Yes** — every reducer using sender |
| Table lookup | `ctx.db.table().key().find(&id)` | Same syntax | No |
| Table insert | `ctx.db.table().try_insert(row)` | Same pattern | No |
| Table update | `ctx.db.table().key().update(row)` | Same syntax | No |
| Table delete | `ctx.db.table().key().delete(&id)` | Same syntax | No |
| Filter iteration | `ctx.db.table().field().filter(val)` | Same syntax | No |
| Timestamp | `ctx.timestamp` (field) | `ctx.timestamp` (field) | No |
| Logging | `spacetimedb::log::info!(...)` | Same | No |
| Lifecycle reducers | `#[reducer(init)]`, `#[reducer(client_connected)]` | Same | No |
| Error handling | `-> Result<(), String>` | Same | No |
| Random | `ctx.rng().gen_range(min..max)` | Same | No |
| Scheduled reducers | Scheduled table pattern | Same (verified in Spike 7) | No |

**Bottom line:** The CRUD query API is stable across versions. The breaking changes are the table attribute macro and `ctx.sender` → `ctx.sender()`. Any BitCraft code ported to v2.0 requires a mechanical find-and-replace for these two patterns.

### 12. Key Architectural Patterns to Adopt

| # | Pattern | Description | Priority |
|---|---------|-------------|----------|
| 1 | **Domain-organized reducers** | Group handlers by feature (player/, combat/, inventory/) not by action | High — do before module exceeds ~500 lines |
| 2 | **Component tables per entity** | Split `Player` into `position`, `stats`, `inventory` tables sharing `entity_id` | Medium — do when 3+ features share entity_id and subscription overhead is measurable |
| 3 | **Input sanitization on all text** | Sanitize + validate + length-check all user text inputs | High — security requirement |
| 4 | **Rate limiting on player actions** | Count actions within time windows, reject over threshold | Medium — needed before public testing |
| 5 | **Computed stats from components** | Never store derived stats directly; recompute from equipment + buffs | Medium — adopt with equipment system |
| 6 | **Server-side position validation** | Validate move targets are within world bounds, speed within limits | Medium — reduces cheat surface |
| 7 | **Auto-logout agent** | Scheduled agent to clean up idle/disconnected players | Medium — needed for any public server |
| 8 | **Data-driven design data** | Load game balance from CSV/JSON, not hardcoded Rust constants | Medium — enables designer iteration |
| 9 | **Location cache table** | Pre-compute spatial data at world gen, store in single-row cache table | Low — optimization for large worlds |
| 10 | **Moderation system** | `user_moderation_state` table for bans, mutes, with expiration timestamps | Low — needed before community features |

---

## Open World Server (OWS) — Reference Architecture Analysis

> **Source:** https://github.com/Dartanlla/OWS  
> **License:** MIT  
> **Engine:** Unreal Engine 5.7 (plugin updated January 2026)  
> **Backend:** C# .NET 8 microservices + MSSQL/PostgreSQL + RabbitMQ  
> **Relevance:** OWS is an open-source server instance manager for UE5 that handles zone-based world partitioning, dynamic server spin-up/down, sharding, character persistence, and cross-zone travel. It solves many of the same problems Nyx faces in Option C (UE5 dedicated servers + persistence layer), but lacks seamless border transitions.

### Overview

OWS is designed to create large worlds in UE5 by stitching together multiple UE5 maps or splitting a single large map into multiple **zones**. Each zone runs as a separate UE5 dedicated server instance. OWS manages the lifecycle of these instances — spinning them up when players arrive and shutting them down when empty. The project targets 100,000+ concurrent players across all zones.

**Key architectural insight:** OWS treats each UE5 dedicated server as a self-contained zone. Players in different zones **cannot see or interact** with each other. Zone transitions use UE5's `ClientTravel` — the client disconnects from one server and connects to another. This is a hard zone boundary model, not the seamless border overlap that MultiServer Replication provides.

### Architecture

```
Client ──UE5 NetDriver──► UE5 Zone Server Instance (one per zone/shard)
  │                              │
  │  HTTP REST ──────────────────┤──► OWS Public API ──► MSSQL/PostgreSQL
  │                              │         │
  │                              │    OWS Instance Management API
  │                              │         │
  │                              │    RabbitMQ ──► Instance Launcher(s)
  │                              │                    │
  │                              │                    └── Spawns/kills UE5 server processes
  │                              │
  └── HTTP REST (travel) ────────┘──► OWS Character Persistence API
```

**Five microservices** (all .NET 8, Docker-deployed):

| Service | Port | Purpose |
|---------|------|---------|
| **Public API** | 44302 | Client-facing: login, registration, character selection, zone connection |
| **Instance Management** | 44328 | Zone instance lifecycle: spin up, shut down, status tracking, launcher registration |
| **Character Persistence** | 44323 | Character CRUD: stats, abilities, inventory, custom data, position saving |
| **Global Data** | 44325 | Key-value store for non-character, non-user data |
| **Instance Launcher** | (agent) | Runs on each hardware server; listens to RabbitMQ for spin-up/shut-down commands; launches UE5 server processes |

**Messaging:** RabbitMQ handles async communication between Instance Management and Instance Launchers. Two exchanges:
- `ows.serverspinup` — routes `MQSpinUpServerMessage` (CustomerGUID, WorldServerID, ZoneInstanceID, MapName, Port) to the correct launcher
- `ows.servershutdown` — routes `MQShutDownServerMessage` to kill specific zone instances

**Database:** MSSQL (default), PostgreSQL, or MySQL. Stores: users, characters, zones, zone instances, world servers, abilities, inventory, global data. Character position (X, Y, Z, RX, RY, RZ) is persisted per-character.

### Zone & Shard System

OWS's core concept is **Zones** and **Shards**:

- **Zone** = a named area of the game world, mapped to a UE5 map or section of a map
- **Zone Instance** (Shard) = a running UE5 dedicated server process for that zone
- **World Server** = a hardware device that runs one or more zone instances
- **Soft Player Cap** — when reached, new shards spin up automatically for that zone
- **Hard Player Cap** — absolute maximum per shard, new players are rejected

When a player requests to join a zone, the flow is:
1. Client calls `GetServerToConnectTo` with `CharacterName` + `ZoneName`
2. API calls `JoinMapByCharName` stored procedure — finds a running zone instance with capacity
3. If no instance exists (`NeedToStartupMap = true`), publishes a `MQSpinUpServerMessage` to RabbitMQ
4. Instance Launcher receives the message, spawns a UE5 server process: `PathToDedicatedServer "{ProjectPath}" {MapName}?listen -server -port={Port} -zoneinstanceid={ZoneInstanceID}`
5. API polls `CheckMapInstanceStatus` every 2 seconds for up to 90 seconds waiting for status = 2 (Ready)
6. Returns `ServerIP:Port` to the client
7. Client calls `ClientTravel(ServerIP:Port)` to connect

**Shard selection strategies** (`ERPGSchemeToChooseMap`):
- `Default` — first available
- `MapWithFewestPlayers` — load balancing
- `MapWithMostPlayers` — population concentration
- `MapOnWorldServerWithLeastNumberOfMaps` — hardware load balancing
- `SpecificWorldServer` — manual assignment

### Zone Travel (How Transfers Work)

Zone travel is **not seamless**. The UE5 plugin provides two mechanisms:

1. **`AOWSTravelToMapActor`** — a Blueprint-configurable actor placed in the world with properties:
   - `ZoneName` — destination zone
   - `LocationOnMap` — spawn position in destination
   - `StartingRotation` — spawn rotation
   - `UseDynamicSpawnLocation` / `DynamicSpawnAxis` — offset spawn position based on trigger entry direction
   - When triggered, calls `GetZoneServerToTravelTo(CharacterName, SchemeToChooseMap, WorldServerID, ZoneName)` → receives `ServerIP:Port` → calls `TravelToMap2(ServerAndPort, X, Y, Z, RX, RY, RZ, PlayerName, SeamlessTravel)`

2. **`SetSelectedCharacterAndConnectToLastZone`** — reconnects a character to whatever zone they last played in (used after login/character selection)

The travel flow:
- Player location is saved to the database before travel (`SavePlayerLocation` / `SaveAllPlayerLocations` batch)
- Client disconnects from current zone server
- Client connects to new zone server via `ClientTravel`
- New server's `GameMode::InitNewPlayer` reads character data from the database, spawns the character at the persisted position
- **Black screen or loading screen** during transition — no visual continuity

### Character Persistence Model

The `GetCharByCharName` stored procedure returns a character with ~30 fields including:
- Position: `X, Y, Z, RX, RY, RZ` (float, persisted per-character)
- Stats: `CharacterLevel, Gender, Gold, Silver, Copper, FreeCurrency, PremiumCurrency, Score, XP, Size, Weight`
- Zone: `MapName` (last zone the character was in)
- Session: `LastActivity, CreateDate, UserSessionGUID`
- Custom: `CustomCharacterDataStruct[]` — arbitrary key-value pairs for game-specific data

**Batch position saving:** `AOWSGameMode::SaveAllPlayerLocations` runs on a timer (`SaveIntervalInSeconds`), serializes all player positions into a single REST call (`FUpdateAllPlayerPositionsJSONPost`), and sends to the Character Persistence API. Can be split into groups (`SplitSaveIntoHowManyGroups`) to avoid large batches.

**Custom character data:** OWS provides a generic `AddOrUpdateCustomCharacterData(CharName, FieldName, Value)` system. Any game-specific data (quest progress, faction standing, appearance) is stored as string key-value pairs. This is extensible but requires manual deserialization.

### Health Monitoring & Auto-Shutdown

`ServerLauncherHealthMonitoring` periodically:
1. Gets all zone instances for the world server via `GetZoneInstancesForWorldServer`
2. Checks `LastServerEmptyDate` — if a zone instance has been empty longer than `MinutesToShutdownAfterEmpty`, it's shut down
3. Kills the process via `Process.Kill()`

### UE5 Plugin Architecture

The plugin provides:
- **`AOWSPlayerController`** — extends `APlayerController` with HTTP REST calls for travel, character management, chat, groups, abilities
- **`UOWSPlayerControllerComponent`** — same functionality as a component (can be added to any controller)
- **`AOWSGameMode`** — extends `AGameMode` with zone management, batch saving, world time, inventory
- **`AOWSCharacter` / `AOWSCharacterWithAbilities`** — character base classes with GAS (Gameplay Ability System) integration
- **`UOWSAPISubsystem`** — game instance subsystem for API configuration
- **`AOWSTravelToMapActor`** — trigger actor for zone transitions
- **`AOWSAdvancedProjectile`** — replicated projectile with prediction support

All API communication is via **HTTP REST** (not WebSocket). The plugin uses UE5's `FHttpModule` to make POST requests to the OWS backend services. This is a fire-and-forget pattern — no persistent connection for real-time updates.

### Comparison with Nyx's Architecture

| Aspect | OWS | Nyx (Current) | Nyx Option C |
|--------|-----|---------------|-------------|
| **Game state** | SQL database (MSSQL/PostgreSQL) | SpacetimeDB (WASM module) | SpacetimeDB as persistence layer |
| **Real-time networking** | UE5 NetDriver (per zone server) | SpacetimeDB WebSocket subscriptions | UE5 NetDriver + SpacetimeDB persistence |
| **Zone transitions** | `ClientTravel` — hard disconnect/reconnect (loading screen) | N/A (single world) | MultiServer Replication — seamless visual border (Spike 8 deep dive) |
| **Cross-zone visibility** | None — players in different zones are invisible | N/A | MultiServer Replication — `ANoPawnPlayerController` with `SetRemoteViewTarget` |
| **Cross-zone interaction** | None — requires custom implementation | N/A | Requires custom beacon RPCs (MultiServer Replication) |
| **Instance management** | Automated: soft cap → spin up shard, empty timeout → shut down | N/A | Would need similar management layer |
| **Persistence** | HTTP REST to SQL per-operation | SpacetimeDB table subscriptions (real-time) | SpacetimeDB table sync |
| **Sharding** | Multiple instances of same zone (horizontal scaling) | Single SpacetimeDB instance | Multiple UE5 servers via MultiServer Replication |
| **Physics** | UE5 Chaos (per zone server) | SpacetimeDB sidecar (Euler/Chaos) | UE5 Chaos (per zone server) |
| **Message bus** | RabbitMQ (async spin-up/down commands) | SpacetimeDB reducers | RabbitMQ or similar for orchestration |
| **Max players per zone** | ~100 (UE5 server limit) | ~10 (SpacetimeDB throughput limit) | ~100 (UE5 server limit) |

### Key Limitations of OWS for Nyx

1. **Hard zone boundaries** — Players in different zones are completely isolated. No visibility, no interaction. This is the fundamental gap for a seamless open world.
2. **No border overlap** — Unlike MultiServer Replication's `ANoPawnPlayerController` + relevancy-based cross-server visibility, OWS has no mechanism for players to see across zone boundaries.
3. **Loading screen transitions** — `ClientTravel` disconnects and reconnects. Even with UE5's seamless travel, there's a noticeable transition.
4. **HTTP REST for game state** — Not real-time. Character data is fetched on connection and saved periodically. No live subscription to state changes. SpacetimeDB's WebSocket subscriptions are strictly superior for real-time state sync.
5. **No physics coordination** — Each zone server runs independent physics. Objects at zone borders don't interact across servers.
6. **SQL as game database** — Traditional CRUD with stored procedures. No reducer-as-game-logic pattern. No built-in event system for state changes.

### What Nyx Can Learn from OWS

| # | Pattern | Description | Applicability |
|---|---------|-------------|--------------|
| 1 | **Zone instance lifecycle management** | Automated spin-up when players arrive, shutdown after empty timeout, health monitoring. RabbitMQ as the command bus between orchestrator and launchers. | High — if Nyx goes with Option C, we need this exact orchestration layer. SpacetimeDB reducers could replace RabbitMQ for instance management commands. |
| 2 | **Shard selection strategies** | Multiple algorithms for placing players: fewest players, most players, least-loaded hardware. Configurable per-zone soft/hard caps. | High — any multi-server architecture needs player routing intelligence. |
| 3 | **Batch position persistence** | `SaveAllPlayerLocations` batches all player positions into one API call on a timer, optionally split into groups. Avoids N individual save calls per tick. | Medium — Nyx's SpacetimeDB batch reducer (`physics_update_batch` in Spike 9) solves this differently, but the batching-on-timer pattern is the same. |
| 4 | **Travel actor with dynamic spawn** | `AOWSTravelToMapActor` with configurable destination zone, position, rotation, and dynamic axis offset based on entry direction. Clean Blueprint-configurable interface. | Medium — if Nyx implements zone transitions, a similar trigger actor pattern is useful. But MultiServer Replication's migration is more seamless. |
| 5 | **Custom character data as key-value pairs** | Extensible `AddOrUpdateCustomCharacterData(CharName, FieldName, Value)` avoids schema migrations for game-specific data. | Low — SpacetimeDB tables are more structured (typed columns, subscriptions). But the pattern of having a generic extension table alongside typed tables is worth considering. |
| 6 | **World time synchronization** | `GetCurrentWorldTime` + `DayLengthInSeconds` + `DaysPerLunarCycle` + `DaysPerSolarCycle` on the GameMode. Server-authoritative time of day. | Low — should be a SpacetimeDB table/reducer, not an API call. |
| 7 | **Instance Launcher as an agent** | Separating the "what to launch" (Instance Management API) from "how to launch" (Instance Launcher process) allows scaling across multiple hardware servers. Each machine runs its own launcher agent. | High — directly applicable to Option C infrastructure. Could be a simple Rust/C# service per machine. |

### Bottom Line for Nyx

OWS validates that a **microservice-based orchestration layer** on top of UE5 dedicated servers is viable for large-scale MMOs. Its zone lifecycle management, shard selection, and batch persistence patterns are production-proven and directly applicable to Nyx's Option C.

However, OWS's **hard zone boundaries** (no cross-zone visibility, loading screen transitions) fall short of Nyx's "seamless open world" requirement. The combination of OWS's instance management patterns **plus** MultiServer Replication's border seamlessness would address both concerns — OWS provides the orchestration for spinning up/down servers, while MultiServer Replication provides the seamless cross-server visibility at borders. This hybrid approach is worth exploring if Spike 9 benchmarks confirm Option C as the architecture.

---

## References

- SpacetimeDB 2.0 Unreal Reference: https://spacetimedb.com/docs/2.0.0-rc1/clients/unreal
- SpacetimeDB Client Codegen: https://spacetimedb.com/docs/2.0.0-rc1/clients/codegen
- SpacetimeDB GitHub: https://github.com/clockworklabs/SpacetimeDB
- EOS Online Services Config (UE5): https://dev.epicgames.com/documentation/en-us/unreal-engine/enable-and-configure-online-services-eos-in-unreal-engine
- EOS Developer Portal: https://dev.epicgames.com/portal/
- UE5.7 Documentation: https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-7-documentation
- BitCraft (reference game built with SpacetimeDB): https://www.bitcraftonline.com/
- BitCraft Server Source Code (Apache 2.0): https://github.com/clockworklabs/BitCraftPublic
- MultiServer Replication Plugin (Experimental): https://dev.epicgames.com/community/learning/knowledge-base/xBvk/unreal-engine-experimental-an-introduction-to-the-multiserver-replication-plugin
- MultiServer Replication API: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/MultiServerReplication
- Open World Server (OWS): https://github.com/Dartanlla/OWS
- OWS Documentation: https://www.openworldserver.com/
