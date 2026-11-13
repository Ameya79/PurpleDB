cmake_minimum_required(VERSION 3.16)
project(PurpleDB VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler warnings
if(MSVC)
  add_compile_options(/W4 /permissive-)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Sanitizers
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif()
endif()

if(ENABLE_UBSAN)
  if(NOT MSVC)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
  endif()
endif()

if(ENABLE_TSAN)
  if(NOT MSVC)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
  endif()
endif()

find_package(Threads REQUIRED)

# Core library sources
set(PURPLEDB_CORE_SOURCES
  core/storage/block.cc
  core/storage/block_builder.cc
  core/storage/cache.cc
  core/storage/compaction.cc
  core/storage/crc32c.cc
  core/storage/db_impl.cc
  core/storage/db_iter.cc
  core/storage/env.cc
  core/storage/filename.cc
  core/storage/filter_block.cc
  core/storage/filter_policy.cc
  core/storage/format.cc
  core/storage/memtable.cc
  core/storage/merger.cc
  core/storage/table.cc
  core/storage/table_builder.cc
  core/storage/version_edit.cc
  core/storage/version_set.cc
  core/storage/wal_reader.cc
  core/storage/wal_writer.cc
  core/storage/write_batch.cc
  core/vector/distance.cc
  core/vector/delete_bitmap.cc
  core/vector/vector_segment.cc
  core/vector/vector_index.cc
  core/vector/hnsw.cc
  core/embed/api_adapter.cc
  core/embed/local_embedder.cc
  core/pipeline/frame_decoder.cc
  core/pipeline/ingest_job.cc
  core/pipeline/moment_detector.cc
  core/query/query_engine.cc
  core/purpledb_c.cc
  core/util/arena.cc
  core/util/coding.cc
  core/util/hash.cc
  core/util/iterator.cc
  core/util/logging.cc
  core/util/sha256.cc
  core/util/slice.cc
  core/util/status.cc
)

set(PURPLEDB_CORE_HEADERS
  core/include/purpledb/db.h
  core/include/purpledb/filter_policy.h
  core/include/purpledb/iterator.h
  core/include/purpledb/purpledb_c.h
  core/include/purpledb/slice.h
  core/include/purpledb/status.h
  core/include/purpledb/write_batch.h
  core/embed/api_adapter.h
  core/embed/embedder.h
  core/embed/local_embedder.h
  core/pipeline/frame_decoder.h
  core/pipeline/ingest_job.h
  core/pipeline/moment_detector.h
  core/query/moment_metadata.h
  core/query/query_engine.h
  core/storage/block.h
  core/storage/block_builder.h
  core/storage/cache.h
  core/storage/compaction.h
  core/storage/crc32c.h
  core/storage/db_impl.h
  core/storage/db_iter.h
  core/storage/env.h
  core/storage/filename.h
  core/storage/filter_block.h
  core/storage/format.h
  core/storage/memtable.h
  core/storage/merger.h
  core/storage/skiplist.h
  core/storage/snapshot.h
  core/storage/table.h
  core/storage/table_builder.h
  core/storage/version_edit.h
  core/storage/version_set.h
  core/storage/wal_format.h
  core/storage/wal_reader.h
  core/storage/wal_writer.h
  core/util/arena.h
  core/util/coding.h
  core/util/hash.h
  core/util/logging.h
  core/util/sha256.h
  core/util/test_harness.h
  core/vector/delete_bitmap.h
  core/vector/distance.h
  core/vector/hnsw.h
  core/vector/vector_index.h
  core/vector/vector_segment.h
)

add_library(purpledb_core STATIC ${PURPLEDB_CORE_SOURCES} ${PURPLEDB_CORE_HEADERS})

target_include_directories(purpledb_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
)

# Tests
enable_testing()

set(PURPLEDB_TEST_SOURCES
  tests/test_main.cc
  tests/slice_test.cc
  tests/status_test.cc
  tests/crc32c_test.cc
  tests/coding_test.cc
  tests/logging_test.cc
  tests/arena_test.cc
  tests/skiplist_test.cc
  tests/block_test.cc
  tests/merger_test.cc
  tests/table_test.cc
  tests/cache_test.cc
  tests/compaction_test.cc
  tests/filter_test.cc
  tests/version_set_test.cc
  tests/env_test.cc
  tests/wal_test.cc
  tests/write_batch_test.cc
  tests/memtable_test.cc
  tests/db_test.cc
  tests/m2_acceptance_test.cc
  tests/m3_acceptance_test.cc
  tests/m4_acceptance_test.cc
  tests/distance_test.cc
  tests/delete_bitmap_test.cc
  tests/vector_segment_test.cc
  tests/vector_index_test.cc
  tests/m5_acceptance_test.cc
  tests/hnsw_test.cc
  tests/m6_acceptance_test.cc
  tests/embedder_test.cc
  tests/sha256_test.cc
  tests/moment_detector_test.cc
  tests/frame_decoder_test.cc
  tests/ingest_job_test.cc
  tests/m7_acceptance_test.cc
  tests/query_engine_test.cc
  tests/c_abi_test.cc
  tests/m8_acceptance_test.cc
  tests/m9_acceptance_test.cc
)

add_executable(purpledb_tests ${PURPLEDB_TEST_SOURCES})
target_link_libraries(purpledb_tests PRIVATE purpledb_core Threads::Threads)
target_include_directories(purpledb_tests PRIVATE
  include
  core/include
  core
  .
)

add_test(NAME purpledb_tests COMMAND purpledb_tests)

# Benchmarks
add_executable(db_bench bench/db_bench.cc)
target_link_libraries(db_bench PRIVATE purpledb_core Threads::Threads)
target_include_directories(db_bench PRIVATE
  core/include
  core
)

add_executable(vec_bench bench/vec_bench.cc)
target_link_libraries(vec_bench PRIVATE purpledb_core Threads::Threads)
target_include_directories(vec_bench PRIVATE
  core/include
  core
)

# Python Bindings (Milestone M9)
option(BUILD_PYTHON "Build Python bindings via pybind11" OFF)
if(BUILD_PYTHON)
  find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
    OUTPUT_VARIABLE pybind11_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  list(APPEND CMAKE_PREFIX_PATH "${pybind11_DIR}")
  find_package(pybind11 CONFIG QUIET)
  if(NOT pybind11_FOUND)
    find_package(pybind11 REQUIRED)
  endif()

  pybind11_add_module(_purpledb bindings/python/purpledb_py.cc)
  target_link_libraries(_purpledb PRIVATE purpledb_core Threads::Threads)
  target_include_directories(_purpledb PRIVATE include core/include core .)
  if(WIN32 AND MINGW)
    target_link_options(_purpledb PRIVATE -static-libgcc -static-libstdc++)
  endif()
endif()

