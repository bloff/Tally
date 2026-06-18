/*
 * Host demo for llvm-tally. It exports graph functions, loads the instrumented
 * Rust workload, and runs ten minithreads with linearly increasing budgets.
 */
use llvm_tally_runtime::{record_current_thread_work, TallyManager};
use std::env;
use std::error::Error;
use std::ffi::c_void;
use std::fs;
use std::path::PathBuf;
use std::sync::OnceLock;

const N_THREADS: usize = 10;

#[repr(C)]
#[derive(Clone, Copy)]
struct WalkArgs {
    current_node: u32,
    seed: u32,
}

struct Graph {
    edges: Vec<Vec<u32>>,
}

static GRAPH: OnceLock<Graph> = OnceLock::new();

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = env::args().collect();
    let workload = PathBuf::from(
        args.get(1)
            .cloned()
            .unwrap_or_else(|| "llvm-tally/dl/examples/random-walk/random_walk.so".to_string()),
    );
    let graph_path = PathBuf::from(
        args.get(2)
            .cloned()
            .unwrap_or_else(|| "gcc-tally/data/graph.txt".to_string()),
    );
    let metacycles = parse_arg(&args, 3, 1000_u64);
    let base_budget = parse_arg(&args, 4, 100_i64);
    let budget_step = parse_arg(&args, 5, 100_i64);

    GRAPH
        .set(load_graph(&graph_path)?)
        .map_err(|_| "graph was already initialized")?;

    let mut manager = TallyManager::new();
    let entry = manager.load_function(&workload, "run_walk_budget")?;

    let mut walk_args: Vec<Box<WalkArgs>> = (0..N_THREADS)
        .map(|i| {
            Box::new(WalkArgs {
                current_node: 0,
                seed: 0x1234_5678_u32.wrapping_add(i as u32 * 977),
            })
        })
        .collect();

    for (i, args) in walk_args.iter_mut().enumerate() {
        let budget = base_budget + budget_step * i as i64;
        manager.spawn(entry, &mut **args as *mut WalkArgs as *mut c_void, budget)?;
    }

    manager.run_cycles(metacycles)?;
    let stats = manager.stats();
    let baseline = stats
        .first()
        .map(|s| (s.work_units.max(1), s.budget_per_cycle.max(1)))
        .unwrap_or((1, 1));

    println!(
        "thread,budget_per_metacycle,vertices_walked,work_per_budget,linearity_ratio,remaining_budget,charges"
    );
    for stat in stats {
        let total_budget = stat.budget_per_cycle as f64 * metacycles as f64;
        let work_per_budget = stat.work_units as f64 / total_budget.max(1.0);
        let ideal = baseline.0 as f64 * stat.budget_per_cycle as f64 / baseline.1 as f64;
        let linearity = stat.work_units as f64 / ideal.max(1.0);
        println!(
            "{},{},{},{:.6},{:.6},{},{}",
            stat.id,
            stat.budget_per_cycle,
            stat.work_units,
            work_per_budget,
            linearity,
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

fn load_graph(path: &PathBuf) -> Result<Graph, Box<dyn Error>> {
    let content = fs::read_to_string(path)?;
    let mut lines = content.lines();
    let n_nodes: usize = lines
        .next()
        .ok_or("graph is empty")?
        .trim()
        .parse()
        .map_err(|_| "invalid graph node count")?;

    let mut edges = Vec::with_capacity(n_nodes);
    for line in lines.take(n_nodes) {
        let mut values = line.split_whitespace();
        let degree: usize = values
            .next()
            .ok_or("missing graph degree")?
            .parse()
            .map_err(|_| "invalid graph degree")?;
        let neighbours: Result<Vec<u32>, _> = values.take(degree).map(str::parse).collect();
        edges.push(neighbours.map_err(|_| "invalid graph neighbour")?);
    }

    if edges.len() != n_nodes {
        return Err(format!("expected {n_nodes} graph rows, got {}", edges.len()).into());
    }

    Ok(Graph { edges })
}

fn graph() -> &'static Graph {
    GRAPH.get().expect("graph is initialized before workload runs")
}

#[no_mangle]
pub extern "C" fn tally_graph_node_count() -> u32 {
    graph().edges.len() as u32
}

#[no_mangle]
pub extern "C" fn tally_graph_neighbor_count(node: u32) -> u32 {
    graph()
        .edges
        .get(node as usize)
        .map(|neighbours| neighbours.len() as u32)
        .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn tally_graph_neighbor(node: u32, index: u32) -> u32 {
    graph()
        .edges
        .get(node as usize)
        .and_then(|neighbours| neighbours.get(index as usize))
        .copied()
        .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn tally_graph_add_hop() {
    record_current_thread_work(1);
}
