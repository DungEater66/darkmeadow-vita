cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(so_loader C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "The Dark Meadow")
set(VITA_TITLEID "DMPT00001")
set(VITA_VPKNAME "DarkMeadow")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.0.198" CACHE STRING "PSVita IP (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions
set(DATA_PATH "ux0:data/darkmeadow/") # Path to data (with trailing /)
set(SO_PATH "${DATA_PATH}libUnrealEngine3.so") # Path to .so
add_definitions(-DDATA_PATH="${DATA_PATH}" -DSO_PATH="${SO_PATH}")

# Build types:
#   Debug   - full logging (debug lines too), -O1, vitaGL with its checks and error logging (TEST=1 LOG_ERRORS=1)
#   Profile - optimised like Release (-O3, vitaGL NO_DEBUG) but keeps the log (info/warning/[cpu]/[gpu]/[mem] lines);
#             debug lines and the per-draw diagnostics are compiled out (DM_PROFILE)
#   Release - optimised, no log except errors
if (CMAKE_BUILD_TYPE STREQUAL "Profile")
  add_definitions(-DDEBUG_SOLOADER -DDM_PROFILE)
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Uncomment this for verbose FalsoJNI logging:
  #add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
  # Uncomment this for OpenGL logging:
  #add_definitions(-DDEBUG_OPENGL)
  # Uncomment this for so_util logging:
  #add_definitions(-DSO_UTIL_VERBOSE=1)
  # Space-separated flags passed to vitaGL
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 TEST=1 LOG_ERRORS=1 HAVE_SHADER_CACHE=1")
else()
  # Space-separated flags passed to vitaGL
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1")
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu11 -Wno-deprecated")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu++20 -Wno-write-strings -Wno-psabi")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0 -ffast-math")
endif()

set(SOURCES
    source/android/dynlib.c
    source/android/egl.c
    source/android/engine.c
    source/android/engine_natives.c
    source/android/java_impl.c
    source/android/java_tables.c
    source/android/patch.c
    source/audio/opensl.c
    source/audio/song.c
    source/config.c
    source/gfx/gl_fixes.c
    source/gfx/glutil.c
    source/hooks/engine_log.c
    source/hooks/game_state.c
    source/hooks/hook.c
    source/hooks/load_profiler.c
    source/hooks/text_scale.c
    source/input/overlay.c
    source/input/padinput.c
    source/input/touch.c
    source/libc/bits/_ctype.c
    source/libc/errno.c
    source/libc/io.c
    source/libc/log.c
    source/libc/mem.c
    source/libc/pthr.c
    source/libc/sys.c
    source/libc/time64.c
    source/libc/ue3_compat.c
    source/libc/ue3_stdio.c
    source/main.c
    source/storage/io_stats.c
    source/storage/mem_valve.c
    source/storage/obb_stream.c
    source/storage/pkg_cache.c
    source/storage/pkg_prefetch.c
    source/storage/save_behind.c
    source/storage/tex_cache.c
    source/storage/tex_prefetch.c
    source/util/crash.c
    source/util/fatal.c
    source/util/init.c
    source/util/logger.c
    source/util/utils.c
    source/util/watchdog.c
    source/video/movie.c
    lib/falso_jni/FalsoJNI.c
    lib/falso_jni/FalsoJNI_ImplBridge.c
    lib/falso_jni/FalsoJNI_Logger.c
    lib/falso_jni/converter.c
    lib/so_util/so_util.c
    )

add_executable(${CMAKE_PROJECT_NAME} ${SOURCES})

# VitaGL built from lib/vitagl via its own Makefile.
# Made this way so that testing quick adjustments to VitaGL is possible,
# as well as changing build flags in a fast and efficient way.
separate_arguments(VITAGL_MAKE_ARGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS}")

set(VITAGL_DIR ${CMAKE_SOURCE_DIR}/lib/vitagl)
set(VITAGL_LIB ${VITAGL_DIR}/libvitaGL.a)

# Force a full rebuild of vitaGL whenever VITAGL_MAKE_FLAGS change
set(VITAGL_FLAGS_STAMP ${CMAKE_BINARY_DIR}/vitagl_flags.stamp)
set(vitagl_old_flags "")
if (EXISTS ${VITAGL_FLAGS_STAMP})
  file(READ ${VITAGL_FLAGS_STAMP} vitagl_old_flags)
endif()
if (NOT vitagl_old_flags STREQUAL VITAGL_MAKE_FLAGS)
  file(GLOB_RECURSE vitagl_stale_objs ${VITAGL_DIR}/source/*.o)
  file(REMOVE ${vitagl_stale_objs} ${VITAGL_LIB})
  file(WRITE ${VITAGL_FLAGS_STAMP} "${VITAGL_MAKE_FLAGS}")
endif()

include(ProcessorCount)
ProcessorCount(VITAGL_NPROC)
if (VITAGL_NPROC EQUAL 0)
  set(VITAGL_NPROC 4)
endif()

add_custom_target(vitaGL_build
                  COMMAND make -C ${VITAGL_DIR} -j${VITAGL_NPROC} ${VITAGL_MAKE_ARGS}
                  BYPRODUCTS ${VITAGL_LIB}
                  COMMENT "Building VitaGL"
                  VERBATIM)

add_library(vitaGL STATIC IMPORTED)
set_target_properties(vitaGL PROPERTIES
                      IMPORTED_LOCATION ${VITAGL_LIB}
                      INTERFACE_INCLUDE_DIRECTORIES ${VITAGL_DIR}/source)
add_dependencies(${CMAKE_PROJECT_NAME} vitaGL_build)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      mpg123
                      stdc++
                      vitaGL
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      SceHid_stub
                      SceTouch_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceVshBridge_stub
                      SceAvPlayer_stub
                      SceSysmodule_stub
                      )

# Configurator: the companion settings app, started from the "Settings" button on the LiveArea page
# (source/util/init.c runs app0:/configurator.bin when the game is launched with "-config").
add_executable(configurator configurator/configurator.c)
add_dependencies(configurator vitaGL_build)
target_include_directories(configurator PRIVATE ${CMAKE_SOURCE_DIR}/configurator)
target_link_libraries(configurator
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      vitaGL
                      vitashark
                      SceShaccCgExt
                      stdc++
                      taihen_stub
                      SceShaccCg_stub
                      mathneon
                      m
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceCommonDialog_stub
                      SceCtrl_stub
                      SceDisplay_stub
                      SceGxm_stub
                      SceKernelDmacMgr_stub
                      SceSysmodule_stub
                      SceIofilemgr_stub
                      )
vita_create_self(configurator.bin configurator UNSAFE)

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME "${VITA_APP_NAME}"
                FILE ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/extras/cpu_present cpu_present
                ${CMAKE_SOURCE_DIR}/extras/urandom urandom
                ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
                ${CMAKE_SOURCE_DIR}/extras/livearea/config.png sce_sys/livearea/contents/config.png
                ${CMAKE_BINARY_DIR}/configurator.bin configurator.bin
                )
add_dependencies(${VITA_VPKNAME}.vpk-vpk configurator.bin-self)

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(dump
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.sh ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
