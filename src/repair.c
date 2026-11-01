#include "repair.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <ovarray.h>
#include <ovmo.h>
#include <ovprintf.h>
#include <ovprintf_ex.h>

#include "blocker.h"
#include "clock_manager.h"
#include "device_manager.h"
#include "device_probe.h"
#include "logger.h"
#include "process_manager.h"
#include "removal_manager.h"
#include "settings.h"
#include "veto_event.h"
#include "veto_log_manager.h"

/**
 * @brief Where a run stands between two waits
 *
 * A run that has a window must not sit on its own thread while a lookup takes its seconds,
 * so the run is cut at the waits and taken forward one piece at a time.  Every other piece
 * runs to its end without waiting and is not a phase of its own.
 */
enum repair_phase {
  REPAIR_PHASE_START,       //!< the devices are taken up, or looked for
  REPAIR_PHASE_TARGET,      //!< one device is taken up to its first removal
  REPAIR_PHASE_VETO_FIRST,  //!< the record of the first refusal is read
  REPAIR_PHASE_MIXER,       //!< the mixers the record names are closed
  REPAIR_PHASE_PAUSE,       //!< the Windows Audio service is paused
  REPAIR_PHASE_SECOND,      //!< the second removal is asked for
  REPAIR_PHASE_VETO_SECOND, //!< the record of the second refusal is read, and the device is done
  REPAIR_PHASE_CONFIRM,     //!< the released device is asked whether it is an audio device again
  REPAIR_PHASE_RESUME,      //!< the service that was paused is started again
  REPAIR_PHASE_CLEANUP,     //!< what the device left behind is settled
  REPAIR_PHASE_NEXT,        //!< the next device, or the end of the run
  REPAIR_PHASE_DONE,        //!< the run is over
};

/** @brief The work a run does, and where it stands */
struct repair_run {
  struct settings const *settings; //!< the settings this run works for
  bool allow_mixer_close;          //!< the user allowed closing the mixer
  bool allow_audiodg_close;        //!< the user allowed pausing the audio engine
  bool can_pause_audio_service;    //!< pausing the audio service is allowed on this build and by these settings
  repair_progress_fn progress;     //!< may be NULL; called where the run is taken forward
  void *progress_userdata;         //!< handed back to progress
  HANDLE cancel;                   //!< may be NULL; signaled to ask the run to stop
  repair_wake_fn wake;             //!< may be NULL; called when a wait is over
  void *wake_userdata;             //!< handed to wake
  struct ov_error *err;            //!< borrowed, the caller's, written by the thread of a wait
  bool wake_failed;                //!< whether the thread that waited could not be woken
  HANDLE wake_handle;              //!< the event that wakes the thread driving the run
  struct repair_result *out;       //!< receives what happened
  size_t step;                     //!< the step the run is at, counted from zero
  size_t total;                    //!< how many steps the longest way takes

  enum repair_phase phase;     //!< where the run stands
  struct device_target *list;  //!< the devices to repair
  bool owns_list;              //!< whether the list is the run's to release
  bool probed;                 //!< whether the run looked the devices up itself
  size_t index;                //!< the device being repaired
  size_t count;                //!< how many devices the list holds
  enum repair_outcome outcome; //!< the worst outcome so far

  struct device_target const *target; //!< the device being repaired
  uint64_t since;                     //!< when the attempt that is being read was made
  struct veto_event *events;          //!< the records of that attempt
  struct blocker *blockers;           //!< the processes those records name
  uint32_t *closed;                   //!< ovarray: the ids this run closed for this device
  bool audio_service_paused;          //!< whether the audio service was paused and has to come back
  bool audio_service_checked;         //!< whether the run already asked for the audio service to be running
  enum repair_outcome target_outcome; //!< what came of the device

  struct veto_log_wait *lookup; //!< the lookup in flight, NULL when the run waits on none
  struct device_watch *watch;   //!< the watch in flight, NULL when the run waits on none
  struct process_act *act;      //!< the act on the processes in flight, NULL when the run waits on none
  struct removal_op *removal;   //!< the removal in flight, NULL when the run waits on none
  struct blocker_iter closing;  //!< the entry that act is about, meaningful while act is not NULL
  size_t mixers;                //!< how many mixers the records name
  bool has_audiodg;             //!< whether they name the audio engine
  bool may_close_mixer;         //!< whether this run may close a mixer
  bool may_pause_audio;         //!< whether this run may pause the audio service
  bool lookup_ready;            //!< whether that lookup is over
  bool lookup_cancelled;        //!< whether it was already asked to stop
  bool cancel_seen;             //!< whether the run was asked to stop
  bool hard_failure;            //!< whether the run stopped on a failure that err still holds
};

/**
 * @brief May this build pause the audio service at all
 *
 * Pausing it takes the sound away from the whole machine, so a build that ships only does it
 * when the settings say so in as many words.  A debug build is free to do it without that,
 * so the behaviour can be tried out at all.
 *
 * @param setting the value of allow_audio_service_pause from the settings file
 * @return true when the service may be paused
 */
static bool can_pause_audio_service(bool const setting) {
#ifdef NDEBUG
  return setting;
#else
  (void)setting;
  return true;
#endif
}

bool repair_can_pause_audio_service(struct settings const *const s) {
  return can_pause_audio_service((s != NULL) && s->allow_audio_service_pause);
}

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
char const *repair_option_audiodg_label(void) { return gettext("Allow pausing the Windows Audio service"); }

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
static void blocked_sentence(char const *const option, size_t const blockers, char *const out, size_t const out_size) {
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
 * @brief Tell the caller what the run is waiting for
 *
 * @param ctx the run that reports
 * @param stage what the run is waiting for
 */
static void report_stage(struct repair_run const *const ctx, enum repair_stage const stage) {
  if (ctx->progress != NULL) {
    ctx->progress(stage, ctx->step, ctx->total, ctx->progress_userdata);
  }
}

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
 * @brief Was the run asked to stop
 *
 * The event is the ask of the caller and is read between the steps of the run: a signaled
 * event ends the run with the aborted outcome instead of walking into the next step.
 *
 * @param run the run to look at
 * @return true when the run is to give up at the next boundary
 */
static bool run_stopped(struct repair_run *const run) {
  if (!run->cancel_seen && (run->cancel != NULL) && (WaitForSingleObject(run->cancel, 0) == WAIT_OBJECT_0)) {
    run->cancel_seen = true;
  }
  return run->cancel_seen;
}

/**
 * @brief Tell the run that the wait it waits for is over
 *
 * Called from the thread of the wait, so it does no more than mark the wait and wake the
 * caller that drives the run.  An ask to stop is not an end of the wait: the wait itself says
 * that it ended, and the run reads the ask at its next boundary.
 *
 * @param userdata the run
 * @param stopped true when the caller asked the wait to stop
 */
static void run_wake(void *const userdata, bool const stopped) {
  struct repair_run *const run = (struct repair_run *)userdata;

  if (stopped) {
    return;
  }
  run->lookup_ready = true;
  if (run->wake != NULL) {
    run->wake(run->wake_userdata);
  }
}

/**
 * @brief Start the lookup of the record of the attempt that just failed
 *
 * The veto record is written a moment after the removal call returns, so the lookup waits
 * for the record of this attempt instead of racing it.  Records older than since_ms belong
 * to an earlier attempt and are dropped by the lookup itself.
 *
 * @param run the run that reads
 * @param since_ms records older than this belong to an earlier attempt
 * @param err receives the failure to start the lookup
 * @return false when the lookup could not be started
 */
static NODISCARD bool veto_read_start(struct repair_run *const run, uint64_t const since_ms, struct ov_error *const err) {
  report_stage(run, REPAIR_STAGE_VETO_RECORD);
  run->lookup_ready = false;
  run->lookup_cancelled = false;
  run->lookup = veto_log_manager()->wait(run->settings->veto_log_name,
                                         run->settings->veto_provider_name,
                                         (unsigned short)run->settings->veto_event_id,
                                         since_ms,
                                         run->settings->veto_max_wait_ms,
                                         run_wake,
                                         run,
                                         err);
  return run->lookup != NULL;
}

/**
 * @brief Take the answer of the lookup once it is over
 *
 * @param run the run that reads
 * @param out_list receives the records, empty but never NULL when the lookup itself did not
 *        fail; NULL when it did, with the failure in the err the lookup was started with
 * @return true when the answer was taken, false when the run has to wait for it
 */
static NODISCARD bool veto_read_take(struct repair_run *const run, struct veto_event **const out_list) {
  if (!run->lookup_ready) {
    if (!run->lookup_cancelled && run->cancel_seen) {
      veto_log_manager()->wait_cancel(run->lookup);
      run->lookup_cancelled = true;
    }
    return false;
  }
  *out_list = veto_log_manager()->wait_take(&run->lookup);
  return true;
}

/**
 * @brief Release a list of closed process ids, which may never have been made
 *
 * @param closed the list, set to NULL
 */
static void closed_ids_destroy(uint32_t **const closed) {
  if (*closed != NULL) {
    OV_ARRAY_DESTROY(closed);
  }
}

/**
 * @brief Keep the records that are worth showing
 *
 * A veto record belongs to the device whose removal was stopped, so a record stays when the
 * instance it names is one of the ancestors of the devnode.  A record that names no
 * instance cannot be told apart from the records of the other devices, so it goes: acting
 * on it would close a process that blocks another device.
 *
 * @param list the list to shrink in place
 * @param devnode the KS devnode of the device being repaired
 * @param err receives the failure of the ancestor walk
 * @return false when the walk failed
 */
static NODISCARD bool veto_event_keep_records(struct veto_event **const list, wchar_t const *const devnode, struct ov_error *const err) {
  size_t kept = 0;
  size_t i = 0;
  size_t const n = OV_ARRAY_LENGTH(*list);
  bool success = false;

  for (i = 0; i < n; i++) {
    struct veto_event *const record = &(*list)[i];
    ov_tribool named = ov_false;

    if (record->device_instance == NULL) {
      continue;
    }
    named = device_veto_event_names_ancestor(record->device_instance, devnode, err);
    if (named == ov_indeterminate) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (named == ov_false) {
      continue;
    }
    if (kept != i) {
      struct veto_event const dropped = (*list)[kept];

      (*list)[kept] = *record;
      (*list)[i] = dropped;
    }
    kept++;
  }
  OV_ARRAY_SET_LENGTH(*list, kept);
  success = true;

cleanup:
  if (success) {
    for (i = kept; i < n; i++) {
      if ((*list)[i].path != NULL) {
        OV_ARRAY_DESTROY(&(*list)[i].path);
      }
      if ((*list)[i].device_instance != NULL) {
        OV_ARRAY_DESTROY(&(*list)[i].device_instance);
      }
    }
  }
  return success;
}

/**
 * @brief What the list of blockers holds
 *
 * The walk is the only way the list is read: a count of each kind is taken here rather than
 * asked of the list, so no entry leaves it.
 *
 * @param list the list to walk
 * @param out_mixers receives how many of the entries are the mixer
 * @param out_has_audiodg receives whether any of them is the audio engine
 * @return the number of entries
 */
static size_t blockers_survey(struct blocker const *const list, size_t *const out_mixers, bool *const out_has_audiodg) {
  struct blocker_iter it = {0};
  size_t count = 0;

  *out_mixers = 0;
  *out_has_audiodg = false;
  while (blocker_next(list, &it)) {
    count++;
    if (it.mixer_name_index != SIZE_MAX) {
      (*out_mixers)++;
    } else {
      *out_has_audiodg = true;
    }
  }
  return count;
}

/**
 * @brief Find the next mixer that was not closed yet
 *
 * The walk starts over on every call, so the list itself carries no cursor and stays the
 * same for whatever else reads it.  What a run already closed is in its own list, which is
 * what tells the walk where to go on.
 *
 * @param run the run
 * @param out receives the entry to close
 * @return false when no mixer is left
 */
static bool next_mixer(struct repair_run const *const run, struct blocker_iter *const out) {
  struct blocker_iter it = {0};

  while (blocker_next(run->blockers, &it)) {
    bool closed = false;

    if (it.mixer_name_index == SIZE_MAX) {
      continue;
    }
    for (size_t i = 0; i < OV_ARRAY_LENGTH(run->closed); i++) {
      if (run->closed[i] == it.pid) {
        closed = true;
        break;
      }
    }
    if (closed) {
      continue;
    }
    *out = it;
    return true;
  }
  return false;
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

/** @brief What one phase of a run ended in */
enum phase_result {
  PHASE_ADVANCE, //!< the phase is over and the next one is set
  PHASE_WAITING, //!< the run waits for a lookup and is to be taken up again when it is over
};

/**
 * @brief Take up the devices to repair, or look them up
 *
 * @param run the run
 * @param err receives the failure of the lookup
 * @return PHASE_ADVANCE
 */
static NODISCARD enum phase_result phase_start(struct repair_run *const run, struct ov_error *const err) {
  if (run_stopped(run)) {
    say_aborted(run->out);
    run->out->outcome = REPAIR_OUTCOME_ABORTED;
    run->phase = REPAIR_PHASE_DONE;
    return PHASE_ADVANCE;
  }
  if (run->list == NULL) {
    run->step++;
    report_stage(run, REPAIR_STAGE_PROBE);
    run->list = device_probe_all(run->settings->ks_service, run->settings->devices, OV_ARRAY_LENGTH(run->settings->devices), err);
    if (run->list == NULL) {
      if (!ov_error_is(err, ov_error_type_generic, ov_error_generic_not_found)) {
        OV_ERROR_ADD_TRACE(err);
        run->out->outcome = REPAIR_OUTCOME_FAILED;
        run->out->failure = repair_failure_device_state;
        say(run->out, "The device state could not be determined.");
        run->hard_failure = true;
        run->phase = REPAIR_PHASE_DONE;
        return PHASE_ADVANCE;
      }
      // no device of the service is present, which the run answers as ABSENT below
      OV_ERROR_DESTROY(err);
    }
    run->owns_list = true;
    run->probed = true;
    device_targets_keep_faulted(&run->list);
  }
  run->count = OV_ARRAY_LENGTH(run->list);
  run->total = (run->probed ? 1 : 0) + (run->count * REPAIR_STEPS_PER_TARGET);
  run->index = 0;
  run->outcome = REPAIR_OUTCOME_CLOSED;
  if (run->count == 0) {
    if (run_stopped(run)) {
      say_aborted(run->out);
      run->out->outcome = REPAIR_OUTCOME_ABORTED;
    } else {
      run->out->outcome = REPAIR_OUTCOME_ABSENT;
      say(run->out, "No Audient device was found.");
    }
    run->phase = REPAIR_PHASE_DONE;
    return PHASE_ADVANCE;
  }
  run->target = &run->list[0];
  run->phase = REPAIR_PHASE_TARGET;
  return PHASE_ADVANCE;
}

/**
 * @brief Take the answer of the removal once it is over
 *
 * @param run the run
 * @param out_removed receives whether the device was released
 * @return true when the answer was taken, false when the run has to wait for it
 */
static NODISCARD bool removal_take(struct repair_run *const run, bool *const out_removed) {
  if (!run->lookup_ready) {
    if (!run->lookup_cancelled && run->cancel_seen) {
      removal_manager()->op_cancel(run->removal);
      run->lookup_cancelled = true;
    }
    return false;
  }
  *out_removed = removal_manager()->op_release(&run->removal);
  return true;
}

/**
 * @brief Take one device up to its first removal
 *
 * The removal runs on a thread of its own: asking the configuration manager to remove a
 * subtree takes seconds on a device that is in use, and the window of the run has to stay
 * alive while it does.
 *
 * @param run the run
 * @param err receives the failure that stopped the device
 * @return PHASE_WAITING while the removal or the record of a refusal is being read
 */
static NODISCARD enum phase_result phase_target(struct repair_run *const run, struct ov_error *const err) {
  struct repair_result *const out = run->out;
  struct device_target const *const target = run->target;
  bool removed = false;

  if (run->removal != NULL) {
    if (!removal_take(run, &removed)) {
      return PHASE_WAITING;
    }
    if (removed) {
      run->target_outcome = REPAIR_OUTCOME_CLOSED;
      say(out, "The device was released.");
      logger_notef("remove", "The device was released on the first attempt.");
      run->phase = REPAIR_PHASE_CONFIRM;
      return PHASE_ADVANCE;
    }
    if (!removal_error_is_veto(err)) {
      // the reason is kept for the caller, and the code is what it tells the user to do about it
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      out->removal_error = removal_error_of(err);
      say(out, "The device could not be released.");
      run->hard_failure = true;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    // the veto is the answer the run works with, not a failure: the log keeps the reason, and
    // the second attempt needs the struct empty to raise its own refusal into
    OV_ERROR_DESTROY(err);
    if (run_stopped(run)) {
      say_aborted(out);
      run->target_outcome = REPAIR_OUTCOME_ABORTED;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    if (!veto_read_start(run, run->since, err)) {
      OV_ERROR_ADD_TRACE(err);
      out->failure = repair_failure_record;
      run->hard_failure = true;
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    run->phase = REPAIR_PHASE_VETO_FIRST;
    return PHASE_WAITING;
  }

  run->since = 0;
  run->events = NULL;
  run->blockers = NULL;
  closed_ids_destroy(&run->closed);
  run->closing = (struct blocker_iter){0};
  run->audio_service_paused = false;
  run->audio_service_checked = false;
  run->target_outcome = REPAIR_OUTCOME_FAILED;

  if (target->state == DEVICE_STATE_PROBLEM) {
    run->target_outcome = REPAIR_OUTCOME_OTHER_PROBLEM;
    say(out, "The device has a problem that this tool cannot solve.");
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  run->since = clock_now_ms();
  logger_notef("remove", "first attempt, device=%ls since=%llu", target->devnode, (unsigned long long)run->since);
  if (run_stopped(run)) {
    say_aborted(out);
    run->target_outcome = REPAIR_OUTCOME_ABORTED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  run->step++;
  report_stage(run, REPAIR_STAGE_REMOVE);
  run->lookup_ready = false;
  run->lookup_cancelled = false;
  run->removal = removal_manager()->remove_start(target->devnode, run_wake, run, err);
  if (run->removal == NULL) {
    OV_ERROR_ADD_TRACE(err);
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    say(out, "The device could not be released.");
    run->hard_failure = true;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  return PHASE_WAITING;
}

/**
 * @brief Read the record of the first refusal, then deal with the blockers and remove again
 *
 * @param run the run
 * @param err receives the failure that stopped the device
 * @return PHASE_WAITING when the record of the second refusal is being read
 */
static NODISCARD enum phase_result phase_veto_first(struct repair_run *const run, struct ov_error *const err) {
  struct repair_result *const out = run->out;
  struct device_target const *const target = run->target;

  if (!veto_read_take(run, &run->events)) {
    return PHASE_WAITING;
  }
  if (run->events == NULL) {
    // the records are what tell which process holds the device, so a lookup that failed is
    // one the repair cannot go on without: the reason is kept for the caller
    OV_ERROR_ADD_TRACE(err);
    out->failure = repair_failure_record;
    run->hard_failure = true;
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    say(out, "The device could not be released, and the reason could not be found.");
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  {
    size_t records = 0;
    struct veto_event_iter it = {0};

    while (veto_event_next(run->events, &it)) {
      records++;
    }
    logger_notef("veto", "the removal of device=%ls was vetoed by %u record(s)", target->devnode, (unsigned)records);
  }
  if (!veto_event_keep_records(&run->events, target->devnode, err)) {
    OV_ERROR_ADD_TRACE(err);
    out->failure = repair_failure_blockers;
    run->hard_failure = true;
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  // what a record says a process is called is what it was called then; the machine is
  // asked what the process the id names is now, and a process that took the id over after
  // the record was written is dropped by that same answer
  run->blockers = blocker_list_build(run->events, settings_mixer_names(run->settings), settings_mixer_name_count(run->settings), err);
  if (run->blockers == NULL) {
    OV_ERROR_ADD_TRACE(err);
    out->failure = repair_failure_blockers;
    run->hard_failure = true;
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  if (run_stopped(run)) {
    say_aborted(out);
    run->target_outcome = REPAIR_OUTCOME_ABORTED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  {
    size_t const blockers = blockers_survey(run->blockers, &run->mixers, &run->has_audiodg);

    if (blockers == 0) {
      // every process the records name is already gone or is not the one they named
      logger_notef("probe", "none of the records names a running process");
    }
    run->may_close_mixer = run->allow_mixer_close && (run->mixers > 0);
    run->may_pause_audio = run->allow_audiodg_close && run->has_audiodg;
    if ((blockers > 0) && !run->may_close_mixer && !run->may_pause_audio) {
      char option_buf[256] = {0};
      char const *option = NULL;
      char sentence[256];

      run->target_outcome = REPAIR_OUTCOME_VETOED;
      // the blockers the user did not allow this tool to close are still worth naming: the
      // kind that is there says which option would have helped
      if (run->mixers > 0) {
        repair_option_mixer_label(run->settings, option_buf, sizeof(option_buf));
        option = option_buf;
      } else if (run->has_audiodg) {
        option = repair_option_audiodg_label();
      }
      blocked_sentence(option, blockers, sentence, sizeof(sentence));
      say(out, sentence);
      if (veto_event_next(run->events, &(struct veto_event_iter){0})) {
        // the caller shows one list, so a run of several devices hands the last one over and
        // what an earlier device named is released rather than dropped
        veto_event_destroy_list(&out->events);
        out->events = run->events;
        run->events = NULL;
        closed_ids_destroy(&out->closed);
        out->closed = run->closed;
        run->closed = NULL;
      }
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
  }
  run->phase = REPAIR_PHASE_MIXER;
  return PHASE_ADVANCE;
}

/**
 * @brief Whether an act that failed was cut short by the ask to stop
 *
 * A close and a pause both wait for the machine to answer, and an ask to stop ends that
 * wait where it stands.  What the act was asked to do then has no answer, which is not the
 * same as an answer that the machine refused.
 *
 * @param err the failure the act left, may be NULL
 * @return true when it was the ask that ended the act
 */
static bool act_stopped(struct ov_error const *const err) {
  return (err != NULL) && ov_error_is(err, ov_error_type_generic, ov_error_generic_abort);
}

/**
 * @brief Close the mixers the record names, one at a time
 *
 * Each close is an act of its own, so the run is taken up again between two of them.
 *
 * @param run the run
 * @param err receives the failure that stopped the device
 * @return PHASE_WAITING while a close is in flight
 */
static NODISCARD enum phase_result phase_mixer(struct repair_run *const run, struct ov_error *const err) {
  enum phase_result result = PHASE_ADVANCE;

  if (run->act == NULL) {
    struct blocker_iter b = {0};

    if (!next_mixer(run, &b)) {
      // no mixer is left that this run did not close yet, so the mixes are over
      run->phase = run->may_pause_audio ? REPAIR_PHASE_PAUSE : REPAIR_PHASE_SECOND;
      goto cleanup;
    }
    if (OV_ARRAY_LENGTH(run->closed) == 0) {
      report_stage(run, REPAIR_STAGE_MIXER);
    }
    logger_notef("close", "mixer pid=%lu", (unsigned long)b.pid);
    run->lookup_ready = false;
    run->lookup_cancelled = false;
    run->closing = b;
    run->act = process_manager()->terminate(b.pid, b.event_time_ms, run_wake, run, err);
    if (run->act == NULL) {
      OV_ERROR_ADD_TRACE(err);
      run->out->failure = repair_failure_process_close;
      run->out->failed_mixer = b.mixer_name_index;
      run->hard_failure = true;
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      run->phase = REPAIR_PHASE_CLEANUP;
      goto cleanup;
    }
    result = PHASE_WAITING; // the close is in flight, and the run is taken up again
    goto cleanup;
  }
  {
    struct blocker_iter const b = run->closing;

    if (!run->lookup_ready) {
      if (!run->lookup_cancelled && run->cancel_seen) {
        process_manager()->act_cancel(run->act);
        run->lookup_cancelled = true;
      }
      result = PHASE_WAITING;
      goto cleanup;
    }
    if (!process_manager()->act_release(&run->act)) {
      if (act_stopped(err)) {
        OV_ERROR_DESTROY(err);
        say_aborted(run->out);
        run->target_outcome = REPAIR_OUTCOME_ABORTED;
        run->phase = REPAIR_PHASE_CLEANUP;
        goto cleanup;
      }
      OV_ERROR_ADD_TRACE(err);
      run->out->outcome = REPAIR_OUTCOME_FAILED;
      sayf(run->out, "%1$hs could not be closed.", "The mixer");
      run->out->failure = repair_failure_process_close;
      run->out->failed_mixer = b.mixer_name_index;
      run->hard_failure = true;
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      run->phase = REPAIR_PHASE_CLEANUP;
      goto cleanup;
    }
    if (!OV_ARRAY_PUSH(&run->closed, b.pid)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
      OV_ERROR_ADD_TRACE(err);
      run->out->outcome = REPAIR_OUTCOME_FAILED;
      run->out->failure = repair_failure_process_close;
      run->hard_failure = true;
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      run->phase = REPAIR_PHASE_CLEANUP;
      goto cleanup;
    }
    // the next mixer is taken up by the next step of the run, so the run is not stuck in here
  }

cleanup:
  return result;
}

/**
 * @brief Pause the Windows Audio service, so the audio engine lets the device go
 *
 * The engine is started by this service and comes back within milliseconds of being
 * terminated, so terminating it does not free the device.  Pausing the service does, and it
 * is started again when the device is done with.
 *
 * @param run the run
 * @param err receives the failure that stopped the device
 * @return PHASE_WAITING while the pause is in flight
 */
static NODISCARD enum phase_result phase_pause(struct repair_run *const run, struct ov_error *const err) {
  if (run->act == NULL) {
    if (!run->can_pause_audio_service) {
      logger_notef("close", "the audio engine holds the device, but pausing the service is not allowed");
      run->phase = REPAIR_PHASE_SECOND;
      return PHASE_ADVANCE;
    }
    report_stage(run, REPAIR_STAGE_PAUSE);
    logger_notef("close", "pause the audio service");
    run->lookup_ready = false;
    run->lookup_cancelled = false;
    run->act = process_manager()->pause_audio_service(run_wake, run, err);
    if (run->act == NULL) {
      OV_ERROR_ADD_TRACE(err);
      run->out->failure = repair_failure_audio_service_pause;
      run->hard_failure = true;
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    // the service may be on its way down from this moment, so it is asked to come back
    // whatever comes of the act: one that was stopped halfway may have sent the control
    // already, and a machine with no sound is worse than the device this run was about
    run->audio_service_paused = true;
    return PHASE_WAITING;
  }
  if (!run->lookup_ready) {
    if (!run->lookup_cancelled && run->cancel_seen) {
      process_manager()->act_cancel(run->act);
      run->lookup_cancelled = true;
    }
    return PHASE_WAITING;
  }
  if (!process_manager()->act_release(&run->act)) {
    if (act_stopped(err)) {
      OV_ERROR_DESTROY(err);
      say_aborted(run->out);
      run->target_outcome = REPAIR_OUTCOME_ABORTED;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    OV_ERROR_ADD_TRACE(err);
    run->out->outcome = REPAIR_OUTCOME_FAILED;
    say(run->out, "The Windows Audio service could not be paused.");
    logger_notef("close", "the audio service could not be paused");
    run->out->failure = repair_failure_audio_service_pause;
    run->hard_failure = true;
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  logger_notef("close", "the audio service is paused");
  run->phase = REPAIR_PHASE_SECOND;
  return PHASE_ADVANCE;
}

/**
 * @brief Ask for the second removal
 *
 * @param run the run
 * @param err receives the failure that stopped the device
 * @return PHASE_WAITING while the removal or the record of a second refusal is being read
 */
static NODISCARD enum phase_result phase_second(struct repair_run *const run, struct ov_error *const err) {
  struct repair_result *const out = run->out;
  struct device_target const *const target = run->target;
  bool removed = false;

  if (run->removal != NULL) {
    if (!removal_take(run, &removed)) {
      return PHASE_WAITING;
    }
    if (removed) {
      run->target_outcome = REPAIR_OUTCOME_CLOSED;
      say(out, "The device was released.");
      logger_notef("remove", "The device was released on the second attempt.");
      run->phase = REPAIR_PHASE_CONFIRM;
      return PHASE_ADVANCE;
    }
    if (!removal_error_is_veto(err)) {
      // the reason is kept for the caller, and the code is what it tells the user to do about it
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      out->removal_error = removal_error_of(err);
      say(out, "The device could not be released.");
      run->hard_failure = true;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    // the veto is the answer the run works with, not a failure: the log keeps the reason, and
    // the lookup is asked for the records that say what holds the device now
    OV_ERROR_DESTROY(err);
    if (run_stopped(run)) {
      say_aborted(out);
      run->target_outcome = REPAIR_OUTCOME_ABORTED;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    if (!veto_read_start(run, run->since, err)) {
      OV_ERROR_ADD_TRACE(err);
      out->failure = repair_failure_record;
      run->hard_failure = true;
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    run->phase = REPAIR_PHASE_VETO_SECOND;
    return PHASE_WAITING;
  }

  if (run_stopped(run)) {
    say_aborted(out);
    run->target_outcome = REPAIR_OUTCOME_ABORTED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  run->since = clock_now_ms();
  logger_notef("remove", "second attempt, device=%ls since=%llu", target->devnode, (unsigned long long)run->since);
  run->step++;
  report_stage(run, REPAIR_STAGE_REMOVE);
  run->lookup_ready = false;
  run->lookup_cancelled = false;
  run->removal = removal_manager()->remove_start(target->devnode, run_wake, run, err);
  if (run->removal == NULL) {
    OV_ERROR_ADD_TRACE(err);
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    say(out, "The device could not be released.");
    run->hard_failure = true;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  return PHASE_WAITING;
}

/**
 * @brief Ask for the sound of the machine to be there again
 *
 * The tool is used because the sound is wanted, and a machine whose audio service is stopped
 * has none: a run that leaves it stopped has not done its work, however well the device
 * itself came out.  The service is asked about whether this run stopped it or found it
 * stopped, because a service left stopped by an earlier run of this program is the same
 * machine without sound.  The act is not asked to stop for the same reason.
 *
 * @param run the run
 * @param err receives the failure to have the service running
 * @return PHASE_WAITING while the start is in flight
 */
static NODISCARD enum phase_result phase_resume(struct repair_run *const run, struct ov_error *const err) {
  // the sound of the machine is asked for whatever came of the device, so this step runs even
  // when the run already stopped for a failure.  That failure is the one the caller acts on,
  // and this one is often no more than a consequence of it, so it does not take its place:
  // the act is given no place of its own to write when the run already stopped, and what came
  // of it joins the run's error as a second entry, so nothing is lost and nothing is hidden

  if (run->act == NULL) {
    report_stage(run, REPAIR_STAGE_START);
    run->lookup_ready = false;
    run->lookup_cancelled = false;
    logger_notef("close", "start the audio service again");
    run->act = process_manager()->resume_audio_service(run_wake, run, run->hard_failure ? NULL : err);
    if (run->act == NULL) {
      if (run->hard_failure) {
        OV_ERROR_PUSH(err, ov_error_type_generic, ov_error_generic_fail, "the Windows Audio service could not be started again");
      } else {
        OV_ERROR_ADD_TRACE(err);
      }
      logger_notef("close", "the audio service could not be started again");
      if (!run->hard_failure) {
        run->out->outcome = REPAIR_OUTCOME_FAILED;
        run->out->failure = repair_failure_audio_service_start;
        say(run->out, "The Windows Audio service could not be started again.");
        run->target_outcome = REPAIR_OUTCOME_FAILED;
      }
      run->hard_failure = true;
      run->audio_service_paused = false;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    return PHASE_WAITING;
  }
  if (!run->lookup_ready) {
    return PHASE_WAITING;
  }
  if (!process_manager()->act_release(&run->act)) {
    if (run->hard_failure) {
      OV_ERROR_PUSH(err, ov_error_type_generic, ov_error_generic_fail, "the Windows Audio service could not be started again");
    } else {
      OV_ERROR_ADD_TRACE(err);
    }
    logger_notef("close", "the audio service could not be started again");
    if (!run->hard_failure) {
      run->out->outcome = REPAIR_OUTCOME_FAILED;
      run->out->failure = repair_failure_audio_service_start;
      say(run->out, "The Windows Audio service could not be started again.");
      run->target_outcome = REPAIR_OUTCOME_FAILED;
    }
    run->hard_failure = true;
  } else {
    logger_notef("close", "the audio service is running again");
  }
  run->audio_service_paused = false;
  run->phase = REPAIR_PHASE_CLEANUP;
  return PHASE_ADVANCE;
}
/**
 * @brief Read the record of the second refusal and leave the device be
 *
 * @param run the run
 * @param err receives the failure that stopped the device
 * @return PHASE_ADVANCE once the records are handed to the caller
 */
static NODISCARD enum phase_result phase_veto_second(struct repair_run *const run, struct ov_error *const err) {
  struct repair_result *const out = run->out;
  struct veto_event *current = NULL;

  if (!veto_read_take(run, &current)) {
    return PHASE_WAITING;
  }
  if (current == NULL) {
    // the records of the second refusal are what the run answers with, so a lookup that
    // failed is one the repair cannot go on without: the reason is kept for the caller
    OV_ERROR_ADD_TRACE(err);
    out->failure = repair_failure_record;
    run->hard_failure = true;
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  veto_event_destroy_list(&run->events);
  run->events = current;
  current = NULL;
  if (!veto_event_keep_records(&run->events, run->target->devnode, err)) {
    OV_ERROR_ADD_TRACE(err);
    out->failure = repair_failure_blockers;
    run->hard_failure = true;
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  if (veto_event_next(run->events, &(struct veto_event_iter){0})) {
    // the caller shows one list, so a run of several devices hands the last one over and
    // what an earlier device named is released rather than dropped
    veto_event_destroy_list(&out->events);
    out->events = run->events;
    run->events = NULL;
    closed_ids_destroy(&out->closed);
    out->closed = run->closed;
    run->closed = NULL;
  }
  run->target_outcome = REPAIR_OUTCOME_VETOED;
  say(out, "The device is still in use.");
  logger_notef("remove", "The blockers were closed but the device is still blocked.");
  run->phase = REPAIR_PHASE_CLEANUP;
  return PHASE_ADVANCE;
}

/**
 * @brief Ask the released device whether it is an audio device again
 *
 * @param run the run
 * @param err receives the failure of the questions
 * @return PHASE_ADVANCE
 */
static NODISCARD enum phase_result phase_confirm(struct repair_run *const run, struct ov_error *const err) {
  struct device_facts facts = {0};

  if (run->watch == NULL) {
    run->step++;
    report_stage(run, REPAIR_STAGE_CONFIRM);
    run->lookup_ready = false;
    run->lookup_cancelled = false;
    run->watch = device_manager()->watch(
        run->target->devnode, (unsigned long)run->settings->confirm_max_wait_ms, device_facts_is_healthy, run_wake, run, err);
    if (run->watch == NULL) {
      OV_ERROR_ADD_TRACE(err);
      run->out->failure = repair_failure_not_confirmed;
      run->hard_failure = true;
      run->target_outcome = REPAIR_OUTCOME_FAILED;
      run->phase = REPAIR_PHASE_CLEANUP;
      return PHASE_ADVANCE;
    }
    return PHASE_WAITING;
  }
  if (!run->lookup_ready) {
    if (!run->lookup_cancelled && run->cancel_seen) {
      device_manager()->watch_cancel(run->watch);
      run->lookup_cancelled = true;
    }
    return PHASE_WAITING;
  }
  facts = device_manager()->watch_take(&run->watch, err);
  if (run_stopped(run)) {
    // the ask came first: what the watch would have answered is not the answer the caller
    // waits for, so the reason it left behind goes
    if (err != NULL) {
      OV_ERROR_DESTROY(err);
    }
    say_aborted(run->out);
    run->target_outcome = REPAIR_OUTCOME_ABORTED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  if (device_facts_state(&facts) == DEVICE_STATE_QUERY_FAILED) {
    // the watch could not look, so the run cannot tell what came of the device: the reason
    // is kept for the caller and the run stops
    OV_ERROR_ADD_TRACE(err);
    run->out->outcome = REPAIR_OUTCOME_FAILED;
    run->out->failure = repair_failure_not_confirmed;
    run->hard_failure = true;
    run->target_outcome = REPAIR_OUTCOME_FAILED;
    run->phase = REPAIR_PHASE_CLEANUP;
    return PHASE_ADVANCE;
  }
  if (device_facts_is_healthy(&facts)) {
    run->target_outcome = REPAIR_OUTCOME_CLOSED;
  } else {
    // the look answered that the device is not an audio device: that is what came of it
    run->out->outcome = REPAIR_OUTCOME_FAILED;
    run->out->failure = repair_failure_not_recovered;
    say(run->out, "The device did not come back as an audio device.");
    run->target_outcome = REPAIR_OUTCOME_FAILED;
  }
  run->phase = REPAIR_PHASE_CLEANUP;
  return PHASE_ADVANCE;
}

/**
 * @brief Settle what the device left behind
 *
 * Whatever came of the device, the sound of the machine comes back here: a service left
 * stopped is a machine with no audio at all, which is worse than the device this run was
 * about.
 *
 * @param run the run
 * @param err the failure that stopped the device, left as it is
 * @return PHASE_ADVANCE
 */
static NODISCARD enum phase_result phase_cleanup(struct repair_run *const run, struct ov_error *const err) {
  (void)err;
  // a machine with no sound is not what this tool is for, so the service is asked to be
  // running again at the end of every run, whether this run stopped it or found it stopped
  if (run->audio_service_paused || !run->audio_service_checked) {
    run->audio_service_checked = true;
    run->phase = REPAIR_PHASE_RESUME;
    return PHASE_ADVANCE;
  }
  veto_event_destroy_list(&run->events);
  blocker_list_destroy(&run->blockers);
  closed_ids_destroy(&run->closed);
  if (run->hard_failure) {
    // the reason is the caller's and the run stops here: nothing after this step writes
    // into err again, so what the caller reads is the one failure that stopped the run
    run->out->outcome = REPAIR_OUTCOME_FAILED;
    run->phase = REPAIR_PHASE_DONE;
    return PHASE_ADVANCE;
  }
  run->phase = REPAIR_PHASE_NEXT;
  return PHASE_ADVANCE;
}

/**
 * @brief Fold what came of one device into the run, and take up the next one
 *
 * @param run the run
 * @return PHASE_ADVANCE
 */
static enum phase_result phase_next(struct repair_run *const run) {
  enum repair_outcome const one = run->target_outcome;

  if (one == REPAIR_OUTCOME_ABORTED) {
    run->outcome = one;
    run->out->outcome = one;
    run->phase = REPAIR_PHASE_DONE;
    return PHASE_ADVANCE;
  }
  if (worse_outcome(one, run->outcome) == one) {
    run->outcome = one;
  }
  if (one == REPAIR_OUTCOME_FAILED) {
    run->out->outcome = run->outcome;
    run->phase = REPAIR_PHASE_DONE;
    return PHASE_ADVANCE;
  }
  run->index++;
  if (run->index >= run->count) {
    run->out->outcome = run->outcome;
    run->phase = REPAIR_PHASE_DONE;
    return PHASE_ADVANCE;
  }
  run->target = &run->list[run->index];
  run->phase = REPAIR_PHASE_TARGET;
  return PHASE_ADVANCE;
}

/**
 * @brief Take the run through the phase it stands at
 *
 * @param run the run
 * @param err receives the failure that stopped the run
 * @return what the phase ended in
 */
static NODISCARD enum phase_result phase_step(struct repair_run *const run, struct ov_error *const err) {
  run_stopped(run); // the ask to stop is read at every boundary between the steps

  switch (run->phase) {
  case REPAIR_PHASE_START:
    return phase_start(run, err);
  case REPAIR_PHASE_TARGET:
    return phase_target(run, err);
  case REPAIR_PHASE_VETO_FIRST:
    return phase_veto_first(run, err);
  case REPAIR_PHASE_MIXER:
    return phase_mixer(run, err);
  case REPAIR_PHASE_PAUSE:
    return phase_pause(run, err);
  case REPAIR_PHASE_SECOND:
    return phase_second(run, err);
  case REPAIR_PHASE_VETO_SECOND:
    return phase_veto_second(run, err);
  case REPAIR_PHASE_CONFIRM:
    return phase_confirm(run, err);
  case REPAIR_PHASE_RESUME:
    return phase_resume(run, err);
  case REPAIR_PHASE_CLEANUP:
    return phase_cleanup(run, err);
  case REPAIR_PHASE_NEXT:
    return phase_next(run);
  case REPAIR_PHASE_DONE:
    break;
  }
  return PHASE_ADVANCE;
}

NODISCARD enum repair_step_result repair_step(struct repair_run *const run, struct ov_error *const err) {
  if (run == NULL) {
    return REPAIR_STEP_DONE;
  }
  for (;;) {
    if ((run->phase == REPAIR_PHASE_DONE) || (phase_step(run, err) == PHASE_WAITING)) {
      break;
    }
  }
  return (run->phase == REPAIR_PHASE_DONE) ? REPAIR_STEP_DONE : REPAIR_STEP_WAITING;
}

/**
 * @brief Take up a run
 *
 * The run repairs every device the caller names, or, when it names none, every device that
 * is faulted.  It is taken forward one piece at a time with repair_step, because a lookup it
 * waits for runs on a thread of its own and the caller is not to be held up by it.
 *
 * @param s the settings this run works for, may not be NULL
 * @param targets the devices to repair, NULL says the run looks for the faulted devices
 *        itself; an empty list is the same as no device at all
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @param progress may be NULL
 * @param progress_userdata handed back to progress
 * @param cancel may be NULL; a manual-reset event the caller owns.  A signaled event ends
 *        the run with the aborted outcome between its steps, and ends a lookup it waits on
 * @param wake called when a lookup is over, may be NULL
 * @param wake_userdata handed to wake
 * @param out receives what happened; release with repair_result_release
 * @param err receives the failure that stopped the run
 * @return the run, release with repair_run_end; NULL when it could not be taken up
 */
NODISCARD struct repair_run *repair_run_begin(struct settings const *const s,
                                              struct device_target *const targets,
                                              bool const allow_mixer_close,
                                              bool const allow_audiodg_close,
                                              repair_progress_fn const progress,
                                              void *const progress_userdata,
                                              HANDLE const cancel,
                                              repair_wake_fn const wake,
                                              void *const wake_userdata,
                                              struct repair_result *const out,
                                              struct ov_error *const err) {
  struct repair_run *run = NULL;

  if ((s == NULL) || (out == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return NULL;
  }
  if (!OV_REALLOC(&run, 1, sizeof(*run))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    return NULL;
  }
  memset(run, 0, sizeof(*run));
  memset(out, 0, sizeof(*out));
  run->settings = s;
  run->allow_mixer_close = allow_mixer_close;
  run->allow_audiodg_close = allow_audiodg_close;
  // the service is only paused when this build allows it, and, in a build that ships, only
  // when the settings say so: pausing it takes the sound away from the whole machine
  run->can_pause_audio_service = repair_can_pause_audio_service(s);
  run->progress = progress;
  run->progress_userdata = progress_userdata;
  run->cancel = cancel;
  run->wake = wake;
  run->wake_userdata = wake_userdata;
  run->err = err;
  run->out = out;
  run->list = targets;
  run->phase = REPAIR_PHASE_START;
  return run;
}

/**
 * @brief Release what a run holds
 *
 * @param run the run, set to NULL afterwards, NULL is allowed
 */
NODISCARD bool repair_run_finish(struct repair_run **const run, struct ov_error *const err) {
  bool ok = false;

  if ((run == NULL) || (*run == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  ok = !(*run)->hard_failure;
  if ((*run)->removal != NULL) {
    removal_manager()->op_cancel((*run)->removal);
    removal_manager()->op_release(&(*run)->removal);
  }
  if ((*run)->owns_list) {
    device_targets_release(&(*run)->list);
  }
  closed_ids_destroy(&(*run)->closed);
  OV_FREE(run);
  return ok;
}

/** @brief Wake the thread that drives a run */
static void wake_set(void *const userdata) {
  struct repair_run *const run = (struct repair_run *)userdata;

  if (!SetEvent(run->wake_handle)) {
    OV_ERROR_SET_HRESULT(run->err, HRESULT_FROM_WIN32(GetLastError()));
    run->wake_failed = true;
  }
}

/**
 * @brief Run the repair to its end on this thread
 *
 * The pieces the run is cut into, with a wait between them for the lookup that is over.  A
 * caller that has a window takes the run forward with repair_step itself instead, so its
 * window stays alive while a lookup takes its seconds.
 *
 * @param s the settings this run works for
 * @param targets the devices to repair, NULL says the run looks for the faulted devices itself
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @param progress may be NULL; it is called from the thread that runs the repair
 * @param progress_userdata handed back to progress
 * @param cancel the flag of the caller, may be NULL
 * @param out receives what happened; release with repair_result_release
 * @param err receives the failure that stopped the run
 * @return false only on a hard failure.  The outcome carries what came of the devices: a
 *         failure the run cannot go on with stops it, and the reason is what err holds when
 *         the call returns false.
 */
NODISCARD bool repair_run(struct settings const *const s,
                          struct device_target *const targets,
                          bool const allow_mixer_close,
                          bool const allow_audiodg_close,
                          repair_progress_fn const progress,
                          void *const progress_userdata,
                          HANDLE const cancel,
                          struct repair_result *const out,
                          struct ov_error *const err) {
  struct repair_run *run = NULL;
  HANDLE wake = NULL;
  bool ok = false;
  bool wait_failed = false;

  if ((s == NULL) || (out == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  wake = CreateEventW(NULL, FALSE, FALSE, NULL);
  if (wake == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  run = repair_run_begin(s, targets, allow_mixer_close, allow_audiodg_close, progress, progress_userdata, cancel, wake_set, NULL, out, err);
  if (run == NULL) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  // the wake carries the run, so a wait that cannot be woken leaves its reason in err
  run->wake_userdata = run;
  run->wake_handle = wake;
  while (repair_step(run, err) != REPAIR_STEP_DONE) {
    HANDLE ends[2];
    DWORD count = 1;
    DWORD answer = 0;

    if (run->wake_failed) {
      // the thread of the lookup could not wake this one, and its reason is already in err
      OV_ERROR_ADD_TRACE(err);
      wait_failed = true;
      break;
    }
    ends[0] = wake;
    if ((cancel != NULL) && !run->cancel_seen) {
      ends[1] = cancel;
      count = 2;
    }
    answer = WaitForMultipleObjects(count, ends, FALSE, INFINITE);
    if (answer == WAIT_FAILED) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      wait_failed = true;
      break; // the run cannot be driven any further, and what it holds is released below
    }
    if ((count == 2) && (answer == (WAIT_OBJECT_0 + 1))) {
      // the run reads the ask at its next boundary, so the wake is what makes it take that step
      if (!SetEvent(wake)) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
        wait_failed = true;
        break;
      }
    }
  }
  ok = repair_run_finish(&run, err) && !wait_failed;

cleanup:
  if (wake != NULL) {
    if (!CloseHandle(wake)) {
      if (ok) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      } else {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
      ok = false;
    }
  }
  return ok;
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
  veto_event_destroy_list(&r->events);
  closed_ids_destroy(&r->closed);
  r->outcome = REPAIR_OUTCOME_UNKNOWN;
}
