#include "device_manager.h"

#include <windows.h>

#include <cfgmgr32.h>
#include <ks.h>

#include <string.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#ifndef CM_PROB_NONE
#  define CM_PROB_NONE 0x00000000
#endif

// KSCATEGORY_RENDER and its neighbours are macros that name themselves, which clang refuses
// to expand without being told to; they are the names of the categories and are meant to be
// used, and the GUIDs they name come from ksguid.
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"

/**
 * @brief Raise the error of a CONFIGRET that a CM_* call answered with
 *
 * CONFIGRET of cfgmgr32 is not an HRESULT, so it is reported with its number in the
 * message.  This stays a macro on purpose: OV_ERROR_SETF records the file and the line it
 * is expanded at, and a function wrapper would blame the wrapper for every failing call.
 *
 * @param err receives the error
 * @param func the name of the CM_* call that failed
 * @param cr the CONFIGRET it answered with
 */
#define SET_CONFIGRET_ERROR(err, func, cr)                                                                                                 \
  OV_ERROR_SETF(                                                                                                                           \
      (err), ov_error_type_generic, ov_error_generic_fail, "%1$s failed: CONFIGRET=%2$u", "%1$s failed: CONFIGRET=%2$u", (func), (cr))

/** @brief How many times a CM_* list is read again while it keeps growing */
enum { kCmListAttempts = 5 };

/**
 * @brief Does a name of the caller fit the buffers this layer copies it into
 *
 * The copies below are made without a bound of their own, so a name too long for the
 * buffers is turned away here rather than overrunning them.
 *
 * @param devnode the name to look at, may be NULL
 * @return true when the name fits
 */
static bool devnode_fits(wchar_t const *const devnode) { return (devnode != NULL) && (wcslen(devnode) <= MAX_DEVICE_ID_LEN); }

/**
 * @brief Walk the entries of a multi-sz list
 *
 * @param buf the list, it is not terminated beyond len
 * @param len how many characters the list holds
 * @param on_entry the callback that looks at one entry
 * @param ctx handed back to on_entry
 * @param err receives the refusal of the callback
 * @return false when the callback refused one of the entries
 */
static NODISCARD bool walk_multisz(wchar_t const *const buf,
                                   size_t const len,
                                   bool (*on_entry)(wchar_t const *entry, void *ctx, struct ov_error *err),
                                   void *const ctx,
                                   struct ov_error *const err) {
  for (size_t i = 0; i < len;) {
    size_t const slen = wcslen(&buf[i]);
    if (slen == 0) {
      i += 1;
      continue;
    }
    if (!on_entry(&buf[i], ctx, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    i += slen + 1;
  }
  return true;
}

/**
 * @brief Does one category have an endpoint at this devnode
 *
 * @param category the KS category to ask for
 * @param devnode the devnode to ask
 * @param flags CM_GET_DEVICE_INTERFACE_LIST_PRESENT or _ALL_DEVICES
 * @param err receives the failure of the question
 * @return ov_true when the list holds at least one endpoint, ov_false when it is empty,
 *         ov_indeterminate when the question could not be answered, with err set
 */
static NODISCARD ov_tribool category_has_endpoint(GUID const *const category,
                                                  wchar_t const *const devnode,
                                                  ULONG const flags,
                                                  struct ov_error *const err) {
  ov_tribool result = ov_indeterminate;
  wchar_t *buf = NULL;
  GUID cls = *category;
  wchar_t devnode_id[MAX_DEVICE_ID_LEN + 1];

  if (!devnode_fits(devnode)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  wcscpy(devnode_id, devnode);
  for (int attempt = 0; attempt < kCmListAttempts; attempt++) {
    ULONG len = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&len, &cls, devnode_id, flags);

    if (cr != CR_SUCCESS) {
      SET_CONFIGRET_ERROR(err, "CM_Get_Device_Interface_List_SizeW", cr);
      goto cleanup;
    }
    if (len == 0) {
      result = ov_false; // the list is empty, which is the answer
      goto cleanup;
    }
    if (!OV_ARRAY_GROW(&buf, (size_t)len)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    cr = CM_Get_Device_Interface_ListW(&cls, devnode_id, buf, len, flags);
    if (cr == CR_BUFFER_SMALL) {
      continue; // the list grew between the size query and the read
    }
    if (cr != CR_SUCCESS) {
      SET_CONFIGRET_ERROR(err, "CM_Get_Device_Interface_ListW", cr);
      goto cleanup;
    }
    result = (wcslen(buf) > 0) ? ov_true : ov_false;
    goto cleanup;
  }
  OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);

cleanup:
  if (buf != NULL) {
    OV_ARRAY_DESTROY(&buf);
  }
  return result;
}

static NODISCARD bool foreach_devnode(wchar_t const *const service,
                                      bool (*on_devnode)(wchar_t const *devnode, void *ctx, struct ov_error *err),
                                      void *const ctx,
                                      struct ov_error *const err) {
  bool success = false;
  wchar_t *buf = NULL;
  ULONG const flags = CM_GETIDLIST_FILTER_SERVICE | CM_GETIDLIST_FILTER_PRESENT;

  for (int attempt = 0; attempt < kCmListAttempts; attempt++) {
    ULONG len = 0;
    CONFIGRET cr = CM_Get_Device_ID_List_SizeW(&len, service, flags);

    if (cr != CR_SUCCESS) {
      SET_CONFIGRET_ERROR(err, "CM_Get_Device_ID_List_SizeW", cr);
      goto cleanup;
    }
    if (len == 0) {
      success = true; // the service has no device, which is not a failure
      goto cleanup;
    }
    if (!OV_ARRAY_GROW(&buf, (size_t)len)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    cr = CM_Get_Device_ID_ListW(service, buf, len, flags);
    if (cr == CR_BUFFER_SMALL) {
      continue; // the list grew between the size query and the read
    }
    if (cr != CR_SUCCESS) {
      SET_CONFIGRET_ERROR(err, "CM_Get_Device_ID_ListW", cr);
      goto cleanup;
    }
    if (!walk_multisz(buf, (size_t)len, on_devnode, ctx, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    success = true;
    goto cleanup;
  }
  OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);

cleanup:
  if (buf != NULL) {
    OV_ARRAY_DESTROY(&buf);
  }
  return success;
}

static NODISCARD bool facts_of(wchar_t const *const devnode, struct device_facts *const out, struct ov_error *const err) {
  bool success = false;
  DEVINST devinst = 0;
  ULONG status = 0;
  ULONG problem = 0;
  CONFIGRET cr = 0;
  wchar_t devnode_id[MAX_DEVICE_ID_LEN + 1];
  ov_tribool render_present = ov_indeterminate;
  ov_tribool render_registered = ov_indeterminate;
  ov_tribool capture_present = ov_indeterminate;
  ov_tribool capture_registered = ov_indeterminate;

  if (!devnode_fits(devnode)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  wcscpy(devnode_id, devnode);
  cr = CM_Locate_DevNodeW(&devinst, devnode_id, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Locate_DevNodeW", cr);
    goto cleanup;
  }
  cr = CM_Get_DevNode_Status(&status, &problem, devinst, 0);
  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Get_DevNode_Status", cr);
    goto cleanup;
  }
  render_present = category_has_endpoint(&KSCATEGORY_RENDER, devnode, CM_GET_DEVICE_INTERFACE_LIST_PRESENT, err);
  if (render_present == ov_indeterminate) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  render_registered = category_has_endpoint(&KSCATEGORY_RENDER, devnode, CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES, err);
  if (render_registered == ov_indeterminate) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  capture_present = category_has_endpoint(&KSCATEGORY_CAPTURE, devnode, CM_GET_DEVICE_INTERFACE_LIST_PRESENT, err);
  if (capture_present == ov_indeterminate) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  capture_registered = category_has_endpoint(&KSCATEGORY_CAPTURE, devnode, CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES, err);
  if (capture_registered == ov_indeterminate) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  out->problem_code = (problem == CM_PROB_NONE) ? 0u : (uint32_t)problem;
  out->endpoint_present = (render_present == ov_true) || (capture_present == ov_true);
  out->endpoint_registered = (render_registered == ov_true) || (capture_registered == ov_true);
  success = true;

cleanup:
  return success;
}

/**
 * @brief How deep the ancestors of a KS devnode are followed
 *
 * A KS devnode sits under the audio function of a USB composite device, so the removal
 * record of the USB device is an ancestor a few levels up, not the devnode itself.
 */
enum { kKsAncestorDepth = 4 };

static NODISCARD ov_tribool parent_of(wchar_t const *const devnode, wchar_t **const buf, struct ov_error *const err) {
  ov_tribool result = ov_indeterminate;
  wchar_t id[MAX_DEVICE_ID_LEN + 1];
  wchar_t devnode_id[MAX_DEVICE_ID_LEN + 1];
  DEVINST devinst = 0;
  CONFIGRET cr = 0;

  if (!devnode_fits(devnode)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  wcscpy(devnode_id, devnode);
  cr = CM_Locate_DevNodeW(&devinst, devnode_id, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Locate_DevNodeW", cr);
    goto cleanup;
  }
  {
    DEVINST parent = 0;

    cr = CM_Get_Parent(&parent, devinst, 0);
    if (cr == CR_NO_SUCH_DEVNODE) {
      result = ov_false; // the devnode is at the root: it has no device above it
      goto cleanup;
    }
    if (cr != CR_SUCCESS) {
      SET_CONFIGRET_ERROR(err, "CM_Get_Parent", cr);
      goto cleanup;
    }
    cr = CM_Get_Device_IDW(parent, id, (ULONG)(sizeof(id) / sizeof(id[0])), 0);
    if (cr != CR_SUCCESS) {
      SET_CONFIGRET_ERROR(err, "CM_Get_Device_IDW", cr);
      goto cleanup;
    }
  }
  if (!ov_sprintf_wchar(buf, err, NULL, L"%ls", id)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = ov_true;

cleanup:
  return result;
}

/** @brief How long a devnode name may be */
enum { kWatchedDevnodeLen = MAX_DEVICE_ID_LEN + 1 };

/** @brief One watch on its thread */
struct device_watch {
  struct async *async;                                        //!< the thread of the watch
  struct async_context *ctx;                                  //!< the watch, as its thread reaches it
  HANDLE arrival;                                             //!< up once an interface arrives
  HANDLE stop;                                                //!< up once the caller asks the watch to stop
  HCMNOTIFICATION registration;                               //!< what sets the arrival
  wchar_t devnode[kWatchedDevnodeLen];                        //!< the devnode to watch
  unsigned long max_wait_ms;                                  //!< how long to watch at most
  bool (*on_update_device)(struct device_facts const *facts); //!< true ends the watch
  async_notify_fn notify;                                     //!< the caller's call, may be NULL
  void *userdata;                                             //!< handed to notify
  struct device_facts facts;                                  //!< what the last look that could be taken reported
  struct ov_error *err;                                       //!< borrowed, the caller's, written by the thread of the watch
  bool failed;                                                //!< whether the last look could not be taken
};

/**
 * @brief Set the event the watch is waiting on when an interface arrives
 *
 * Runs on a thread of the configuration manager, so it does no more than that.  Which
 * interface it was is not told apart: the watch looks at the devnode it cares about again.
 * A wait that cannot be woken is kept for the thread of the watch, which is the one that
 * may write the error of the watch.
 *
 * @param notify the registration
 * @param ctx the watch the registration was made for
 * @param action what happened to the interface
 * @param data the interface it happened to
 * @param size size of data
 * @return ERROR_SUCCESS, always
 */
static DWORD CALLBACK on_interface_arrival(
    HCMNOTIFICATION const notify, PVOID const ctx, CM_NOTIFY_ACTION const action, PCM_NOTIFY_EVENT_DATA const data, DWORD const size) {
  struct device_watch *const w = (struct device_watch *)ctx;
  (void)notify;
  (void)data;
  (void)size;
  if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
    if (!SetEvent(w->arrival)) {
      OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
      w->failed = true;
    }
  }
  return ERROR_SUCCESS;
}

/**
 * @brief Tell the caller what came of the watch
 *
 * Called on the thread that asked the watch to stop, or on the thread of the watch once it is
 * over, which is what stopped says.
 *
 * @param userdata the watch
 * @param stopped true when the caller asked the watch to stop
 */
static void watch_notify(void *const userdata, bool const stopped) {
  struct device_watch *const w = (struct device_watch *)userdata;

  if (stopped) {
    if (!SetEvent(w->stop)) {
      // the ask never reaches the wait, so the watch ends at its bound with no facts
      OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
      w->failed = true;
    }
    return;
  }
  if (w->notify != NULL) {
    w->notify(w->userdata, false);
  }
}

/**
 * @brief Look at the devnode until it is an audio device again
 *
 * Runs on the thread of the watch.  A look that answers with anything but an audio device
 * is answered by waiting for an interface to show up, which is a moment worth looking
 * again; a look that failed is one the watch does without, and its reason goes before the
 * next look writes its own.
 *
 * @param ctx the watch, as this thread may reach it
 */
static void watch_body(struct async_context *const ctx) {
  struct device_watch *const w = (struct device_watch *)ctx->userdata;
  CM_NOTIFY_FILTER filter;
  ULONGLONG const deadline = GetTickCount64() + w->max_wait_ms;
  bool look_failed = false;

  w->ctx = ctx;
  w->arrival = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (w->arrival == NULL) {
    OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
    w->failed = true;
    goto cleanup;
  }
  memset(&filter, 0, sizeof(filter));
  filter.cbSize = sizeof(filter);
  filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
  filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
  if (CM_Register_Notification(&filter, w, on_interface_arrival, &w->registration) != CR_SUCCESS) {
    w->registration = NULL;
    OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
    w->failed = true;
    goto cleanup;
  }
  for (;;) {
    ULONGLONG const now = GetTickCount64();
    struct device_facts facts = {0};
    HANDLE ends[2];

    if (w->failed) {
      // the wait could not be woken, and its reason is already in err
      OV_ERROR_ADD_TRACE(w->err);
      goto cleanup;
    }
    if (look_failed) {
      // the look before this one wrote its reason: a look that cannot be taken is one the
      // wait goes on without, so its reason goes before the next one writes its own
      OV_ERROR_DESTROY(w->err);
    }
    look_failed = !facts_of(w->devnode, &facts, w->err);
    if (!look_failed) {
      w->facts = facts;
      if (w->on_update_device(&facts)) {
        goto cleanup; // the caller's answer ends the watch
      }
    }
    if (now >= deadline) {
      // the last look is the one that says how the watch ended: one that could be taken is
      // the caller's to judge, and one that could not is handed back as the failure
      w->failed = look_failed;
      goto cleanup;
    }
    ends[0] = w->stop;
    ends[1] = w->arrival;
    switch (WaitForMultipleObjects(2, ends, FALSE, (DWORD)(deadline - now))) {
    case WAIT_OBJECT_0:
      goto cleanup; // the caller asked the watch to stop
    case (WAIT_OBJECT_0 + 1):
      if (!ResetEvent(w->arrival)) {
        OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
        w->failed = true;
        goto cleanup;
      }
      break; // a moment worth looking again
    case WAIT_TIMEOUT:
      break; // the bound is up, and the look at the top says what came of it
    default:
      OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
      w->failed = true;
      goto cleanup;
    }
  }

cleanup:
  if (w->registration != NULL) {
    CONFIGRET const cr = CM_Unregister_Notification(w->registration);

    w->registration = NULL;
    // a registration that stays would reach a watch that is freed
    if ((cr != CR_SUCCESS) && !w->failed) {
      SET_CONFIGRET_ERROR(w->err, "CM_Unregister_Notification", cr);
      w->failed = true;
    }
  }
  if (w->arrival != NULL) {
    if (!CloseHandle(w->arrival) && !w->failed) {
      OV_ERROR_SET_HRESULT(w->err, HRESULT_FROM_WIN32(GetLastError()));
      w->failed = true;
    }
    w->arrival = NULL;
  }
  w->ctx = NULL;
}

/**
 * @brief One watch on its thread, as the caller asked for it
 */
static NODISCARD struct device_watch *watch(wchar_t const *const devnode,
                                            unsigned long const max_wait_ms,
                                            bool (*const on_update_device)(struct device_facts const *facts),
                                            async_notify_fn const notify,
                                            void *const userdata,
                                            struct ov_error *const err) {
  struct device_watch *w = NULL;
  bool success = false;

  if ((devnode == NULL) || (wcslen(devnode) >= kWatchedDevnodeLen) || (on_update_device == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  if (!OV_REALLOC(&w, 1, sizeof(*w))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  memset(w, 0, sizeof(*w));
  wcscpy(w->devnode, devnode);
  w->max_wait_ms = max_wait_ms;
  w->on_update_device = on_update_device;
  w->notify = notify;
  w->userdata = userdata;
  w->err = err;
  w->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (w->stop == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  // the event a stop sets is made here, so an ask is answered from the first moment the
  // operation is there rather than only once its body got that far
  w->async = async_start(watch_body, w, watch_notify, err);
  if (w->async == NULL) {
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success && (w != NULL)) {
    if (w->stop != NULL) {
      CloseHandle(w->stop);
    }
    OV_FREE(&w);
  }
  return success ? w : NULL;
}

static NODISCARD struct device_facts watch_take(struct device_watch **const w, struct ov_error *const err) {
  struct device_facts facts = {.problem_code = DEVICE_PROBLEM_NO_REPORT};

  if ((w == NULL) || (*w == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!async_release(&(*w)->async, err)) {
    // the last look could not be taken either: the watch answers no facts, and err says why
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  facts = (*w)->facts;
  if ((*w)->failed) {
    facts = (struct device_facts){.problem_code = DEVICE_PROBLEM_NO_REPORT};
  }

cleanup:
  if ((w != NULL) && (*w != NULL)) {
    if (!CloseHandle((*w)->stop)) {
      if (facts.problem_code == DEVICE_PROBLEM_NO_REPORT) {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      } else {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      }
      facts = (struct device_facts){.problem_code = DEVICE_PROBLEM_NO_REPORT};
    }
    OV_FREE(w);
  }
  return facts;
}

/**
 * @brief The sentence this program has for one problem the machine named
 *
 * The codes are the machine's own, so this is the only place that knows which of them the
 * program has a sentence for.  A code it has none for answers NULL, and the caller falls
 * back to its own sentence.
 *
 * @param problem_code the machine's own number
 * @return the identifier to translate, NULL when there is no sentence of its own
 */
static char const *problem_id(uint32_t const problem_code) {
  switch (problem_code) {
  case CM_PROB_DISABLED:
    return gettext_noop("The device is disabled.");
  default:
    return NULL;
  }
}

static void watch_cancel(struct device_watch *const w) {
  if (w != NULL) {
    async_cancel(w->async);
  }
}

/** @brief The machine of the system, asked through the configuration manager */
static struct device_manager const kSystemDeviceManager = {
    .foreach_devnode = foreach_devnode,
    .facts_of = facts_of,
    .parent_of = parent_of,
    .problem_id = problem_id,
    .watch = watch,
    .watch_take = watch_take,
    .watch_cancel = watch_cancel,
};

struct device_manager const *device_manager_system(void) { return &kSystemDeviceManager; }
