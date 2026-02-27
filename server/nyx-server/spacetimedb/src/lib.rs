// Nyx MMO — Minimal SpacetimeDB Server Module (Spike 1+2)
//
// This is the absolute minimum schema needed to:
//   1. Generate the Unreal C++ SDK bindings
//   2. Test a round-trip connection from UE5
//   3. Validate player insert/update/delete flow

use spacetimedb::{Identity, ReducerContext, Table, Timestamp};

// ─── Tables ────────────────────────────────────────────────────────

/// Every connected player. One row per identity.
#[spacetimedb::table(accessor = player, public)]
pub struct Player {
    #[primary_key]
    pub identity: Identity,
    pub display_name: String,
    pub pos_x: f64,
    pub pos_y: f64,
    pub pos_z: f64,
    pub rot_yaw: f32,
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

/// Called after EOS auth to register the player in the world.
#[spacetimedb::reducer]
pub fn create_player(ctx: &ReducerContext, display_name: String) {
    // Prevent duplicate entries
    if ctx.db.player().identity().find(ctx.sender()).is_some() {
        log::warn!("Player already exists for identity {:?}", ctx.sender());
        return;
    }

    ctx.db.player().insert(Player {
        identity: ctx.sender(),
        display_name,
        pos_x: 0.0,
        pos_y: 0.0,
        pos_z: 100.0, // Spawn slightly above ground
        rot_yaw: 0.0,
        last_update: ctx.timestamp,
    });

    log::info!("Player created for {:?}", ctx.sender());
}

/// Move the calling player to a new position.
#[spacetimedb::reducer]
pub fn move_player(ctx: &ReducerContext, x: f64, y: f64, z: f64, yaw: f32) {
    if let Some(mut player) = ctx.db.player().identity().find(ctx.sender()) {
        player.pos_x = x;
        player.pos_y = y;
        player.pos_z = z;
        player.rot_yaw = yaw;
        player.last_update = ctx.timestamp;
        ctx.db.player().identity().update(player);
    } else {
        log::warn!("move_player called but no player for {:?}", ctx.sender());
    }
}
