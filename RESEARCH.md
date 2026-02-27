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

## Spike 1: SpacetimeDB 2.0 Unreal Plugin Integration

**Goal:** Confirm the official SpacetimeDB Unreal SDK plugin compiles and connects from our UE5.7 project.

**Duration:** 1–2 days

### Tasks

1. **Install SpacetimeDB CLI**
   - Download and install from https://spacetimedb.com/install
   - Verify `spacetime version` reports 2.0.x
   - Run `spacetime start` to confirm local instance boots

2. **Clone the SpacetimeDB Unreal Plugin**
   - Clone from https://github.com/clockworklabs/SpacetimeDB
   - Locate the Unreal plugin under `crates/sdk/` or the dedicated plugin repo
   - Copy plugin to `C:\UE\Nyx\Plugins\SpacetimeDB\`

3. **Add Plugin to Project**
   - Add to `Nyx.uproject`:
     ```json
     {
       "Name": "SpacetimeDB",
       "Enabled": true
     }
     ```
   - Add `"SpacetimeDB"` to `Nyx.Build.cs` PrivateDependencyModuleNames

4. **Compile and Verify**
   - Build `NyxEditor Win64 Development`
   - Fix any compile errors (UE5.7 API changes, missing includes, etc.)
   - Confirm `UDbConnection`, `URemoteTables`, `USubscriptionBuilder` are accessible

### Deliverable
- [ ] Plugin compiles with UE5.7
- [ ] Can `#include` SpacetimeDB headers in Nyx source
- [ ] Document any UE5.7-specific patches needed

### Key Questions to Answer
- Does the plugin support UE5.7 out of the box, or does it need patches?
- What is the plugin's module name for `Build.cs` dependencies?
- Does it conflict with any default UE5 plugins?

---

## Spike 2: SpacetimeDB 2.0 Module (Server-Side) Hello World

**Goal:** Write, deploy, and interact with a minimal SpacetimeDB module.

**Duration:** 1–2 days

### Tasks

1. **Create a Rust Module**
   ```bash
   mkdir C:\UE\Nyx\Server
   cd C:\UE\Nyx\Server
   spacetime init --lang rust nyx-server
   ```

2. **Define a Minimal Schema**
   ```rust
   // Server/nyx-server/src/lib.rs
   use spacetimedb::spacetimedb;

   #[spacetimedb::table(name = player, public)]
   pub struct Player {
       #[primary_key]
       #[auto_inc]
       id: u64,
       name: String,
       x: f32,
       y: f32,
       z: f32,
   }

   #[spacetimedb::reducer]
   pub fn create_player(ctx: &ReducerContext, name: String) {
       ctx.db.player().insert(Player {
           id: 0,
           name,
           x: 0.0,
           y: 0.0,
           z: 0.0,
       });
   }

   #[spacetimedb::reducer]
   pub fn move_player(ctx: &ReducerContext, id: u64, x: f32, y: f32, z: f32) {
       if let Some(mut p) = ctx.db.player().id().find(id) {
           p.x = x;
           p.y = y;
           p.z = z;
           ctx.db.player().id().update(p);
       }
   }
   ```

3. **Publish Locally**
   ```bash
   spacetime publish nyx-world --project-path Server/nyx-server
   ```

4. **Generate UE5 Bindings**
   ```bash
   spacetime generate --lang unrealcpp --uproject-dir C:\UE\Nyx --module-path Server/nyx-server --unreal-module-name Nyx
   ```

5. **Verify Generated Code**
   - Check `C:\UE\Nyx\Source\Nyx\ModuleBindings\` (or wherever codegen outputs)
   - Confirm generated `UDbConnection`, `URemoteTables` subclasses, `URemoteReducers` subclass

### Deliverable
- [ ] Module publishes to local SpacetimeDB without errors
- [ ] `spacetime generate` produces Unreal C++ bindings
- [ ] Generated code compiles within the Nyx project
- [ ] Document the exact codegen command and output directory

### Key Questions to Answer
- What is the exact `spacetime generate` Unreal C++ output structure?
- Does codegen produce a separate UE module or files within our existing module?
- What's the iteration loop? (edit module → publish → regenerate → recompile UE5)

---

## Spike 3: UE5 ↔ SpacetimeDB Round-Trip Connection

**Goal:** Connect from UE5, call a reducer, subscribe to a table, and verify round-trip data flow.

**Duration:** 2–3 days

### Tasks

1. **Create a Test Actor** (`ANyxConnectionTestActor`)
   ```cpp
   void ANyxConnectionTestActor::BeginPlay()
   {
       Super::BeginPlay();
       
       FOnConnectDelegate OnConnect;
       OnConnect.BindDynamic(this, &ANyxConnectionTestActor::HandleConnected);
       
       Connection = UDbConnection::Builder()
           ->WithUri(TEXT("127.0.0.1:3000"))
           ->WithDatabaseName(TEXT("nyx-world"))
           ->OnConnect(OnConnect)
           ->Build();
       
       // Register table callbacks
       Connection->Db->Player->OnInsert.AddDynamic(this, &ANyxConnectionTestActor::OnPlayerInsert);
       Connection->Db->Player->OnUpdate.AddDynamic(this, &ANyxConnectionTestActor::OnPlayerUpdate);
   }
   
   void ANyxConnectionTestActor::HandleConnected(UDbConnection* Conn, FSpacetimeDBIdentity Identity, const FString& Token)
   {
       // Subscribe to all players
       Conn->SubscriptionBuilder()
           ->OnApplied(...)
           ->Subscribe({TEXT("SELECT * FROM player")});
       
       // Create a player
       Conn->Reducers->CreatePlayer(TEXT("TestPlayer"));
   }
   ```

2. **Measure Latency**
   - Time from `Reducers->MovePlayer(...)` call to `OnUpdate` callback
   - Test with 1, 10, 50, 100 concurrent subscribed rows
   - Log results to `Saved/Logs/`

3. **Test Subscription Queries**
   - `SELECT * FROM player` (all players)
   - `SELECT * FROM player WHERE x > 0` (filtered)
   - Verify the client cache updates correctly for both

4. **Test Subscription Updates**
   - Change subscription query at runtime (e.g., as player moves, update spatial query)
   - Verify old rows are removed and new rows appear
   - Measure time to apply new subscription

### Deliverable
- [ ] UE5 connects to local SpacetimeDB
- [ ] Reducer calls work (create_player, move_player)
- [ ] OnInsert / OnUpdate / OnDelete callbacks fire correctly
- [ ] Subscription queries work with filters
- [ ] Latency numbers documented
- [ ] Subscription update (change query at runtime) works

### Key Questions to Answer
- What is the round-trip latency? (client → reducer → subscription update → client)
- Can we update subscriptions dynamically without reconnecting?
- What SQL subset is supported in subscriptions?
- How many rows can a single client cache handle before performance degrades?
- Does `UDbConnection` auto-tick or do we need to pump it?  
  (Docs say it inherits `FTickableGameObject` — verify this.)

---

## Spike 4: EOS Authentication → SpacetimeDB Token Flow

**Goal:** Authenticate via EOS, obtain an OIDC-compliant JWT, and pass it to SpacetimeDB's `WithToken()`.

**Duration:** 2–3 days

### Tasks

1. **Set Up EOS Developer Portal**
   - Create/configure Product, Sandbox, Deployment in Epic Developer Portal
   - Obtain `ProductId`, `SandboxId`, `DeploymentId`, `ClientId`, `ClientSecret`
   - Configure a Client Policy that allows "Epic Games" login type

2. **Enable EOS Plugins in UE5**
   - Enable `OnlineServicesEOS` and `OnlineServicesEOSGS` plugins
   - Configure `DefaultEngine.ini`:
     ```ini
     [OnlineServices]
     DefaultServices=Epic

     [OnlineServices.EOS]
     ProductId=YOUR_PRODUCT_ID
     SandboxId=YOUR_SANDBOX_ID
     DeploymentId=YOUR_DEPLOYMENT_ID
     ClientId=YOUR_CLIENT_ID
     ClientSecret=YOUR_CLIENT_SECRET
     ClientEncryptionKey=1111111111111111111111111111111111111111111111111111111111111111
     ```

3. **Implement EOS Login**
   - Use `IOnlineServicesPtr` → `GetAuthInterface()` → `Login()`
   - Obtain the EOS `id_token` (OIDC-compliant JWT)
   - Verify the token contains `sub` (ProductUserId), `iss`, `aud` claims

4. **Pass EOS Token to SpacetimeDB**
   - Use `UDbConnection::Builder()->WithToken(EOSIdToken)` 
   - Verify SpacetimeDB accepts or rejects the token
   - If SpacetimeDB requires specific OIDC issuer configuration, document it

5. **Server-Side Token Validation**
   - In the SpacetimeDB Rust module, verify the token claims
   - Link the SpacetimeDB `Identity` to the EOS `ProductUserId`
   - Store the mapping in a `player_account` table

### Deliverable
- [ ] EOS login works in the UE5 editor (Development configuration)
- [ ] EOS `id_token` obtained successfully
- [ ] Token passed to SpacetimeDB via `WithToken()`
- [ ] SpacetimeDB accepts the token and assigns an `Identity`
- [ ] Server module can read the EOS `ProductUserId` from the token
- [ ] `player_account` table created linking SpacetimeDB Identity ↔ EOS PUID
- [ ] Document any OIDC configuration needed on the SpacetimeDB side

### Key Questions to Answer
- Does SpacetimeDB 2.0's `WithToken()` accept EOS JWTs directly?
- Does SpacetimeDB validate the JWT signature (need to configure trusted issuers)?
- Or is `WithToken()` just an opaque reconnection token (not OIDC)?  
  If so, we need a two-step flow: anonymous connect → call `authenticate_with_eos` reducer
- What EOS login types work in the editor? (AccountPortal? DevAuth?)

### Fallback Plan
If `WithToken()` is only for SpacetimeDB's own reconnection tokens (not external OIDC):
1. Connect anonymously to SpacetimeDB (no token)
2. Call an `authenticate_with_eos` reducer, passing the EOS JWT
3. Module validates the JWT server-side
4. Module creates/updates a `player_account` row linking `ctx.sender` (SpacetimeDB Identity) to EOS ProductUserId
5. Save the SpacetimeDB reconnection token (from `OnConnect` callback) for future sessions

---

## Spike 5: Spatial Interest Management at Scale

**Goal:** Determine how to partition the game world so clients only receive data about nearby entities.

**Duration:** 2–3 days

### Tasks

1. **Design Chunk System**
   - Divide world into chunks (e.g., 100m × 100m)
   - Add `chunk_x: i32, chunk_y: i32` columns to entity tables
   - Client subscribes to: `SELECT * FROM entity WHERE chunk_x BETWEEN ? AND ?`

2. **Test Subscription Updates on Movement**
   - As player moves to a new chunk, unsubscribe from old chunks, subscribe to new ones
   - Measure: How fast do new entities appear? How fast do old ones leave the cache?
   - Is there a visible "pop-in" effect?

3. **Stress Test**
   - Populate 10,000 entities across the world
   - Connect 100 clients (can use bot scripts or a test harness)
   - Measure SpacetimeDB CPU, memory, network bandwidth
   - Measure per-client subscription update latency

4. **Compare Strategies**
   - A) One subscription with parameterized spatial query
   - B) Multiple subscriptions per chunk (subscribe to each visible chunk individually)
   - C) Subscribe to all entities but filter client-side (bad for MMO scale, but test baseline)

### Deliverable
- [ ] Chunk-based spatial subscription working
- [ ] Performance numbers with 10K entities, 100 clients
- [ ] Recommended chunk size and subscription strategy
- [ ] Estimated maximum concurrent entities per SpacetimeDB instance

### Key Questions to Answer
- Can subscriptions use parameterized (dynamic value) WHERE clauses?
- What's the maximum number of active subscriptions per client?
- What's the latency of subscription change (unsubscribe old + subscribe new)?
- At what entity count does SpacetimeDB start to struggle?

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
| **Auth flow** | A) EOS JWT → WithToken direct, B) Anonymous + reducer auth | Spike 4 |
| **Spatial partitioning** | A) Single DB + spatial subs, B) Sharded DBs | Spike 5 |
| **Movement model** | A) Full server-auth, B) Client-auth with validation | Spike 3, 6 |
| **AI system** | A) WASM module, B) Sidecar UE5 process, C) Hybrid | Spike 7 |
| **Module language** | A) Rust (performance), B) C# (familiarity) | Spike 2, 7 |

---

## Timeline

| Week | Spikes | Blockers |
|------|--------|----------|
| Week 1 | Spike 1 (Plugin), Spike 2 (Module) | None |
| Week 1–2 | Spike 3 (Round-Trip) | Needs Spike 1 + 2 |
| Week 2 | Spike 4 (EOS Auth) | Needs Spike 3 + EOS Portal setup |
| Week 2–3 | Spike 5 (Spatial), Spike 6 (Prediction) | Needs Spike 3 |
| Week 3 | Spike 7 (WASM Perf) | Needs Spike 2 |

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
