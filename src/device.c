#include "device.h"

#include <windows.h>

#include <cfgmgr32.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#ifndef CM_PROB_NONE
#  define CM_PROB_NONE 0x00000000
#endif

/** @brief The KS category of the render endpoints */
static GUID const kKsCategoryRender = {0x65e8773d, 0x8f56, 0x11d0, {0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
/** @brief The KS category of the capture endpoints */
static GUID const kKsCategoryCapture = {0x65e8773e, 0x8f56, 0x11d0, {0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};

/**
 * @brief Decide the state of the device from what the system reports
 *
 * @param problem_code a CM_PROB_* value of the devnode
 * @param render_present interfaces of the render category that are present
 * @param render_registered interfaces of the render category that are registered
 * @param capture_present interfaces of the capture category that are present
 * @param capture_registered interfaces of the capture category that are registered
 * @return the state the dialog shows
 */
enum device_state device_evaluate(
    unsigned long problem_code, size_t render_present, size_t render_registered, size_t capture_present, size_t capture_registered) {
  bool const present = (render_present > 0) || (capture_present > 0);
  bool const registered = (render_registered > 0) || (capture_registered > 0);

  if (problem_code != CM_PROB_NONE) {
    return DEVICE_STATE_PROBLEM;
  }
  if (present) {
    return DEVICE_STATE_HEALTHY;
  }
  if (registered) {
    return DEVICE_STATE_FAULTED;
  }
  return DEVICE_STATE_UNKNOWN;
}

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

/**
 * @brief A string in UTF-8 as the wide string the Win32 calls want
 *
 * @param src the string to convert, UTF-8
 * @param dst receives the copy, release with OV_ARRAY_DESTROY
 * @param err receives the failure of the conversion
 * @return false when the conversion failed
 */
static bool to_native(char const *const src, wchar_t **const dst, struct ov_error *const err) {
  return ov_sprintf_wchar(dst, err, NULL, L"%s", src);
}

/**
 * @brief A wide string of the Win32 calls as the UTF-8 string this program works with
 *
 * @param src the string to convert
 * @param dst receives the copy, release with OV_ARRAY_DESTROY
 * @param err receives the failure of the conversion
 * @return false when the conversion failed
 */
static bool to_utf8(WCHAR const *const src, char **const dst, struct ov_error *const err) {
  return ov_sprintf_char(dst, err, NULL, "%ls", src);
}

/**
 * @brief Count the entries of a multi-sz list
 *
 * @param buf the list, it is not terminated beyond len
 * @param len how many characters the list holds
 * @return how many entries the list has
 */
static size_t multisz_count(wchar_t const *const buf, size_t const len) {
  size_t count = 0;
  for (size_t i = 0; i < len;) {
    size_t const slen = wcslen(&buf[i]);
    if (slen == 0) {
      i += 1;
      continue;
    }
    count++;
    i += slen + 1;
  }
  return count;
}

/**
 * @brief What one attempt to read a CM_* list answered
 *
 * The CM_* list calls can race against a list that grows between the size query and the
 * read, so one attempt reports whether it is done or has to be retried.
 */
enum attempt_result {
  ATTEMPT_DONE,   //!< the list was read
  ATTEMPT_RETRY,  //!< the list grew between the size query and the read
  ATTEMPT_FAILED, //!< the call failed, err says why
};

/**
 * @brief Count the interfaces of one category that a devnode offers, one attempt
 *
 * The buffer is owned by the caller and reused by the next attempt.
 *
 * @param category the KS category to ask for
 * @param devnode the device to ask
 * @param flags what CM_Get_Device_Interface_ListW takes
 * @param buf the list buffer, grown when the list is bigger
 * @param out_count receives how many interfaces are in the list
 * @param err receives the failure of the call
 * @return ATTEMPT_RETRY when the list grew between the size query and the read
 */
static enum attempt_result interface_count_once(GUID const *const category,
                                                WCHAR const *const devnode,
                                                ULONG const flags,
                                                wchar_t **const buf,
                                                size_t *const out_count,
                                                struct ov_error *const err) {
  ULONG len = 0;
  GUID cls = *category; // CM_* wants GUID*, so take a local copy instead of casting away const.
  CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&len, &cls, (WCHAR *)ov_deconster_(devnode), flags);

  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Get_Device_Interface_List_SizeW", cr);
    return ATTEMPT_FAILED;
  }
  if (len == 0) {
    *out_count = 0;
    return ATTEMPT_DONE;
  }
  if (!OV_REALLOC(buf, (size_t)len, sizeof((*buf)[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return ATTEMPT_FAILED;
  }
  cr = CM_Get_Device_Interface_ListW(&cls, (WCHAR *)ov_deconster_(devnode), *buf, len, flags);
  if (cr == CR_BUFFER_SMALL) {
    return ATTEMPT_RETRY;
  }
  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Get_Device_Interface_ListW", cr);
    return ATTEMPT_FAILED;
  }
  *out_count = multisz_count(*buf, (size_t)len);
  return ATTEMPT_DONE;
}

/**
 * @brief Count the interfaces of one category that a devnode offers
 *
 * One attempt is repeated while the list keeps growing.
 *
 * @param category the KS category to ask for
 * @param devnode the device to ask
 * @param flags what CM_Get_Device_Interface_ListW takes
 * @param out_count receives how many interfaces are in the list
 * @param err receives the failure of the call
 * @return false when the count could not be read
 */
static bool interface_count(
    GUID const *const category, WCHAR const *const devnode, ULONG const flags, size_t *const out_count, struct ov_error *const err) {
  bool success = false;
  bool failed = false;
  wchar_t *buf = NULL;

  for (int attempt = 0; attempt < 5; attempt++) {
    enum attempt_result const result = interface_count_once(category, devnode, flags, &buf, out_count, err);
    if (result == ATTEMPT_FAILED) {
      failed = true;
      break;
    }
    if (result == ATTEMPT_DONE) {
      success = true;
      break;
    }
  }
  if (!success && !failed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
    goto cleanup;
  }

cleanup:
  OV_FREE(&buf);
  buf = NULL;
  return success;
}

/**
 * @brief What the walk over the devnodes carries with it
 */
struct found_ctx {
  char const *instance_prefix; // UTF-8, NULL means no prefix filter
  char **out_devnode;
  bool found;
};

/**
 * @brief Does a devnode sit under the prefix the settings name
 *
 * @param devnode the device to look at
 * @param instance_prefix the prefix to look for, NULL means no filter
 * @param out_matched receives the answer
 * @param err receives the failure of the conversion
 * @return false on such a failure
 */
static bool
prefix_matches(WCHAR const *const devnode, char const *const instance_prefix, bool *const out_matched, struct ov_error *const err) {
  bool success = false;
  char *utf8 = NULL;
  size_t plen = 0;

  *out_matched = false;
  if (instance_prefix == NULL) {
    *out_matched = true;
    success = true;
    goto cleanup;
  }
  if (!to_utf8(devnode, &utf8, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  plen = strlen(instance_prefix);
  if (plen == 0) {
    *out_matched = true;
  } else {
    *out_matched = (_strnicmp(utf8, instance_prefix, plen) == 0);
  }
  success = true;

cleanup:
  if (utf8) {
    OV_ARRAY_DESTROY(&utf8);
  }
  return success;
}

/**
 * @brief Call on_devnode for every entry of a multi-sz list
 *
 * It holds no resources of its own, so a refused callback only ends the walk.
 *
 * @param buf the list to walk
 * @param len how many characters the list holds
 * @param on_devnode the callback that looks at one device
 * @param ctx handed back to on_devnode
 * @param err receives the refusal of the callback
 * @return false when the callback refused one of the entries
 */
static bool walk_devnodes(wchar_t const *const buf,
                          size_t const len,
                          bool (*on_devnode)(WCHAR const *const devnode, void *const ctx, struct ov_error *const err),
                          void *const ctx,
                          struct ov_error *const err) {
  for (size_t i = 0; i < len;) {
    size_t const slen = wcslen(&buf[i]);
    if (slen == 0) {
      i += 1;
      continue;
    }
    if (!on_devnode(&buf[i], ctx, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    i += slen + 1;
  }
  return true;
}

/**
 * @brief Enumerate the present devnodes of a service, one attempt
 *
 * The buffer is owned by the caller and reused by the next attempt.
 *
 * @param filter the service to enumerate, NULL for every one
 * @param on_devnode the callback that looks at one device
 * @param ctx handed back to on_devnode
 * @param buf the list buffer, grown when the list is bigger
 * @param err receives the failure of the call
 * @return ATTEMPT_RETRY when the list grew between the size query and the read
 */
static enum attempt_result devnode_list_once(wchar_t const *const filter,
                                             bool (*on_devnode)(WCHAR const *const devnode, void *const ctx, struct ov_error *const err),
                                             void *const ctx,
                                             wchar_t **const buf,
                                             struct ov_error *const err) {
  ULONG len = 0;
  CONFIGRET cr = CM_Get_Device_ID_List_SizeW(&len, filter, CM_GETIDLIST_FILTER_SERVICE | CM_GETIDLIST_FILTER_PRESENT);

  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Get_Device_ID_List_SizeW", cr);
    return ATTEMPT_FAILED;
  }
  if (len == 0) {
    return ATTEMPT_DONE;
  }
  if (!OV_REALLOC(buf, (size_t)len, sizeof((*buf)[0]))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return ATTEMPT_FAILED;
  }
  cr = CM_Get_Device_ID_ListW(filter, *buf, len, CM_GETIDLIST_FILTER_SERVICE | CM_GETIDLIST_FILTER_PRESENT);
  if (cr == CR_BUFFER_SMALL) {
    return ATTEMPT_RETRY;
  }
  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Get_Device_ID_ListW", cr);
    return ATTEMPT_FAILED;
  }
  return walk_devnodes(*buf, (size_t)len, on_devnode, ctx, err) ? ATTEMPT_DONE : ATTEMPT_FAILED;
}

/**
 * @brief Call on_devnode for every present devnode of a service
 *
 * One attempt is repeated while the list keeps growing.
 *
 * @param service the service whose devices are looked at
 * @param on_devnode the callback that looks at one device
 * @param ctx handed back to on_devnode
 * @param err receives the failure of the call or the refusal of the callback
 * @return false when the walk could not be completed
 */
static bool foreach_devnode(char const *const service,
                            bool (*on_devnode)(WCHAR const *const devnode, void *const ctx, struct ov_error *const err),
                            void *const ctx,
                            struct ov_error *const err) {
  bool success = false;
  bool failed = false;
  wchar_t *buf = NULL;
  wchar_t *filter = NULL;

  if (!to_native(service, &filter, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  for (int attempt = 0; attempt < 5; attempt++) {
    enum attempt_result const result = devnode_list_once(filter, on_devnode, ctx, &buf, err);
    if (result == ATTEMPT_FAILED) {
      failed = true;
      break;
    }
    if (result == ATTEMPT_DONE) {
      success = true;
      break;
    }
  }
  if (!success && !failed) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_unexpected);
  }

cleanup:
  OV_FREE(&buf);
  if (filter) {
    OV_ARRAY_DESTROY(&filter); // made by ov_sprintf_wchar
  }
  return success;
}

/**
 * @brief Take the first devnode that fits the prefix filter
 *
 * @param devnode the device the walk found
 * @param ctx_void a struct found_ctx
 * @param err receives the failure of the checks
 * @return false to stop the walk with an error
 */
static bool on_devnode_copy(WCHAR const *const devnode, void *const ctx_void, struct ov_error *const err) {
  struct found_ctx *ctx = (struct found_ctx *)ctx_void;
  bool success = false;
  char *utf8 = NULL;

  if (ctx->found) {
    return true;
  }
  bool matched = false;
  if (!prefix_matches(devnode, ctx->instance_prefix, &matched, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!matched) {
    success = true; // not our device, keep walking
    goto cleanup;
  }
  if (!to_utf8(devnode, &utf8, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  *ctx->out_devnode = utf8;
  utf8 = NULL;
  ctx->found = true;
  success = true;

cleanup:
  if (utf8) {
    OV_ARRAY_DESTROY(&utf8);
  }
  return success;
}

/**
 * @brief First present devnode of the KS service
 *
 * @param service DEVPKEY_Device_Service of the KS devnode
 * @param instance_prefix only devices under this prefix are looked at, NULL means no filter
 * @param err receives the failure of the query
 * @return the devnode in UTF-8, NULL when no device of the service is present
 * @note Release with device_string_free.
 */
char *device_ks_devnode(char const *const service, char const *const instance_prefix, struct ov_error *const err) {
  char *result = NULL;
  struct found_ctx ctx;
  bool success = false;

  if (!service) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  ctx.instance_prefix = instance_prefix;
  ctx.out_devnode = &result;
  ctx.found = false;
  if (!foreach_devnode(service, on_devnode_copy, &ctx, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    device_string_free(&result);
  }
  return success ? result : NULL;
}

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void device_string_free(char **const value) {
  if (value && *value) {
    OV_ARRAY_DESTROY(value);
  }
}

/**
 * @brief What the probe collects about the device
 */
struct probe_ctx {
  char const *instance_prefix;
  unsigned long problem_code;
  size_t render_present;
  size_t render_registered;
  size_t capture_present;
  size_t capture_registered;
  bool evaluated;
};

/**
 * @brief Measure one devnode and keep what device_evaluate needs
 *
 * @param devnode the device the walk found
 * @param ctx_void a struct probe_ctx
 * @param err receives the failure of the checks
 * @return false to stop the walk with an error
 */
static bool on_devnode_probe(WCHAR const *const devnode, void *const ctx_void, struct ov_error *const err) {
  struct probe_ctx *ctx = (struct probe_ctx *)ctx_void;
  bool success = false;
  DEVINST devinst = 0;
  ULONG status = 0;
  ULONG problem = 0;
  CONFIGRET cr = 0;

  if (ctx->evaluated) {
    return true;
  }
  bool matched = false;
  if (!prefix_matches(devnode, ctx->instance_prefix, &matched, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!matched) {
    success = true; // not our device, keep walking
    goto cleanup;
  }

  cr = CM_Locate_DevNodeW(&devinst, (WCHAR *)ov_deconster_(devnode), CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Locate_DevNodeW", cr);
    goto cleanup;
  }
  cr = CM_Get_DevNode_Status(&status, &problem, devinst, 0);
  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Get_DevNode_Status", cr);
    goto cleanup;
  }
  if (!interface_count(&kKsCategoryRender, devnode, CM_GET_DEVICE_INTERFACE_LIST_PRESENT, &ctx->render_present, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!interface_count(&kKsCategoryRender, devnode, CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES, &ctx->render_registered, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!interface_count(&kKsCategoryCapture, devnode, CM_GET_DEVICE_INTERFACE_LIST_PRESENT, &ctx->capture_present, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!interface_count(&kKsCategoryCapture, devnode, CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES, &ctx->capture_registered, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  ctx->problem_code = problem;
  ctx->evaluated = true;
  success = true;

cleanup:
  return success;
}

/**
 * @brief Look at the device and report what state it is in
 *
 * Detection only.  Never modifies the device.
 *
 * @param service DEVPKEY_Device_Service of the KS devnode
 * @param instance_prefix only devices under this prefix are looked at, NULL means no filter
 * @param err receives the failure of the checks; a state that is neither healthy nor
 *            faulted always carries one
 * @return the state the dialog shows
 */
enum device_state device_probe(char const *const service, char const *const instance_prefix, struct ov_error *const err) {
  struct probe_ctx ctx;
  enum device_state state = DEVICE_STATE_UNKNOWN;

  if (!service) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return DEVICE_STATE_UNKNOWN;
  }
  ctx.instance_prefix = instance_prefix;
  ctx.problem_code = 0;
  ctx.render_present = 0;
  ctx.render_registered = 0;
  ctx.capture_present = 0;
  ctx.capture_registered = 0;
  ctx.evaluated = false;
  if (!foreach_devnode(service, on_devnode_probe, &ctx, err)) {
    OV_ERROR_ADD_TRACE(err);
    return DEVICE_STATE_UNKNOWN;
  }
  if (!ctx.evaluated) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "no devnode of service %1$s is registered",
                  "no devnode of service %1$s is registered",
                  service);
    return DEVICE_STATE_ABSENT;
  }
  state = device_evaluate(ctx.problem_code, ctx.render_present, ctx.render_registered, ctx.capture_present, ctx.capture_registered);
  if (state == DEVICE_STATE_PROBLEM) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "devnode of service %1$s has problem code 0x%2$08lx",
                  "devnode of service %1$s has problem code 0x%2$08lx",
                  service,
                  ctx.problem_code);
  } else if (state == DEVICE_STATE_UNKNOWN) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "no KS interface of service %1$s is registered",
                  "no KS interface of service %1$s is registered",
                  service);
  }
  return state;
}
