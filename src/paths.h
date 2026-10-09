#pragma once

#include <ovbase.h>

/**
 * The paths this program derives from its own location: the log has to sit next to the
 * executable, and so does the settings file.  Both computations live here.  The log of an
 * installed copy is the exception: Program Files is not writable, so the log moves to the
 * local application data of the user.
 */

/**
 * @brief The longest path this program handles, in wide characters
 *
 * The path of the executable is read into a stack buffer of this size and every other path
 * is derived from it, so a path that does not fit is refused instead of being cut into
 * another path.
 */
#define PATHS_MAX 1024

/**
 * @brief Full path of the running executable
 *
 * @param err receives the failure of the lookup
 * @return the path in UTF-8, NULL when it could not be read
 * @note Release with paths_string_free.
 */
char *paths_executable(struct ov_error *const err);

/**
 * @brief Directory of the running executable
 *
 * @param err receives the failure of the lookup
 * @return the directory in UTF-8 without a trailing separator, NULL when it could not be read
 * @note Release with paths_string_free.
 */
char *paths_executable_dir(struct ov_error *const err);

/**
 * @brief The file name of a path, the part behind the last separator
 *
 * @param path_utf8 the path to look at, UTF-8, NULL is allowed
 * @return the file name inside path_utf8, empty when the path ends with a separator, NULL for
 *         NULL
 * @note The returned string lives inside path_utf8, there is nothing to release.
 */
char const *paths_file_name(char const *const path_utf8);

/**
 * @brief Is the executable installed under one of the Program Files folders
 *
 * A copy in Program Files shares the folder with the system and every other program, so a
 * user process cannot write its log there.  The check is case-insensitive and looks for
 * the folder name behind a separator, so "C:\\Program Files\\..." and
 * "C:\\Program Files (x86)\\..." both match whatever drive they sit on.  A folder of the
 * same name somewhere else in the path matches as well, which is fine: a folder that
 * calls itself Program Files asks for the same treatment.
 *
 * @param exe_path the path to look at, UTF-8, NULL is allowed
 * @return true when exe_path names a file inside a Program Files folder
 */
bool paths_is_in_program_files(char const *const exe_path);

/**
 * @brief The folder the log of an installed copy writes into
 *
 * The folder is the local application data folder of the user plus audient-device-closer,
 * and it is created when it is not there, so the caller can put the log into it right
 * away.
 *
 * @param err receives the failure of the lookup or of the creation
 * @return the folder in UTF-8 without a trailing separator, NULL when it could not be built
 * @note Release with paths_string_free.
 */
char *paths_log_folder(struct ov_error *const err);

/**
 * @brief The path of a file that sits next to an executable under the same name
 *
 * The extension of the executable is replaced by extension, which carries its own dot:
 * C:\dir\audient-device-closer.exe with ".log" gives C:\dir\audient-device-closer.log, and
 * with ".json" it gives C:\dir\audient-device-closer.json.  A dot behind the last separator
 * is the extension, a dot inside a directory name is not, and a name without an extension
 * only gets the extension appended.
 *
 * @param exe_path the executable the file belongs to, UTF-8
 * @param extension the extension the file carries, with its dot
 * @param err receives the failure of the build
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with OV_FREE.
 */
char *paths_replace_extension(char const *const exe_path, char const *const extension, struct ov_error *const err);

/**
 * @brief Join a directory and a file name with one backslash
 *
 * @param directory the directory, without a trailing separator
 * @param name the file name
 * @param err receives a failure of the join
 * @return the path in UTF-8, NULL when it could not be built
 * @note Release with paths_string_free.
 */
char *paths_join(char const *const directory, char const *const name, struct ov_error *const err);

/**
 * @brief Release a string this module handed out
 *
 * @param value set to NULL afterwards, NULL is allowed
 */
void paths_string_free(char **const value);
