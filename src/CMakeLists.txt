include(FetchContent)

# Pinned so a build cannot break at an unintended moment.  build.bash reads this value too.
set(OVBASE_COMMIT_HASH "faa1fdd631062fcc9727c8f28f5877cc9869752a")
FetchContent_Declare(ovbase
  GIT_REPOSITORY https://github.com/oov/ovbase.git
  GIT_TAG ${OVBASE_COMMIT_HASH}
  GIT_SHALLOW OFF
)
# ovbase builds its own clang-format target; we format our own sources below instead.
set(FORMAT_SOURCES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ovbase)

# Pinned the same way as ovbase, for the settings file reader.
set(YYJSON_COMMIT_HASH "0.12.0")
set(YYJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YYJSON_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(YYJSON_ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)
set(YYJSON_WARNING_LEVEL_THRESHOLD info CACHE STRING "" FORCE)
FetchContent_Declare(yyjson
  GIT_REPOSITORY https://github.com/ibireme/yyjson.git
  GIT_TAG ${YYJSON_COMMIT_HASH}
)
FetchContent_MakeAvailable(yyjson)

# yyjson is one big translation unit.  Without per function sections the linker can only
# keep or drop all of it, and this project only reads a settings file with it: the writer,
# the mutable API and everything else would ride along.  With these sections --gc-sections
# drops what nothing calls.
target_compile_options(yyjson PRIVATE -ffunction-sections -fdata-sections)
# The settings file is plain JSON and numbers are integers at most: the JSON5, comment and
# NaN/Infinity handling and the hand written floating point conversion are dead weight
# here.  Speed of the conversion does not matter for one small file at start-up.
target_compile_definitions(yyjson PRIVATE YYJSON_DISABLE_NON_STANDARD YYJSON_DISABLE_FAST_FP_CONV)

option(ADC_FORMAT_SOURCES "execute clang-format on our sources" ON)
if(ADC_FORMAT_SOURCES)
  file(GLOB_RECURSE adc_sources LIST_DIRECTORIES false CONFIGURE_DEPENDS "*.h" "*.c")
  find_program(CLANG_FORMAT_EXE clang-format)
  if(CLANG_FORMAT_EXE)
    add_custom_target(${PROJECT_NAME}-format ALL
      COMMAND ${CLANG_FORMAT_EXE} -style=file -i ${adc_sources}
    )
  endif()
endif()

add_library(adc_intf INTERFACE)
target_compile_definitions(adc_intf INTERFACE
  _WIN32
  _WIN32_WINNT=0x0A00
  WINVER=0x0A00
  UNICODE
  _UNICODE
  $<$<CONFIG:Release>:NDEBUG>
)
target_compile_options(adc_intf INTERFACE
  -march=x86-64-v2
  -mstackrealign
  -Wall
  -Wextra
  -Werror
  -Weverything
  -Wshadow
  -Werror=return-type
  -pedantic-errors
  -Wno-declaration-after-statement
  -Wno-padded
  -Wno-used-but-marked-unused
  -Wno-unsafe-buffer-usage
  -Wno-switch-default
  -Wno-pre-c11-compat
  -Wno-c++-keyword
  -ffunction-sections
  -fdata-sections
)
target_link_options(adc_intf INTERFACE
  -fuse-ld=lld
  -Wl,--gc-sections
  -Wl,--kill-at
  -static
  $<$<CONFIG:Release>:-s>
)

# Everything that is not the window itself, so that the tests can reach it.
add_library(adc_core STATIC
  culprit.c
  layout.c
  settings.c
  logger.c
  paths.c
  mixer.c
  process.c
  device.c
  logger.c
  loggate.c
  options.c
  removal.c
  repair.c
  task.c
)
# ovbase reaches our public headers, so it must be PUBLIC.
target_link_libraries(adc_core PUBLIC ovbase)
target_link_libraries(adc_core PRIVATE
  adc_intf
  yyjson
  cfgmgr32
  advapi32
  wevtapi
  ntdll
  shell32
  ole32
  oleaut32
  taskschd
  comctl32
)
target_include_directories(adc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The translations are compiled into the executable as a resource: langs.csv lists the
# languages, i18n_rc.cmake runs msgfmt over every *.po.DO_NOT_EDIT and writes the .rc that
# embeds the resulting .mo files.  See AGENTS.md and src/i18n/.
set(adc_lang_csv "${CMAKE_CURRENT_SOURCE_DIR}/i18n/langs.csv")
file(READ "${adc_lang_csv}" adc_langs)
string(STRIP "${adc_langs}" adc_langs)
string(REPLACE "
" ";" adc_lang_lines "${adc_langs}")
set(adc_po_files "")
foreach(adc_line IN LISTS adc_lang_lines)
  if(adc_line MATCHES "^#.*$|^([^,]+),$")
    continue()
  endif()
  if(adc_line MATCHES "^([^,]+),([^,]+)$")
    list(APPEND adc_po_files "${CMAKE_CURRENT_SOURCE_DIR}/i18n/${CMAKE_MATCH_1}.po.DO_NOT_EDIT")
  else()
    message(FATAL_ERROR "invalid language definition: ${adc_line}")
  endif()
endforeach()
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc"
  COMMAND
    "${CMAKE_COMMAND}"
    -Doutput_dir="${CMAKE_CURRENT_BINARY_DIR}"
    -Drctmpl="${CMAKE_CURRENT_SOURCE_DIR}/res/i18n.rc.tmpl"
    -P "${PROJECT_SOURCE_DIR}/cmake/i18n_rc.cmake"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/i18n.rc.tmpl"
    ${adc_po_files}
)
add_custom_target(${PROJECT_NAME}_generate_i18n_rc DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc")

add_executable(adc
  app.rc
  "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc"
  main.c
  ui.c
  ui_blockers_window.c
  ui_progress_window.c
  theme.c
  theme_button.c
  theme_header.c
)
# The rc compiler picks the manifest by this define, so the debug build starts with the
# ordinary token and the release one asks for the administrator (see app.rc).
target_compile_definitions(adc PRIVATE $<$<CONFIG:Debug>:ADC_DEBUG_TOKEN>)
# The RC compiler reports no dependencies of its own, so a change of a file the script
# names would not reach the object file.  These are declared by hand.
set_property(SOURCE app.rc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/app.debug.manifest"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/resource.h"
)
# The folder the program is built into is also the folder its documents are built into, so
# the two are named together and cannot drift apart.
set(adc_bin_dir "${CMAKE_BINARY_DIR}/bin")
set_target_properties(adc PROPERTIES
  OUTPUT_NAME audient-device-closer
  RUNTIME_OUTPUT_DIRECTORY "${adc_bin_dir}"
)
# The program reads its settings next to the executable, and a build leaves the sample there.
# It is written over every time: this is the directory a developer runs the program from, not
# an installed copy, and a sample that keeps the values of an earlier build hides a change of
# the settings from the person who starts it.
add_custom_command(TARGET adc POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -D "src=${PROJECT_SOURCE_DIR}/audient-device-closer.example.json"
          -D "dst=$<TARGET_FILE_DIR:adc>/audient-device-closer.json"
          -P "${PROJECT_SOURCE_DIR}/cmake/copy-settings.cmake"
  VERBATIM
)
# The readmes describe the program that sits in this folder, so a build leaves them beside
# it: a folder taken away from a build explains itself.  This is a rule of its own rather
# than a post build step of the program, so that a changed readme is copied without relinking
# the program and the program is relinked without copying an unchanged readme.
set(adc_readmes "README.md" "README-jp.md")
set(adc_readme_inputs "")
set(adc_readme_outputs "")
foreach(adc_readme IN LISTS adc_readmes)
  list(APPEND adc_readme_inputs "${PROJECT_SOURCE_DIR}/${adc_readme}")
  list(APPEND adc_readme_outputs "${adc_bin_dir}/${adc_readme}")
endforeach()
add_custom_command(
  OUTPUT ${adc_readme_outputs}
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${adc_readme_inputs} "${adc_bin_dir}/"
  DEPENDS ${adc_readme_inputs}
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_copy_readmes ALL DEPENDS ${adc_readme_outputs})
add_dependencies(adc ${PROJECT_NAME}_generate_i18n_rc)
target_link_options(adc PRIVATE -municode -mwindows -Wl,-utf8-codepage)
target_link_libraries(adc PRIVATE
  adc_core
  adc_intf
  comctl32
  user32
  gdi32
  uxtheme
  dwmapi
)

# The victim of the process tests.  A console stub, no ovbase.
add_executable(test_dummy test_dummy.c)
target_link_libraries(test_dummy PRIVATE adc_intf)

# The window of the application, built from the same sources as adc.  A defect of the
# window (a control without the font of the window, for one) only shows up in a real
# window, so this test owns one and checks it.  It runs without an elevation on purpose:
# that is what app.debug.manifest makes possible for the debug builds.
add_executable(test_ui
  ui_test.c
  ui.c
  ui_blockers_window.c
  ui_progress_window.c
  theme.c
  theme_button.c
  theme_header.c
  app.rc
  "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc"
)
set_property(SOURCE app.rc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/app.debug.manifest"
  "${CMAKE_CURRENT_SOURCE_DIR}/res/resource.h"
)
# A test harness must never ask for an elevation: it has to start from an ordinary shell
# and from ctest, in every configuration.  Without this the debug test carried the release
# manifest and could not be started at all, which is how this was found.
target_compile_definitions(test_ui PRIVATE ADC_DEBUG_TOKEN)
target_link_libraries(test_ui PRIVATE adc_core adc_intf comctl32 user32 gdi32 uxtheme dwmapi)
add_dependencies(test_ui ${PROJECT_NAME}_generate_i18n_rc)
add_test(NAME ui COMMAND test_ui)

# Unit tests: the pure decision logic of each module, no real device needed.  A test that
# writes a file next to itself (logger, mixer) cleans up after itself.
foreach(adc_test device removal culprit process mixer settings repair options layout loggate logger paths task)
  add_executable(test_${adc_test} ${adc_test}_test.c)
  target_link_libraries(test_${adc_test} PRIVATE adc_core ovbase adc_intf)
  add_test(NAME ${adc_test} COMMAND test_${adc_test})
endforeach()

# clang-format rewrites the sources, so it has to finish before anything compiles them.
# Every target that compiles one of our sources is listed here.
if(TARGET ${PROJECT_NAME}-format)
  foreach(adc_target adc_core adc test_dummy)
    add_dependencies(${adc_target} ${PROJECT_NAME}-format)
  endforeach()
  foreach(adc_test device removal culprit process mixer settings repair options layout loggate logger paths task)
    add_dependencies(test_${adc_test} ${PROJECT_NAME}-format)
  endforeach()
  add_dependencies(test_ui ${PROJECT_NAME}-format)
endif()
