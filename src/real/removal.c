#include "removal_manager.h"

#include <windows.h>

#include <cfgmgr32.h>

#include <string.h>

#include <ovmo.h>

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
 * @return the code of ov_error_type_app the caller acts on
 */
static enum app_error_code removal_error_from_configret(unsigned long const cr) {
  if (cr == CR_ACCESS_DENIED) {
    return app_error_removal_access_denied;
  }
  if ((cr == CR_NO_SUCH_DEVNODE) || (cr == CR_NO_SUCH_DEVINST) || (cr == CR_NO_SUCH_VALUE) || (cr == CR_INVALID_DEVNODE) ||
      (cr == CR_INVALID_DEVINST)) {
    return app_error_removal_device_gone;
  }
  return app_error_removal_refused;
}

/**
 * @brief Raise the error of a CONFIGRET with the name of the call that answered it
 *
 * @param err receives the error
 * @param func the name of the call that failed
 * @param cr the CONFIGRET it answered with
 * @param code the code of ov_error_type_app the caller acts on
 */
static void
set_configret_error(struct ov_error *const err, char const *const func, unsigned long const cr, enum app_error_code const code) {
  OV_ERROR_SETF(err, ov_error_type_app, (int)code, "%1$s failed: CONFIGRET=%2$u", "%1$s failed: CONFIGRET=%2$u", func, cr);
}

static NODISCARD bool remove_device(wchar_t const *const ks_devnode, struct ov_error *const err) {
  bool success = false;
  char veto_device[MAX_PATH * 3 + 1] = {0};
  DEVINST ks = 0;
  DEVINST parent = 0;
  DEVINST parent_devinst = 0;
  PNP_VETO_TYPE veto_type = PNP_VetoTypeUnknown;
  CONFIGRET cr = 0;
  wchar_t veto_name[MAX_PATH] = {0};
  wchar_t parent_id[MAX_DEVICE_ID_LEN + 1] = {0};
  wchar_t devnode_id[MAX_DEVICE_ID_LEN + 1] = {0};

  if (ks_devnode == NULL) {
    OV_ERROR_SET(err, ov_error_type_app, app_error_removal_refused, "the removal was asked for without a devnode");
    goto cleanup;
  }
  if (wcslen(ks_devnode) >= (sizeof(devnode_id) / sizeof(devnode_id[0]))) {
    OV_ERROR_SET(err, ov_error_type_app, app_error_removal_call_failed, "the devnode is too long for the system call");
    goto cleanup;
  }
  wcscpy(devnode_id, ks_devnode);

  cr = CM_Locate_DevNodeW(&ks, devnode_id, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    set_configret_error(err, "CM_Locate_DevNodeW", cr, app_error_removal_call_failed);
    goto cleanup;
  }
  cr = CM_Get_Parent(&parent, ks, 0);
  if (cr != CR_SUCCESS) {
    set_configret_error(err, "CM_Get_Parent", cr, app_error_removal_call_failed);
    goto cleanup;
  }
  cr = CM_Get_Device_IDW(parent, parent_id, (ULONG)(sizeof(parent_id) / sizeof(parent_id[0])), 0);
  if (cr != CR_SUCCESS) {
    set_configret_error(err, "CM_Get_Device_IDW", cr, app_error_removal_call_failed);
    goto cleanup;
  }
  cr = CM_Locate_DevNodeW(&parent_devinst, parent_id, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    set_configret_error(err, "CM_Locate_DevNodeW(parent)", cr, app_error_removal_call_failed);
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
                  ov_error_type_app,
                  app_error_removal_veto,
                  "the removal was vetoed, blocked device: %1$hs",
                  "the removal was vetoed, blocked device: %1$hs",
                  veto_device);
    goto cleanup;
  }
  set_configret_error(err, "CM_Query_And_Remove_SubTreeW", cr, removal_error_from_configret(cr));

cleanup:
  return success;
}

/** @brief One removal, running on a thread of its own */
struct removal_op {
  struct async *async;                    //!< the thread of the removal
  async_notify_fn notify;                 //!< the caller's call, may be NULL
  void *userdata;                         //!< handed to notify
  struct ov_error *err;                   //!< borrowed, the caller's, written by the thread of the removal
  bool ok;                                //!< whether the device was released
  wchar_t devnode[MAX_DEVICE_ID_LEN + 1]; //!< the device to release
};

/**
 * @brief Tell the caller what came of the removal
 *
 * Called on the thread of the removal once it is over.  An ask to stop has nothing to hand
 * on: the configuration manager answers in one call and cannot be told to stop, so the mark
 * the thread of async keeps is all the ask changes.
 *
 * @param userdata the removal
 * @param stopped true when the caller asked the removal to stop
 */
static void op_notify(void *const userdata, bool const stopped) {
  struct removal_op *const op = (struct removal_op *)userdata;

  if (!stopped && (op->notify != NULL)) {
    op->notify(op->userdata, false);
  }
}

/**
 * @brief Run the removal
 *
 * The configuration manager answers in one call and has no way to be asked to stop, so the
 * ask is read before the call only: the call is over when it is over, and the answer is read
 * after that.
 *
 * @param ctx the removal, as this thread may reach it
 */
static void op_body(struct async_context *const ctx) {
  struct removal_op *const op = (struct removal_op *)ctx->userdata;

  if (ctx->stopped(ctx)) {
    OV_ERROR_SET_GENERIC(op->err, ov_error_generic_abort);
    return;
  }
  op->ok = remove_device(op->devnode, op->err);
}

static NODISCARD struct removal_op *
remove_start(wchar_t const *const devnode, async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  struct removal_op *op = NULL;
  bool success = false;

  if (devnode == NULL) {
    OV_ERROR_SET(err, ov_error_type_app, app_error_removal_refused, "the removal was asked for without a devnode");
    goto cleanup;
  }
  if (!OV_REALLOC(&op, 1, sizeof(*op))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *op = (struct removal_op){0};
  op->notify = notify;
  op->userdata = userdata;
  op->err = err;
  wcsncpy(op->devnode, devnode, sizeof(op->devnode) / sizeof(op->devnode[0]) - 1);
  op->async = async_start(op_body, op, op_notify, err);
  if (op->async == NULL) {
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    OV_FREE(&op);
  }
  return success ? op : NULL;
}

static void op_cancel(struct removal_op *const op) {
  if (op != NULL) {
    async_cancel(op->async);
  }
}

static NODISCARD bool op_release(struct removal_op **const op) {
  bool ok = false;

  if ((op == NULL) || (*op == NULL)) {
    return false;
  }
  if (!async_release(&(*op)->async, (*op)->err)) {
    OV_ERROR_ADD_TRACE((*op)->err);
  }
  ok = (*op)->ok;
  OV_FREE(op);
  return ok;
}

/** @brief The machine of the system, releasing through the configuration manager */
static struct removal_manager const kSystemRemovalManager = {
    .remove_start = remove_start,
    .op_cancel = op_cancel,
    .op_release = op_release,
};

struct removal_manager const *removal_manager_system(void) { return &kSystemRemovalManager; }
