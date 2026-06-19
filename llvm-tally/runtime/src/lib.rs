/*
 * Rust minithread runtime for llvm-tally. It provides stackful context
 * switching, dynamic loading, work accounting, and the __tally_charge C ABI.
 */
use core::arch::{asm, global_asm};
use std::cell::Cell;
use std::error::Error;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::fmt;
use std::fs;
use std::mem;
use std::path::Path;
use std::ptr;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::Once;
use std::time::Instant;

global_asm!(
    r#"
    .text
    .global tally_swap_context
    .type tally_swap_context,@function
tally_swap_context:
    movq %rsp, 0(%rdi)
    movq %r15, 8(%rdi)
    movq %r14, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r12, 32(%rdi)
    movq %rbx, 40(%rdi)
    movq %rbp, 48(%rdi)

    movq 8(%rsi), %r15
    movq 16(%rsi), %r14
    movq 24(%rsi), %r13
    movq 32(%rsi), %r12
    movq 40(%rsi), %rbx
    movq 48(%rsi), %rbp
    movq 0(%rsi), %rsp
    ret
    .size tally_swap_context, .-tally_swap_context
"#,
    options(att_syntax)
);

unsafe extern "C" {
    fn tally_swap_context(current: *mut Context, next: *const Context);
}

#[link(name = "dl")]
unsafe extern "C" {
    fn dlopen(filename: *const c_char, flags: c_int) -> *mut c_void;
    fn dlmopen(nsid: LinkMapNamespace, filename: *const c_char, flags: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn dlinfo(handle: *mut c_void, request: c_int, arg: *mut c_void) -> c_int;
    fn dlclose(handle: *mut c_void) -> c_int;
    fn dlerror() -> *const c_char;
}

unsafe extern "C" {
    fn mmap(
        addr: *mut c_void,
        length: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: isize,
    ) -> *mut c_void;
    fn mprotect(addr: *mut c_void, length: usize, prot: c_int) -> c_int;
    fn munmap(addr: *mut c_void, length: usize) -> c_int;
    fn sigaction(signum: c_int, act: *const SigAction, oldact: *mut SigAction) -> c_int;
    fn sigaltstack(ss: *const StackT, old_ss: *mut StackT) -> c_int;
    fn raise(signum: c_int) -> c_int;
    fn signal(signum: c_int, handler: usize) -> usize;
    fn _exit(status: c_int) -> !;
}

const RTLD_NOW: c_int = 2;
const RTLD_LOCAL: c_int = 0;
const RTLD_GLOBAL: c_int = 0x100;
const RTLD_DI_LMID: c_int = 1;
const LM_ID_NEWLM: LinkMapNamespace = -1;
const PROT_NONE: c_int = 0;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_PRIVATE: c_int = 0x02;
const MAP_ANONYMOUS: c_int = 0x20;
const MAP_STACK: c_int = 0x20000;
const MAP_FAILED: *mut c_void = !0_usize as *mut c_void;
const SIGSEGV: c_int = 11;
const SIG_DFL: usize = 0;
const SA_SIGINFO: c_int = 0x00000004;
const SA_ONSTACK: c_int = 0x08000000;
const SA_NODEFER: c_int = 0x40000000;
const PAGE_SIZE: usize = 4096;
const SIGNAL_STACK_SIZE: usize = 64 * 1024;
const MIN_STACK_SIZE: usize = 4096;
const DEFAULT_STACK_GUARD_SIZE: usize = 64 * 1024;
const DEFAULT_STACK_SIZE: usize = 1024 * 1024;
const CALIBRATION_FILE_HEADER: &str = "tally_virtual_calibration_v1";
const CALIBRATION_BUDGETS: [i64; 4] = [50, 100, 500, 1000];
const CALIBRATION_THREAD_COUNTS: [usize; 4] = [1, 2, 5, 10];

#[repr(C)]
struct SigAction {
    sa_sigaction: usize,
    sa_mask: [u64; 16],
    sa_flags: c_int,
    sa_restorer: usize,
}

#[repr(C)]
struct SigInfo {
    si_signo: c_int,
    si_errno: c_int,
    si_code: c_int,
    _pad: c_int,
    si_addr: *mut c_void,
    _rest: [u8; 104],
}

#[repr(C)]
struct StackT {
    ss_sp: *mut c_void,
    ss_flags: c_int,
    ss_size: usize,
}

static SIGNAL_HANDLER_ONCE: Once = Once::new();
static SIGNAL_HANDLER_STATUS: AtomicI32 = AtomicI32::new(1);

thread_local! {
    static ALT_SIGNAL_STACK: Cell<*mut c_void> = const { Cell::new(ptr::null_mut()) };
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Context {
    rsp: usize,
    r15: usize,
    r14: usize,
    r13: usize,
    r12: usize,
    rbx: usize,
    rbp: usize,
}

#[derive(Debug, Clone)]
pub struct TallyError {
    message: String,
}

impl TallyError {
    fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl fmt::Display for TallyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl Error for TallyError {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ThreadState {
    Ready,
    Running,
    Yielded,
    Returned,
    Errored,
}

pub type TallyEntry = unsafe extern "C" fn(*mut c_void);
pub type TallyStdEnvironmentId = usize;

type LinkMapNamespace = isize;

#[repr(C)]
struct TallyAbiHooks {
    charge: Option<extern "C" fn(u64)>,
    alloc: Option<extern "C" fn(u64, u64) -> *mut c_void>,
    dealloc: Option<extern "C" fn(*mut c_void, u64, u64)>,
    realloc: Option<extern "C" fn(*mut c_void, u64, u64, u64) -> *mut c_void>,
}

type TallyAbiSetHooks = unsafe extern "C" fn(*const TallyAbiHooks);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ThreadError {
    StackSoftLimit,
    StackGuardFault,
    HeapLimit,
    InvalidAllocationRequest,
    SchedulerUnavailable,
}

#[derive(Debug, Clone, Copy)]
pub struct MemoryLimits {
    pub stack_bytes: usize,
    pub heap_bytes: Option<usize>,
}

impl MemoryLimits {
    pub fn new(stack_bytes: usize, heap_bytes: usize) -> Self {
        Self {
            stack_bytes,
            heap_bytes: Some(heap_bytes),
        }
    }

    pub fn stack_only(stack_bytes: usize) -> Self {
        Self {
            stack_bytes,
            heap_bytes: Some(0),
        }
    }

    pub fn unlimited_heap(stack_bytes: usize) -> Self {
        Self {
            stack_bytes,
            heap_bytes: None,
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct MemoryStats {
    pub stack_limit_bytes: usize,
    pub stack_used_bytes: usize,
    pub stack_peak_bytes: usize,
    pub stack_overflowed: bool,
    pub heap_limit_bytes: usize,
    pub heap_unlimited: bool,
    pub heap_live_bytes: usize,
    pub heap_peak_live_bytes: usize,
    pub heap_committed_bytes: usize,
    pub allocations: u64,
    pub deallocations: u64,
    pub allocation_failures: u64,
    pub error: Option<ThreadError>,
}

pub struct TallyThread {
    id: usize,
    context: Context,
    stack: TallyStack,
    heap: TallyHeap,
    entry: TallyEntry,
    arg: *mut c_void,
    budget_per_cycle: i64,
    remaining_budget: i64,
    state: ThreadState,
    error: Option<ThreadError>,
    cycles_run: u64,
    charges: u64,
    work_units: u64,
}

#[derive(Debug, Clone, Copy)]
pub struct ThreadStats {
    pub id: usize,
    pub budget_per_cycle: i64,
    pub remaining_budget: i64,
    pub state: ThreadState,
    pub cycles_run: u64,
    pub charges: u64,
    pub work_units: u64,
    pub error: Option<ThreadError>,
}

#[derive(Debug, Clone, Copy)]
pub struct VirtualCalibration {
    pub seconds_per_budget_unit: f64,
    pub seconds_per_activation: f64,
    pub seconds_per_scheduler_round: f64,
    pub min_internal_budget: i64,
    pub max_internal_budget: i64,
}

#[derive(Debug, Clone, Copy)]
pub struct VirtualCalibrationConfig {
    pub target_seconds: f64,
    pub work_per_sample: u64,
    pub stack_size: usize,
}

#[derive(Debug, Clone)]
pub struct VirtualAdaptiveState {
    pub smoothing: f64,
    pub min_observation_seconds: f64,
    pub accumulated_observed_seconds: f64,
    pub accumulated_budget_units: u64,
    pub accumulated_activations: u64,
    pub accumulated_scheduler_rounds: u64,
    pub observations: u64,
    pub updates: u64,
    pub last_sample_seconds_per_budget_unit: f64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VirtualRunResult {
    NotReady,
    Ran(ThreadState),
    Finished,
    Errored,
}

#[derive(Debug, Clone)]
pub struct VirtualThread {
    pub thread_id: usize,
    pub cpu_share: f64,
    pub credit_seconds: f64,
    pub last_budget: i64,
    pub last_budget_units_consumed: i64,
    pub activations: u64,
    pub skipped_cycles: u64,
    pub completed_cycles: u64,
    pub budget_units_consumed: u64,
}

struct TallyStack {
    mapping_base: NonNull<u8>,
    mapping_len: usize,
    usable_base: NonNull<u8>,
    usable_len: usize,
    guard_low_len: usize,
    guard_high_len: usize,
    peak_bytes: usize,
    overflowed: bool,
}

struct TallyHeap {
    arena: Option<MmapRegion>,
    unlimited: bool,
    offset: usize,
    live_bytes: usize,
    peak_live_bytes: usize,
    allocations: u64,
    deallocations: u64,
    allocation_failures: u64,
}

struct MmapRegion {
    base: NonNull<u8>,
    len: usize,
}

pub struct TallyManager {
    scheduler_context: Context,
    threads: Vec<Box<TallyThread>>,
    libraries: Vec<DynamicLibrary>,
    std_environments: Vec<TallyStdEnvironment>,
}

pub struct DynamicLibrary {
    handle: *mut c_void,
}

struct TallyStdEnvironment {
    namespace_id: LinkMapNamespace,
    _bridge: DynamicLibrary,
    workloads: Vec<DynamicLibrary>,
}

static mut CURRENT_THREAD: *mut TallyThread = ptr::null_mut();
static mut SCHEDULER_CONTEXT: *mut Context = ptr::null_mut();

impl TallyManager {
    pub fn new() -> Self {
        Self {
            scheduler_context: Context::default(),
            threads: Vec::new(),
            libraries: Vec::new(),
            std_environments: Vec::new(),
        }
    }

    pub fn load_function(
        &mut self,
        path: impl AsRef<Path>,
        symbol: &str,
    ) -> Result<TallyEntry, TallyError> {
        let library = DynamicLibrary::open(path.as_ref())?;
        let function = library.symbol(symbol)?;
        self.libraries.push(library);
        Ok(function)
    }

    pub fn create_std_environment(
        &mut self,
        bridge_path: impl AsRef<Path>,
    ) -> Result<TallyStdEnvironmentId, TallyError> {
        let environment = TallyStdEnvironment::open(bridge_path.as_ref())?;
        let id = self.std_environments.len();
        self.std_environments.push(environment);
        Ok(id)
    }

    pub fn load_function_in_std_environment(
        &mut self,
        environment_id: TallyStdEnvironmentId,
        path: impl AsRef<Path>,
        symbol: &str,
    ) -> Result<TallyEntry, TallyError> {
        let environment = self
            .std_environments
            .get_mut(environment_id)
            .ok_or_else(|| {
                TallyError::new(format!("unknown std environment id {environment_id}"))
            })?;
        environment.load_function(path.as_ref(), symbol)
    }

    pub fn unload_std_environment_workloads(
        &mut self,
        environment_id: TallyStdEnvironmentId,
    ) -> Result<usize, TallyError> {
        if self
            .threads
            .iter()
            .any(|thread| !matches!(thread.state, ThreadState::Returned | ThreadState::Errored))
        {
            return Err(TallyError::new(
                "cannot unload std workloads while minithreads are still active",
            ));
        }

        let environment = self
            .std_environments
            .get_mut(environment_id)
            .ok_or_else(|| {
                TallyError::new(format!("unknown std environment id {environment_id}"))
            })?;
        Ok(environment.unload_workloads())
    }

    pub fn std_environment_namespace_id(
        &self,
        environment_id: TallyStdEnvironmentId,
    ) -> Result<isize, TallyError> {
        self.std_environments
            .get(environment_id)
            .map(|environment| environment.namespace_id)
            .ok_or_else(|| TallyError::new(format!("unknown std environment id {environment_id}")))
    }

    pub fn spawn(
        &mut self,
        entry: TallyEntry,
        arg: *mut c_void,
        budget_per_cycle: i64,
    ) -> Result<usize, TallyError> {
        self.spawn_with_stack(entry, arg, budget_per_cycle, DEFAULT_STACK_SIZE)
    }

    pub fn spawn_with_stack(
        &mut self,
        entry: TallyEntry,
        arg: *mut c_void,
        budget_per_cycle: i64,
        stack_size: usize,
    ) -> Result<usize, TallyError> {
        self.spawn_with_limits(
            entry,
            arg,
            budget_per_cycle,
            MemoryLimits::stack_only(stack_size),
        )
    }

    pub fn spawn_with_limits(
        &mut self,
        entry: TallyEntry,
        arg: *mut c_void,
        budget_per_cycle: i64,
        limits: MemoryLimits,
    ) -> Result<usize, TallyError> {
        if budget_per_cycle <= 0 {
            return Err(TallyError::new("budget_per_cycle must be positive"));
        }
        ensure_memory_signal_support()?;

        let id = self.threads.len();
        let thread = Box::new(TallyThread::new(id, entry, arg, budget_per_cycle, limits)?);
        self.threads.push(thread);
        Ok(id)
    }

    pub fn run_cycle(&mut self, id: usize) -> Result<ThreadState, TallyError> {
        if id >= self.threads.len() {
            return Err(TallyError::new(format!("unknown thread id {id}")));
        }

        let manager_context = &mut self.scheduler_context as *mut Context;
        let thread = &mut *self.threads[id] as *mut TallyThread;

        unsafe {
            if matches!(
                (*thread).state,
                ThreadState::Returned | ThreadState::Errored
            ) {
                return Ok((*thread).state);
            }

            (*thread).remaining_budget += (*thread).budget_per_cycle;
            (*thread).cycles_run += 1;
            (*thread).state = ThreadState::Running;

            CURRENT_THREAD = thread;
            SCHEDULER_CONTEXT = manager_context;
            tally_swap_context(manager_context, &(*thread).context as *const Context);
            CURRENT_THREAD = ptr::null_mut();
            SCHEDULER_CONTEXT = ptr::null_mut();

            if (*thread).state == ThreadState::Running {
                (*thread).state = ThreadState::Yielded;
            }

            Ok((*thread).state)
        }
    }

    pub fn run_cycles(&mut self, metacycles: u64) -> Result<(), TallyError> {
        for _ in 0..metacycles {
            for id in 0..self.threads.len() {
                self.run_cycle(id)?;
            }
        }
        Ok(())
    }

    pub fn thread_count(&self) -> usize {
        self.threads.len()
    }

    pub fn set_budget_per_cycle(
        &mut self,
        id: usize,
        budget_per_cycle: i64,
    ) -> Result<(), TallyError> {
        if budget_per_cycle <= 0 {
            return Err(TallyError::new("budget_per_cycle must be positive"));
        }

        let thread = self
            .threads
            .get_mut(id)
            .ok_or_else(|| TallyError::new(format!("unknown thread id {id}")))?;
        thread.budget_per_cycle = budget_per_cycle;
        Ok(())
    }

    pub fn thread_state(&self, id: usize) -> Result<ThreadState, TallyError> {
        Ok(self.stats_for_thread(id)?.state)
    }

    pub fn stats_for_thread(&self, id: usize) -> Result<ThreadStats, TallyError> {
        self.threads
            .get(id)
            .map(|thread| thread.stats())
            .ok_or_else(|| TallyError::new(format!("unknown thread id {id}")))
    }

    pub fn stats(&self) -> Vec<ThreadStats> {
        self.threads.iter().map(|thread| thread.stats()).collect()
    }

    pub fn memory_stats_for_thread(&self, id: usize) -> Result<MemoryStats, TallyError> {
        self.threads
            .get(id)
            .map(|thread| thread.memory_stats())
            .ok_or_else(|| TallyError::new(format!("unknown thread id {id}")))
    }
}

impl Default for TallyManager {
    fn default() -> Self {
        Self::new()
    }
}

impl Default for VirtualCalibration {
    fn default() -> Self {
        Self {
            seconds_per_budget_unit: 1.0e-9,
            seconds_per_activation: 0.0,
            seconds_per_scheduler_round: 0.0,
            min_internal_budget: 1,
            max_internal_budget: i64::MAX / 4,
        }
    }
}

impl VirtualCalibration {
    pub fn calibrate(config: VirtualCalibrationConfig) -> Result<Self, TallyError> {
        let config = config.normalized();
        let mut samples = Vec::new();
        let started = Instant::now();

        loop {
            for thread_count in CALIBRATION_THREAD_COUNTS {
                for budget in CALIBRATION_BUDGETS {
                    samples.push(run_virtual_calibration_sample(
                        thread_count,
                        budget,
                        config.work_per_sample,
                        config.stack_size,
                    )?);
                }
            }

            if config.target_seconds <= 0.0
                || started.elapsed().as_secs_f64() >= config.target_seconds
                || samples.len() >= 512
            {
                break;
            }
        }

        fit_virtual_calibration(&samples)
    }

    pub fn write_to_file(&self, path: impl AsRef<Path>) -> Result<(), TallyError> {
        let text = format!(
            "{CALIBRATION_FILE_HEADER}\n\
             seconds_per_budget_unit={:.17}\n\
             seconds_per_activation={:.17}\n\
             seconds_per_scheduler_round={:.17}\n\
             min_internal_budget={}\n\
             max_internal_budget={}\n",
            self.seconds_per_budget_unit,
            self.seconds_per_activation,
            self.seconds_per_scheduler_round,
            self.min_internal_budget,
            self.max_internal_budget
        );
        fs::write(path.as_ref(), text).map_err(|err| {
            TallyError::new(format!(
                "failed to write calibration file {}: {err}",
                path.as_ref().display()
            ))
        })
    }

    pub fn read_from_file(path: impl AsRef<Path>) -> Result<Self, TallyError> {
        let text = fs::read_to_string(path.as_ref()).map_err(|err| {
            TallyError::new(format!(
                "failed to read calibration file {}: {err}",
                path.as_ref().display()
            ))
        })?;
        parse_virtual_calibration(&text)
    }

    pub fn budget_from_seconds(&self, credit_seconds: f64) -> i64 {
        if credit_seconds <= 0.0 {
            return 0;
        }

        let activation_seconds = positive_or_zero(self.seconds_per_activation);
        let available_seconds = credit_seconds - activation_seconds;
        if available_seconds <= 0.0 {
            return 0;
        }

        let min_budget = self.min_internal_budget.max(1);
        let max_budget = self.max_internal_budget.max(min_budget);
        let raw_budget =
            (available_seconds / positive_or(self.seconds_per_budget_unit, 1.0e-9)) + 1.0e-9;

        if raw_budget < min_budget as f64 {
            0
        } else if raw_budget > max_budget as f64 {
            max_budget
        } else {
            raw_budget as i64
        }
    }

    pub fn context_switch_budget_units(&self) -> f64 {
        positive_or_zero(self.seconds_per_activation)
            / positive_or(self.seconds_per_budget_unit, 1.0e-9)
    }

    pub fn scheduler_round_budget_units(&self) -> f64 {
        positive_or_zero(self.seconds_per_scheduler_round)
            / positive_or(self.seconds_per_budget_unit, 1.0e-9)
    }
}

impl Default for VirtualCalibrationConfig {
    fn default() -> Self {
        Self {
            target_seconds: 30.0,
            work_per_sample: 200_000,
            stack_size: 64 * 1024,
        }
    }
}

impl VirtualCalibrationConfig {
    fn normalized(self) -> Self {
        Self {
            target_seconds: self.target_seconds,
            work_per_sample: if self.work_per_sample == 0 {
                200_000
            } else {
                self.work_per_sample
            },
            stack_size: self.stack_size.max(4096),
        }
    }
}

impl Default for VirtualAdaptiveState {
    fn default() -> Self {
        Self {
            smoothing: 0.10,
            min_observation_seconds: 0.005,
            accumulated_observed_seconds: 0.0,
            accumulated_budget_units: 0,
            accumulated_activations: 0,
            accumulated_scheduler_rounds: 0,
            observations: 0,
            updates: 0,
            last_sample_seconds_per_budget_unit: 0.0,
        }
    }
}

impl VirtualAdaptiveState {
    pub fn observe(
        &mut self,
        calibration: &mut VirtualCalibration,
        observed_seconds: f64,
        budget_units_consumed: u64,
        activations: u64,
        scheduler_rounds: u64,
    ) {
        if observed_seconds <= 0.0 {
            return;
        }

        self.accumulated_observed_seconds += observed_seconds;
        self.accumulated_budget_units = self
            .accumulated_budget_units
            .saturating_add(budget_units_consumed);
        self.accumulated_activations = self.accumulated_activations.saturating_add(activations);
        self.accumulated_scheduler_rounds = self
            .accumulated_scheduler_rounds
            .saturating_add(scheduler_rounds);
        self.observations = self.observations.saturating_add(1);

        if self.accumulated_observed_seconds < positive_or(self.min_observation_seconds, 0.005)
            || self.accumulated_budget_units == 0
        {
            return;
        }

        let fixed_seconds = self.accumulated_activations as f64
            * positive_or_zero(calibration.seconds_per_activation)
            + self.accumulated_scheduler_rounds as f64
                * positive_or_zero(calibration.seconds_per_scheduler_round);
        let work_seconds = self.accumulated_observed_seconds - fixed_seconds;
        if work_seconds > 0.0 {
            let old_unit = positive_or(calibration.seconds_per_budget_unit, 1.0e-9);
            let mut sample_unit = work_seconds / self.accumulated_budget_units as f64;
            sample_unit = sample_unit.clamp(old_unit / 4.0, old_unit * 4.0);
            let smoothing = if self.smoothing > 0.0 && self.smoothing <= 1.0 {
                self.smoothing
            } else {
                0.10
            };
            calibration.seconds_per_budget_unit =
                (old_unit * (1.0 - smoothing)) + (sample_unit * smoothing);
            self.last_sample_seconds_per_budget_unit = sample_unit;
            self.updates = self.updates.saturating_add(1);
        }

        self.accumulated_observed_seconds = 0.0;
        self.accumulated_budget_units = 0;
        self.accumulated_activations = 0;
        self.accumulated_scheduler_rounds = 0;
    }
}

impl VirtualThread {
    pub fn new(thread_id: usize, cpu_share: f64) -> Self {
        Self {
            thread_id,
            cpu_share: positive_or_zero(cpu_share),
            credit_seconds: 0.0,
            last_budget: 0,
            last_budget_units_consumed: 0,
            activations: 0,
            skipped_cycles: 0,
            completed_cycles: 0,
            budget_units_consumed: 0,
        }
    }

    pub fn add_credit(&mut self, elapsed_seconds: f64) {
        if elapsed_seconds > 0.0 && self.cpu_share > 0.0 {
            self.credit_seconds += elapsed_seconds * self.cpu_share;
        }
    }

    pub fn charge_scheduler_round(
        &mut self,
        calibration: &VirtualCalibration,
        participating_threads: usize,
    ) {
        if participating_threads == 0 {
            return;
        }
        let round_seconds = positive_or_zero(calibration.seconds_per_scheduler_round);
        if round_seconds > 0.0 {
            self.credit_seconds -= round_seconds / participating_threads as f64;
        }
    }

    pub fn run_ready(
        &mut self,
        manager: &mut TallyManager,
        calibration: &VirtualCalibration,
    ) -> Result<VirtualRunResult, TallyError> {
        let budget = calibration.budget_from_seconds(self.credit_seconds);
        self.last_budget = budget;
        if budget <= 0 {
            self.skipped_cycles = self.skipped_cycles.saturating_add(1);
            return Ok(VirtualRunResult::NotReady);
        }

        let before = manager.stats_for_thread(self.thread_id)?;
        manager.set_budget_per_cycle(self.thread_id, budget)?;
        let state = manager.run_cycle(self.thread_id)?;
        let after = manager.stats_for_thread(self.thread_id)?;

        let charged_budget = (budget + before.remaining_budget - after.remaining_budget).max(0);
        self.credit_seconds -= positive_or_zero(calibration.seconds_per_activation);
        self.credit_seconds -=
            charged_budget as f64 * positive_or(calibration.seconds_per_budget_unit, 1.0e-9);
        self.last_budget_units_consumed = charged_budget;
        self.budget_units_consumed = self
            .budget_units_consumed
            .saturating_add(charged_budget as u64);
        self.activations = self.activations.saturating_add(1);

        match state {
            ThreadState::Returned => Ok(VirtualRunResult::Finished),
            ThreadState::Errored => Ok(VirtualRunResult::Errored),
            other => {
                self.completed_cycles = self.completed_cycles.saturating_add(1);
                Ok(VirtualRunResult::Ran(other))
            }
        }
    }
}

fn positive_or(value: f64, fallback: f64) -> f64 {
    if value > 0.0 {
        value
    } else {
        fallback
    }
}

fn positive_or_zero(value: f64) -> f64 {
    positive_or(value, 0.0)
}

#[repr(C)]
struct VirtualCalibrationProbeArgs {
    work_done: u64,
    target_work: u64,
    state: u64,
}

#[derive(Debug, Clone, Copy)]
struct VirtualCalibrationSample {
    run_seconds: f64,
    budget_units_consumed: u64,
    thread_cycles: u64,
    scheduler_cycles: u64,
}

unsafe extern "C" fn virtual_calibration_probe(arg: *mut c_void) {
    let input = arg as *mut VirtualCalibrationProbeArgs;
    let mut work = ptr::read_volatile(&(*input).work_done);
    let target = ptr::read_volatile(&(*input).target_work);
    let mut state = ptr::read_volatile(&(*input).state);

    while work < target {
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

fn run_virtual_calibration_sample(
    thread_count: usize,
    budget: i64,
    target_work: u64,
    stack_size: usize,
) -> Result<VirtualCalibrationSample, TallyError> {
    let mut manager = TallyManager::new();
    let mut args: Vec<Box<VirtualCalibrationProbeArgs>> = (0..thread_count)
        .map(|i| {
            Box::new(VirtualCalibrationProbeArgs {
                work_done: 0,
                target_work: target_for_thread(target_work, thread_count, i),
                state: 0x9e37_79b9_7f4a_7c15_u64 ^ (i as u64).wrapping_mul(0xbf58_476d_1ce4_e5b9),
            })
        })
        .collect();

    let mut active = Vec::with_capacity(thread_count);
    let mut active_count = 0_usize;
    for args in args.iter_mut() {
        let is_active = args.target_work > 0;
        active.push(is_active);
        if is_active {
            active_count += 1;
        }
        manager.spawn_with_stack(
            virtual_calibration_probe,
            &mut **args as *mut VirtualCalibrationProbeArgs as *mut c_void,
            budget,
            stack_size,
        )?;
    }

    let mut scheduler_cycles = 0_u64;
    let mut thread_cycles = 0_u64;
    let mut budget_units_consumed = 0_u64;
    let started = Instant::now();
    while active_count > 0 {
        scheduler_cycles = scheduler_cycles.saturating_add(1);
        for id in 0..thread_count {
            if !active[id] {
                continue;
            }

            let before = manager.stats_for_thread(id)?;
            let state = manager.run_cycle(id)?;
            let after = manager.stats_for_thread(id)?;
            let charged_budget = (budget + before.remaining_budget - after.remaining_budget).max(0);
            budget_units_consumed = budget_units_consumed.saturating_add(charged_budget as u64);
            thread_cycles = thread_cycles.saturating_add(1);

            if state == ThreadState::Returned || args[id].work_done >= args[id].target_work {
                active[id] = false;
                active_count -= 1;
            } else if state == ThreadState::Errored {
                return Err(TallyError::new(format!(
                    "calibration probe thread {id} errored"
                )));
            }
        }
    }

    Ok(VirtualCalibrationSample {
        run_seconds: started.elapsed().as_secs_f64(),
        budget_units_consumed,
        thread_cycles,
        scheduler_cycles,
    })
}

fn target_for_thread(total_work: u64, thread_count: usize, index: usize) -> u64 {
    let base = total_work / thread_count as u64;
    let remainder = total_work % thread_count as u64;
    base + u64::from((index as u64) < remainder)
}

fn fit_virtual_calibration(
    samples: &[VirtualCalibrationSample],
) -> Result<VirtualCalibration, TallyError> {
    if samples.len() < 4 {
        return Err(TallyError::new(
            "at least four calibration samples are required",
        ));
    }

    let feature_rows: Vec<[f64; 4]> = samples
        .iter()
        .map(|sample| {
            [
                sample.budget_units_consumed as f64,
                sample.thread_cycles as f64,
                sample.scheduler_cycles as f64,
                1.0,
            ]
        })
        .collect();
    let y_values: Vec<f64> = samples.iter().map(|sample| sample.run_seconds).collect();
    let coefficients = least_squares_4(&feature_rows, &y_values)?;

    let total_budget_units: u64 = samples
        .iter()
        .map(|sample| sample.budget_units_consumed)
        .sum();
    let total_seconds: f64 = samples.iter().map(|sample| sample.run_seconds).sum();
    let seconds_per_budget_unit = if coefficients[0] > 0.0 {
        coefficients[0]
    } else if total_budget_units > 0 {
        total_seconds / total_budget_units as f64
    } else {
        1.0e-9
    };

    Ok(VirtualCalibration {
        seconds_per_budget_unit: positive_or(seconds_per_budget_unit, 1.0e-9),
        seconds_per_activation: positive_or_zero(coefficients[1]),
        seconds_per_scheduler_round: positive_or_zero(coefficients[2]),
        min_internal_budget: 1,
        max_internal_budget: i64::MAX / 4,
    })
}

fn least_squares_4(feature_rows: &[[f64; 4]], y_values: &[f64]) -> Result<[f64; 4], TallyError> {
    let mut scales = [0.0_f64; 4];
    for features in feature_rows {
        for i in 0..4 {
            scales[i] += features[i] * features[i];
        }
    }
    for scale in &mut scales {
        *scale = if *scale > 0.0 { scale.sqrt() } else { 1.0 };
    }

    let mut matrix = [[0.0_f64; 5]; 4];
    for (features, y_value) in feature_rows.iter().zip(y_values) {
        let scaled = [
            features[0] / scales[0],
            features[1] / scales[1],
            features[2] / scales[2],
            features[3] / scales[3],
        ];
        for row in 0..4 {
            matrix[row][4] += scaled[row] * y_value;
            for col in 0..4 {
                matrix[row][col] += scaled[row] * scaled[col];
            }
        }
    }
    for (i, row) in matrix.iter_mut().enumerate() {
        row[i] += 1.0e-10;
    }

    let scaled_solution = solve_linear_system_4(matrix)?;
    Ok([
        scaled_solution[0] / scales[0],
        scaled_solution[1] / scales[1],
        scaled_solution[2] / scales[2],
        scaled_solution[3] / scales[3],
    ])
}

fn solve_linear_system_4(mut matrix: [[f64; 5]; 4]) -> Result<[f64; 4], TallyError> {
    for column in 0..4 {
        let mut pivot = column;
        let mut pivot_abs = matrix[column][column].abs();
        for (row, values) in matrix.iter().enumerate().skip(column + 1) {
            let value = values[column].abs();
            if value > pivot_abs {
                pivot = row;
                pivot_abs = value;
            }
        }
        if pivot_abs < 1.0e-18 {
            return Err(TallyError::new("calibration fit is singular"));
        }
        if pivot != column {
            matrix.swap(column, pivot);
        }

        let pivot_value = matrix[column][column];
        for j in column..5 {
            matrix[column][j] /= pivot_value;
        }

        for row in 0..4 {
            if row == column {
                continue;
            }
            let factor = matrix[row][column];
            if factor == 0.0 {
                continue;
            }
            for j in column..5 {
                matrix[row][j] -= factor * matrix[column][j];
            }
        }
    }

    Ok([matrix[0][4], matrix[1][4], matrix[2][4], matrix[3][4]])
}

fn parse_virtual_calibration(text: &str) -> Result<VirtualCalibration, TallyError> {
    let mut lines = text
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty() && !line.starts_with('#'));

    if lines.next() != Some(CALIBRATION_FILE_HEADER) {
        return Err(TallyError::new("invalid calibration file header"));
    }

    let mut calibration = VirtualCalibration::default();
    let mut fields = 0_u8;
    for line in lines {
        let (key, value) = line
            .split_once('=')
            .ok_or_else(|| TallyError::new(format!("invalid calibration line: {line}")))?;
        match key {
            "seconds_per_budget_unit" => {
                calibration.seconds_per_budget_unit = parse_f64(value, key)?;
                fields += 1;
            }
            "seconds_per_activation" => {
                calibration.seconds_per_activation = parse_f64(value, key)?;
                fields += 1;
            }
            "seconds_per_scheduler_round" => {
                calibration.seconds_per_scheduler_round = parse_f64(value, key)?;
                fields += 1;
            }
            "min_internal_budget" => {
                calibration.min_internal_budget = parse_i64(value, key)?;
                fields += 1;
            }
            "max_internal_budget" => {
                calibration.max_internal_budget = parse_i64(value, key)?;
                fields += 1;
            }
            _ => {}
        }
    }

    if fields < 5
        || calibration.seconds_per_budget_unit <= 0.0
        || calibration.min_internal_budget <= 0
        || calibration.max_internal_budget < calibration.min_internal_budget
    {
        return Err(TallyError::new("invalid calibration values"));
    }

    Ok(calibration)
}

fn parse_f64(value: &str, key: &str) -> Result<f64, TallyError> {
    value
        .parse::<f64>()
        .map_err(|err| TallyError::new(format!("invalid {key}: {err}")))
}

fn parse_i64(value: &str, key: &str) -> Result<i64, TallyError> {
    value
        .parse::<i64>()
        .map_err(|err| TallyError::new(format!("invalid {key}: {err}")))
}

impl TallyStack {
    fn new(stack_size: usize) -> Result<Self, TallyError> {
        if stack_size < MIN_STACK_SIZE {
            return Err(TallyError::new(format!(
                "stack_size must be at least {MIN_STACK_SIZE} bytes"
            )));
        }

        let usable_len = page_align(stack_size)?;
        let guard_low_len = page_align(DEFAULT_STACK_GUARD_SIZE.max(usable_len.min(64 * 1024)))?;
        let guard_high_len = PAGE_SIZE;
        let mapping_len = guard_low_len
            .checked_add(usable_len)
            .and_then(|len| len.checked_add(guard_high_len))
            .ok_or_else(|| TallyError::new("stack mapping size overflow"))?;
        let region = MmapRegion::map(mapping_len, PROT_READ | PROT_WRITE, MAP_STACK)?;

        unsafe {
            protect_region(region.base.as_ptr(), guard_low_len, PROT_NONE)?;
            protect_region(
                region.base.as_ptr().add(guard_low_len + usable_len),
                guard_high_len,
                PROT_NONE,
            )?;
        }

        let usable_base =
            unsafe { NonNull::new_unchecked(region.base.as_ptr().add(guard_low_len)) };
        let mapping_base = region.base;
        mem::forget(region);

        Ok(Self {
            mapping_base,
            mapping_len,
            usable_base,
            usable_len,
            guard_low_len,
            guard_high_len,
            peak_bytes: 0,
            overflowed: false,
        })
    }

    fn usable_high(&self) -> usize {
        self.usable_base.as_ptr() as usize + self.usable_len
    }

    fn usable_low(&self) -> usize {
        self.usable_base.as_ptr() as usize
    }

    fn soft_slop(&self) -> usize {
        PAGE_SIZE.min(self.usable_len / 4).max(512)
    }

    fn current_used_from_saved_context(&self, rsp: usize) -> usize {
        self.used_from_rsp(rsp)
    }

    fn observe_stack_pointer(&mut self, rsp: usize) -> bool {
        let used = self.used_from_rsp(rsp);
        self.peak_bytes = self.peak_bytes.max(used);

        let soft_low = self.usable_low().saturating_add(self.soft_slop());
        if rsp < soft_low || rsp > self.usable_high() {
            self.overflowed = true;
            true
        } else {
            false
        }
    }

    fn used_from_rsp(&self, rsp: usize) -> usize {
        let high = self.usable_high();
        let low = self.usable_low();
        if rsp >= high {
            0
        } else if rsp <= low {
            self.usable_len
        } else {
            high - rsp
        }
    }

    fn contains_guard_fault(&self, fault_addr: usize) -> bool {
        let base = self.mapping_base.as_ptr() as usize;
        let low_guard_end = base + self.guard_low_len;
        let high_guard_start = self.usable_high();
        let high_guard_end = high_guard_start + self.guard_high_len;

        (fault_addr >= base && fault_addr < low_guard_end)
            || (fault_addr >= high_guard_start && fault_addr < high_guard_end)
    }
}

impl Drop for TallyStack {
    fn drop(&mut self) {
        unsafe {
            munmap(self.mapping_base.as_ptr() as *mut c_void, self.mapping_len);
        }
    }
}

impl TallyHeap {
    fn new(limit_bytes: Option<usize>) -> Result<Self, TallyError> {
        let (arena, unlimited) = match limit_bytes {
            Some(0) => (None, false),
            Some(limit_bytes) => (
                Some(MmapRegion::map(
                    page_align(limit_bytes)?,
                    PROT_READ | PROT_WRITE,
                    0,
                )?),
                false,
            ),
            None => (None, true),
        };

        Ok(Self {
            arena,
            unlimited,
            offset: 0,
            live_bytes: 0,
            peak_live_bytes: 0,
            allocations: 0,
            deallocations: 0,
            allocation_failures: 0,
        })
    }

    fn limit_bytes(&self) -> usize {
        self.arena.as_ref().map(|arena| arena.len).unwrap_or(0)
    }

    fn is_unlimited(&self) -> bool {
        self.unlimited
    }

    fn committed_bytes(&self) -> usize {
        self.offset
    }

    unsafe fn alloc(&mut self, size: usize, align: usize) -> Result<*mut c_void, ThreadError> {
        if size == 0 || !align.is_power_of_two() {
            self.allocation_failures = self.allocation_failures.saturating_add(1);
            return Err(ThreadError::InvalidAllocationRequest);
        }

        if self.unlimited {
            let ptr = host_alloc(size, align);
            if ptr.is_null() {
                self.allocation_failures = self.allocation_failures.saturating_add(1);
                return Err(ThreadError::HeapLimit);
            }
            self.live_bytes = self.live_bytes.saturating_add(size);
            self.peak_live_bytes = self.peak_live_bytes.max(self.live_bytes);
            self.allocations = self.allocations.saturating_add(1);
            return Ok(ptr);
        }

        let Some(arena) = self.arena.as_mut() else {
            self.allocation_failures = self.allocation_failures.saturating_add(1);
            return Err(ThreadError::HeapLimit);
        };

        let aligned_offset = align_up(self.offset, align).ok_or_else(|| {
            self.allocation_failures = self.allocation_failures.saturating_add(1);
            ThreadError::HeapLimit
        })?;
        let end = aligned_offset.checked_add(size).ok_or_else(|| {
            self.allocation_failures = self.allocation_failures.saturating_add(1);
            ThreadError::HeapLimit
        })?;

        if end > arena.len {
            self.allocation_failures = self.allocation_failures.saturating_add(1);
            return Err(ThreadError::HeapLimit);
        }

        self.offset = end;
        self.live_bytes = self.live_bytes.saturating_add(size);
        self.peak_live_bytes = self.peak_live_bytes.max(self.live_bytes);
        self.allocations = self.allocations.saturating_add(1);
        Ok(arena.base.as_ptr().add(aligned_offset) as *mut c_void)
    }

    unsafe fn dealloc(&mut self, ptr: *mut c_void, size: usize, _align: usize) {
        if ptr.is_null() {
            return;
        }
        self.deallocations = self.deallocations.saturating_add(1);
        self.live_bytes = self.live_bytes.saturating_sub(size);
        if self.unlimited {
            host_dealloc(ptr, size, _align);
        }
    }

    unsafe fn realloc(
        &mut self,
        ptr: *mut c_void,
        old_size: usize,
        align: usize,
        new_size: usize,
    ) -> Result<*mut c_void, ThreadError> {
        if ptr.is_null() {
            return self.alloc(new_size, align);
        }
        if new_size == 0 {
            self.dealloc(ptr, old_size, align);
            return Ok(ptr::null_mut());
        }

        if self.unlimited {
            let new_ptr = host_realloc(ptr, old_size, align, new_size);
            if new_ptr.is_null() {
                self.allocation_failures = self.allocation_failures.saturating_add(1);
                return Err(ThreadError::HeapLimit);
            }
            self.live_bytes = self.live_bytes.saturating_sub(old_size);
            self.live_bytes = self.live_bytes.saturating_add(new_size);
            self.peak_live_bytes = self.peak_live_bytes.max(self.live_bytes);
            self.allocations = self.allocations.saturating_add(1);
            self.deallocations = self.deallocations.saturating_add(1);
            return Ok(new_ptr);
        }

        let new_ptr = self.alloc(new_size, align)?;
        ptr::copy_nonoverlapping(ptr as *const u8, new_ptr as *mut u8, old_size.min(new_size));
        self.dealloc(ptr, old_size, align);
        Ok(new_ptr)
    }
}

impl MmapRegion {
    fn map(len: usize, prot: c_int, extra_flags: c_int) -> Result<Self, TallyError> {
        if len == 0 {
            return Err(TallyError::new("mmap length must be positive"));
        }

        let ptr = unsafe {
            mmap(
                ptr::null_mut(),
                len,
                prot,
                MAP_PRIVATE | MAP_ANONYMOUS | extra_flags,
                -1,
                0,
            )
        };
        if ptr == MAP_FAILED || ptr.is_null() {
            return Err(TallyError::new("mmap failed"));
        }

        Ok(Self {
            base: unsafe { NonNull::new_unchecked(ptr as *mut u8) },
            len,
        })
    }
}

impl Drop for MmapRegion {
    fn drop(&mut self) {
        unsafe {
            munmap(self.base.as_ptr() as *mut c_void, self.len);
        }
    }
}

fn page_align(size: usize) -> Result<usize, TallyError> {
    align_up(size, PAGE_SIZE).ok_or_else(|| TallyError::new("page alignment overflow"))
}

fn align_up(value: usize, align: usize) -> Option<usize> {
    let mask = align.checked_sub(1)?;
    value.checked_add(mask).map(|value| value & !mask)
}

unsafe fn protect_region(ptr: *mut u8, len: usize, prot: c_int) -> Result<(), TallyError> {
    if len == 0 {
        return Ok(());
    }
    if mprotect(ptr as *mut c_void, len, prot) != 0 {
        Err(TallyError::new("mprotect failed"))
    } else {
        Ok(())
    }
}

fn ensure_memory_signal_support() -> Result<(), TallyError> {
    SIGNAL_HANDLER_ONCE.call_once(|| {
        let action = SigAction {
            sa_sigaction: tally_sigsegv_handler as *const () as usize,
            sa_mask: [0; 16],
            sa_flags: SA_SIGINFO | SA_ONSTACK | SA_NODEFER,
            sa_restorer: 0,
        };
        let status = unsafe { sigaction(SIGSEGV, &action, ptr::null_mut()) };
        SIGNAL_HANDLER_STATUS.store(status, Ordering::SeqCst);
    });

    if SIGNAL_HANDLER_STATUS.load(Ordering::SeqCst) != 0 {
        return Err(TallyError::new("failed to install SIGSEGV handler"));
    }

    install_alt_signal_stack_for_current_thread()
}

fn install_alt_signal_stack_for_current_thread() -> Result<(), TallyError> {
    ALT_SIGNAL_STACK.with(|slot| {
        if !slot.get().is_null() {
            return Ok(());
        }

        let region = MmapRegion::map(SIGNAL_STACK_SIZE, PROT_READ | PROT_WRITE, 0)?;
        let stack = StackT {
            ss_sp: region.base.as_ptr() as *mut c_void,
            ss_flags: 0,
            ss_size: region.len,
        };
        if unsafe { sigaltstack(&stack, ptr::null_mut()) } != 0 {
            return Err(TallyError::new("failed to install alternate signal stack"));
        }

        let ptr = region.base.as_ptr() as *mut c_void;
        mem::forget(region);
        slot.set(ptr);
        Ok(())
    })
}

unsafe extern "C" fn tally_sigsegv_handler(
    signum: c_int,
    info: *mut SigInfo,
    _context: *mut c_void,
) {
    let fault_addr = if info.is_null() {
        0
    } else {
        (*info).si_addr as usize
    };
    let thread = CURRENT_THREAD;
    if !thread.is_null()
        && (*thread).state == ThreadState::Running
        && (*thread).stack.contains_guard_fault(fault_addr)
    {
        (*thread).stack.overflowed = true;
        (*thread).stack.peak_bytes = (*thread).stack.usable_len;
        (*thread).mark_errored(ThreadError::StackGuardFault);
        yield_to_scheduler(thread);
        _exit(128 + signum);
    }

    signal(signum, SIG_DFL);
    raise(signum);
    _exit(128 + signum);
}

#[inline(always)]
fn current_stack_pointer() -> usize {
    let rsp: usize;
    unsafe {
        asm!("mov {}, rsp", out(reg) rsp, options(nomem, nostack, preserves_flags));
    }
    rsp
}

unsafe fn mark_current_thread_errored(error: ThreadError) {
    let thread = CURRENT_THREAD;
    if thread.is_null() {
        return;
    }
    (*thread).mark_errored(error);
    if !SCHEDULER_CONTEXT.is_null() {
        yield_to_scheduler(thread);
    }
}

impl TallyThread {
    fn new(
        id: usize,
        entry: TallyEntry,
        arg: *mut c_void,
        budget_per_cycle: i64,
        limits: MemoryLimits,
    ) -> Result<Self, TallyError> {
        let stack = TallyStack::new(limits.stack_bytes)?;
        let top = stack.usable_high();
        let aligned_top = top & !0xf;
        let initial_rsp = aligned_top
            .checked_sub(16)
            .ok_or_else(|| TallyError::new("stack too small"))?;

        unsafe {
            (initial_rsp as *mut usize).write(thread_trampoline as *const () as usize);
        }

        Ok(Self {
            id,
            context: Context {
                rsp: initial_rsp,
                ..Context::default()
            },
            stack,
            heap: TallyHeap::new(limits.heap_bytes)?,
            entry,
            arg,
            budget_per_cycle,
            remaining_budget: 0,
            state: ThreadState::Ready,
            error: None,
            cycles_run: 0,
            charges: 0,
            work_units: 0,
        })
    }

    fn stats(&self) -> ThreadStats {
        ThreadStats {
            id: self.id,
            budget_per_cycle: self.budget_per_cycle,
            remaining_budget: self.remaining_budget,
            state: self.state,
            cycles_run: self.cycles_run,
            charges: self.charges,
            work_units: self.work_units,
            error: self.error,
        }
    }

    fn memory_stats(&self) -> MemoryStats {
        MemoryStats {
            stack_limit_bytes: self.stack.usable_len,
            stack_used_bytes: self.stack.current_used_from_saved_context(self.context.rsp),
            stack_peak_bytes: self.stack.peak_bytes,
            stack_overflowed: self.stack.overflowed,
            heap_limit_bytes: self.heap.limit_bytes(),
            heap_unlimited: self.heap.is_unlimited(),
            heap_live_bytes: self.heap.live_bytes,
            heap_peak_live_bytes: self.heap.peak_live_bytes,
            heap_committed_bytes: self.heap.committed_bytes(),
            allocations: self.heap.allocations,
            deallocations: self.heap.deallocations,
            allocation_failures: self.heap.allocation_failures,
            error: self.error,
        }
    }

    unsafe fn observe_stack_pointer(&mut self, rsp: usize) -> bool {
        if self.stack.observe_stack_pointer(rsp) {
            self.mark_errored(ThreadError::StackSoftLimit);
            true
        } else {
            false
        }
    }

    fn mark_errored(&mut self, error: ThreadError) {
        self.error.get_or_insert(error);
        self.state = ThreadState::Errored;
    }
}

impl DynamicLibrary {
    fn open(path: &Path) -> Result<Self, TallyError> {
        Self::open_with_dlopen(path, RTLD_NOW | RTLD_GLOBAL)
    }

    fn open_in_new_namespace(path: &Path) -> Result<Self, TallyError> {
        Self::open_with_dlmopen(LM_ID_NEWLM, path, RTLD_NOW | RTLD_LOCAL)
    }

    fn open_in_namespace(namespace_id: LinkMapNamespace, path: &Path) -> Result<Self, TallyError> {
        Self::open_with_dlmopen(namespace_id, path, RTLD_NOW | RTLD_LOCAL)
    }

    fn open_with_dlopen(path: &Path, flags: c_int) -> Result<Self, TallyError> {
        let path = CString::new(path.to_string_lossy().as_bytes())
            .map_err(|_| TallyError::new("shared object path contains a NUL byte"))?;
        unsafe {
            let handle = dlopen(path.as_ptr(), flags);
            if handle.is_null() {
                return Err(TallyError::new(dl_error()));
            }
            Ok(Self { handle })
        }
    }

    fn open_with_dlmopen(
        namespace_id: LinkMapNamespace,
        path: &Path,
        flags: c_int,
    ) -> Result<Self, TallyError> {
        let path = CString::new(path.to_string_lossy().as_bytes())
            .map_err(|_| TallyError::new("shared object path contains a NUL byte"))?;
        unsafe {
            let handle = dlmopen(namespace_id, path.as_ptr(), flags);
            if handle.is_null() {
                return Err(TallyError::new(dl_error()));
            }
            Ok(Self { handle })
        }
    }

    fn symbol(&self, symbol: &str) -> Result<TallyEntry, TallyError> {
        let symbol =
            CString::new(symbol).map_err(|_| TallyError::new("symbol name contains a NUL byte"))?;
        unsafe {
            let raw = dlsym(self.handle, symbol.as_ptr());
            if raw.is_null() {
                return Err(TallyError::new(dl_error()));
            }
            Ok(mem::transmute::<*mut c_void, TallyEntry>(raw))
        }
    }

    fn raw_symbol(&self, symbol: &str) -> Result<*mut c_void, TallyError> {
        let symbol =
            CString::new(symbol).map_err(|_| TallyError::new("symbol name contains a NUL byte"))?;
        unsafe {
            let raw = dlsym(self.handle, symbol.as_ptr());
            if raw.is_null() {
                return Err(TallyError::new(dl_error()));
            }
            Ok(raw)
        }
    }

    fn namespace_id(&self) -> Result<LinkMapNamespace, TallyError> {
        let mut namespace_id = 0 as LinkMapNamespace;
        unsafe {
            let result = dlinfo(
                self.handle,
                RTLD_DI_LMID,
                &mut namespace_id as *mut LinkMapNamespace as *mut c_void,
            );
            if result != 0 {
                return Err(TallyError::new(dl_error()));
            }
        }
        Ok(namespace_id)
    }
}

impl TallyStdEnvironment {
    fn open(bridge_path: &Path) -> Result<Self, TallyError> {
        let bridge = DynamicLibrary::open_in_new_namespace(bridge_path)?;
        let namespace_id = bridge.namespace_id()?;
        install_tally_abi_hooks(&bridge)?;
        Ok(Self {
            namespace_id,
            _bridge: bridge,
            workloads: Vec::new(),
        })
    }

    fn load_function(&mut self, path: &Path, symbol: &str) -> Result<TallyEntry, TallyError> {
        let library = DynamicLibrary::open_in_namespace(self.namespace_id, path)?;
        let function = library.symbol(symbol)?;
        self.workloads.push(library);
        Ok(function)
    }

    fn unload_workloads(&mut self) -> usize {
        let unloaded = self.workloads.len();
        self.workloads.clear();
        unloaded
    }
}

impl Drop for DynamicLibrary {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe {
                dlclose(self.handle);
            }
            self.handle = ptr::null_mut();
        }
    }
}

fn dl_error() -> String {
    unsafe {
        let err = dlerror();
        if err.is_null() {
            "dynamic loader error".to_string()
        } else {
            CStr::from_ptr(err).to_string_lossy().into_owned()
        }
    }
}

fn install_tally_abi_hooks(bridge: &DynamicLibrary) -> Result<(), TallyError> {
    let set_hooks = bridge.raw_symbol("tally_abi_set_hooks")?;
    let set_hooks = unsafe { mem::transmute::<*mut c_void, TallyAbiSetHooks>(set_hooks) };
    let hooks = TallyAbiHooks {
        charge: Some(tally_abi_bridge_charge),
        alloc: Some(tally_abi_bridge_alloc),
        dealloc: Some(tally_abi_bridge_dealloc),
        realloc: Some(tally_abi_bridge_realloc),
    };

    unsafe {
        set_hooks(&hooks as *const TallyAbiHooks);
    }
    Ok(())
}

extern "C" fn tally_abi_bridge_charge(cost: u64) {
    __tally_charge(cost);
}

extern "C" fn tally_abi_bridge_alloc(size: u64, align: u64) -> *mut c_void {
    __tally_alloc(size, align)
}

extern "C" fn tally_abi_bridge_dealloc(ptr: *mut c_void, size: u64, align: u64) {
    __tally_dealloc(ptr, size, align);
}

extern "C" fn tally_abi_bridge_realloc(
    ptr: *mut c_void,
    old_size: u64,
    align: u64,
    new_size: u64,
) -> *mut c_void {
    __tally_realloc(ptr, old_size, align, new_size)
}

#[no_mangle]
pub extern "C" fn __tally_charge(cost: u64) {
    unsafe {
        let thread = CURRENT_THREAD;
        if thread.is_null() {
            return;
        }

        let rsp = current_stack_pointer();
        if (*thread).observe_stack_pointer(rsp) {
            yield_to_scheduler(thread);
            return;
        }

        (*thread).charges = (*thread).charges.saturating_add(1);
        let cost = i64::try_from(cost).unwrap_or(i64::MAX);
        (*thread).remaining_budget = (*thread).remaining_budget.saturating_sub(cost);

        if (*thread).remaining_budget <= 0 && (*thread).state == ThreadState::Running {
            (*thread).state = ThreadState::Yielded;
            yield_to_scheduler(thread);
        }
    }
}

#[no_mangle]
pub extern "C" fn __tally_alloc(size: u64, align: u64) -> *mut c_void {
    let Some(size) = usize::try_from(size).ok() else {
        unsafe {
            mark_current_thread_errored(ThreadError::InvalidAllocationRequest);
        }
        return ptr::null_mut();
    };
    let Some(align) = usize::try_from(align).ok() else {
        unsafe {
            mark_current_thread_errored(ThreadError::InvalidAllocationRequest);
        }
        return ptr::null_mut();
    };

    unsafe {
        let thread = CURRENT_THREAD;
        if thread.is_null() {
            return host_alloc(size, align);
        }

        match (*thread).heap.alloc(size, align) {
            Ok(ptr) => ptr,
            Err(error) => {
                (*thread).mark_errored(error);
                yield_to_scheduler(thread);
                ptr::null_mut()
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn __tally_dealloc(ptr: *mut c_void, size: u64, align: u64) {
    let Some(size) = usize::try_from(size).ok() else {
        unsafe {
            mark_current_thread_errored(ThreadError::InvalidAllocationRequest);
        }
        return;
    };
    let Some(align) = usize::try_from(align).ok() else {
        unsafe {
            mark_current_thread_errored(ThreadError::InvalidAllocationRequest);
        }
        return;
    };

    unsafe {
        let thread = CURRENT_THREAD;
        if thread.is_null() {
            host_dealloc(ptr, size, align);
        } else {
            (*thread).heap.dealloc(ptr, size, align);
        }
    }
}

#[no_mangle]
pub extern "C" fn __tally_realloc(
    ptr: *mut c_void,
    old_size: u64,
    align: u64,
    new_size: u64,
) -> *mut c_void {
    let Some(old_size) = usize::try_from(old_size).ok() else {
        unsafe {
            mark_current_thread_errored(ThreadError::InvalidAllocationRequest);
        }
        return ptr::null_mut();
    };
    let Some(align) = usize::try_from(align).ok() else {
        unsafe {
            mark_current_thread_errored(ThreadError::InvalidAllocationRequest);
        }
        return ptr::null_mut();
    };
    let Some(new_size) = usize::try_from(new_size).ok() else {
        unsafe {
            mark_current_thread_errored(ThreadError::InvalidAllocationRequest);
        }
        return ptr::null_mut();
    };

    unsafe {
        let thread = CURRENT_THREAD;
        if thread.is_null() {
            return host_realloc(ptr, old_size, align, new_size);
        }

        match (*thread).heap.realloc(ptr, old_size, align, new_size) {
            Ok(ptr) => ptr,
            Err(error) => {
                (*thread).mark_errored(error);
                yield_to_scheduler(thread);
                ptr::null_mut()
            }
        }
    }
}

unsafe fn host_alloc(size: usize, align: usize) -> *mut c_void {
    let Ok(layout) = std::alloc::Layout::from_size_align(size, align) else {
        return ptr::null_mut();
    };
    if layout.size() == 0 {
        return ptr::null_mut();
    }
    std::alloc::alloc(layout) as *mut c_void
}

unsafe fn host_dealloc(ptr: *mut c_void, size: usize, align: usize) {
    if ptr.is_null() {
        return;
    }
    let Ok(layout) = std::alloc::Layout::from_size_align(size, align) else {
        return;
    };
    if layout.size() != 0 {
        std::alloc::dealloc(ptr as *mut u8, layout);
    }
}

unsafe fn host_realloc(
    ptr: *mut c_void,
    old_size: usize,
    align: usize,
    new_size: usize,
) -> *mut c_void {
    if ptr.is_null() {
        return host_alloc(new_size, align);
    }
    let Ok(layout) = std::alloc::Layout::from_size_align(old_size, align) else {
        return ptr::null_mut();
    };
    if layout.size() == 0 {
        return host_alloc(new_size, align);
    }
    std::alloc::realloc(ptr as *mut u8, layout, new_size) as *mut c_void
}

pub fn record_current_thread_work(units: u64) {
    unsafe {
        let thread = CURRENT_THREAD;
        if !thread.is_null() {
            (*thread).work_units = (*thread).work_units.saturating_add(units);
        }
    }
}

pub fn current_thread_id() -> Option<usize> {
    unsafe {
        let thread = CURRENT_THREAD;
        if thread.is_null() {
            None
        } else {
            Some((*thread).id)
        }
    }
}

unsafe extern "C" fn thread_trampoline() -> ! {
    let thread = CURRENT_THREAD;
    if thread.is_null() {
        std::process::abort();
    }

    let entry = (*thread).entry;
    let arg = (*thread).arg;
    entry(arg);
    (*thread).state = ThreadState::Returned;
    yield_to_scheduler(thread);
    std::hint::unreachable_unchecked()
}

unsafe fn yield_to_scheduler(thread: *mut TallyThread) {
    let scheduler = SCHEDULER_CONTEXT;
    if scheduler.is_null() {
        (*thread).state = ThreadState::Errored;
        return;
    }
    tally_swap_context(
        &mut (*thread).context as *mut Context,
        scheduler as *const Context,
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "C" fn yielding_counter(arg: *mut c_void) {
        let counter = &mut *(arg as *mut u64);
        loop {
            *counter += 1;
            __tally_charge(1);
        }
    }

    unsafe extern "C" fn returns_immediately(arg: *mut c_void) {
        let counter = &mut *(arg as *mut u64);
        *counter += 1;
    }

    #[repr(C)]
    struct HeapProbeArgs {
        writes: u64,
        observed_null: u64,
    }

    unsafe extern "C" fn heap_allocates_and_frees(arg: *mut c_void) {
        let args = &mut *(arg as *mut HeapProbeArgs);
        let ptr = __tally_alloc(256, 16) as *mut u8;
        if ptr.is_null() {
            args.observed_null = 1;
            return;
        }

        for offset in 0..256 {
            ptr::write_volatile(ptr.add(offset), offset as u8);
        }
        args.writes = 256;
        __tally_dealloc(ptr as *mut c_void, 256, 16);
        __tally_charge(1);
    }

    unsafe extern "C" fn heap_exhausts(arg: *mut c_void) {
        let args = &mut *(arg as *mut HeapProbeArgs);
        let ptr = __tally_alloc(8192, 16);
        if ptr.is_null() {
            args.observed_null = 1;
        }
        args.writes = 1;
        __tally_charge(1);
    }

    #[repr(C)]
    struct StackProbeArgs {
        calls: u64,
        marker: u8,
    }

    unsafe extern "C" fn recursive_stack_probe(arg: *mut c_void) {
        recursive_stack_probe_inner(arg as *mut StackProbeArgs);
    }

    #[allow(unconditional_recursion)]
    #[inline(never)]
    unsafe fn recursive_stack_probe_inner(args: *mut StackProbeArgs) {
        let mut local = [0_u8; 2048];
        ptr::write_volatile(local.as_mut_ptr(), (*args).marker);
        (*args).calls = (*args).calls.saturating_add(1);
        __tally_charge(1);
        recursive_stack_probe_inner(args);
        ptr::read_volatile(local.as_ptr());
    }

    unsafe extern "C" fn large_stack_frame_probe(arg: *mut c_void) {
        let args = &mut *(arg as *mut StackProbeArgs);
        let mut local = [0_u8; 96 * 1024];
        for offset in (0..local.len()).step_by(4096) {
            ptr::write_volatile(local.as_mut_ptr().add(offset), args.marker);
        }
        args.calls = args.calls.saturating_add(1);
        __tally_charge(1);
    }

    #[test]
    fn budget_debt_carries_between_cycles() {
        let mut manager = TallyManager::new();
        let mut counter = 0_u64;
        let id = manager
            .spawn_with_stack(
                yielding_counter,
                &mut counter as *mut u64 as *mut c_void,
                3,
                65536,
            )
            .unwrap();

        manager.run_cycle(id).unwrap();
        let first = manager.stats()[id];
        assert!(counter >= 3);
        assert!(first.remaining_budget <= 0);

        manager.run_cycle(id).unwrap();
        let second = manager.stats()[id];
        assert!(counter >= 6);
        assert!(second.remaining_budget <= 0);
    }

    #[test]
    fn returned_threads_are_not_resumed() {
        let mut manager = TallyManager::new();
        let mut counter = 0_u64;
        let id = manager
            .spawn_with_stack(
                returns_immediately,
                &mut counter as *mut u64 as *mut c_void,
                10,
                65536,
            )
            .unwrap();

        assert_eq!(manager.run_cycle(id).unwrap(), ThreadState::Returned);
        assert_eq!(counter, 1);
        assert_eq!(manager.run_cycle(id).unwrap(), ThreadState::Returned);
        assert_eq!(counter, 1);
    }

    #[test]
    fn rejects_nonpositive_budget() {
        let mut manager = TallyManager::new();
        let mut counter = 0_u64;
        let result = manager.spawn_with_stack(
            yielding_counter,
            &mut counter as *mut u64 as *mut c_void,
            0,
            65536,
        );
        assert!(result.is_err());
    }

    #[test]
    fn per_thread_heap_tracks_allocations_and_deallocations() {
        let mut manager = TallyManager::new();
        let mut args = HeapProbeArgs {
            writes: 0,
            observed_null: 0,
        };
        let id = manager
            .spawn_with_limits(
                heap_allocates_and_frees,
                &mut args as *mut HeapProbeArgs as *mut c_void,
                10,
                MemoryLimits::new(65_536, 1024),
            )
            .unwrap();

        assert_eq!(manager.run_cycle(id).unwrap(), ThreadState::Returned);
        assert_eq!(args.writes, 256);
        assert_eq!(args.observed_null, 0);

        let memory = manager.memory_stats_for_thread(id).unwrap();
        assert_eq!(memory.heap_limit_bytes, 4096);
        assert!(!memory.heap_unlimited);
        assert_eq!(memory.heap_live_bytes, 0);
        assert_eq!(memory.heap_peak_live_bytes, 256);
        assert_eq!(memory.allocations, 1);
        assert_eq!(memory.deallocations, 1);
        assert_eq!(memory.allocation_failures, 0);
        assert_eq!(memory.error, None);
    }

    #[test]
    fn unlimited_heap_delegates_to_host_allocator_and_tracks_usage() {
        let mut manager = TallyManager::new();
        let mut args = HeapProbeArgs {
            writes: 0,
            observed_null: 0,
        };
        let id = manager
            .spawn_with_limits(
                heap_allocates_and_frees,
                &mut args as *mut HeapProbeArgs as *mut c_void,
                10,
                MemoryLimits::unlimited_heap(65_536),
            )
            .unwrap();

        assert_eq!(manager.run_cycle(id).unwrap(), ThreadState::Returned);
        assert_eq!(args.writes, 256);
        assert_eq!(args.observed_null, 0);

        let memory = manager.memory_stats_for_thread(id).unwrap();
        assert_eq!(memory.heap_limit_bytes, 0);
        assert!(memory.heap_unlimited);
        assert_eq!(memory.heap_live_bytes, 0);
        assert_eq!(memory.heap_peak_live_bytes, 256);
        assert_eq!(memory.allocations, 1);
        assert_eq!(memory.deallocations, 1);
        assert_eq!(memory.allocation_failures, 0);
        assert_eq!(memory.error, None);
    }

    #[test]
    fn heap_exhaustion_errors_one_thread_and_scheduler_continues() {
        let mut manager = TallyManager::new();
        let mut heap_args = HeapProbeArgs {
            writes: 0,
            observed_null: 0,
        };
        let failing = manager
            .spawn_with_limits(
                heap_exhausts,
                &mut heap_args as *mut HeapProbeArgs as *mut c_void,
                10,
                MemoryLimits::new(65_536, 128),
            )
            .unwrap();

        let mut counter = 0_u64;
        let healthy = manager
            .spawn_with_stack(
                returns_immediately,
                &mut counter as *mut u64 as *mut c_void,
                10,
                65_536,
            )
            .unwrap();

        assert_eq!(manager.run_cycle(failing).unwrap(), ThreadState::Errored);
        let failing_stats = manager.stats_for_thread(failing).unwrap();
        assert_eq!(failing_stats.error, Some(ThreadError::HeapLimit));
        let failing_memory = manager.memory_stats_for_thread(failing).unwrap();
        assert!(!failing_memory.heap_unlimited);
        assert_eq!(failing_memory.allocation_failures, 1);
        assert_eq!(failing_memory.error, Some(ThreadError::HeapLimit));

        assert_eq!(manager.run_cycle(healthy).unwrap(), ThreadState::Returned);
        assert_eq!(counter, 1);
        assert_eq!(manager.run_cycle(failing).unwrap(), ThreadState::Errored);
    }

    #[test]
    fn deep_recursion_stack_overflow_is_scheduler_recoverable() {
        let mut manager = TallyManager::new();
        let mut args = StackProbeArgs {
            calls: 0,
            marker: 0x5a,
        };
        let failing = manager
            .spawn_with_limits(
                recursive_stack_probe,
                &mut args as *mut StackProbeArgs as *mut c_void,
                1_000_000,
                MemoryLimits::stack_only(32 * 1024),
            )
            .unwrap();

        let mut counter = 0_u64;
        let healthy = manager
            .spawn_with_stack(
                returns_immediately,
                &mut counter as *mut u64 as *mut c_void,
                10,
                65_536,
            )
            .unwrap();

        assert_eq!(manager.run_cycle(failing).unwrap(), ThreadState::Errored);
        assert!(args.calls > 0);
        let failing_stats = manager.stats_for_thread(failing).unwrap();
        assert_eq!(failing_stats.error, Some(ThreadError::StackSoftLimit));
        let memory = manager.memory_stats_for_thread(failing).unwrap();
        assert!(memory.stack_overflowed);
        assert_eq!(memory.error, Some(ThreadError::StackSoftLimit));

        assert_eq!(manager.run_cycle(healthy).unwrap(), ThreadState::Returned);
        assert_eq!(counter, 1);
    }

    #[test]
    fn excessive_stack_frame_is_scheduler_recoverable() {
        let mut manager = TallyManager::new();
        let mut args = StackProbeArgs {
            calls: 0,
            marker: 0xa5,
        };
        let failing = manager
            .spawn_with_limits(
                large_stack_frame_probe,
                &mut args as *mut StackProbeArgs as *mut c_void,
                1_000_000,
                MemoryLimits::stack_only(32 * 1024),
            )
            .unwrap();

        let mut counter = 0_u64;
        let healthy = manager
            .spawn_with_stack(
                returns_immediately,
                &mut counter as *mut u64 as *mut c_void,
                10,
                65_536,
            )
            .unwrap();

        assert_eq!(manager.run_cycle(failing).unwrap(), ThreadState::Errored);
        let error = manager.stats_for_thread(failing).unwrap().error;
        assert!(matches!(
            error,
            Some(ThreadError::StackGuardFault) | Some(ThreadError::StackSoftLimit)
        ));

        let memory = manager.memory_stats_for_thread(failing).unwrap();
        assert!(memory.stack_overflowed);
        assert_eq!(memory.error, error);

        assert_eq!(manager.run_cycle(healthy).unwrap(), ThreadState::Returned);
        assert_eq!(counter, 1);
    }

    #[test]
    fn virtual_calibration_converts_credit_to_internal_budget() {
        let calibration = VirtualCalibration {
            seconds_per_budget_unit: 0.001,
            seconds_per_activation: 0.010,
            seconds_per_scheduler_round: 0.020,
            min_internal_budget: 5,
            max_internal_budget: 100,
        };

        assert_eq!(calibration.budget_from_seconds(0.010), 0);
        assert_eq!(calibration.budget_from_seconds(0.014), 0);
        assert_eq!(calibration.budget_from_seconds(0.015), 5);
        assert_eq!(calibration.budget_from_seconds(0.060), 50);
        assert_eq!(calibration.budget_from_seconds(1.000), 100);
        assert!((calibration.context_switch_budget_units() - 10.0).abs() < 1.0e-12);
        assert!((calibration.scheduler_round_budget_units() - 20.0).abs() < 1.0e-12);
    }

    #[test]
    fn virtual_thread_accumulates_share_credit() {
        let calibration = VirtualCalibration {
            seconds_per_budget_unit: 0.001,
            seconds_per_activation: 0.010,
            seconds_per_scheduler_round: 0.020,
            min_internal_budget: 5,
            max_internal_budget: 100,
        };
        let mut virtual_thread = VirtualThread::new(0, 0.25);

        virtual_thread.add_credit(4.0);
        assert!((virtual_thread.credit_seconds - 1.0).abs() < 1.0e-12);
        assert_eq!(virtual_thread.budget_units_consumed, 0);

        virtual_thread.charge_scheduler_round(&calibration, 4);
        assert!((virtual_thread.credit_seconds - 0.995).abs() < 1.0e-12);
    }

    #[test]
    fn virtual_thread_runs_only_after_affording_activation_and_budget() {
        let calibration = VirtualCalibration {
            seconds_per_budget_unit: 0.001,
            seconds_per_activation: 0.010,
            seconds_per_scheduler_round: 0.0,
            min_internal_budget: 5,
            max_internal_budget: 100,
        };

        let mut manager = TallyManager::new();
        let mut counter = 0_u64;
        let id = manager
            .spawn_with_stack(
                yielding_counter,
                &mut counter as *mut u64 as *mut c_void,
                1,
                65536,
            )
            .unwrap();
        let mut virtual_thread = VirtualThread::new(id, 1.0);

        assert_eq!(
            virtual_thread
                .run_ready(&mut manager, &calibration)
                .unwrap(),
            VirtualRunResult::NotReady
        );
        assert_eq!(virtual_thread.skipped_cycles, 1);

        virtual_thread.add_credit(0.015);
        let result = virtual_thread
            .run_ready(&mut manager, &calibration)
            .unwrap();
        assert!(matches!(
            result,
            VirtualRunResult::Ran(ThreadState::Yielded)
        ));
        assert_eq!(virtual_thread.last_budget, 5);
        assert!(counter >= 5);
        assert!(virtual_thread.credit_seconds <= 0.0);
        assert!(virtual_thread.budget_units_consumed >= 5);
    }

    #[test]
    fn virtual_calibration_round_trips_file() {
        let calibration = VirtualCalibration {
            seconds_per_budget_unit: 0.001,
            seconds_per_activation: 0.010,
            seconds_per_scheduler_round: 0.020,
            min_internal_budget: 5,
            max_internal_budget: 100,
        };
        let path = std::env::temp_dir().join("llvm_tally_virtual_calibration_test.txt");
        calibration.write_to_file(&path).unwrap();
        let loaded = VirtualCalibration::read_from_file(&path).unwrap();
        std::fs::remove_file(&path).ok();

        assert!(
            (loaded.seconds_per_budget_unit - calibration.seconds_per_budget_unit).abs() < 1e-12
        );
        assert!((loaded.seconds_per_activation - calibration.seconds_per_activation).abs() < 1e-12);
        assert!(
            (loaded.seconds_per_scheduler_round - calibration.seconds_per_scheduler_round).abs()
                < 1e-12
        );
        assert_eq!(loaded.min_internal_budget, calibration.min_internal_budget);
        assert_eq!(loaded.max_internal_budget, calibration.max_internal_budget);
    }

    #[test]
    fn adaptive_state_updates_unit_cost_from_window() {
        let mut calibration = VirtualCalibration {
            seconds_per_budget_unit: 0.001,
            seconds_per_activation: 0.0,
            seconds_per_scheduler_round: 0.0,
            min_internal_budget: 1,
            max_internal_budget: 1000,
        };
        let mut adaptive = VirtualAdaptiveState::default();
        adaptive.min_observation_seconds = 0.0;
        adaptive.observe(&mut calibration, 1.0, 500, 0, 0);

        assert_eq!(adaptive.updates, 1);
        assert!(calibration.seconds_per_budget_unit > 0.001);
    }

    #[test]
    fn native_virtual_calibration_smoke() {
        let calibration = VirtualCalibration::calibrate(VirtualCalibrationConfig {
            target_seconds: 0.0,
            work_per_sample: 1_000,
            stack_size: 65_536,
        })
        .unwrap();

        assert!(calibration.seconds_per_budget_unit > 0.0);
        assert!(calibration.min_internal_budget > 0);
        assert!(calibration.max_internal_budget >= calibration.min_internal_budget);
    }
}
