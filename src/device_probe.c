#include "device_probe.h"

#include <windows.h>

#include "device_manager.h"

#include <string.h>
#include <wchar.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

/**
 * @brief The state the facts stand for
 *
 * The one place that says what the facts mean, so the machine can report them without
 * deciding anything.
 *
 * @param facts what the machine reported about the devnode
 * @return the state of the device
 */
enum device_state device_facts_state(struct device_facts const *const facts) {
  if (facts->problem_code == DEVICE_PROBLEM_NO_REPORT) {
    return DEVICE_STATE_QUERY_FAILED;
  }
  if (facts->problem_code != 0) {
    return DEVICE_STATE_PROBLEM;
  }
  if (facts->endpoint_present) {
    return DEVICE_STATE_HEALTHY;
  }
  if (facts->endpoint_registered) {
    return DEVICE_STATE_FAULTED;
  }
  return DEVICE_STATE_NO_INTERFACE;
}

bool device_facts_is_healthy(struct device_facts const *const facts) { return device_facts_state(facts) == DEVICE_STATE_HEALTHY; }

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
 * @param devnode instance ID of the devnode
 * @param id the identifier of a device or of a unit
 * @return true when devnode and id name the same device
 */
static bool device_is_same(wchar_t const *const devnode, wchar_t const *const id) {
  if ((devnode == NULL) || (devnode[0] == L'\0') || (id == NULL) || (id[0] == L'\0')) {
    return false;
  }

  size_t delimiters = 0;
  for (size_t i = 0; id[i] != L'\0'; ++i) {
    if (id[i] == L'\\') {
      ++delimiters;
    }
  }

  size_t const id_len = wcslen(id);
  if (wcslen(devnode) < id_len) {
    return false;
  }

  // An instance ID is `Enumerator\DeviceID\InstanceID`, as in
  // `TUSBAUDIO_ENUM\VID_2708&PID_0008&KS\6&D6CB2A&0&2`: the identifier without the last
  // part, like `TUSBAUDIO_ENUM\VID_2708&PID_0008&KS`, names the whole device, so every
  // unit of it is a match.  With the last part it names one unit, so only that one is.
  bool const is_instance_id = delimiters > 1;
  if (is_instance_id && (wcslen(devnode) != id_len)) {
    return false;
  }

  // the case is folded the way the system folds it, and only the first id_len characters of
  // the devnode take part, which is what "starts with" means for the system
  return CompareStringOrdinal(devnode, (int)id_len, id, (int)id_len, TRUE) == CSTR_EQUAL;
}

/**
 * @brief Does a devnode belong to one of the devices the settings name
 *
 * A devnode sits under the instance identifier of its device, so the devnode is that
 * device's when it starts with the identifier.
 *
 * @param devnode the device to look at
 * @param devices the devices to look for, may not be NULL or empty
 * @param device_count how many devices the list holds
 * @return true when the devnode belongs to one of them
 */
static bool matches(wchar_t const *const devnode, struct device_entry const *const devices, size_t const device_count) {
  for (size_t i = 0; i < device_count; ++i) {
    if (device_is_same(devnode, devices[i].id)) {
      return true;
    }
  }
  return false;
}

NODISCARD size_t device_find_entry(wchar_t const *const devnode,
                                   struct device_entry const *const devices,
                                   size_t const device_count,
                                   struct ov_error *const err) {
  if ((devnode == NULL) || (devnode[0] == L'\0')) {
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
 * @brief State one devnode the machine reports on
 *
 * @param devnode instance ID of the KS devnode
 * @param out_code receives the problem the machine named, DEVICE_PROBLEM_NO_REPORT when it could not be asked
 * @param err receives the failure of the question
 * @return the state the devnode is in, DEVICE_STATE_QUERY_FAILED when it could not be asked
 */
static NODISCARD enum device_state state_of(wchar_t const *const devnode, uint32_t *const out_code, struct ov_error *const err) {
  struct device_facts facts = {0};

  *out_code = DEVICE_PROBLEM_NO_REPORT;
  if (!device_manager()->facts_of(devnode, &facts, err)) {
    OV_ERROR_ADD_TRACE(err);
    return DEVICE_STATE_QUERY_FAILED;
  }
  *out_code = facts.problem_code;
  return device_facts_state(&facts);
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
static NODISCARD bool on_devnode_collect(wchar_t const *const devnode, void *const ctx_void, struct ov_error *const err) {
  struct targets_ctx *ctx = (struct targets_ctx *)ctx_void;
  bool success = false;
  struct device_target t;

  memset(&t, 0, sizeof(t));
  if (!matches(devnode, ctx->devices, ctx->device_count)) {
    success = true; // not our device, keep walking
    goto cleanup;
  }
  t.state = state_of(devnode, &t.problem_code, err);
  if (t.state == DEVICE_STATE_QUERY_FAILED) {
    goto cleanup;
  }
  {
    size_t const chars = wcslen(devnode) + 1;

    if (!OV_ARRAY_GROW(&t.devnode, chars)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    OV_ARRAY_SET_LENGTH(t.devnode, chars - 1);
    wcscpy(t.devnode, devnode);
  }
  if (!OV_ARRAY_PUSH(&ctx->targets, t)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  t.devnode = NULL;
  success = true;

cleanup:
  if (t.devnode != NULL) {
    OV_ARRAY_DESTROY(&t.devnode);
  }
  return success;
}

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

NODISCARD struct device_target *device_probe_all(wchar_t const *const service,
                                                 struct device_entry const *const devices,
                                                 size_t const device_count,
                                                 struct ov_error *const err) {
  struct targets_ctx ctx;
  struct device_target *result = NULL;

  ctx.targets = NULL;
  if (service == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  ctx.devices = devices;
  ctx.device_count = device_count;
  if (!device_manager()->foreach_devnode(service, on_devnode_collect, &ctx, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (OV_ARRAY_LENGTH(ctx.targets) == 0) {
    // no device of the service is present, which is not a failure of the call
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
    goto cleanup;
  }
  result = ctx.targets;
  ctx.targets = NULL;

cleanup:
  device_targets_release(&ctx.targets);
  return result;
}

NODISCARD enum device_state device_probe_devnode(wchar_t const *const devnode, struct ov_error *const err) {
  uint32_t problem_code = DEVICE_PROBLEM_NO_REPORT;

  if ((devnode == NULL) || (devnode[0] == L'\0')) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return DEVICE_STATE_QUERY_FAILED;
  }
  return state_of(devnode, &problem_code, err);
}

/**
 * @brief How deep the ancestors of a KS devnode are followed
 *
 * A KS devnode sits under the audio function of a USB composite device, so the removal
 * record of the USB device is an ancestor a few levels up, not the devnode itself.
 */
enum { kKsAncestorDepth = 4 };

NODISCARD ov_tribool device_veto_event_names_ancestor(wchar_t const *const device_instance,
                                                      wchar_t const *const devnode,
                                                      struct ov_error *const err) {
  ov_tribool result = ov_indeterminate;
  // one buffer for the whole walk: parent_of writes into it and grows it only when the
  // instance above is longer than the one before, so nothing is taken or given back inside
  // the loop
  wchar_t *current = NULL;

  if ((device_instance == NULL) || (device_instance[0] == L'\0') || (devnode == NULL) || (devnode[0] == L'\0')) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!ov_sprintf_wchar(&current, err, NULL, L"%ls", devnode)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  result = ov_false;
  for (size_t level = 0; level < (size_t)kKsAncestorDepth; level++) {
    ov_tribool const has_parent = device_manager()->parent_of(current, &current, err);

    if (has_parent == ov_indeterminate) {
      OV_ERROR_ADD_TRACE(err);
      result = ov_indeterminate;
      goto cleanup;
    }
    if (has_parent == ov_false) {
      break; // the walk reached the top of the tree
    }
    if (device_is_same(current, device_instance)) {
      result = ov_true;
      break;
    }
  }

cleanup:
  if (current != NULL) {
    OV_ARRAY_DESTROY(&current);
  }
  return result;
}
