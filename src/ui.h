#pragma once

#include <ovbase.h>

#include <stdbool.h>

#include <windows.h>

#include "culprit.h"
#include "device.h"
#include "repair.h"

struct options;
struct settings;

/**
 * @brief Open the window and run until it closes
 *
 * @param options what this run was asked to do, it must outlive the call
 * @param settings the settings this run works for, they must outlive the call
 * @param err receives the failure of the start-up
 * @return false when the window could not be made
 */
bool ui_run(struct options const *const options, struct settings const *const settings, struct ov_error *const err);

/**
 * @brief The headline of the result dialog
 *
 * It says what the run did; the name of the program is the title bar already, so it is not
 * repeated here.
 *
 * @param outcome what came of the run
 * @return the translated sentence, UTF-8
 */
char const *ui_result_headline(enum repair_outcome outcome);

/**
 * @brief Does the window close itself when the run is done
 *
 * Only a run nobody is watching may do that -- a scheduled run has nobody to read the
 * result, while a run a person started has to keep it in front of them.  A run that did
 * not release the device keeps its window in both cases: the sentence of the failure is
 * the reason the reader is there at all.
 *
 * @param auto_run the scheduler started this run
 * @param outcome what came of the run
 * @return true when there is nothing left for a person to do here
 */
bool ui_closes_itself_when_done(bool auto_run, enum repair_outcome outcome);

/**
 * @brief Does the finished run put its result in front of the reader in a dialog
 *
 * The run nobody watches has what it wanted once the device was released: it says nothing
 * and takes the result with it, and a dialog nobody reads would hold the window open after
 * the run is done.  Everything else has somebody waiting for the sentence -- a run a
 * person started and every failure, whatever the run was.  This is the other side of
 * ui_closes_itself_when_done: what closes its own window says nothing.
 *
 * @param auto_run the scheduler started this run
 * @param outcome what came of the run
 * @return true when the window stays open and the outcome is worth a dialog
 */
bool ui_shows_result_dialog(bool auto_run, enum repair_outcome outcome);

/**
 * @brief What the window does by itself after the first look at the device
 *
 * The task starts this program at every resume from sleep and nobody stands in front of
 * the window then: the state this tool is for is acted on at once, and every other state
 * is a run with nothing to do, so the window goes away by itself instead of waiting for a
 * person who is not there.  A run a person started waits for that person, whatever the
 * state is.
 */
enum ui_auto_run {
  UI_AUTO_RUN_WAIT,   //!< nothing happens by itself
  UI_AUTO_RUN_REPAIR, //!< the faulty state: close the device at once
  UI_AUTO_RUN_LEAVE,  //!< nothing to fix here: the window closes itself
};

/**
 * @brief What the run of the scheduler does with the state it finds
 *
 * @param auto_run the scheduler started this run
 * @param state the state of the device
 * @return what the run should do next
 */
enum ui_auto_run ui_auto_run_action(bool auto_run, enum device_state state);

/**
 * @brief What the run of the scheduler says about the state it found
 *
 * A run that found nothing to do leaves one sentence in the log, and the sentence is the
 * state it looked at.  The faulty state has none here: the run answers it by working, and
 * the work says what came of it.
 *
 * @param state the state of the device
 * @return the sentence, UTF-8, NULL when the run has work to do
 */
char const *ui_auto_run_sentence(enum device_state state);

/**
 * @brief Does a press of the close button have to ask before the run starts
 *
 * A device that is not in the faulty state does not need the close at all, and the close
 * of one that works is likely to be refused, so the reader is asked first.  The run nobody
 * is watching does not come through the button, so it is never asked.
 *
 * @param faulted the device is in the state this tool is for
 * @return true when the person should be asked first
 */
bool ui_asks_before_close(bool faulted);

/**
 * @brief Does a press of the button find a device the tool cannot handle
 *
 * A device with a problem this tool cannot cure, a device that is not there and a state
 * nobody can decide from have no run to walk through: the press is answered with a dialog
 * instead of a run.
 *
 * @param state the state the last refresh saw
 * @return true when the press is answered with a dialog instead of a run
 */
bool ui_state_is_not_repairable(enum device_state state);

/**
 * @brief The two lines of that question
 *
 * The headline names the state that was found, the body names the risk and asks.  Buffers
 * the caller owns, both UTF-8.
 *
 * @param headline receives the question, UTF-8
 * @param headline_size room of headline
 * @param body receives what the answer is about, UTF-8
 * @param body_size room of body
 */
void ui_confirm_dialog_text(char *headline, size_t headline_size, char *body, size_t body_size);

/**
 * @brief Both lines of the result dialog at once
 *
 * headline and body are UTF-8 buffers the caller owns.  This is what the dialog is handed,
 * so a test can read the whole dialog without a window.
 *
 * The dialog is made of translated sentences only: the verdict as the headline, the reason
 * the program worked out as the body.  What a run leaves in repair_result.message is the
 * sentence for the log and has no way into the dialog.
 *
 * @param outcome what came of the run
 * @param reason why the removal was refused
 * @param headline receives the verdict, UTF-8
 * @param headline_size room of headline
 * @param body receives what happened, UTF-8
 * @param body_size room of body
 */
void ui_result_dialog_text(
    enum repair_outcome outcome, enum removal_reason reason, char *headline, size_t headline_size, char *body, size_t body_size);
