cmake_minimum_required(VERSION 3.21)
project(audient-device-closer C)
enable_language(RC)
enable_testing()

add_subdirectory(src)

# The minimal PowerShell script carries a copy of the settings so that it can be handed around
# as one file, and the two release the same devices only while the copy is the one of the file.
# The check is a rule of the build rather than a step of it at configure time: a settings file
# edited after configuring has to stop the next build, not the configure after that.
set(adc_embedded_settings_stamp "${CMAKE_BINARY_DIR}/check-embedded-settings.stamp")
add_custom_command(
  OUTPUT "${adc_embedded_settings_stamp}"
  COMMAND "${CMAKE_COMMAND}"
          -D "script=${PROJECT_SOURCE_DIR}/audient-device-closer-minimal.ps1"
          -D "settings=${PROJECT_SOURCE_DIR}/audient-device-closer.json"
          -P "${PROJECT_SOURCE_DIR}/cmake/check-embedded-settings.cmake"
  COMMAND "${CMAKE_COMMAND}" -E touch "${adc_embedded_settings_stamp}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/audient-device-closer-minimal.ps1"
    "${PROJECT_SOURCE_DIR}/audient-device-closer.json"
    "${PROJECT_SOURCE_DIR}/cmake/check-embedded-settings.cmake"
  COMMENT "Checking the settings inside audient-device-closer-minimal.ps1"
  VERBATIM
)
add_custom_target(${PROJECT_NAME}_check_embedded_settings ALL DEPENDS "${adc_embedded_settings_stamp}")
