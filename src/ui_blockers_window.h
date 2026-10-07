#pragma once

#include <ovbase.h>

#include <windows.h>

#include "culprit.h"

/**
 * @brief The window that names the processes that hold the device
 *
 * A vetoed run does not answer with a sentence but with a list: the run opens this window
 * over its owner, the reader reads the processes there and presses OK (or the close box)
 * to come back.  The window owns no state beyond its own controls: the dpi and the font
 * come from the owner at the moment it is made, the way every dialog of the application
 * takes them from the window it sits over.
 */

/**
 * @brief Show the window that names the processes that block the close
 *
 * The list of the veto records and the button that takes the window down.  The owner is
 * disabled while the window is up and enabled again when the window goes away, the way a
 * dialog holds its owner.  The window is resizable, so the reader can make the paths
 * readable.  The window destroys itself; there is nothing to hold on to.
 *
 * When the owner has no font to lend (WM_GETFONT answers nothing) the window makes one of
 * its own and deletes it when it goes.
 *
 * @param owner the window that waits while the list is read
 * @param culprits the records to show, may be NULL
 * @param err receives the failure of the start-up
 * @return false when the window could not be made
 */
bool ui_blockers_window_show(HWND const owner, struct culprit const *const culprits, struct ov_error *const err);
