# The Android build of the GT Racing host: libmain.so (SDL's activity loads it and calls SDL_main) and SDL 3.
#
# Sources, as host/build.py has them for Windows:
#   build/gen_android/  the recompiled game (android/prepare.py: build/gen with --wrap made GNU ld's)
#   build/host/gen/     bridges.c, the import bridges (recomp/bridgegen.py)
#   runtime/            rt_jump.c, rt_selftest.c
#   host/core, host/shims, host/shims/gl   the lower layer: its own folders and games/gtracing (layout.h) only
#   host/games/gtracing/...                the game layer: every folder
#   android/SDL/        SDL 3 (android/prepare.py clones it)
# Two ABIs, one APK:
#   armeabi-v7a  libmain.so: the game itself (guest and host pointers are the same 32-bit words), libSDL3.so;
#   arm64-v8a    libgame.so: the game, SDL linked in, its memory kept below 4 GB (host/core/lowmem.c: the heap
#                and the stacks; --wrap below); libmain.so: stub.c, which loads libgame.so below 4 GB.
#   The Snapdragon 8 Gen 3 and later run 64-bit code only.
cmake_minimum_required(VERSION 3.22)
project(gtracing C)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(GEN "${REPO}/build/gen_android")
set(HOST_GEN "${REPO}/build/host/gen")
set(HOST "${REPO}/host")

if(ANDROID_ABI STREQUAL "arm64-v8a")
  set(GT64 ON)
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
  set(GT64 OFF)
else()
  message(FATAL_ERROR "GT Racing builds armeabi-v7a and arm64-v8a (not ${ANDROID_ABI}).")
endif()
if(NOT EXISTS "${GEN}/dispatch.c" OR NOT EXISTS "${HOST_GEN}/bridges.c")
  message(FATAL_ERROR "No generated code: run  python -I android/prepare.py  first (it needs your game).")
endif()
if(NOT EXISTS "${REPO}/android/SDL/CMakeLists.txt")
  message(FATAL_ERROR "No SDL 3 in android/SDL: run  python -I android/prepare.py  first.")
endif()

# SDL 3: libSDL3.so, which SDLActivity loads first (32-bit); and the static library (64-bit: linked into libgame.so,
# so its memory and its threads' stacks are lowmem.c's too).  Both on both ABIs: the APK's targets are the same.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
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
# ARMv8 in its 32-bit mode (every phone that runs Android 10 and 32-bit apps), tuned for the Cortex-A76/A77
# class (Snapdragon 855-888 and 870, Dimensity 1000-1200); ARM code, not Thumb (the NDK's default): the
# recompiled game's long straight-line functions run faster as 32-bit instructions on those cores
# arm64: ARMv8.0 (every 64-bit phone), the same tuning
if(GT64)
  set(GT_ARCH -march=armv8-a -mtune=cortex-a76)
else()
  set(GT_ARCH -march=armv8-a -mfpu=neon-fp-armv8 -mtune=cortex-a76 -marm)
endif()
set(COMMON -O2 -fno-strict-aliasing -fwrapv -fvisibility=hidden -DARM_LOW_DELTA=0x8bf000u ${GT_ARCH}
           -fomit-frame-pointer "-ffile-prefix-map=${REPO}=.")

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

if(GT64)
  set(GAME game)
else()
  set(GAME main)
endif()
add_library(${GAME} SHARED $<TARGET_OBJECTS:gt_gen> $<TARGET_OBJECTS:gt_lower> $<TARGET_OBJECTS:gt_game>)
if(GT64)
  target_link_libraries(game PRIVATE SDL3::SDL3-static GLESv3 EGL android log m dl)
  # lowmem.c: the heap and the threads' stacks of everything in libgame.so below 4 GB (malloc, calloc, realloc
  # and free are build.py's wraps already: liveblocks.c, which calls lowmem.c's allocator on this ABI)
  set(LOWMEM_WRAPS memalign posix_memalign aligned_alloc malloc_usable_size strdup strndup pthread_create)
  list(TRANSFORM LOWMEM_WRAPS PREPEND "-Wl,--wrap=")
  target_link_options(game PRIVATE ${LOWMEM_WRAPS})
  # libmain.so: the stub that loads libgame.so below 4 GB (SDLActivity loads "main")
  add_library(main SHARED stub.c)
  target_compile_options(main PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(main PRIVATE android log dl)
else()
  target_link_libraries(main PRIVATE SDL3::SDL3-shared GLESv3 EGL android log m dl)
  # the APK's targets are the same on both ABIs: an empty libgame.so here (Java does not load it on 32-bit)
  file(WRITE "${CMAKE_BINARY_DIR}/game_empty.c" "/* empty: armeabi-v7a's game is libmain.so */\nvoid gt_game_empty(void) {}\n")
  add_library(game SHARED "${CMAKE_BINARY_DIR}/game_empty.c")
endif()

# the link-time hooks: host/build.py's --wrap list, read from it so the two builds never differ
file(READ "${HOST}/build.py" BUILD_PY)
string(REGEX MATCHALL "--wrap=[A-Za-z0-9_]+" WRAPS "${BUILD_PY}")
list(REMOVE_DUPLICATES WRAPS)
list(TRANSFORM WRAPS PREPEND "-Wl,")
list(LENGTH WRAPS NWRAPS)
message(STATUS "GT Racing: ${NWRAPS} --wrap hooks from host/build.py")
target_link_options(${GAME} PRIVATE ${WRAPS} -Wl,--no-undefined "-Wl,-Map=${CMAKE_BINARY_DIR}/lib${GAME}.map")
