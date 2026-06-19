/*
 * Host runner for the llvm-tally self-contained random-walk benchmark. It runs
 * k instrumented Rust minithreads with the same budget until they collectively
 * traverse a requested number of synthetic graph edges.
 */
use llvm_tally_runtime::{TallyManager, ThreadState};
use std::env;
use std::error::Error;
use std::ffi::c_void;
use std::path::PathBuf;
use std::time::Instant;

const DEFAULT_TARGET_EDGES: u64 = 50_000_000;
const BENCHMARK_STACK_SIZE: usize = 64 * 1024;

#[repr(C)]
struct SelfWalkArgs {
    current_node: u32,
    seed: u32,
    vertices_walked: u64,
    target_vertices: u64,
}

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = env::args().collect();
    let workload = PathBuf::from(
        args.get(1)
            .cloned()
            .unwrap_or_else(|| "llvm-tally/dl/examples/self-walk/self_walk.so".to_string()),
    );
    let thread_count = parse_arg(&args, 2, 10_usize);
    let budget = parse_arg(&args, 3, 100_i64);
    let target_edges = parse_arg(&args, 4, DEFAULT_TARGET_EDGES);
    let stack_size = parse_arg(&args, 5, BENCHMARK_STACK_SIZE);
    let summary_only = parse_bool_arg(&args, 6);

    if thread_count == 0 {
        return Err("thread_count must be positive".into());
    }
    if budget <= 0 {
        return Err("budget must be positive".into());
    }

    let mut manager = TallyManager::new();
    let entry = manager.load_function(&workload, "run_self_walk")?;

    let mut walk_args: Vec<Box<SelfWalkArgs>> = (0..thread_count)
        .map(|i| {
            Box::new(SelfWalkArgs {
                current_node: (i as u32) & 1023,
                seed: 0x9e37_79b9_u32 ^ (i as u32).wrapping_mul(0x85eb_ca6b),
                vertices_walked: 0,
                target_vertices: target_for_thread(target_edges, thread_count, i),
            })
        })
        .collect();

    let mut active = Vec::with_capacity(thread_count);
    let mut active_count = 0_usize;
    for args in walk_args.iter_mut() {
        active.push(args.target_vertices > 0);
        if args.target_vertices > 0 {
            active_count += 1;
        }
        manager.spawn_with_stack(
            entry,
            &mut **args as *mut SelfWalkArgs as *mut c_void,
            budget,
            stack_size,
        )?;
    }

    let mut scheduler_cycles = 0_u64;
    let mut thread_cycles = 0_u64;
    let mut budget_units_consumed = 0_u64;
    let run_start = Instant::now();
    while active_count > 0 {
        scheduler_cycles += 1;
        for id in 0..thread_count {
            if !active[id] {
                continue;
            }

            let before = manager.stats_for_thread(id)?;
            let state = manager.run_cycle(id)?;
            let after = manager.stats_for_thread(id)?;
            let charged_budget = (budget + before.remaining_budget - after.remaining_budget).max(0);
            budget_units_consumed = budget_units_consumed.saturating_add(charged_budget as u64);
            thread_cycles += 1;
            if state == ThreadState::Returned
                || walk_args[id].vertices_walked >= walk_args[id].target_vertices
            {
                active[id] = false;
                active_count -= 1;
            } else if state == ThreadState::Errored {
                return Err(format!("thread {id} errored").into());
            }
        }
    }
    let run_seconds = run_start.elapsed().as_secs_f64();

    let stats = manager.stats();
    let total_edges: u64 = walk_args.iter().map(|args| args.vertices_walked).sum();

    println!("threads: {thread_count}");
    println!("budget_per_cycle: {budget}");
    println!("target_edges: {target_edges}");
    println!("total_edges: {total_edges}");
    println!("run_seconds: {run_seconds:.9}");
    println!("scheduler_cycles: {scheduler_cycles}");
    println!("thread_cycles: {thread_cycles}");
    println!("budget_units_consumed: {budget_units_consumed}");
    println!("stack_bytes: {stack_size}");
    println!("summary_only: {}", u8::from(summary_only));
    println!();

    if summary_only {
        return Ok(());
    }

    println!(
        "thread,budget_per_cycle,target_edges,vertices_walked,cycles_run,remaining_budget,charges"
    );
    for (i, stat) in stats.iter().enumerate() {
        println!(
            "{},{},{},{},{},{},{}",
            stat.id,
            budget,
            walk_args[i].target_vertices,
            walk_args[i].vertices_walked,
            stat.cycles_run,
            stat.remaining_budget,
            stat.charges
        );
    }

    Ok(())
}

fn parse_arg<T>(args: &[String], index: usize, fallback: T) -> T
where
    T: std::str::FromStr,
{
    args.get(index)
        .and_then(|value| value.parse::<T>().ok())
        .unwrap_or(fallback)
}

fn parse_bool_arg(args: &[String], index: usize) -> bool {
    args.get(index)
        .map(|value| !value.is_empty() && value != "0")
        .unwrap_or(false)
}

fn target_for_thread(total_edges: u64, thread_count: usize, index: usize) -> u64 {
    let base = total_edges / thread_count as u64;
    let remainder = total_edges % thread_count as u64;
    base + u64::from((index as u64) < remainder)
}
