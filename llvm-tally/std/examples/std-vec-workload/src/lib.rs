/*
 * std-using workload for the future instrumented-std path. This intentionally
 * uses Vec, Box, iterators, and sorting so useful work occurs in std/alloc.
 */
use std::ffi::c_void;

#[repr(C)]
pub struct StdVecArgs {
    pub target_len: u64,
    pub sum: u64,
    pub len: u64,
}

#[no_mangle]
pub unsafe extern "C" fn run_std_vec_workload(args: *mut c_void) {
    let args = &mut *(args as *mut StdVecArgs);
    let mut values = Vec::with_capacity(args.target_len as usize);
    for i in 0..args.target_len {
        values.push(i.wrapping_mul(1_664_525).wrapping_add(1_013_904_223));
    }
    values.sort_unstable();

    let boxed: Box<[u64]> = values.into_boxed_slice();
    args.len = boxed.len() as u64;
    args.sum = boxed
        .iter()
        .copied()
        .fold(0_u64, |acc, value| acc.wrapping_add(value));
}
