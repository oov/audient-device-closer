#pragma once

#include <ovbase.h>

#include <wchar.h>

/**
 * The paths this program derives from its own location: the settings file sits next to the
 * executable, and so does the log of a portable copy.  An installed copy keeps its log in
 * the machine-wide application data folder.  These computations live here.
 *
 * A path is what the system is asked to open, not a text this program shows or writes, so
 * everything here is a wide string: the Win32 calls take one, and the conversions a UTF-8
 * form would need on the way in and on the way out are the only thing that form would add.
 * The text inside a file is another matter: that is UTF-8, and it stays there.
 */

/**
 * @brief Full path of the running executable
 *
 * @param err receives the failure of the lookup
 * @return the path, NULL when it could not be read
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *paths_executable(struct ov_error *const err);

/**
 * @brief The file name of a path, the part behind the last separator
 *
 * @param path the path to look at, NULL is allowed
 * @return the file name inside path, empty when the path ends with a separator, NULL for
 *         NULL
 * @note The returned string lives inside path, there is nothing to release.
 */
wchar_t const *paths_file_name(wchar_t const *const path);

/**
 * @brief Is the file name of a path the name the caller gives
 *
 * The name has to be a component of its own, so "foo.exe" never matches "foobar.exe", and
 * the case folds the way the system folds it.
 *
 * @param path the path to look at, NULL is not a match
 * @param name the file name to look for, NULL and the empty name are not a match
 * @return true when the last component of path is name
 */
bool paths_file_name_matches(wchar_t const *const path, wchar_t const *const name);

/**
 * @brief Are two paths the same folder
 *
 * The answer is the identity the file system gives the folder, not the spelling of the
 * path: the case of the letters, a trailing separator and a path built with a dot all name
 * the same folder, and a copy of it somewhere else does not.
 *
 * @param a the first path to look at, NULL is refused
 * @param b the second path to look at, NULL is refused
 * @param err receives the failure of the lookup
 * @return ov_true when both paths name the same folder, ov_false when they do not or when
 *         one of them is not there, ov_indeterminate when the answer could not be worked
 *         out, with err set
 */
NODISCARD ov_tribool paths_same_directory(wchar_t const *const a, wchar_t const *const b, struct ov_error *const err);

/**
 * @brief The path of a file with the reparse points on the way resolved
 *
 * A junction or a symbolic link makes a file reachable under a path that can be pointed
 * somewhere else later.  The task scheduler has to carry the path of the file itself, so
 * this is the path a caller writes into the entry.
 *
 * @param path the file to look at, NULL is refused
 * @param err receives the failure of the lookup
 * @return the path, NULL when it could not be read
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *paths_final_path(wchar_t const *const path, struct ov_error *const err);

/**
 * @brief The folder the log of an installed copy writes into
 *
 * The folder is the machine-wide application data folder plus AudientDeviceCloser.  Only the name
 * is built here, nothing on the disk is touched: see paths_create_folder for making the folder
 * exist, which the log does when it opens.
 *
 * It is the machine-wide folder rather than a person's own because a copy in Program Files is run
 * both by the person at the screen and by the task scheduler as the system account: the
 * application data of the system account is a folder nobody opens afterwards, and the answer of a
 * repair that happened while nobody was signed in is meant to be read.
 *
 * @param err receives the failure of the lookup
 * @return the folder without a trailing separator, NULL when it could not be built
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *paths_log_folder(struct ov_error *const err);

/**
 * @brief Make the folder of this name exist
 *
 * A folder that is already there is the answer the caller wanted, so this is not a failure.
 *
 * @param folder the folder to make, NULL is refused
 * @param err receives the failure of the create
 * @return false when the folder could not be made
 */
NODISCARD bool paths_create_folder(wchar_t const *const folder, struct ov_error *const err);

/**
 * @brief The path of a file that sits next to an executable under the same name
 *
 * The extension of the executable is replaced by extension, which carries its own dot:
 * C:\dir\audient-device-closer.exe with ".log" gives C:\dir\audient-device-closer.log, and
 * with ".json" it gives C:\dir\audient-device-closer.json.  A dot behind the last separator
 * is the extension, a dot inside a directory name is not, and a name without an extension
 * only gets the extension appended.
 *
 * @param exe_path the executable the file belongs to
 * @param extension the extension the file carries, with its dot
 * @param err receives the failure of the build
 * @return the path, NULL when it could not be built
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *paths_replace_extension(wchar_t const *const exe_path, wchar_t const *const extension, struct ov_error *const err);

/**
 * @brief Join a directory and a file name with one backslash
 *
 * @param directory the directory, without a trailing separator
 * @param name the file name
 * @param err receives a failure of the join
 * @return the path, NULL when it could not be built
 * @note Release with OV_ARRAY_DESTROY.
 */
NODISCARD wchar_t *paths_join(wchar_t const *const directory, wchar_t const *const name, struct ov_error *const err);
