/*
 * Host runner for a std-using workload linked against instrumented local std.
 */
use llvm_tally_runtime::{MemoryLimits, TallyManager, ThreadState};
use std::env;
use std::error::Error;
use std::ffi::c_void;
use std::path::PathBuf;

#[repr(C)]
struct StdVecArgs {
    target_len: u64,
    sum: u64,
    len: u64,
}

#[derive(Clone, Copy)]
enum HeapSetting {
    Limited(usize),
    Unlimited,
}

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = env::args().collect();
    let workload = PathBuf::from(args.get(1).cloned().unwrap_or_else(|| {
        "llvm-tally/dl/std/examples/std-vec-workload/std_vec_workload.so".to_string()
    }));
    let thread_count = parse_arg(&args, 2, 4_usize);
    let target_len = parse_arg(&args, 3, 512_u64);
    let budget = parse_arg(&args, 4, 10_000_i64);
    let abi_bridge = args.get(5).map(PathBuf::from);
    let rounds = parse_arg(&args, 6, 1_usize);
    let heap_setting = parse_heap_setting(args.get(7).map(String::as_str))?;

    let mut manager = TallyManager::new();
    let std_environment = abi_bridge
        .as_ref()
        .map(|bridge| manager.create_std_environment(bridge))
        .transpose()?;

    println!(
        "round,thread,target_len,len,sum,charges,state,error,heap_limit_bytes,heap_unlimited,heap_peak_live_bytes,allocations,allocation_failures"
    );
    for round in 0..rounds {
        let entry = match std_environment {
            Some(environment_id) => manager.load_function_in_std_environment(
                environment_id,
                &workload,
                "run_std_vec_workload",
            )?,
            None => manager.load_function(&workload, "run_std_vec_workload")?,
        };

        let mut workload_args: Vec<Box<StdVecArgs>> = (0..thread_count)
            .map(|i| {
                Box::new(StdVecArgs {
                    target_len: target_len + i as u64,
                    sum: 0,
                    len: 0,
                })
            })
            .collect();
        let mut thread_ids = Vec::with_capacity(thread_count);

        for args in &mut workload_args {
            let limits = match heap_setting {
                HeapSetting::Limited(heap_bytes) => MemoryLimits::new(512 * 1024, heap_bytes),
                HeapSetting::Unlimited => MemoryLimits::unlimited_heap(512 * 1024),
            };
            thread_ids.push(manager.spawn_with_limits(
                entry,
                &mut **args as *mut StdVecArgs as *mut c_void,
                budget,
                limits,
            )?);
        }

        if let Some(environment_id) = std_environment {
            if manager
                .unload_std_environment_workloads(environment_id)
                .is_ok()
            {
                return Err("std workload unloaded while minithreads were still active".into());
            }
        }

        let mut active = thread_count;
        while active > 0 {
            active = 0;
            for id in &thread_ids {
                let state = manager.run_cycle(*id)?;
                if !matches!(state, ThreadState::Returned | ThreadState::Errored) {
                    active += 1;
                }
            }
        }

        for (index, args) in workload_args.iter().enumerate() {
            let stats = manager.stats_for_thread(thread_ids[index])?;
            let memory = manager.memory_stats_for_thread(thread_ids[index])?;
            println!(
                "{},{},{},{},{},{},{:?},{:?},{},{},{},{},{}",
                round,
                index,
                args.target_len,
                args.len,
                args.sum,
                stats.charges,
                stats.state,
                stats.error,
                memory.heap_limit_bytes,
                u8::from(memory.heap_unlimited),
                memory.heap_peak_live_bytes,
                memory.allocations,
                memory.allocation_failures
            );
        }

        if let Some(environment_id) = std_environment {
            manager.unload_std_environment_workloads(environment_id)?;
        }
    }

    Ok(())
}

fn parse_heap_setting(value: Option<&str>) -> Result<HeapSetting, Box<dyn Error>> {
    let Some(value) = value else {
        return Ok(HeapSetting::Unlimited);
    };
    if value.eq_ignore_ascii_case("unlimited") {
        return Ok(HeapSetting::Unlimited);
    }

    let bytes = value.parse::<usize>()?;
    Ok(HeapSetting::Limited(bytes))
}

fn parse_arg<T>(args: &[String], index: usize, fallback: T) -> T
where
    T: std::str::FromStr,
{
    args.get(index)
        .and_then(|value| value.parse::<T>().ok())
        .unwrap_or(fallback)
}
