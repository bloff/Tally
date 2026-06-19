/*
 * Host runner for the llvm-tally memory-recovery integration test. It verifies
 * that minithread stack/heap failures become scheduler-visible errors and do
 * not terminate the host process.
 */
use llvm_tally_runtime::{MemoryLimits, TallyManager, ThreadError, ThreadState};
use std::env;
use std::error::Error;
use std::ffi::c_void;
use std::path::PathBuf;

const CASE_HEALTHY: u32 = 0;
const CASE_HEAP_OVERFLOW: u32 = 1;
const CASE_RECURSIVE_STACK: u32 = 2;
const CASE_LARGE_FRAME: u32 = 3;

#[repr(C)]
struct MemoryRecoveryArgs {
    case_id: u32,
    progress: u64,
    marker: u8,
}

#[no_mangle]
pub extern "C" fn tally_memory_escape(ptr: *mut u8) {
    std::hint::black_box(ptr);
}

fn main() -> Result<(), Box<dyn Error>> {
    let workload = env::args().nth(1).map(PathBuf::from).unwrap_or_else(|| {
        PathBuf::from("llvm-tally/dl/examples/memory-recovery/memory_recovery.so")
    });

    run_case(
        &workload,
        CASE_HEAP_OVERFLOW,
        MemoryLimits::new(64 * 1024, 128),
        Some(ThreadError::HeapLimit),
    )?;
    run_case(
        &workload,
        CASE_RECURSIVE_STACK,
        MemoryLimits::stack_only(32 * 1024),
        Some(ThreadError::StackSoftLimit),
    )?;
    run_case(
        &workload,
        CASE_LARGE_FRAME,
        MemoryLimits::stack_only(32 * 1024),
        None,
    )?;

    println!("memory recovery integration: ok");
    Ok(())
}

fn run_case(
    workload: &PathBuf,
    case_id: u32,
    limits: MemoryLimits,
    expected_error: Option<ThreadError>,
) -> Result<(), Box<dyn Error>> {
    let mut manager = TallyManager::new();
    let entry = manager.load_function(workload, "run_memory_recovery_case")?;
    let mut failing_args = MemoryRecoveryArgs {
        case_id,
        progress: 0,
        marker: 0x5a,
    };
    let failing = manager.spawn_with_limits(
        entry,
        &mut failing_args as *mut MemoryRecoveryArgs as *mut c_void,
        1_000_000,
        limits,
    )?;

    let mut healthy_args = MemoryRecoveryArgs {
        case_id: CASE_HEALTHY,
        progress: 0,
        marker: 0xa5,
    };
    let healthy = manager.spawn_with_limits(
        entry,
        &mut healthy_args as *mut MemoryRecoveryArgs as *mut c_void,
        100,
        MemoryLimits::new(64 * 1024, 0),
    )?;

    let state = manager.run_cycle(failing)?;
    if state != ThreadState::Errored {
        return Err(format!("case {case_id} returned {state:?}, expected Errored").into());
    }

    let stats = manager.stats_for_thread(failing)?;
    match expected_error {
        Some(error) if stats.error != Some(error) => {
            return Err(
                format!("case {case_id} error {:?}, expected {error:?}", stats.error).into(),
            );
        }
        None if !matches!(
            stats.error,
            Some(ThreadError::StackGuardFault) | Some(ThreadError::StackSoftLimit)
        ) =>
        {
            return Err(format!(
                "case {case_id} error {:?}, expected stack error",
                stats.error
            )
            .into());
        }
        _ => {}
    }

    if manager.run_cycle(healthy)? != ThreadState::Returned {
        return Err(format!("healthy thread did not return after case {case_id}").into());
    }
    if healthy_args.progress != 1 {
        return Err(format!("healthy thread progress was {}", healthy_args.progress).into());
    }
    if manager.run_cycle(failing)? != ThreadState::Errored {
        return Err(format!("errored thread resumed after case {case_id}").into());
    }

    Ok(())
}
