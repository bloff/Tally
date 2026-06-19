/*
 * no_std memory-recovery workload for llvm-tally. It intentionally exhausts
 * stack or heap limits so the host can verify scheduler-level recovery.
 */
#![no_std]

use core::ffi::c_void;
use core::panic::PanicInfo;
use core::ptr::write_volatile;

const CASE_HEALTHY: u32 = 0;
const CASE_HEAP_OVERFLOW: u32 = 1;
const CASE_RECURSIVE_STACK: u32 = 2;
const CASE_LARGE_FRAME: u32 = 3;

#[repr(C)]
pub struct MemoryRecoveryArgs {
    pub case_id: u32,
    pub progress: u64,
    pub marker: u8,
}

extern "C" {
    fn __tally_alloc(size: u64, align: u64) -> *mut c_void;
    fn tally_memory_escape(ptr: *mut u8);
}

#[no_mangle]
pub unsafe extern "C" fn run_memory_recovery_case(args: *mut c_void) {
    let args = args as *mut MemoryRecoveryArgs;
    match (*args).case_id {
        CASE_HEALTHY => healthy(args),
        CASE_HEAP_OVERFLOW => heap_overflow(args),
        CASE_RECURSIVE_STACK => recursive_stack(args),
        CASE_LARGE_FRAME => large_frame(args),
        _ => healthy(args),
    }
}

#[inline(never)]
unsafe fn healthy(args: *mut MemoryRecoveryArgs) {
    (*args).progress = (*args).progress.wrapping_add(1);
}

#[inline(never)]
unsafe fn heap_overflow(args: *mut MemoryRecoveryArgs) {
    let ptr = __tally_alloc(8192, 16);
    if ptr.is_null() {
        (*args).progress = 1;
    } else {
        (*args).progress = 2;
    }
}

#[allow(unconditional_recursion)]
#[inline(never)]
unsafe fn recursive_stack(args: *mut MemoryRecoveryArgs) {
    let mut local = [0_u8; 2048];
    write_volatile(local.as_mut_ptr(), (*args).marker);
    (*args).progress = (*args).progress.wrapping_add(1);
    recursive_stack(args);
    write_volatile(local.as_mut_ptr(), (*args).marker);
}

#[inline(never)]
unsafe fn large_frame(args: *mut MemoryRecoveryArgs) {
    let mut local = [0_u8; 96 * 1024];
    tally_memory_escape(local.as_mut_ptr());
    let mut offset = 0_usize;
    while offset < local.len() {
        write_volatile(local.as_mut_ptr().add(offset), (*args).marker);
        offset += 4096;
    }
    tally_memory_escape(local.as_mut_ptr());
    (*args).progress = (*args).progress.wrapping_add(1);
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {}
}
