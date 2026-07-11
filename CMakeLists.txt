# DRUMku — a native VST3 MIDI drum sampler for Haiku (no framework).
#
# Builds two ways:
#   - standalone: point VST3_SDK_DIR at a Haiku-patched VST 3 SDK checkout
#     (default: ../VST3-haiku/vst3sdk) and configure this directory directly;
#   - as a subdirectory of a project that already builds that SDK (the parent
#     must provide the SDK targets and public_sdk_SOURCE_DIR).

cmake_minimum_required(VERSION 3.25.0)

project(smtg-drumku
    VERSION 0.1.0
    DESCRIPTION "DRUMku MIDI drum sampler VST3 for Haiku"
)

if(NOT TARGET sdk)
    set(VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../VST3-haiku/vst3sdk"
        CACHE PATH "Haiku-patched VST 3 SDK source tree")
    if(NOT EXISTS ${VST3_SDK_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "VST 3 SDK not found at '${VST3_SDK_DIR}' - set -DVST3_SDK_DIR=<path>")
    endif()

    # Headless SDK: core libraries plus the validator (part of the hosting
    # examples), no VSTGUI (no Haiku backend yet), no plug-in examples.
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "VSTGUI has no Haiku backend yet" FORCE)
    set(SMTG_ENABLE_WAYLAND_SUPPORT OFF CACHE BOOL "no Wayland on Haiku" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "validator" FORCE)
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "plug-in examples not needed" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "run manually instead" FORCE)
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "install manually instead" FORCE)

    add_subdirectory(${VST3_SDK_DIR} vst3sdk)

    # The SDK's plugin helper functions read these directory-scoped variables;
    # the SDK sets them only in its own scope.
    set(public_sdk_SOURCE_DIR "${VST3_SDK_DIR}/public.sdk")
    set(pluginterfaces_SOURCE_DIR "${VST3_SDK_DIR}/pluginterfaces")
endif()

# ---------------------------------------------------------------------------
# The plug-in
# ---------------------------------------------------------------------------
smtg_add_vst3plugin(DRUMku
    source/idrumloader.h
    source/drumids.h
    source/version.h
    source/wavsample.h
    source/wavsample.cpp
    source/drumvoices.h
    source/drumvoices.cpp
    source/drumprocessor.h
    source/drumprocessor.cpp
    source/drumcontroller.h
    source/drumcontroller.cpp
    source/drumfactory.cpp
)

smtg_target_configure_version_file(DRUMku)

target_compile_features(DRUMku
    PUBLIC
        cxx_std_17
)
target_compile_options(DRUMku PRIVATE -ffast-math)
target_link_libraries(DRUMku
    PRIVATE
        sdk
)
