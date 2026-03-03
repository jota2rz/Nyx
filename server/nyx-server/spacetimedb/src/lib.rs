// Nyx MMO — SpacetimeDB Server Module
//
// Architecture: Option 4 — Hybrid A + C + D
//   UE5 Dedicated Server(s) → SpacetimeDB (single-connection model)
//   Clients connect ONLY to UE5 via UDP. SpacetimeDB is internal-only.
//
// SpacetimeDB responsibilities:
//   - Player accounts & authentication (EOS identity linking)
//   - Character persistence (stats, inventory, equipment)
//   - Combat compute (damage resolution, buff management — Pattern C)
//   - Zone server orchestration (auto-scaling — Pattern A)
//   - Social features (guilds, chat, auction house)
//   - Audit/event logging
//
// Tables:
//   Core:
//     - player_account: Links SpacetimeDB Identity to EOS ProductUserId
//     - player: Active player state (position, chunk, display name)
//     - character_stats: Persistent character data (level, HP, stats, equipment)
//     - world_entity: Non-player entities for spatial interest
//   Orchestration:
//     - zone_server: Registry of active UE5 dedicated servers per zone
//     - zone_population: Aggregated zone population for auto-scaling decisions
//   Combat (Pattern C):
//     - combat_event: Audit log of all combat interactions
//   Benchmark (Phase 0 spikes — preserved for reference):
//     - physics_body, bench_counter, bench_entity, bench_tick_log, etc.
//
// Auth flow:
//   1. Client connects to UE5 Dedicated Server via UDP
//   2. UE5 server authenticates client via EOS
//   3. UE5 server calls SpacetimeDB: authenticate_with_eos(eos_puid, ...)
//   4. UE5 server calls SpacetimeDB: load_character(identity) → gets stats
//   5. UE5 server creates player actor with loaded data

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

// ─── Orchestration Tables (Pattern A: Entity-Sharded MultiServer) ──

/// Registry of active UE5 dedicated servers. Each server registers itself
/// on startup and updates entity_count as players join/leave.
/// The orchestrator watches this table to make scaling decisions.
#[spacetimedb::table(accessor = zone_server, public)]
pub struct ZoneServer {
    #[primary_key]
    pub server_id: String,
    /// Which zone this server is running (e.g., "castle_siege", "plains_nw")
    pub zone_id: String,
    /// Internal network address (e.g., "10.0.1.5")
    pub container_ip: String,
    /// UDP port for client connections
    pub port: u16,
    /// Current number of entities this server is authoritative over
    pub entity_count: u32,
    /// Maximum entities before requesting scale-up
    pub max_entities: u32,
    /// "active", "draining", "standby"
    pub status: String,
    /// When this server joined the mesh
    pub joined_at: Timestamp,
    /// Last heartbeat from this server
    pub last_heartbeat: Timestamp,
}

/// Aggregated zone population tracking. Updated by reducers when
/// servers register/deregister or entity counts change.
/// The orchestrator subscribes to this table for scaling decisions.
#[spacetimedb::table(accessor = zone_population, public)]
pub struct ZonePopulation {
    #[primary_key]
    pub zone_id: String,
    /// Total players across all servers in this zone
    pub total_players: u32,
    /// Number of active servers for this zone
    pub server_count: u32,
    /// Orchestrator sets these flags based on thresholds
    pub needs_scale_up: bool,
    pub needs_scale_down: bool,
    pub last_updated: Timestamp,
}

// ─── Character Persistence Tables ──────────────────────────────────

/// Persistent character data. Loaded when a player joins a zone,
/// dirty-flushed periodically by the dedicated server,
/// and saved on disconnect.
#[spacetimedb::table(accessor = character_stats, public)]
pub struct CharacterStats {
    #[primary_key]
    pub identity: Identity,
    pub display_name: String,
    pub level: u32,
    pub experience: u64,
    pub max_hp: i32,
    pub current_hp: i32,
    pub max_mp: i32,
    pub current_mp: i32,
    // Base stats
    pub strength: u32,
    pub dexterity: u32,
    pub intelligence: u32,
    pub constitution: u32,
    // Derived/cached stats (recalculated from equipment + base)
    pub attack_power: u32,
    pub defense: u32,
    pub magic_power: u32,
    pub magic_defense: u32,
    // Position persistence (saved periodically, loaded on join)
    pub saved_pos_x: f64,
    pub saved_pos_y: f64,
    pub saved_pos_z: f64,
    pub saved_zone_id: String,
    pub last_saved: Timestamp,
}

// ─── Combat Compute Tables (Pattern C) ─────────────────────────────

/// Audit log of combat events. Every hit, heal, buff application
/// is recorded here for debugging, anti-cheat, and analytics.
/// Uses event-table semantics — rows are append-only, auto-pruned.
#[spacetimedb::table(accessor = combat_event, public)]
pub struct CombatEvent {
    #[primary_key]
    #[auto_inc]
    pub id: u64,
    /// Identity of the attacker/source
    pub source_id: Identity,
    /// Identity of the target
    pub target_id: Identity,
    /// Skill/ability ID used
    pub skill_id: u32,
    /// Damage dealt (negative for heals)
    pub damage: i32,
    /// Result HP of the target after this event
    pub result_hp: i32,
    /// Whether the target died from this event
    pub is_kill: bool,
    /// Additional context (crit, resist, absorbed, etc.)
    pub flags: String,
    pub timestamp: Timestamp,
}

// ─── Orchestration Reducers ────────────────────────────────────────

/// Helper: recalculate zone population from all active servers.
fn recalculate_zone_population(ctx: &ReducerContext, zone_id: &str) {
    let mut total_players: u32 = 0;
    let mut server_count: u32 = 0;

    for server in ctx.db.zone_server().iter() {
        if server.zone_id == zone_id && server.status == "active" {
            total_players += server.entity_count;
            server_count += 1;
        }
    }

    // Scale thresholds: per-server max of 300
    let needs_scale_up = server_count > 0
        && total_players > server_count * 250; // >250 avg per server
    let needs_scale_down = server_count > 1
        && total_players < (server_count - 1) * 200; // could fit in N-1 servers

    if let Some(mut pop) = ctx.db.zone_population().zone_id().find(zone_id.to_string()) {
        pop.total_players = total_players;
        pop.server_count = server_count;
        pop.needs_scale_up = needs_scale_up;
        pop.needs_scale_down = needs_scale_down;
        pop.last_updated = ctx.timestamp;
        ctx.db.zone_population().zone_id().update(pop);
    } else {
        ctx.db.zone_population().insert(ZonePopulation {
            zone_id: zone_id.to_string(),
            total_players,
            server_count,
            needs_scale_up,
            needs_scale_down,
            last_updated: ctx.timestamp,
        });
    }
}

/// Called by a UE5 dedicated server on startup to register itself.
#[spacetimedb::reducer]
pub fn register_zone_server(
    ctx: &ReducerContext,
    server_id: String,
    zone_id: String,
    container_ip: String,
    port: u16,
    max_entities: u32,
) {
    // Remove any stale entry with same server_id
    if ctx.db.zone_server().server_id().find(server_id.clone()).is_some() {
        ctx.db.zone_server().server_id().delete(server_id.clone());
    }

    ctx.db.zone_server().insert(ZoneServer {
        server_id: server_id.clone(),
        zone_id: zone_id.clone(),
        container_ip,
        port,
        entity_count: 0,
        max_entities,
        status: "active".to_string(),
        joined_at: ctx.timestamp,
        last_heartbeat: ctx.timestamp,
    });

    recalculate_zone_population(ctx, &zone_id);
    log::info!("Zone server registered: {} for zone {}", server_id, zone_id);
}

/// Called by a UE5 dedicated server to deregister (graceful shutdown).
#[spacetimedb::reducer]
pub fn deregister_zone_server(ctx: &ReducerContext, server_id: String) {
    if let Some(server) = ctx.db.zone_server().server_id().find(server_id.clone()) {
        let zone_id = server.zone_id.clone();
        ctx.db.zone_server().server_id().delete(server_id.clone());
        recalculate_zone_population(ctx, &zone_id);
        log::info!("Zone server deregistered: {}", server_id);
    }
}

/// Called periodically by each UE5 server as a heartbeat + entity count update.
#[spacetimedb::reducer]
pub fn server_heartbeat(ctx: &ReducerContext, server_id: String, entity_count: u32) {
    if let Some(mut server) = ctx.db.zone_server().server_id().find(server_id.clone()) {
        let zone_id = server.zone_id.clone();
        server.entity_count = entity_count;
        server.last_heartbeat = ctx.timestamp;
        ctx.db.zone_server().server_id().update(server);
        recalculate_zone_population(ctx, &zone_id);
    }
}

/// Called by orchestrator to mark a server for draining (no new players).
#[spacetimedb::reducer]
pub fn drain_zone_server(ctx: &ReducerContext, server_id: String) {
    if let Some(mut server) = ctx.db.zone_server().server_id().find(server_id.clone()) {
        server.status = "draining".to_string();
        ctx.db.zone_server().server_id().update(server);
        log::info!("Zone server marked for draining: {}", server_id);
    }
}

/// Returns the best server for a new player joining a zone.
/// Picks the active server with the fewest entities.
#[spacetimedb::reducer]
pub fn assign_player_to_zone(
    ctx: &ReducerContext,
    zone_id: String,
    _player_identity: Identity,
) {
    let best = ctx
        .db
        .zone_server()
        .iter()
        .filter(|s| s.zone_id == zone_id && s.status == "active")
        .min_by_key(|s| s.entity_count);

    match best {
        Some(server) => {
            log::info!(
                "Player assigned to server {} (zone {}, {} entities)",
                server.server_id,
                zone_id,
                server.entity_count
            );
            // The dedicated server that receives this player will call
            // server_heartbeat to update its entity_count
        }
        None => {
            log::warn!("No active servers for zone {}", zone_id);
        }
    }
}

// ─── Character Persistence Reducers ────────────────────────────────

/// Called by UE5 dedicated server when a player joins — loads or creates
/// character data and returns it via subscription.
#[spacetimedb::reducer]
pub fn load_character(ctx: &ReducerContext, identity: Identity, display_name: String) {
    if ctx.db.character_stats().identity().find(identity).is_some() {
        log::info!("Character loaded for {:?}", identity);
        // Data already exists — UE5 server reads it via subscription
        return;
    }

    // New character — create with defaults
    ctx.db.character_stats().insert(CharacterStats {
        identity,
        display_name,
        level: 1,
        experience: 0,
        max_hp: 1000,
        current_hp: 1000,
        max_mp: 500,
        current_mp: 500,
        strength: 10,
        dexterity: 10,
        intelligence: 10,
        constitution: 10,
        attack_power: 15,
        defense: 10,
        magic_power: 12,
        magic_defense: 8,
        saved_pos_x: 0.0,
        saved_pos_y: 0.0,
        saved_pos_z: 100.0,
        saved_zone_id: "default".to_string(),
        last_saved: ctx.timestamp,
    });
    log::info!("New character created for {:?}", identity);
}

/// Called periodically by UE5 server to dirty-flush character state.
#[spacetimedb::reducer]
pub fn save_character(
    ctx: &ReducerContext,
    identity: Identity,
    current_hp: i32,
    current_mp: i32,
    pos_x: f64,
    pos_y: f64,
    pos_z: f64,
    zone_id: String,
) {
    if let Some(mut stats) = ctx.db.character_stats().identity().find(identity) {
        stats.current_hp = current_hp;
        stats.current_mp = current_mp;
        stats.saved_pos_x = pos_x;
        stats.saved_pos_y = pos_y;
        stats.saved_pos_z = pos_z;
        stats.saved_zone_id = zone_id;
        stats.last_saved = ctx.timestamp;
        ctx.db.character_stats().identity().update(stats);
    }
}

/// Called by UE5 server when a player levels up.
#[spacetimedb::reducer]
pub fn update_progression(
    ctx: &ReducerContext,
    identity: Identity,
    new_level: u32,
    new_experience: u64,
) {
    if let Some(mut stats) = ctx.db.character_stats().identity().find(identity) {
        stats.level = new_level;
        stats.experience = new_experience;
        // Level-up stat gains (simple formula — would be data-driven in production)
        stats.max_hp = 1000 + (new_level as i32 - 1) * 50;
        stats.current_hp = stats.max_hp; // Full heal on level up
        stats.max_mp = 500 + (new_level as i32 - 1) * 25;
        stats.current_mp = stats.max_mp;
        stats.strength = 10 + (new_level - 1) * 2;
        stats.dexterity = 10 + (new_level - 1) * 2;
        stats.intelligence = 10 + (new_level - 1) * 2;
        stats.constitution = 10 + (new_level - 1) * 2;
        stats.attack_power = stats.strength + 5;
        stats.defense = stats.constitution;
        stats.magic_power = stats.intelligence + 2;
        stats.magic_defense = stats.intelligence / 2 + stats.constitution / 3;
        ctx.db.character_stats().identity().update(stats);
        log::info!("Player {:?} leveled up to {}", identity, new_level);
    }
}

// ─── Combat Compute Reducers (Pattern C) ───────────────────────────

/// Called by UE5 dedicated server when hit detection confirms a melee/skill hit.
/// SpacetimeDB resolves damage using authoritative character stats, applies it
/// atomically, and logs the event. UE5 receives the result via subscription
/// to character_stats and combat_event tables.
#[spacetimedb::reducer]
pub fn resolve_hit(
    ctx: &ReducerContext,
    attacker_id: Identity,
    defender_id: Identity,
    skill_id: u32,
) {
    let attacker = match ctx.db.character_stats().identity().find(attacker_id) {
        Some(a) => a,
        None => {
            log::warn!("resolve_hit: attacker {:?} not found", attacker_id);
            return;
        }
    };

    let mut defender = match ctx.db.character_stats().identity().find(defender_id) {
        Some(d) => d,
        None => {
            log::warn!("resolve_hit: defender {:?} not found", defender_id);
            return;
        }
    };

    // Skip if defender is already dead
    if defender.current_hp <= 0 {
        return;
    }

    // Damage formula: attack_power * skill_modifier - defense
    // skill_id 0 = basic attack (1.0x), skill_id 1 = power attack (1.5x), etc.
    let skill_modifier: f64 = match skill_id {
        0 => 1.0,   // Basic attack
        1 => 1.5,   // Power strike
        2 => 2.0,   // Critical strike
        3 => 0.8,   // Quick jab (fast, weak)
        10 => 1.2,  // Fire bolt (magic)
        11 => 1.8,  // Fireball (magic AoE)
        _ => 1.0,
    };

    // Determine if physical or magical (skill_id >= 10 = magic)
    let raw_damage = if skill_id >= 10 {
        (attacker.magic_power as f64 * skill_modifier) as i32 - defender.magic_defense as i32
    } else {
        (attacker.attack_power as f64 * skill_modifier) as i32 - defender.defense as i32
    };

    // Minimum 1 damage
    let damage = raw_damage.max(1);

    // Apply damage atomically
    defender.current_hp = (defender.current_hp - damage).max(0);
    let is_kill = defender.current_hp <= 0;
    let result_hp = defender.current_hp;

    ctx.db.character_stats().identity().update(defender);

    // Log combat event
    ctx.db.combat_event().insert(CombatEvent {
        id: 0, // auto_inc
        source_id: attacker_id,
        target_id: defender_id,
        skill_id,
        damage,
        result_hp,
        is_kill,
        flags: if is_kill { "KILL".to_string() } else { String::new() },
        timestamp: ctx.timestamp,
    });

    if is_kill {
        log::info!(
            "KILL: {:?} killed {:?} with skill {} for {} damage",
            attacker_id, defender_id, skill_id, damage
        );
    }
}

/// Batch resolve: for AoE skills that hit multiple targets in one frame.
/// UE5 server sends all targets at once for atomic resolution.
#[spacetimedb::reducer]
pub fn resolve_hit_batch(
    ctx: &ReducerContext,
    attacker_id: Identity,
    defender_ids: Vec<Identity>,
    skill_id: u32,
) {
    for defender_id in defender_ids {
        // Delegate to single-target resolver (runs in same transaction)
        resolve_hit(ctx, attacker_id, defender_id, skill_id);
    }
}

/// Heal a character. Called by UE5 server when a heal skill lands.
#[spacetimedb::reducer]
pub fn resolve_heal(
    ctx: &ReducerContext,
    healer_id: Identity,
    target_id: Identity,
    skill_id: u32,
) {
    let healer = match ctx.db.character_stats().identity().find(healer_id) {
        Some(h) => h,
        None => return,
    };

    let mut target = match ctx.db.character_stats().identity().find(target_id) {
        Some(t) => t,
        None => return,
    };

    // Heal formula: magic_power * skill_modifier
    let skill_modifier: f64 = match skill_id {
        20 => 1.0,  // Minor heal
        21 => 2.0,  // Major heal
        22 => 0.5,  // HoT tick
        _ => 1.0,
    };

    let heal_amount = (healer.magic_power as f64 * skill_modifier) as i32;
    target.current_hp = (target.current_hp + heal_amount).min(target.max_hp);
    let result_hp = target.current_hp;
    ctx.db.character_stats().identity().update(target);

    // Log as negative damage
    ctx.db.combat_event().insert(CombatEvent {
        id: 0,
        source_id: healer_id,
        target_id,
        skill_id,
        damage: -heal_amount,
        result_hp,
        is_kill: false,
        flags: "HEAL".to_string(),
        timestamp: ctx.timestamp,
    });
}

// ─── Phase 0 Spike Code (preserved for reference/benchmarking) ────

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
    // Remove active player entry on disconnect
    if let Some(player) = ctx.db.player().identity().find(ctx.sender()) {
        log::info!("Removing player {} on disconnect", player.display_name);
        ctx.db.player().identity().delete(ctx.sender());
    }
}

// ─── Core Game Reducers ────────────────────────────────────────────

/// Called by the UE5 dedicated server after EOS login to link a
/// SpacetimeDB Identity to an EOS ProductUserId.
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

/// Called by UE5 dedicated server to register the player in the active world.
#[spacetimedb::reducer]
pub fn create_player(ctx: &ReducerContext, display_name: String) {
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
        pos_z: 100.0,
        rot_yaw: 0.0,
        chunk_x: pos_to_chunk(spawn_x),
        chunk_y: pos_to_chunk(spawn_y),
        seq: 0,
        last_update: ctx.timestamp,
    });

    log::info!("Player created for {:?}", ctx.sender());
}

/// Move the calling player to a new position.
/// In the current plan, this is called by the UE5 dedicated server
/// for periodic dirty-flush of authoritative positions (NOT per-frame).
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

// ─── Spike 8: Physics Body Table & Sidecar Reducers ───────────────

/// Physics-simulated objects. Clients insert rows (e.g. spawn_projectile),
/// the UE5 sidecar picks them up, runs physics, and writes positions
/// back via the physics_update reducer.
#[spacetimedb::table(accessor = physics_body, public)]
pub struct PhysicsBody {
    #[primary_key]
    #[auto_inc]
    pub entity_id: u64,
    pub body_type: String,
    pub pos_x: f64,
    pub pos_y: f64,
    pub pos_z: f64,
    pub vel_x: f64,
    pub vel_y: f64,
    pub vel_z: f64,
    pub active: bool,
    pub owner: Identity,
    pub last_update: Timestamp,
}

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

// ─── Test/Debug Reducers ───────────────────────────────────────────

/// Smoke-test for the combat compute pipeline.
/// Creates two synthetic characters, runs a hit + heal, then verifies results.
/// Results are logged — inspect via `spacetime logs nyx`.
#[spacetimedb::reducer]
pub fn test_combat_pipeline(ctx: &ReducerContext) {
    // Create two synthetic identities (deterministic, non-zero)
    let atk_bytes: [u8; 32] = {
        let mut b = [0u8; 32];
        b[31] = 1;
        b
    };
    let def_bytes: [u8; 32] = {
        let mut b = [0u8; 32];
        b[31] = 2;
        b
    };
    let attacker_id = Identity::from_byte_array(atk_bytes);
    let defender_id = Identity::from_byte_array(def_bytes);

    // Clean up any prior test data
    if ctx.db.character_stats().identity().find(attacker_id).is_some() {
        ctx.db.character_stats().identity().delete(attacker_id);
    }
    if ctx.db.character_stats().identity().find(defender_id).is_some() {
        ctx.db.character_stats().identity().delete(defender_id);
    }

    // Create attacker: ATK 15, DEF 10
    ctx.db.character_stats().insert(CharacterStats {
        identity: attacker_id,
        display_name: "TestAttacker".to_string(),
        level: 1,
        experience: 0,
        max_hp: 1000,
        current_hp: 1000,
        max_mp: 500,
        current_mp: 500,
        strength: 10,
        dexterity: 10,
        intelligence: 10,
        constitution: 10,
        attack_power: 15,
        defense: 10,
        magic_power: 12,
        magic_defense: 8,
        saved_pos_x: 0.0,
        saved_pos_y: 0.0,
        saved_pos_z: 100.0,
        saved_zone_id: "test".to_string(),
        last_saved: ctx.timestamp,
    });

    // Create defender: HP 100 (low so we can kill), DEF 5
    ctx.db.character_stats().insert(CharacterStats {
        identity: defender_id,
        display_name: "TestDefender".to_string(),
        level: 1,
        experience: 0,
        max_hp: 100,
        current_hp: 100,
        max_mp: 100,
        current_mp: 100,
        strength: 8,
        dexterity: 8,
        intelligence: 8,
        constitution: 8,
        attack_power: 10,
        defense: 5,
        magic_power: 10,
        magic_defense: 5,
        saved_pos_x: 0.0,
        saved_pos_y: 0.0,
        saved_pos_z: 100.0,
        saved_zone_id: "test".to_string(),
        last_saved: ctx.timestamp,
    });

    log::info!("=== COMBAT TEST: Characters created ===");

    // ── Test 1: resolve_hit (skill_id=0 = basic_attack, modifier 1.0)
    // Expected damage: max(1, ATK(15) * 1.0 - DEF(5)) = 10
    // Expected HP: 100 - 10 = 90
    resolve_hit(ctx, attacker_id, defender_id, 0);
    let def_after_hit = ctx.db.character_stats().identity().find(defender_id).unwrap();
    log::info!(
        "Test 1 — basic_attack: damage=10 expected, HP={} (expected 90) — {}",
        def_after_hit.current_hp,
        if def_after_hit.current_hp == 90 { "PASS" } else { "FAIL" }
    );

    // ── Test 2: resolve_hit (skill_id=1 = power_strike, modifier 1.5)
    // Expected damage: max(1, 15 * 1.5 - 5) = max(1, 22 - 5) = 17
    // Expected HP: 90 - 17 = 73
    resolve_hit(ctx, attacker_id, defender_id, 1);
    let def_after_heavy = ctx.db.character_stats().identity().find(defender_id).unwrap();
    log::info!(
        "Test 2 — power_strike: damage=17 expected, HP={} (expected 73) — {}",
        def_after_heavy.current_hp,
        if def_after_heavy.current_hp == 73 { "PASS" } else { "FAIL" }
    );

    // ── Test 3: resolve_heal (skill_id=20 = minor_heal, modifier 1.0)
    // Expected heal: magic_power(12) * 1.0 = 12
    // Expected HP: min(73 + 12, 100) = 85
    resolve_heal(ctx, attacker_id, defender_id, 20);
    let def_after_heal = ctx.db.character_stats().identity().find(defender_id).unwrap();
    log::info!(
        "Test 3 — minor_heal: heal=12 expected, HP={} (expected 85) — {}",
        def_after_heal.current_hp,
        if def_after_heal.current_hp == 85 { "PASS" } else { "FAIL" }
    );
    assert!(def_after_heal.current_hp == 85, "minor_heal result mismatch");

    // ── Test 4: Kill shot — hit until dead
    // HP is 85, keep hitting with basic_attack (10 dmg each)
    // 85/10 = 8.5, so 9 hits to kill
    for _ in 0..9 {
        resolve_hit(ctx, attacker_id, defender_id, 0);
    }
    let def_after_kill = ctx.db.character_stats().identity().find(defender_id).unwrap();
    let last_event = ctx.db.combat_event().iter().filter(|e| e.is_kill).count();
    log::info!(
        "Test 4 — kill shot: HP={} (expected 0), kill_events={} (expected >=1) — {}",
        def_after_kill.current_hp,
        last_event,
        if def_after_kill.current_hp == 0 && last_event >= 1 { "PASS" } else { "FAIL" }
    );

    // ── Test 5: resolve_hit_batch — batch combat (same skill to same target 3x)
    // Reset defender HP to 100 for batch test
    let mut def_reset = ctx.db.character_stats().identity().find(defender_id).unwrap();
    def_reset.current_hp = 100;
    ctx.db.character_stats().identity().update(def_reset);

    // 3 basic_attacks at 10 dmg each: 100 - 30 = 70
    resolve_hit_batch(ctx, attacker_id, vec![defender_id, defender_id, defender_id], 0);
    let def_after_batch = ctx.db.character_stats().identity().find(defender_id).unwrap();
    log::info!(
        "Test 5 — batch(3x basic_attack): HP={} (expected 70) — {}",
        def_after_batch.current_hp,
        if def_after_batch.current_hp == 70 { "PASS" } else { "FAIL" }
    );

    // Count total combat events generated
    let total_events: u64 = ctx.db.combat_event().iter().count() as u64;
    log::info!("=== COMBAT TEST COMPLETE: {} combat events logged ===", total_events);

    // Clean up test data
    ctx.db.character_stats().identity().delete(attacker_id);
    ctx.db.character_stats().identity().delete(defender_id);
    // Leave combat_events for audit inspection
}
