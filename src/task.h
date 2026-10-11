#pragma once

#include <ovbase.h>

#include <stdbool.h>

struct settings;

/**
 * The scheduled task of this program: it runs the program itself at the end of a resume from
 * sleep, with the highest privileges the account has.  It is registered under the task name
 * of the settings and replaces whatever sat there before, so asking for it twice leaves one
 * entry and not two.
 */

/**
 * @brief Register the task of this program
 *
 * Registers the task, or overwrites the one that is already registered.  Fails only when the
 * system refuses the registration.
 *
 * @param settings the settings that name the task
 * @param close_mixer the box of the caller that allows closing the mixer
 * @param close_audiodg the box of the caller that allows closing the audio engine
 * @param err receives the refusal of the system
 * @return false when the registration did not happen
 */
bool task_install(struct settings const *settings, bool close_mixer, bool close_audiodg, struct ov_error *const err);

/**
 * @brief Remove the task of this program
 *
 * A task that is not there is not a failure: the answer the caller asked for is "no entry",
 * and that is what holds afterwards either way.
 *
 * @param settings the settings that name the task
 * @param err receives a failure that leaves the entry behind
 * @return false when the task could not be removed
 */
bool task_uninstall(struct settings const *settings, struct ov_error *const err);
