#pragma once

#include <ovbase.h>

#include <wchar.h>

struct device_entry;

/**
 * @brief Everything that differs between machines lives in the JSON file, not in the code
 *
 * The file is read as UTF-8, which is what a JSON file is, and what comes out of it is kept
 * as the wide text this program works with: every one of these strings is a name the system
 * is asked about or shown to a reader, and never a text this program writes out again.
 */
struct settings {
  wchar_t *ks_service;               // required: DEVPKEY_Device_Service of the KS devnode
  struct device_entry *devices;      // the devices this program supports: name and identifier pairs
  wchar_t **mixer_process_names;     // required: image names of the mixers this program may close
  wchar_t *veto_log_name;            // required: event log holding the veto record
  wchar_t *veto_provider_name;       // required: provider name of that record
  unsigned long veto_event_id;       // required: event id of that record
  wchar_t *task_dir;                 // optional: folder of the Task Scheduler library that holds the entry
  wchar_t *task_name;                // optional: name of the Task Scheduler entry
  unsigned long auto_run_delay_ms;   // optional: delay before probing in auto-run mode, milliseconds
  unsigned long confirm_max_wait_ms; // optional: how long the confirmation waits for the device to come back
  unsigned long veto_max_wait_ms;    // optional: how long a run waits for the veto record of the attempt it just made
  bool use_theme;                    // optional: draw the windows with the theme of this program, default true
  bool allow_audio_service_pause;    // optional: let this tool pause the Windows Audio service, default false

  /**
   * @brief Debug only: the made-up machine to run against instead of the one this is on
   *
   * The name is read from "simulate" and is one of these:
   *
   * - "slow_removal": every removal takes three seconds, as one on a device that is in use
   *   does on the machine.
   * - "access_denied": the removal is refused for want of the privilege, three seconds later.
   * - "unreadable": the devnodes of the service cannot be read at all.
   * - "no_device": the service has no devnode.
   *
   * NULL or an empty name, which is what a Release build always has, reads the machine this
   * program is on.
   */
  wchar_t *simulate;
};

/**
 * @brief Read the settings file and fill the settings from it
 *
 * @param path the file to read
 * @param out filled in and owned by the caller afterwards; release with settings_release
 * @param err receives the failure of the read or of the contents
 * @return false when the file could not be read or understood
 */
NODISCARD bool settings_load(wchar_t const *const path, struct settings *const out, struct ov_error *const err);

/**
 * @brief The settings file this program reads: the executable with its extension replaced
 *        by ".json", next to the executable
 *
 * @param err receives the failure of the lookup
 * @return the path, NULL when it could not be built
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *settings_default_path(struct ov_error *const err);

/**
 * @brief Release the strings the settings hold
 *
 * @param s NULL is allowed
 */
void settings_release(struct settings *const s);

/**
 * @brief The mixer image names the settings carry, read only
 *
 * @param s the settings to read from, may be NULL
 * @return the names as a flat read-only array, NULL when there is none
 */
wchar_t const *const *settings_mixer_names(struct settings const *const s);

/**
 * @brief How many mixer image names the settings carry
 *
 * @param s the settings to read from, may be NULL
 * @return the count, 0 when there is none
 */
size_t settings_mixer_name_count(struct settings const *const s);
