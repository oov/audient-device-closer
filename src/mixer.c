#include "mixer.h"

#include <string.h>

#include <ovarray.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "logger.h"
#include "process.h"

/**
 * @brief Take the identity of the process the record named
 *
 * @param pid the process the record named
 * @param exe_name the image name the caller expects
 * @param event_time_ms the moment of the veto record
 * @param out_found false when no instance matched, true when out carries one
 * @param out the identity to act on, release with mixer_target_release
 * @param err receives a hard failure, not a mismatch
 * @return false only on a hard failure
 */
bool mixer_take(uint32_t const pid,
                char const *const exe_name,
                long long const event_time_ms,
                bool *const out_found,
                struct mixer_target *const out,
                struct ov_error *const err) {
  bool success = false;
  struct process_ref ref;
  char *image = NULL;

  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  memset(out, 0, sizeof(*out));
  if ((exe_name == NULL) || (out_found == NULL) || (out == NULL) || (pid == 0)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  *out_found = false;

  if (!process_ref_open(&ref, pid, false, NULL)) {
    // a pid the record named may well be gone by now: that is not the failure of the
    // caller, so the reason goes to the log and the answer stays "no blocker here"
    logger_notef("process", "reject pid=%lu start=0 image=(unknown) reason=the process could not be opened", (unsigned long)pid);
    success = true;
    goto cleanup;
  }
  if (!process_ref_alive(&ref)) {
    logger_notef(
        "process", "reject pid=%lu start=%lld image=(unknown) reason=the process is already gone", (unsigned long)pid, ref.start_ms_utc);
    success = true;
    goto cleanup;
  }
  image = process_ref_image_path(&ref, NULL);
  if (image == NULL) {
    logger_notef("process",
                 "reject pid=%lu start=%lld image=(unreadable) reason=the image of the process could not be read",
                 (unsigned long)pid,
                 ref.start_ms_utc);
    success = true;
    goto cleanup;
  }
  if (!process_path_matches_name(image, exe_name)) {
    logger_notef("process",
                 "reject pid=%lu start=%lld image=%s reason=the image name is not the one the caller asked for",
                 (unsigned long)pid,
                 ref.start_ms_utc,
                 image);
    success = true;
    goto cleanup;
  }
  if (!process_start_matches_event_time(ref.start_ms_utc, event_time_ms)) {
    logger_notef("process",
                 "reject pid=%lu start=%lld image=%s reason=the process is younger than the event, so the pid was reused",
                 (unsigned long)pid,
                 ref.start_ms_utc,
                 image);
    success = true;
    goto cleanup;
  }
  out->pid = ref.pid;
  out->path = image;
  image = NULL;
  *out_found = true;
  logger_notef("process",
               "accept pid=%lu start=%lld image=%s reason=the event names this instance",
               (unsigned long)pid,
               ref.start_ms_utc,
               out->path);
  success = true;

cleanup:
  process_string_free(&image);
  if (!success) {
    mixer_target_release(out);
  }
  process_ref_close(&ref);
  return success;
}

/**
 * @brief Terminate the taken instance and wait until it is gone
 *
 * The saved path and command line stay usable afterwards, so the caller can restart it from
 * them.
 *
 * @param target the instance mixer_take found
 * @param err receives the failure of the termination
 * @return false when the process could not be closed
 */
bool mixer_close(struct mixer_target *const target, struct ov_error *const err) {
  bool success = false;
  bool gone = false;
  struct process_ref ref;
  char *image = NULL;

  memset(&ref, 0, sizeof(ref));
  ref.handle = NULL;
  if ((target == NULL) || (target->pid == 0)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }

  if (!process_ref_open(&ref, target->pid, true, err)) {
    logger_notef("process",
                 "reject pid=%lu start=0 image=%s reason=the process could not be opened for termination",
                 (unsigned long)target->pid,
                 target->path ? target->path : "(unknown)");
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  image = process_ref_image_path(&ref, err);
  if (image == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if ((target->path != NULL) && (_stricmp(image, target->path) != 0)) {
    logger_notef("process",
                 "reject pid=%lu start=%lld image=%s reason=the handle belongs to another image than the one taken",
                 (unsigned long)target->pid,
                 ref.start_ms_utc,
                 image);
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "the process behind pid %1$lu is no longer the one that was taken",
                  "the process behind pid %1$lu is no longer the one that was taken",
                  (unsigned long)target->pid);
    goto cleanup;
  }
  if (!process_ref_terminate(&ref, err)) {
    logger_notef("process",
                 "reject pid=%lu start=%lld image=%s reason=the termination was refused",
                 (unsigned long)target->pid,
                 ref.start_ms_utc,
                 image);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!process_ref_wait(&ref, 5000, &gone, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!gone) {
    OV_ERROR_SET(err, ov_error_type_generic, ov_error_generic_fail, "the process did not exit within 5 seconds");
    goto cleanup;
  }
  logger_notef("process", "closed pid=%lu start=%lld image=%s", (unsigned long)target->pid, ref.start_ms_utc, image);
  target->pid = 0;
  success = true;

cleanup:
  process_string_free(&image);
  process_ref_close(&ref);
  return success;
}

/**
 * @brief Release what mixer_take kept
 *
 * @param target NULL is allowed
 */
void mixer_target_release(struct mixer_target *const target) {
  if (target == NULL) {
    return;
  }
  process_string_free(&target->path);
  target->pid = 0;
}
