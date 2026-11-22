# The Android build of the GT Racing host: libmain.so (SDL's activity loads it and calls SDL_main) and SDL 3.
#
# Sources, as host/build.py has them for Windows:
#   build/gen_android/  the recompiled game (android/prepare.py: build/gen with --wrap made GNU ld's)
#   build/host/gen/     bridges.c, the import bridges (recomp/bridgegen.py)
#   runtime/            rt_jump.c, rt_selftest.c
#   host/core, host/shims, host/shims/gl   the lower layer: its own folders and games/gtracing (layout.h) only
#   host/games/gtracing/...                the game layer: every folder
#   android/SDL/        SDL 3 (android/prepare.py clones it)
# armeabi-v7a only: the host is a 32-bit program (guest and host pointers are the same words; plat.h).
cmake_minimum_required(VERSION 3.22)
project(gtracing C)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(GEN "${REPO}/build/gen_android")
set(HOST_GEN "${REPO}/build/host/gen")
set(HOST "${REPO}/host")

if(NOT ANDROID_ABI STREQUAL "armeabi-v7a")
  message(FATAL_ERROR "The GT Racing host is a 32-bit program: build armeabi-v7a only (not ${ANDROID_ABI}).")
endif()
if(NOT EXISTS "${GEN}/dispatch.c" OR NOT EXISTS "${HOST_GEN}/bridges.c")
  message(FATAL_ERROR "No generated code: run  python -I android/prepare.py  first (it needs your game).")
endif()
if(NOT EXISTS "${REPO}/android/SDL/CMakeLists.txt")
  message(FATAL_ERROR "No SDL 3 in android/SDL: run  python -I android/prepare.py  first.")
endif()

# SDL 3, as a shared library (libSDL3.so, which SDLActivity loads first)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/android/SDL" SDL)

option(GT_DEV "the development build: the diagnostic keys and traces (host/build.py without --release)" OFF)
if(GT_DEV)
  set(GT_DEV_FLAG -DGT_DEV=1)
else()
  set(GT_DEV_FLAG -DGT_DEV=0)
endif()

# every compile: no build-machine path in the library (as on Windows, tools/privacy_scan.py), the guest's integer
# and aliasing rules, hidden symbols (direct calls between the 23,873 functions, no PLT); SDL_main stays visible
set(COMMON -O2 -fno-strict-aliasing -fwrapv -fvisibility=hidden -DARM_LOW_DELTA=0x8bf000u
           "-ffile-prefix-map=${REPO}=.")

# the generated code and runtime/ (compile_all.py's flags)
file(GLOB GEN_SRC "${GEN}/*.c")
list(FILTER GEN_SRC EXCLUDE REGEX "/(imports_stub|imports)\\.c$")
add_library(gt_gen OBJECT ${GEN_SRC} "${REPO}/runtime/rt_jump.c" "${REPO}/runtime/rt_selftest.c")
target_include_directories(gt_gen PRIVATE "${REPO}/runtime" "${GEN}")
target_compile_options(gt_gen PRIVATE ${COMMON} -ffp-contract=off -Wall -Wno-unused-label -Wno-unused-variable
                       -Wno-unused-but-set-variable -Wno-self-assign -Wno-unused-function)

# the lower layer and the bridges
set(LOWER_DIRS "${HOST}/core" "${HOST}/shims" "${HOST}/shims/gl" "${HOST}/games/gtracing")
file(GLOB LOWER_SRC "${HOST}/core/*.c" "${HOST}/shims/*.c" "${HOST}/shims/gl/*.c")
add_library(gt_lower OBJECT ${LOWER_SRC} "${HOST_GEN}/bridges.c")
target_include_directories(gt_lower PRIVATE ${LOWER_DIRS} "${REPO}/runtime" "${HOST_GEN}")
target_compile_options(gt_lower PRIVATE ${COMMON} -std=gnu11 -Wall -Wextra ${GT_DEV_FLAG})
target_link_libraries(gt_lower PRIVATE SDL3::Headers)

# the game layer
file(GLOB GAME_SRC "${HOST}/games/gtracing/*.c" "${HOST}/games/gtracing/features/*.c"
                   "${HOST}/games/gtracing/render/*.c")
add_library(gt_game OBJECT ${GAME_SRC})
target_include_directories(gt_game PRIVATE ${LOWER_DIRS} "${HOST}/games/gtracing/features"
                           "${HOST}/games/gtracing/render" "${REPO}/runtime" "${HOST_GEN}")
target_compile_options(gt_game PRIVATE ${COMMON} -std=gnu11 -Wall -Wextra ${GT_DEV_FLAG})
target_link_libraries(gt_game PRIVATE SDL3::Headers)

add_library(main SHARED $<TARGET_OBJECTS:gt_gen> $<TARGET_OBJECTS:gt_lower> $<TARGET_OBJECTS:gt_game>)
target_link_libraries(main PRIVATE SDL3::SDL3-shared GLESv3 EGL android log m dl)

# the link-time hooks: host/build.py's --wrap list, read from it so the two builds never differ
file(READ "${HOST}/build.py" BUILD_PY)
string(REGEX MATCHALL "--wrap=[A-Za-z0-9_]+" WRAPS "${BUILD_PY}")
list(REMOVE_DUPLICATES WRAPS)
list(TRANSFORM WRAPS PREPEND "-Wl,")
list(LENGTH WRAPS NWRAPS)
message(STATUS "GT Racing: ${NWRAPS} --wrap hooks from host/build.py")
target_link_options(main PRIVATE ${WRAPS} -Wl,--no-undefined "-Wl,-Map=${CMAKE_BINARY_DIR}/libmain.map")
