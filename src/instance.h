#pragma once

#include <stdbool.h>

#include <windows.h>

#include <ovbase.h>

/**
 * The single running copy of this program.
 *
 * Two copies at once would both reach the device, and the second would report a veto the
 * first one made, so a copy that finds the mutex of this program already taken brings the
 * window of the running copy to the front and goes away.  The installer watches the same
 * mutex: an uninstall that starts while a copy runs would leave the executable behind.
 *
 * The mutex lives in the session of the person who started the copy, not in Global: the
 * run that the task scheduler starts has no window to show and no person to answer, so it
 * must not stand in the way of the copy at the screen.
 */

/**
 * @brief The name the mutex of a running copy carries
 *
 * Windows compares mutex names case-sensitively, and installer.iss names the same string
 * in its AppMutex directive: the two have to agree exactly.
 */
#define INSTANCE_MUTEX_NAME L"AudientDeviceCloser.Mutex"

/**
 * @brief Take the mutex of this name, which marks the copy that holds it as the running one
 *
 * A copy that finds the mutex already taken is not a failure: the caller is told so and
 * leaves the way a second start of any single copy program leaves.  The handle is released
 * with instance_release, and in any case when the process ends.
 *
 * @param mutex_name the name of the mutex to take
 * @param out_other_running true when somebody already holds the name, false for the first
 * @param err receives the failure of the create, only when no handle came back
 * @return the handle of the mutex, NULL when the system refused the create
 */
HANDLE instance_take(wchar_t const *const mutex_name, bool *const out_other_running, struct ov_error *const err);

/**
 * @brief Hand the mutex back
 *
 * Safe to call without a held mutex.
 *
 * @param mutex the handle to release, set to NULL afterwards, NULL is allowed
 */
void instance_release(HANDLE *const mutex);
