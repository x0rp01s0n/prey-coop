if(NOT COOP_CHAIRLOADER_COMMON_PATH)
	message(FATAL_ERROR
		"COOP_CHAIRLOADER_COMMON_PATH was not prepared. "
		"Call coop_prepare_chairloader_common() before adding CommonMod."
	)
endif()

set(COMMON_PATH "${COOP_CHAIRLOADER_COMMON_PATH}")
set(COMMON_ROOT_PATH "${COMMON_PATH}/..")

# Check if there are any .cpp files
file(GLOB_RECURSE COMMON_CPP_SOURCE_FILES
	LIST_DIRECTORIES false
	"${COMMON_PATH}/*.cpp"
)

if(NOT COMMON_CPP_SOURCE_FILES)
	message(STATUS "Staged Chairloader common path: ${COMMON_PATH}")
	message(FATAL_ERROR "No Chairloader Common source files found in the staged SDK tree.")
endif()

# Grab all files in Common directory
file(GLOB_RECURSE COMMON_SOURCE_FILES
	LIST_DIRECTORIES false
	CONFIGURE_DEPENDS
	"${COMMON_PATH}/*.*"
)

# List of common source files
set(SOURCE_FILES
	CMakeLists.txt
	CommonModPch.h
	README_ModSDK.txt
)

# CommonMod static library
add_library(CommonMod STATIC
	${COMMON_SOURCE_FILES}
	${SOURCE_FILES}
)

target_include_directories(CommonMod PUBLIC
	.
	"${COMMON_PATH}"
	"${COMMON_PATH}/Chairloader.ImGui" # In Chairloader it's built as a separate static lib. Here, it's part of CommonMod
	"${COMMON_PATH}/pugixml"
	${Boost_INCLUDE_DIRS}
)

target_link_libraries(CommonMod PUBLIC
	Detours::Detours
	fmt::fmt-header-only
)

target_precompile_headers(CommonMod PUBLIC CommonModPch.h)

source_group(TREE "${COMMON_ROOT_PATH}" FILES ${COMMON_SOURCE_FILES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${SOURCE_FILES})
