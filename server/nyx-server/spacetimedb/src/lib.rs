// Nyx MMO — SpacetimeDB Server Module (Spikes 1–6)
//
// Tables:
//   - player: Active player state (position, chunk, display name)
//   - player_account: Links SpacetimeDB Identity to EOS ProductUserId
//   - world_entity: Non-player entities for spatial interest testing
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

use spacetimedb::{Identity, ReducerContext, Table, Timestamp};

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
