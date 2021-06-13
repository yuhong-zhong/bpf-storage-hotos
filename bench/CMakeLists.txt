cmake_minimum_required(VERSION 3.10)
project(bpf_storage)

set(CMAKE_CXX_STANDARD 14)

add_compile_options(-fpermissive)

add_executable(read_bpf read_bpf.cpp)
add_executable(read_baseline read_baseline.cpp)

add_executable(uring_bpf uring_bpf.cpp)
add_executable(uring_baseline uring_baseline.cpp)

target_link_libraries(read_bpf pthread)
target_link_libraries(read_baseline pthread)
