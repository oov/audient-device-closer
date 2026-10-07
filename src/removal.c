#include "removal.h"

#include <windows.h>

#include <cfgmgr32.h>

#include <ovmo.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#define SET_CONFIGRET_ERROR(err, func, cr)                                                                                                 \
  OV_ERROR_SETF(                                                                                                                           \
      (err), ov_error_type_generic, ov_error_generic_fail, "%1$s failed: CONFIGRET=%2$u", "%1$s failed: CONFIGRET=%2$u", (func), (cr))

/**
 * @brief Map the CONFIGRET of the removal call to its result
 *
 * @param cr the CONFIGRET the call answered with
 * @return the result the caller acts on
 */
enum removal_result removal_result_from_configret(unsigned long const cr) {
  if (cr == CR_SUCCESS) {
    return REMOVAL_RESULT_OK;
  }
  if (cr == CR_REMOVE_VETOED) {
    return REMOVAL_RESULT_VETO;
  }
  return REMOVAL_RESULT_FAILED;
}

/**
 * @brief Report the reason of this answer to the caller that asked for one
 *
 * @param out_reason NULL is allowed, then nothing is written
 * @param reason the reason to report
 */
static void set_reason(enum removal_reason *const out_reason, enum removal_reason const reason) {
  if (out_reason != NULL) {
    *out_reason = reason;
  }
}

/**
 * @brief Map the CONFIGRET of the removal call to the reason the user sees
 *
 * @param cr the CONFIGRET the call answered with
 * @return the reason the dialog can explain
 */
enum removal_reason removal_reason_from_configret(unsigned long const cr) {
  if (cr == CR_SUCCESS) {
    return REMOVAL_REASON_NONE;
  }
  if (cr == CR_ACCESS_DENIED) {
    return REMOVAL_REASON_ACCESS_DENIED;
  }
  if ((cr == CR_NO_SUCH_DEVNODE) || (cr == CR_NO_SUCH_DEVINST) || (cr == CR_NO_SUCH_VALUE) || (cr == CR_INVALID_DEVNODE) ||
      (cr == CR_INVALID_DEVINST)) {
    return REMOVAL_REASON_DEVICE_GONE;
  }
  return REMOVAL_REASON_FAILED;
}

/**
 * @brief Name of the reason, as the dialog offers it
 *
 * @param reason the reason to name
 * @return a static string
 */
char const *removal_reason_name(enum removal_reason const reason) {
  switch (reason) {
  case REMOVAL_REASON_NONE:
    return "none";
  case REMOVAL_REASON_ACCESS_DENIED:
    return "access denied";
  case REMOVAL_REASON_CALL_FAILED:
    return "the removal call failed";
  case REMOVAL_REASON_DEVICE_GONE:
    return "the device is gone";
  case REMOVAL_REASON_FAILED:
    break;
  }
  return "the system refused";
}

/**
 * @brief Name of the result, as the log records it
 *
 * @param result the result to name
 * @return a static string
 */
char const *removal_result_name(enum removal_result const result) {
  switch (result) {
  case REMOVAL_RESULT_OK:
    return "ok";
  case REMOVAL_RESULT_VETO:
    return "veto";
  case REMOVAL_RESULT_FAILED:
    return "failed";
  }
  return "unknown";
}

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
enum removal_result removal_close(char const *const ks_devnode, enum removal_reason *const out_reason, struct ov_error *const err) {
  enum removal_result result = REMOVAL_RESULT_FAILED;
  wchar_t ks_id[MAX_DEVICE_ID_LEN + 1] = {0}; // the devnode of the caller, the same bound
  char veto_device[MAX_PATH * 3 + 1] = {0};
  DEVINST ks = 0;
  DEVINST parent = 0;
  DEVINST parent_devinst = 0;
  PNP_VETO_TYPE veto_type = PNP_VetoTypeUnknown;
  CONFIGRET cr = 0;
  wchar_t veto_name[MAX_PATH] = {0};
  wchar_t parent_id[MAX_DEVICE_ID_LEN + 1] = {0};

  if (out_reason != NULL) {
    *out_reason = REMOVAL_REASON_NONE;
  }
  if (!ks_devnode) {
    set_reason(out_reason, REMOVAL_REASON_FAILED);
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  {
    int const n = OV_SNPRINTF(ks_id, sizeof(ks_id) / sizeof(ks_id[0]), NULL, L"%s", ks_devnode);
    if ((n < 0) || ((size_t)n >= (sizeof(ks_id) / sizeof(ks_id[0])))) {
      set_reason(out_reason, REMOVAL_REASON_CALL_FAILED);
      OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
      goto cleanup;
    }
  }

  cr = CM_Locate_DevNodeW(&ks, ks_id, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    set_reason(out_reason, REMOVAL_REASON_CALL_FAILED);
    SET_CONFIGRET_ERROR(err, "CM_Locate_DevNodeW", cr);
    goto cleanup;
  }
  cr = CM_Get_Parent(&parent, ks, 0);
  if (cr != CR_SUCCESS) {
    set_reason(out_reason, REMOVAL_REASON_CALL_FAILED);
    SET_CONFIGRET_ERROR(err, "CM_Get_Parent", cr);
    goto cleanup;
  }
  cr = CM_Get_Device_IDW(parent, parent_id, (ULONG)(sizeof(parent_id) / sizeof(parent_id[0])), 0);
  if (cr != CR_SUCCESS) {
    set_reason(out_reason, REMOVAL_REASON_CALL_FAILED);
    SET_CONFIGRET_ERROR(err, "CM_Get_Device_IDW", cr);
    goto cleanup;
  }

  cr = CM_Locate_DevNodeW(&parent_devinst, parent_id, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    set_reason(out_reason, REMOVAL_REASON_CALL_FAILED);
    SET_CONFIGRET_ERROR(err, "CM_Locate_DevNodeW(parent)", cr);
    goto cleanup;
  }

  cr = CM_Query_And_Remove_SubTreeW(parent_devinst, &veto_type, veto_name, (ULONG)(sizeof(veto_name) / sizeof(veto_name[0])), 0);
  result = removal_result_from_configret(cr);
  if (result != REMOVAL_RESULT_VETO) {
    set_reason(out_reason, removal_reason_from_configret(cr));
  }
  if (cr == CR_SUCCESS) {
    goto cleanup;
  }
  if (result == REMOVAL_RESULT_VETO) {
    if (OV_SNPRINTF(veto_device, sizeof(veto_device), NULL, "%ls", veto_name) < 0) {
      veto_device[0] = '\0'; // a name the conversion refuses is not worth dropping the report for
    }
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "the removal was vetoed, blocked device: %1$hs",
                  "the removal was vetoed, blocked device: %1$hs",
                  veto_device);
    goto cleanup;
  }
  SET_CONFIGRET_ERROR(err, "CM_Query_And_Remove_SubTreeW", cr);

cleanup:
  return result;
}
