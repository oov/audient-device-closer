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
  async.c
  veto_event.c
  veto_log_manager.c
  real/veto_log.c
  clock_manager.c
  real/clock.c
  blocker.c
  layout.c
  settings.c
  logger.c
  paths.c
  install.c
  process_manager.c
  real/process.c
  device_probe.c
  device_manager.c
  real/device.c
  loggate.c
  options.c
  removal_manager.c
  real/removal.c
  repair.c
  task.c
  instance.c
)
# ovbase reaches our public headers, so it must be PUBLIC.
target_link_libraries(adc_core PUBLIC ovbase)
target_link_libraries(adc_core PRIVATE
  adc_intf
  yyjson
  cfgmgr32
  ksguid
  advapi32
  wevtapi
  shell32
  shlwapi
  ole32
  uuid
  oleaut32
  taskschd
  comctl32
)
target_include_directories(adc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# The debug build carries the stand-ins, and the managers name them through the select
# functions, which exist in this configuration of the core as well.
target_compile_definitions(adc_core PRIVATE $<$<CONFIG:Debug>:ADC_MOCK>)

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
  ui_device_picker.c
  ui_progress_window.c
  theme.c
  theme_button.c
  theme_combo.c
  theme_header.c
)
# The rc compiler picks the manifest by this define, so the debug build starts with the
# ordinary token and the release one asks for the administrator (see app.rc).
target_compile_definitions(adc PRIVATE $<$<CONFIG:Debug>:ADC_DEBUG_TOKEN>)
# A debug build can be asked, through the settings file, to walk the whole way a repair goes
# against a made-up device fault instead of the machine.  The stand-ins are part of that
# build alone: a build that ships has no way to be told the machine is not what it is.
target_sources(adc PRIVATE
  $<$<CONFIG:Debug>:mock/device.c>
  $<$<CONFIG:Debug>:mock/removal.c>
  $<$<CONFIG:Debug>:mock/veto_log.c>
  $<$<CONFIG:Debug>:mock/process.c>
  $<$<CONFIG:Debug>:mock/clock.c>
  $<$<CONFIG:Debug>:simulate.c>
)
target_compile_definitions(adc PRIVATE $<$<CONFIG:Debug>:ADC_MOCK>)
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
  COMMAND "${CMAKE_COMMAND}" -D "src=${PROJECT_SOURCE_DIR}/audient-device-closer.json"
          -D "dst=$<TARGET_FILE_DIR:adc>/audient-device-closer.json"
          -P "${PROJECT_SOURCE_DIR}/cmake/copy-settings.cmake"
  VERBATIM
)
# The readmes, the license and the agreement describe the program that sits in this folder,
# so a build leaves them beside it: a folder taken away from a build explains itself.  This is
# a rule of its own rather than a post build step of the program, so that a changed document
# is copied without relinking the program and the program is relinked without copying an
# unchanged document.
set(adc_readmes "README.md" "README-jp.md" "LICENSE")
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
  ui_blockers_window.c
  ui_progress_window.c
  ui_device_picker.c
  theme.c
  theme_button.c
  theme_combo.c
  theme_header.c
  theme_button.c
  theme_combo.c
  theme_header.c
  test_suppress.c
  app.rc
  "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc"
)
# The geometry of the main window is computed in ui.c, which this test compiles itself, and
# only the measuring every window shares arrives through adc_core.
set_property(TARGET test_ui APPEND PROPERTY LINK_LIBRARIES)
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

# clang-format rewrites the sources, so it has to finish before anything compiles them.
# Every target that compiles one of our sources belongs here; the unit tests add themselves
# in adc_add_unit_test below.
if(TARGET ${PROJECT_NAME}-format)
  foreach(adc_target adc_core adc test_dummy test_ui)
    add_dependencies(${adc_target} ${PROJECT_NAME}-format)
  endforeach()
endif()

# Unit tests: the decision logic of each module, without a real device.  A test compiles the
# module it tests itself, so it names the sources that module needs; a test that has nothing
# to name links adc_core, which already holds every module.  A test that writes a file next
# to itself (logger) cleans up after itself.
#
# The keyword arguments are the ones target_link_libraries and target_compile_definitions
# take, so the two stand out from each other and from the sources.  TEST names the file of the
# test when it does not sit next to this one (the machines under real/ and mock/ carry their
# own).
function(adc_add_unit_test name)
  cmake_parse_arguments(ARG "" "TEST" "SOURCES;LIBS;DEFINES" ${ARGN})
  if(NOT ARG_LIBS)
    set(ARG_LIBS adc_core)
  endif()
  if(ARG_TEST)
    set(ARG_TEST_FILE ${ARG_TEST})
  else()
    set(ARG_TEST_FILE ${name}_test.c)
  endif()
  add_executable(test_${name} ${ARG_TEST_FILE} ${ARG_SOURCES})
  target_link_libraries(test_${name} PRIVATE ovbase adc_intf ${ARG_LIBS})
  # The sources of a test live under real/ and mock/ as often as they do here, so the folder
  # that holds the headers they reach for comes along.
  target_include_directories(test_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  if(ARG_DEFINES)
    target_compile_definitions(test_${name} PRIVATE ${ARG_DEFINES})
  endif()
  # clang-format rewrites the sources, so it has to finish before anything compiles them.
  if(TARGET ${PROJECT_NAME}-format)
    add_dependencies(test_${name} ${PROJECT_NAME}-format)
  endif()
  add_test(NAME ${name} COMMAND test_${name})
endfunction()

# device_probe.c answers through the machine, so the manager and the machines it can ask
# come along.
adc_add_unit_test(device_probe SOURCES async.c device_manager.c real/device.c mock/device.c LIBS cfgmgr32 ksguid DEFINES ADC_MOCK)
adc_add_unit_test(removal SOURCES async.c removal_manager.c real/removal.c mock/removal.c LIBS cfgmgr32 DEFINES ADC_MOCK)
# The machine of the system has a test of its own, because the code that runs before the
# removal call is worth checking without an elevation.  It compiles the machine itself, so the
# manager that would name it does not come along.
adc_add_unit_test(real_removal SOURCES async.c LIBS cfgmgr32 TEST real/removal_test.c)
adc_add_unit_test(veto_event SOURCES async.c veto_log_manager.c real/veto_log.c mock/veto_log.c LIBS wevtapi DEFINES ADC_MOCK)
adc_add_unit_test(blocker SOURCES process_manager.c mock/process.c DEFINES ADC_MOCK)
# The machine of the system has a test of its own, because ending a process is worth checking
# without a stand-in.  It compiles the machine itself, so the manager that would name it does
# not come along.
adc_add_unit_test(real_process SOURCES async.c logger.c paths.c install.c loggate.c clock_manager.c real/clock.c LIBS "advapi32;shlwapi" TEST real/process_test.c)
adc_add_unit_test(settings SOURCES paths.c loggate.c clock_manager.c real/clock.c LIBS "yyjson;shlwapi")
adc_add_unit_test(logger SOURCES paths.c install.c loggate.c clock_manager.c real/clock.c LIBS "yyjson;shlwapi;advapi32")
adc_add_unit_test(install SOURCES paths.c install.c LIBS "advapi32;shlwapi")
adc_add_unit_test(task SOURCES paths.c install.c LIBS "ole32;oleaut32;uuid;taskschd;shlwapi;advapi32")

# The modules whose tests compile them and need nothing else in particular.
adc_add_unit_test(async)
adc_add_unit_test(repair SOURCES
  clock_manager.c device_manager.c process_manager.c removal_manager.c veto_log_manager.c
  mock/device.c mock/removal.c mock/veto_log.c mock/process.c mock/clock.c simulate.c DEFINES ADC_MOCK)
adc_add_unit_test(options)
# What every window measures with: the scaling, the frame the window manager is asked for
# and the font.  A window's own geometry is checked in that window's test.
adc_add_unit_test(layout)
# The theme puts windows of its own up: the buttons, the combo boxes and the headers it
# subclasses come along, and the measuring they share arrives through adc_core.
adc_add_unit_test(
  theme SOURCES theme_button.c theme_combo.c theme_header.c test_suppress.c LIBS "adc_core;comctl32;user32;gdi32;uxtheme;dwmapi" DEFINES ADC_DEBUG_TOKEN)
# The windows this test makes carry the common controls the program asks for, and that request
# is part of the manifest: the test is built with the same one the program is built with.
target_sources(test_theme PRIVATE app.rc "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc")
add_dependencies(test_theme ${PROJECT_NAME}_generate_i18n_rc)
# The picker draws with the palette and the geometry of the program, so the theme comes along;
# the windows it makes carry the common controls the manifest asks for.
adc_add_unit_test(
  ui_device_picker SOURCES theme.c theme_button.c theme_combo.c theme_header.c test_suppress.c
  LIBS "adc_core;comctl32;user32;gdi32;uxtheme;dwmapi" DEFINES ADC_DEBUG_TOKEN)
target_sources(test_ui_device_picker PRIVATE app.rc "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc")
add_dependencies(test_ui_device_picker ${PROJECT_NAME}_generate_i18n_rc)
# The progress window draws with the palette, so the theme comes along; the windows it makes
# carry the common controls the manifest asks for.
adc_add_unit_test(
  ui_progress_window SOURCES theme.c theme_button.c theme_combo.c theme_header.c test_suppress.c
  LIBS "adc_core;comctl32;user32;gdi32;uxtheme;dwmapi" DEFINES ADC_DEBUG_TOKEN)
target_sources(test_ui_progress_window PRIVATE app.rc "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc")
add_dependencies(test_ui_progress_window ${PROJECT_NAME}_generate_i18n_rc)
# The blockers window is a window of its own -- its geometry, its columns and its theme live
# in it -- so the test makes one against a window of its own; the windows carry the common
# controls the manifest asks for.
adc_add_unit_test(
  ui_blockers_window SOURCES theme.c theme_button.c theme_combo.c theme_header.c test_suppress.c
  LIBS "adc_core;comctl32;user32;gdi32;uxtheme;dwmapi" DEFINES ADC_DEBUG_TOKEN)
target_sources(test_ui_blockers_window PRIVATE app.rc "${CMAKE_CURRENT_BINARY_DIR}/i18n.rc")
add_dependencies(test_ui_blockers_window ${PROJECT_NAME}_generate_i18n_rc)
adc_add_unit_test(loggate)
adc_add_unit_test(paths)
adc_add_unit_test(instance)
