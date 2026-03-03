// Nyx MMO — SpacetimeDB Server Module (Spikes 1–9)
//
// Tables:
//   - player: Active player state (position, chunk, display name)
//   - player_account: Links SpacetimeDB Identity to EOS ProductUserId
//   - world_entity: Non-player entities for spatial interest testing
//   - physics_body: Physics-simulated objects managed by the UE5 sidecar
//   - bench_counter: Simple counter for benchmark throughput
//   - bench_entity: Entity table for read/write benchmarks
//   - bench_tick_log: Logs from each scheduled tick
//   - tick_schedule: Scheduled table driving the game tick loop
//   - fanout_entity: Entities for subscription fan-out stress testing (Spike 9)
//   - fanout_schedule: Scheduled table driving the fan-out tick (Spike 9)
//   - fanout_tick_log: Logs from each fan-out tick (Spike 9)
//   - stress_result: Records multi-client contention results (Spike 9)
//
// Auth flow:
//   1. Client connects anonymously (gets SpacetimeDB Identity)
//   2. Client calls authenticate_with_eos(eos_puid, display_name, platform)
//   3. Server links Identity ↔ EOS PUID in player_account table
//   4. Client calls create_player to enter the world
//
// Spatial interest (Spike 5):
//   - World is divided into chunks (CHUNK_SIZE units each)
//   - Players and entities have chunk_x/chunk_y columns
//   - Clients subscribe with WHERE chunk_x/chunk_y BETWEEN min AND max
//   - On movement, server recomputes chunk from position

use spacetimedb::{Identity, ReducerContext, ScheduleAt, Table, TimeDuration, Timestamp};

/// Chunk size in world units (matches UE5 cm: 10000 = 100 meters).
const CHUNK_SIZE: f64 = 10000.0;

/// Compute chunk coordinate from a world position value.
fn pos_to_chunk(pos: f64) -> i32 {
    (pos / CHUNK_SIZE).floor() as i32
}

// ─── Tables ────────────────────────────────────────────────────────

/// Links a SpacetimeDB Identity to an EOS ProductUserId.
/// Created during the authenticate_with_eos reducer call.
#[spacetimedb::table(accessor = player_account, public)]
pub struct PlayerAccount {
    #[primary_key]
    pub identity: Identity,
    /// EOS ProductUserId (cross-platform persistent ID)
    pub eos_product_user_id: String,
    /// Display name from EOS
    pub display_name: String,
    /// Platform the player logged in from (e.g., "Windows", "PS5", "Xbox")
    pub platform: String,
    /// When this account was first linked
    pub created_at: Timestamp,
    /// Most recent login
    pub last_login: Timestamp,
}

/// Every connected player. One row per identity.
/// chunk_x/chunk_y are derived from pos_x/pos_y for spatial subscriptions.
#[spacetimedb::table(accessor = player, public)]
pub struct Player {
    #[primary_key]
    pub identity: Identity,
    pub display_name: String,
    pub pos_x: f64,
    pub pos_y: f64,
    pub pos_z: f64,
    pub rot_yaw: f32,
    /// Spatial chunk coordinate (derived from pos_x / CHUNK_SIZE)
    pub chunk_x: i32,
    /// Spatial chunk coordinate (derived from pos_y / CHUNK_SIZE)
    pub chunk_y: i32,
    /// Move sequence number for client-side prediction reconciliation.
    /// Echoed back in OnUpdate so the client can discard confirmed predictions.
    pub seq: u32,
    pub last_update: Timestamp,
}

/// Non-player world entities (NPCs, items, interactables).
/// Used for spatial interest stress testing in Spike 5.
#[spacetimedb::table(accessor = world_entity, public)]
pub struct WorldEntity {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    pub entity_type: String,
    pub pos_x: f64,
    pub pos_y: f64,
    pub pos_z: f64,
    /// Spatial chunk coordinate (derived from pos_x / CHUNK_SIZE)
    pub chunk_x: i32,
    /// Spatial chunk coordinate (derived from pos_y / CHUNK_SIZE)
    pub chunk_y: i32,
    pub data: String,
}

// ─── Spike 8: Physics Body (UE5 Sidecar) ──────────────────────────

/// Physics-simulated objects. Clients insert rows (e.g. spawn_projectile),
/// the UE5 sidecar picks them up, runs physics, and writes positions
/// back via the physics_update reducer. Clients see the updates via
/// OnUpdate subscription callbacks.
#[spacetimedb::table(accessor = physics_body, public)]
pub struct PhysicsBody {
    #[primary_key]
    #[auto_inc]
    pub entity_id: u64,
    /// Type tag: "projectile", "debris", etc.
    pub body_type: String,
    pub pos_x: f64,
    pub pos_y: f64,
    pub pos_z: f64,
    pub vel_x: f64,
    pub vel_y: f64,
    pub vel_z: f64,
    /// False once the body has been deactivated (hit ground, out of range, etc.)
    pub active: bool,
    /// Identity of the player who spawned this body
    pub owner: Identity,
    pub last_update: Timestamp,
}

// ─── Lifecycle Reducers ────────────────────────────────────────────

#[spacetimedb::reducer(init)]
pub fn init(_ctx: &ReducerContext) {
    log::info!("Nyx server module initialized");
}

#[spacetimedb::reducer(client_connected)]
pub fn client_connected(_ctx: &ReducerContext) {
    log::info!("Client connected: {:?}", _ctx.sender());
}

#[spacetimedb::reducer(client_disconnected)]
pub fn client_disconnected(ctx: &ReducerContext) {
    // Remove player on disconnect
    if let Some(player) = ctx.db.player().identity().find(ctx.sender()) {
        log::info!("Removing player {} on disconnect", player.display_name);
        ctx.db.player().identity().delete(ctx.sender());
    }
}

// ─── Game Reducers ─────────────────────────────────────────────────

/// Called by the client after EOS login to link their SpacetimeDB Identity
/// to their EOS ProductUserId. This must be called before create_player.
///
/// In production, this would validate the EOS id_token JWT server-side.
/// For now, we trust the client-provided PUID (the SpacetimeDB Identity
/// itself provides the security layer — each client gets a unique one).
#[spacetimedb::reducer]
pub fn authenticate_with_eos(
    ctx: &ReducerContext,
    eos_product_user_id: String,
    display_name: String,
    platform: String,
) {
    let identity = ctx.sender();

    if let Some(mut account) = ctx.db.player_account().identity().find(identity) {
        // Returning player — update last login
        account.display_name = display_name.clone();
        account.platform = platform.clone();
        account.last_login = ctx.timestamp;
        ctx.db.player_account().identity().update(account);
        log::info!(
            "Player account updated for {:?} (EOS PUID: {})",
            identity,
            eos_product_user_id
        );
    } else {
        // New player — create account link
        ctx.db.player_account().insert(PlayerAccount {
            identity,
            eos_product_user_id: eos_product_user_id.clone(),
            display_name: display_name.clone(),
            platform: platform.clone(),
            created_at: ctx.timestamp,
            last_login: ctx.timestamp,
        });
        log::info!(
            "New player account created for {:?} (EOS PUID: {})",
            identity,
            eos_product_user_id
        );
    }
}

/// Called after EOS auth to register the player in the world.
#[spacetimedb::reducer]
pub fn create_player(ctx: &ReducerContext, display_name: String) {
    // Prevent duplicate entries
    if ctx.db.player().identity().find(ctx.sender()).is_some() {
        log::warn!("Player already exists for identity {:?}", ctx.sender());
        return;
    }

    let spawn_x: f64 = 0.0;
    let spawn_y: f64 = 0.0;

    ctx.db.player().insert(Player {
        identity: ctx.sender(),
        display_name,
        pos_x: spawn_x,
        pos_y: spawn_y,
        pos_z: 100.0, // Spawn slightly above ground
        rot_yaw: 0.0,
        chunk_x: pos_to_chunk(spawn_x),
        chunk_y: pos_to_chunk(spawn_y),
        seq: 0,
        last_update: ctx.timestamp,
    });

    log::info!("Player created for {:?}", ctx.sender());
}

/// Move the calling player to a new position.
/// `seq` is the client's prediction sequence number, echoed back for reconciliation.
#[spacetimedb::reducer]
pub fn move_player(ctx: &ReducerContext, x: f64, y: f64, z: f64, yaw: f32, seq: u32) {
    if let Some(mut player) = ctx.db.player().identity().find(ctx.sender()) {
        player.pos_x = x;
        player.pos_y = y;
        player.pos_z = z;
        player.rot_yaw = yaw;
        player.chunk_x = pos_to_chunk(x);
        player.chunk_y = pos_to_chunk(y);
        player.seq = seq;
        player.last_update = ctx.timestamp;
        ctx.db.player().identity().update(player);
    } else {
        log::warn!("move_player called but no player for {:?}", ctx.sender());
    }
}

// ─── Spike 5: Spatial Interest Stress-Test Reducers ────────────────

/// Seed the world with `count` entities spread across a grid of chunks.
/// Distributes entities in a square pattern centered on the origin.
/// Each entity is placed at a random-ish position within its chunk.
#[spacetimedb::reducer]
pub fn seed_entities(ctx: &ReducerContext, count: u32) {
    // Spread entities across a grid. Side length = sqrt(count).
    let side = (count as f64).sqrt().ceil() as i32;
    let half = side / 2;
    let mut spawned: u32 = 0;

    'outer: for cx in -half..=half {
        for cy in -half..=half {
            if spawned >= count {
                break 'outer;
            }
            // Place entity at center of chunk with slight offset based on index
            let offset = (spawned as f64) * 17.0 % CHUNK_SIZE; // deterministic spread
            let px = (cx as f64) * CHUNK_SIZE + offset;
            let py = (cy as f64) * CHUNK_SIZE + offset;

            ctx.db.world_entity().insert(WorldEntity {
                id: 0, // auto_inc
                entity_type: "npc".to_string(),
                pos_x: px,
                pos_y: py,
                pos_z: 0.0,
                chunk_x: cx,
                chunk_y: cy,
                data: format!("entity_{}", spawned),
            });
            spawned += 1;
        }
    }

    log::info!("Seeded {} world entities across {} chunks", spawned, (2 * half + 1) * (2 * half + 1));
}

/// Remove all world entities (for re-seeding).
#[spacetimedb::reducer]
pub fn clear_entities(ctx: &ReducerContext) {
    let mut removed: u32 = 0;
    // Iterate and delete all
    for entity in ctx.db.world_entity().iter() {
        ctx.db.world_entity().id().delete(entity.id);
        removed += 1;
    }
    log::info!("Cleared {} world entities", removed);
}

// ─── Spike 8: Physics / Sidecar Reducers ──────────────────────────

/// Client spawns a projectile. The UE5 sidecar will pick this up via
/// OnInsert, simulate physics, and write updated positions back.
#[spacetimedb::reducer]
pub fn spawn_projectile(
    ctx: &ReducerContext,
    pos_x: f64,
    pos_y: f64,
    pos_z: f64,
    vel_x: f64,
    vel_y: f64,
    vel_z: f64,
) {
    ctx.db.physics_body().insert(PhysicsBody {
        entity_id: 0, // auto_inc
        body_type: "projectile".to_string(),
        pos_x,
        pos_y,
        pos_z,
        vel_x,
        vel_y,
        vel_z,
        active: true,
        owner: ctx.sender(),
        last_update: ctx.timestamp,
    });
    log::info!(
        "Projectile spawned at ({:.0}, {:.0}, {:.0}) vel=({:.0}, {:.0}, {:.0}) by {:?}",
        pos_x, pos_y, pos_z, vel_x, vel_y, vel_z, ctx.sender()
    );
}

/// Spawn N projectiles in a single reducer call (Spike 9 benchmarking utility).
/// All bodies start at different X positions with the same velocity.
#[spacetimedb::reducer]
pub fn spawn_projectiles_batch(ctx: &ReducerContext, count: u32) {
    for i in 0..count {
        ctx.db.physics_body().insert(PhysicsBody {
            entity_id: 0,
            body_type: "projectile".to_string(),
            pos_x: (i as f64) * 100.0,
            pos_y: 0.0,
            pos_z: 500.0,
            vel_x: 10.0,
            vel_y: 0.0,
            vel_z: 100.0,
            active: true,
            owner: ctx.sender(),
            last_update: ctx.timestamp,
        });
    }
    log::info!("spawn_projectiles_batch: {} bodies spawned", count);
}

/// Sidecar updates a single physics body's position and velocity.
/// Called every sidecar tick for each active body.
#[spacetimedb::reducer]
pub fn physics_update(
    ctx: &ReducerContext,
    entity_id: u64,
    pos_x: f64,
    pos_y: f64,
    pos_z: f64,
    vel_x: f64,
    vel_y: f64,
    vel_z: f64,
    active: bool,
) {
    if let Some(mut body) = ctx.db.physics_body().entity_id().find(entity_id) {
        body.pos_x = pos_x;
        body.pos_y = pos_y;
        body.pos_z = pos_z;
        body.vel_x = vel_x;
        body.vel_y = vel_y;
        body.vel_z = vel_z;
        body.active = active;
        body.last_update = ctx.timestamp;
        ctx.db.physics_body().entity_id().update(body);
    }
}

/// Remove all inactive physics bodies (cleanup).
#[spacetimedb::reducer]
pub fn physics_cleanup(ctx: &ReducerContext) {
    let mut removed = 0u32;
    let ids: Vec<u64> = ctx
        .db
        .physics_body()
        .iter()
        .filter(|b| !b.active)
        .map(|b| b.entity_id)
        .collect();
    for id in ids {
        ctx.db.physics_body().entity_id().delete(id);
        removed += 1;
    }
    log::info!("physics_cleanup: removed {} inactive bodies", removed);
}

/// Remove ALL physics bodies (full reset).
#[spacetimedb::reducer]
pub fn physics_reset(ctx: &ReducerContext) {
    let mut removed = 0u32;
    for body in ctx.db.physics_body().iter() {
        ctx.db.physics_body().entity_id().delete(body.entity_id);
        removed += 1;
    }
    log::info!("physics_reset: removed {} bodies", removed);
}

// ─── Spike 9: Scalability & 1000-Player Chunk Viability ────────────

// ── Test 1: Batched Physics Update ─────────────────────────────────

/// A single body update within a batch. Passed as Vec to physics_update_batch.
#[derive(spacetimedb::SpacetimeType)]
pub struct BodyUpdate {
    pub entity_id: u64,
    pub pos_x: f64,
    pub pos_y: f64,
    pub pos_z: f64,
    pub vel_x: f64,
    pub vel_y: f64,
    pub vel_z: f64,
    pub active: bool,
}

/// Batched physics update: sidecar sends ALL body updates in a single reducer call.
/// This avoids per-reducer invocation overhead and subscription diff overhead by
/// doing all updates in one transaction with one commit + one diff.
#[spacetimedb::reducer]
pub fn physics_update_batch(ctx: &ReducerContext, updates: Vec<BodyUpdate>) {
    let mut applied = 0u32;
    for u in &updates {
        if let Some(mut body) = ctx.db.physics_body().entity_id().find(u.entity_id) {
            body.pos_x = u.pos_x;
            body.pos_y = u.pos_y;
            body.pos_z = u.pos_z;
            body.vel_x = u.vel_x;
            body.vel_y = u.vel_y;
            body.vel_z = u.vel_z;
            body.active = u.active;
            body.last_update = ctx.timestamp;
            ctx.db.physics_body().entity_id().update(body);
            applied += 1;
        }
    }
    log::info!(
        "physics_update_batch: {}/{} bodies updated",
        applied,
        updates.len()
    );
}

/// Spike 9 Test 1 self-contained benchmark: measures batched update throughput
/// server-side. Spawns `body_count` bodies, then updates all of them in a single
/// batch call, repeated `iterations` times. Uses bench_counter(id=500) to record
/// the iteration count. Compare server logs for timing.
#[spacetimedb::reducer]
pub fn bench_batch_physics(ctx: &ReducerContext, body_count: u32, iterations: u32) {
    // Clean existing bodies
    for body in ctx.db.physics_body().iter() {
        ctx.db.physics_body().entity_id().delete(body.entity_id);
    }
    // Spawn bodies
    let mut body_ids: Vec<u64> = Vec::with_capacity(body_count as usize);
    for i in 0..body_count {
        let body = ctx.db.physics_body().insert(PhysicsBody {
            entity_id: 0,
            body_type: "bench".to_string(),
            pos_x: (i as f64) * 100.0,
            pos_y: 0.0,
            pos_z: 500.0,
            vel_x: 10.0,
            vel_y: 0.0,
            vel_z: 100.0,
            active: true,
            owner: ctx.sender(),
            last_update: ctx.timestamp,
        });
        body_ids.push(body.entity_id);
    }
    log::info!("bench_batch_physics: spawned {} bodies, running {} iterations", body_count, iterations);

    // Run iterations — each iteration updates all bodies
    for iter in 0..iterations {
        for id in &body_ids {
            if let Some(mut body) = ctx.db.physics_body().entity_id().find(*id) {
                body.pos_x += 1.0;
                body.pos_y += 0.5;
                body.pos_z -= 0.1;
                body.last_update = ctx.timestamp;
                ctx.db.physics_body().entity_id().update(body);
            }
        }
        // Log every 10th iteration
        if (iter + 1) % 10 == 0 || iter == 0 {
            log::info!("bench_batch_physics: iteration {}/{} ({} bodies)", iter + 1, iterations, body_count);
        }
    }

    // Record completion
    if let Some(mut c) = ctx.db.bench_counter().id().find(500) {
        c.value = (body_count as u64) * (iterations as u64);
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    } else {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 500,
            value: (body_count as u64) * (iterations as u64),
            last_updated: ctx.timestamp,
        });
    }
    log::info!(
        "bench_batch_physics: COMPLETE — {} bodies × {} iterations = {} total updates",
        body_count, iterations, (body_count as u64) * (iterations as u64)
    );
}

// ── Test 2: Multi-Client Contention ────────────────────────────────

/// Table to record stress test results. Each row captures one measurement sample.
#[spacetimedb::table(accessor = stress_result, public)]
pub struct StressResult {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    /// Number of concurrent clients in this test run
    pub client_count: u32,
    /// Total reducer calls processed during the measurement window
    pub total_calls: u64,
    /// Measurement window duration in milliseconds
    pub window_ms: u64,
    /// Server timestamp when this result was recorded
    pub recorded_at: Timestamp,
}

/// Simple movement reducer for stress testing. Each stress client calls this at N Hz.
/// Uses the same pattern as move_player but on bench_entity rows to avoid
/// interfering with real player state.
#[spacetimedb::reducer]
pub fn stress_move(ctx: &ReducerContext, entity_id: u64, dx: f64, dy: f64) {
    if let Some(mut e) = ctx.db.bench_entity().id().find(entity_id) {
        e.pos_x += dx;
        e.pos_y += dy;
        e.updated_at = ctx.timestamp;
        ctx.db.bench_entity().id().update(e);
    }
    // Increment the stress call counter (id=300)
    if let Some(mut c) = ctx.db.bench_counter().id().find(300) {
        c.value += 1;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    } else {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 300,
            value: 1,
            last_updated: ctx.timestamp,
        });
    }
}

/// Record the stress test results. Called by the test harness after a measurement window.
#[spacetimedb::reducer]
pub fn stress_record(ctx: &ReducerContext, client_count: u32, window_ms: u64) {
    let total_calls = ctx
        .db
        .bench_counter()
        .id()
        .find(300)
        .map(|c| c.value)
        .unwrap_or(0);
    ctx.db.stress_result().insert(StressResult {
        id: 0,
        client_count,
        total_calls,
        window_ms,
        recorded_at: ctx.timestamp,
    });
    // Reset the counter for the next window
    if let Some(mut c) = ctx.db.bench_counter().id().find(300) {
        c.value = 0;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    }
    log::info!(
        "stress_record: {} calls from {} clients in {}ms ({:.0} calls/sec)",
        total_calls,
        client_count,
        window_ms,
        total_calls as f64 / (window_ms as f64 / 1000.0)
    );
}

/// Clear all stress test results.
#[spacetimedb::reducer]
pub fn stress_reset(ctx: &ReducerContext) {
    for r in ctx.db.stress_result().iter() {
        ctx.db.stress_result().id().delete(r.id);
    }
    if let Some(mut c) = ctx.db.bench_counter().id().find(300) {
        c.value = 0;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    }
    log::info!("stress_reset: cleared all stress results");
}

// ── Test 3: Subscription Fan-Out Stress ────────────────────────────

/// Entity for fan-out testing. Each row simulates a "player" whose position
/// is updated every tick. Subscribers see OnUpdate for every row change.
#[spacetimedb::table(accessor = fanout_entity, public)]
pub struct FanoutEntity {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    pub pos_x: f64,
    pub pos_y: f64,
    pub pos_z: f64,
    pub vel_x: f64,
    pub vel_y: f64,
    pub updated_at: Timestamp,
}

/// Scheduling table for the fan-out tick. Controls the update frequency.
#[spacetimedb::table(accessor = fanout_schedule, scheduled(fanout_tick))]
pub struct FanoutSchedule {
    #[primary_key]
    #[auto_inc]
    pub scheduled_id: u64,
    pub scheduled_at: ScheduleAt,
}

/// Log entry for each fan-out tick.
#[spacetimedb::table(accessor = fanout_tick_log, public)]
pub struct FanoutTickLog {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    pub tick_number: u64,
    pub entities_updated: u32,
    pub timestamp: Timestamp,
}

/// Fan-out tick: updates ALL fanout_entity rows each tick.
/// Subscribers receive OnUpdate for every row change — this is the O(N²) test.
#[spacetimedb::reducer]
pub fn fanout_tick(ctx: &ReducerContext, _arg: FanoutSchedule) -> Result<(), String> {
    // Read/increment tick counter (id=400)
    let tick_num = if let Some(mut c) = ctx.db.bench_counter().id().find(400) {
        c.value += 1;
        c.last_updated = ctx.timestamp;
        let n = c.value;
        ctx.db.bench_counter().id().update(c);
        n
    } else {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 400,
            value: 1,
            last_updated: ctx.timestamp,
        });
        1
    };

    // Update all fan-out entities (simulates N players moving)
    let mut updated: u32 = 0;
    let ids: Vec<u64> = ctx.db.fanout_entity().iter().map(|e| e.id).collect();
    for id in &ids {
        if let Some(mut e) = ctx.db.fanout_entity().id().find(*id) {
            e.pos_x += e.vel_x;
            e.pos_y += e.vel_y;
            e.updated_at = ctx.timestamp;
            ctx.db.fanout_entity().id().update(e);
            updated += 1;
        }
    }

    // Log this tick (keep last 50)
    ctx.db.fanout_tick_log().insert(FanoutTickLog {
        id: 0,
        tick_number: tick_num,
        entities_updated: updated,
        timestamp: ctx.timestamp,
    });
    if tick_num > 50 {
        let cutoff = tick_num - 50;
        for row in ctx.db.fanout_tick_log().iter() {
            if row.tick_number <= cutoff {
                ctx.db.fanout_tick_log().id().delete(row.id);
            }
        }
    }

    Ok(())
}

/// Seed N fan-out entities with random-ish velocities.
#[spacetimedb::reducer]
pub fn fanout_seed(ctx: &ReducerContext, count: u32) {
    for i in 0..count {
        let angle = (i as f64) * 0.1;
        ctx.db.fanout_entity().insert(FanoutEntity {
            id: 0,
            pos_x: (i as f64) * 100.0,
            pos_y: (i as f64) * 50.0,
            pos_z: 0.0,
            vel_x: angle.cos() * 5.0,
            vel_y: angle.sin() * 5.0,
            updated_at: ctx.timestamp,
        });
    }
    log::info!("fanout_seed: seeded {} entities", count);
}

/// Start the fan-out tick at the given interval.
#[spacetimedb::reducer]
pub fn fanout_start(ctx: &ReducerContext, interval_ms: u64) {
    // Stop any existing fan-out ticks
    for row in ctx.db.fanout_schedule().iter() {
        ctx.db
            .fanout_schedule()
            .scheduled_id()
            .delete(row.scheduled_id);
    }
    // Reset tick counter
    if let Some(mut c) = ctx.db.bench_counter().id().find(400) {
        c.value = 0;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    }
    // Clear tick logs
    for row in ctx.db.fanout_tick_log().iter() {
        ctx.db.fanout_tick_log().id().delete(row.id);
    }
    // Start
    let interval = TimeDuration::from_micros((interval_ms * 1000) as i64);
    ctx.db.fanout_schedule().insert(FanoutSchedule {
        scheduled_id: 0,
        scheduled_at: interval.into(),
    });
    log::info!("fanout_start: tick every {}ms", interval_ms);
}

/// Stop the fan-out tick.
#[spacetimedb::reducer]
pub fn fanout_stop(ctx: &ReducerContext) {
    let mut stopped = 0u32;
    for row in ctx.db.fanout_schedule().iter() {
        ctx.db
            .fanout_schedule()
            .scheduled_id()
            .delete(row.scheduled_id);
        stopped += 1;
    }
    log::info!("fanout_stop: stopped {} schedules", stopped);
}

/// Clear all fan-out test data.
#[spacetimedb::reducer]
pub fn fanout_reset(ctx: &ReducerContext) {
    // Stop ticks
    for row in ctx.db.fanout_schedule().iter() {
        ctx.db
            .fanout_schedule()
            .scheduled_id()
            .delete(row.scheduled_id);
    }
    // Clear entities
    for e in ctx.db.fanout_entity().iter() {
        ctx.db.fanout_entity().id().delete(e.id);
    }
    // Clear tick logs
    for row in ctx.db.fanout_tick_log().iter() {
        ctx.db.fanout_tick_log().id().delete(row.id);
    }
    // Reset counter
    if let Some(mut c) = ctx.db.bench_counter().id().find(400) {
        c.value = 0;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    }
    log::info!("fanout_reset: cleared all fan-out test data");
}

// ─── Spike 7: WASM Module Performance Benchmarks ──────────────────

/// Simple counter for benchmark throughput measurement.
#[spacetimedb::table(accessor = bench_counter, public)]
pub struct BenchCounter {
    #[primary_key]
    pub id: u32,
    pub value: u64,
    pub last_updated: Timestamp,
}

/// Entity table for read/write benchmark operations.
#[spacetimedb::table(accessor = bench_entity, public)]
pub struct BenchEntity {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    pub health: f32,
    pub pos_x: f64,
    pub pos_y: f64,
    pub updated_at: Timestamp,
}

/// Logs from each scheduled tick — records tick number, entity count, and timing.
#[spacetimedb::table(accessor = bench_tick_log, public)]
pub struct BenchTickLog {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    pub tick_number: u64,
    pub entities_updated: u32,
    pub timestamp: Timestamp,
}

/// Scheduling table for the game tick loop. Insert a row to start ticking,
/// delete it to stop. The `scheduled_at` field controls the repeat interval.
#[spacetimedb::table(accessor = tick_schedule, scheduled(game_tick))]
pub struct TickSchedule {
    #[primary_key]
    #[auto_inc]
    pub scheduled_id: u64,
    pub scheduled_at: ScheduleAt,
}

// ─── Benchmark Reducers: Throughput Testing ────────────────────────

/// SIMPLE: Increment a single counter row.
/// Tests: single-row read + update overhead.
#[spacetimedb::reducer]
pub fn bench_simple(ctx: &ReducerContext) {
    if let Some(mut c) = ctx.db.bench_counter().id().find(1) {
        c.value += 1;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    } else {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 1,
            value: 1,
            last_updated: ctx.timestamp,
        });
    }
}

/// MEDIUM: Read up to 10 entities + update the first one.
/// Tests: multi-row iteration + single update.
#[spacetimedb::reducer]
pub fn bench_medium(ctx: &ReducerContext) {
    let mut count = 0u32;
    let mut first_id: Option<u64> = None;
    for entity in ctx.db.bench_entity().iter() {
        if first_id.is_none() {
            first_id = Some(entity.id);
        }
        count += 1;
        if count >= 10 {
            break;
        }
    }
    if let Some(fid) = first_id {
        if let Some(mut e) = ctx.db.bench_entity().id().find(fid) {
            e.health = (e.health + 1.0) % 100.0;
            e.updated_at = ctx.timestamp;
            ctx.db.bench_entity().id().update(e);
        }
    }
    // Record iteration count
    if let Some(mut c) = ctx.db.bench_counter().id().find(3) {
        c.value += 1;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    } else {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 3,
            value: 1,
            last_updated: ctx.timestamp,
        });
    }
}

/// COMPLEX: Iterate ALL bench entities, sum their health, store result.
/// Tests: full-table scan + aggregation overhead.
#[spacetimedb::reducer]
pub fn bench_complex(ctx: &ReducerContext) {
    let mut _total_health: f64 = 0.0;
    let mut count: u32 = 0;
    for entity in ctx.db.bench_entity().iter() {
        _total_health += entity.health as f64;
        count += 1;
    }
    // Store result in counter id=2
    if let Some(mut c) = ctx.db.bench_counter().id().find(2) {
        c.value = count as u64;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    } else {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 2,
            value: count as u64,
            last_updated: ctx.timestamp,
        });
    }
}

/// BURST: Perform `count` single-row updates inside one reducer call.
/// Tests: raw DB operation throughput within a single reducer invocation.
#[spacetimedb::reducer]
pub fn bench_burst(ctx: &ReducerContext, count: u32) {
    // Ensure counter exists
    if ctx.db.bench_counter().id().find(10).is_none() {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 10,
            value: 0,
            last_updated: ctx.timestamp,
        });
    }
    for _ in 0..count {
        if let Some(mut c) = ctx.db.bench_counter().id().find(10) {
            c.value += 1;
            c.last_updated = ctx.timestamp;
            ctx.db.bench_counter().id().update(c);
        }
    }
    log::info!(
        "bench_burst: {} operations completed",
        count
    );
}

/// BURST UPDATE: Update `count` bench_entity rows in one reducer call.
/// Tests: multi-row update throughput (simulates game tick updating N entities).
#[spacetimedb::reducer]
pub fn bench_burst_update(ctx: &ReducerContext, count: u32) {
    let mut updated: u32 = 0;
    for entity in ctx.db.bench_entity().iter() {
        if updated >= count {
            break;
        }
        let id = entity.id;
        if let Some(mut e) = ctx.db.bench_entity().id().find(id) {
            e.pos_x += 1.0;
            e.pos_y += 0.5;
            e.health = (e.health - 0.1).max(0.0);
            e.updated_at = ctx.timestamp;
            ctx.db.bench_entity().id().update(e);
            updated += 1;
        }
    }
    log::info!(
        "bench_burst_update: updated {} / {} requested entities",
        updated,
        count
    );
}

// ─── Benchmark Reducers: Scheduled Tick ────────────────────────────

/// Scheduled game tick reducer. Fired by the TickSchedule table.
/// Updates ALL bench_entity rows each tick and logs stats.
#[spacetimedb::reducer]
pub fn game_tick(ctx: &ReducerContext, _arg: TickSchedule) -> Result<(), String> {
    // Read current tick number from counter id=100
    let tick_num = if let Some(mut c) = ctx.db.bench_counter().id().find(100) {
        c.value += 1;
        c.last_updated = ctx.timestamp;
        let n = c.value;
        ctx.db.bench_counter().id().update(c);
        n
    } else {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 100,
            value: 1,
            last_updated: ctx.timestamp,
        });
        1
    };

    // Update all bench entities (simulate NPC AI tick)
    let mut updated: u32 = 0;
    let ids: Vec<u64> = ctx.db.bench_entity().iter().map(|e| e.id).collect();
    for id in &ids {
        if let Some(mut e) = ctx.db.bench_entity().id().find(*id) {
            // Simple movement simulation
            e.pos_x += 0.1;
            e.pos_y += 0.05;
            e.health = (e.health - 0.001).max(0.0);
            e.updated_at = ctx.timestamp;
            ctx.db.bench_entity().id().update(e);
            updated += 1;
        }
    }

    // Log this tick (keep only last 100 logs to avoid unbounded growth)
    ctx.db.bench_tick_log().insert(BenchTickLog {
        id: 0,
        tick_number: tick_num,
        entities_updated: updated,
        timestamp: ctx.timestamp,
    });

    // Prune old logs (keep last 100)
    if tick_num > 100 {
        let cutoff = tick_num - 100;
        for row in ctx.db.bench_tick_log().iter() {
            if row.tick_number <= cutoff {
                ctx.db.bench_tick_log().id().delete(row.id);
            }
        }
    }

    Ok(())
}

/// Start the scheduled game tick at the given interval (in milliseconds).
#[spacetimedb::reducer]
pub fn bench_start_tick(ctx: &ReducerContext, interval_ms: u64) {
    // Stop any existing ticks first
    for row in ctx.db.tick_schedule().iter() {
        ctx.db.tick_schedule().scheduled_id().delete(row.scheduled_id);
    }
    // Reset tick counter
    if let Some(mut c) = ctx.db.bench_counter().id().find(100) {
        c.value = 0;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    }
    // Clear tick logs
    for row in ctx.db.bench_tick_log().iter() {
        ctx.db.bench_tick_log().id().delete(row.id);
    }

    let interval = TimeDuration::from_micros((interval_ms * 1000) as i64);
    ctx.db.tick_schedule().insert(TickSchedule {
        scheduled_id: 0,
        scheduled_at: interval.into(),
    });
    log::info!("Game tick started at {}ms interval", interval_ms);
}

/// Stop the scheduled game tick.
#[spacetimedb::reducer]
pub fn bench_stop_tick(ctx: &ReducerContext) {
    let mut stopped = 0u32;
    for row in ctx.db.tick_schedule().iter() {
        ctx.db.tick_schedule().scheduled_id().delete(row.scheduled_id);
        stopped += 1;
    }
    log::info!("Game tick stopped ({} schedules removed)", stopped);
}

// ─── Benchmark Reducers: Memory Stress Test ────────────────────────

/// Allocate `megabytes` MB of memory inside a reducer and write to it.
/// Tests WASM module memory limits.
#[spacetimedb::reducer]
pub fn bench_memory(ctx: &ReducerContext, megabytes: u32) {
    let bytes = (megabytes as usize) * 1024 * 1024;
    let mut data: Vec<u8> = Vec::with_capacity(bytes);
    // Actually write to force allocation (not just reserve)
    for i in 0..bytes {
        data.push((i & 0xFF) as u8);
    }
    // Store something to prevent optimization
    let checksum: u64 = data.iter().map(|b| *b as u64).sum();
    if let Some(mut c) = ctx.db.bench_counter().id().find(200) {
        c.value = checksum;
        c.last_updated = ctx.timestamp;
        ctx.db.bench_counter().id().update(c);
    } else {
        ctx.db.bench_counter().insert(BenchCounter {
            id: 200,
            value: checksum,
            last_updated: ctx.timestamp,
        });
    }
    log::info!(
        "bench_memory: allocated {} MB, checksum={}, ok",
        megabytes,
        checksum
    );
}

// ─── Benchmark Utility Reducers ────────────────────────────────────

/// Seed `count` bench entities for read/write benchmarks.
#[spacetimedb::reducer]
pub fn bench_seed(ctx: &ReducerContext, count: u32) {
    for i in 0..count {
        ctx.db.bench_entity().insert(BenchEntity {
            id: 0,
            health: 100.0,
            pos_x: (i as f64) * 10.0,
            pos_y: (i as f64) * 5.0,
            updated_at: ctx.timestamp,
        });
    }
    log::info!("Seeded {} bench entities", count);
}

/// Clear all benchmark data (counters, entities, tick logs, schedules).
#[spacetimedb::reducer]
pub fn bench_reset(ctx: &ReducerContext) {
    // Stop ticks
    for row in ctx.db.tick_schedule().iter() {
        ctx.db.tick_schedule().scheduled_id().delete(row.scheduled_id);
    }
    for e in ctx.db.bench_entity().iter() {
        ctx.db.bench_entity().id().delete(e.id);
    }
    for c in ctx.db.bench_counter().iter() {
        ctx.db.bench_counter().id().delete(c.id);
    }
    for s in ctx.db.bench_tick_log().iter() {
        ctx.db.bench_tick_log().id().delete(s.id);
    }
    log::info!("Bench data reset");
}
