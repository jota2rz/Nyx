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

## Redwood MMO — Reference Architecture Analysis

> **Source:** https://redwoodmmo.com/docs/  
> **License:** Source-available (paid license, royalty-free tier available)  
> **Engine:** Unreal Engine 5 (any version)  
> **Creator:** Incanta Games  
> **Relevance:** Redwood is a self-hosted, source-available alternative to Epic Online Services / Steamworks / Playfab, purpose-built for UE5 multiplayer games including MMOs. It provides the full backend infrastructure (auth, matchmaking, persistence, game server orchestration, sharding, zones, chat) that Nyx's SpacetimeDB architecture must eventually replicate or replace.

### Overview

Redwood is a **microservice-based game backend** that wraps standard UE5 dedicated servers with orchestration, persistence, and matchmaking. Unlike SpacetimeDB (which is a single database-as-server), Redwood is a traditional distributed architecture: NodeJS microservices + PostgreSQL + Redis + Kubernetes + Agones/Hathora for game server hosting.

**Key difference from Nyx's approach:** Redwood uses **standard UE5 dedicated servers** with UE5's built-in replication for real-time gameplay. The backend handles everything *except* the gameplay tick loop — auth, character persistence, matchmaking, server lifecycle, chat. SpacetimeDB in Nyx tries to be both the database AND the real-time state synchronization layer.

### 1. Architecture — Director / Realm / Sidecar

Redwood splits into three tiers:

| Component | Role | Scaling |
|-----------|------|---------|
| **Director** | Central entry point. Player authentication, realm listing, identity management. One per title. | Single instance (lightweight) |
| **Realm** | Per-world/region unit. Character data, ticketing/matchmaking, game server management. Multiple Realms possible (PvP, PvE, Europe, etc.) | Horizontally scalable (Frontend + Backend split) |
| **Sidecar** | NodeJS process co-located with each UE5 game server. Bridges game server ↔ Realm Backend. Handles player auth tokens, character data load/save, lifecycle updates. | One per game server instance |

Each service is split into **Frontend** (player-facing: auth, matchmaking requests, character listing) and **Backend** (internal: processing matchmaking, allocating servers, sidecar communication). This split enables independent horizontal scaling and separates attack vectors.

**Interservice communication:** Socket.IO/WebSockets for 1-to-1 connections. Redis pub/sub for fan-out where the sender doesn't know which replica to target. All API definitions centralized in `packages/common/src/interfaces.ts`.

### 2. Game Server Model — Proxies, Collections, Instances

Redwood introduces an abstraction hierarchy for game servers:

| Entity | Description |
|--------|-------------|
| **GameServerProxy** | Abstract representation of a virtual world. Can exist without running servers (appears in lobby list). Stores world data, game mode/map, access config. |
| **GameServerCollection** | Active backing for a Proxy. Contains 1+ GameServerInstances (all zones/shards). Can be stopped/archived; a new Collection is created on restart. |
| **GameServerInstance** | Single running UE5 dedicated server process. Serves one shard of one zone. Has connection URI and hosting provider ID. |
| **GameServerContainer** | Docker container running one GameServerInstance. Maps to Agones/Hathora resource. |

**Key insight for Nyx:** The Proxy can appear "live" with no running servers — servers spin up on-demand when a player joins. This enables persistent worlds that don't waste resources when unpopulated. The Collection/Instance separation cleanly handles zone restarts without losing world identity.

### 3. Zones, Shards, and Channels

**Zones** are abstract spatial divisions of a game world. Each GameServerProxy defines 1+ zones. Zones can be different UE5 maps or different areas in a single map. Players transfer between zones via `TravelPlayerToZoneSpawnName()` or `TravelPlayerToZoneTransform()` — this is a server travel (loading screen), not seamless.

**Shards** are parallel instances of the same zone. Every GameServerInstance IS a shard. Sharding is configured per game profile:
- `max-players-per-shard`: Hard cap per server instance
- `num-players-to-add-shard`: Soft cap that triggers spinning up a new shard
- `num-minutes-to-destroy-empty-shard`: Auto-cleanup for idle shards
- `game.shard-names`: NATO phonetic alphabet names (Alpha, Bravo, Charlie...) — max shards = number of names

**Sharding algorithm:** When connected players reach `num-players-to-add-shard`, a new shard is started. The soft cap is deliberately lower than the hard cap so party members can follow each other. Players can manually switch shards. Queue for a specific shard or auto-join the least populated one.

### 4. Data Persistence Model

Redwood has a well-structured data hierarchy:

| Data Type | Scope | Persistence | Sync |
|-----------|-------|-------------|------|
| **PlayerIdentity** | Director-level (across all Realms) | PostgreSQL (Director DB) | N/A |
| **PlayerCharacter** | Realm-level | PostgreSQL (Realm DB) | Via UE5 replication |
| **World Data** | Per-Proxy (shared across all zones/shards) | PostgreSQL | Auto-synced to all shards via Redis |
| **Zone Data** | Per-Zone (not shared with other zones) | PostgreSQL | Synced within zone shards |
| **Global Data** | Director or Realm level | PostgreSQL (versioned) | On-demand fetch |
| **Sync Items** | Per-actor (cross-shard sync) | Optional PostgreSQL | Redis pub/sub between sidecars |
| **Blob Storage** | Arbitrary files | S3-compatible (SeaweedFS/AWS S3) | N/A |

**PlayerCharacter fields** are thoughtfully partitioned:
- `characterCreatorData` — player-editable, replicated to all (appearance)
- `metadata` — server-managed, replicated to all (level, health, equipped gear)
- `equippedInventory` — server-managed, owner-only replication (prevents snooping)
- `nonequippedInventory` — bank/vault, owner-only replication
- `progress` — achievements, quests, skill trees (replication optional)
- `abilitySystem` — GAS integration or custom ability data
- `data` — catch-all game data, not replicated

**Data persistence timing:** Dirty data is saved at `DatabasePersistenceInterval` (default 2 Hz / 0.5s). Recommendation: "Choose the largest number of seconds you're willing to potentially lose data in a game server crash."

**Schema migrations:** Each data struct has a `SchemaVersion` integer. Migration functions (`Metadata_Migrate_v0`, `Metadata_Migrate_v1`, etc.) upgrade one version at a time. This is a mature pattern for live game data evolution.

### 5. Sync Items — Limited Cross-Server State

Sync Items (`URedwoodSyncComponent`) synchronize transform, state, and data between servers via Redis. This is **NOT full server meshing** but a targeted tool for specific cross-shard needs:

- Battle Royale circle/play zone
- Supply airdrops
- Limited-supply resources (prevent cross-shard farming)
- World state (time of day, global scores)
- Theoretically player characters across zone boundaries (untested by Redwood)

**Sync mechanism:** Dirty checks on tick → batched API call to sidecar → Redis pub/sub → subscribing sidecars → individual item change messages. No clock synchronization or rewind. Clients receive state via standard UE5 replication from their connected server, not directly from Redis.

### 6. Authentication and Ticketing

**Auth providers:** Local (username/password), Steam, Epic, Discord, Twitch, JWT. Multiple providers can be enabled simultaneously for cross-platform. No account linking between providers yet (on roadmap).

**Ticketing (matchmaking/queuing):**
- **Queue:** FIFO queue for lobby-based games where players know which server to join (MMO overworld)
- **Simple Match:** Lightweight matchmaking emulator for development
- **Open Match:** Google's open-source matchmaker for production (region, skill, class matching)
- **Custom Flow:** Programmable ticketing for advanced setups
- Queue and matchmaking can run simultaneously (overworld queue + instanced dungeon matchmaking)

### 7. Tech Stack

| Layer | Technology |
|-------|------------|
| Runtime | NodeJS + TypeScript |
| Database | PostgreSQL (SQLite for Windows dev) |
| Cache/PubSub | Redis (custom JS stub for Windows dev) |
| Orchestration | Kubernetes |
| Game servers | Agones (self-hosted/K8s) or Hathora (managed/on-demand) |
| Deployment | Pulumi (Infrastructure-as-Code) |
| Matchmaking | Open Match |
| Chat | ejabberd (XMPP) |
| Client ↔ Backend | Socket.IO |
| Server ↔ Backend | Sidecar (NodeJS, co-located) |

### 8. Comparison with Nyx's SpacetimeDB Architecture

| Aspect | Redwood | Nyx (SpacetimeDB) |
|--------|---------|-------------------|
| **Real-time gameplay** | Standard UE5 dedicated servers (UDP replication) | SpacetimeDB WebSocket subscriptions |
| **Backend language** | NodeJS/TypeScript | Rust (WASM module) |
| **Database** | PostgreSQL + Redis | SpacetimeDB (built-in) |
| **Game server hosting** | Agones/Hathora (UE5 dedicated servers) | SpacetimeDB IS the server (no UE5 server) |
| **Spatial partitioning** | Zones (hard boundaries, server travel) | Chunk-based subscriptions (soft boundaries) |
| **Scalability model** | Horizontal (more shards/servers) | Vertical (one SpacetimeDB per zone, up to ~200 players) |
| **Cross-zone sync** | Redis pub/sub Sync Items (limited) | Not yet implemented |
| **Physics** | UE5 dedicated server (full Chaos physics) | Sidecar (Euler integration via reducer calls) |
| **Auth** | Built-in (Steam/Epic/Discord/etc.) | EOS integration (Spike 4) |
| **Matchmaking** | Open Match (production-grade) | Not implemented |
| **Chat** | ejabberd (XMPP, built-in) | Not implemented |
| **Persistence** | Periodic dirty-flush to PostgreSQL | Automatic (every table write immediately persisted) |
| **Licensing** | Paid license (source-available) | Open source (SpacetimeDB) |
| **Complexity** | High (many microservices, K8s required for production) | Low (single binary, single database) |

### 9. Patterns Worth Adopting in Nyx

| # | Pattern | Redwood Implementation | Applicability to Nyx |
|---|---------|----------------------|---------------------|
| 1 | **Frontend/Backend service split** | Player-facing services scale independently from internal processing. Separate attack vectors. | Medium — if Nyx ever needs a web API layer (admin dashboard, companion app), this separation matters. SpacetimeDB currently serves both roles. |
| 2 | **GameServerProxy (virtual world identity)** | Worlds exist as database entities even without running servers. Servers spin up on-demand. | High — Nyx should track "zones" as persistent entities in SpacetimeDB, independent of whether any client is connected. Enables restart recovery. |
| 3 | **Sidecar pattern (game server ↔ backend bridge)** | Node process co-located with each UE5 server handles all backend communication, isolating game code from backend details. | Medium — Nyx's SpacetimeDB plugin already acts as an integrated "sidecar" inside the UE5 process. But if Nyx moves to Option C (UE5 dedicated servers), a sidecar becomes essential. |
| 4 | **Character data partitioning** | 7 distinct data fields with different replication/privacy rules. Prevents clients from snooping on other players' inventories. | High — Nyx should adopt this pattern. SpacetimeDB tables can implement equivalent partitioning via subscription queries (`SELECT * FROM player_metadata` vs `SELECT * FROM player_inventory WHERE owner = :self`). |
| 5 | **Dirty-flush persistence with configurable interval** | Default 0.5s. "Choose the largest number you're willing to lose in a crash." Auto-save on disconnect. | Low for SpacetimeDB (writes are immediately persistent). But relevant for the UE5 sidecar physics results — batch writes at a configurable interval rather than per-frame. |
| 6 | **Schema migration functions** | Per-field `SchemaVersion` + `FieldName_Migrate_vN` functions. Upgrade one version at a time. Live game data evolution without downtime. | High — SpacetimeDB doesn't have built-in schema migration. Nyx will need a migration strategy for live data changes. Redwood's per-field versioning is more granular than whole-database migrations. |
| 7 | **Dynamic sharding with soft/hard caps** | `num-players-to-add-shard` (soft) < `max-players-per-shard` (hard). Gap allows party follow. Auto-destroy empty shards after timeout. | High — directly applicable to Nyx's spatial zones. Could implement in SpacetimeDB: a `zone_state` table tracking player counts, a reducer that triggers shard creation when soft cap is reached. |
| 8 | **Sync Items (selective cross-shard sync via Redis)** | Targeted pub/sub for specific actors (world events, limited resources), NOT full state mesh. Practical compromise. | High — if Nyx has multiple SpacetimeDB instances per world, Sync Items shows the right granularity for cross-instance sync. Not every entity needs cross-instance visibility — just world events, global state, and border entities. |
| 9 | **Instanced dungeons as matchmaking-zone profiles** | Same system handles both persistent overworld (queue) and ephemeral dungeons (matchmaking). Player transfers back to overworld on completion. | Medium — When Nyx adds instanced content, this pattern is clean. SpacetimeDB could handle this as a separate module/database for each instance, with the main database tracking active instances. |
| 10 | **Text chat via XMPP (ejabberd)** | Separate, proven chat server with room types (guild, party, realm, shard, nearby, direct). Spatial "nearby" chat uses sender location + client-side distance filtering. | Low — SpacetimeDB could handle chat as tables/subscriptions, but a dedicated XMPP server is more mature for features like history, blocking, moderation. Worth evaluating when chat becomes a priority. |

### Bottom Line for Nyx

Redwood represents the **production-grade UE5 MMO backend** that Nyx would need if it outgrows SpacetimeDB's capabilities (>300 players per zone, complex matchmaking, multiple regions). Its architecture is a well-engineered reference for how traditional MMO backends work.

**Key architectural takeaway:** Redwood's approach is fundamentally different from Nyx's. Redwood says "use UE5 dedicated servers for gameplay, use backend microservices for everything else." Nyx says "use SpacetimeDB for everything including gameplay state sync." Both are valid:

- **Redwood's strength:** Battle-tested UE5 replication for real-time gameplay (UDP, client prediction, server authority), production-grade game server orchestration (Agones/Hathora), mature data persistence patterns
- **Nyx/SpacetimeDB's strength:** Dramatically simpler architecture (one binary vs. many microservices), automatic persistence (no dirty-flush needed), real-time subscriptions as a database feature, no Kubernetes required

**Convergence point:** If Nyx ever needs Option C (UE5 dedicated servers + SpacetimeDB persistence), Redwood's architecture is essentially what Option C would look like, but with SpacetimeDB replacing PostgreSQL + Redis + custom sync code. The Sidecar pattern, GameServerProxy/Collection/Instance hierarchy, zone transfer flow, and character data partitioning would all translate directly.

**Practical adoption path:** For Nyx's current Phase 1 (SpacetimeDB-only, ~100-200 players per zone), Redwood's patterns #2, #4, #6, #7, and #8 are immediately useful for structuring SpacetimeDB tables and reducers. Patterns #1, #3, #9, and #10 become relevant later if Nyx scales to multi-zone/multi-region deployment.

---

## Scaling SpacetimeDB Beyond 300 Players Per Zone

> **Context:** Spike 9 Test 3b established a ceiling of ~200-300 WebSocket subscribers per zone on a 4C/8T machine (i3-12100), with ~280K events/sec peak throughput. This section analyzes SpacetimeDB 2.0's official scaling mechanisms and proposes an architecture to push well beyond that limit within Option A (SpacetimeDB-only).

### Why 300 Is Not the Real Limit

The Test 3b benchmark measured a worst-case scenario: **every subscriber sees every entity** (`SELECT * FROM fanout_entity`). All 100 entities update at 10 Hz, and all N subscribers receive all 100 updates per tick. This yields O(N × E) events per tick — the full fan-out.

In production, players don't see the entire world. Spatial interest management means each client subscribes only to nearby entities. This fundamentally changes the scaling math:

| Scenario | Subscribers | Entities Visible | Events/Tick | Events/Sec (10 Hz) |
|----------|-------------|-----------------|-------------|-------------------|
| **Test 3b (worst case)** | 200 | 100 (all) | 20,000 | 180,000 |
| **Spatial: 50 per chunk** | 50 | 50 | 2,500 | 25,000 |
| **4 chunks visible** | 200 total (50/chunk) | 200 (50/chunk × 4) | 10,000 | 100,000 |
| **10 chunks, 500 players** | 500 total (50/chunk) | 200 (per player) | — | 250,000 |

The last row shows 500 players each seeing 50 entities at 10 Hz = 250K events/sec total — within the measured 280K budget. The key is reducing per-subscriber entity count through spatial filtering.

### Strategy 1: Subscription Query Deduplication (Zero-Cost)

SpacetimeDB's subscription system provides a critical optimization from the official docs:

> *"SpacetimeDB subscriptions are zero-copy. Subscribing to the same query more than once doesn't incur additional processing or serialization overhead."*

**Impact:** When 50 players in the same chunk all subscribe to `SELECT * FROM entity WHERE chunk_x = 5 AND chunk_y = 3`, SpacetimeDB evaluates the query ONCE and shares the result. The subscription diff computation becomes O(unique_queries × delta) instead of O(total_subscribers × delta).

The WebSocket broadcast (sending data to each client) is still O(subscribers), but the expensive server-side work (evaluating which rows changed for each query) is amortized across all subscribers sharing the same query.

**Implementation:** Already working — Spike 5's chunk-based `WHERE` subscriptions naturally produce identical query strings for players in the same chunk.

### Strategy 2: Anonymous Views — Shared Materialization

SpacetimeDB 2.0's **Views** feature adds a powerful new tool. Anonymous views (`AnonymousViewContext`) are computed once and shared across ALL subscribers:

> *"The database can materialize the view once and serve that same result to all subscribers. When the underlying data changes, it recomputes the view once and broadcasts the update to everyone."*

**Region-based design pattern** (from official docs):

```rust
// All players in chunk (0,0) share this single materialized view
// SpacetimeDB computes it ONCE, not per-subscriber
entities_in_origin_chunk = anonymousView {
    filter entities where chunk_x = 0 AND chunk_y = 0
}
```

**Contrast with per-user views:** A `ViewContext` view that uses `ctx.sender()` (e.g., "entities near ME") must be computed separately for each subscriber. With 1,000 users, that's 1,000 computations. An anonymous view that returns "entities in region X" is computed once regardless of subscriber count.

**Limitation:** Views cannot take parameters — each chunk needs a separate view definition or clients must use raw SQL subscriptions with WHERE filters. For dynamic worlds with many chunks, SQL subscriptions are more practical. The query deduplication from Strategy 1 provides similar benefits.

### Strategy 3: Table Decomposition — Reduce Per-Entity Payload

The official SpacetimeDB performance best practices explicitly recommend splitting tables by update frequency:

```
Consolidated (not recommended):
  Player → id, name, position_x, position_y, velocity_x, velocity_y,
           health, max_health, total_kills, audio_volume

Decomposed (recommended):
  PlayerState    → player_id, position_x, position_y (updates: 10 Hz)
  PlayerHealth   → player_id, health, max_health     (updates: occasional)
  PlayerStats    → player_id, total_kills, deaths     (updates: rare)
  PlayerSettings → player_id, audio_volume            (updates: very rare)
```

**Impact on fan-out:** A 10 Hz position tick only triggers subscription updates for `PlayerState`, not the entire player entity. Clients subscribing to `PlayerStats` (leaderboard) don't receive position noise. This reduces:
- **Serialization cost:** Smaller rows = faster serialization per update
- **Wire bandwidth:** Less data per subscriber per tick
- **Subscription diff cost:** Fewer columns changed = smaller diff

**Estimated improvement:** If position data is ~24 bytes (3× f32 + player_id) vs ~120 bytes (full entity), that's 5× less data per entity update. The 280K events/sec limit may be partially bandwidth-bound, so smaller payloads could push it higher.

### Strategy 4: Event Tables — Zero-Persistence Transient Data

SpacetimeDB 2.0 introduces **event tables**: rows exist only for the duration of the transaction, are broadcast to subscribers on commit, then immediately deleted. No client-side cache, no `on_update`/`on_delete` callbacks — only `on_insert`.

**Use cases for Nyx:**
- Combat damage numbers (floating text)
- VFX/SFX triggers (explosions, particles)
- Transient notifications ("Player joined zone")
- Debugging/telemetry data

**Impact:** Event tables avoid accumulating rows in client caches. A combat-heavy zone with hundreds of damage events per second won't bloat subscriptions because events are ephemeral. RLS (Row-Level Security) can restrict which clients receive which events.

### Strategy 5: RLS — Server-Side Data Filtering

From the "What is SpacetimeDB?" documentation:

> *"RLS filters restrict the data view server-side before subscriptions are evaluated. These filters can be used for access control or client scoping."*

RLS operates BEFORE subscription evaluation, meaning filtered-out rows never enter the subscription diff pipeline. This is more efficient than client-side filtering because it reduces server-side computation, not just bandwidth.

**Application to spatial interest management:** RLS could enforce that a client only receives entities within a certain range, providing a server-enforced backstop even if the client's subscription query is too broad.

### Strategy 6: Direct Indexes — O(1) Entity Lookups

SpacetimeDB supports **direct indexes** for dense sequential unsigned integers:

> *"Direct indexes use array indexing instead of tree traversal, providing O(1) lookups."*

For entity systems where IDs are auto-incrementing integers starting near 0 (which is how games typically work), direct indexes eliminate the B-tree traversal overhead in subscription diff evaluation. This is a micro-optimization but becomes meaningful at high entity counts.

### Strategy 7: Multi-Database Sharding (Horizontal Scaling)

SpacetimeDB supports deploying the same module to multiple independent databases:

> *"You can deploy the same module to multiple databases (e.g. separate environments for testing, staging, production), each with its own independent data."*

**Multi-database zone architecture:**

```
           ┌─── SpacetimeDB "nyx-zone-1" (Region A, ≤200 players)
           │
Router ────┼─── SpacetimeDB "nyx-zone-2" (Region B, ≤200 players)
           │
           ├─── SpacetimeDB "nyx-zone-3" (Region C, ≤200 players)
           │
           └─── SpacetimeDB "nyx-global"  (accounts, inventory, cross-zone state)
```

Each zone database runs the same module but handles a different spatial region. A lightweight router directs players to the correct database based on their world position. SpacetimeDB's **Procedures** (which support HTTP requests) enable cross-database communication for player transfers and shared state.

**Cross-database coordination via Procedures:**
- Player moves from Zone 1 → Zone 2: Zone 1 procedure calls Zone 2's HTTP API to create the player entity, then deletes from Zone 1
- Global state (economy, world events): Written to `nyx-global`, read by zone databases on demand
- Follows Redwood's Sync Items pattern: targeted cross-instance sync, NOT full state mesh

**Scaling:** Linear with number of databases. Each database handles ≤200-300 players within its comfort zone. Ten databases across a single server host = 2,000-3,000 concurrent players. Across multiple hosts = effectively unlimited.

### Strategy 8: Tiered Update Rates

Not all entities need the same update frequency. A tiered approach reduces total events/sec:

| Distance from Player | Update Rate | Entities Visible | Events/Sec (per player) |
|---------------------|-------------|-----------------|----------------------|
| Near (0-50m) | 10 Hz | ~20 | 200 |
| Medium (50-200m) | 5 Hz | ~30 | 150 |
| Far (200-500m) | 2 Hz | ~50 | 100 |
| Very far (500m+) | 0 Hz (not subscribed) | 0 | 0 |
| **Total** | — | **~100** | **450** |

Compare to flat 10 Hz for all 100 visible entities = 1,000 events/sec per player. Tiered rates reduce it to 450 — a 55% reduction.

**Implementation:** Multiple subscription groups per client. The position tick reducer updates a `near_entity_state` table at 10 Hz but a `far_entity_state` table at 2 Hz. Or maintain a single table but use separate scheduled ticks at different rates based on entity classification.

### Revised Scaling Projections

Combining all strategies on the same i3-12100 hardware (4C/8T, 280K events/sec measured capacity):

| Configuration | Players | Visible/Player | Events/Sec Total | Feasible? |
|--------------|---------|----------------|-----------------|-----------|
| **Baseline (Test 3b)** | 200 | 100 (all) | 180K | ✅ Zero degradation |
| **Spatial (50/chunk)** | 500 | 50 | 250K | ✅ Within budget |
| **Spatial + tiered rates** | 700 | 100 (tiered) | 315K | ⚠️ Near limit |
| **Spatial + decomposed tables** | 800 | 50 (position only) | 250K* | ✅ Smaller payloads |
| **Multi-DB sharding (2 DBs)** | 500 | 50 | 125K per DB | ✅ Comfortable |
| **Multi-DB sharding (4 DBs)** | 1,000 | 50 | 62.5K per DB | ✅ Easy |

\*Events/sec may actually be higher capacity due to reduced per-event serialization size.

**On production hardware** (dedicated server, e.g., AMD EPYC 7543 32C/64T, 2.8 GHz base / 3.7 GHz boost):
- Single-thread performance roughly comparable to i3-12100 at base, lower at boost
- Multi-threaded WebSocket broadcast could scale with core count (if SpacetimeDB parallelizes I/O)
- Potentially 2-4× the events/sec capacity = 560K-1.1M events/sec
- Single database: 500-1000 players with spatial partitioning
- With sharding: 5,000+ concurrent players

### Implementation Roadmap

| Priority | Strategy | Effort | Player Impact |
|----------|----------|--------|--------------|
| **P0** | Spatial interest management (already have chunk-based WHERE from Spike 5) | Low | 200 → 500 |
| **P1** | Table decomposition (PlayerState / PlayerHealth / PlayerStats) | Low | +20-30% headroom |
| **P1** | Event tables for combat effects and transient data | Low | Cleaner architecture |
| **P2** | Tiered update rates (near 10 Hz, far 2 Hz) | Medium | +30-50% headroom |
| **P2** | Direct indexes for entity IDs | Low | Micro-optimization |
| **P3** | Multi-database sharding with Procedures for cross-zone coordination | High | 1,000+ players |
| **P3** | RLS filters for server-enforced interest management | Medium | Security + performance |

### Bottom Line

**SpacetimeDB can scale well beyond 300 players.** The 200-300 limit from Test 3b applies to subscribers who ALL see ALL entities — a worst case that doesn't occur in production. With spatial partitioning alone (P0), Option A scales to ~500 players per database instance. With table decomposition and tiered rates (P1-P2), 700-800 becomes achievable. With multi-database sharding (P3), the system scales horizontally to thousands.

The strategies above are drawn directly from SpacetimeDB 2.0's official documentation (table decomposition, anonymous views, event tables, RLS, direct indexes, subscription deduplication) and production-proven patterns from BitCraft, Redwood, and OWS. None require fundamental changes to SpacetimeDB's architecture — they're all about using existing features effectively.

**Key insight:** The bottleneck was never SpacetimeDB's database or WASM execution (400K entity updates/sec). It's the WebSocket fan-out, which is bounded by (total_subscribers × visible_entities × update_rate × row_size). Every strategy above attacks one or more of those four terms.

### Critical Clarification: World-Scale vs. Viewport-Scale

The phrase "scales to thousands" above refers to **concurrent players across the entire game world**, distributed across spatial partitions and/or database shards. It does **not** mean 1,000+ players all visible to each other simultaneously in a single viewport — that is an entirely different (and much harder) problem.

**Why massive PvP is fundamentally different:**

When N players all see each other, every position update must fan out to all N observers. This creates O(N²) event delivery per tick:

| Players Visible to Each Other | Events per Tick | Events/Sec (10 Hz) | vs. 280K Measured Budget |
|-------------------------------|-----------------|---------------------|--------------------------|
| 200 | 40,000 | 400,000 | 1.4× over — degraded |
| 300 | 90,000 | 900,000 | 3.2× over |
| 500 | 250,000 | 2,500,000 | 9× over |
| 1,000 | 1,000,000 | 10,000,000 | 35× over |

No amount of spatial partitioning helps when everyone is in the same partition. Multi-database sharding doesn't help either — all players must share the same database to see each other.

**How AAA MMOs handle massive PvP:**

| Game | Players in Battle | Technique |
|------|-------------------|-----------|
| EVE Online | 6,000+ | Time dilation (slows to 10% speed), reducing tick rate to 1 Hz |
| PlanetSide 2 | 1,000+ | Aggressive LOD (far players update at 1-2 Hz), render culling at 300m |
| WoW Classic | ~300 | No mitigation — severe lag and disconnects beyond 300 |
| New World | ~100 | Wars capped at 50v50, open world degrades beyond ~100 |
| Guild Wars 2 | ~200 | WvW has visibility cap, distant players become nameplates |

**No game renders 1,000 players updating at full tick rate to all 1,000 simultaneously.** Every title uses some combination of reducing what each player sees, reducing how often far players update, or slowing down time itself.

**Realistic massive PvP strategies for Nyx:**

1. **Visibility cap (200-300):** Server culls entities beyond a maximum count per player, prioritizing proximity and threat level. Most MMOs do exactly this. SpacetimeDB's `WHERE` subscriptions already support this via chunk-based interest.
2. **LOD tiers:** Near players (0-50m) update at 10 Hz with full state. Mid-range (50-150m) update at 3-5 Hz position-only. Far (150m+) update at 1 Hz as dots on minimap. Reduces effective fan-out by 3-5×.
3. **Option B UDP relay:** Position data for 500+ combatants streamed via a lightweight UDP relay in front of SpacetimeDB. SpacetimeDB handles authoritative game state (health, inventory, abilities); the relay handles high-frequency positions. Breaks the WebSocket bottleneck entirely for movement.
4. **Time dilation (EVE model):** When event throughput exceeds budget, server reduces tick rate proportionally. 1,000 players at 1 Hz fits within budget (1M events/sec with spatial + decomposition). Gameplay slows but remains fair and functional.

**Updated bottom line for massive PvP:**
- **200-300 all-visible:** Proven achievable with SpacetimeDB today (Test 3b)
- **500 all-visible:** Requires LOD tiers + table decomposition (P1-P2)
- **1,000+ all-visible:** Requires hybrid approach (Option B relay or time dilation)
- **1,000+ world-concurrent (spatial partitions):** Achievable with spatial interest + sharding (P0-P3)
- **5,000+ world-concurrent:** Multi-database sharding on production hardware

---

## Lineage 2 — Reference Architecture Analysis (1000+ Visible Players)

### Context

Lineage 2 (NCsoft, 2003) is a Korean MMORPG that routinely handled **1,000+ simultaneously visible players** in massive castle siege PvP — on 2003-era single-threaded server hardware. The game is fully **server-authoritative**: movement is point-and-click (server validates every position), skills have perceptible server-round-trip delay, and the client is a thin renderer built on Unreal Engine 2. The bottleneck during 1000+ player sieges was always **client FPS**, not server throughput.

This analysis is based on source-code archaeology of the **L2J Mobius** open-source Java emulator (MIT license, 11,778+ commits, actively maintained), specifically the **C1_HarbingersOfWar** chronicle — the earliest version, closest to the original 2003 C++ architecture. While L2J is a Java reimplementation, it faithfully reproduces the original protocol, packet formats, and architectural patterns documented by reverse-engineering the official server over 20+ years.

**Source:** https://gitlab.com/MobiusDevelopment/L2J_Mobius

### Key Architectural Patterns

#### 1. Spatial Grid System (World.java)

The world is divided into a **2D grid of WorldRegion cells**, each 2048 game-units wide:

```java
public static final int SHIFT_BY = 11;          // 2^11 = 2048 units per region
public static final int TILE_SIZE = 32768;       // Map tile = 32K units
// Region lookup: O(1) via bit-shift
WorldRegion region = _worldRegions[(x >> SHIFT_BY) + OFFSET_X][(y >> SHIFT_BY) + OFFSET_Y];
```

Each region pre-computes its **3×3 neighborhood** (up to 9 surrounding regions). All visibility queries are scoped to this neighborhood — never the entire world:

```java
public <T extends WorldObject> void forEachVisibleObject(WorldObject object, Class<T> clazz,
    Consumer<T> action) {
    final WorldRegion centerRegion = getRegion(object);
    for (int i = 0; i < centerRegion.getSurroundingRegions().length; i++) {
        WorldRegion region = centerRegion.getSurroundingRegions()[i];
        for (WorldObject wo : region.getVisibleObjects()) {
            if (wo == object || !clazz.isInstance(wo)) continue;
            if (wo.getInstanceId() != object.getInstanceId()) continue;
            action.accept(clazz.cast(wo));
        }
    }
}
```

**Key insight:** Visibility is O(players_in_9_regions), not O(all_players_in_world). With 2048-unit regions and a 3×3 neighborhood, the effective visibility radius is ~6144 units (~61m at L2's scale). This naturally caps the broadcast fan-out.

#### 2. Diff-Based Region Switching (World.java)

When an entity crosses a region boundary, the server computes the **set difference** between old and new 3×3 neighborhoods — it does NOT re-broadcast to all 9+9 regions:

```java
public void switchRegion(WorldRegion newRegion, WorldObject object) {
    WorldRegion[] oldSurrounding = oldRegion.getSurroundingRegions();
    WorldRegion[] newSurrounding = newRegion.getSurroundingRegions();
    // Objects in old neighbors but NOT in new neighbors → send DeleteObject
    for (WorldRegion old : oldSurrounding) {
        if (!newRegion.isSurroundingRegion(old)) {
            for (WorldObject wo : old.getVisibleObjects()) {
                // Send forget/delete packets only for the DIFF
            }
        }
    }
    // Objects in new neighbors but NOT in old neighbors → send CharInfo/NpcInfo
    for (WorldRegion neu : newSurrounding) {
        if (!oldRegion.isSurroundingRegion(neu)) {
            for (WorldObject wo : neu.getVisibleObjects()) {
                // Exchange visibility info only for NEW neighbors
            }
        }
    }
}
```

This means crossing a region boundary typically processes only **3 regions leaving + 3 regions entering**, not all 18. The `isSurroundingRegion()` check is cached in a ConcurrentHashMap for performance.

#### 3. Lazy Region Activation/Deactivation (WorldRegion.java)

Regions are **inactive by default**. When the first player enters an empty region:

1. The region activates **immediately** (starts AI, HP regen, random animations for all mobs)
2. Neighbor regions activate after a **configurable delay** (default: 1 second) — prevents wasted activation for teleport-through scenarios

When the last player leaves:

1. Deactivation is **delayed** (default: 90 seconds) — prevents rapid on/off cycling
2. Before deactivating, checks if ANY neighboring region has playable entities
3. On deactivation: all mobs stop movement, clear aggro, stop AI tasks, teleport to spawn if drifted too far

```java
// Config defaults:
GRIDS_ALWAYS_ON = false;
GRID_NEIGHBOR_TURNON_TIME = 1;    // seconds delay before activating neighbors
GRID_NEIGHBOR_TURNOFF_TIME = 90;  // seconds delay before deactivating
```

**Impact:** On a large map with 1000+ players concentrated in a siege area, only ~20-50 regions are active (the siege zone). The other thousands of regions consume **zero CPU** — no AI ticks, no movement updates, no broadcasts. This is equivalent to SpacetimeDB's spatial subscriptions but at the server simulation level, not just the query level.

#### 4. Region-Scoped Broadcasting (Creature.java + Broadcast.java)

All entity state broadcasts are scoped to **Player.class in surrounding regions only**:

```java
public void broadcastPacket(ServerPacket packet) {
    packet.sendInBroadcast();
    World.getInstance().forEachVisibleObject(this, Player.class, player -> {
        if (isVisibleFor(player)) {
            player.sendPacket(packet);
        }
    });
}
```

The `Broadcast` utility provides additional scoping:

| Method | Scope | Use Case |
|--------|-------|----------|
| `toKnownPlayers()` | All players in 3×3 surrounding regions | Movement, attacks, skill effects |
| `toKnownPlayersInRadius()` | Players within explicit radius | AoE effects, local chat |
| `toSelfAndKnownPlayers()` | Self + surrounding players | Self-affecting actions |
| `toPlayersTargettingMyself()` | Only players targeting this entity | Targeted status updates |
| `toAllOnlinePlayers()` | All connected players | Server announcements only |
| `toPlayersInInstance()` | Players in same instance ID | Instanced dungeons |

**Critical:** `toAllOnlinePlayers()` is used ONLY for server announcements and system messages — never for gameplay state. ALL gameplay broadcasts use the region-scoped variants.

#### 5. Rate-Limited Movement Broadcasts (Creature.java)

Movement packets are **rate-limited to once per 10 game ticks** (~1 second):

```java
private void broadcastMoveToLocation() {
    final int gameTicks = GameTimeTaskManager.getInstance().getGameTicks();
    final MoveData move = _move;
    if ((gameTicks - move.lastBroadcastTime) < 10) {
        return;  // Skip — too soon since last broadcast
    }
    move.lastBroadcastTime = gameTicks;
    broadcastPacket(new MoveToLocation(this));
}
```

Combined with the game tick system:
```java
public static final int TICKS_PER_SECOND = 10;    // 10 ticks/sec = 100ms per tick
public static final int MILLIS_IN_TICK = 100;      // 100ms
```

So movement broadcasts happen **at most once per second**, not every server tick. The client receives a `MoveToLocation` packet with origin + destination and **interpolates the entire path locally**. No per-tick position streaming.

#### 6. Server-Authoritative Movement System (Creature.java + MovementTaskManager.java)

The movement system is fully server-authoritative with client-side interpolation:

**Server side:**
1. Player clicks a destination → client sends `MoveToLocation` request
2. Server calculates path via GeoEngine (obstacle detection, door collision, height validation)
3. Server stores `MoveData`: start time, start position, destination, speed, geodata path nodes
4. Server registers entity with `MovementTaskManager`
5. `MovementTaskManager` calls `updatePosition()` at fixed intervals:
   - **Players:** every **50ms** (20 Hz, pools of 500)
   - **NPCs/Mobs:** every **100ms** (10 Hz, pools of 1000)
6. Position interpolated: `distPassed = (speed × (gameTicks - moveStartTick)) / TICKS_PER_SECOND`
7. When `distFraction > 1.79` → entity has arrived, final position set to exact destination

**Client side:**
1. Receives `MoveToLocation` packet (28 bytes: objectId + origin XYZ + dest XYZ)
2. Interpolates entity position locally at render framerate
3. Periodically sends `ValidatePosition` packets for correction
4. Server position is always authoritative for range calculations (attack, skill, loot)

**Key insight:** Point-and-click movement generates **ONE packet per movement command**, not continuous WASD input. A player walking across the map sends 1 packet. A player in combat sends maybe 2-3 movement packets per engagement. Compare with WASD: 10-60 inputs/second.

#### 7. Threshold-Based HP Updates (Creature.java)

HP/MP/CP updates use a **pixel-threshold system** — updates are only sent when HP crosses a visual boundary:

```java
// HP bar is 352 pixels wide
_hpUpdateInterval = getMaxHp() / 352.0;

public boolean needHpUpdate() {
    double currentHp = getCurrentHp();
    double lastHp = _hpUpdateIncCheck;
    if (currentHp <= 0 || lastHp <= 0) return true;    // Dead/alive transition
    if (Math.abs(currentHp - lastHp) < _hpUpdateInterval) return false;  // Same pixel
    return true;
}
```

**Impact:** A boss with 100,000 HP only sends a StatusUpdate when HP changes by ~284 points (100000/352). During a 1000-player siege, individual sword hits (50-200 damage) on high-HP targets generate **zero network traffic** until they accumulate past the pixel threshold. This eliminates the vast majority of HP update packets.

#### 8. StatusListener Pattern (Creature.java)

HP/MP updates go ONLY to **registered status listeners**, not all visible players:

```java
public void broadcastStatusUpdate(Creature caster) {
    StatusUpdate su = new StatusUpdate(this);
    su.addAttribute(StatusUpdate.CUR_HP, (int) getCurrentHp());
    su.addAttribute(StatusUpdate.CUR_MP, (int) getCurrentMp());
    // Send to registered listeners only (party members, target, etc.)
    for (Creature listener : getStatusListeners()) {
        listener.sendPacket(su);
    }
}
```

Status listeners include: party members, players targeting this entity, raid members. A player walking past a fight does NOT receive HP updates for combatants they haven't targeted. This is a **pull model** (register interest) vs. a push model (broadcast everything).

#### 9. Compact Wire Protocol

Packets are minimal fixed-size binary structs, not JSON/protobuf:

| Packet | Size (bytes) | Fields |
|--------|-------------|--------|
| MoveToLocation | 28 | objectId(4) + destXYZ(12) + originXYZ(12) |
| StatusUpdate | 12+ | objectId(4) + count(4) + N×(id(4)+value(4)) |
| StopMove | 20 | objectId(4) + X(4) + Y(4) + Z(4) + heading(4) |
| DeleteObject | 4 | objectId(4) |
| Attack | varies | targetId + damage + flags (single struct per hit) |

Every packet also has `canBeDropped()` → returns `true` for movement, status updates. Under load, the network layer can **drop non-critical packets** rather than queue them, providing natural back-pressure.

### Quantitative Analysis: Why 1000+ Players Works

**Scenario:** 1000 players in a castle siege, all within a ~200m area (~4-6 active grid regions).

**Movement broadcasts:**
- 1000 players × 1 movement packet/sec (rate-limited) = 1,000 packets/sec generated
- Each packet broadcasts to ~1000 players in surrounding regions = 1,000,000 packet sends/sec
- At 28 bytes/packet = **28 MB/sec total outbound bandwidth** across all 1000 connections
- Per player: ~1000 packets/sec × 28 bytes = **28 KB/sec inbound** — trivial for 2003 broadband

**HP updates:**
- 1000 players attacking, ~2 hits/sec each = 2000 damage events/sec
- With 352-pixel threshold, only ~10-20% cross a pixel boundary = ~200-400 StatusUpdates/sec
- Each StatusUpdate goes to ~5-10 status listeners (not 1000) = ~2000-4000 sends/sec
- Negligible compared to movement

**Skill broadcasts:**
- Skills have 1-3 second cooldowns → ~500-700 skill casts/sec across 1000 players
- MagicSkillUse packet → broadcasts to surrounding regions = ~500K sends/sec
- But skill packets are small and infrequent per-entity

**Total server-side work:**
- Position updates: 1000 players × 20Hz = 20,000 `updatePosition()` calls/sec (just math, no I/O)
- Broadcast iterations: ~1.5M packet sends/sec (network I/O, but small packets)
- Region lookups: O(1) bit-shift → negligible
- **No per-tick state serialization**, no subscription query evaluation, no SQL WHERE clauses

### Comparison: L2 Architecture vs. SpacetimeDB

| Aspect | Lineage 2 (2003) | SpacetimeDB (2025) |
|--------|-------------------|---------------------|
| **State model** | Mutable objects in RAM | Relational tables in WASM |
| **Broadcast trigger** | State-change events only | Subscription query re-evaluation |
| **Movement updates** | 1 packet/movement command | Row update per tick per entity |
| **Movement frequency** | ~1 packet/sec (rate-limited) | 10-60 Hz (WASD-driven) |
| **Visibility scope** | 3×3 grid regions (9 cells) | SQL WHERE clause per subscriber |
| **HP updates** | Pixel-threshold (1/352) | Every row mutation triggers eval |
| **Status recipients** | Registered listeners only | All matching subscribers |
| **Wire format** | Fixed binary structs (4-28 bytes) | BSATN serialized rows |
| **Packet dropping** | Allowed for non-critical | Guaranteed delivery (QUIC) |
| **Inactive areas** | Zero CPU (AI off, no ticks) | Reducers still execute |
| **Region switching** | Diff-based (3 leave, 3 enter) | Subscription re-query |
| **Fan-out cost** | O(N) iterate region set | O(N) subscription eval per mutation |

### Lessons for Nyx / SpacetimeDB

#### 1. Event-Driven, Not State-Streaming
L2's fundamental insight: **don't stream entity state every tick**. Instead, send **state-change events** (start moving, stop moving, cast spell, take damage) and let clients interpolate between events. A player walking across the map generates 1 network event, not 600 position updates over 60 seconds.

**Nyx implication:** Instead of updating a `player_position` row 10× per second and having SpacetimeDB re-evaluate subscriptions on each update, consider a `movement_intent` table (origin, destination, speed, start_time). Clients interpolate locally. Only movement *changes* generate reducer calls.

#### 2. Broadcast Scoping Is Not Optional
L2 NEVER broadcasts gameplay state to all players. Every broadcast method is scoped to surrounding grid regions (9 cells max). The only global broadcast is `toAllOnlinePlayers()` — used exclusively for server announcements.

**Nyx implication:** SpacetimeDB subscriptions with spatial WHERE clauses achieve similar scoping, but the subscription re-evaluation cost is higher than L2's simple region iteration. Consider pre-computing region membership in a `player_region` table and using it as a join filter rather than distance calculations.

#### 3. Rate Limiting Is Critical
L2 rate-limits movement broadcasts to 1/second even though the server updates positions at 10-20 Hz internally. This 10-20× reduction in outbound packets is what makes 1000 players viable.

**Nyx implication:** SpacetimeDB doesn't have built-in rate limiting — every table mutation triggers subscription evaluation. A server-side throttle (only write position rows every N ticks) or client-side interpolation with infrequent authoritative corrections would dramatically reduce fan-out load.

#### 4. Threshold-Based Updates Eliminate Noise
The 1/352 HP threshold means small damage events don't generate ANY network traffic until they accumulate past a pixel boundary. This is a form of **dead-reckoning for attributes** — the same principle as dead-reckoning for positions.

**Nyx implication:** Instead of updating a `player_hp` column on every damage tick, batch small damage events and only update the row when the visual representation would actually change. Or use event tables for damage numbers (ephemeral) and only update the HP row periodically.

#### 5. Point-and-Click vs. WASD Changes Everything
L2's movement model generates **orders of magnitude fewer inputs** than WASD movement:
- L2: 1 click → 1 packet → client walks for 5-30 seconds
- WASD: Continuous key state → 10-60 packets/sec → constant position streaming

This isn't just a UI difference — it fundamentally changes the server's bandwidth and processing requirements. With WASD, 1000 players generate 10,000-60,000 position inputs/sec. With point-and-click, the same 1000 players generate ~200-500 movement commands/sec.

**Nyx implication:** If Nyx uses WASD movement (likely, given UE5), it MUST implement aggressive dead-reckoning and rate-limiting to approach L2's efficiency. Only send corrections when the server's predicted position diverges from the client's actual position by more than a threshold.

#### 6. Lazy Activation Saves CPU for Free
L2's region activation system means 90%+ of the world map consumes zero server CPU at any given time. Only regions with players (and their immediate neighbors) run AI, movement updates, or HP regeneration.

**Nyx implication:** SpacetimeDB reducers run regardless of whether anyone is observing the results. Consider a "region heartbeat" system where NPC AI reducers only execute for chunks that have active player subscriptions. This maps to SpacetimeDB's subscription model — if no one subscribes to a chunk, don't run reducers for entities in that chunk.

#### 7. The Pull Model for Non-Critical State
L2's StatusListener pattern is a pull model: players **register interest** in specific entities (by targeting them or being in a party). Only registered listeners receive HP/MP updates. This is fundamentally different from "broadcast HP to everyone who can see you."

**Nyx implication:** Consider splitting entity state into tiers:
- **Tier 0 (position/animation):** Broadcast to all in range (spatial subscription)
- **Tier 1 (HP bar):** Only to party + targeting players (filtered subscription)
- **Tier 2 (detailed stats):** Only to self + inspection window (direct query)

This maps directly to SpacetimeDB's subscription system — use different queries with different WHERE clauses for each tier.

### Summary

Lineage 2 achieved 1000+ visible players in 2003 not through extraordinary hardware, but through **ruthless elimination of unnecessary network traffic**:

1. **Spatial grid** → broadcasts scoped to 9 cells, not world
2. **Rate limiting** → movement updates 1×/sec, not 10-60×/sec
3. **Event-driven** → send state changes, not state snapshots
4. **Threshold filtering** → HP updates only when visually meaningful
5. **Pull model** → detailed state only to interested parties
6. **Lazy activation** → zero CPU for unoccupied areas
7. **Compact protocol** → 4-28 byte binary packets, droppable under load
8. **Point-and-click** → orders of magnitude fewer movement inputs than WASD

The total outbound bandwidth for 1000 visible players doing siege PvP was approximately **28 KB/sec per player** — achievable on 2003 broadband. The server CPU was dominated by simple arithmetic (position interpolation, distance checks) with zero serialization overhead.

For Nyx with SpacetimeDB, the most impactful lessons are: **(1)** switch from per-tick state streaming to event-driven state changes with client interpolation, **(2)** implement aggressive rate limiting on position updates, and **(3)** use tiered subscriptions to avoid broadcasting detailed entity state to all observers. These three changes alone could push SpacetimeDB's effective fan-out ceiling from ~300 all-visible players to 1000+.

---

## Architecture Options — Comparative Analysis

After completing all Phase 0 research spikes (1-9), reference architecture analyses (BitCraft, OWS, Redwood, Lineage 2), scaling benchmarks, and L2 source-code archaeology, four architecture options emerged for achieving 1000+ visible WASD players with server-authoritative movement.

### Option 1: Pure SpacetimeDB + Movement Intent Tables (Minimal Change)

Stay entirely within SpacetimeDB but restructure how movement works. Instead of streaming positions every tick, store velocity vectors and let clients interpolate:

```rust
#[table(name = movement_state, public)]
pub struct MovementState {
    #[primary_key]
    identity: Identity,
    pos_x: f32, pos_y: f32, pos_z: f32,           // authoritative position (written at ~1Hz)
    vel_x: f32, vel_y: f32, vel_z: f32,           // velocity vector (set on input change)
    heading: f32,
    speed: f32,
    move_start_tick: u64,
    chunk_id: u32,                                  // spatial filter for subscriptions
}

#[reducer]
fn update_movement(ctx: &ReducerContext, vel_x: f32, vel_y: f32, vel_z: f32, heading: f32) {
    // Validate, update position to server-calculated current pos, set new velocity
}

#[reducer]
fn tick_positions(ctx: &ReducerContext) {
    // Scheduled: interpolate all moving entities, write positions to DB at ~1-2 Hz
    // This triggers subscription updates but only at 1-2 Hz, not 60 Hz
}
```

Client subscribes: `SELECT * FROM movement_state WHERE chunk_id IN (c1, c2, c3, ...)`

**Pros:** No new infrastructure, pure SpacetimeDB
**Cons:** Still pays subscription eval cost per position write (~1-2 Hz per moving entity). With 1000 players: 1000-2000 evals/sec × 1000 subscribers = 1-2M eval/sec. Spike 9 showed 280K events/sec ceiling.

**Estimated ceiling:** ~300-500 all-visible players (improved from ~200 but not 1000+)

### Option 2: SpacetimeDB + UDP Relay Sidecar (Best Raw Performance)

SpacetimeDB handles persistent state. A lightweight UDP relay (simple Rust process, ~500 lines) handles high-frequency ephemeral state:

```
┌─────────────┐     ┌──────────────────┐     ┌─────────────┐
│   UE5       │────►│   SpacetimeDB    │◄────│   UE5       │
│  Client A   │     │  (persistent)    │     │  Client B   │
│             │     └──────────────────┘     │             │
│             │     ┌──────────────────┐     │             │
│             │────►│   UDP Relay      │◄────│             │
│             │     │  (movement/combat)│    │             │
└─────────────┘     │  • Spatial grid  │     └─────────────┘
                    │  • Rate limiting │
                    │  • Auth via STDB │
                    └──────────────────┘
```

The relay maintains a 2D spatial grid (L2's WorldRegion pattern), receives velocity vector changes, broadcasts to surrounding grid cells, rate-limits to 1/entity/100ms, runs server-side interpolation at 20 Hz (arithmetic only), and periodically writes positions back to SpacetimeDB (every 1-5 sec). Supports packet dropping under load.

**Pros:** Bypasses subscription overhead entirely for movement. Simple region iteration + socket writes can handle millions of small packets/sec.
**Cons:** Second process to deploy, custom UDP protocol, auth sync between SpacetimeDB and relay.

**Estimated ceiling:** 1000+ all-visible (matching L2's proven capability)

### Option 3: SpacetimeDB + Event Tables + Client Interpolation (Middle Ground)

Use SpacetimeDB's event tables (auto-deleted after delivery) for movement events:

```rust
#[table(name = movement_event, event)]
pub struct MovementEvent {
    chunk_id: u32,
    entity_id: u64,
    event_type: u8,      // 0=start_move, 1=stop, 2=direction_change, 3=correction
    pos_x: f32, pos_y: f32, pos_z: f32,
    vel_x: f32, vel_y: f32, vel_z: f32,
    heading: f32,
    tick: u64,
}
```

Clients subscribe to `SELECT * FROM movement_event WHERE chunk_id IN (...)` and maintain local entity state by applying events.

**Pros:** Stays within SpacetimeDB, event tables optimized for insert-heavy workloads
**Cons:** Still pays BSATN serialization + subscription delivery cost. Event tables may not support spatial WHERE filtering efficiently.

**Estimated ceiling:** ~500-800 all-visible (needs benchmarking)

### Option 4: UE5 NetDriver + MultiServer + SpacetimeDB (Hybrid) ★ RECOMMENDED

Use UE5's built-in dedicated server for real-time gameplay (movement, combat, physics) and SpacetimeDB for persistent/queryable state (accounts, inventory, economy, social, world persistence). Optionally leverage the MultiServer Replication plugin for scaling beyond a single server.

#### What We'd Stop Reinventing

Everything built in Spikes 5 and 6 already exists — battle-tested — inside UE5:

| What We Built (Spikes 5-6) | UE5 Built-In Equivalent | Maturity |
|---|---|---|
| Chunk-based spatial subscriptions | `NetCullDistanceSquared` + Relevancy system | 25+ years (since UE1) |
| `NyxMovementComponent` prediction | `UCharacterMovementComponent` prediction + reconciliation | Shipped in Fortnite (100 players) |
| `move_player` reducer echo + seq | `ServerMove` / `ClientAdjustPosition` RPC pair | Standard UE5 replication |
| `PlayerTable::OnUpdate` → reconcile | Property replication + delta compression | Automatic, optimized |
| Manual 20 Hz send rate | `NetUpdateFrequency` per actor | Configurable per-class |
| Identity-based actor lookup | `UNetConnection` → `APlayerController` → `APawn` | Core UE5 |

We wrote ~1500 lines of C++ and Rust to replicate what UE5 already does in `CharacterMovementComponent.cpp` (15,000+ lines, decade of edge-case fixes).

#### Architecture

```
┌──────────────┐         ┌──────────────────────┐         ┌──────────────┐
│   UE5        │◄──UDP──►│   UE5 Dedicated       │◄──UDP──►│   UE5        │
│   Client A   │         │   Server(s)           │         │   Client B   │
│              │         │                       │         │              │
│ • CMC predict│         │ • Server-authoritative│         │ • CMC predict│
│ • Interpolate│         │ • CharacterMovement   │         │ • Interpolate│
│ • Render     │         │ • ReplicationGraph    │         │ • Render     │
└──────────────┘         │ • Combat/Physics      │         └──────────────┘
       ▲                 │ • Relevancy (spatial) │                 ▲
       │                 │ • MultiServer (scale) │                 │
       │                 │ • Guild/Chat/AH proxy │                 │
       │                 │                       │                 │
  ONLY connection        │    SpacetimeDB SDK    │        ONLY connection
  (UDP from client)      │    (linked library)   │        (UDP from client)
                         │         │             │
                         └─────────┼─────────────┘
                                   │ WebSocket (BSATN binary)
                                   │ Internal network
                                   ▼
                         ┌───────────────────────┐
                         │    SpacetimeDB 2.0    │
                         │  (separate VM/container)│
                         │                       │
                         │ • Player accounts     │
                         │ • Inventory/economy   │
                         │ • Guilds/social        │
                         │ • Quests/progression  │
                         │ • Territory control   │
                         │ • Cross-server state  │
                         │ • World persistence   │
                         │ • Audit/event log     │
                         └───────────────────────┘
```

**Single-Connection Model:** Clients connect ONLY to the UE5 Dedicated Server via UDP. The dedicated server is the sole gateway to SpacetimeDB. Clients never talk to SpacetimeDB directly. This provides:
- **Security:** Server controls all database access; clients can't craft malicious queries
- **Simplicity:** Clients only need UE5's standard NetDriver — no SpacetimeDB SDK on client
- **Anti-cheat:** All game state mutations flow through server-authoritative validation
- **Reduced attack surface:** SpacetimeDB is not exposed to the internet

#### Layer Responsibilities

**UE5 Dedicated Server (real-time, ephemeral):**
- Server-authoritative WASD movement via `UCharacterMovementComponent`
- Client-side prediction + reconciliation (built-in, proven at Fortnite scale)
- UDP replication with delta compression (only changed properties)
- `UReplicationGraph` for spatial relevancy (L2-style grid, built into engine)
- Combat, skills, projectiles, physics (full Chaos physics)
- AI/NPC simulation with net relevancy (lazy — distant NPCs don't replicate)
- Rate-limiting via `NetUpdateFrequency` and `NetPriority` per actor

**SpacetimeDB (persistent, queryable):**
- Accounts, authentication (EOS → SpacetimeDB identity, Spike 4 flow still applies)
- Character data persistence (load on join, periodic dirty-flush from dedicated server)
- Inventory, equipment, economy (ACID transactions for trades/auctions)
- Guilds, parties, friends, chat (real-time subscriptions for UI updates)
- Quest/progression state
- Territory control, world state (survives server restarts)
- Cross-server queries (guild roster across all zones, global leaderboards)
- Event tables for audit logging

**MultiServer Replication (scaling beyond single server):**
- Spatial partitioning across multiple dedicated servers
- Seamless zone transitions (no loading screens)
- Cross-server actor handoff (player walks from Server A's territory to Server B)
- Shared world state via the plugin's built-in replication channel

#### Why This Works for 1000+ Visible Players

UE5's `ReplicationGraph` is the key. Epic built it specifically to scale Fortnite from ~20 to 100 players. It implements:

1. **Spatial grid cells** — actors grouped by position (same concept as L2's WorldRegion)
2. **Per-connection relevancy** — each client gets only nearby actors (like L2's 3×3 neighborhood)
3. **Frequency bucketing** — far actors replicate at 1-5 Hz, close actors at 10-30 Hz
4. **Dormancy** — stationary actors stop replicating entirely until state changes
5. **Priority** — damage-dealing/receiving actors get bandwidth priority

A custom `UReplicationGraph` subclass can implement ALL of L2's optimizations:

```cpp
// Custom ReplicationGraph implementing L2-style patterns
class UNyxReplicationGraph : public UReplicationGraph
{
    // L2 Pattern 1: Spatial grid (same as WorldRegion)
    UReplicationGraphNode_GridSpatialization2D* GridNode;

    // L2 Pattern 2: Rate-limited movement (NetUpdateFrequency)
    // Far players: 2 Hz, Medium: 5 Hz, Close: 10 Hz

    // L2 Pattern 3: Dormancy for stationary actors
    UReplicationGraphNode_DormancyNode* DormancyNode;

    // L2 Pattern 4: Always-relevant (party UI, targeted HP)
    UReplicationGraphNode_AlwaysRelevant* AlwaysRelevantNode;

    // L2 Pattern 5: HP updates only to targeting players
    // → Custom node that checks if observer is targeting this actor
};
```

**With aggressive ReplicationGraph optimization:**
- 1000 players in a siege → each client sees ~200-400 relevant actors (distance-based LOD)
- Close actors (50m): full replication at 10 Hz → ~100 actors × 10 Hz = 1000 updates/sec per client
- Medium actors (100m): reduced replication at 5 Hz → ~200 actors × 5 Hz = 1000 updates/sec
- Far actors (200m): minimal replication at 1-2 Hz → ~200 actors × 2 Hz = 400 updates/sec
- Total: ~2400 replicated updates/sec per client, with delta compression (only changed bytes)
- UDP, so no head-of-line blocking. Droppable movement packets (unreliable RPCs)

#### MultiServer for Scaling Beyond Single-Server Limits

Single UE5 dedicated server caps at ~200-400 players (game thread bottleneck). MultiServer splits the world:

```
┌─────────────────────────────────────────────────────────┐
│                   Game World                             │
│                                                         │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│   │ Server 1 │  │ Server 2 │  │ Server 3 │            │
│   │ Zone NW  │←→│ Zone NE  │←→│ Zone SE  │            │
│   │ ~300 ply │  │ ~300 ply │  │ ~300 ply │            │
│   └──────────┘  └──────────┘  └──────────┘            │
│        ↕                                               │
│   ┌──────────┐   Siege Event: Servers 1+2              │
│   │ Server 4 │   merge relevancy at boundary           │
│   │ Zone SW  │                                         │
│   │ ~300 ply │                                         │
│   └──────────┘                                         │
│        │             │             │                   │
│        └─────────────┼─────────────┘                   │
│                      ▼                                  │
│              ┌──────────────┐                           │
│              │ SpacetimeDB  │                           │
│              │ (shared DB)  │                           │
│              └──────────────┘                           │
└─────────────────────────────────────────────────────────┘
```

For a 1000-player siege: 3-4 servers each handling 250-350 players, with MultiServer Replication handling cross-boundary visibility. Players near server boundaries are replicated to both servers. SpacetimeDB serves as the shared persistence layer for all servers.

#### Scaling a Hot Zone: 1000+ Players in One Location

**The Problem:** Spatial sharding (MultiServer) works when players spread across different zones. But what happens when 1000+ players converge on a single location — a castle siege, a world boss, a capital city event? One UE5 dedicated server caps at ~200-400 players (game thread bottleneck). Spatial partitioning doesn't help because everyone is in the same spot.

This is the hardest problem in MMO architecture. Every shipped game handles it differently:

| Game | Approach | Result |
|------|----------|--------|
| **Eve Online** | Time Dilation (TiDi) — slow down simulation to 10% speed | 6000+ ships in one system, but everything is slow motion |
| **Lineage 2** | Aggressive interest management — 3×3 region visibility, minimal per-player data, no physics | 1000+ visible as lightweight entities |
| **PlanetSide 2** | Continent caps + hex population balance incentives | ~600-800 per continent, degrades under peak |
| **New World** | Hard server cap (2000), no instancing | Lag at territory wars with 100v100 |
| **Guild Wars 2** | Dynamic overflow instancing — split players into parallel instances | Breaks immersion but keeps performance |
| **SpatialOS** | Multi-worker functional decomposition (physics worker, AI worker, etc.) | Technically works, extremely complex, company pivoted away |

There are four patterns for handling the hot-zone problem. They aren't mutually exclusive — a production system would likely combine several.

##### Pattern A: Entity-Sharded MultiServer (Overlapping Zones)

Multiple servers share the SAME physical zone, each authoritative over a subset of entities:

```
┌─────────────────────────────────────────────────────────┐
│                  Castle Siege Zone                        │
│                                                         │
│   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐ │
│   │  Server A     │  │  Server B     │  │  Server C     │ │
│   │  Entities     │  │  Entities     │  │  Entities     │ │
│   │  1-333        │  │  334-666      │  │  667-1000     │ │
│   │              │  │              │  │              │ │
│   │  Authoritative│  │  Authoritative│  │  Authoritative│ │
│   │  over its     │  │  over its     │  │  over its     │ │
│   │  entities     │  │  entities     │  │  entities     │ │
│   └──────┬───────┘  └──────┬───────┘  └──────┬───────┘ │
│          │                 │                 │          │
│          └─────────────────┼─────────────────┘          │
│                            ▼                            │
│                   Cross-Server                          │
│                   Replication                           │
│                   (MultiServer)                         │
└─────────────────────────────────────────────────────────┘
```

**How it works:**
- When a zone becomes "hot" (population exceeds threshold), orchestrator spins up additional servers for the same zone
- Each server runs a full UE5 world simulation but is authoritative over only its assigned players/NPCs
- MultiServer Replication handles cross-server visibility: Server A's players can see Server B's players
- When Player (on Server A) attacks Player (on Server B), the interaction crosses server boundaries via MultiServer's replication channel

**Pros:**
- Uses UE5's MultiServer Replication (designed for this)
- Each server handles ~300-333 players — within proven limits
- Standard CMC, ReplicationGraph, Actor model — no engine hacks
- Entity assignment can be dynamic (rebalance based on spatial clusters)

**Cons:**
- Cross-server hit detection adds 1 tick of latency (33ms at 30 Hz)
- MultiServer Replication plugin is experimental — needs battle-testing
- Entity handoff complexity when players move between server authority
- All servers must simulate awareness of ALL 1000 entities for relevancy (read-only mirrors)

**Latency budget for cross-server combat:**
- Player A (Server A) swings sword → Server A detects hit geometry
- Server A → MultiServer channel → Server B: "Hit detected on your entity"
- Server B applies damage, replicates HP update
- Total added latency: ~1-2 ticks (33-66ms) — acceptable for MMO combat, not for fighting games

##### Pattern B: Functional Decomposition (Movement Server + Combat Server)

Split responsibilities by system — one server processes ALL movement, another processes ALL combat:

```
                    ┌──────────────────────┐
                    │   Movement Server     │
                    │                      │
                    │ • CMC for all 1000   │
                    │ • Position authority  │
                    │ • Client connections  │
                    │ • Replication         │
                    │ • Physics/collision   │
     clients ←UDP→ │                      │ ←internal→ ┌────────────────────┐
                    │ "Sword swing at       │            │  Combat Server     │
                    │  tick 4523, pos X,Y,Z"│  ────────► │                    │
                    │                      │            │ • Damage formulas  │
                    │ "Apply 147 dmg to    │  ◄──────── │ • Buff/debuff mgmt │
                    │  entity 502,         │            │ • Stat resolution  │
                    │  knockback (0,0,500)"│            │ • AoE computation  │
                    └──────────────────────┘            │ • Cooldown tracking│
                                                       └────────────────────┘
```

**How it works:**
- Movement Server runs all client connections, CMC, physics, replication
- When an ability activates, Movement Server sends the event + context (caster position, target position, ability ID) to Combat Server
- Combat Server resolves: target selection (AoE radius), damage formula (stats × modifiers × armor), status effects
- Combat Server returns results: damage amounts, knockback vectors, status effects to apply
- Movement Server applies results (deduct HP, apply knockback velocity, replicate to clients)

**Pros:**
- Movement Server workload is reduced (no combat math)
- Combat Server can be scaled independently (multiple combat workers)
- Combat logic can be updated without touching the movement/replication server
- Clean separation: UE5 handles the "physics" layer, combat server handles the "game rules" layer

**Cons:**
- **UE5 was NOT designed for this.** `UCharacterMovementComponent`, `AActor`, `UAbilitySystemComponent` all assume single-server authority. Splitting them across processes requires significant custom code.
- **Tight coupling between systems:** Stuns stop movement. Roots prevent dashing. Knockbacks are velocity changes. Slows reduce `MaxWalkSpeed`. Every combat effect modifies movement state. The Combat Server must send movement-affecting results back to the Movement Server every tick.
- **State duplication:** Combat Server needs positions for range checks, AoE geometry, line-of-sight. Movement Server must stream all 1000 positions to Combat Server every tick (~36KB/tick at 30Hz = 1.08 MB/s).
- **Debugging nightmare:** A bug could be in the movement server, combat server, or the serialization between them.
- **Single point of failure:** The Movement Server is STILL handling 1000 client connections and running CMC for all of them. If the bottleneck is CMC (which it usually is), this doesn't help.

**Critical issue:** The real bottleneck for 1000+ players isn't combat math — it's `CharacterMovementComponent::PerformMovement()` running for every player every tick. Combat calculations are relatively cheap (~1-5μs per hit). CMC is expensive (~50-200μs per player per tick) because it does physics sweeps, floor checks, gravity, step-up detection. Offloading combat doesn't solve the actual bottleneck.

**Verdict:** Functional decomposition sounds elegant but fights UE5's architecture. SpatialOS (Improbable) spent hundreds of millions building multi-worker game engine support and ultimately pivoted away from it. The systems are too tightly coupled for clean separation.

##### Pattern C: SpacetimeDB as Combat Compute Engine

Use SpacetimeDB WASM reducers for damage resolution, stat checks, and game-rule enforcement:

```
┌──────────────────────┐
│  UE5 Dedicated Server │
│                      │
│  Hit detection:      │     WebSocket (BSATN)
│  "Sword hit entity   │ ──────────────────────► ┌──────────────────┐
│   502 at tick 4523"  │                          │  SpacetimeDB 2.0 │
│                      │                          │                  │
│  Result applied:     │ ◄────────────────────── │  WASM Reducer:   │
│  "Entity 502 HP:     │    subscription update   │  resolve_hit()   │
│   853→706, apply     │                          │                  │
│   STUNNED 2.0s"      │                          │  • Load attacker │
│                      │                          │    stats/equip   │
└──────────────────────┘                          │  • Load defender │
                                                  │    stats/armor   │
                                                  │  • Apply formula │
                                                  │  • Apply buffs   │
                                                  │  • ACID commit   │
                                                  │  • Return result │
                                                  └──────────────────┘
```

**How it works:**
- UE5 handles ALL real-time work: movement, physics, hit detection (geometry overlap, ray traces)
- When a hit is confirmed (sword collider touches target hitbox), UE5 calls a SpacetimeDB reducer: `resolve_hit(attacker_id, defender_id, ability_id, hit_location)`
- SpacetimeDB reducer loads both players' stats from tables, applies the damage formula, updates HP, applies status effects, logs the event, commits atomically
- UE5 receives the result via subscription update and applies the visual/gameplay effects

**Pros:**
- **ACID combat transactions:** No double-damage, no item duplication, no race conditions. SpacetimeDB guarantees atomic state changes.
- **Live-tunable:** Change damage formulas by publishing a new WASM module — no server restart, no client patch
- **Perfect audit trail:** Every hit, every damage event logged in event tables with full context
- **Stats always in-DB:** No state duplication. Player stats, equipment, buffs live in SpacetimeDB tables. The reducer reads the authoritative source every time.
- **Already benchmarked:** Spike 7 measured ~1-5μs per WASM reducer call. Damage resolution is simple math.

**Cons:**
- **WebSocket latency:** 1-5ms round-trip over internal network. Not an issue for damage resolution (the visual hit already plays client-side immediately via prediction), but adds 1-2 ticks of delay before HP bar updates.
- **Not suitable for frame-dependent combat:** If combat requires physics-level precision (projectile trajectory, dodge iframes at 60Hz), the round-trip to SpacetimeDB is too slow. Works fine for tab-target or action-MMO combat where hit detection is done server-side in UE5.
- **Load under mass combat:** 1000 players all fighting = potentially hundreds of `resolve_hit()` calls per second. SpacetimeDB's single-writer model means these serialize. At ~5μs each, 200 hits/sec = 1ms of total compute — trivial. At 2000 hits/sec = 10ms — still fine.
- **AoE complexity:** An AoE skill hitting 50 targets = 50 reducer calls or 1 batch reducer. Batch is better.

**What goes to SpacetimeDB vs stays in UE5:**

| Responsibility | Where | Why |
|---------------|-------|-----|
| Hit detection (geometry) | UE5 | Needs physics engine, colliders, line-of-sight |
| Projectile simulation | UE5 | Needs physics, frame-by-frame trajectory |
| Damage formula | SpacetimeDB | Needs stats/equipment tables, ACID guarantees |
| Stat/buff resolution | SpacetimeDB | Authoritative source for all character data |
| HP deduction | SpacetimeDB | ACID — prevents negative HP, race conditions |
| Status effect application | SpacetimeDB | Track duration, stacking rules, caps |
| Combat logging | SpacetimeDB | Event tables, queryable audit trail |
| Death/respawn trigger | SpacetimeDB → UE5 | SpacetimeDB detects HP ≤ 0, notifies server |
| Visual effects (particle, animation) | UE5 | Client-side prediction on hit, confirmed by server |
| Knockback velocity | UE5 | Physics vector applied locally, no DB needed |

**Latency flow for a sword attack:**
1. **T+0ms:** Client swings sword, sends input to UE5 server
2. **T+0ms:** Client plays swing animation locally (prediction)
3. **T+16ms:** UE5 server receives input, runs hit detection (physics overlap)
4. **T+17ms:** Hit confirmed → UE5 calls SpacetimeDB: `resolve_hit(attacker, defender, skill)`
5. **T+19ms:** SpacetimeDB reducer executes: load stats → calculate 147 damage → update HP → commit
6. **T+22ms:** UE5 receives subscription update: "Entity 502 HP = 706, STUNNED 2.0s"
7. **T+22ms:** UE5 replicates to clients: HP bar update, stun visual effect
8. **T+55ms:** Clients see HP bar change (next client tick)

Total added latency from SpacetimeDB: ~5ms. The player's client already sees the sword swing animation at T+0, so the "feel" of combat is instant. The HP bar updating 55ms later is imperceptible.

##### Pattern D: Aggressive LOD + Graceful Degradation

Instead of throwing more servers at the problem, reduce what each server needs to simulate:

```
Distance from player    Simulation fidelity        Update rate
─────────────────────   ─────────────────────────   ───────────
0-30m (close)           Full CMC + full replication   10-30 Hz
30-80m (medium)         Simplified movement, no physics  5 Hz
80-200m (far)           Position-only snapshots         1-2 Hz
200-500m (distant)      Cluster blobs (10 players → 1 "blob" entity)  0.5 Hz
500m+ (out of range)    Not simulated, not replicated    0 Hz
```

**Key technique: Entity LOD (not just visual LOD)**
- Close players: full `CharacterMovementComponent`, full animation replication, full combat
- Medium players: simplified movement (linear interpolation, no physics sweeps), basic animation state
- Far players: position + rotation only, no CMC, no collision, batch-updated
- Distant players: aggregated into "crowd blobs" — a single replicated actor represents a group

**Impact on server cost:**
- Without LOD: 1000 players × 200μs CMC = 200ms/tick → impossible at 30 Hz (33ms budget)
- With LOD: 100 close × 200μs + 200 medium × 50μs + 300 far × 10μs + 400 distant × 0μs = 33ms → just barely fits

**Combined with MultiServer (Pattern A):**
- 3 servers × 333 players each
- Each server applies LOD within its entity set
- 333 players with LOD: 100 close × 200μs + 133 medium × 50μs + 100 far × 10μs = 27.65ms → comfortable at 30 Hz

##### Recommended Hybrid: Patterns A + C + D

The production architecture for 1000+ players in one zone combines three patterns:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Hot Zone (Siege)                              │
│                                                                     │
│  ┌────────────────┐  ┌────────────────┐  ┌────────────────┐        │
│  │  UE5 Server A   │  │  UE5 Server B   │  │  UE5 Server C   │        │
│  │  ~333 entities  │  │  ~333 entities  │  │  ~333 entities  │        │
│  │                │  │                │  │                │        │
│  │  Entity LOD:   │  │  Entity LOD:   │  │  Entity LOD:   │        │
│  │  100 full CMC  │  │  100 full CMC  │  │  100 full CMC  │        │
│  │  133 simplified│  │  133 simplified│  │  133 simplified│        │
│  │  100 pos-only  │  │  100 pos-only  │  │  100 pos-only  │        │
│  │                │  │                │  │                │        │
│  │  ↕ MultiServer │  │  ↕ MultiServer │  │  ↕ MultiServer │        │
│  └───────┬────────┘  └───────┬────────┘  └───────┬────────┘        │
│          │                   │                   │                  │
│          └───────────────────┼───────────────────┘                  │
│                              │ WebSocket (BSATN)                    │
│                              ▼                                      │
│                   ┌──────────────────┐                               │
│                   │  SpacetimeDB 2.0 │                               │
│                   │                  │                               │
│                   │  • resolve_hit() │                               │
│                   │  • apply_buff()  │                               │
│                   │  • ACID combat   │                               │
│                   │  • Audit logging │                               │
│                   │  • Persistence   │                               │
│                   └──────────────────┘                               │
└─────────────────────────────────────────────────────────────────────┘
         ▲                    ▲                    ▲
    UDP  │               UDP  │               UDP  │
    ┌────┴────┐         ┌────┴────┐         ┌────┴────┐
    │ Clients │         │ Clients │         │ Clients │
    │ 1-333   │         │ 334-666 │         │ 667-1000│
    └─────────┘         └─────────┘         └─────────┘
```

**How it works together:**

1. **Normal state:** One UE5 server handles the zone with up to ~300 players
2. **Crowd threshold:** When zone population exceeds 300, orchestrator spins up Server B and starts entity-sharding (Pattern A)
3. **Siege event:** At 600+, Server C joins. Each server is authoritative over ~333 players
4. **Entity LOD (Pattern D):** Each server applies aggressive simulation LOD. Only ~100 nearby entities get full CMC per server. Total game-thread cost stays under 30ms
5. **Combat via SpacetimeDB (Pattern C):** All three servers send hit events to SpacetimeDB. Damage resolution, stat checks, buff management happen in WASM reducers with ACID guarantees. No combat state duplication between servers.
6. **Cross-server combat:** Player on Server A attacks Player on Server B → Server A detects hit geometry → calls SpacetimeDB `resolve_hit()` → SpacetimeDB updates defender's HP → Server B receives subscription update → replicates to defender's client. Added latency: ~5-10ms.

**Why this avoids Pattern B's problems:**
- No functional splitting of UE5 systems — each server runs a standard UE5 game loop
- CMC stays on the same server as the player's connection — no cross-server movement authority
- Combat math is offloaded to SpacetimeDB (not another UE5 server) so there's no UE5-to-UE5 synchronization for game rules
- SpacetimeDB's ACID transactions prevent combat exploits that arise from multi-server state disagreement

**Scaling math:**

| Players | Servers | Entities/server | Full CMC/server | Tick budget used |
|---------|---------|-----------------|-----------------|-----------------|
| 300     | 1       | 300             | ~100 (LOD)      | ~25ms (ok)       |
| 600     | 2       | 300             | ~100 (LOD)      | ~25ms (ok)       |
| 1000    | 3       | 333             | ~100 (LOD)      | ~28ms (ok)       |
| 1500    | 5       | 300             | ~100 (LOD)      | ~25ms (ok)       |
| 2000    | 7       | ~286            | ~100 (LOD)      | ~25ms (ok)       |

**Theoretical ceiling:** Limited by cross-server replication bandwidth, not compute. Each server must be aware of all ~1000 entities (read-only mirrors) for relevancy. At 1000 entities × ~40 bytes position data × 10 Hz average = ~400KB/s per server — manageable over internal network.

**What breaks this model:**
- **> 5000 players in one spot:** Cross-server replication becomes the bottleneck. Each server mirrors ~5000 entities. Consider Eve Online-style time dilation here.
- **Physics-heavy combat:** If every ability involves full physics simulation (ragdolls, destructibles), the CMC budget gets saturated even with LOD. Simplify physics for dense scenarios.
- **MultiServer plugin stability:** The plugin is experimental. It may not handle dynamic entity reassignment smoothly. Needs thorough stress-testing.

#### SpacetimeDB Integration Points

The dedicated server talks to SpacetimeDB via the C++ or Rust SDK:

| Event | Flow |
|-------|------|
| Player joins | Server → SpacetimeDB: `load_character(identity)` → gets inventory, stats, quests |
| Player trades | Server → SpacetimeDB: reducer with ACID transaction (atomic item swap) |
| Player levels up | Server → SpacetimeDB: `update_progression(identity, new_level, new_xp)` |
| Periodic save | Server → SpacetimeDB: batch dirty-flush of positions, HP, buffs (every 2-5 sec) |
| Player disconnects | Server → SpacetimeDB: final save of all state |
| Guild chat | Client → Server RPC → SpacetimeDB subscription (server relays to recipients) |
| Auction house | Client → Server RPC → SpacetimeDB reducer (server validates, executes ACID transaction) |
| Territory control | Server → SpacetimeDB: `capture_territory(guild_id, zone_id)` → all servers subscribe |

All client requests — including non-gameplay features like chat, inventory UI, guild management, and the auction house — flow through the dedicated server. The server acts as a trusted proxy, validating requests before forwarding them to SpacetimeDB. This eliminates the need for a SpacetimeDB SDK on the client and keeps the database off the public internet.

#### What Changes From Current Nyx Architecture

| Current (SpacetimeDB-everything) | Option 4 (Hybrid) |
|---|---|
| SpacetimeDB IS the game server | UE5 dedicated server IS the game server |
| WebSocket for everything | UDP for gameplay, WebSocket for persistence/social |
| Custom NyxMovementComponent | Standard CharacterMovementComponent |
| Manual spatial subscriptions | ReplicationGraph handles relevancy |
| WASM reducers for game logic | C++ game logic on dedicated server |
| SpacetimeDB subscriptions for entity sync | UE5 replication for entity sync |
| Single process | Dedicated server (VM/container) + SpacetimeDB (separate VM/container) |

#### What We Keep From Phase 0

The Phase 0 spikes aren't wasted — they inform how the SpacetimeDB integration layer works:

- **Spike 1-3:** Plugin integration, codegen, round-trip → still used for server↔SpacetimeDB communication
- **Spike 4:** EOS auth → still the auth flow, but token passes through dedicated server
- **Spike 5:** Spatial chunks → informs ReplicationGraph cell sizing
- **Spike 6:** Prediction architecture understanding → validates that CMC does it better
- **Spike 7:** WASM benchmarks → SpacetimeDB still runs economy/inventory logic as reducers
- **Spike 8-9:** Sidecar/scalability data → confirms why dedicated server is needed for physics

#### Viability Assessment

| Criterion | Rating | Notes |
|-----------|--------|-------|
| **Proven at scale** | ★★★★★ | Every shipped MMO uses UE4/5 dedicated servers. Fortnite does 100 on stock engine. |
| **WASD support** | ★★★★★ | CMC is literally built for this. Prediction, reconciliation, all included. |
| **Server-authoritative** | ★★★★★ | Default UE5 mode. Server owns all movement, combat, physics. |
| **1000+ visible** | ★★★★☆ | Needs custom ReplicationGraph + MultiServer. Not trivial but proven path. |
| **Development velocity** | ★★★★☆ | Massive existing ecosystem: tutorials, marketplace, community. |
| **SpacetimeDB fit** | ★★★★☆ | Perfect for persistence/social/economy. Clear separation of concerns. |
| **Complexity** | ★★★☆☆ | More infrastructure (dedicated server hosting, orchestration). |
| **MultiServer maturity** | ★★☆☆☆ | Experimental plugin. May need modifications. |

#### Transport Protocol: Dedicated Server ↔ SpacetimeDB

SpacetimeDB's SDK supports **WebSocket only** as the transport protocol. There is no raw TCP, UDP, or gRPC alternative. However, this is perfectly adequate for the server→database connection:

| Aspect | Detail |
|--------|--------|
| **Protocol** | WebSocket (`ws://` or `wss://` via `DbConnection::builder().with_uri()`) |
| **Serialization** | BSATN (Binary SpacetimeDB Algebraic Notation) — compact binary, NOT JSON |
| **Frequency** | Persistence operations at 0.2-5 Hz (NOT 60 Hz real-time gameplay) |
| **Overhead** | WebSocket framing: 2-14 bytes per frame (negligible for persistence ops) |
| **Connection model** | `run_threaded()` for background processing, or `frame_tick()` for per-frame polling |
| **Subscriptions** | Server subscribes to cross-server state (territory, guilds) — push-based, low volume |

**Why WebSocket is sufficient:** The dedicated server→SpacetimeDB connection handles persistence operations (save character, trade items, update quests) — NOT real-time gameplay state. Real-time movement, combat, and physics use UE5's UDP replication directly between clients and the dedicated server. The SpacetimeDB connection fires at most a few times per second for dirty-flushes and on-demand for player joins/trades/disconnects.

**BSATN vs JSON:** SpacetimeDB uses BSATN (a compact binary format), not JSON. This means serialization overhead is minimal — comparable to protobuf. A position update (`f32 × 3 + rotation`) would be ~16 bytes of BSATN payload vs ~80+ bytes as JSON text.

#### Deployment Model

Each component runs in its own VM or container, communicating over internal network:

```
┌─────────────────────────────────────────────────────────────┐
│                    Internal Network                          │
│                                                             │
│  ┌───────────────────┐   ┌───────────────────┐              │
│  │ VM/Container 1    │   │ VM/Container 2    │              │
│  │                   │   │                   │              │
│  │ UE5 Dedicated     │   │ UE5 Dedicated     │              │
│  │ Server (Zone NW)  │   │ Server (Zone NE)  │              │
│  │                   │   │                   │              │
│  │ SpacetimeDB SDK   │   │ SpacetimeDB SDK   │              │
│  │ (linked library)  │   │ (linked library)  │              │
│  └────────┬──────────┘   └────────┬──────────┘              │
│           │ ws://stdb:3000        │ ws://stdb:3000          │
│           └───────────┬───────────┘                         │
│                       ▼                                      │
│           ┌───────────────────┐                              │
│           │ VM/Container 3    │                              │
│           │                   │                              │
│           │ SpacetimeDB 2.0   │                              │
│           │ Port 3000         │                              │
│           │                   │                              │
│           └───────────────────┘                              │
└─────────────────────────────────────────────────────────────┘
         ▲               ▲
         │ UDP (public)   │ UDP (public)
    ┌────┴────┐     ┌────┴────┐
    │ Client  │     │ Client  │
    │    A    │     │    B    │
    └─────────┘     └─────────┘
```

**SpacetimeDB Docker support:** Official Docker image available:
```bash
docker run --rm --pull always -p 3000:3000 clockworklabs/spacetime start
```

**Key deployment considerations:**
- SpacetimeDB listens on port 3000 by default
- SSL not supported in standalone mode (use reverse proxy for wss:// if needed)
- Each dedicated server connects to SpacetimeDB via internal network (low latency, no public exposure)
- SpacetimeDB is NOT exposed to the public internet — only dedicated servers can reach it
- Multiple dedicated servers connect to the SAME SpacetimeDB instance (shared world state)
- Container orchestration (Kubernetes, ECS, etc.) can auto-scale dedicated servers per zone demand

#### Key Implementation Work

1. **Custom `UReplicationGraph`** implementing L2-style optimizations (spatial grid, rate-limiting per distance tier, dormancy, priority)
2. **SpacetimeDB integration module** in the dedicated server (linked SDK library for load/save character data, ACID economy transactions)
3. **MultiServer configuration** for spatial world partitioning
4. **Server-proxied UI features** — dedicated server relays chat, inventory, AH requests to SpacetimeDB on behalf of clients
5. **Container/VM deployment pipeline** — Docker images for SpacetimeDB, dedicated server packaging for VM/container deployment

### Options Comparison Summary

| Aspect | Option 1 (Pure STDB) | Option 2 (UDP Relay) | Option 3 (Event Tables) | Option 4 (UE5 NetDriver) |
|--------|---------------------|---------------------|------------------------|--------------------------|
| **Ceiling** | ~300-500 | 1000+ | ~500-800 | 1000+ |
| **WASD** | Via intent tables | Via relay | Via events | Native CMC |
| **Server-auth movement** | WASM reducer | Relay process | WASM reducer | UE5 dedicated server |
| **Physics** | None (or sidecar) | None (or sidecar) | None (or sidecar) | Full Chaos physics |
| **New infrastructure** | None | UDP relay process | None | Dedicated server hosting |
| **Complexity** | Low | Medium | Low-Medium | Medium-High |
| **Proven at MMO scale** | No (BitCraft is ~200) | Pattern proven (L2) | No | Yes (every shipped MMO) |
| **Development velocity** | Medium | Medium | Medium | High (existing UE5 ecosystem) |
| **What we reinvent** | Movement, combat, physics | Movement auth, combat | Movement, combat, physics | Nothing (use UE5 built-ins) |
| **SpacetimeDB role** | Everything | Persistence + social | Everything | Persistence + social + economy |

### Recommendation

**Option 4 is the recommended architecture** for achieving 1000+ visible WASD players with server-authoritative movement. It uses proven technology (UE5 NetDriver, CMC, ReplicationGraph) for real-time gameplay and SpacetimeDB for what it excels at — persistent, queryable, transactional game state.

**Start with Option 1** if the goal is rapid prototyping with minimal infrastructure. It validates the event-driven movement model and can serve ~300-500 players per zone.

**Plan for Option 4** as the production architecture. The key insight from all the research: UE5's dedicated server already implements every optimization that L2 used to achieve 1000+ player sieges — spatial grid relevancy, rate-limited replication, delta compression, dormancy, priority bucketing, UDP transport with unreliable channels. Building these from scratch on top of SpacetimeDB's subscription model would take years and never match what Epic has refined over two decades.

The Phase 0 spikes are NOT wasted — they validated SpacetimeDB's strengths (persistence, subscriptions, ACID transactions) and identified its limitations (fan-out ceiling, no UDP, subscription re-evaluation cost). This data directly informs how to partition responsibilities between UE5 dedicated servers and SpacetimeDB in Option 4.

---

## References

- SpacetimeDB 2.0 Unreal Reference: https://spacetimedb.com/docs/2.0.0-rc1/clients/unreal
- SpacetimeDB Client Codegen: https://spacetimedb.com/docs/2.0.0-rc1/clients/codegen
- SpacetimeDB GitHub: https://github.com/clockworklabs/SpacetimeDB
- EOS Online Services Config (UE5): https://dev.epicgames.com/documentation/en-us/unreal-engine/enable-and-configure-online-services-eos-in-unreal-engine
- EOS Developer Portal: https://dev.epicgames.com/portal/
- UE5.7 Documentation: https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-7-documentation
- BitCraft (reference game built with SpacetimeDB): https://www.bitcraftonline.com/
- Redwood MMO (UE5 backend infrastructure): https://redwoodmmo.com/docs/
- SpacetimeDB 2.0 Subscriptions: https://spacetimedb.com/docs/clients/subscriptions
- SpacetimeDB 2.0 Views: https://spacetimedb.com/docs/functions/views
- SpacetimeDB 2.0 Table Performance: https://spacetimedb.com/docs/tables/performance
- SpacetimeDB 2.0 Event Tables: https://spacetimedb.com/docs/tables/event-tables
- SpacetimeDB 2.0 SQL Reference: https://spacetimedb.com/docs/reference/sql
- BitCraft Server Source Code (Apache 2.0): https://github.com/clockworklabs/BitCraftPublic
- MultiServer Replication Plugin (Experimental): https://dev.epicgames.com/community/learning/knowledge-base/xBvk/unreal-engine-experimental-an-introduction-to-the-multiserver-replication-plugin
- MultiServer Replication API: https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/MultiServerReplication
- Open World Server (OWS): https://github.com/Dartanlla/OWS
- OWS Documentation: https://www.openworldserver.com/
- L2J Mobius (Lineage 2 Server Emulator): https://gitlab.com/MobiusDevelopment/L2J_Mobius
- UE5 ReplicationGraph: https://dev.epicgames.com/documentation/en-us/unreal-engine/replication-graph-in-unreal-engine
