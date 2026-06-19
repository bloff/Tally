/*
 * Rust minithread runtime for llvm-tally. It provides stackful context
 * switching, dynamic loading, work accounting, and the __tally_charge C ABI.
 */
use core::arch::global_asm;
use std::error::Error;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::fmt;
use std::mem;
use std::path::Path;
use std::ptr;

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
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn dlclose(handle: *mut c_void) -> c_int;
    fn dlerror() -> *const c_char;
}

const RTLD_NOW: c_int = 2;
const RTLD_GLOBAL: c_int = 0x100;
const DEFAULT_STACK_SIZE: usize = 1024 * 1024;

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

pub struct TallyThread {
    id: usize,
    context: Context,
    stack: Vec<u8>,
    entry: TallyEntry,
    arg: *mut c_void,
    budget_per_cycle: i64,
    remaining_budget: i64,
    state: ThreadState,
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
}

#[derive(Debug, Clone, Copy)]
pub struct VirtualCalibration {
    pub seconds_per_budget_unit: f64,
    pub seconds_per_activation: f64,
    pub seconds_per_scheduler_round: f64,
    pub min_internal_budget: i64,
    pub max_internal_budget: i64,
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
    pub activations: u64,
    pub skipped_cycles: u64,
    pub completed_cycles: u64,
}

pub struct TallyManager {
    scheduler_context: Context,
    threads: Vec<Box<TallyThread>>,
    libraries: Vec<DynamicLibrary>,
}

pub struct DynamicLibrary {
    handle: *mut c_void,
}

static mut CURRENT_THREAD: *mut TallyThread = ptr::null_mut();
static mut SCHEDULER_CONTEXT: *mut Context = ptr::null_mut();

impl TallyManager {
    pub fn new() -> Self {
        Self {
            scheduler_context: Context::default(),
            threads: Vec::new(),
            libraries: Vec::new(),
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
        if budget_per_cycle <= 0 {
            return Err(TallyError::new("budget_per_cycle must be positive"));
        }

        let id = self.threads.len();
        let thread = Box::new(TallyThread::new(
            id,
            entry,
            arg,
            budget_per_cycle,
            stack_size,
        )?);
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

impl VirtualThread {
    pub fn new(thread_id: usize, cpu_share: f64) -> Self {
        Self {
            thread_id,
            cpu_share: positive_or_zero(cpu_share),
            credit_seconds: 0.0,
            last_budget: 0,
            activations: 0,
            skipped_cycles: 0,
            completed_cycles: 0,
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

impl TallyThread {
    fn new(
        id: usize,
        entry: TallyEntry,
        arg: *mut c_void,
        budget_per_cycle: i64,
        stack_size: usize,
    ) -> Result<Self, TallyError> {
        if stack_size < 4096 {
            return Err(TallyError::new("stack_size must be at least 4096 bytes"));
        }

        let mut stack = vec![0_u8; stack_size];
        let top = stack.as_mut_ptr() as usize + stack.len();
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
            entry,
            arg,
            budget_per_cycle,
            remaining_budget: 0,
            state: ThreadState::Ready,
            cycles_run: 0,
            charges: 0,
            work_units: 0,
        })
    }

    fn stats(&self) -> ThreadStats {
        let _keep_stack_alive = self.stack.len();
        ThreadStats {
            id: self.id,
            budget_per_cycle: self.budget_per_cycle,
            remaining_budget: self.remaining_budget,
            state: self.state,
            cycles_run: self.cycles_run,
            charges: self.charges,
            work_units: self.work_units,
        }
    }
}

impl DynamicLibrary {
    fn open(path: &Path) -> Result<Self, TallyError> {
        let path = CString::new(path.to_string_lossy().as_bytes())
            .map_err(|_| TallyError::new("shared object path contains a NUL byte"))?;
        unsafe {
            let handle = dlopen(path.as_ptr(), RTLD_NOW | RTLD_GLOBAL);
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

#[no_mangle]
pub extern "C" fn __tally_charge(cost: u64) {
    unsafe {
        let thread = CURRENT_THREAD;
        if thread.is_null() {
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
    }
}
