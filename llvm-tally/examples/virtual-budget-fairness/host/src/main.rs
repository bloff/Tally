/*
 * Native LLVM/Rust virtual-budget fairness experiment. It calibrates the Rust
 * runtime, creates many minithreads with random virtual CPU shares summing to a
 * requested total, runs timed rounds, and emits per-thread work data.
 */
use llvm_tally_runtime::{
    __tally_charge, TallyManager, VirtualAdaptiveState, VirtualCalibration,
    VirtualCalibrationConfig, VirtualRunResult, VirtualThread,
};
use std::env;
use std::error::Error;
use std::ffi::c_void;
use std::ptr;
use std::time::Instant;

#[repr(C)]
struct FairnessArgs {
    work_done: u64,
    state: u64,
    stop: u32,
}

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = env::args().collect();
    let thread_count = parse_arg(&args, 1, 50_000_usize);
    let rounds = parse_arg(&args, 2, 3_usize);
    let min_round_seconds = parse_arg(&args, 3, 10.0_f64);
    let max_round_seconds = parse_arg(&args, 4, 20.0_f64);
    let total_virtual_budget = parse_arg(&args, 5, 0.5_f64);
    let mut seed = parse_arg(&args, 6, 0x5eed_1234_u64);
    let calibration_seconds = parse_arg(&args, 7, 30.0_f64);
    let calibration_work = parse_arg(&args, 8, 200_000_u64);
    let stack_size = parse_arg(&args, 9, 16 * 1024_usize);
    let adaptive = parse_bool_arg(&args, 10, false);

    if thread_count == 0
        || rounds == 0
        || total_virtual_budget <= 0.0
        || min_round_seconds <= 0.0
        || max_round_seconds <= 0.0
    {
        return Err("invalid virtual-budget fairness arguments".into());
    }

    let mut calibration = VirtualCalibration::calibrate(VirtualCalibrationConfig {
        target_seconds: calibration_seconds,
        work_per_sample: calibration_work,
        stack_size,
    })?;

    let shares = random_virtual_shares(thread_count, total_virtual_budget, &mut seed);
    let mut manager = TallyManager::new();
    let mut thread_args: Vec<Box<FairnessArgs>> = (0..thread_count)
        .map(|i| {
            Box::new(FairnessArgs {
                work_done: 0,
                state: 0x9e37_79b9_7f4a_7c15_u64 ^ (i as u64).wrapping_mul(0xbf58_476d_1ce4_e5b9),
                stop: 0,
            })
        })
        .collect();

    let mut virtual_threads = Vec::with_capacity(thread_count);
    for (i, args) in thread_args.iter_mut().enumerate() {
        let id = manager.spawn_with_stack(
            fairness_worker,
            &mut **args as *mut FairnessArgs as *mut c_void,
            1,
            stack_size,
        )?;
        virtual_threads.push(VirtualThread::new(id, shares[i]));
    }

    println!("implementation: llvm-rust");
    println!("thread_count: {thread_count}");
    println!("rounds: {rounds}");
    println!("min_round_seconds: {min_round_seconds:.9}");
    println!("max_round_seconds: {max_round_seconds:.9}");
    println!("total_virtual_budget: {total_virtual_budget:.17}");
    println!("adaptive: {}", u8::from(adaptive));
    println!("calibration_seconds: {calibration_seconds:.9}");
    println!(
        "seconds_per_budget_unit: {:.17}",
        calibration.seconds_per_budget_unit
    );
    println!(
        "seconds_per_activation: {:.17}",
        calibration.seconds_per_activation
    );
    println!(
        "seconds_per_scheduler_round: {:.17}",
        calibration.seconds_per_scheduler_round
    );
    println!(
        "context_switch_budget_units: {:.17}",
        calibration.context_switch_budget_units()
    );
    println!();
    println!(
        "thread,round,virtual_budget,activation_corrected_budget_seconds,work,activations,budget_units_consumed,round_seconds"
    );

    let mut adaptive_state = VirtualAdaptiveState::default();
    let mut work_start = vec![0_u64; thread_count];
    let mut activation_start = vec![0_u64; thread_count];
    let mut budget_unit_start = vec![0_u64; thread_count];

    for round in 0..rounds {
        for i in 0..thread_count {
            virtual_threads[i].credit_seconds = 0.0;
            work_start[i] = thread_args[i].work_done;
            activation_start[i] = virtual_threads[i].activations;
            budget_unit_start[i] = virtual_threads[i].budget_units_consumed;
        }

        let requested_seconds =
            choose_round_seconds(&mut seed, min_round_seconds, max_round_seconds);
        let round_start = Instant::now();
        let mut last_tick = round_start;
        loop {
            let tick_start = Instant::now();
            if tick_start.duration_since(round_start).as_secs_f64() >= requested_seconds {
                break;
            }

            let mut delta = tick_start.duration_since(last_tick).as_secs_f64();
            if delta <= 0.0 {
                delta = 1.0e-9;
            }
            last_tick = tick_start;

            for thread in &mut virtual_threads {
                thread.add_credit(delta);
                thread.charge_scheduler_round(&calibration, thread_count);
            }

            let mut window_units = 0_u64;
            let mut window_activations = 0_u64;
            for thread in &mut virtual_threads {
                let before_units = thread.budget_units_consumed;
                let before_activations = thread.activations;
                let result = thread.run_ready(&mut manager, &calibration)?;
                if result == VirtualRunResult::Errored {
                    return Err(format!("thread {} errored", thread.thread_id).into());
                }
                window_units =
                    window_units.saturating_add(thread.budget_units_consumed - before_units);
                window_activations =
                    window_activations.saturating_add(thread.activations - before_activations);
            }

            if adaptive {
                adaptive_state.observe(
                    &mut calibration,
                    tick_start.elapsed().as_secs_f64(),
                    window_units,
                    window_activations,
                    1,
                );
            }
        }

        let actual_seconds = round_start.elapsed().as_secs_f64();
        for i in 0..thread_count {
            let work = thread_args[i].work_done - work_start[i];
            let activations = virtual_threads[i].activations - activation_start[i];
            let budget_units = virtual_threads[i].budget_units_consumed - budget_unit_start[i];
            let corrected_seconds = (shares[i] * actual_seconds)
                - (activations as f64 * calibration.seconds_per_activation);
            println!(
                "{},{},{:.17},{:.17},{},{},{},{:.9}",
                i,
                round,
                shares[i],
                corrected_seconds.max(0.0),
                work,
                activations,
                budget_units,
                actual_seconds
            );
        }
    }

    for args in thread_args.iter_mut() {
        args.stop = 1;
    }
    Ok(())
}

unsafe extern "C" fn fairness_worker(arg: *mut c_void) {
    let input = arg as *mut FairnessArgs;
    let mut work = ptr::read_volatile(&(*input).work_done);
    let mut state = ptr::read_volatile(&(*input).state);

    while ptr::read_volatile(&(*input).stop) == 0 {
        state = state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        state ^= state >> 23;
        work = work.wrapping_add(1);
        ptr::write_volatile(&mut (*input).state, state);
        ptr::write_volatile(&mut (*input).work_done, work);
        __tally_charge(1);
    }
}

fn random_virtual_shares(
    thread_count: usize,
    total_virtual_budget: f64,
    seed: &mut u64,
) -> Vec<f64> {
    let mut weights = Vec::with_capacity(thread_count);
    let mut total_weight = 0.0;
    for _ in 0..thread_count {
        let u = random_unit(seed);
        let weight = (-3.0 + (6.0 * u)).exp();
        total_weight += weight;
        weights.push(weight);
    }
    weights
        .into_iter()
        .map(|weight| (weight / total_weight) * total_virtual_budget)
        .collect()
}

fn choose_round_seconds(seed: &mut u64, min_seconds: f64, max_seconds: f64) -> f64 {
    if max_seconds <= min_seconds {
        min_seconds
    } else {
        min_seconds + (random_unit(seed) * (max_seconds - min_seconds))
    }
}

fn next_random(seed: &mut u64) -> u64 {
    *seed = seed
        .wrapping_mul(6_364_136_223_846_793_005)
        .wrapping_add(1_442_695_040_888_963_407);
    let mut value = *seed;
    value ^= value >> 33;
    value = value.wrapping_mul(0xff51_afd7_ed55_8ccd);
    value ^= value >> 33;
    value
}

fn random_unit(seed: &mut u64) -> f64 {
    ((next_random(seed) >> 11) as f64) * (1.0 / 9_007_199_254_740_992.0)
}

fn parse_arg<T>(args: &[String], index: usize, fallback: T) -> T
where
    T: std::str::FromStr,
{
    args.get(index)
        .and_then(|value| value.parse::<T>().ok())
        .unwrap_or(fallback)
}

fn parse_bool_arg(args: &[String], index: usize, fallback: bool) -> bool {
    args.get(index)
        .map(|value| !value.is_empty() && value != "0")
        .unwrap_or(fallback)
}
