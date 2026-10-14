#pragma once

#include <ovbase.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device.h"
#include "removal.h"

struct settings;
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
 * @brief What the caller should tell the user
 *
 * The outcome of the run and, when the system refused the removal, why.  The sentences
 * themselves are chosen by the UI, next to the dialog that shows them; the numbers stay in
 * the log.  The reason of a refusal is carried as the code of the removal error, 0 when
 * the outcome is not a refusal of the removal.
 *
 * The events are the records of the veto, shown as they were written; the processes this
 * run closed are not among them.
 */
struct repair_result {
  enum repair_outcome outcome;
  enum removal_error_code removal_error; // 0 unless the removal was refused
  struct veto_event *events;             // ovarray or NULL.  released by repair_result_release
  char message[512];                     // UTF-8, meant for the UI log
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
 * @brief The words of the setting that lets this tool pause the audio service
 *
 * @return the caption, UTF-8
 */
char const *repair_option_audiodg_label(void);

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
 * @brief How long the confirmation waits between two looks at a released device
 *
 * The release puts the device back on the bus, and the bus takes a moment to bring it up
 * again.  The confirmation looks once a second, for as long as the settings let it wait:
 * a device that is healthy within that window is found by it, and one that never comes
 * back is reported as a failure the reason of which is not known.
 */
#define REPAIR_CONFIRM_POLL_INTERVAL_MS 1000

/**
 * @brief The audio engine, a blocker as often as the mixer is
 *
 * Its image name is the same everywhere, so it is not part of the settings.
 */
#define REPAIR_AUDIODG_NAME "audiodg.exe"

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
  REPAIR_STAGE_AUDIODG,     // audiodg is being closed and waited for
  REPAIR_STAGE_VETO_RECORD, // the veto record is being waited for and read
  REPAIR_STAGE_CONFIRM      // the released device is being asked whether it is an audio device again
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
 * @brief Run the repair
 *
 * The documented sequence: remove, look up the events when vetoed, optionally close the
 * blockers (the mixer and audiodg) and remove again.  It never touches anything outside the
 * settings.
 *
 * @param s the settings this run works for
 * @param targets the devices to repair, NULL says the run looks for the faulted
 *        devices itself; an empty list is the same as no device at all
 * @param allow_mixer_close the user allowed closing the mixer
 * @param allow_audiodg_close the user allowed closing the audio engine
 * @param progress may be NULL; it is called from the thread that runs the repair
 * @param progress_userdata handed back to progress
 * @param cancel may be NULL; a flag the caller owns.  The run reads it between its steps
 *        and ends with REPAIR_OUTCOME_ABORTED instead of walking into the next one, it
 *        never interrupts a step that is already going.
 * @param out receives what happened; release with repair_result_release
 * @param err receives the failure that stopped the run
 * @return false only on a hard failure.  The outcome carries the news: a step whose failure
 *         is folded into the outcome releases its error into the log before the next step
 *         starts, so err is only worth reading when the call returns false.
 */
bool repair_run(struct settings const *const s,
                struct device_target *const targets,
                bool const allow_mixer_close,
                bool const allow_audiodg_close,
                repair_progress_fn const progress,
                void *const progress_userdata,
                bool *const cancel,
                struct repair_result *const out,
                struct ov_error *const err);

/**
 * @brief Release what a run collected
 *
 * @param r NULL is allowed
 */
void repair_result_release(struct repair_result *const r);
