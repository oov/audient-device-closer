#include "removal.h"

#include <windows.h>

#include <cfgmgr32.h>

#include <ovmo.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

/**
 * @brief Map the CONFIGRET of the removal call to the code the dialog explains
 *
 * The CONFIGRET number itself stays in the log: a user cannot act on it, and the text the
 * system offers for it is wrong for several codes (CR_ACCESS_DENIED is answered with a
 * network error text).
 *
 * @param cr the CONFIGRET the call answered with
 * @return the code of ov_error_type_removal the caller acts on
 */
static int removal_error_from_configret(unsigned long const cr) {
  if (cr == CR_ACCESS_DENIED) {
    return removal_error_access_denied;
  }
  if ((cr == CR_NO_SUCH_DEVNODE) || (cr == CR_NO_SUCH_DEVINST) || (cr == CR_NO_SUCH_VALUE) || (cr == CR_INVALID_DEVNODE) ||
      (cr == CR_INVALID_DEVINST)) {
    return removal_error_device_gone;
  }
  return removal_error_refused;
}

bool removal_error_is_veto(struct ov_error const *const err) { return ov_error_is(err, ov_error_type_removal, removal_error_veto); }

enum removal_error_code removal_error_of(struct ov_error const *const err) {
  int code = 0;

  if (!ov_error_get_code(err, ov_error_type_removal, &code)) {
    return removal_error_none;
  }
  return (enum removal_error_code)code;
}

/**
 * @brief Release the device with CM_Query_And_Remove_SubTreeW only
 *
 * Does not detect, does not look up the culprit, does not close any process.
 *
 * @param ks_devnode the devnode to release
 * @param err receives the failure of the call
 * @return true when the device was released
 */
bool removal_close(char const *const ks_devnode, struct ov_error *const err) {
  bool success = false;
  wchar_t ks_id[MAX_DEVICE_ID_LEN + 1] = {0}; // the devnode of the caller, the same bound
  char veto_device[MAX_PATH * 3 + 1] = {0};
  DEVINST ks = 0;
  DEVINST parent = 0;
  DEVINST parent_devinst = 0;
  PNP_VETO_TYPE veto_type = PNP_VetoTypeUnknown;
  CONFIGRET cr = 0;
  wchar_t veto_name[MAX_PATH] = {0};
  wchar_t parent_id[MAX_DEVICE_ID_LEN + 1] = {0};

  if (!ks_devnode) {
    OV_ERROR_SET(err, ov_error_type_removal, removal_error_refused, "the removal was asked for without a devnode");
    goto cleanup;
  }
  {
    int const n = OV_SNPRINTF(ks_id, sizeof(ks_id) / sizeof(ks_id[0]), NULL, L"%s", ks_devnode);
    if ((n < 0) || ((size_t)n >= (sizeof(ks_id) / sizeof(ks_id[0])))) {
      OV_ERROR_SET(err, ov_error_type_removal, removal_error_call_failed, "the devnode is too long for the system call");
      goto cleanup;
    }
  }

  cr = CM_Locate_DevNodeW(&ks, ks_id, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    OV_ERROR_SETF(err,
                  ov_error_type_removal,
                  removal_error_call_failed,
                  "%1$s failed: CONFIGRET=%2$u",
                  "%1$s failed: CONFIGRET=%2$u",
                  "CM_Locate_DevNodeW",
                  cr);
    goto cleanup;
  }
  cr = CM_Get_Parent(&parent, ks, 0);
  if (cr != CR_SUCCESS) {
    OV_ERROR_SETF(err,
                  ov_error_type_removal,
                  removal_error_call_failed,
                  "%1$s failed: CONFIGRET=%2$u",
                  "%1$s failed: CONFIGRET=%2$u",
                  "CM_Get_Parent",
                  cr);
    goto cleanup;
  }
  cr = CM_Get_Device_IDW(parent, parent_id, (ULONG)(sizeof(parent_id) / sizeof(parent_id[0])), 0);
  if (cr != CR_SUCCESS) {
    OV_ERROR_SETF(err,
                  ov_error_type_removal,
                  removal_error_call_failed,
                  "%1$s failed: CONFIGRET=%2$u",
                  "%1$s failed: CONFIGRET=%2$u",
                  "CM_Get_Device_IDW",
                  cr);
    goto cleanup;
  }

  cr = CM_Locate_DevNodeW(&parent_devinst, parent_id, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    OV_ERROR_SETF(err,
                  ov_error_type_removal,
                  removal_error_call_failed,
                  "%1$s failed: CONFIGRET=%2$u",
                  "%1$s failed: CONFIGRET=%2$u",
                  "CM_Locate_DevNodeW(parent)",
                  cr);
    goto cleanup;
  }

  cr = CM_Query_And_Remove_SubTreeW(parent_devinst, &veto_type, veto_name, (ULONG)(sizeof(veto_name) / sizeof(veto_name[0])), 0);
  if (cr == CR_SUCCESS) {
    success = true;
    goto cleanup;
  }
  if (cr == CR_REMOVE_VETOED) {
    if (OV_SNPRINTF(veto_device, sizeof(veto_device), NULL, "%ls", veto_name) < 0) {
      veto_device[0] = '\0'; // a name the conversion refuses is not worth dropping the report for
    }
    OV_ERROR_SETF(err,
                  ov_error_type_removal,
                  removal_error_veto,
                  "the removal was vetoed, blocked device: %1$hs",
                  "the removal was vetoed, blocked device: %1$hs",
                  veto_device);
    goto cleanup;
  }
  OV_ERROR_SETF(err,
                ov_error_type_removal,
                removal_error_from_configret(cr),
                "%1$s failed: CONFIGRET=%2$u",
                "%1$s failed: CONFIGRET=%2$u",
                "CM_Query_And_Remove_SubTreeW",
                cr);

cleanup:
  return success;
}
