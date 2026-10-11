#pragma once

#include <ovbase.h>

#include <stddef.h>

/**
 * @brief Everything that differs between machines lives in the JSON file, not in the code
 */
struct settings {
  char *ks_service;                // required: DEVPKEY_Device_Service of the KS devnode
  char *ks_instance_prefix;        // optional: instance path prefix, NULL = no prefix filter
  char *veto_log_name;             // required: event log holding the veto record
  char *veto_provider_name;        // required: provider name of that record
  unsigned long veto_event_id;     // required: event id of that record
  char *mixer_process_name;        // required: image name to close, e.g. "iD.exe"
  char *mixer_display_name;        // optional: display name of the mixer, e.g. "iD Mixer"
  char *task_name;                 // optional: task path used for the Task Scheduler entry
  unsigned long auto_run_delay_ms; // optional: delay before probing in auto-run mode, milliseconds
  bool use_theme;                  // optional: draw the windows with the theme of this program, default true
};

/**
 * @brief Read the settings file and fill the settings from it
 *
 * @param path_utf8 the file to read, UTF-8
 * @param out filled in and owned by the caller afterwards; release with settings_release
 * @param err receives the failure of the read or of the contents
 * @return false when the file could not be read or understood
 */
bool settings_load(char const *const path_utf8, struct settings *const out, struct ov_error *const err);

/**
 * @brief The settings file this program reads: the executable with its extension replaced
 *        by ".json", next to the executable
 *
 * @param err receives the failure of the lookup
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with settings_string_free.
 */
char *settings_default_path(struct ov_error *const err);

/**
 * @brief Release the strings the settings hold
 *
 * @param s NULL is allowed
 */
void settings_release(struct settings *const s);

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void settings_string_free(char **const value);
