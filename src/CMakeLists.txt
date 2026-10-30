# PlayShell: games on the PS5 home screen. Builds inside the OnionHEN beta2 tree as source/playshell
# (add_subdirectory(playshell)), using OnionHEN's libraries, the same way LiveShell does. GPL-3.0.
#   playshell_shellui.elf - runs inside SceShellUI (embedded in playshell.elf)
#   playshell.elf         - the payload the user loads
cmake_minimum_required (VERSION 3.20)
project(playshell C CXX ASM)

onion_module_prologue(C_STANDARD 11 CXX_STANDARD 20)
set(D_CWD "${CMAKE_CURRENT_SOURCE_DIR}")
onion_output_dirs()

set(CMAKE_C_FLAGS "${ONION_PS5_TARGET_FLAGS} -fPIC -fPIE -march=znver2 -Wall -fstack-protector-all -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")

# Nintendo 3DS: off (13.6 fps drawn on the CPU, 2026-10-30) - ON once the payload can use the GPU
option(PLAYSHELL_3DS "Nintendo 3DS (Azahar)" OFF)
if (PLAYSHELL_3DS)
	set(PLAYSHELL_3DS_LIB "${AZAHAR_BUILD}/libazahar.a")
	set(PLAYSHELL_3DS_DEF 1)
else()
	set(PLAYSHELL_3DS_LIB "")
	set(PLAYSHELL_3DS_DEF 0)
endif()
add_executable(playshell_shellui ${D_CWD}/playshell_shellui.cpp ${D_CWD}/miniz.c)
set_target_properties(playshell_shellui PROPERTIES OUTPUT_NAME "playshell_shellui.elf")
target_link_options(playshell_shellui PRIVATE "-Wl,--gc-sections")
target_compile_definitions(playshell_shellui PRIVATE PLAYSHELL_3DS=${PLAYSHELL_3DS_DEF})
add_dependencies(playshell_shellui onion_proc onion_platform onion_detour)
target_include_directories(playshell_shellui PRIVATE
	"${PROJECT_ROOT}/libonion_proc/include"
	"${PROJECT_ROOT}/libonion_platform/include"
	"${PROJECT_ROOT}/libonion_detour/include"
	"${PROJECT_ROOT}/include")
target_link_directories(playshell_shellui PUBLIC "${ONIONHEN_PS5_STUB_DIR}")
target_link_libraries(playshell_shellui PUBLIC onion_payload onion_proc onion_platform onion_detour SceNotification kernel_sys)
onion_strip_debug(playshell_shellui)

# Mesen 2 (GPL-3.0): its core built by tools/mesen_lib.sh into MESEN_BUILD/libmesen.a (sources in MESEN_BUILD/src)
set(MESEN_BUILD "$ENV{HOME}/ps5/mesen-build" CACHE PATH "Mesen 2 core build (tools/mesen_lib.sh)")
# PCSX-ReARMed (GPL-2.0+): its libretro core built by tools/pcsx_lib.sh into PCSX_BUILD/libpcsx.a
set(PCSX_BUILD "$ENV{HOME}/ps5/pcsx-build" CACHE PATH "PCSX-ReARMed build (tools/pcsx_lib.sh)")
# zlib for the cores: built by tools/ppsspp_lib.sh from PPSSPP's copy into PPSSPP_BUILD/libz.a (PSP itself: removed)
set(PPSSPP_BUILD "$ENV{HOME}/ps5/ppsspp-build" CACHE PATH "PPSSPP build (tools/ppsspp_lib.sh)")
# melonDS, DOSBox Pure, Mupen64Plus-Next: tools/core_lib.sh -> CORES_BUILD/<name>/lib<name>.a; tools/glstub.sh ->
# CORES_BUILD/libglstub.a (one set of empty OpenGL stand-ins for the cores)
set(CORES_BUILD "$ENV{HOME}/ps5/cores" CACHE PATH "libretro cores (tools/core_lib.sh)")
set(AZAHAR_BUILD "$ENV{HOME}/ps5/azahar-build" CACHE PATH "Azahar, Nintendo 3DS (tools/azahar_lib.sh)")
add_executable(playshell ${D_CWD}/playshell_daemon.cpp ${D_CWD}/playshell_emu.c ${D_CWD}/minigb_apu.c ${D_CWD}/playshell_mesen.cpp ${D_CWD}/playshell_lr.c)
set_source_files_properties(${D_CWD}/playshell_lr.c PROPERTIES INCLUDE_DIRECTORIES "${PCSX_BUILD}/src/deps/libretro-common/include")
set_source_files_properties(${D_CWD}/playshell_mesen.cpp PROPERTIES COMPILE_OPTIONS "-std=c++17;-w"
	INCLUDE_DIRECTORIES "${MESEN_BUILD}/src;${MESEN_BUILD}/src/Core;${MESEN_BUILD}/src/Utilities")
set_target_properties(playshell PROPERTIES OUTPUT_NAME "playshell.elf")
target_link_options(playshell PRIVATE "-Wl,--gc-sections" "-Wl,-Map=${CMAKE_BINARY_DIR}/playshell.map") # (crash offsets -> functions)
add_dependencies(playshell playshell_shellui NineS onion_proc onion_platform onion_elfldr)
set_source_files_properties(${D_CWD}/playshell_daemon.cpp PROPERTIES OBJECT_DEPENDS "${ONIONHEN_OUT_BIN}/playshell_shellui.elf")
target_compile_definitions(playshell PRIVATE PLAYSHELL_3DS=${PLAYSHELL_3DS_DEF}
	AUDIO_SAMPLE_RATE=48000 MINIGB_APU_AUDIO_FORMAT_S16SYS
	PLAYSHELL_SHELLUI_ELF="${ONIONHEN_OUT_BIN}/playshell_shellui.elf")
target_include_directories(playshell PRIVATE
	"${PROJECT_ROOT}/libonion_proc/include"
	"${PROJECT_ROOT}/libonion_platform/include"
	"${PROJECT_ROOT}/include")
target_link_directories(playshell PUBLIC "${ONIONHEN_PS5_STUB_DIR}")
target_link_libraries(playshell PUBLIC NineS onion_proc onion_platform onion_elfldr "${MESEN_BUILD}/libmesen.a" "${PCSX_BUILD}/libpcsx.a" "${CORES_BUILD}/melonds/libmelonds.a" "${CORES_BUILD}/dosbox/libdosbox.a" "${CORES_BUILD}/mupen64/libmupen64.a" ${PLAYSHELL_3DS_LIB}
	"${PPSSPP_BUILD}/libz.a" "${CORES_BUILD}/libglstub.a" SceNotification kernel_sys)
onion_strip_debug(playshell)
