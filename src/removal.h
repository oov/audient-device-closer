#pragma once

#include <ovbase.h>

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief The error type of the failures a removal answers with
 *
 * ovbase keeps the types 0-3 for itself (invalid, generic, hresult, errno).  The types of
 * this program start above them, so a report of a removal is told apart from one of the
 * system without reading its message.
 */
enum removal_error_type {
  ov_error_type_removal = 0x10,
};

/**
 * @brief Why a removal did not release the device, as a code of ov_error_type_removal
 *
 * Whether the release happened used to be a result of its own next to a reason; the two
 * are one answer now: the return of removal_close says whether it happened, and the code
 * of the error it raises says why it did not.
 */
enum removal_error_code {
  removal_error_none = 0,          //!< no removal error: the outcome was not a refusal of the removal
  removal_error_veto = 1,          //!< a process still holds the device; the caller closes it and asks again
  removal_error_access_denied = 2, //!< this program may not ask for the removal
  removal_error_call_failed = 3,   //!< an earlier step of the removal already failed
  removal_error_device_gone = 4,   //!< the device was not there any more
  removal_error_refused = 5,       //!< the system refused for a reason of its own
};

/**
 * @brief Release the device with CM_Query_And_Remove_SubTreeW only
 *
 * Does not detect, does not look up the culprit, does not close any process.  A refusal is
 * raised as ov_error_type_removal with the code that says why, so the caller reads the
 * reason out of the error it has to handle anyway.
 *
 * @param ks_devnode the devnode to release
 * @param err receives the failure; on false it carries ov_error_type_removal with the code
 *            that says why, a veto among them
 * @return true when the device was released
 */
bool removal_close(char const *const ks_devnode, struct ov_error *const err);

/**
 * @brief Was the removal refused because a process still holds the device
 *
 * A veto is not a give-up: the caller closes what holds the device and asks again.  The
 * code is read through this one place, so the meaning of a veto lives next to the call
 * that raises it.
 *
 * @param err the error of a removal that failed
 * @return true when the code is removal_error_veto
 */
bool removal_error_is_veto(struct ov_error const *const err);

/**
 * @brief The code of a removal error, for a caller that carries it further
 *
 * @param err the error of a removal that failed
 * @return the code of ov_error_type_removal, removal_error_none when err carries no removal error
 */
enum removal_error_code removal_error_of(struct ov_error const *const err);
