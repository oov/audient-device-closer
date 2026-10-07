#pragma once

#include <windows.h>

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
 * @brief The switches the task runs with, as the arguments field of the action
 *
 * -auto-run is always in it: a run that does not know it was started by the scheduler would
 * keep its window and wait for a person who is not there.  The two close flags follow the
 * boxes of the caller.  The program is not named here: the command field names it and the
 * scheduler runs it with these arguments in front of it.
 *
 * @param close_mixer the box of the caller that allows closing the mixer
 * @param close_audiodg the box of the caller that allows closing the audio engine
 * @param err receives a failure of the build
 * @return the arguments in UTF-8, NULL when they could not be built
 * @note Release with task_string_free.
 */
char *task_build_arguments(bool close_mixer, bool close_audiodg, struct ov_error *const err);

/**
 * @brief The document the task is registered from
 *
 * The trigger is the resume from sleep, the principal asks for the highest privileges of the
 * account, and the settings are the ones this program needs: one instance at a time, no
 * demand for a battery or a network.  Command, arguments and description are escaped here,
 * and the command is written with quotes around it so a path with a space in it stays one
 * argument of the run.  A description of NULL registers the default one of this program,
 * which says what the task is for and when it runs.
 *
 * @param command the program to run
 * @param arguments the switches of the action
 * @param description the description the properties show, NULL takes the default of this program
 * @param err receives a failure of the build
 * @return the document in UTF-8, NULL when it could not be built
 * @note Release with task_string_free.
 */
char *task_build_xml(char const *command, char const *arguments, char const *description, struct ov_error *const err);

/**
 * @brief Does the answer of the system mean that the task is gone
 *
 * Removing a task that is not there is that state, so the caller is done even though
 * the call failed.
 *
 * @param result the HRESULT of the removal call
 * @return true when the task is not there any more
 */
bool task_removal_is_done(long const result);

/**
 * @brief Split a task path into the folder of the library and the name inside it
 *
 * A path without a separator is a task in the root of the library, and the folder of it
 * is the root itself.  Both buffers are the caller's.
 *
 * @param path the task path
 * @param folder receives the folder of the library
 * @param folder_size room of folder
 * @param leaf receives the name inside the folder
 * @param leaf_size room of leaf
 * @return false when the path is empty, too long for a buffer, or ends in a separator
 */
bool task_split_path(char const *path, char *folder, size_t folder_size, char *leaf, size_t leaf_size);

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
 * @brief Register a document of the caller under the task name of the settings
 *
 * The same call with the document handed in instead of built here.  The two steps are split
 * so that the document and the registration can be looked at on their own; the window uses
 * task_install above, the check on the command line hands in a document of its own.
 *
 * @param settings the settings that name the task
 * @param xml the document to register
 * @param err receives the refusal of the system
 * @return false when the registration did not happen
 */
bool task_install_document(struct settings const *settings, char const *xml, struct ov_error *const err);

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

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void task_string_free(char **const value);
