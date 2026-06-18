/*
 * Controlled no_std Rust workload for llvm-tally. It performs an infinite
 * random walk over graph functions exported by the host process.
 */
#![no_std]

use core::ffi::c_void;
use core::panic::PanicInfo;

#[repr(C)]
pub struct WalkArgs {
    pub current_node: u32,
    pub seed: u32,
}

unsafe extern "C" {
    fn tally_graph_neighbor_count(node: u32) -> u32;
    fn tally_graph_neighbor(node: u32, index: u32) -> u32;
    fn tally_graph_add_hop();
}

#[no_mangle]
pub unsafe extern "C" fn run_walk_budget(args: *mut c_void) {
    let input = &mut *(args as *mut WalkArgs);
    let mut node = input.current_node;
    let mut seed = input.seed;

    loop {
        let degree = tally_graph_neighbor_count(node);
        if degree == 0 {
            input.current_node = node;
            input.seed = seed;
            loop {}
        }

        seed = next_random(seed);
        let index = seed % degree;
        node = tally_graph_neighbor(node, index);
        tally_graph_add_hop();

        input.current_node = node;
        input.seed = seed;
    }
}

fn next_random(seed: u32) -> u32 {
    seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223)
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {}
}
