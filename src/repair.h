#pragma once

#include <ovbase.h>

#include <windows.h>

#include "removal_manager.h"

struct settings;
struct device_target;
struct veto_event;
struct blocker;

/**
 * @brief What came of the run
 *
 * The device is not in the state this tool is for, and the two ways that happens need
 * different sentences: a device that is not there at all, and a device that is there with a
 * problem the removal does not cure.
 */
enum repair_outcome {
  REPAIR_OUTCOME_UNKNOWN,
  REPAIR_OUTCOME_ABSENT,        // no device of that service is present
  REPAIR_OUTCOME_OTHER_PROBLEM, // the device is there, but not with the problem this tool removes
  REPAIR_OUTCOME_CLOSED,        // the removal succeeded
  REPAIR_OUTCOME_VETOED,        // still blocked; events may name who blocks it
  REPAIR_OUTCOME_FAILED,        // any other failure
  REPAIR_OUTCOME_ABORTED,       // the reader stopped it and it quit between its steps
};

/**
 * @brief Why a run could not go on
 *
 * The outcome says what came of the devices; this says which step of the repair could not be
 * taken, for a caller that has to tell the user what to do about it.  Every one of these is
 * a step the repair cannot do without, so the run stops there.
 */
enum repair_failure {
  repair_failure_none,                //!< the outcome carries what happened
  repair_failure_device_state,        //!< the state of the devices could not be determined
  repair_failure_record,              //!< the record that says what holds the device could not be read
  repair_failure_blockers,            //!< the processes the records name could not be looked at
  repair_failure_process_close,       //!< a process that holds the device could not be closed
  repair_failure_audio_service_pause, //!< the audio service could not be stopped
  repair_failure_audio_service_start, //!< the audio service could not be started again
  repair_failure_not_recovered,       //!< the device was released but did not come back as an audio device
  repair_failure_not_confirmed,       //!< the device was released but whether it came back could not be checked
};

/**
 * @brief What the caller should tell the user
 *
 * The outcome of the run and, when the system refused the removal, why.  The sentences
 * themselves are chosen by the UI, next to the dialog that shows them; the numbers stay in
 * the log.  The reason of a refusal is carried as the code of the removal error, 0 when the
 * outcome is not a refusal of the removal, and which step of the repair could not be taken
 * is carried next to it, so a dialog has something to say about a run that stopped.
 *
 * The events are the records of the veto, shown as they were written; the processes this
 * run closed are not among them.
 */
struct repair_result {
  enum repair_outcome outcome;
  enum repair_failure failure;       // repair_failure_none unless a step of the repair failed
  size_t failed_mixer;               // which of the mixer names of the settings could not be closed
  enum app_error_code removal_error; // 0 unless the removal was refused
  struct veto_event *events;         // ovarray or NULL.  released by repair_result_release
  uint32_t *closed;                  // ovarray of the process ids this run closed.  released by repair_result_release
  char message[512];                 // UTF-8, meant for the UI log
};

/**
 * @brief The words of the setting that lets this tool close the mixer
 *
 * The window puts them on its checkbox and the log quotes them when a veto is left
 * standing, so they are said once here.  They are translated: whoever reads the log of a
 * window in their own language meets the words they can press.  The names come from the
 * settings: the display name of the mixer and its image name.
 *
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the caption, UTF-8, always terminated
 * @param out_size room of out
 */
void repair_option_mixer_label(struct settings const *const s, char *const out, size_t const out_size);

/**
 * @brief The words the stage report shows while a mixer is being closed, same names as the option
 *
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the caption, UTF-8, always terminated
 * @param out_size room of out
 */
void repair_stage_mixer_caption(struct settings const *const s, char *const out, size_t const out_size);

/**
 * @brief The words of the setting that lets this tool pause the Windows Audio service
 *
 * @return the caption, UTF-8
 */
char const *repair_option_audio_service_label(void);

/**
 * @brief May this build pause the audio service for these settings at all
 *
 * Pausing it takes the sound away from the whole machine, so a build that ships only does
 * it when the settings say so in as many words, and the window does not offer the choice
 * either.  A debug build is free to do it without that, so the behaviour can be tried out
 * at all.
 *
 * @param s the settings that may allow it, may be NULL
 * @return true when the service may be paused and the window offers the choice
 */
bool repair_can_pause_audio_service(struct settings const *const s);

/**
 * @brief What the run is waiting for at the moment
 *
 * The caller can show it while it happens.
 */
enum repair_stage {
  REPAIR_STAGE_WAIT,        // the devices are still coming back on the bus
  REPAIR_STAGE_PROBE,       // the state of the devices is being determined
  REPAIR_STAGE_REMOVE,      // the removal is with the system, waiting for its answer
  REPAIR_STAGE_MIXER,       // the mixer is being closed and waited for
  REPAIR_STAGE_PAUSE,       // the audio service is being stopped and waited for
  REPAIR_STAGE_VETO_RECORD, // the veto record is being waited for and read
  REPAIR_STAGE_CONFIRM,     // the released device is being asked whether it is an audio device again
  REPAIR_STAGE_START        // the audio service is being started and waited for
};

/**
 * @brief The steps one device takes on the longest way through its repair
 *
 * Remove, the veto record, the two closable blockers, the second
 * removal, the second veto record and the confirmation that the
 * device came back as an audio device.  A shorter way stops early;
 * the run counts up to this many steps for every device it repairs.
 * The wait a scheduled run starts with and the probe of the devices
 * are counted by the caller on top of these.
 */
enum { REPAIR_STEPS_PER_TARGET = 7 };

/**
 * @brief Reports the progress of a run
 *
 * @param stage what the run is waiting for
 * @param step the step of the run, counted from one
 * @param total how many steps the longest way takes
 * @param userdata what the caller handed to repair_run
 */
typedef void (*repair_progress_fn)(enum repair_stage const stage, size_t const step, size_t const total, void *const userdata);

/**
 * @brief Tells the caller that a wait of the run is over
 *
 * Called from the thread that finished the wait, so it does no more than put the caller's
 * own ask up: it posts a message, sets an event, and returns.
 *
 * @param userdata what the caller handed to repair_run_begin
 */
typedef void (*repair_wake_fn)(void *userdata);

/**
 * @brief A run in flight
 *
 * A run is taken forward one piece at a time, so a caller that has a window can keep it
 * alive while a lookup takes its seconds.  repair_run takes the same run through on one
 * thread for a caller that has nothing else to do.
 */
struct repair_run;

/** @brief What one step of a run ended in */
enum repair_step_result {
  REPAIR_STEP_WAITING, //!< a lookup is in flight; take the run up again when wake was called
  REPAIR_STEP_DONE,    //!< the run is over and out holds what came of it
};

/**
 * @brief Take up a run
 *
 * @param s the settings this run works for, may not be NULL
 * @param targets the devices to repair, NULL says the run looks for the faulted devices itself
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audio_service_pause the user allowed pausing the Windows Audio service
 * @param progress may be NULL
 * @param progress_userdata handed back to progress
 * @param cancel may be NULL; a manual-reset event the caller owns
 * @param wake called when a lookup is over, may be NULL
 * @param wake_userdata handed to wake
 * @param out receives what happened; release with repair_result_release
 * @param err receives the failure that stopped the run
 * @return the run, release with repair_run_finish; NULL when it could not be taken up
 */
NODISCARD struct repair_run *repair_run_begin(struct settings const *s,
                                              struct device_target *targets,
                                              bool allow_mixer_close,
                                              bool allow_audio_service_pause,
                                              repair_progress_fn progress,
                                              void *progress_userdata,
                                              HANDLE cancel,
                                              repair_wake_fn wake,
                                              void *wake_userdata,
                                              struct repair_result *out,
                                              struct ov_error *err);

/**
 * @brief Take a run forward as far as it goes without waiting
 *
 * @param run the run
 * @param err receives the failure that stopped the run
 * @return REPAIR_STEP_WAITING while a lookup is in flight, REPAIR_STEP_DONE when the run is over
 */
NODISCARD enum repair_step_result repair_step(struct repair_run *run, struct ov_error *err);

/**
 * @brief Settle a run and release it
 *
 * @param run the run, set to NULL afterwards
 * @param err the failure that stopped the run, left in err for the caller when the run
 *            failed hard
 * @return false only on a hard failure, with err holding why
 */
NODISCARD bool repair_run_finish(struct repair_run **run, struct ov_error *err);

/**
 * @brief Run the repair
 *
 * The documented sequence: remove, look up the events when vetoed, optionally close the
 * mixer and pause the Windows Audio service, and remove again.  It never touches anything
 * outside the settings.
 *
 * @param s the settings this run works for
 * @param targets the devices to repair, NULL says the run looks for the faulted
 *        devices itself; an empty list is the same as no device at all
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audio_service_pause the user allowed pausing the Windows Audio service
 * @param progress may be NULL; it is called from the thread that runs the repair
 * @param progress_userdata handed back to progress
 * @param cancel may be NULL; a manual-reset event the caller owns.  The run waits on it
 *        while it waits for the veto record, and reads it between its steps: a signaled
 *        event ends the run with REPAIR_OUTCOME_ABORTED instead of walking into the next
 *        step, it never interrupts a step that is already going.
 * @param out receives what happened; release with repair_result_release
 * @param err receives the failure that stopped the run
 * @return false only on a hard failure.  The outcome carries what came of the devices: a
 *         failure the run cannot go on with stops it, and the reason is what err holds when
 *         the call returns false.
 */
NODISCARD bool repair_run(struct settings const *const s,
                          struct device_target *const targets,
                          bool const allow_mixer_close,
                          bool const allow_audio_service_pause,
                          repair_progress_fn const progress,
                          void *const progress_userdata,
                          HANDLE const cancel,
                          struct repair_result *const out,
                          struct ov_error *const err);

/**
 * @brief Release what a run collected
 *
 * @param r NULL is allowed
 */
void repair_result_release(struct repair_result *const r);
