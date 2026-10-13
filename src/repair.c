#include "repair.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "culprit.h"
#include "device.h"
#include "logger.h"
#include "mixer.h"
#include "process.h"
#include "removal.h"
#include "settings.h"

/**
 * @brief The look the confirmation asks the device for
 *
 * A device query reads the system, and a test cannot make a device come back within
 * the window the confirmation waits for.  The look it goes through is therefore
 * replaceable, so the polling structure can be checked without a real device.  No
 * production path ever replaces it.
 */
static enum device_state (*probe_devnode_fn)(char const *devnode, struct ov_error *err) = device_probe_devnode;

/**
 * @brief Put a sentence of its own into the result
 *
 * A run that repairs more than one device says what came of every one of them,
 * so a sentence joins the ones before it on a line of its own.
 *
 * @param out the result to fill in
 * @param text the sentence to carry, UTF-8
 */
static void say(struct repair_result *const out, char const *const text) {
  size_t used = 0;
  size_t room = 0;

  if ((out == NULL) || (text == NULL)) {
    return;
  }
  used = strnlen(out->message, sizeof(out->message) - 1);
  room = sizeof(out->message) - 1 - used;
  if (used > 0) {
    if (room > 1) {
      out->message[used] = '\n';
      used += 1;
      room -= 1;
    } else {
      return;
    }
  }
  strncpy(out->message + used, text, room);
  out->message[sizeof(out->message) - 1] = '\0';
}

/**
 * @brief The words of the setting that lets this tool close the mixer
 *
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the caption, UTF-8, always terminated
 * @param out_size room of out
 */
void repair_option_mixer_label(struct settings const *const s, char *const out, size_t const out_size) {
  (void)s;
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  strncpy(out, gettext("Close the Audient mixer automatically"), out_size - 1);
  out[out_size - 1] = '\0';
}

/**
 * @brief The words the stage report shows while a mixer is being closed
 *
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the caption, UTF-8, always terminated
 * @param out_size room of out
 */
void repair_stage_mixer_caption(struct settings const *const s, char *const out, size_t const out_size) {
  (void)s;
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  strncpy(out, gettext("Closing the Audient mixer..."), out_size - 1);
  out[out_size - 1] = '\0';
}

/**
 * @brief The words of the setting that lets this tool close the audio engine
 *
 * @return the caption, UTF-8
 */
char const *repair_option_audiodg_label(void) { return gettext("Close audiodg (audiodg.exe) automatically"); }

/**
 * @brief The sentence of a veto that closed nothing for lack of a blocker this run may close
 *
 * blockers is how many distinct processes the records name as the blockers of the device.
 * Every one of these sentences opens with the state the reader is left with, because that is
 * what they came to the log for.  Then it names the single thing they can change: option
 * carries the words of the setting that would have let this tool close the process, said the
 * way the window says them; NULL says the record named somebody this tool never closes.
 * More than one blocker makes the sentence carry the number instead: naming one of them
 * would read as if it were the only one.
 *
 * @param option the words of the setting that stands in the way, or NULL
 * @param blockers how many distinct processes the records name
 * @param out receives the sentence, UTF-8
 * @param out_size room of out
 */
static void repair_blocked_sentence(char const *const option, size_t const blockers, char *const out, size_t const out_size) {
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  out[0] = '\0';
  if (blockers > 1) {
    OV_SNPRINTF(out, out_size, NULL, "The device is still in use by %1$u processes.", (unsigned)blockers);
  } else if ((option != NULL) && (option[0] != '\0')) {
    OV_SNPRINTF(out, out_size, NULL, "The device is still in use because \"%s\" is turned off.", option);
  } else {
    strncpy(out, "The device is still in use by another process.", out_size - 1);
    out[out_size - 1] = '\0';
  }
}

/**
 * @brief Put a formatted sentence into the result
 *
 * The message names the process that was in the way; reference describes the arguments
 * for the format the way ov_sprintf_char is called everywhere else here.
 *
 * @param out the result to fill in
 * @param reference the pattern of the sentence, and its reference
 * @param what the process the sentence names
 */
static void sayf(struct repair_result *const out, char const *const reference, char const *const what) {
  OV_SNPRINTF(out->message, sizeof(out->message), reference, reference, what);
}

/**
 * @brief The pids of the blockers of a run
 *
 * The caller uses them to keep the processes it closed out of the list it shows.  A
 * process this tool closed is not a reason the device was blocked: it is gone, the user
 * watched it go, and a list that still names it says the opposite of what happened.  The
 * identity that is dropped is the exact instance that was closed, so the same image running
 * elsewhere stays on the list.
 *
 * @param blockers the blockers of the run
 * @param out receives the pids, capacity 2 * REPAIR_MAX_BLOCKERS
 * @return how many pids were written
 */
static size_t repair_closed_pids(struct repair_blockers const *const blockers, uint32_t *const out) {
  size_t n = 0;
  size_t i = 0;

  if ((blockers == NULL) || (out == NULL)) {
    return 0;
  }
  for (i = 0; i < blockers->mixer_count; i++) {
    if (blockers->mixer[i].found) {
      out[n++] = blockers->mixer[i].pid;
    }
  }
  for (i = 0; i < blockers->audiodg_count; i++) {
    if (blockers->audiodg[i].found) {
      out[n++] = blockers->audiodg[i].pid;
    }
  }
  return n;
}

/**
 * @brief Keep the processes a run closed out of the list of the culprits
 *
 * @param list the list to shrink in place
 * @param closed the blockers the run closed
 */
static void drop_closed(struct culprit **const list, struct repair_blockers const *const closed) {
  uint32_t pids[2 * REPAIR_MAX_BLOCKERS];
  size_t const n = repair_closed_pids(closed, pids);

  culprit_drop_pids(list, pids, n);
}

/**
 * @brief Tell the caller what the run is waiting for
 *
 * @param progress the callback of the caller, may be NULL
 * @param userdata handed back to progress
 * @param stage what the run is waiting for
 * @param step the step of the run
 * @param total how many steps the longest way through this run takes
 */
static void report_stage(
    repair_progress_fn const progress, void *const userdata, enum repair_stage const stage, size_t const step, size_t const total) {
  if (progress != NULL) {
    progress(stage, step, total, userdata);
  }
}

/**
 * @brief Was the run asked to stop
 *
 * The flag is a plain bool the caller owns: the window sets it from its own thread, and
 * the run reads it between its steps.  A bool written by one thread and read by another is
 * not a synchronisation primitive, but one writer that only ever writes true is exactly
 * the case the plain flag is for.
 *
 * @param cancel the flag of the caller, may be NULL
 * @return true when the run is to quit at the next step boundary
 */
static bool cancel_requested(bool const *const cancel) { return (cancel != NULL) && *cancel; }

/**
 * @brief Fold the abort into the result
 *
 * The sentence says who stopped the run: only a person at the progress window can, whether
 * they pressed its button or closed the window, and the run does not tell the two apart.
 * That the device was left alone follows from the stop and is not said twice.
 *
 * @param out receives the outcome and the sentence
 */
static void say_aborted(struct repair_result *const out) {
  out->outcome = REPAIR_OUTCOME_ABORTED;
  say(out, "The operation was aborted by the user.");
}

/**
 * @brief Read the veto records of the attempt that just failed
 *
 * The veto record is written a few seconds after the removal call returns, so the read
 * waits for the record of this attempt instead of racing it.  Events older than since_ms
 * belong to an earlier attempt and are dropped here.
 *
 * @param s the settings that name the log and the provider
 * @param since_ms records older than this belong to an earlier attempt
 * @param progress the callback of the caller, may be NULL
 * @param progress_userdata handed back to progress
 * @param step the step of the run
 * @param total how many steps the longest way through this run takes
 * @param cancel the flag of the caller, may be NULL: a raised flag ends the wait
 * @param err receives the failure of the query
 * @return NULL when the query itself failed, an empty list when no veto record showed up
 */
static struct culprit *read_veto_events(struct settings const *const s,
                                        long long const since_ms,
                                        repair_progress_fn const progress,
                                        void *const progress_userdata,
                                        size_t const step,
                                        size_t const total,
                                        bool const *const cancel,
                                        struct ov_error *const err) {
  struct culprit *list = NULL;
  size_t attempt = 0;

  for (;;) {
    if (attempt > 0) {
      Sleep(1000);
    }
    report_stage(progress, progress_userdata, REPAIR_STAGE_VETO_RECORD, step, total);
    if (cancel_requested(cancel)) {
      culprit_destroy_list(&list);
      return NULL;
    }
    list = culprit_read(s->veto_log_name, s->veto_provider_name, (unsigned short)s->veto_event_id, err);
    if (list == NULL) {
      return NULL;
    }
    culprit_prune(&list, since_ms);
    if ((culprit_count(list) > 0) || (attempt >= 5)) {
      return list; // the last try hands back the empty list, the query itself worked
    }
    culprit_destroy_list(&list);
    attempt++;
  }
}

/**
 * @brief Collect all blockers of the given name
 *
 * All matching culprits are collected; duplicates by PID are skipped.  A NULL out asks
 * only for the count, so the caller can name the blockers it was not allowed to close.
 *
 * @param culprits the records to search in, may be NULL
 * @param names the image names to look for, a path matches when yes one names it
 * @param name_count how many image names the list holds
 * @param out array to fill, NULL counts the matches only
 * @param max_count maximum number of entries to fill
 * @return number of blockers collected
 */
static size_t pick_all(struct culprit const *const culprits,
                       char const *const *names,
                       size_t const name_count,
                       struct repair_blocker *const out,
                       size_t const max_count,
                       bool *const truncated) {
  size_t count = 0;
  size_t const n = culprit_count(culprits);
  size_t i = 0;
  size_t j = 0;
  size_t k = 0;

  if ((culprits == NULL) || (names == NULL) || (name_count == 0) || (max_count == 0)) {
    return 0;
  }
  if (truncated != NULL) {
    *truncated = false;
  }
  for (i = 0; i < n; i++) {
    if (culprits[i].pid == 0) {
      continue;
    }
    for (k = 0; k < name_count; k++) {
      if (process_path_matches_name(culprits[i].path, names[k])) {
        break;
      }
    }
    if (k == name_count) {
      continue;
    }
    if (out != NULL) {
      for (j = 0; j < count; j++) {
        if (out[j].pid == culprits[i].pid) {
          break;
        }
      }
      if (j < count) {
        continue;
      }
      if (count >= max_count) {
        if (truncated != NULL) {
          *truncated = true;
        }
        break;
      }
      out[count].found = true;
      out[count].pid = culprits[i].pid;
      out[count].event_time_ms = culprits[i].event_time_ms;
      out[count].name = names[k];
    }
    count++;
  }
  return count;
}

/**
 * @brief Which blockers of a veto record this run may close
 *
 * mixer_name is the image name from the settings, REPAIR_AUDIODG_NAME is the audio
 * engine; the two allow_* flags are the user's decision to let this tool close them at
 * all.
 *
 * @param culprits the records of the veto, may be NULL
 * @param mixer_matches the image names the settings name, count them flat out
 * @param mixer_count how many entries the mixer holds
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @return the blockers to close
 */
static struct repair_blockers repair_pick_blockers(struct culprit const *const culprits,
                                                   char const *const *mixer_matches,
                                                   size_t const mixer_count,
                                                   bool const allow_mixer_close,
                                                   bool const allow_audiodg_close,
                                                   struct ov_error *const err) {
  struct repair_blockers b;
  bool mixer_truncated = false;
  bool audiodg_truncated = false;
  bool success = false;

  memset(&b, 0, sizeof(b));
  if (allow_mixer_close) {
    b.mixer_count = pick_all(culprits, mixer_matches, mixer_count, b.mixer, REPAIR_MAX_BLOCKERS, &mixer_truncated);
  }
  if (allow_audiodg_close) {
    char const *const audiodg_names[] = {REPAIR_AUDIODG_NAME};
    b.audiodg_count = pick_all(culprits, audiodg_names, 1, b.audiodg, REPAIR_MAX_BLOCKERS, &audiodg_truncated);
  }
  if (mixer_truncated || audiodg_truncated) {
    OV_ERROR_SETF(err,
                  ov_error_type_generic,
                  ov_error_generic_fail,
                  "%1$hs",
                  gettext("Too many blocker processes are named in the record, more than this tool can close at once."),
                  NULL);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    memset(&b, 0, sizeof(b));
  }
  return b;
}

/**
 * @brief Close one blocking process and wait until it is gone
 *
 * The identity comes from the veto record, so only that instance is touched; a process
 * that is already gone is no blocker any more and costs nothing.  Only a hard failure
 * stops the run and folds its reason into the outcome.
 *
 * @param blocker the instance the record named
 * @param name the image name to check against
 * @param stage the stage to report
 * @param progress the callback of the caller, may be NULL
 * @param progress_userdata handed back to progress
 * @param step the step of the run
 * @param total how many steps the longest way through this run takes
 * @param out receives what happened
 * @param target receives the identity that was closed
 * @param err receives the hard failure
 * @return false only on a hard failure
 */
static bool close_blocking_process(struct repair_blocker const *const blocker,
                                   char const *const name,
                                   enum repair_stage const stage,
                                   repair_progress_fn const progress,
                                   void *const progress_userdata,
                                   size_t *const step,
                                   size_t const total,
                                   struct repair_result *const out,
                                   struct mixer_target *const target,
                                   struct ov_error *const err) {
  bool found = false;
  bool success = false;

  report_stage(progress, progress_userdata, stage, ++(*step), total);
  logger_notef("close", "%s pid=%lu event_time=%lld", name, (unsigned long)blocker->pid, blocker->event_time_ms);
  if (!mixer_take(blocker->pid, name, blocker->event_time_ms, &found, target, err)) {
    OV_ERROR_ADD_TRACE(err);
    out->outcome = REPAIR_OUTCOME_FAILED;
    sayf(out, "%1$hs could not be inspected.", name);
    goto cleanup;
  }
  if (!found) {
    logger_notef("close", "%s pid=%lu was not closed: it is not the instance the record named", name, (unsigned long)blocker->pid);
    sayf(out, "%1$hs is no longer running.", name);
    success = true;
    goto cleanup;
  }
  if (!mixer_close(target, err)) {
    OV_ERROR_ADD_TRACE(err);
    out->outcome = REPAIR_OUTCOME_FAILED;
    sayf(out, "%1$hs could not be closed.", name);
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Keep the records that name the device of the devnode
 *
 * A veto record belongs to the device whose removal was stopped, so a record
 * stays when the instance it names is one of the ancestors of the devnode.  A
 * record that names no instance cannot be told apart from the records of the
 * other devices, so it goes: acting on it would close a process that blocks
 * another device.
 *
 * @param list the list to shrink in place
 * @param devnode the KS devnode of the device being repaired
 * @param err receives the failure of the ancestor walk
 * @return false when the walk failed
 */
static bool culprit_keep_device(struct culprit **const list, char const *const devnode, struct ov_error *const err) {
  size_t kept = 0;
  size_t i = 0;
  size_t const n = culprit_count(*list);
  bool success = false;

  for (i = 0; i < n; i++) {
    struct culprit *const record = &(*list)[i];
    bool named = false;

    if (record->device_instance == NULL) {
      OV_ARRAY_DESTROY(&record->path);
      OV_ARRAY_DESTROY(&record->device_instance);
      continue;
    }
    if (!device_culprit_names_ancestor(record->device_instance, devnode, &named, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!named) {
      OV_ARRAY_DESTROY(&record->path);
      OV_ARRAY_DESTROY(&record->device_instance);
      continue;
    }
    (*list)[kept++] = *record;
  }
  OV_ARRAY_SET_LENGTH(*list, kept);
  success = true;

cleanup:
  return success;
}

/**
 * @brief Move every record of one list into another
 *
 * The records leave the list they came from without being copied, so the
 * strings travel with them and the shell of the list is left empty.
 *
 * @param dst the list to fill
 * @param src the list to empty
 * @param err receives the failure of the growth
 * @return false when the records could not all be moved
 */
static bool culprit_move_all(struct culprit **const dst, struct culprit **const src, struct ov_error *const err) {
  size_t i = 0;
  size_t const n = culprit_count(*src);
  bool success = false;

  for (i = 0; i < n; i++) {
    if (!OV_ARRAY_PUSH(dst, (*src)[i])) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      goto cleanup;
    }
    (*src)[i].path = NULL;
    (*src)[i].device_instance = NULL;
  }
  culprit_destroy_list(src);
  success = true;

cleanup:
  return success;
}

/**
 * @brief Which of two outcomes stands over the other
 *
 * The run stops for a device it could not repair for a reason it does not know,
 * so that outcome stands over the one that says the device is still in use.
 *
 * @param a one outcome of a device
 * @param b the other outcome of a device
 * @return the outcome that stands over the other
 */
static enum repair_outcome worse_outcome(enum repair_outcome const a, enum repair_outcome const b) {
  enum repair_outcome const order[] = {REPAIR_OUTCOME_CLOSED, REPAIR_OUTCOME_VETOED, REPAIR_OUTCOME_OTHER_PROBLEM, REPAIR_OUTCOME_FAILED};
  size_t ia = 0;
  size_t ib = 0;

  for (ia = 0; ia < sizeof(order) / sizeof(order[0]); ia++) {
    if (order[ia] == a) {
      break;
    }
  }
  for (ib = 0; ib < sizeof(order) / sizeof(order[0]); ib++) {
    if (order[ib] == b) {
      break;
    }
  }
  return (ib > ia) ? b : a;
}

/**
 * @brief How many looks the confirmation takes while it waits for a device to come back
 *
 * One second between two looks is fixed, so the number of looks follows from the wait the
 * settings name and nothing else.  The smallest wait still takes one look: a device that
 * is back at once has to be found by it.
 *
 * @param max_wait_ms how long the confirmation is willing to wait, milliseconds
 * @return how many looks it takes
 */
static size_t confirm_poll_count(DWORD const max_wait_ms) {
  size_t const count = (size_t)(max_wait_ms / REPAIR_CONFIRM_POLL_INTERVAL_MS);
  return (count < 1) ? 1 : count;
}

/**
 * @brief Ask the released device whether it is an audio device again
 *
 * The release puts the device back on the bus, and the bus takes a moment to bring the
 * device up again.  The confirmation waits one second between two looks and keeps looking
 * until the wait the settings name runs out, so a device that comes back is found however
 * slow the bus is, and a look that finds the device healthy ends the polling at once.
 *
 * @param s the settings that name how long the confirmation waits
 * @param target the device that was released
 * @param progress the callback of the caller, may be NULL
 * @param progress_userdata handed back to progress
 * @param step the step of the run
 * @param total how many steps the longest way through this run takes
 * @param cancel the flag of the caller, may be NULL
 * @param out receives what happened
 * @param err receives the failure of the query
 * @return the outcome of the confirmation
 */
static enum repair_outcome confirm_recovered(struct settings const *const s,
                                             struct device_target const *const target,
                                             repair_progress_fn const progress,
                                             void *const progress_userdata,
                                             size_t *const step,
                                             size_t const total,
                                             bool const *const cancel,
                                             struct repair_result *const out,
                                             struct ov_error *const err) {
  report_stage(progress, progress_userdata, REPAIR_STAGE_CONFIRM, ++(*step), total);
  for (size_t poll = 0; poll < confirm_poll_count((DWORD)s->confirm_max_wait_ms); poll++) {
    Sleep(REPAIR_CONFIRM_POLL_INTERVAL_MS);
    if (cancel_requested(cancel)) {
      say_aborted(out);
      return REPAIR_OUTCOME_ABORTED;
    }
    enum device_state const state = probe_devnode_fn(target->devnode, err);
    if (state == DEVICE_STATE_HEALTHY) {
      return REPAIR_OUTCOME_CLOSED;
    }
  }
  OV_ERROR_REPORT(err, NULL);
  out->outcome = REPAIR_OUTCOME_FAILED;
  say(out, "The device did not come back as an audio device, and the reason is not known.");
  return REPAIR_OUTCOME_FAILED;
}

/**
 * @brief Repair one device
 *
 * The documented sequence for the device: remove, look up the culprits when
 * vetoed, optionally close the blockers (the mixer and audiodg) and remove
 * again, then confirm that the device came back as an audio device.  The
 * records the run leaves behind belong to the device, so the caller can name
 * the processes that still block it.
 *
 * @param s the settings this run works for
 * @param target the device to repair
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @param progress may be NULL; it is called from the thread that runs the repair
 * @param progress_userdata handed back to progress
 * @param step the step of the run, counted up by every stage
 * @param total how many steps the longest way through this run takes
 * @param cancel the flag of the caller, may be NULL
 * @param out receives what happened
 * @param err receives the failure that stopped the repair of the device
 * @return the outcome of the device
 */
static enum repair_outcome repair_target(struct settings const *const s,
                                         struct device_target const *const target,
                                         bool const allow_mixer_close,
                                         bool const allow_audiodg_close,
                                         repair_progress_fn const progress,
                                         void *const progress_userdata,
                                         size_t *const step,
                                         size_t const total,
                                         bool const *const cancel,
                                         struct repair_result *const out,
                                         struct ov_error *const err) {
  enum repair_outcome outcome = REPAIR_OUTCOME_FAILED;
  long long since = 0;
  struct repair_blockers picked;
  struct mixer_target taken_mixer;
  struct mixer_target taken_audiodg;
  struct culprit *culprits = NULL;
  struct culprit *current = NULL;
  struct ov_error removal_err = {0};
  struct ov_error read_err = {0};
  size_t i = 0;
  bool success = false;

  memset(&picked, 0, sizeof(picked));
  memset(&taken_mixer, 0, sizeof(taken_mixer));
  memset(&taken_audiodg, 0, sizeof(taken_audiodg));

  if (target->state == DEVICE_STATE_PROBLEM) {
    outcome = REPAIR_OUTCOME_OTHER_PROBLEM;
    say(out, "The device has a problem that this tool cannot solve.");
    success = true;
    goto cleanup;
  }

  since = process_now_ms();
  logger_notef("remove", "first attempt, device=%s since=%lld", target->devnode, since);
  if (cancel_requested(cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  report_stage(progress, progress_userdata, REPAIR_STAGE_REMOVE, ++(*step), total);
  if (removal_close(target->devnode, &removal_err)) {
    outcome = REPAIR_OUTCOME_CLOSED;
    say(out, "The device was released.");
    logger_notef("remove", "The device was released on the first attempt.");
    outcome = confirm_recovered(s, target, progress, progress_userdata, step, total, cancel, out, err);
    success = true;
    goto cleanup;
  }
  if (!removal_error_is_veto(&removal_err)) {
    outcome = REPAIR_OUTCOME_FAILED;
    out->removal_error = removal_error_of(&removal_err);
    say(out, "The device could not be released.");
    success = true;
    goto cleanup;
  }
  if (cancel_requested(cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  culprits = read_veto_events(s, since, progress, progress_userdata, ++(*step), total, cancel, &read_err);
  if (culprits == NULL) {
    if (cancel_requested(cancel)) {
      say_aborted(out);
      outcome = REPAIR_OUTCOME_ABORTED;
      success = true;
      goto cleanup;
    }
    outcome = REPAIR_OUTCOME_VETOED;
    say(out, "The device could not be released, and the reason could not be found.");
    success = true;
    goto cleanup;
  }
  logger_notef("veto", "the removal of device=%s was vetoed by %u record(s)", target->devnode, (unsigned)culprit_count(culprits));
  if (!culprit_keep_device(&culprits, target->devnode, err)) {
    OV_ERROR_ADD_TRACE(err);
    outcome = REPAIR_OUTCOME_FAILED;
    goto cleanup;
  }

  picked =
      repair_pick_blockers(culprits, settings_mixer_names(s), settings_mixer_name_count(s), allow_mixer_close, allow_audiodg_close, err);
  if (picked.mixer_count == 0 && picked.audiodg_count == 0) {
    char option_buf[256] = {0};
    char const *option = NULL;
    char sentence[256];

    outcome = REPAIR_OUTCOME_VETOED;
    // the blockers the user did not allow this tool to close are still worth naming: the
    // pick with both flags up says which option would have helped
    {
      char const *const audiodg_names[] = {REPAIR_AUDIODG_NAME};
      if (pick_all(culprits, settings_mixer_names(s), settings_mixer_name_count(s), NULL, REPAIR_MAX_BLOCKERS, NULL) > 0) {
        repair_option_mixer_label(s, option_buf, sizeof(option_buf));
        option = option_buf;
      } else if (pick_all(culprits, audiodg_names, 1, NULL, REPAIR_MAX_BLOCKERS, NULL) > 0) {
        option = repair_option_audiodg_label();
      }
    }
    repair_blocked_sentence(option, culprit_count(culprits), sentence, sizeof(sentence));
    say(out, sentence);
    if (culprit_count(culprits) > 0) {
      if (!culprit_move_all(&out->culprits, &culprits, err)) {
        OV_ERROR_ADD_TRACE(err);
        outcome = REPAIR_OUTCOME_FAILED;
      }
    }
    success = true;
    goto cleanup;
  }

  if (cancel_requested(cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  for (i = 0; i < picked.mixer_count; i++) {
    if (!close_blocking_process(
            &picked.mixer[i], picked.mixer[i].name, REPAIR_STAGE_MIXER, progress, progress_userdata, step, total, out, &taken_mixer, err)) {
      outcome = REPAIR_OUTCOME_FAILED;
      goto cleanup;
    }
    if (cancel_requested(cancel)) {
      say_aborted(out);
      outcome = REPAIR_OUTCOME_ABORTED;
      success = true;
      goto cleanup;
    }
  }
  for (i = 0; i < picked.audiodg_count; i++) {
    if (!close_blocking_process(&picked.audiodg[i],
                                REPAIR_AUDIODG_NAME,
                                REPAIR_STAGE_AUDIODG,
                                progress,
                                progress_userdata,
                                step,
                                total,
                                out,
                                &taken_audiodg,
                                err)) {
      outcome = REPAIR_OUTCOME_FAILED;
      goto cleanup;
    }
    if (cancel_requested(cancel)) {
      say_aborted(out);
      outcome = REPAIR_OUTCOME_ABORTED;
      success = true;
      goto cleanup;
    }
  }

  if (cancel_requested(cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  since = process_now_ms();
  logger_notef("remove", "second attempt, device=%s since=%lld", target->devnode, since);
  report_stage(progress, progress_userdata, REPAIR_STAGE_REMOVE, ++(*step), total);
  if (removal_close(target->devnode, &removal_err)) {
    outcome = REPAIR_OUTCOME_CLOSED;
    say(out, "The device was released.");
    logger_notef("remove", "The device was released on the second attempt.");
    outcome = confirm_recovered(s, target, progress, progress_userdata, step, total, cancel, out, err);
    success = true;
    goto cleanup;
  }
  if (cancel_requested(cancel)) {
    say_aborted(out);
    outcome = REPAIR_OUTCOME_ABORTED;
    success = true;
    goto cleanup;
  }
  current = read_veto_events(s, since, progress, progress_userdata, ++(*step), total, cancel, &read_err);
  if (current != NULL) {
    drop_closed(&current, &picked);
    culprit_destroy_list(&culprits);
    culprits = current;
    current = NULL;
    if (!culprit_keep_device(&culprits, target->devnode, err)) {
      OV_ERROR_ADD_TRACE(err);
      outcome = REPAIR_OUTCOME_FAILED;
      goto cleanup;
    }
  } else {
    drop_closed(&culprits, &picked);
  }
  if (culprit_count(culprits) > 0) {
    if (!culprit_move_all(&out->culprits, &culprits, err)) {
      OV_ERROR_ADD_TRACE(err);
      outcome = REPAIR_OUTCOME_FAILED;
      goto cleanup;
    }
  }
  outcome = REPAIR_OUTCOME_VETOED;
  say(out, "The device is still in use.");
  logger_notef("remove", "The blockers were closed but the device is still blocked.");
  success = true;

cleanup:
  if (success) {
    OV_ERROR_REPORT(err, NULL);
  }
  OV_ERROR_REPORT(&removal_err, NULL);
  OV_ERROR_REPORT(&read_err, NULL);
  culprit_destroy_list(&culprits);
  culprit_destroy_list(&current);
  mixer_target_release(&taken_mixer);
  mixer_target_release(&taken_audiodg);
  return outcome;
}

/**
 * @brief Run the repair
 *
 * The run repairs every device the caller names, or, when it names none, every
 * device that is faulted.  A device that does not come back as an audio device
 * stops the run, for the reason it stayed away is not one this tool knows.
 *
 * @param s the settings this run works for
 * @param targets the devices to repair, NULL says the run looks for the faulted
 *        devices itself; an empty list is the same as no device at all
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @param progress may be NULL; it is called from the thread that runs the repair
 * @param progress_userdata handed back to progress
 * @param cancel the flag of the caller, may be NULL: when it is up the run quits at the
 *        next boundary between the steps and ends with the aborted outcome
 * @param out receives what happened; release with repair_result_release
 * @param err receives the failure that stopped the run
 * @return false only on a hard failure.  The outcome carries the news: a step whose
 *         failure is folded into the outcome releases its error into the log before the
 *         next step starts, so err is only worth reading when the call returns false.
 */
bool repair_run(struct settings const *const s,
                struct device_target *const targets,
                bool const allow_mixer_close,
                bool const allow_audiodg_close,
                repair_progress_fn const progress,
                void *const progress_userdata,
                bool *const cancel,
                struct repair_result *const out,
                struct ov_error *const err) {
  struct device_target *list = NULL;
  bool owned = false;
  bool probed = false;
  enum repair_outcome outcome = REPAIR_OUTCOME_CLOSED;
  size_t total = 0;
  size_t step = 0;
  size_t i = 0;
  size_t n = 0;
  bool success = false;

  if ((s == NULL) || (out == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  memset(out, 0, sizeof(*out));

  if (cancel_requested(cancel)) {
    say_aborted(out);
    success = true;
    goto cleanup;
  }
  if (targets != NULL) {
    list = targets;
  } else {
    report_stage(progress, progress_userdata, REPAIR_STAGE_PROBE, ++step, REPAIR_STEPS_PER_TARGET);
    if (!device_probe_all(s->ks_service, s->devices, OV_ARRAY_LENGTH(s->devices), &list, err)) {
      OV_ERROR_ADD_TRACE(err);
      out->outcome = REPAIR_OUTCOME_FAILED;
      say(out, "The device state could not be determined.");
      goto cleanup;
    }
    owned = true;
    probed = true;
    device_targets_keep_faulted(&list);
  }
  n = OV_ARRAY_LENGTH(list);
  if (n == 0) {
    if (cancel_requested(cancel)) {
      say_aborted(out);
      success = true;
      goto cleanup;
    }
    out->outcome = REPAIR_OUTCOME_ABSENT;
    say(out, "No Audient device was found.");
    success = true;
    goto cleanup;
  }
  total = (probed ? 1 : 0) + (n * REPAIR_STEPS_PER_TARGET);
  for (i = 0; i < n; i++) {
    enum repair_outcome const one =
        repair_target(s, &list[i], allow_mixer_close, allow_audiodg_close, progress, progress_userdata, &step, total, cancel, out, err);
    if (one == REPAIR_OUTCOME_ABORTED) {
      outcome = one;
      break;
    }
    if (worse_outcome(one, outcome) == one) {
      outcome = one;
    }
    if (one == REPAIR_OUTCOME_FAILED) {
      break; // the reason is not known, so the run stops here
    }
  }
  out->outcome = outcome;
  success = true;

cleanup:
  if (success) {
    OV_ERROR_REPORT(err, NULL);
  }
  if (owned) {
    device_targets_release(&list);
  }
  return success;
}

/**
 * @brief Release what a run collected
 *
 * @param r NULL is allowed
 */
void repair_result_release(struct repair_result *const r) {
  if (r == NULL) {
    return;
  }
  culprit_destroy_list(&r->culprits);
  r->outcome = REPAIR_OUTCOME_UNKNOWN;
}
