include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.4
    GIT_SHALLOW ON
)

FetchContent_MakeAvailable(googlebenchmark)

add_executable(Luau.Benchmark bench_main.cpp)
set_target_properties(Luau.Benchmark PROPERTIES OUTPUT_NAME luau-benchmark)

target_compile_features(Luau.Benchmark PRIVATE cxx_std_17)
target_compile_options(Luau.Benchmark PRIVATE ${LUAU_OPTIONS})

target_link_libraries(Luau.Benchmark PRIVATE benchmark::benchmark Luau.Ast Luau.Common Luau.Compiler Luau.VM Luau.CodeGen Luau.CLI.lib)
