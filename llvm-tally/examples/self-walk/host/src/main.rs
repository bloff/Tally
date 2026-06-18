/*
 * Host runner for the llvm-tally self-contained random-walk benchmark. It
 * loads the instrumented Rust workload and reports each workload-owned vertex
 * counter after running fixed scheduler metacycles.
 */
use llvm_tally_runtime::TallyManager;
use std::env;
use std::error::Error;
use std::ffi::c_void;
use std::path::PathBuf;

const N_THREADS: usize = 10;

#[repr(C)]
struct SelfWalkArgs {
    current_node: u32,
    seed: u32,
    vertices_walked: u64,
}

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = env::args().collect();
    let workload = PathBuf::from(
        args.get(1)
            .cloned()
            .unwrap_or_else(|| "llvm-tally/dl/examples/self-walk/self_walk.so".to_string()),
    );
    let metacycles = parse_arg(&args, 2, 1000_u64);
    let base_budget = parse_arg(&args, 3, 100_i64);
    let budget_step = parse_arg(&args, 4, 100_i64);

    let mut manager = TallyManager::new();
    let entry = manager.load_function(&workload, "run_self_walk")?;

    let mut walk_args: Vec<Box<SelfWalkArgs>> = (0..N_THREADS)
        .map(|i| {
            Box::new(SelfWalkArgs {
                current_node: (i as u32) & 1023,
                seed: 0x9e37_79b9_u32 ^ (i as u32).wrapping_mul(0x85eb_ca6b),
                vertices_walked: 0,
            })
        })
        .collect();

    for (i, args) in walk_args.iter_mut().enumerate() {
        let budget = base_budget + budget_step * i as i64;
        manager.spawn(
            entry,
            &mut **args as *mut SelfWalkArgs as *mut c_void,
            budget,
        )?;
    }

    manager.run_cycles(metacycles)?;
    let stats = manager.stats();
    let baseline_budget = stats
        .first()
        .map(|s| s.budget_per_cycle.max(1))
        .unwrap_or(1);
    let baseline_vertices = walk_args
        .first()
        .map(|args| args.vertices_walked.max(1))
        .unwrap_or(1);

    println!("metacycles: {metacycles}");
    println!("threads: {N_THREADS}");
    println!("base_budget: {base_budget}");
    println!("budget_step: {budget_step}");
    println!();
    println!(
        "thread,budget_per_metacycle,total_budget,vertices_walked,vertices_per_metacycle,vertices_per_1000_cycles,budget_multiple,work_multiple,linearity_ratio,remaining_budget,charges"
    );
    for (i, stat) in stats.iter().enumerate() {
        let budget = stat.budget_per_cycle;
        let total_budget = budget * metacycles as i64;
        let vertices = walk_args[i].vertices_walked;
        let vertices_per_metacycle = vertices as f64 / metacycles.max(1) as f64;
        let vertices_per_1000_cycles = vertices as f64 * 1000.0 / (total_budget.max(1) as f64);
        let budget_multiple = budget as f64 / baseline_budget as f64;
        let work_multiple = vertices as f64 / baseline_vertices as f64;
        let linearity_ratio = work_multiple / budget_multiple.max(f64::MIN_POSITIVE);

        println!(
            "{},{},{},{},{:.6},{:.6},{:.6},{:.6},{:.6},{},{}",
            stat.id,
            budget,
            total_budget,
            vertices,
            vertices_per_metacycle,
            vertices_per_1000_cycles,
            budget_multiple,
            work_multiple,
            linearity_ratio,
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
