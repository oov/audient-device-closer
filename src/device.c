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
static enum device_state device_evaluate(
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
  return DEVICE_STATE_NO_INTERFACE;
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
 * @brief The letter a byte of the ASCII stands for
 *
 * Only the letters of the ASCII have a case; every other
 * byte stands for itself, so the two spellings of a letter
 * fold onto the one letter.
 *
 * @param c the byte to fold
 * @return the letter of c, in the one case the folding has
 */
static char ascii_letter(char const c) {
  if ((c >= 'A') && (c <= 'Z')) {
    return (char)(c - 'A' + 'a');
  }
  return c;
}

/**
 * @brief Does a string start with a prefix, the case left out
 *
 * @param s the string to look at, may not be NULL
 * @param prefix the prefix to look for, may not be NULL
 * @return true when s starts with prefix
 */
static bool ascii_starts_with(char const *const s, char const *const prefix) {
  for (size_t i = 0; prefix[i] != '\0'; ++i) {
    if (ascii_letter(s[i]) != ascii_letter(prefix[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Are two strings the same one, the case left out
 *
 * @param a the first string to compare, may not be NULL
 * @param b the second string to compare, may not be NULL
 * @return true when both strings end on the same letters
 */
static bool ascii_equals(char const *const a, char const *const b) {
  size_t i = 0;

  while ((a[i] != '\0') && (b[i] != '\0')) {
    if (ascii_letter(a[i]) != ascii_letter(b[i])) {
      return false;
    }
    ++i;
  }
  return a[i] == b[i]; // both ended, or one still has letters
}

/**
 * @brief Does a devnode name the device an identifier names
 *
 * An identifier of a device, the one without the unit, names
 * every unit of it: the devnode is that device's when it
 * starts with the identifier.  An identifier of one unit
 * names that unit alone: the devnode is that unit's when it
 * is the identifier, so a devnode of a namesake unit of the
 * same device is not the one the identifier names.
 *
 * @param devnode instance ID of the devnode, UTF-8
 * @param id the identifier of a device or of a unit, UTF-8
 * @return true when devnode and id name the same device
 */
bool device_is_same(char const *const devnode, char const *const id) {
  size_t units = 0;

  if ((devnode == NULL) || (devnode[0] == '\0') || (id == NULL) || (id[0] == '\0')) {
    return false;
  }
  for (size_t i = 0; id[i] != '\0'; ++i) {
    if (id[i] == '\\') {
      ++units;
    }
  }
  if (units > 1) {
    // the identifier carries the unit, so only that unit is the one
    return ascii_equals(devnode, id);
  }
  // the identifier names the device, so every unit of it is the one
  return ascii_starts_with(devnode, id);
}

/**
 * @brief Does a devnode belong to one of the devices the settings name
 *
 * A devnode sits under the instance identifier of its device, so the devnode is that
 * device's when it starts with the identifier.  A device list with no entries keeps
 * every devnode of the service in play.
 *
 * @param devnode the device to look at
 * @param devices the devices to look for, NULL or empty means no filter
 * @param device_count how many devices the list holds
 * @param out_matched receives the answer
 * @param err receives the failure of the conversion
 * @return false on such a failure
 */
static bool device_matches(WCHAR const *const devnode,
                           struct device_entry const *const devices,
                           size_t const device_count,
                           bool *const out_matched,
                           struct ov_error *const err) {
  bool success = false;
  char *utf8 = NULL;

  *out_matched = false;
  if ((devices == NULL) || (device_count == 0)) {
    *out_matched = true;
    success = true;
    goto cleanup;
  }
  if (!to_utf8(devnode, &utf8, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  for (size_t i = 0; i < device_count; ++i) {
    if (device_is_same(utf8, devices[i].id)) {
      *out_matched = true;
      break;
    }
  }
  success = true;

cleanup:
  if (utf8) {
    OV_ARRAY_DESTROY(&utf8);
  }
  return success;
}

/**
 * @brief The entry of the list a devnode belongs to
 *
 * The identifier of an entry is a prefix of the devnode of its device,
 * so the devnode says which entry names it.  A devnode no entry names
 * is one the list does not cover.
 *
 * @param devnode instance ID of the KS devnode, UTF-8
 * @param devices the devices this program looks at, NULL or empty means
 *                no entry covers any devnode
 * @param device_count how many devices the list holds
 * @param err receives the refused arguments, or a not_found for a devnode no entry names
 * @return the index of the entry, SIZE_MAX when no entry names the devnode
 */
size_t device_find_entry(char const *const devnode,
                         struct device_entry const *const devices,
                         size_t const device_count,
                         struct ov_error *const err) {
  if ((devnode == NULL) || (devnode[0] == '\0')) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return SIZE_MAX;
  }
  if ((devices != NULL) && (device_count > 0)) {
    for (size_t i = 0; i < device_count; ++i) {
      if (device_is_same(devnode, devices[i].id)) {
        return i;
      }
    }
  }
  // a devnode no entry names is not a failure of the call, and the caller tells it apart
  // from one by this code
  OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
  return SIZE_MAX;
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
 * @brief Measure one devnode: its problem code and what its endpoints offer
 *
 * Detection only.  The numbers mean what device_evaluate reads.
 *
 * @param devnode the device to measure
 * @param out_problem_code receives the CM_PROB_* value of the devnode
 * @param out_render_present receives how many render endpoints are present
 * @param out_render_registered receives how many render endpoints are registered
 * @param out_capture_present receives how many capture endpoints are present
 * @param out_capture_registered receives how many capture endpoints are registered
 * @param err receives the failure of the queries
 * @return false when the devnode could not be measured
 */
static bool measure_devnode(WCHAR const *const devnode,
                            unsigned long *const out_problem_code,
                            size_t *const out_render_present,
                            size_t *const out_render_registered,
                            size_t *const out_capture_present,
                            size_t *const out_capture_registered,
                            struct ov_error *const err) {
  bool success = false;
  DEVINST devinst = 0;
  ULONG status = 0;
  ULONG problem = 0;
  CONFIGRET cr = 0;

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
  if (!interface_count(&kKsCategoryRender, devnode, CM_GET_DEVICE_INTERFACE_LIST_PRESENT, out_render_present, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!interface_count(&kKsCategoryRender, devnode, CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES, out_render_registered, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!interface_count(&kKsCategoryCapture, devnode, CM_GET_DEVICE_INTERFACE_LIST_PRESENT, out_capture_present, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!interface_count(&kKsCategoryCapture, devnode, CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES, out_capture_registered, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  *out_problem_code = problem;
  success = true;

cleanup:
  return success;
}

/**
 * @brief What the walk over the devnodes collects for every connected device
 */
struct targets_ctx {
  struct device_entry const *devices; // the devices this program looks at
  size_t device_count;
  struct device_target *targets; // the list that fills up
};

/**
 * @brief Measure one devnode and file it with its state
 *
 * @param devnode the device the walk found
 * @param ctx_void a struct targets_ctx
 * @param err receives the failure of the checks
 * @return false to stop the walk with an error
 */
static bool on_devnode_collect(WCHAR const *const devnode, void *const ctx_void, struct ov_error *const err) {
  struct targets_ctx *ctx = (struct targets_ctx *)ctx_void;
  bool success = false;
  bool matched = false;
  unsigned long problem = 0;
  size_t render_present = 0;
  size_t render_registered = 0;
  size_t capture_present = 0;
  size_t capture_registered = 0;
  struct device_target t;
  char *utf8 = NULL;

  memset(&t, 0, sizeof(t));
  if (!device_matches(devnode, ctx->devices, ctx->device_count, &matched, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!matched) {
    success = true; // not our device, keep walking
    goto cleanup;
  }
  if (!measure_devnode(devnode, &problem, &render_present, &render_registered, &capture_present, &capture_registered, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!to_utf8(devnode, &utf8, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  t.devnode = utf8;
  utf8 = NULL;
  t.state = device_evaluate(problem, render_present, render_registered, capture_present, capture_registered);
  if (!OV_ARRAY_PUSH(&ctx->targets, t)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  success = true;

cleanup:
  if (utf8) {
    OV_ARRAY_DESTROY(&utf8);
  }
  return success;
}

/**
 * @brief Release the list of the targets
 *
 * @param targets set to NULL afterwards, NULL is allowed
 */
void device_targets_release(struct device_target **const targets) {
  size_t i = 0;
  size_t n = 0;

  if (targets == NULL) {
    return;
  }
  n = OV_ARRAY_LENGTH(*targets);
  for (i = 0; i < n; i++) {
    OV_ARRAY_DESTROY(&(*targets)[i].devnode);
  }
  if (*targets) {
    OV_ARRAY_DESTROY(targets);
  }
}

void device_targets_keep_faulted(struct device_target **const targets) {
  size_t kept = 0;
  size_t i = 0;
  size_t n = 0;

  if (targets == NULL) {
    return;
  }
  n = OV_ARRAY_LENGTH(*targets);
  for (i = 0; i < n; i++) {
    if ((*targets)[i].state == DEVICE_STATE_FAULTED) {
      (*targets)[kept++] = (*targets)[i];
    } else {
      OV_ARRAY_DESTROY(&(*targets)[i].devnode);
    }
  }
  OV_ARRAY_SET_LENGTH(*targets, kept);
}

/**
 * @brief Enumerate the present devnodes of the service and state each one
 *
 * @param service DEVPKEY_Device_Service of the KS devnodes
 * @param devices the devices this program looks at, may not be NULL or empty here
 * @param device_count how many devices the list holds
 * @param err receives the failure of the queries, or a not_found for a service no device
 *            of it is present
 * @return the list of the targets, NULL when no device is present or the queries failed;
 *         release with device_targets_release
 */
struct device_target *device_probe_all(char const *const service,
                                       struct device_entry const *const devices,
                                       size_t const device_count,
                                       struct ov_error *const err) {
  struct targets_ctx ctx;

  ctx.targets = NULL;
  if (service == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  ctx.devices = devices;
  ctx.device_count = device_count;
  if (!foreach_devnode(service, on_devnode_collect, &ctx, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (OV_ARRAY_LENGTH(ctx.targets) == 0) {
    // no device of the service is present, which is not a failure of the call
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
    goto cleanup;
  }
  return ctx.targets;

cleanup:
  device_targets_release(&ctx.targets);
  return NULL;
}

/**
 * @brief State one devnode the caller already knows by name
 *
 * The state and the failure of the queries are told apart by the value the call answers
 * with, so a caller that has to act on the failure can do so.
 *
 * @param devnode instance ID of the KS devnode, UTF-8
 * @param err receives the failure of the queries
 * @return the state the devnode is in, DEVICE_STATE_NO_INTERFACE when no endpoint is
 *         registered for it, DEVICE_STATE_QUERY_FAILED when the queries failed
 */
enum device_state device_probe_devnode(char const *const devnode, struct ov_error *const err) {
  enum device_state state = DEVICE_STATE_QUERY_FAILED;
  wchar_t *native = NULL;
  unsigned long problem = 0;
  size_t render_present = 0;
  size_t render_registered = 0;
  size_t capture_present = 0;
  size_t capture_registered = 0;

  if ((devnode == NULL) || (devnode[0] == '\0')) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!to_native(devnode, &native, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!measure_devnode(native, &problem, &render_present, &render_registered, &capture_present, &capture_registered, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  state = device_evaluate(problem, render_present, render_registered, capture_present, capture_registered);

cleanup:
  if (native) {
    OV_ARRAY_DESTROY(&native);
  }
  return state;
}

/**
 * @brief How deep the ancestors of a KS devnode are followed
 *
 * A KS devnode sits under the audio function of a USB composite device, so the removal
 * record of the USB device is an ancestor a few levels up, not the devnode itself.
 */
enum { kKsAncestorDepth = 4 };

/**
 * @brief Does the removal record name one of the ancestors of a devnode
 *
 * @param device_instance the DeviceInstance of the removal record, UTF-8, may not be NULL
 * @param devnode instance ID of the KS devnode, UTF-8, may not be NULL
 * @param err receives the failure of the queries
 * @return ov_true when the record names an ancestor, ov_false when it does not,
 *         ov_indeterminate when the ancestors could not be walked, with err set
 */
ov_tribool device_veto_event_names_ancestor(char const *const device_instance, char const *const devnode, struct ov_error *const err) {
  ov_tribool result = ov_indeterminate;
  wchar_t *native = NULL;
  WCHAR id[MAX_DEVICE_ID_LEN];
  DEVINST devinst = 0;
  CONFIGRET cr = 0;
  size_t level = 0;

  if ((device_instance == NULL) || (device_instance[0] == '\0') || (devnode == NULL) || (devnode[0] == '\0')) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!to_native(devnode, &native, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  cr = CM_Locate_DevNodeW(&devinst, native, CM_LOCATE_DEVNODE_NORMAL);
  if (cr != CR_SUCCESS) {
    SET_CONFIGRET_ERROR(err, "CM_Locate_DevNodeW", cr);
    goto cleanup;
  }
  result = ov_false;
  for (level = 0; level < (size_t)kKsAncestorDepth; level++) {
    DEVINST parent = 0;
    char *id_utf8 = NULL;

    cr = CM_Get_Parent(&parent, devinst, 0);
    if (cr != CR_SUCCESS) {
      // the root has no parent; the walk over the ancestors has to stop for that reason and
      // not with an error
      if (level > 0) {
        break;
      }
      SET_CONFIGRET_ERROR(err, "CM_Get_Parent", cr);
      result = ov_indeterminate;
      goto cleanup;
    }
    cr = CM_Get_Device_IDW(parent, id, MAX_DEVICE_ID_LEN, 0);
    if (cr != CR_SUCCESS) {
      SET_CONFIGRET_ERROR(err, "CM_Get_Device_IDW", cr);
      result = ov_indeterminate;
      goto cleanup;
    }
    if (!to_utf8(id, &id_utf8, err)) {
      OV_ERROR_ADD_TRACE(err);
      result = ov_indeterminate;
      goto cleanup;
    }
    if (device_is_same(id_utf8, device_instance)) {
      OV_ARRAY_DESTROY(&id_utf8);
      result = ov_true;
      break;
    }
    OV_ARRAY_DESTROY(&id_utf8);
    devinst = parent;
  }

cleanup:
  if (native) {
    OV_ARRAY_DESTROY(&native);
  }
  return result;
}
