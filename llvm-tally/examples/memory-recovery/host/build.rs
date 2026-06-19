/*
 * Ensure symbols from the Rust host/runtime remain visible to instrumented
 * shared objects loaded with dlopen.
 */
fn main() {
    println!("cargo:rustc-link-arg=-rdynamic");
}
