#include "blocker.h"

#include <windows.h>

#include <tlhelp32.h>

#include <string.h>

#include <ovarray.h>
#include <ovprintf_ex.h>

#include "logger.h"
#include "process.h"
#include "veto_event.h"

/**
 * @brief Does this image name belong to the mixer the settings name
 *
 * @param image the image name a snapshot of the running processes reports, UTF-8
 * @param names the image names the settings carry, may be NULL
 * @param name_count how many names that is
 * @return true when one of the names is this image name
 */
static bool is_mixer_image(char const *const image, char const *const *const names, size_t const name_count) {
  size_t i = 0;

  if ((names == NULL) || (image == NULL)) {
    return false;
  }
  for (i = 0; i < name_count; i++) {
    if (process_path_matches_name(image, names[i])) {
      return true;
    }
  }
  return false;
}

/**
 * @brief The image name of one process, as a snapshot of the running processes states it
 *
 * The snapshot reads the machine without opening a process and without any privilege, so
 * asking what a process is costs nothing and reaches processes this tool could not open.
 *
 * @param snapshot the snapshot to walk, made by the caller
 * @param pid the process to look for
 * @param out receives the image name, UTF-8, always terminated
 * @param out_size room of out
 * @return true when the snapshot names the process
 */
static bool image_of(HANDLE const snapshot, uint32_t const pid, char *const out, size_t const out_size) {
  PROCESSENTRY32W entry;
  bool found = false;

  memset(&entry, 0, sizeof(entry));
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot, &entry)) {
    return false;
  }
  do {
    if (entry.th32ProcessID == pid) {
      found = true;
      break;
    }
  } while (Process32NextW(snapshot, &entry));
  if (!found) {
    return false;
  }
  if (WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, out, (int)out_size, NULL, NULL) == 0) {
    out[0] = '\0';
    return false;
  }
  return true;
}

struct blocker *blocker_list_build(struct veto_event const *const events,
                                   char const *const *const mixer_names,
                                   size_t const mixer_name_count,
                                   struct ov_error *const err) {
  struct blocker *list = NULL;
  HANDLE snapshot = INVALID_HANDLE_VALUE;
  size_t const n = veto_event_count(events);
  size_t i = 0;
  bool success = false;

  snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  for (i = 0; i < n; i++) {
    struct blocker b;
    char image[MAX_PATH];

    if (events[i].pid == 0) {
      continue;
    }
    if (!image_of(snapshot, events[i].pid, image, sizeof(image))) {
      // the process the record named is gone: an answer, and the record is history
      logger_notef("probe", "skip pid=%lu reason=no running process holds the id", (unsigned long)events[i].pid);
      continue;
    }
    bool const mixer = is_mixer_image(image, mixer_names, mixer_name_count);
    bool const audiodg = process_path_matches_name(image, "audiodg.exe");
    if (!mixer && !audiodg) {
      logger_notef("probe", "skip pid=%lu image=%s reason=this tool never closes that", (unsigned long)events[i].pid, image);
      continue;
    }
    memset(&b, 0, sizeof(b));
    b.pid = events[i].pid;
    b.event_time_ms = events[i].event_time_ms;
    b.kind = mixer ? BLOCKER_KIND_MIXER : BLOCKER_KIND_AUDIODG;
    if (!OV_ARRAY_PUSH(&list, b)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    logger_notef(
        "probe", "keep pid=%lu image=%s kind=%s", (unsigned long)b.pid, image, (b.kind == BLOCKER_KIND_MIXER) ? "mixer" : "audiodg");
  }
  if ((list == NULL) && !OV_ARRAY_GROW(&list, 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  success = true;

cleanup:
  if (snapshot != INVALID_HANDLE_VALUE) {
    CloseHandle(snapshot);
  }
  if (!success) {
    blocker_list_destroy(&list);
  }
  return success ? list : NULL;
}

size_t blocker_count(struct blocker const *const list) {
  if (list == NULL) {
    return 0;
  }
  return OV_ARRAY_LENGTH(list);
}

size_t blocker_mixer_count(struct blocker const *const list) {
  size_t const n = blocker_count(list);
  size_t count = 0;
  size_t i = 0;

  for (i = 0; i < n; i++) {
    if (list[i].kind == BLOCKER_KIND_MIXER) {
      count++;
    }
  }
  return count;
}

size_t blocker_audiodg_count(struct blocker const *const list) {
  size_t const n = blocker_count(list);
  size_t count = 0;
  size_t i = 0;

  for (i = 0; i < n; i++) {
    if (list[i].kind == BLOCKER_KIND_AUDIODG) {
      count++;
    }
  }
  return count;
}

/**
 * @brief Terminate one process and wait until it is gone
 *
 * The process is opened at this moment, and the open is refused unless it still is the one
 * the record names: a process that started after the record was written cannot be it, the
 * id was given to somebody else in between.  The handle stays open from the check to the
 * termination, so the instance that was checked is the instance that goes.
 *
 * The right to terminate, and the privilege the audio engine needs to be opened at all, are
 * asked for only when the caller allows them.
 *
 * @param b the process to close
 * @param allow_privilege allow SeDebugPrivilege when a plain open was refused
 * @param err receives the failure
 * @return false when the process could not be closed
 */
bool blocker_close(struct blocker const *const b, bool const allow_privilege, struct ov_error *const err) {
  struct process_ref ref;
  ov_tribool mine = ov_indeterminate;
  bool success = false;

  memset(&ref, 0, sizeof(ref));
  ref.handle = INVALID_HANDLE_VALUE;
  if ((b == NULL) || (b->pid == 0)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  // the process is opened now, and the handle is what keeps the id on the instance that is
  // checked: from here to the termination no other process can take the id over
  if (!process_ref_open(&ref, b->pid, true, allow_privilege, err)) {
    logger_notef("close", "reject pid=%lu reason=the process could not be opened", (unsigned long)b->pid);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  mine = process_ref_started_before_event(&ref, b->event_time_ms, err);
  if (mine == ov_indeterminate) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (mine == ov_false) {
    logger_notef("close",
                 "reject pid=%lu event_time=%llu reason=the process started after the record, so the id was given to another one",
                 (unsigned long)b->pid,
                 (unsigned long long)b->event_time_ms);
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_not_found,
                  "the process behind pid %1$lu is no longer the one the record names",
                  "the process behind pid %1$lu is no longer the one the record names",
                  (unsigned long)b->pid);
    goto cleanup;
  }
  if (!process_ref_terminate(&ref, err)) {
    logger_notef("close", "reject pid=%lu reason=the termination was refused", (unsigned long)b->pid);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  switch (process_ref_wait(&ref, 5000, err)) {
  case ov_true:
    break;
  case ov_false:
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "the process behind pid %1$lu did not exit within 5 seconds",
                  "the process behind pid %1$lu did not exit within 5 seconds",
                  (unsigned long)b->pid);
    goto cleanup;
  case ov_indeterminate:
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  logger_notef("close", "closed pid=%lu", (unsigned long)b->pid);
  success = true;

cleanup:
  process_ref_close(&ref);
  return success;
}

void blocker_list_destroy(struct blocker **const list) {
  if ((list == NULL) || (*list == NULL)) {
    return;
  }
  OV_ARRAY_DESTROY(list);
}
