cmake_minimum_required(VERSION 3.20)
# The version is here and in vcpkg.json, and cmake/spots.cmake checks the two
# against each other: vcpkg reads its manifest before CMake runs, so nothing
# else would catch them drifting. "Which build is this?" is the first question
# asked in an incident and a quietly wrong answer is worse than none.
# C as well as C++, from the start: bitshuffle and LZ4 are C, and CMake
# before 3.24 or so finds HDF5 by compiling a C test, which fails -- "Unknown
# extension .c ... try_compile() works only for enabled languages" -- where C
# is enabled only later (cmake/spots.cmake), as on Ubuntu 22.04's CMake 3.22.
project(mxi VERSION 1.3.2 LANGUAGES C CXX)

# Installed programs keep the paths to the libraries they were linked against
# outside the system's own -- HDF5 from conda or Homebrew -- or they would not
# find them; and the install has nothing to rewrite.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set(CMAKE_CXX_STANDARD 20)

# Code generation, before any target is defined: add_compile_options reaches
# only the targets defined after it, and this had stood after the library's,
# which so had none of it -- only the spot finder's targets had SSE4.1. Found
# 13 November on a user's L40S workstation, a 2.9 GHz Xeon: indexing 8 times
# and integrating 3.5 times slower than on an M4 Max, from the same release.
#
# SSE4.1 on x86, for the rounding instruction. Baseline x86-64 predates it, so
# std::rint compiles to a sequence rather than roundsd and the scoring loop in
# indexing runs three times slower: 3.5 nanoseconds a value against 1.2, and
# there are three values per reflection per triple of candidates scored.
# Anything from 2009 onwards has it; -DMXI_SSE41=OFF for a machine that does
# not. ARM needs nothing: the rounding instruction has always been there,
# which is why the same scoring loop was twenty-two times faster on an Apple
# machine than in this container, and why that difference first looked like
# the data.
option(MXI_SSE41 "Use SSE4.1 on x86, for the rounding instruction" ON)
if(MXI_SSE41 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[36]86")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-msse4.1 MXI_COMPILER_TAKES_SSE41)
    if(MXI_COMPILER_TAKES_SSE41)
        add_compile_options(-msse4.1)
        message(STATUS "x86: SSE4.1 enabled for the rounding instruction")
    endif()
endif()
# And the C library's maths as Clang treats it by default, Apple's on a Mac
# among them: no errno set by sqrt or lround, which mxi never reads, and no
# floating-point exception flags, which it never tests. No result changes;
# GCC then inlines sqrt, round and lround where it had called them.
if(NOT MSVC)
    add_compile_options(-fno-math-errno -fno-trapping-math)
endif()

# Warnings, and what Windows wants, by compiler (docs/windows.md). clang-cl
# and MSVC's cl take Microsoft's flags -- clang-cl reads -Wall as /Wall,
# every warning clang has -- so /W4 there, clang's -Wall -Wextra in effect;
# GCC and clang as before. MXI_WARNINGS on every program, the pedantic set on
# the library.
if(MSVC)
    set(MXI_WARNINGS /W4)
    set(MXI_WARNINGS_PEDANTIC /W4)
    # The C runtime linked in, as vcpkg's x64-windows-static is: one mxi.exe,
    # nothing to install beside it.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # windows.h's min and max macros would break std::min; fopen, getenv and
    # the rest are not "unsafe"; UTF-8 source; mxi_integrate.cc is past the
    # default limit of sections in an object.
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
    add_compile_options(/utf-8 /bigobj)
else()
    set(MXI_WARNINGS -Wall -Wextra)
    set(MXI_WARNINGS_PEDANTIC -Wall -Wextra -Wpedantic)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# No third-party dependencies, deliberately. The spot finder hand-rolled its
# msgpack writer for the same reason: this has to build on a laptop and on a
# beamline machine with nothing installed on it.
# FFTW for the transform, which is the largest phase of indexing on a fast
# machine. The built-in radix-2 in src/fft.cc is the default and needs nothing;
# this is faster and is GPL, which is why it is opt-in rather than assumed.
#
#   sudo apt-get install libfftw3-dev      # Debian and Ubuntu
#   brew install fftw                      # macOS
#   cmake -S . -B build -DMXI_FFTW=ON
option(MXI_FFTW "Use FFTW for the transform instead of the built-in radix-2" OFF)
if(MXI_FFTW)
    # pkg-config first, through an imported target rather than by hand.
    # pkg_check_modules sets FFTW3_LIBRARIES to the link NAME -- "fftw3" -- and
    # keeps the directory in FFTW3_LIBRARY_DIRS, so linking the first without
    # the second works only where the library is already on the linker's
    # default path. It is on Linux, and it is not under Homebrew's
    # /opt/homebrew, which is where this failed. The imported target carries
    # both.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFTW3 QUIET IMPORTED_TARGET fftw3)
    endif()
    if(TARGET PkgConfig::FFTW3)
        set(MXI_FFTW_TARGET PkgConfig::FFTW3)
    else()
        # No pkg-config, or no .pc file. find_library returns a full path, so
        # this needs no directory of its own -- but it does need to be told
        # where Homebrew puts things, since /opt/homebrew is not searched by
        # default.
        find_library(FFTW3_LIBRARY NAMES fftw3
                     HINTS ENV FFTW_ROOT ENV HOMEBREW_PREFIX
                     PATHS /opt/homebrew /usr/local /opt/local
                     PATH_SUFFIXES lib)
        find_path(FFTW3_INCLUDE_DIR NAMES fftw3.h
                  HINTS ENV FFTW_ROOT ENV HOMEBREW_PREFIX
                  PATHS /opt/homebrew /usr/local /opt/local
                  PATH_SUFFIXES include)
        if(FFTW3_LIBRARY AND FFTW3_INCLUDE_DIR)
            add_library(mxi_fftw INTERFACE)
            target_link_libraries(mxi_fftw INTERFACE ${FFTW3_LIBRARY})
            target_include_directories(mxi_fftw INTERFACE ${FFTW3_INCLUDE_DIR})
            set(MXI_FFTW_TARGET mxi_fftw)
        endif()
    endif()
    if(NOT MXI_FFTW_TARGET)
        message(FATAL_ERROR
                "MXI_FFTW is on but FFTW was not found. Install it with "
                "'apt-get install libfftw3-dev' or 'brew install fftw', or "
                "point at it with -DFFTW3_LIBRARY=<path to libfftw3> and "
                "-DFFTW3_INCLUDE_DIR=<path to fftw3.h>. Configure without "
                "-DMXI_FFTW=ON to use the built-in transform.")
    endif()
    # FFTW's own threading, if it was built with any. Two flavours exist and
    # either will do: fftw3_threads is pthreads, fftw3_omp is OpenMP. Debian
    # ships the second, Homebrew the first. Absent both, the transform runs on
    # one thread and everything still works.
    find_library(FFTW3_THREADS_LIBRARY NAMES fftw3_threads fftw3_omp
                 HINTS ENV FFTW_ROOT ENV HOMEBREW_PREFIX
                 PATHS /opt/homebrew /usr/local /opt/local
                 PATH_SUFFIXES lib)
    if(FFTW3_THREADS_LIBRARY)
        message(STATUS "FFT: FFTW (${MXI_FFTW_TARGET}), threaded via "
                       "${FFTW3_THREADS_LIBRARY}")
    else()
        message(STATUS "FFT: FFTW (${MXI_FFTW_TARGET}), single threaded -- "
                       "no fftw3_threads or fftw3_omp found")
    endif()
else()
    message(STATUS "FFT: the built-in radix-2; -DMXI_FFTW=ON for FFTW")
endif()

# gemmi, for space groups: operators, the reciprocal asymmetric unit, absences.
# A submodule pinned at a release, as bitshuffle is. Only its symmetry source
# is compiled -- the rest used so far is headers -- and with its own warnings
# off: the no-warnings rule is about this code, and gemmi's headers come in as
# system headers so that it stays so.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmi/include/gemmi/symmetry.hpp)
    message(FATAL_ERROR "third_party/gemmi is empty: run git submodule update --init")
endif()
add_library(mxi_gemmi STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmi/src/symmetry.cpp)
target_include_directories(mxi_gemmi SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmi/include)
target_compile_options(mxi_gemmi PRIVATE -w)
set_target_properties(mxi_gemmi PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(mxi
  src/linalg.cc
  src/geometry.cc
  src/predict.cc
  src/json.cc
  src/expt.cc
  src/refl.cc
  src/rflx.cc
  src/cosym.cc
  src/views.cc
  src/processing.cc
  src/two_theta.cc
  src/correlation.cc
  src/spot_table.cc
  src/untrusted.cc
  src/verify.cc
  src/summary_cut.cc
  src/triage.cc
  src/experiment_images.cc
  src/hdf5_handle.cc
  src/combine.cc
  src/fft.cc
  $<$<BOOL:${MXI_FFTW}>:src/fft_fftw.cc>
  src/index.cc
  src/refine.cc
  src/args.cc
  src/derivatives.cc
  src/shoebox.cc
  src/profile_model.cc
  src/profile_grid.cc
  src/mask.cc
  src/forward.cc
  src/background.cc
  src/integrate.cc
  src/reference.cc
  src/summary.cc
  src/postrefine.cc
  src/symmetry.cc
  src/laue.cc
  src/scale_model.cc
  src/parallel.cc
  src/fit_batch.cc
  src/scale.cc
  src/resolution.cc
)

target_link_libraries(mxi PUBLIC mxi_gemmi)

if(MXI_FFTW)
    # PUBLIC: fft3d is called from mxi and the symbol has to resolve in every
    # executable that links mxi, not only inside the archive.
    target_compile_definitions(mxi PUBLIC MXI_USE_FFTW)
    target_link_libraries(mxi PUBLIC ${MXI_FFTW_TARGET})
    if(FFTW3_THREADS_LIBRARY)
        # Before the main library on the link line, as FFTW asks.
        target_link_libraries(mxi PUBLIC ${FFTW3_THREADS_LIBRARY})
        target_compile_definitions(mxi PRIVATE MXI_FFTW_THREADS)
        find_package(Threads REQUIRED)
        target_link_libraries(mxi PUBLIC Threads::Threads)
    endif()
endif()
# Always: src/parallel.cc runs std::threads, FFTW's threads or not.
find_package(Threads REQUIRED)
target_link_libraries(mxi PUBLIC Threads::Threads)
target_include_directories(mxi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(mxi PRIVATE ${MXI_WARNINGS_PEDANTIC})

add_library(mxi_index OBJECT apps/mxi_index.cc)
target_link_libraries(mxi_index PRIVATE mxi)
target_compile_options(mxi_index PRIVATE ${MXI_WARNINGS})

add_library(mxi_refine OBJECT apps/mxi_refine.cc)
target_link_libraries(mxi_refine PRIVATE mxi)
target_compile_options(mxi_refine PRIVATE ${MXI_WARNINGS})

add_library(mxi_scale OBJECT apps/mxi_scale.cc)
target_link_libraries(mxi_scale PRIVATE mxi)
target_compile_options(mxi_scale PRIVATE ${MXI_WARNINGS})

# gemmi's MTZ writer, for mxi_export: a library of its own, as it wants zlib
# and nothing else here does.
find_package(ZLIB REQUIRED)
add_library(mxi_gemmi_mtz STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmi/src/mtz.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmi/src/gz.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmi/src/sprintf.cpp)
target_include_directories(mxi_gemmi_mtz SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmi/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gemmi/third_party)
target_link_libraries(mxi_gemmi_mtz PUBLIC ZLIB::ZLIB mxi_gemmi)
target_compile_options(mxi_gemmi_mtz PRIVATE -w)

add_library(mxi_export OBJECT apps/mxi_export.cc)
target_link_libraries(mxi_export PRIVATE mxi mxi_gemmi_mtz)
target_compile_options(mxi_export PRIVATE ${MXI_WARNINGS})

add_library(mxi_symmetry OBJECT apps/mxi_symmetry.cc)
target_link_libraries(mxi_symmetry PRIVATE mxi)
target_compile_options(mxi_symmetry PRIVATE ${MXI_WARNINGS})
add_library(mxi_show OBJECT apps/mxi_show.cc)
target_link_libraries(mxi_show PRIVATE mxi)
target_compile_options(mxi_show PRIVATE ${MXI_WARNINGS})

add_library(mxi_correlation OBJECT apps/mxi_correlation.cc)
target_link_libraries(mxi_correlation PRIVATE mxi)
target_compile_options(mxi_correlation PRIVATE ${MXI_WARNINGS})
add_library(mxi_report OBJECT apps/mxi_report.cc)
target_link_libraries(mxi_report PRIVATE mxi)
target_compile_options(mxi_report PRIVATE ${MXI_WARNINGS})

add_library(mxi_residuals OBJECT apps/mxi_residuals.cc)
target_link_libraries(mxi_residuals PRIVATE mxi)
target_compile_options(mxi_residuals PRIVATE ${MXI_WARNINGS})

add_library(mxi_profile OBJECT apps/mxi_profile.cc)
target_link_libraries(mxi_profile PRIVATE mxi)
target_compile_options(mxi_profile PRIVATE ${MXI_WARNINGS})

# Each predicted reflection's foreground region, for dials.image_viewer (was
# mxi_mask, whose name the mask editor took, 22 October).
add_library(mxi_foreground OBJECT apps/mxi_foreground.cc)
target_link_libraries(mxi_foreground PRIVATE mxi)
target_compile_options(mxi_foreground PRIVATE ${MXI_WARNINGS})

add_library(mxi_background OBJECT apps/mxi_background.cc)
target_link_libraries(mxi_background PRIVATE mxi)
target_compile_options(mxi_background PRIVATE ${MXI_WARNINGS})

add_library(mxi_forward OBJECT apps/mxi_forward.cc)
target_link_libraries(mxi_forward PRIVATE mxi)
target_compile_options(mxi_forward PRIVATE ${MXI_WARNINGS})

add_library(mxi_convert OBJECT apps/mxi_convert.cc)
target_link_libraries(mxi_convert PRIVATE mxi)
target_compile_options(mxi_convert PRIVATE ${MXI_WARNINGS})
add_library(mxi_combine OBJECT apps/mxi_combine.cc)
target_link_libraries(mxi_combine PRIVATE mxi)
target_compile_options(mxi_combine PRIVATE ${MXI_WARNINGS})
add_library(mxi_split OBJECT apps/mxi_split.cc)
target_link_libraries(mxi_split PRIVATE mxi)
target_compile_options(mxi_split PRIVATE ${MXI_WARNINGS})
# mxi-view, in Python (mxi-ui's viewer, Tk): copied beside the programs, its
# version filled in, with the module mxi's Python tools share -- a command of
# its own, as mxi-ui is, mxi starting neither (docs/windows.md).
configure_file(python/src/mxi_view.py ${CMAKE_CURRENT_BINARY_DIR}/mxi-view @ONLY
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
  GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
configure_file(python/src/mxi_tools.py ${CMAKE_CURRENT_BINARY_DIR}/mxi_tools.py
  COPYONLY)
# mxi-ui, the interface (python/src/mxi_ui, docs/ui.md): the package copied
# beside mxi, its version stamped as mxi-view's is, and the launcher mxi-ui.
file(GLOB mxi_ui_sources CONFIGURE_DEPENDS python/src/mxi_ui/*.py)
foreach(source ${mxi_ui_sources})
  get_filename_component(name ${source} NAME)
  configure_file(${source} ${CMAKE_CURRENT_BINARY_DIR}/mxi_ui/${name} @ONLY)
endforeach()
configure_file(python/launch/mxi-ui ${CMAKE_CURRENT_BINARY_DIR}/mxi-ui COPYONLY
               FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
               GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
add_library(mxi_untrusted OBJECT apps/mxi_untrusted.cc)
target_link_libraries(mxi_untrusted PRIVATE mxi)
target_compile_options(mxi_untrusted PRIVATE ${MXI_WARNINGS})
# How much of what each lattice predicts was seen (src/verify.hh).
add_library(mxi_verify OBJECT apps/mxi_verify.cc)
target_link_libraries(mxi_verify PRIVATE mxi)
target_compile_options(mxi_verify PRIVATE ${MXI_WARNINGS})
# Every step in turn, as a script of them (apps/mxi_process.cc).
add_library(mxi_process OBJECT apps/mxi_process.cc)
target_link_libraries(mxi_process PRIVATE mxi)
target_compile_options(mxi_process PRIVATE ${MXI_WARNINGS})
# What the scaled data say of twinning, as phenix.xtriage (apps/mxi_triage.cc).
add_library(mxi_triage OBJECT apps/mxi_triage.cc)
target_link_libraries(mxi_triage PRIVATE mxi)
target_compile_options(mxi_triage PRIVATE ${MXI_WARNINGS})

add_library(mxi_grid OBJECT apps/mxi_grid.cc)
target_link_libraries(mxi_grid PRIVATE mxi)
target_compile_options(mxi_grid PRIVATE ${MXI_WARNINGS})

# The spot finder and the image reader, which need HDF5 and the bitshuffle
# submodule -- both required (below): without them nothing reads an image, and
# a build that quietly leaves out spot finding and integration is not one
# worth having (Graeme, v1.0).
#
# enable_testing() is called BEFORE it, not after: a subdirectory's add_test
# calls are only registered with the top-level CTest if testing was enabled
# before the subdirectory was entered. It sat after for a long time, which
# meant `ctest` from the top ran this project's tests and silently none of the
# spot finder's.

# AddressSanitizer and UndefinedBehaviorSanitizer, for running the tests under:
#
#     cmake -S . -B build-san -DMXI_SANITIZE=ON && cmake --build build-san
#     ctest --test-dir build-san
#
# -fno-sanitize-recover makes undefined behaviour ABORT rather than print and
# carry on. Without it UBSan reports and the test still passes, which is a
# sanitizer run that cannot fail. Global, so the spot finder and the libraries
# are instrumented as well as the tests: memory misused in a library and
# noticed in a test is only caught if both are built this way.
# MXI_SANITIZE itself is in cmake/spots.cmake: ON, or a list of sanitizers.


enable_testing()

# HDF5 is required (docs/rflx.md, Graeme's decision, October 2026): mxi's own
# files, .rflx, are HDF5, and so every program needs it, not only the spot
# finder and the importer.
# Every program's --version (src/args.cc).
# What this build calls itself -- mxi --version, the banner, every file's
# creator: the source's version, or what the build was asked to call it.
# A release's tag without its v (1.3.2-rc1, a candidate saying so), or
# "snapshot": the version and the commit (1.3.2+3806819), the rolling builds
# of every push to main telling one from another (Graeme, 14 November).
set(MXI_VERSION_LABEL "" CACHE STRING
    "What mxi calls itself: a release's tag without its v, or snapshot")
set(mxi_version_text "${PROJECT_VERSION}")
if(MXI_VERSION_LABEL STREQUAL "snapshot")
    execute_process(COMMAND git rev-parse --short=7 HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE mxi_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET RESULT_VARIABLE mxi_git)
    if(mxi_git EQUAL 0 AND mxi_commit)
        set(mxi_version_text "${PROJECT_VERSION}+${mxi_commit}")
    elseif(DEFINED ENV{GITHUB_SHA} AND NOT "$ENV{GITHUB_SHA}" STREQUAL "")
        # In the Linux builds' container git will not read a checkout owned
        # by another user ("dubious ownership"), and the first snapshots said
        # 1.3.2+snapshot: the workflow's own record of the commit instead.
        string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 mxi_commit)
        set(mxi_version_text "${PROJECT_VERSION}+${mxi_commit}")
    else()
        set(mxi_version_text "${PROJECT_VERSION}+snapshot")
    endif()
elseif(MXI_VERSION_LABEL)
    # A version, a candidate's or a release's, or nothing: anything else is a
    # mistake to stop at here, not to compile into every program -- as the
    # snapshot's first macOS build did, its label the rest of a shell line.
    if(NOT MXI_VERSION_LABEL MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[A-Za-z0-9.]+)?$")
        message(FATAL_ERROR "MXI_VERSION_LABEL \"${MXI_VERSION_LABEL}\" is "
                "neither a version (1.3.2, 1.3.2-rc1) nor snapshot")
    endif()
    set(mxi_version_text "${MXI_VERSION_LABEL}")
endif()
message(STATUS "mxi calls itself ${mxi_version_text}")
target_compile_definitions(mxi PRIVATE MXI_VERSION="${mxi_version_text}")
find_package(HDF5 REQUIRED COMPONENTS C)
target_include_directories(mxi SYSTEM PUBLIC ${HDF5_C_INCLUDE_DIRS})
target_link_libraries(mxi PUBLIC ${HDF5_C_LIBRARIES})
# bitshuffle (with LZ4) decompresses the images and compresses the .rflx's
# integer datasets: required, as HDF5 is.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bitshuffle/LICENSE")
    message(FATAL_ERROR
            "The bitshuffle submodule is not checked out, and without it nothing "
            "reads an image: git submodule update --init --recursive")
endif()
include(cmake/spots.cmake)
target_link_libraries(mxi PUBLIC bitshuffle)
target_compile_definitions(mxi PRIVATE MXI_HAVE_BITSHUFFLE)

# Profile fitting on a device, for mxi_integrate --gpu: CUDA's kernel where the
# spot finder's CUDA is on, and otherwise the stubs, which fit in single
# precision on the CPU instead.
if(MXI_CUDA)
    target_sources(mxi_integrate PRIVATE src/fit_device_cuda.cc src/fit_cuda.cu)
    target_link_libraries(mxi_integrate PRIVATE mxi_cuda_device)
elseif(MXI_METAL)
    # The shader compiled, linked and embedded as the spot finder's is, with the
    # same flags, under a name of its own.
    set(fit_metal_air "${CMAKE_CURRENT_BINARY_DIR}/fit_metal.air")
    set(fit_metallib "${CMAKE_CURRENT_BINARY_DIR}/fit_metal.metallib")
    set(fit_metallib_cc "${CMAKE_CURRENT_BINARY_DIR}/fit_metal_library.cc")
    add_custom_command(
        OUTPUT ${fit_metal_air}
        COMMAND ${MXI_XCRUN} -sdk macosx metal
                -std=metal3.0 ${mxi_metal_math} -Wall -Werror
                -c "${CMAKE_CURRENT_SOURCE_DIR}/src/fit_metal.metal"
                -o ${fit_metal_air}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/fit_metal.metal"
        COMMENT "Compiling src/fit_metal.metal")
    add_custom_command(
        OUTPUT ${fit_metallib}
        COMMAND ${MXI_XCRUN} -sdk macosx metallib ${fit_metal_air} -o ${fit_metallib}
        DEPENDS ${fit_metal_air}
        COMMENT "Linking fit_metal.metallib")
    add_custom_command(
        OUTPUT ${fit_metallib_cc}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${fit_metallib} -DOUTPUT=${fit_metallib_cc}
                -DSYMBOL=mxi_fit_metallib
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_metallib.cmake"
        DEPENDS ${fit_metallib} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_metallib.cmake"
        COMMENT "Embedding fit_metal.metallib")
    target_sources(mxi_integrate PRIVATE src/fit_device_metal.cc ${fit_metallib_cc})
    target_link_libraries(mxi_integrate PRIVATE mxi_metal)
    # SYSTEM: metal-cpp is not clean under -Wall -Wextra.
    target_include_directories(mxi_integrate SYSTEM PRIVATE ${METAL_CPP_INCLUDE_DIR})
    target_link_libraries(mxi_integrate PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
else()
    target_sources(mxi_integrate PRIVATE src/fit_device_none.cc)
endif()

# mimalloc in place of the C library's malloc, on Linux: glibc's, with 48
# threads on a two-socket Xeon allocating shoeboxes and freeing each other's,
# cost integration 11 s of 38 -- building boxes 12.7 s, 24 threads or 48 alike;
# Intel's scalable allocator preloaded, 4.3 s, every result the same (Graeme,
# 15 November, an L40S workstation). Linked as objects defining malloc, free
# and the rest, it serves everything in the process: mxi, the C++ library,
# HDF5, CUDA's runtime. macOS's allocator was never the trouble; Windows'
# untried. -DMXI_MIMALLOC=OFF for the C library's.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(mxi_mimalloc_default ON)
else()
    set(mxi_mimalloc_default OFF)
endif()
option(MXI_MIMALLOC "Link mimalloc in place of malloc (Linux by default)"
       ${mxi_mimalloc_default})
if(MXI_MIMALLOC)
    set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT ON CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # No architecture-specific code: on arm64 mimalloc's build turns on
    # -march=armv8.1-a whatever MI_OPT_ARCH says -- LSE atomics in every
    # malloc -- unless MI_NO_OPT_ARCH. 1.3.2's arm64 build died with SIGILL
    # on a Raspberry Pi 4 (ARMv8.0) before printing anything (Graeme, 16
    # November). GCC's outline atomics still give LSE where the CPU has it,
    # chosen at run time: the GH200 loses nothing.
    # scripts/check_arm64_portable.py holds the release builds to it.
    set(MI_NO_OPT_ARCH ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/mimalloc EXCLUDE_FROM_ALL)
    message(STATUS "malloc: mimalloc (third_party/mimalloc)")
else()
    message(STATUS "malloc: the C library's")
endif()

add_executable(mxi_tests
  tests/test_allocator.cc
  tests/main.cc
  tests/test_combine.cc
  tests/test_summary_cut.cc
  tests/test_triage.cc
  tests/test_fit_together.cc
  tests/test_index_neighbours.cc
  tests/test_output.cc
  tests/spots/test_dext_squares.cc
  tests/spots/test_dext_fused.cc
  tests/spots/test_signal_order.cc
  tests/spots/test_dials_spots.cc
  tests/spots/test_refl.cc
  tests/spots/test_refl_golden.cc
  tests/spots/test_expt.cc
  tests/spots/test_dials_end_to_end.cc
  tests/test_geometry.cc
  tests/test_real_geometry.cc
  tests/test_multi_axis.cc
  tests/test_index.cc
  tests/test_refine.cc
  tests/test_args.cc
  tests/test_derivatives.cc
  tests/test_precision.cc
  tests/test_expt_format.cc
  tests/test_background.cc
  tests/test_integrate.cc
  tests/test_reference.cc
  tests/test_refl.cc
  tests/test_shoebox.cc
  tests/test_summary.cc
  tests/test_postrefine.cc
  tests/test_symmetry.cc
  tests/test_lattice.cc
  tests/test_laue.cc
  tests/test_rflx.cc
  tests/test_cosym.cc
  tests/test_views.cc
  tests/test_processing.cc
  tests/test_two_theta.cc
  tests/test_correlation.cc
  tests/test_space_group.cc
  tests/test_resolution.cc
  tests/test_timing.cc
  tests/test_parallel.cc
  tests/test_fill_row.cc
  tests/test_scale_model.cc
  tests/test_scale.cc
  tests/test_mask.cc
  tests/test_profile_model.cc
)
target_link_libraries(mxi_tests PRIVATE mxi mxi_dext mxi_signal_order mxi_spots)
if(MXI_MIMALLOC)
    # The tests on the allocator the program has.
    target_link_libraries(mxi_tests PRIVATE mimalloc-obj)
    target_compile_definitions(mxi_tests PRIVATE MXI_MIMALLOC)
endif()
target_compile_options(mxi_tests PRIVATE ${MXI_WARNINGS})
add_test(NAME mxi_tests COMMAND mxi_tests)

# One program, mxi, every other a subcommand of it: mxi find, mxi index ...
# (apps/mxi.cc). Each program above is an object library, its entry
# mxi_<name>_main, its own sources and settings kept; mxi links them all. One
# binary rather than 24, each of which carried its own HDF5 and library -- 156
# MB where one is some 8 -- and one name to install (Graeme, 28 October).
set(MXI_PROGRAMS
  mxi_background mxi_combine mxi_convert mxi_correlation mxi_export mxi_find
  mxi_foreground mxi_forward mxi_grid mxi_import mxi_index mxi_integrate
  mxi_max mxi_profile mxi_readtest mxi_refine mxi_report mxi_residuals
  mxi_scale mxi_show mxi_split mxi_symmetry mxi_untrusted mxi_verify mxi_process mxi_triage)
# mxi_readtest, timing reads with POSIX's own calls, a diagnostic: not on
# Windows (docs/windows.md); apps/mxi.cc leaves its command out there too.
if(WIN32)
  list(REMOVE_ITEM MXI_PROGRAMS mxi_readtest)
endif()
# No source in two commands: they are one binary, and a source compiled into
# two is defined twice -- src/cuda_device.cu was, in mxi_find and
# mxi_integrate, and the CUDA release build failed to link (1.1.0). Shared code
# goes in a library (mxi_cuda_device, mxi_metal), linked once. Checked at
# configure, for whatever backend is configured.
set(mxi_seen_sources "")
foreach(program ${MXI_PROGRAMS})
  get_target_property(program_sources ${program} SOURCES)
  foreach(source ${program_sources})
    get_filename_component(source "${source}" ABSOLUTE)
    list(FIND mxi_seen_sources "${source}" seen)
    if(NOT seen EQUAL -1)
      list(GET mxi_seen_owners ${seen} owner)
      message(FATAL_ERROR "${source} is compiled into both ${owner} and "
              "${program}, which are one binary: put it in a library both link")
    endif()
    list(APPEND mxi_seen_sources "${source}")
    list(APPEND mxi_seen_owners ${program})
  endforeach()
endforeach()

# The target mxi_program, its file mxi: the target mxi is the library's.
add_executable(mxi_program apps/mxi.cc)
set_target_properties(mxi_program PROPERTIES OUTPUT_NAME mxi)
target_link_libraries(mxi_program PRIVATE ${MXI_PROGRAMS} mxi)
if(MXI_MIMALLOC)
    target_link_libraries(mxi_program PRIVATE mimalloc-obj)
endif()
target_compile_definitions(mxi_program PRIVATE MXI_VERSION="${mxi_version_text}")
# What this build is, for the banner every program prints first and every log
# begins with: the accelerator and its version, and the compiler (Graeme, 13
# November -- logs sent from a dozen machines had none of it).
if(MXI_CUDA)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" mxi_cuda_short "${CMAKE_CUDA_COMPILER_VERSION}")
    set(mxi_accelerator "CUDA ${mxi_cuda_short}")
elseif(MXI_METAL)
    set(mxi_accelerator "Metal")
else()
    set(mxi_accelerator "CPU only")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" mxi_cxx_short "${CMAKE_CXX_COMPILER_VERSION}")
if(MXI_MIMALLOC)
    set(mxi_allocator ", mimalloc")
else()
    set(mxi_allocator "")
endif()
target_compile_definitions(mxi_program PRIVATE
    MXI_BUILD_DESCRIPTION="${mxi_accelerator}, ${CMAKE_CXX_COMPILER_ID} ${mxi_cxx_short}${mxi_allocator}")
target_compile_options(mxi_program PRIVATE ${MXI_WARNINGS})
# In the build directory, each program's old name a link to mxi, which runs
# the program it is called by: the tests run them so. Not installed.
# Not on Windows: a link there wants privileges, and would be to mxi, not
# mxi.exe; its tests do not run them.
if(NOT WIN32)
  foreach(program ${MXI_PROGRAMS})
    file(CREATE_LINK mxi ${CMAKE_CURRENT_BINARY_DIR}/${program} SYMBOLIC)
  endforeach()
endif()

# Installing: mxi alone --
#   cmake --install build --prefix ~/.local
# -- which then wants to be on the PATH. The library is linked in; nothing
# else is needed beside it (the Metal libraries are compiled in). The
# Python -- mxi-ui, mxi-view, mxeq, mxi_tools -- is the wheel's: pip install
# ./python, which finds this mxi on the PATH; a release's wheel carries mxi
# itself (scripts/make_wheel.py; Graeme, 8 November: everything in the
# wheel, the release archives mxi alone). The build directory keeps its
# copies of the Python beside mxi, to run and test from there.
install(TARGETS mxi_program RUNTIME DESTINATION bin)
