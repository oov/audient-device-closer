#pragma once

#include <ovbase.h>

#include <windows.h>

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
 * @brief Make the window the run reports into and show it
 *
 * The owner is disabled while the window is up and enabled again when the window is
 * destroyed.  The range of the bar is the number of steps the caller names.  The window
 * draws with a font of its own, made for the dpi it is given.
 *
 * @param owner the window the run is started from, disabled while the window is up
 * @param dpi the dpi the geometry and the font are scaled to
 * @param steps the number of steps of the run, the range of the bar
 * @param cancel the ask to stop the run; the window writes it on the press of the button
 *        or the close box, the run reads it between its steps.  Must outlive the window.
 * @param first_text the text the label starts with, UTF-8, may be NULL
 * @param widest_text the stage text of the run that needs the most room, UTF-8, may be NULL
 *        for first_text: the window is made wide enough to show that line whole, so no report
 *        of the run is ever cut off
 * @param out receives the window; hand it back to ui_progress_window_destroy()
 * @param err receives the failure of the start-up
 * @return false when the window could not be made
 */
bool ui_progress_window_create(HWND const owner,
                               int const dpi,
                               size_t const steps,
                               bool *const cancel,
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
 */
void ui_progress_window_destroy(HWND *const progress);

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
 */
void ui_progress_window_report(HWND const progress, size_t const step, char const *const text, size_t const steps);

/**
 * @brief How many steps the window counts
 *
 * The steps of the repair plus the wait of a scheduled run.
 *
 * @param wait_first the run waits for the devices before it looks at them
 * @return the number of steps to show
 */
size_t ui_progress_window_steps(bool const wait_first);
