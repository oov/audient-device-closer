#pragma once

#include <ovbase.h>

struct options;
struct settings;

/**
 * @brief Open the window and run until it closes
 *
 * @param options what this run was asked to do, it must outlive the call
 * @param settings the settings this run works for, they must outlive the call
 * @param err receives the failure of the start-up
 * @return false when the window could not be made
 */
NODISCARD bool ui_run(struct options const *const options, struct settings const *const settings, struct ov_error *const err);

/**
 * @brief Bring the window of the copy that is already running to the front
 *
 * A second start of this program has no window to show, so it reaches the one that is on
 * the screen instead: the person who pressed the icon twice gets the window they meant to
 * open, and nothing else happens.  Restoring a window that sits minimized is part of the
 * answer; failing to make it the foreground window is not a reason to say that nobody is
 * running, so it is written to the log only.
 *
 * @param err receives the failure of the search
 * @return false when no window of this program was found
 */
NODISCARD bool ui_activate_running_window(struct ov_error *const err);
