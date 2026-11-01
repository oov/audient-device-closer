#include "blocker.h"

#include <string.h>
#include <wchar.h>

#include <ovarray.h>

#include "logger.h"
#include "paths.h"
#include "process_manager.h"
#include "veto_event.h"

/**
 * @brief One process that holds a device, as the machine states it now
 *
 * A veto record names a process id and the moment the record was written, and nothing else
 * about it can be acted on: what the record says the process was called is what it was
 * called then.  The id is looked up in the machine as it is at this moment, and the process
 * enters this list only when it is one this tool may close at all.
 *
 * The walk is what the list is read through, so neither the entry nor the order it sits in
 * leaves this file.
 */
struct blocker {
  uint32_t pid;
  uint64_t event_time_ms;  //!< the moment of the record that named it
  size_t mixer_name_index; //!< which of the mixer names it is, SIZE_MAX when it is the audio engine
};

/**
 * @brief Does this image name belong to the mixer the settings name
 *
 * @param image the image name a snapshot of the running processes reports
 * @param names the image names the settings carry, may be NULL
 * @param name_count how many names that is
 * @return which of the names it is, SIZE_MAX when it is none of them
 */
static size_t mixer_index_of(wchar_t const *const image, wchar_t const *const *const names, size_t const name_count) {
  if ((names == NULL) || (image == NULL)) {
    return SIZE_MAX;
  }
  for (size_t i = 0; i < name_count; i++) {
    if (paths_file_name_matches(image, names[i])) {
      return i;
    }
  }
  return SIZE_MAX;
}

NODISCARD struct blocker *blocker_list_build(struct veto_event const *const events,
                                             wchar_t const *const *const mixer_names,
                                             size_t const mixer_name_count,
                                             struct ov_error *const err) {
  struct blocker *list = NULL;
  struct process_name *names = NULL;
  struct veto_event_iter *records = NULL;
  struct veto_event_iter it = {0};
  bool success = false;

  // the look is taken for the ids the records name, not for every process of the machine: a
  // walk is about a handful of records, and what it holds must follow that
  while (veto_event_next(events, &it)) {
    struct process_name const name = {.pid = it.pid};

    if (!OV_ARRAY_PUSH(&names, name) || !OV_ARRAY_PUSH(&records, it)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
  }
  // the whole walk sees one moment of the machine, so two records of the same process cannot
  // be told apart by a process that ended between them
  if (process_manager()->take_snapshot(names, OV_ARRAY_LENGTH(names), err) == SIZE_MAX) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }

  for (size_t i = 0; i < OV_ARRAY_LENGTH(names); i++) {
    struct blocker b;
    wchar_t const *const image = names[i].image;
    size_t const mixer_index = mixer_index_of(image, mixer_names, mixer_name_count);
    bool mixer = (mixer_index != SIZE_MAX);
    bool audiodg = false;

    if (image[0] == L'\0') {
      // the process the record named is gone: an answer, and the record is history
      logger_notef("probe", "skip pid=%lu reason=no running process holds the id", (unsigned long)names[i].pid);
      continue;
    }
    // The audio engine is a blocker as often as the mixer is, and its image name is the same
    // everywhere, so it is not part of the settings.
    audiodg = paths_file_name_matches(image, L"audiodg.exe");
    if (!mixer && !audiodg) {
      logger_notef("probe", "skip pid=%lu image=%ls reason=this tool never closes that", (unsigned long)names[i].pid, image);
      continue;
    }
    memset(&b, 0, sizeof(b));
    b.pid = names[i].pid;
    b.event_time_ms = records[i].event_time_ms;
    b.mixer_name_index = mixer_index;
    if (!OV_ARRAY_PUSH(&list, b)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    logger_notef("probe", "keep pid=%lu image=%ls kind=%s", (unsigned long)b.pid, image, mixer ? "mixer" : "audiodg");
  }
  if ((list == NULL) && !OV_ARRAY_GROW(&list, 1)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  success = true;

cleanup:
  if (names != NULL) {
    OV_ARRAY_DESTROY(&names);
  }
  if (records != NULL) {
    OV_ARRAY_DESTROY(&records);
  }
  if (!success) {
    blocker_list_destroy(&list);
  }
  return success ? list : NULL;
}

bool blocker_next(struct blocker const *const list, struct blocker_iter *const it) {
  if ((list == NULL) || (it == NULL)) {
    return false;
  }
  if (it->idx >= OV_ARRAY_LENGTH(list)) {
    return false;
  }
  it->pid = list[it->idx].pid;
  it->event_time_ms = list[it->idx].event_time_ms;
  it->mixer_name_index = list[it->idx].mixer_name_index;
  it->idx++;
  return true;
}

void blocker_list_destroy(struct blocker **const list) {
  if ((list == NULL) || (*list == NULL)) {
    return;
  }
  OV_ARRAY_DESTROY(list);
}
