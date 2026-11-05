#pragma once

#include <ovbase.h>

struct settings;

/**
 * The scheduled task of this program: it runs the program itself at the end of a resume from
 * sleep, with the highest privileges the account has.  It is registered under the task name
 * of the settings and replaces whatever sat there before, so asking for it twice leaves one
 * entry and not two.
 */

/**
 * @brief The folder and the name the task of this program is registered under
 *
 * The two are separate values in the settings file, so neither is looked through for a
 * separator.  Both are needed to register or to remove the entry, and a settings file that
 * leaves one of them out is an answer this program cannot work with.
 *
 * @param settings the settings that name the task
 * @param out_dir receives the folder, borrowed from the settings
 * @param out_name receives the name, borrowed from the settings
 * @param err receives the refusal of a settings file that does not name one task
 * @return false when the settings do not carry a folder and a name to use
 */
NODISCARD bool
task_target(struct settings const *settings, wchar_t const **const out_dir, wchar_t const **const out_name, struct ov_error *const err);

/**
 * @brief Register the task of this program
 *
 * Registers the task, or overwrites the one that is already registered.  Fails only when the
 * system refuses the registration, or when this copy is not the one the installer put on the
 * machine.  The second refusal is app_error_task_not_installed.
 *
 * @param settings the settings that name the task
 * @param close_mixer the box of the caller that allows closing the mixer
 * @param allow_audio_service_pause the box of the caller that allows pausing the Windows
 *        Audio service
 * @param err receives the refusal of the system
 * @return false when the registration did not happen
 */
NODISCARD bool task_install(struct settings const *settings, bool close_mixer, bool allow_audio_service_pause, struct ov_error *const err);

/**
 * @brief Remove the task of this program
 *
 * A task that is not there is not a failure: the answer the caller asked for is "no entry",
 * and that is what holds afterwards either way.
 *
 * @param settings the settings that name the task
 * @param err receives the failure.  HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) says that there
 *            was no entry to remove; HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY) says that the
 *            entry went and the folder stayed
 * @return false when the entry could not be removed
 */
NODISCARD bool task_uninstall(struct settings const *settings, struct ov_error *const err);
