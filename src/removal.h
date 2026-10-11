#pragma once

#include <ovbase.h>

#include <stdbool.h>
#include <stddef.h>

struct ov_error;

/**
 * @brief What the removal call answered
 */
enum removal_result {
  REMOVAL_RESULT_OK,     //!< the device is released
  REMOVAL_RESULT_VETO,   //!< something still holds the device
  REMOVAL_RESULT_FAILED, //!< the call did not release the device
};

/**
 * @brief Why a removal failed, in the terms the user has to learn about
 *
 * The CONFIGRET number itself stays in the log: a user cannot act on it, and the text the
 * system offers for it is wrong for several codes (CR_ACCESS_DENIED is answered with a
 * network error text).
 */
enum removal_reason {
  REMOVAL_REASON_NONE = 0,
  REMOVAL_REASON_ACCESS_DENIED, // this program may not ask for the removal
  REMOVAL_REASON_CALL_FAILED,   // an earlier step of the removal already failed
  REMOVAL_REASON_DEVICE_GONE,   // the device was not there any more
  REMOVAL_REASON_FAILED,        // the system refused for a reason of its own
};

/**
 * @brief Release the device with CM_Query_And_Remove_SubTreeW only
 *
 * Does not detect, does not look up the culprit, does not close any process.
 *
 * @param ks_devnode the devnode to release
 * @param out_reason may be NULL; it is filled in for every answer that is not OK
 * @param err receives the failure of the call
 * @return what the call answered
 */
enum removal_result removal_close(char const *const ks_devnode, enum removal_reason *const out_reason, struct ov_error *const err);
