cmake_minimum_required(VERSION 3.20)

project(cagent C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---------------------------------------------------------------------------
# Sanitizers (AddressSanitizer + UndefinedBehaviorSanitizer)
#   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCAGENT_SANITIZERS=ON
# ---------------------------------------------------------------------------
option(CAGENT_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(CAGENT_SANITIZERS)
  add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address -fsanitize=undefined)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
add_compile_options(
  -Wall
  -Wextra
  -Wpedantic
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# Use the wide-character ncurses backend for the real TUI. The screen model
# and layout renderer remain independent, so tests do not require a terminal.
set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)

# yyjson ships a proper CMake config in nixpkgs (the pkg-config file in
# nixpkgs has a broken libdir/includedir — duplicated prefix).
find_package(yyjson CONFIG REQUIRED)

get_target_property(YYJSON_INCLUDE_DIRS yyjson::yyjson INTERFACE_INCLUDE_DIRECTORIES)
if(NOT YYJSON_INCLUDE_DIRS)
  message(FATAL_ERROR "yyjson target does not expose include directories")
endif()

# Nix devShell quirk: the clang wrapper injects NIX_CFLAGS_COMPILE into the
# compiler, so CMake records curl/yyjson include dirs as *implicit* and
# silently drops matching target_include_directories entries from compile
# commands (breaking compile_commands.json for clangd/clang-tidy). Injecting
# the same CMake-discovered dirs as -isystem compile options bypasses the
# dedup. Harmless on non-Nix systems: a missing -isystem dir is ignored.
set(CAGENT_DEP_INCLUDE_DIRS ${CURL_INCLUDE_DIRS} ${CURSES_INCLUDE_DIRS} ${YYJSON_INCLUDE_DIRS})

# ---------------------------------------------------------------------------
# Core library: every src module goes here; main stays in the executable
# ---------------------------------------------------------------------------
add_library(cagent_core STATIC
  src/agent/agent.c
  src/auth/oauth.c
  src/agent/context.c
  src/agent/loop.c
  src/agent/message.c
  src/model/anthropic.c
  src/model/model.c
  src/model/openai.c
  src/model/provider.c
  src/model/responses.c
  src/model/stream.c
  src/runtime/event_loop.c
  src/runtime/http.c
  src/runtime/process.c
  src/runtime/runtime.c
  src/runtime/scheduler.c
  src/session/session.c
  src/report/report.c
  src/tui/format.c
  src/tui/render.c
  src/tui/terminal.c
  src/tui/tui.c
  src/tool/bash.c
  src/tool/bench.c
  src/tool/edit.c
  src/tool/diagnose.c
  src/tool/find.c
  src/tool/git.c
  src/tool/grep.c
  src/tool/list.c
  src/tool/memory.c
  src/tool/plan.c
  src/tool/path_policy.c
  src/tool/builtin.c
  src/tool/read.c
  src/tool/registry.c
  src/tool/subagent.c
  src/tool/test.c
  src/tool/write.c
  src/util/arena.c
  src/util/buffer.c
  src/util/diff.c
  src/util/error.c
  src/util/hashmap.c
  src/util/json.c
  src/util/log.c
  src/util/project_context.c
  src/util/string.c
  src/util/vector.c
)

target_include_directories(cagent_core PUBLIC src)

# The project is Linux/POSIX-only by design (DESIGN.md): _GNU_SOURCE
# covers Linux APIs (pipe2, epoll, eventfd...) plus POSIX 2008 under
# strict -std=c17.
target_compile_definitions(cagent_core PUBLIC _GNU_SOURCE)

# -isystem injection, see note above (after add_library: options need the
# target to exist).
foreach(dir IN LISTS CAGENT_DEP_INCLUDE_DIRS)
  target_compile_options(cagent_core PUBLIC "-isystem${dir}")
endforeach()

target_link_libraries(cagent_core
  PUBLIC
    CURL::libcurl
    OpenSSL::Crypto
    ${CURSES_LIBRARIES}
    yyjson::yyjson
)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(cagent src/main.c)

target_include_directories(cagent PRIVATE src)

target_link_libraries(cagent
  PRIVATE
    cagent_core
)

install(TARGETS cagent RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Optional micro-benchmarks (Phase 7; not built by default)
#   cmake -B build -DCAGENT_BUILD_BENCH=ON
# ---------------------------------------------------------------------------
option(CAGENT_BUILD_BENCH "Build the micro-benchmark suite" OFF)

if(CAGENT_BUILD_BENCH)
  add_executable(cagent_bench bench/bench.c)
  target_include_directories(cagent_bench PRIVATE src)
  target_link_libraries(cagent_bench PRIVATE cagent_core)
endif()

# ---------------------------------------------------------------------------
# Tests (BUILD_TESTING comes from include(CTest); nixpkgs passes -DBUILD_TESTING=OFF)
# ---------------------------------------------------------------------------
include(CTest)

if(BUILD_TESTING)
  set(CAGENT_TESTS
    agent_loop
    anthropic
    arena
    buffer
    context
    edit
    event_loop
    grep
    hashmap
    http
    json
    message
    models
    openai
    oauth
    responses
    process
    scheduler
    session
    smoke
    sse
    string
    subagent
    tool_registry
    tools
    tui
    tui_format
    vector
  )

  # mock model + test server: shared by tests; not part of the library
  add_library(mock_model STATIC tests/mock_model.c)
  target_include_directories(mock_model PUBLIC src tests)
  target_compile_definitions(mock_model PRIVATE _GNU_SOURCE)

  add_library(test_server STATIC tests/test_server.c)
  target_include_directories(test_server PUBLIC src tests)
  target_compile_definitions(test_server PRIVATE _GNU_SOURCE)

  foreach(test_name IN LISTS CAGENT_TESTS)
    add_executable(test_${test_name} tests/test_${test_name}.c)
    target_include_directories(test_${test_name} PRIVATE src tests)
    target_link_libraries(test_${test_name} PRIVATE cagent_core)
    if(test_name STREQUAL "agent_loop" OR test_name STREQUAL "scheduler"
       OR test_name STREQUAL "subagent")
      target_link_libraries(test_${test_name} PRIVATE mock_model)
    endif()
    if(test_name STREQUAL "http" OR test_name STREQUAL "openai"
       OR test_name STREQUAL "anthropic" OR test_name STREQUAL "responses"
       OR test_name STREQUAL "models")
      target_link_libraries(test_${test_name} PRIVATE test_server)
    endif()
    add_test(NAME ${test_name} COMMAND test_${test_name})
  endforeach()
endif()
