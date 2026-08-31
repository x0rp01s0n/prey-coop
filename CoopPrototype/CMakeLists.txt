cmake_minimum_required(VERSION 3.25)
project(CoopPrototype)

# Search for libraries in CMake
list(APPEND CMAKE_MODULE_PATH
	"${CMAKE_CURRENT_LIST_DIR}/CMake"
)

# Chairloader functions
include(CMake/CompilerConfig.cmake)
include(CMake/ModConfig.cmake)

# vcpkg libraries
find_package(Boost CONFIG REQUIRED)
find_package(Detours MODULE REQUIRED)
find_package(fmt CONFIG REQUIRED)

# Chairloader SDK compatibility
set(
	CHAIRLOADER_COMMON_PATH
	""
	CACHE PATH
	"Path to an unmodified upstream Chairloader/Common directory."
)
option(
	COOP_ALLOW_UNSUPPORTED_CHAIRLOADER
	"Allow a non-pinned or locally modified Chairloader Common tree (the compatibility patch must still apply)."
	OFF
)
set(
	COOP_CHAIRLOADER_SOURCE_REVISION
	""
	CACHE STRING
	"Chairloader Git HEAD reported by an outer build wrapper when the checkout metadata is not mounted."
)
set(
	COOP_CHAIRLOADER_SOURCE_DIRTY
	""
	CACHE STRING
	"Chairloader Common dirty state reported by an outer build wrapper (ON, OFF, or UNKNOWN)."
)
mark_as_advanced(
	COOP_CHAIRLOADER_SOURCE_REVISION
	COOP_CHAIRLOADER_SOURCE_DIRTY
)

include(CMake/PrepareChairloaderCommon.cmake)
coop_prepare_chairloader_common(
	"${CHAIRLOADER_COMMON_PATH}"
	COOP_CHAIRLOADER_COMMON_PATH
)

# Third-party libraries
# add_subdirectory(ThirdParty/ExampleLibrary)

# Project libraries
add_subdirectory(CommonMod)

# Mods
add_subdirectory(Src)

# Hardware faults must reach the SEH filters used by runtime reads. Keep /EHa
# and the matching PCH exclusion scoped to the guard implementation only.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Src/CoopRuntimeGuards.cpp"
	TARGET_DIRECTORY CoopPrototype
	APPEND PROPERTY COMPILE_OPTIONS /EHa
)
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Src/CoopRuntimeGuards.cpp"
	TARGET_DIRECTORY CoopPrototype
	PROPERTY SKIP_PRECOMPILE_HEADERS ON
)
