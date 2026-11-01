#pragma once

#include <ovbase.h>

#include <windows.h>

#include "repair.h"

/**
 * @brief The window the run reports into
 *
 * The stage label, the progress bar under it and the button that asks the run to stop,
 * and nothing else.  The window is made for one run and the caller destroys it when the
 * run is over; while it is up it holds its owner the way a dialog does.
 *
 * The window reads no state of the caller beyond the values it is made with, so a test
 * can make it against any window of its own.
 */

/**
 * @brief What the window of the run does when the reader asks it to stop
 *
 * Called on the thread of the window, once the event the run reads has been signaled and
 * the button that asks has gone grey.  What it does with the ask is the caller's: a run
 * that is taken forward by a message puts one up, and a caller with nothing to do sends
 * none.
 *
 * @param userdata handed back to the call
 */
typedef void (*progress_cancel_fn)(void *userdata);

/**
 * @brief Make the window the run reports into and show it
 *
 * The owner is disabled while the window is up and enabled again when the window is
 * destroyed.  The range of the bar is the number of steps the caller names.  The window
 * draws with a font of its own, made for the dpi it is given.
 *
 * @param owner the window the run is started from, disabled while the window is up
 * @param dpi the dpi the geometry and the font are scaled to
 * @param steps the number of steps of the run, the range of the bar
 * @param cancel the ask to stop the run; the window signals it on the press of the button
 *        or the close box, the run reads it between its steps.  Must be a manual-reset
 *        event and must outlive the window.
 * @param on_cancel called on the thread of the window when the reader asks to stop it, may
 *        be NULL; the button that asks and the close box are greyed when it is, so a window
 *        that can tell nobody about the ask does not offer it
 * @param on_cancel_userdata handed to on_cancel
 * @param first_text the text the label starts with, UTF-8, may be NULL
 * @param widest_text the stage text of the run that needs the most room, UTF-8, may be NULL
 *        for first_text: the window is made wide enough to show that line whole, so no report
 *        of the run is ever cut off
 * @param out receives the window; hand it back to ui_progress_window_destroy()
 * @param err receives the failure of the start-up
 * @return false when the window could not be made
 */
NODISCARD bool ui_progress_window_create(HWND const owner,
                                         int const dpi,
                                         size_t const steps,
                                         HANDLE const cancel,
                                         progress_cancel_fn on_cancel,
                                         void *on_cancel_userdata,
                                         char const *const first_text,
                                         char const *const widest_text,
                                         HWND *const out,
                                         struct ov_error *const err);

/**
 * @brief Take the progress window down and give the owner back
 *
 * The window is destroyed and the handle is set to NULL, so the call is safe to repeat.
 * The owner the window held is enabled again.
 *
 * @param progress the window made by ui_progress_window_create, may be NULL
 * @param err receives the failure of the teardown, NULL is allowed
 * @return false when the window could not be taken down
 */
NODISCARD bool ui_progress_window_destroy(HWND *const progress, struct ov_error *const err);

/**
 * @brief Say what the run is waiting for
 *
 * The label shows the text with the step in it ("Closing the device... (2/6)") and the
 * bar moves to the step.  Nothing happens when there is no window.
 *
 * @param progress the window made by ui_progress_window_create
 * @param step the step the run is at, counted from one
 * @param text the text of the stage, UTF-8
 * @param steps the number of steps of the run, the count the label shows
 * @param err receives the failure of the report
 * @return false when the label or the bar could not be told
 */
NODISCARD bool ui_progress_window_report(HWND progress, size_t step, char const *text, size_t steps, struct ov_error *const err);

/**
 * @brief How many steps the window counts
 *
 * The steps of the repair plus the wait of a scheduled run.
 *
 * @param wait_first the run waits for the devices before it looks at them
 * @return the number of steps to show
 */
size_t ui_progress_window_steps(bool const wait_first, size_t const targets);

/**
 * @brief The line the window shows for one stage of a run
 *
 * A run reports a stage, and the window puts the stage into words: the line here is what
 * the label shows, with the step and the count added by ui_progress_window_report.  The
 * mapping lives here rather than with the repair because gettext() has to see the literal
 * at the call site, which is what src/i18n/start.bash collects.
 *
 * @param stage the stage of the run to name
 * @param s the settings that name the devices, may be NULL for the defaults
 * @param out receives the line, UTF-8, always terminated
 * @param out_size room of out, nothing is written when it is 0
 */
void ui_progress_window_line(enum repair_stage stage, struct settings const *s, char *out, size_t out_size);

/**
 * @brief The line the window shows when a stage has no text of its own
 *
 * @return the translated line, UTF-8
 */
char const *ui_progress_window_default_line(void);
