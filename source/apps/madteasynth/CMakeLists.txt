cmake_minimum_required(VERSION 3.14)
project(madteasynth VERSION 1.0.0)

# Normalised to forward slashes: CMake treats \U as an escape.
if(NOT DEFINED IPLUG2_DIR)
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/iplug2" IPLUG2_DIR)
  set(IPLUG2_DIR "${IPLUG2_DIR}" CACHE PATH "iPlug2 root directory")
endif()

file(TO_CMAKE_PATH "${IPLUG2_DIR}" IPLUG2_DIR)

include(${IPLUG2_DIR}/iPlug2.cmake)

find_package(iPlug2 REQUIRED)

# Legacy Indexynt backend, off by default. Always defined as 0 or 1 so `#if`
# reads the same everywhere.
option(STEEPSYNTH_ENABLE_INDEXYNT "Build the legacy Indexynt synth backend" OFF)
if(STEEPSYNTH_ENABLE_INDEXYNT)
  set(STEEPSYNTH_INDEXYNT_DEFINE STEEPSYNTH_ENABLE_INDEXYNT=1)
else()
  set(STEEPSYNTH_INDEXYNT_DEFINE STEEPSYNTH_ENABLE_INDEXYNT=0)
endif()
message(STATUS "Indexynt backend: ${STEEPSYNTH_ENABLE_INDEXYNT}")

# Ceiling for the preview, which follows the selected export target. 1 MB of
# BSS; also the settings page's win32 size limit, which must not exceed it or
# exported sizes would be clamped in the preview.
add_compile_definitions(VSCREEN_MAX_W=1024 VSCREEN_MAX_H=1024)

# UI library sources
set(UI_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/textmode.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/textmode_ui.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/textmode_ui_textarea.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/ui_glue.c
)

set(UI_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/textmode.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/textmode_cell.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/textmode_ui.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/textmode_ui_textarea.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/ui_glue.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../ui/ui_scrubber.h
)

set(FONT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/font_cache.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/gen_font.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/glyph_render.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/render_lowspec.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/render_ctx.c
)

set(FONT_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/font_cache.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/gen_font.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/glyph_render.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/render_lowspec.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/render_ctx.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/font_assembly_line.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/font_assembly_line_lowspec.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/font_idealist_hacker_mono.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/font_idealist_hacker_mono_lowspec.h
)

# RS_PEBBLE_TIME2 for render_lowspec.c only: the vscreen is GColor8, while
# glyph_render.c's UI tiles must stay 8-bit gray.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/render_lowspec.c
  PROPERTIES COMPILE_DEFINITIONS "RS_PEBBLE_TIME2=1"
)

set(COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/string_pure.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/string_view.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/array.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/arena.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/math_pure.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/math_fixedp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/intern.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/token_iterator.c
)

set(COMMON_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/tsys.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/string_pure.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/string_view.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/array.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/arena.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/intern.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/math_pure.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/math_fixedp.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../common/token_iterator.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/platform.h
)

# VM + parser; vm.c #includes vm_run.c itself.
set(VM_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_emit_c.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_emit_curlywas.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_emit_script.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../parser/parser.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../parser/tokens.c
)

set(VM_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_types.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_run.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_config.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_emit_c.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_emit_curlywas.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../vm/vm_emit_shared.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../parser/parser.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../../parser/tokens.h
)

# interactive_coding_wrap.c alone, for non-UI consumers. interactive_coding.c
# #includes it, so use one list or the other, never both.
set(IC_WRAP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../interactive_coding/interactive_coding_wrap.c
)

set(IC_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../interactive_coding/interactive_coding.c
)

set(IC_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/../../interactive_coding/interactive_coding.h
)

set(CODESYNTH_SOURCES
  codesynth/CodeSynthParse.cpp
  codesynth/CompressSequence.cpp
  codesynth/SongExport.cpp
  codesynth/ExportTemplate.cpp
  codesynth/ExportProject.cpp
  codesynth/SongSeq.cpp
  codesynth/tinywav.c
  codesynth/tinywavwrap.c
  codesynth/sample_loader.c
  codesynth/vscreen_vm_bind.c
)

set(CODESYNTH_HEADERS
  codesynth/CodeSynthParse.h
  codesynth/CompressSequence.h
  codesynth/SongExport.h
  codesynth/ExportTemplate.h
  codesynth/ExportProject.h
  codesynth/SongSeq.h
  codesynth/tinywav.h
  codesynth/tinywavwrap.h
  codesynth/sample_loader.h
  codesynth/song_dsp.h
  codesynth/CodeSynthVmCtx.h
  codesynth/vscreen_vm_bind.h
)

# song_dsp.h embedded as a string, so exported song.c inlines the code live
# playback runs.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/codesynth/song_dsp.h)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/codesynth/song_dsp.h SONG_DSP_H_CONTENT)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/codesynth/song_dsp_embedded.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/song_dsp_embedded.h
  @ONLY)

# The export template, walked from an #include graph and chunked for MSVC's
# 16380-byte literal cap. Generated into the binary dir so no stray copy beside
# ExportTemplate.cpp can shadow it.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/codesynth/tools/gen_template_embed.py
          --root     ${CMAKE_CURRENT_SOURCE_DIR}/../..
          --manifest ${CMAKE_CURRENT_SOURCE_DIR}/codesynth/templates/manifest.txt
          --out      ${CMAKE_CURRENT_BINARY_DIR}/export_template_embedded.h
          --deps     ${CMAKE_CURRENT_BINARY_DIR}/export_template_deps.txt
  RESULT_VARIABLE EXPORT_TEMPLATE_RESULT
  OUTPUT_VARIABLE EXPORT_TEMPLATE_OUTPUT
  ERROR_VARIABLE  EXPORT_TEMPLATE_ERROR)
if(NOT EXPORT_TEMPLATE_RESULT EQUAL 0)
  message(FATAL_ERROR "gen_template_embed.py failed:\n${EXPORT_TEMPLATE_ERROR}")
endif()
message(STATUS "${EXPORT_TEMPLATE_OUTPUT}")

# Reconfigure when any embedded source changes; the script reports the closure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/codesynth/templates/manifest.txt
  ${CMAKE_CURRENT_SOURCE_DIR}/codesynth/tools/gen_template_embed.py)
file(STRINGS ${CMAKE_CURRENT_BINARY_DIR}/export_template_deps.txt EXPORT_TEMPLATE_DEPS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EXPORT_TEMPLATE_DEPS})

set(VSCREEN_SOURCES
  vscreen.c
)

set(VSCREEN_HEADERS
  vscreen.h
)

# png2span.c carries its own stb_image, STB_IMAGE_STATIC to avoid nanovg.c's copy.
set(PNG2SPAN_SOURCES
  png2span.c
)

set(PNG2SPAN_HEADERS
  png2span.h
  stb_image.h
)

# CodeSynth.cpp is #included by SteepSynthEngine.cpp; listing it here would
# compile it twice.

# Override deploy paths to system-wide locations.
set(IPLUG_VST3_DEPLOY_PATH "C:/Program Files/Common Files/VST3" CACHE PATH "" FORCE)
set(IPLUG_CLAP_DEPLOY_PATH "$ENV{LOCALAPPDATA}/Programs/Common/CLAP" CACHE PATH "" FORCE)

# Include path so ui/, common/, font/ find each other.
add_library(madteasynth_ui_includes INTERFACE)
target_include_directories(madteasynth_ui_includes INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
  ${CMAKE_CURRENT_BINARY_DIR})   # for the generated song_dsp_embedded.h

# Build VST3 + CLAP formats.
# FORMATS VST3 CLAP
iplug_add_plugin(${PROJECT_NAME}
  FORMATS APP VST3 CLAP
  DEFINES
    VST3_NUM_CC_CHANS=0
    VST3_NUM_MIDI_IN_CHANS=0
    ${STEEPSYNTH_INDEXYNT_DEFINE}
  SOURCES
    steepsynth.cpp
    steepsynth.h
    SteepSynthControl.cpp
    SteepSynthControl.h
    SteepSynthEngine.cpp
    SteepSynthEngine.h
    tdPlugin.h
    PlatformClipboard.cpp
    PlatformFolderPicker.cpp
    PlatformKeyFocus.cpp
    resources/resource.h
    ${CODESYNTH_SOURCES}
    ${CODESYNTH_HEADERS}
    ${VSCREEN_SOURCES}
    ${VSCREEN_HEADERS}
    ${PNG2SPAN_SOURCES}
    ${PNG2SPAN_HEADERS}
    ${UI_SOURCES}
    ${UI_HEADERS}
    ${FONT_SOURCES}
    ${FONT_HEADERS}
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
    ${VM_SOURCES}
    ${VM_HEADERS}
    ${IC_SOURCES}
    ${IC_HEADERS}
  LINK
    madteasynth_ui_includes
  RESOURCES
    resources/fonts/Roboto-Regular.ttf
)

# codesynth_export: the standalone "Save C Song", without iPlug2, the engine or
# the UI.
add_executable(codesynth_export
  tools/codesynth_export.cpp
  ${CODESYNTH_SOURCES}
  ${CODESYNTH_HEADERS}
  ${VSCREEN_SOURCES}
  ${VSCREEN_HEADERS}
  ${COMMON_SOURCES}
  ${COMMON_HEADERS}
  ${VM_SOURCES}
  ${VM_HEADERS}
  ${IC_WRAP_SOURCES}
  ${IC_HEADERS}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/render_lowspec.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../font/render_ctx.c
)
target_link_libraries(codesynth_export PRIVATE madteasynth_ui_includes)
target_compile_definitions(codesynth_export PRIVATE ${STEEPSYNTH_INDEXYNT_DEFINE})
# The codesynth sources include each other relative to this directory.
target_include_directories(codesynth_export PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(codesynth_export PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/out"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
)

# APP output goes to build/out/; VST3 and CLAP deploy to the standard paths.
if(WIN32)
  if(TARGET ${PROJECT_NAME}-app)
    set_target_properties(${PROJECT_NAME}-app PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
      LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/out"
      LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
      LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/out"
      LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/out"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
      RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/out"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
    )
  endif()
endif()
