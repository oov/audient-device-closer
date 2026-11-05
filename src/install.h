#pragma once

#include <ovbase.h>

/**
 * The record the installer leaves on the machine: its uninstall entry, which names the
 * folder it installed into.  The folder is the one thing a copy cannot forge by moving
 * itself, because the entry is written by the installer and names where it put the files.
 *
 * The answer is asked once, before the log is opened, and kept here: the log and the window
 * both need it, and neither has to carry it around.  Nothing changes it after that.
 */

/**
 * @brief Work out whether this copy is the one the installer put on the machine
 *
 * The uninstall entry of the installer is read and its install location is compared with
 * the folder of the running executable.  The comparison is of the folder identity, not of
 * the text of the path, so a portable copy and a copy of an installed folder elsewhere are
 * both told apart from the installed copy.
 *
 * A machine without the entry, or an entry that names another folder, leaves the answer as
 * the portable one.  A failure of the lookup leaves it portable as well and is reported to
 * the caller.
 *
 * Call it once, before any thread that asks install_is_installed() starts.
 *
 * @param err receives the failure of the lookup
 * @return false when the answer could not be worked out; the answer is then portable
 */
NODISCARD bool install_init(struct ov_error *const err);

/**
 * @brief Is this copy the one the installer put on the machine
 *
 * The answer install_init worked out.  A program that never called install_init answers the
 * portable one, which is the safe answer.
 *
 * @return true when the running copy sits in the folder the installer recorded
 */
NODISCARD bool install_is_installed(void);
