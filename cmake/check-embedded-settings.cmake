# The minimal PowerShell script carries a copy of audient-device-closer.json inside itself so
# that it can be handed around as one file.  A copy that has drifted from the file would
# release other devices than the program does, without saying so, so a build compares the two
# and stops when they differ.
#
# The copy is the whole text of the file pasted between two markers, so the comparison is of
# the text and not of the values: a difference of a value the program would read the same way
# (indentation, order of keys) is a copy that was never made, and the message asks for it.
if(NOT DEFINED script OR NOT DEFINED settings)
  message(FATAL_ERROR "check-embedded-settings.cmake needs -Dscript=... -Dsettings=...")
endif()

set(adc_begin_marker "$EmbeddedSettings = @'")
set(adc_end_marker "\n'@\n")

file(READ "${script}" adc_script_text)
file(READ "${settings}" adc_settings_text)

# A checkout may carry either line ending; the comparison is of the settings, not of them.
string(REPLACE "\r\n" "\n" adc_script_text "${adc_script_text}")
string(REPLACE "\r\n" "\n" adc_settings_text "${adc_settings_text}")

string(FIND "${adc_script_text}" "${adc_begin_marker}" adc_begin)
if(adc_begin EQUAL -1)
  message(FATAL_ERROR "${script} has no \"$EmbeddedSettings = @'\" line")
endif()
string(LENGTH "${adc_begin_marker}" adc_begin_length)
math(EXPR adc_body_begin "${adc_begin} + ${adc_begin_length} + 1")
string(SUBSTRING "${adc_script_text}" ${adc_body_begin} -1 adc_after_begin)

string(FIND "${adc_after_begin}" "${adc_end_marker}" adc_end)
if(adc_end EQUAL -1)
  message(FATAL_ERROR "${script} ends the settings it carries with no \"'@\" line")
endif()
string(SUBSTRING "${adc_after_begin}" 0 ${adc_end} adc_embedded)

# The newline the file ends with cannot sit before the marker, so it is not a difference.
string(REGEX REPLACE "\n$" "" adc_settings_text "${adc_settings_text}")

if(adc_embedded STREQUAL adc_settings_text)
  return()
endif()

# Name the line that first differs, so a paste that went wrong is easy to find.  The copy is
# meant to be the text of the file, so the line is the same one in both.
string(LENGTH "${adc_embedded}" adc_embedded_length)
string(LENGTH "${adc_settings_text}" adc_settings_length)
set(adc_limit "${adc_embedded_length}")
if(adc_settings_length LESS adc_limit)
  set(adc_limit "${adc_settings_length}")
endif()
set(adc_offset 0)
set(adc_differing -1)
while(adc_offset LESS adc_limit)
  string(SUBSTRING "${adc_embedded}" ${adc_offset} 1 adc_embedded_char)
  string(SUBSTRING "${adc_settings_text}" ${adc_offset} 1 adc_settings_char)
  if(NOT adc_embedded_char STREQUAL adc_settings_char)
    set(adc_differing ${adc_offset})
    break()
  endif()
  math(EXPR adc_offset "${adc_offset} + 1")
endwhile()
if(adc_differing EQUAL -1)
  set(adc_differing "${adc_limit}")
endif()

# One of the two may have ended where the other goes on, and then there is no line to name.
if(adc_differing EQUAL adc_limit AND NOT adc_embedded_length EQUAL adc_settings_length)
  if(adc_embedded_length LESS adc_settings_length)
    message(FATAL_ERROR
      "${settings} and the copy inside ${script} differ: the copy ends where the file goes on.\n"
      "Paste the whole text of ${settings} between the markers of ${script}."
    )
  else()
    message(FATAL_ERROR
      "${settings} and the copy inside ${script} differ: the copy goes on where the file ends.\n"
      "Paste the whole text of ${settings} between the markers of ${script}."
    )
  endif()
endif()

string(SUBSTRING "${adc_embedded}" 0 ${adc_differing} adc_head)
string(FIND "${adc_head}" "\n" adc_last_newline REVERSE)
if(adc_last_newline EQUAL -1)
  set(adc_line_head "")
  set(adc_line 1)
else()
  math(EXPR adc_line_head_begin "${adc_last_newline} + 1")
  string(SUBSTRING "${adc_head}" ${adc_line_head_begin} -1 adc_line_head)
  string(REGEX MATCHALL "\n" adc_newlines "${adc_head}")
  list(LENGTH adc_newlines adc_line_count)
  math(EXPR adc_line "${adc_line_count} + 1")
endif()
string(SUBSTRING "${adc_embedded}" ${adc_differing} -1 adc_embedded_tail)
string(SUBSTRING "${adc_settings_text}" ${adc_differing} -1 adc_settings_tail)
string(REGEX MATCH "^[^\n]*" adc_embedded_rest "${adc_embedded_tail}")
string(REGEX MATCH "^[^\n]*" adc_settings_rest "${adc_settings_tail}")

message(FATAL_ERROR
  "${settings} and the copy inside ${script} differ at line ${adc_line}:\n"
  "  settings: ${adc_line_head}${adc_settings_rest}\n"
  "  script:   ${adc_line_head}${adc_embedded_rest}\n"
  "Paste the whole text of ${settings} between the markers of ${script}."
)
