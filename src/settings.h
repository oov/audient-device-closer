#pragma once

#include <ovbase.h>

#include <stddef.h>

#include "device.h"

/**
 * @brief Everything that differs between machines lives in the JSON file, not in the code
 */
struct settings {
  char *ks_service;                  // required: DEVPKEY_Device_Service of the KS devnode
  struct device_entry *devices;      // the devices this program supports: name and identifier pairs
  char **mixer_process_names;        // required: image names of the mixers this program may close
  char *veto_log_name;               // required: event log holding the veto record
  char *veto_provider_name;          // required: provider name of that record
  unsigned long veto_event_id;       // required: event id of that record
  char *task_name;                   // optional: task path used for the Task Scheduler entry
  unsigned long auto_run_delay_ms;   // optional: delay before probing in auto-run mode, milliseconds
  unsigned long confirm_max_wait_ms; // optional: how long the confirmation waits for the device to come back
  bool use_theme;                    // optional: draw the windows with the theme of this program, default true
  bool allow_audio_service_pause;    // optional: let this tool pause the Windows Audio service, default false
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

/**
 * @brief The mixer image names the settings carry, read only
 *
 * @param s the settings to read from, may be NULL
 * @return the names as a flat read-only array, NULL when there is none
 */
char const *const *settings_mixer_names(struct settings const *const s);

/**
 * @brief How many mixer image names the settings carry
 *
 * @param s the settings to read from, may be NULL
 * @return the count, 0 when there is none
 */
size_t settings_mixer_name_count(struct settings const *const s);
