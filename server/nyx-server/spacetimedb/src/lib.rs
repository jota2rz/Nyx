// Nyx MMO — SpacetimeDB Server Module (Spikes 1–4)
//
// Tables:
//   - player: Active player state (position, display name)
//   - player_account: Links SpacetimeDB Identity to EOS ProductUserId
//
// Auth flow:
//   1. Client connects anonymously (gets SpacetimeDB Identity)
//   2. Client calls authenticate_with_eos(eos_puid, display_name, platform)
//   3. Server links Identity ↔ EOS PUID in player_account table
//   4. Client calls create_player to enter the world

use spacetimedb::{Identity, ReducerContext, Table, Timestamp};

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
