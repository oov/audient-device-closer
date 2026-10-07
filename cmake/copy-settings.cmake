# Puts the sample settings next to the built program.  The build output is not an installed
# copy of the program but the thing a developer runs, so the sample of the moment is what has
# to be there: a stale sample hides changes of the settings from the person running it.
if(NOT DEFINED src OR NOT DEFINED dst)
  message(FATAL_ERROR "copy-settings.cmake needs -Dsrc=... -Ddst=...")
endif()
file(COPY_FILE "${src}" "${dst}" ONLY_IF_DIFFERENT)
