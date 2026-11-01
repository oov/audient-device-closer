#pragma once

#include <windows.h>

#include <commctrl.h>

#include <ovbase.h>

/**
 * @brief Keep what a test puts on the screen off the desktop of the reader
 *
 * A test that runs against real windows is the only way to check what a window draws and
 * what the window manager does with it, so the tests of this program make real windows.  A
 * window of a test must not reach the reader all the same: one that is drawn is in the way
 * of what the reader is doing, one that takes a press takes the press that was meant for
 * something else, and one that asks for the focus takes the reader away from what they are
 * typing.
 *
 * Two things are kept away:
 *
 * - The windows of the test are layered with no opacity, they let the mouse through to
 *   whatever is under them, and they never become the active window.  A control of the
 *   system makes windows of its own, the drop down list of a combo box among them, and
 *   those are covered too.
 * - The task dialogs and the message boxes of the program are answered without being
 *   shown.  They are modal, so one that is shown waits for a reader who is not there.
 *
 * The hooks are for the calling thread, so a thread that makes a window of a test has to
 * call test_suppress_install() on that thread.
 */

/**
 * @brief Put the hooks in place and answer the dialogs of the program from now on
 *
 * @param err receives the refusal of the system
 * @return false when the hooks could not be put in place; the dialogs are answered either way
 */
bool test_suppress_install(struct ov_error *const err);

/**
 * @brief Put the hooks in place, reporting a refusal where the test reads it
 *
 * A test that cannot keep its windows out of the reader's way goes on anyway, with its
 * windows in the way.
 */
void test_suppress_install_quiet(void);

/**
 * @brief Take the hooks off again
 *
 * Safe to call without test_suppress_install() having worked.
 */
void test_suppress_release(void);

/**
 * @brief The opacity a layered window was given
 *
 * @param hwnd the window to ask
 * @param out receives the opacity, 0 for a window that shows nothing and 255 for one that
 *            shows everything
 * @param err receives the refusal of the system
 * @return false when the window is not layered or the system refused the question
 */
bool test_suppress_opacity(HWND hwnd, BYTE *out, struct ov_error *const err);

/**
 * @brief Answer a task dialog without showing it
 *
 * Read this header after windows.h and before the source under test, so that the code under
 * test asks this instead of the system.  The text of the dialog is still built by the code
 * under test, which is what the tests check.
 *
 * @return S_OK, the answer of a dialog the reader closed with the default button
 */
HRESULT test_suppress_task(TASKDIALOGCONFIG const *config, int *button, int *radio, BOOL *verification);

/**
 * @brief Answer a message box without showing it
 *
 * @return IDOK, the answer of a reader who pressed the only button of a box with MB_OK
 */
int test_suppress_message(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);

#define TaskDialogIndirect test_suppress_task
#define MessageBoxW test_suppress_message
