#pragma once

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
 * @brief The mutex a copy of this program holds while it runs
 *
 * The handle is held by value: a NULL handle is the copy that holds nothing, and the caller
 * keeps the instance until its cleanup hands it back.
 */
struct instance {
  void *handle; // HANDLE, kept opaque so this header stays free of windows.h
};

/**
 * @brief Take the mutex that marks this copy as the running one
 *
 * The mutex is not waited for: a take that finds the mutex taken answers at once, so that a
 * second start can reach the window of the first one and leave.  A take that finds another
 * copy running is a failure of this call like any other, and it is told apart the way the
 * system tells it: the error carries HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS).  The
 * instance is released with instance_release, and in any case when the process ends.
 *
 * @param err receives the reason no mutex is held: the refusal of the create, or
 *            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) when another copy holds the mutex
 * @return the instance of the running copy, one without a handle when err says why not
 */
NODISCARD struct instance instance_take(struct ov_error *const err);

/**
 * @brief Hand the mutex back
 *
 * Safe to call without a held mutex.
 *
 * @param instance the instance to release, NULL is allowed
 * @param err receives the failure of the close, NULL is allowed
 * @return false when the mutex could not be given back
 */
NODISCARD bool instance_release(struct instance *const instance, struct ov_error *const err);
