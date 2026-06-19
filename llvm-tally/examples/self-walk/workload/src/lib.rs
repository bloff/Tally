/*
 * Controlled no_std self-contained random walk for llvm-tally. It performs
 * graph generation entirely inside the instrumented workload.
 */
#![no_std]

use core::ffi::c_void;
use core::panic::PanicInfo;
use core::ptr::{read_volatile, write_volatile};

const NODE_MASK: u32 = 1023;
const OFFSETS: [u32; 4] = [1, 7, 31, 127];

#[repr(C)]
pub struct SelfWalkArgs {
    pub current_node: u32,
    pub seed: u32,
    pub vertices_walked: u64,
    pub target_vertices: u64,
}

#[no_mangle]
pub unsafe extern "C" fn run_self_walk(args: *mut c_void) {
    let input = args as *mut SelfWalkArgs;
    let mut node = read_volatile(&(*input).current_node);
    let mut seed = read_volatile(&(*input).seed);
    let mut vertices = read_volatile(&(*input).vertices_walked);
    let target = read_volatile(&(*input).target_vertices);

    while vertices < target {
        seed = next_random(seed);
        node = synthetic_neighbor(node, seed & 3);
        vertices = vertices.wrapping_add(1);

        write_volatile(&mut (*input).current_node, node);
        write_volatile(&mut (*input).seed, seed);
        write_volatile(&mut (*input).vertices_walked, vertices);
    }
}

#[inline(always)]
fn next_random(seed: u32) -> u32 {
    seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223)
}

#[inline(always)]
fn synthetic_neighbor(node: u32, index: u32) -> u32 {
    let mixed = node ^ (node << 5) ^ (node >> 3);
    mixed.wrapping_add(OFFSETS[(index & 3) as usize]) & NODE_MASK
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {}
}
