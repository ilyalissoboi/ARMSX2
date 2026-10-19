# librashader — RetroArch (.slangp) shader chains for the GS post-process path.
#
# librashader is a RUST project, so it can't be vendored the way the C/C++ 3rdparty deps
# are: this repo keeps no git submodules and no checked-in binaries. Instead we fetch a
# PINNED commit at configure time and cargo-build its C-ABI library for the ABI being
# built. Nothing lands in git, and the pin means the build stays reproducible.
#
# Its C-ABI crate is a LEAF crate (crate-type = cdylib/staticlib, no rlib), so it cannot
# be pulled in as a normal cargo dependency from a small wrapper — it has to be built
# from its own source tree via --manifest-path. That's why we fetch the whole repo.
#
# RUST IS OPTIONAL. If cargo isn't installed we simply don't create the target and leave
# ARMSX2_HAVE_LIBRASHADER off, so every existing build (and any contributor without a
# Rust toolchain) keeps working exactly as before — the shader chain compiles out via
# #ifdef rather than breaking the build.
#
# Both runtimes are enabled, and GLES is NOT a limitation: librashader's GL runtime
# detects an embedded (GLES) context and targets GLSL ES 300/310/320 (see
# librashader-runtime-gl/src/util.rs :: gl_get_version -> version.is_embedded), and its
# gl3 path only uses GLES-3.0-safe calls (no DSA / named_*, no get_tex_image, plural
# draw_buffers). Upstream's "no legacy OpenGL" note means GL 2.x, not GLES 3.x. So
# shaders run on BOTH the Vulkan and OpenGL renderers.

set(LIBRASHADER_PIN "87e8a97b50516d997defeaa168173dcd185d4022"
    CACHE STRING "Pinned librashader commit. Bump deliberately — never track a branch.")

# Explicit opt-out, for builds that must stay single-libc++: the capi cdylib links
# libc++_shared, while ANDROID_STL=c++_static builds link libc++ statically, and a
# bare NDK EXECUTABLE with both copies in-process aborts with std::bad_cast out of
# locale::use_facet at startup (two sets of locale/facet statics; observed on the
# RG477V, Android 14). Headless pcsx2-gsrunner builds pass
# -DARMSX2_DISABLE_LIBRASHADER=ON and compile the shader chain out instead.
if(ARMSX2_DISABLE_LIBRASHADER)
	message(STATUS "librashader: explicitly disabled (ARMSX2_DISABLE_LIBRASHADER)")
	set(ARMSX2_HAVE_LIBRASHADER OFF PARENT_SCOPE)
	return()
endif()

# ---- Rust toolchain (optional) ---------------------------------------------------
find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin" "$ENV{CARGO_HOME}/bin")
if(NOT CARGO_EXECUTABLE)
	message(STATUS "librashader: cargo not found -> RetroArch shader support DISABLED. "
	               "Install Rust (https://rustup.rs) to enable it.")
	set(ARMSX2_HAVE_LIBRASHADER OFF PARENT_SCOPE)
	return()
endif()

# cargo must be recent enough to parse the pinned tree's dependency graph. moxcms (a
# transitive dep) is edition 2024, which needs cargo >= 1.85; an older-but-present cargo
# otherwise fails the WHOLE native build deep inside an opaque transitive manifest parse
# ("feature `edition2024` is required"). Disable + warn instead, matching the "cargo not
# found" contract above so a stale toolchain compiles the feature out rather than breaking
# the build.
execute_process(COMMAND ${CARGO_EXECUTABLE} --version
	OUTPUT_VARIABLE _lrs_cargo_ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _lrs_cargo_ver "${_lrs_cargo_ver}")
if(_lrs_cargo_ver VERSION_LESS "1.85.0")
	message(STATUS "librashader: cargo ${_lrs_cargo_ver} too old (need >= 1.85 for edition2024) "
	               "-> RetroArch shader support DISABLED. Run `rustup update` to enable it.")
	set(ARMSX2_HAVE_LIBRASHADER OFF PARENT_SCOPE)
	return()
endif()

# ---- Android ABI -> Rust target ---------------------------------------------------
if(ANDROID_ABI STREQUAL "arm64-v8a")
	set(_lrs_rust_target "aarch64-linux-android")
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
	set(_lrs_rust_target "armv7-linux-androideabi")
elseif(ANDROID_ABI STREQUAL "x86_64")
	set(_lrs_rust_target "x86_64-linux-android")
elseif(ANDROID_ABI STREQUAL "x86")
	set(_lrs_rust_target "i686-linux-android")
else()
	message(STATUS "librashader: unmapped ANDROID_ABI '${ANDROID_ABI}' -> DISABLED.")
	set(ARMSX2_HAVE_LIBRASHADER OFF PARENT_SCOPE)
	return()
endif()

# ---- NDK toolchain for cargo's linker/cc ------------------------------------------
# cargo drives its own linker, so hand it the same NDK clang CMake is using: the plain
# clang binary, with the target and API level passed as --target. NOT the per-API
# wrappers (aarch64-linux-android26-clang): on a Windows host those are .cmd scripts,
# which cargo can't exec as a linker (it looks for an .exe), so the build failed there.
# The plain binary is the same on every host, and the toolchain dir follows from it.
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	message(STATUS "librashader: not building with the NDK's clang -> DISABLED.")
	set(ARMSX2_HAVE_LIBRASHADER OFF PARENT_SCOPE)
	return()
endif()
set(_lrs_cc "${CMAKE_C_COMPILER}")
set(_lrs_cxx "${CMAKE_CXX_COMPILER}")
get_filename_component(_lrs_bin "${_lrs_cc}" DIRECTORY)
get_filename_component(_lrs_tc "${_lrs_bin}" DIRECTORY)
set(_lrs_ar "${CMAKE_AR}")
if(NOT _lrs_ar)
	set(_lrs_ar "${_lrs_bin}/llvm-ar${CMAKE_EXECUTABLE_SUFFIX}")
endif()
set(_lrs_ranlib "${CMAKE_RANLIB}")
if(NOT _lrs_ranlib)
	set(_lrs_ranlib "${_lrs_bin}/llvm-ranlib${CMAKE_EXECUTABLE_SUFFIX}")
endif()

# API level: ANDROID_PLATFORM looks like "android-26"; clang's --target wants "26".
set(_lrs_api "${ANDROID_PLATFORM}")
string(REGEX REPLACE "^android-" "" _lrs_api "${_lrs_api}")
if(NOT _lrs_api)
	set(_lrs_api 26)
endif()

# armv7 is the one target whose clang triple != its rust triple.
set(_lrs_clang_target "${_lrs_rust_target}${_lrs_api}")
if(_lrs_rust_target STREQUAL "armv7-linux-androideabi")
	set(_lrs_clang_target "armv7a-linux-androideabi${_lrs_api}")
endif()

# cargo wants the triple upper-cased with underscores for its per-target linker var.
string(TOUPPER "${_lrs_rust_target}" _lrs_target_upper)
string(REPLACE "-" "_" _lrs_target_upper "${_lrs_target_upper}")
string(REPLACE "-" "_" _lrs_target_under "${_lrs_rust_target}")

# ---- Fetch the pinned source (configure time, nothing vendored) --------------------
# librashader has no root CMakeLists, so point SOURCE_SUBDIR at a path that doesn't
# exist: that populates the tree WITHOUT trying to add_subdirectory() it.
include(FetchContent)
FetchContent_Declare(librashader_src
	GIT_REPOSITORY https://github.com/SnowflakePowered/librashader.git
	GIT_TAG        ${LIBRASHADER_PIN}
	SOURCE_SUBDIR  cmake-does-not-exist
)
FetchContent_MakeAvailable(librashader_src)

set(_lrs_out_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(_lrs_lib "${_lrs_out_dir}/${_lrs_rust_target}/release/liblibrashader_capi.so")

# ---- cargo build ------------------------------------------------------------------
# Shared (cdylib) rather than the 80 MB staticlib: keeps the Rust objects out of
# emucore's LTO/link entirely (no allocator/panic-handler or duplicate-symbol
# interplay with a huge C++ LTO link), at the cost of one extra ~13 MB .so.
add_custom_command(
	OUTPUT "${_lrs_lib}"
	COMMAND ${CMAKE_COMMAND} -E env
		"CARGO_TARGET_${_lrs_target_upper}_LINKER=${_lrs_cc}"
		"CC_${_lrs_target_under}=${_lrs_cc}"
		"CXX_${_lrs_target_under}=${_lrs_cxx}"
		# cc-rs passes its own unversioned --target first; these come after it and win,
		# so the C/C++ deps (glslang, spirv-cross) see the right __ANDROID_API__.
		"CFLAGS_${_lrs_target_under}=--target=${_lrs_clang_target}"
		"CXXFLAGS_${_lrs_target_under}=--target=${_lrs_clang_target}"
		"AR_${_lrs_target_under}=${_lrs_ar}"
		"RANLIB_${_lrs_target_under}=${_lrs_ranlib}"
		# rustc does NOT emit a SONAME for an Android cdylib. Without one the linker
		# records the full path we hand it as emucore's DT_NEEDED
		# ("3rdparty/librashader/cargo/.../liblibrashader_capi.so"), and at runtime
		# Android — which resolves NEEDED by bare name out of lib/<abi>/ — can't find
		# it, so the core fails to load. Stamp the soname so DT_NEEDED is just the
		# filename, exactly like a CMake-built .so.
		# The --target link-arg is the linker's half of the plain-clang setup above.
		"RUSTFLAGS=-C link-arg=--target=${_lrs_clang_target} -C link-arg=-Wl,-soname,liblibrashader_capi.so"
		${CARGO_EXECUTABLE} build --release
			--manifest-path "${librashader_src_SOURCE_DIR}/librashader-capi/Cargo.toml"
			--target ${_lrs_rust_target}
			--target-dir "${_lrs_out_dir}"
			--no-default-features
			--features runtime-vulkan,runtime-opengl
	WORKING_DIRECTORY "${librashader_src_SOURCE_DIR}"
	COMMENT "librashader: cargo build (${_lrs_rust_target}, vulkan+opengl)"
	VERBATIM
)
add_custom_target(librashader_cargo DEPENDS "${_lrs_lib}")

# AGP only packages .so files that CMake itself emits into the per-ABI library output
# dir. Ours is produced by cargo behind an IMPORTED target, which AGP knows nothing
# about — without this copy the core links happily and then dies at runtime with
# UnsatisfiedLinkError. adrenotools doesn't need this because its hooks are real
# add_library(SHARED) targets.
#
# libc++_shared.so must ship ALONGSIDE it. The rest of ARMSX2 links libc++ STATICALLY, so
# nothing else in the app pulls libc++_shared.so in and AGP never packages it -- but cargo
# builds librashader's C++ deps (glslang / spirv-cross) against the SHARED runtime, so
# liblibrashader_capi.so records NEEDED libc++_shared.so. Miss this and the failure is
# catastrophic and totally unrelated-looking: emucore's whole dlopen chain fails
#   "library libc++_shared.so not found: needed by liblibrashader_capi.so"
# so EVERY JNI method disappears and the app dies on boot with UnsatisfiedLinkError on
# NativeApp.initialize -- even though nothing touched the shader feature.
# Safe despite the app's own static libc++: librashader's boundary is a pure extern "C" API,
# so no C++ objects or exceptions ever cross between the two runtimes.
# The sysroot lib dir is named after the *toolchain* triple, which matches the rust triple
# on every target except armv7 (rust: armv7-linux-androideabi / sysroot: arm-linux-androideabi),
# the same mismatch the clang triple above has to special-case.
set(_lrs_sysroot_triple "${_lrs_rust_target}")
if(_lrs_sysroot_triple STREQUAL "armv7-linux-androideabi")
	set(_lrs_sysroot_triple "arm-linux-androideabi")
endif()
set(_lrs_cxx_shared "${_lrs_tc}/sysroot/usr/lib/${_lrs_sysroot_triple}/libc++_shared.so")
if(NOT EXISTS "${_lrs_cxx_shared}")
	message(FATAL_ERROR "librashader: libc++_shared.so not found at ${_lrs_cxx_shared} -- "
		"liblibrashader_capi.so links it dynamically, so shipping without it bricks boot.")
endif()

if(CMAKE_LIBRARY_OUTPUT_DIRECTORY)
	add_custom_command(TARGET librashader_cargo POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lrs_lib}" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lrs_cxx_shared}" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/"
		COMMENT "librashader: staging liblibrashader_capi.so + libc++_shared.so for packaging"
		VERBATIM
	)
endif()

add_library(librashader SHARED IMPORTED GLOBAL)
set_target_properties(librashader PROPERTIES IMPORTED_LOCATION "${_lrs_lib}")
target_include_directories(librashader INTERFACE "${librashader_src_SOURCE_DIR}/include")
add_dependencies(librashader librashader_cargo)

set(ARMSX2_HAVE_LIBRASHADER ON PARENT_SCOPE)
set(ARMSX2_LIBRASHADER_SO "${_lrs_lib}" PARENT_SCOPE)
message(STATUS "librashader: ENABLED (${_lrs_rust_target}, api ${_lrs_api}, vulkan+opengl)")
