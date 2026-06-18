# Build both Tally implementations from the repository root.
cmake_minimum_required(VERSION 3.16...4.3)
project(tally-all)

enable_testing()

add_subdirectory(gcc-tally)
add_subdirectory(llvm-tally)
