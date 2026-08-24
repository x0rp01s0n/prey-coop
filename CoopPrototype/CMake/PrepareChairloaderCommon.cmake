include_guard(GLOBAL)

function(_coop_chairloader_unsupported)
	string(JOIN "" _message_text ${ARGV})
	if(COOP_ALLOW_UNSUPPORTED_CHAIRLOADER)
		message(WARNING
			"${_message_text}\n"
			"Continuing because COOP_ALLOW_UNSUPPORTED_CHAIRLOADER is enabled."
		)
	else()
		message(FATAL_ERROR
			"${_message_text}\n"
			"Use -DCOOP_ALLOW_UNSUPPORTED_CHAIRLOADER=ON only for deliberate compatibility testing."
		)
	endif()
endfunction()

function(coop_prepare_chairloader_common upstream_common out_var)
	if("${upstream_common}" STREQUAL "")
		message(FATAL_ERROR
			"CHAIRLOADER_COMMON_PATH is required and must point to an unmodified "
			"upstream Chairloader/Common directory."
		)
	endif()

	get_filename_component(_upstream_candidate "${upstream_common}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
	if(NOT IS_DIRECTORY "${_upstream_candidate}")
		message(FATAL_ERROR "Chairloader Common directory does not exist: ${_upstream_candidate}")
	endif()
	file(REAL_PATH "${_upstream_candidate}" _upstream_common)

	set(_required_upstream_files
		"Prey/GameDll/ark/turret/ArkTurret.h"
		"Prey/GameDll/ark/signalsystem/arksignalmanager.h"
		"Prey/CryMovie/IMovieSystem.h"
		"VTableHook.cpp"
	)
	foreach(_required_file IN LISTS _required_upstream_files)
		if(NOT EXISTS "${_upstream_common}/${_required_file}")
			message(FATAL_ERROR
				"CHAIRLOADER_COMMON_PATH is not a compatible Chairloader Common tree.\n"
				"Missing: ${_upstream_common}/${_required_file}"
			)
		endif()
	endforeach()

	set(_compat_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../ChairloaderCompat")
	set(_base_revision_file "${_compat_root}/base-revision.txt")
	set(_overlay_common "${_compat_root}/overlay/Common")
	set(_patch_file "${_compat_root}/patches/0001-prey-coop-chairloader-sdk.patch")

	foreach(_compat_path IN ITEMS "${_base_revision_file}" "${_overlay_common}" "${_patch_file}")
		if(NOT EXISTS "${_compat_path}")
			message(FATAL_ERROR "Chairloader compatibility package is incomplete: ${_compat_path}")
		endif()
	endforeach()

	file(READ "${_base_revision_file}" _supported_revision)
	string(STRIP "${_supported_revision}" _supported_revision)
	string(TOLOWER "${_supported_revision}" _supported_revision)
	string(LENGTH "${_supported_revision}" _supported_revision_length)
	if(NOT _supported_revision_length EQUAL 40 OR
	   NOT _supported_revision MATCHES "^[0-9a-f]+$")
		message(FATAL_ERROR
			"Invalid Chairloader revision in ${_base_revision_file}: '${_supported_revision}'"
		)
	endif()

	find_package(Git REQUIRED)

	set(_actual_revision "")
	set(_dirty_state "")

	# The container build mounts Common read-only and reports these values from
	# the host checkout. Native builds normally fall back to direct Git queries.
	if(NOT "${COOP_CHAIRLOADER_SOURCE_REVISION}" STREQUAL "")
		string(TOLOWER "${COOP_CHAIRLOADER_SOURCE_REVISION}" _actual_revision)
		string(LENGTH "${_actual_revision}" _actual_revision_length)
		if(NOT _actual_revision_length EQUAL 40 OR
		   NOT _actual_revision MATCHES "^[0-9a-f]+$")
			message(FATAL_ERROR
				"COOP_CHAIRLOADER_SOURCE_REVISION is not a full Git SHA: "
				"'${COOP_CHAIRLOADER_SOURCE_REVISION}'"
			)
		endif()
		set(_dirty_state "${COOP_CHAIRLOADER_SOURCE_DIRTY}")
	else()
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${_upstream_common}" rev-parse --show-toplevel
			RESULT_VARIABLE _git_root_result
			OUTPUT_VARIABLE _git_root
			ERROR_VARIABLE _git_root_error
			OUTPUT_STRIP_TRAILING_WHITESPACE
		)
		if(_git_root_result EQUAL 0)
			file(REAL_PATH "${_git_root}" _git_root)
			file(RELATIVE_PATH _common_relative "${_git_root}" "${_upstream_common}")
			if(_common_relative STREQUAL ".." OR _common_relative MATCHES "^\\.\\.[/\\\\]")
				message(FATAL_ERROR
					"Chairloader Common resolved outside its reported Git worktree: ${_upstream_common}"
				)
			endif()

			execute_process(
				COMMAND "${GIT_EXECUTABLE}" -C "${_git_root}" rev-parse HEAD
				RESULT_VARIABLE _git_head_result
				OUTPUT_VARIABLE _actual_revision
				ERROR_VARIABLE _git_head_error
				OUTPUT_STRIP_TRAILING_WHITESPACE
			)
			if(NOT _git_head_result EQUAL 0)
				_coop_chairloader_unsupported(
					"Could not read Chairloader Git HEAD: ${_git_head_error}"
				)
				set(_actual_revision "unknown")
			else()
				string(TOLOWER "${_actual_revision}" _actual_revision)
			endif()

			execute_process(
				COMMAND "${GIT_EXECUTABLE}" -C "${_git_root}"
					status --porcelain --untracked-files=all -- "${_common_relative}"
				RESULT_VARIABLE _git_status_result
				OUTPUT_VARIABLE _git_status
				ERROR_VARIABLE _git_status_error
				OUTPUT_STRIP_TRAILING_WHITESPACE
			)
			if(NOT _git_status_result EQUAL 0)
				_coop_chairloader_unsupported(
					"Could not verify that Chairloader/Common is clean: ${_git_status_error}"
				)
				set(_dirty_state "UNKNOWN")
			elseif("${_git_status}" STREQUAL "")
				set(_dirty_state "OFF")
			else()
				set(_dirty_state "ON")
			endif()
		else()
			_coop_chairloader_unsupported(
				"CHAIRLOADER_COMMON_PATH is not inside a readable Git checkout: "
				"${_upstream_common}\n${_git_root_error}"
			)
			set(_actual_revision "unknown")
			set(_dirty_state "UNKNOWN")
		endif()
	endif()

	if(NOT "${_actual_revision}" STREQUAL "unknown" AND
	   NOT "${_actual_revision}" STREQUAL "${_supported_revision}")
		_coop_chairloader_unsupported(
			"Unsupported Chairloader revision.\n"
			"Expected: ${_supported_revision}\n"
			"Actual:   ${_actual_revision}"
		)
	endif()

	string(TOUPPER "${_dirty_state}" _dirty_state_upper)
	if(_dirty_state_upper STREQUAL "1" OR
	   _dirty_state_upper STREQUAL "ON" OR
	   _dirty_state_upper STREQUAL "TRUE" OR
	   _dirty_state_upper STREQUAL "YES")
		_coop_chairloader_unsupported(
			"Chairloader/Common has tracked or untracked local changes. "
			"Use a clean upstream checkout for reproducible builds."
		)
	elseif(NOT _dirty_state_upper STREQUAL "0" AND
	       NOT _dirty_state_upper STREQUAL "OFF" AND
	       NOT _dirty_state_upper STREQUAL "FALSE" AND
	       NOT _dirty_state_upper STREQUAL "NO")
		_coop_chairloader_unsupported(
			"Could not verify that Chairloader/Common is unmodified."
		)
	endif()

	file(GLOB_RECURSE _overlay_files
		CONFIGURE_DEPENDS
		LIST_DIRECTORIES false
		RELATIVE "${_overlay_common}"
		"${_overlay_common}/*"
	)
	if(NOT _overlay_files)
		message(FATAL_ERROR "Chairloader compatibility overlay is empty: ${_overlay_common}")
	endif()

	# Overlay files must remain absent upstream. An upstream collision signals
	# that this package needs rebasing rather than silently shadowing a real file.
	foreach(_overlay_file IN LISTS _overlay_files)
		if(EXISTS "${_upstream_common}/${_overlay_file}")
			message(FATAL_ERROR
				"Chairloader now provides an overlay path: ${_overlay_file}\n"
				"Rebase ChairloaderCompat and remove or update the duplicate file."
			)
		endif()
	endforeach()

	get_filename_component(_stage_root "${CMAKE_BINARY_DIR}/_deps/chairloader-sdk" ABSOLUTE)
	set(_stage_common "${_stage_root}/Common")
	if("${_upstream_common}" STREQUAL "${_stage_common}")
		message(FATAL_ERROR "CHAIRLOADER_COMMON_PATH must not point at the private staged SDK tree.")
	endif()

	file(REMOVE_RECURSE "${_stage_root}")
	file(MAKE_DIRECTORY "${_stage_common}")
	file(COPY "${_upstream_common}/" DESTINATION "${_stage_common}")
	file(COPY "${_overlay_common}/" DESTINATION "${_stage_common}")

	# Git for Windows commonly materializes text files with CRLF. Normalize only
	# the patch targets in the private stage so the LF patch remains deterministic
	# while the contributor's source checkout stays byte-for-byte untouched.
	set(_patch_target_files
		"Common/Prey/GameDll/ark/turret/ArkTurret.h"
		"Common/Prey/GameDll/ark/signalsystem/arksignalmanager.h"
		"Common/Prey/CryMovie/IMovieSystem.h"
	)
	foreach(_patch_target IN LISTS _patch_target_files)
		set(_patch_target_path "${_stage_root}/${_patch_target}")
		file(READ "${_patch_target_path}" _patch_target_contents)
		string(REPLACE "\r\n" "\n" _patch_target_contents "${_patch_target_contents}")
		file(WRITE "${_patch_target_path}" "${_patch_target_contents}")
	endforeach()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --check --whitespace=nowarn "${_patch_file}"
		WORKING_DIRECTORY "${_stage_root}"
		RESULT_VARIABLE _patch_check_result
		OUTPUT_VARIABLE _patch_check_output
		ERROR_VARIABLE _patch_check_error
	)
	if(NOT _patch_check_result EQUAL 0)
		message(FATAL_ERROR
			"The prey-coop Chairloader compatibility patch does not apply cleanly.\n"
			"Supported revision: ${_supported_revision}\n"
			"Actual revision:    ${_actual_revision}\n\n"
			"${_patch_check_output}${_patch_check_error}"
		)
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${_patch_file}"
		WORKING_DIRECTORY "${_stage_root}"
		RESULT_VARIABLE _patch_result
		OUTPUT_VARIABLE _patch_output
		ERROR_VARIABLE _patch_error
	)
	if(NOT _patch_result EQUAL 0)
		message(FATAL_ERROR
			"Failed to apply the prey-coop Chairloader compatibility patch.\n"
			"${_patch_output}${_patch_error}"
		)
	endif()

	file(WRITE "${_stage_root}/prey-coop-compat-metadata.txt"
		"supported_revision=${_supported_revision}\n"
		"actual_revision=${_actual_revision}\n"
		"source_dirty=${_dirty_state_upper}\n"
	)

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${_base_revision_file}"
		"${_patch_file}"
	)

	set(${out_var} "${_stage_common}" PARENT_SCOPE)
	set(COOP_CHAIRLOADER_COMMON_PATH "${_stage_common}" CACHE INTERNAL
		"Private staged and patched Chairloader Common directory." FORCE)
	message(STATUS "Chairloader Common staged privately at: ${_stage_common}")
endfunction()
