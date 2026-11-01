#include "mock/removal.h"

#ifdef ADC_MOCK

#  include "removal_manager.h"

#  include <string.h>

#  include <windows.h>

/** @brief What the stand-in answers */
static struct removal_mock_script g_script = {0};
/** @brief How often the removal was asked for */
static size_t g_calls = 0;
/** @brief Whether a script is installed at all */
static bool g_installed = false;

/** @brief One removal of the stand-in, running on a thread of its own */
struct removal_op {
  struct async *async;    //!< the thread of the removal
  async_notify_fn notify; //!< the caller's call, may be NULL
  void *userdata;         //!< handed to notify
  struct ov_error *err;   //!< borrowed, the caller's
  bool ok;                //!< whether the device was released
  wchar_t const *devnode; //!< the device to release
};

/**
 * @brief Tell the caller what came of the removal
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
 * @brief The stand-in answers one removal
 *
 * @param devnode the device to release
 * @param err receives the refusal
 * @return true when the device was released
 */
static NODISCARD bool remove_device(wchar_t const *const devnode, struct ov_error *const err) {
  if (!g_installed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if ((devnode == NULL) || (devnode[0] == L'\0')) {
    OV_ERROR_SET(err, ov_error_type_removal, removal_error_refused, "the removal was asked for without a devnode");
    return false;
  }
  if (g_script.delay_ms > 0) {
    Sleep(g_script.delay_ms);
  }
  g_calls++;
  if (g_calls <= g_script.refusals) {
    if (g_script.code == removal_error_veto) {
      OV_ERROR_SETF(err,
                    ov_error_type_removal,
                    removal_error_veto,
                    "%1$ls",
                    "the removal was vetoed, blocked device: %1$ls",
                    (g_script.vetoed_device != NULL) ? g_script.vetoed_device : L"");
    } else {
      OV_ERROR_SET(err, ov_error_type_removal, (int)g_script.code, "the stand-in refused the removal");
    }
    return false;
  }
  if (g_script.on_success != NULL) {
    g_script.on_success(g_script.userdata);
  }
  return true;
}

/**
 * @brief Run the removal
 *
 * @param ctx the removal, as this thread may reach it
 */
static void op_body(struct async_context *const ctx) {
  struct removal_op *const op = (struct removal_op *)ctx->userdata;

  op->ok = remove_device(op->devnode, op->err);
}

static NODISCARD struct removal_op *
remove_start(wchar_t const *const devnode, async_notify_fn const notify, void *const userdata, struct ov_error *const err) {
  struct removal_op *op = NULL;
  bool success = false;

  if (!OV_REALLOC(&op, 1, sizeof(*op))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *op = (struct removal_op){0};
  op->notify = notify;
  op->userdata = userdata;
  op->err = err;
  op->devnode = devnode;
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

/** @brief The stand-in machine, answering what the installed script says */
static struct removal_manager const kMockRemovalManager = {
    .remove_start = remove_start,
    .op_cancel = op_cancel,
    .op_release = op_release,
};

static struct removal_manager const *manager(void) { return &kMockRemovalManager; }

bool removal_mock_install(struct removal_mock_script const *const script) {
  if (script == NULL) {
    return false;
  }
  removal_mock_reset();
  g_script = *script;
  g_calls = 0;
  g_installed = true;
  removal_manager_select(manager());
  return true;
}

size_t removal_mock_remove_count(void) { return g_calls; }

void removal_mock_reset(void) {
  memset(&g_script, 0, sizeof(g_script));
  g_calls = 0;
  g_installed = false;
  removal_manager_select(NULL);
}

#endif // ADC_MOCK
