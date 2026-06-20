include_guard(GLOBAL)

# Shared Slang→SPIR-V build rule used identically by Harmonia, Hyperion and
# Theia: every entry shader is compiled at build time into <output_dir> (the
# project's *_SHADER_DIR compile definition), and the renderers load the .spv
# files from that absolute path — never from a working-directory-relative one.
#
# Set COMPILE_SLANG_SHADER_ROOT before calling to compile shaders from a
# different source tree (used when a project is consumed via FetchContent);
# it defaults to <project>/shaders.
function(compile_slang_shaders target output_dir)
    if(NOT SLANGC_EXECUTABLE)
        message(FATAL_ERROR "SLANGC_EXECUTABLE is not set. Include VulkanSDK.cmake before CompileShaders.cmake.")
    endif()

    if(ARGC LESS 3)
        message(FATAL_ERROR "compile_slang_shaders(target output_dir shader1 shader2 ...) requires at least one shader.")
    endif()

    if(DEFINED COMPILE_SLANG_SHADER_ROOT)
        set(_shader_root "${COMPILE_SLANG_SHADER_ROOT}")
    else()
        set(_shader_root "${CMAKE_SOURCE_DIR}/shaders")
    endif()

    # Build the list of extra -I flags from COMPILE_SLANG_EXTRA_INCLUDE_DIRS.
    # Consumers (Hyperion, Theia) set this to HARMONIA_SHADER_SOURCE_DIR so that
    # shared Slang modules (math, bsdf, env, env_sample, …) resolve to Harmonia's
    # source tree rather than requiring per-renderer copies.
    set(_extra_include_flags)
    if(DEFINED COMPILE_SLANG_EXTRA_INCLUDE_DIRS)
        foreach(_extra_dir IN LISTS COMPILE_SLANG_EXTRA_INCLUDE_DIRS)
            list(APPEND _extra_include_flags "-I" "${_extra_dir}")
        endforeach()
    endif()

    # Non-entry support modules recompile every entry shader when they change.
    # Scan both the local shader root and any extra include dirs.
    set(_support_shaders)
    foreach(_dir "${_shader_root}" ${COMPILE_SLANG_EXTRA_INCLUDE_DIRS})
        foreach(_support common.slang math.slang rng.slang bsdf_shared.slang env.slang env_sample.slang)
            if(EXISTS "${_dir}/${_support}")
                list(APPEND _support_shaders "${_dir}/${_support}")
            endif()
        endforeach()
    endforeach()

    set(_outputs)

    foreach(_shader IN LISTS ARGN)
        if(IS_ABSOLUTE "${_shader}")
            set(_input_shader "${_shader}")
        else()
            set(_input_shader "${_shader_root}/${_shader}")
        endif()

        # Strip only the trailing ".slang" so multi-dot stage names survive
        # (forward_render.task.slang → forward_render.task.spv).
        get_filename_component(_shader_name "${_input_shader}" NAME)
        string(REGEX REPLACE "\\.slang$" "" _shader_name_we "${_shader_name}")
        set(_output_shader "${output_dir}/${_shader_name_we}.spv")

        add_custom_command(
            OUTPUT "${_output_shader}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
            COMMAND "${SLANGC_EXECUTABLE}" "${_input_shader}"
                -target spirv
                -profile spirv_1_6
                -g0 -O2
                -I "${_shader_root}"
                ${_extra_include_flags}
                -o "${_output_shader}"
                # 31000: [[vk::combinedImageSampler]] is HLSL/DXC syntax; Slang 2026.x does not
                #        recognise it as a named attribute but still emits correct combined-image-
                #        sampler SPIR-V.  The pairing is intentional — suppress the noise.
                # 39001: explicit binding overlap is expected and correct for combined image
                #        samplers where Texture2D and SamplerState share the same binding slot.
                # 41012: Slang auto-promotes the effective profile to include standard SPIR-V
                #        extensions (spvImageQuery, spvDerivativeControl, …) from its runtime.
                #        The upgrade is harmless — suppress the informational diagnostic.
                -warnings-disable 31000,39001,41012
            DEPENDS
                "${_input_shader}"
                ${_support_shaders}
            COMMENT "Compiling Slang shader ${_shader_name}"
            VERBATIM
            COMMAND_EXPAND_LISTS
        )

        list(APPEND _outputs "${_output_shader}")
    endforeach()

    add_custom_target(${target} DEPENDS ${_outputs})
    set(${target}_OUTPUTS "${_outputs}" PARENT_SCOPE)
endfunction()
