// Spike 9 Test 3b: Subscription Fan-Out Stress Test with Real WebSocket Subscribers
//
// Connects N actual SpacetimeDB Rust SDK clients, each subscribing to `fanout_entity`.
// A controller connection seeds entities, starts the fanout tick, and after a
// measurement window, stops the tick and reports per-subscriber update counts,
// bandwidth, and whether the server tick interval drifted under load.

mod module_bindings;

use clap::Parser;
use module_bindings::*;
use spacetimedb_sdk::{
    DbContext, Table, TableWithPrimaryKey,
};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

#[derive(Parser, Debug)]
#[command(name = "fanout_stress", about = "SpacetimeDB subscription fan-out stress test")]
struct Args {
    /// Number of subscriber connections
    #[arg(short = 'n', long, default_value_t = 10)]
    subscribers: u32,

    /// Number of entities to seed
    #[arg(short = 'e', long, default_value_t = 100)]
    entities: u32,

    /// Tick interval in milliseconds
    #[arg(short = 'i', long, default_value_t = 100)]
    interval_ms: u64,

    /// Test duration in seconds
    #[arg(short = 'd', long, default_value_t = 15)]
    duration_secs: u64,

    /// SpacetimeDB server URI
    #[arg(short = 'u', long, default_value = "http://127.0.0.1:3000")]
    uri: String,

    /// Database name
    #[arg(long, default_value = "nyx")]
    db: String,
}

struct SubscriberStats {
    update_count: AtomicU64,
    bytes_received: AtomicU64,
    connected: AtomicU64,
    sub_applied: AtomicU64,
}

impl SubscriberStats {
    fn new() -> Self {
        Self {
            update_count: AtomicU64::new(0),
            bytes_received: AtomicU64::new(0),
            connected: AtomicU64::new(0),
            sub_applied: AtomicU64::new(0),
        }
    }
}

fn main() {
    let args = Args::parse();

    println!("=== Spike 9 Test 3b: Subscription Fan-Out Stress ===");
    println!(
        "Subscribers: {} | Entities: {} | Tick: {}ms | Duration: {}s",
        args.subscribers, args.entities, args.interval_ms, args.duration_secs
    );
    println!("Server: {}/database/{}", args.uri, args.db);
    println!();

    // ── Phase 1: Controller connection ──────────────────────────────
    println!("[1/5] Connecting controller...");
    let ctrl = DbConnection::builder()
        .with_uri(&args.uri)
        .with_database_name(&args.db)
        .on_connect(|_ctx, _id, _token| {
            eprintln!("  Controller connected");
        })
        .on_connect_error(|_ctx, err| {
            eprintln!("  Controller connect error: {:?}", err);
            std::process::exit(1);
        })
        .on_disconnect(|_ctx, err| {
            if let Some(e) = err {
                eprintln!("  Controller disconnected with error: {:?}", e);
            }
        })
        .build()
        .expect("Failed to connect controller");

    // Subscribe to bench_counter and fanout_tick_log for monitoring
    ctrl.subscription_builder()
        .on_applied(|_ctx| {
            eprintln!("  Controller subscription applied");
        })
        .subscribe(vec![
            "SELECT * FROM bench_counter".to_string(),
            "SELECT * FROM fanout_tick_log".to_string(),
        ]);

    // Process until subscription is applied
    let ctrl_start = Instant::now();
    loop {
        ctrl.frame_tick().expect("Controller frame_tick failed");
        if ctrl_start.elapsed() > Duration::from_secs(5) {
            eprintln!("  Controller subscription timeout");
            break;
        }
        // Check if we have any bench_counter rows (means subscription is applied)
        if ctrl.db.bench_counter().count() > 0 || ctrl_start.elapsed() > Duration::from_millis(500)
        {
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }

    // ── Phase 2: Reset and seed ─────────────────────────────────────
    println!("[2/5] Resetting and seeding {} entities...", args.entities);
    ctrl.reducers().fanout_reset().expect("fanout_reset failed");
    std::thread::sleep(Duration::from_millis(200));
    ctrl.frame_tick().ok();

    ctrl.reducers()
        .fanout_seed(args.entities)
        .expect("fanout_seed failed");
    std::thread::sleep(Duration::from_millis(500));
    ctrl.frame_tick().ok();
    println!("  Seeded {} entities", args.entities);

    // ── Phase 3: Connect N subscribers ──────────────────────────────
    println!(
        "[3/5] Connecting {} subscribers...",
        args.subscribers
    );

    let stats: Vec<Arc<SubscriberStats>> = (0..args.subscribers)
        .map(|_| Arc::new(SubscriberStats::new()))
        .collect();

    let mut subscriber_conns: Vec<DbConnection> = Vec::new();
    let mut subscriber_threads: Vec<std::thread::JoinHandle<()>> = Vec::new();

    for i in 0..args.subscribers {
        let s = stats[i as usize].clone();
        let uri = args.uri.clone();
        let db = args.db.clone();
        let _entities = args.entities;

        let s_connect = s.clone();
        let conn = DbConnection::builder()
            .with_uri(&uri)
            .with_database_name(&db)
            .on_connect(move |_ctx, _id, _token| {
                s_connect.connected.store(1, Ordering::SeqCst);
            })
            .on_connect_error(|_ctx, err| {
                eprintln!("  Subscriber connect error: {:?}", err);
            })
            .on_disconnect(|_ctx, _err| {})
            .build()
            .expect("Failed to connect subscriber");

        // Register on_update callback BEFORE subscribing
        let s_update = s.clone();
        conn.db.fanout_entity().on_update(move |_ctx, _old, _new| {
            s_update.update_count.fetch_add(1, Ordering::Relaxed);
            // Estimate bytes: each FanoutEntity update ≈ 8+8+8+8+8+8+8 = 56 bytes minimum
            s_update.bytes_received.fetch_add(56, Ordering::Relaxed);
        });

        // Subscribe to fanout_entity
        let s_sub = s.clone();
        conn.subscription_builder()
            .on_applied(move |_ctx| {
                s_sub.sub_applied.store(1, Ordering::SeqCst);
            })
            .subscribe(vec!["SELECT * FROM fanout_entity".to_string()]);

        // Run connection in a dedicated thread
        let handle = conn.run_threaded();
        subscriber_conns.push(conn);
        subscriber_threads.push(handle);

        // Brief pause to avoid thundering herd on the server
        if i % 10 == 9 {
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    // Wait for all subscribers to have their subscription applied
    println!("  Waiting for all subscriptions to be applied...");
    let sub_wait_start = Instant::now();
    loop {
        let applied: u64 = stats.iter().map(|s| s.sub_applied.load(Ordering::SeqCst)).sum();
        if applied == args.subscribers as u64 {
            break;
        }
        if sub_wait_start.elapsed() > Duration::from_secs(30) {
            let connected: u64 = stats.iter().map(|s| s.connected.load(Ordering::SeqCst)).sum();
            eprintln!(
                "  Timeout waiting for subscriptions. Connected: {}/{}, Applied: {}/{}",
                connected, args.subscribers, applied, args.subscribers
            );
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }

    let connected: u64 = stats
        .iter()
        .map(|s| s.connected.load(Ordering::SeqCst))
        .sum();
    let applied: u64 = stats
        .iter()
        .map(|s| s.sub_applied.load(Ordering::SeqCst))
        .sum();
    println!(
        "  {} connected, {} subscriptions applied",
        connected, applied
    );

    // Reset update counts (initial subscription delivery counts as updates)
    for s in &stats {
        s.update_count.store(0, Ordering::SeqCst);
        s.bytes_received.store(0, Ordering::SeqCst);
    }

    // ── Phase 4: Start tick and measure ─────────────────────────────
    println!(
        "[4/5] Starting fanout tick at {}ms interval for {}s...",
        args.interval_ms, args.duration_secs
    );

    let measure_start = Instant::now();

    ctrl.reducers()
        .fanout_start(args.interval_ms)
        .expect("fanout_start failed");

    // Measurement loop
    let check_interval = Duration::from_secs(1);
    let total_duration = Duration::from_secs(args.duration_secs);

    while measure_start.elapsed() < total_duration {
        ctrl.frame_tick().ok();
        std::thread::sleep(check_interval.min(total_duration - measure_start.elapsed().min(total_duration)));

        let elapsed = measure_start.elapsed().as_secs_f64();
        let total_updates: u64 = stats
            .iter()
            .map(|s| s.update_count.load(Ordering::Relaxed))
            .sum();
        let avg_per_sub = total_updates as f64 / args.subscribers as f64;
        let rate = total_updates as f64 / elapsed;

        print!(
            "\r  [{:.0}s] Total updates: {} | Avg/sub: {:.0} | Rate: {:.0}/sec    ",
            elapsed, total_updates, avg_per_sub, rate
        );
    }
    println!();

    // Stop tick
    ctrl.reducers().fanout_stop().expect("fanout_stop failed");
    std::thread::sleep(Duration::from_millis(200));
    ctrl.frame_tick().ok();

    let measure_elapsed = measure_start.elapsed();

    // ── Phase 5: Report results ─────────────────────────────────────
    println!("[5/5] Results:");
    println!();

    let total_updates: u64 = stats
        .iter()
        .map(|s| s.update_count.load(Ordering::Relaxed))
        .sum();
    let total_bytes: u64 = stats
        .iter()
        .map(|s| s.bytes_received.load(Ordering::Relaxed))
        .sum();

    let elapsed_secs = measure_elapsed.as_secs_f64();
    let expected_ticks = (args.duration_secs * 1000) / args.interval_ms;
    let expected_updates_per_sub = expected_ticks * args.entities as u64;
    let expected_total = expected_updates_per_sub * args.subscribers as u64;

    println!("  Duration:       {:.1}s", elapsed_secs);
    println!("  Subscribers:    {}", args.subscribers);
    println!("  Entities:       {}", args.entities);
    println!("  Tick interval:  {}ms", args.interval_ms);
    println!();
    println!(
        "  Expected ticks:        {}",
        expected_ticks
    );
    println!(
        "  Expected updates/sub:  {} ({} entities × {} ticks)",
        expected_updates_per_sub, args.entities, expected_ticks
    );
    println!(
        "  Expected total:        {} ({} updates × {} subscribers)",
        expected_total, expected_updates_per_sub, args.subscribers
    );
    println!();
    println!(
        "  Actual total updates:  {}",
        total_updates
    );
    println!(
        "  Actual avg/subscriber: {:.0}",
        total_updates as f64 / args.subscribers as f64
    );
    println!(
        "  Delivery ratio:        {:.1}%",
        (total_updates as f64 / expected_total as f64) * 100.0
    );
    println!();
    println!(
        "  Update rate (total):   {:.0} events/sec",
        total_updates as f64 / elapsed_secs
    );
    println!(
        "  Update rate (per sub): {:.0} events/sec",
        total_updates as f64 / args.subscribers as f64 / elapsed_secs
    );
    println!(
        "  Bandwidth (est.):      {:.1} KB/sec total, {:.1} KB/sec per sub",
        total_bytes as f64 / 1024.0 / elapsed_secs,
        total_bytes as f64 / 1024.0 / args.subscribers as f64 / elapsed_secs
    );

    // Per-subscriber breakdown
    println!();
    println!("  Per-subscriber update counts:");
    let mut min_updates = u64::MAX;
    let mut max_updates = 0u64;
    for (i, s) in stats.iter().enumerate() {
        let count = s.update_count.load(Ordering::Relaxed);
        min_updates = min_updates.min(count);
        max_updates = max_updates.max(count);
        if args.subscribers <= 20 {
            println!("    Sub {}: {} updates", i, count);
        }
    }
    if args.subscribers > 20 {
        println!("    (showing summary for {} subscribers)", args.subscribers);
    }
    println!("    Min: {} | Max: {} | Spread: {}", min_updates, max_updates, max_updates - min_updates);

    // Query server-side tick log for actual tick intervals
    println!();
    println!("  Server-side tick log (from fanout_tick_log):");
    let tick_logs: Vec<_> = ctrl.db.fanout_tick_log().iter().collect();
    if tick_logs.is_empty() {
        println!("    (no tick logs available — subscription may not cover this table)");
    } else {
        let mut sorted = tick_logs.clone();
        sorted.sort_by_key(|t| t.tick_number);
        let last_n: Vec<_> = sorted.iter().rev().take(10).rev().cloned().collect();
        println!("    Last {} ticks:", last_n.len());
        for log in &last_n {
            println!(
                "      Tick {} | entities_updated: {} | timestamp: {:?}",
                log.tick_number, log.entities_updated, log.timestamp
            );
        }

        // Check counter for total ticks
        if let Some(counter) = ctrl.db.bench_counter().id().find(&400) {
            println!("    Total ticks (counter 400): {}", counter.value);
        }
    }

    // Cleanup
    println!();
    println!("  Disconnecting...");
    for conn in &subscriber_conns {
        conn.disconnect().ok();
    }
    ctrl.disconnect().ok();

    println!("Done!");
}
