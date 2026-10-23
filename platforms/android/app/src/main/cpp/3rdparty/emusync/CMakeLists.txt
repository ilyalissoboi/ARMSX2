# emusync — memory card sync through the cloud (https://github.com/ilyalissoboi/emusync).
#
# Like librashader next door, emusync is a RUST project, so it is not vendored: this repo keeps no
# git submodules and no checked-in binaries. A PINNED commit is fetched at configure time and its
# C-ABI library is built by cargo as libemusync.so for the ABI being built, through emusync's own
# CMake module (bindings/cmake/EmuSyncAndroid.cmake). The pin keeps the build reproducible.
#
# libemusync.so is a SEPARATE shared library, not linked into emucore statically. emusync bundles
# its own zstd and SQLite; a static link would put a second copy of the ZSTD_* symbols next to the
# core's, and drag Rust objects into the core's LTO link. The cdylib exports only es_* symbols.
#
# OPTIONAL, exactly like librashader: without cargo (or the Rust target for this ABI) the module
# leaves ARMSX2_HAVE_EMUSYNC off and memory card sync compiles out (#ifdef ARMSX2_HAS_EMUSYNC), so
# a contributor without a Rust toolchain still builds.
#
# For local development against an emusync checkout, configure with
#   -DEMUSYNC_SOURCE_DIR=/path/to/emusync
# and the fetch is skipped.

# Pinned emusync commit. Bump deliberately; never track a branch. A plain variable, not a cache
# entry: a cached pin stays at its first value in an existing build folder, so a bump here was
# silently ignored there (and the build used the old library). Use EMUSYNC_SOURCE_DIR to build
# against something else.
set(EMUSYNC_PIN "760171a04a2ccc9a762279c534b083327d6e8fdb")
# emusync's module caches EMUSYNC_SOURCE_DIR. A cached folder inside this build folder is an earlier
# fetch, not a checkout given with -D: forget it, or a bump would keep building the old source.
if(DEFINED CACHE{EMUSYNC_SOURCE_DIR})
	get_filename_component(_emusync_cached "$CACHE{EMUSYNC_SOURCE_DIR}" ABSOLUTE)
	string(FIND "${_emusync_cached}/" "${CMAKE_BINARY_DIR}/" _emusync_in_build)
	if(_emusync_in_build EQUAL 0)
		unset(EMUSYNC_SOURCE_DIR CACHE)
	endif()
endif()
option(ARMSX2_DISABLE_EMUSYNC "Build without memory card sync even when Rust is available" OFF)

if(ARMSX2_DISABLE_EMUSYNC)
	message(STATUS "emusync: explicitly disabled (ARMSX2_DISABLE_EMUSYNC)")
	set(ARMSX2_HAVE_EMUSYNC OFF PARENT_SCOPE)
	return()
endif()

# The module reports a missing cargo itself, but checking first avoids a pointless fetch.
find_program(EMUSYNC_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
if(NOT EMUSYNC_CARGO)
	message(STATUS "emusync: cargo not found -> memory card sync DISABLED. "
	               "Install Rust (https://rustup.rs) to enable it.")
	set(ARMSX2_HAVE_EMUSYNC OFF PARENT_SCOPE)
	return()
endif()

if(NOT EMUSYNC_SOURCE_DIR)
	# ---- Fetch the pinned source (configure time, nothing vendored) ----------------------------
	include(FetchContent)
	FetchContent_Declare(emusync_src
		GIT_REPOSITORY https://github.com/ilyalissoboi/emusync.git
		GIT_TAG        ${EMUSYNC_PIN}
	)
	# emusync has no top-level CMakeLists.txt, so this only downloads; nothing is added to the build.
	FetchContent_MakeAvailable(emusync_src)
	set(EMUSYNC_SOURCE_DIR "${emusync_src_SOURCE_DIR}")
endif()

include("${EMUSYNC_SOURCE_DIR}/bindings/cmake/EmuSyncAndroid.cmake")
set(ARMSX2_HAVE_EMUSYNC ${EMUSYNC_ANDROID_FOUND} PARENT_SCOPE)
if(EMUSYNC_ANDROID_FOUND)
	message(STATUS "emusync: ${EMUSYNC_SOURCE_DIR} -> ${EMUSYNC_ANDROID_LIBRARY}")
endif()
