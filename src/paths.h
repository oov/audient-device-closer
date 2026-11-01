#pragma once

#include <ovbase.h>

#include <wchar.h>

/**
 * The paths this program derives from its own location: the log has to sit next to the
 * executable, and so does the settings file.  Both computations live here.
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
 * @brief Is the executable installed under the Program Files folder of this machine
 *
 * A copy that sits in Program Files belongs to the machine rather than to the person who
 * unpacked it, and the run that repairs the device is often the one the task scheduler
 * started, not the one at the screen: the log of such a copy therefore goes to
 * paths_log_folder instead of next to the executable.
 *
 * The folder is the one the system names, not one the path spells: a machine may put its
 * programs elsewhere, and a directory that merely carries the name says nothing about
 * where the file sits.  The two paths are compared through the system, so the cases of
 * the letters, the separators and the "." and ".." inside them fold away as the system
 * folds them, which is the whole reason the answer is asked for rather than worked out.
 *
 * @param exe_path the path to look at, NULL is refused
 * @param err receives the failure of the lookup or the refused argument
 * @return ov_true when exe_path names a file under the folder of the system, ov_false when
 *         it does not, ov_indeterminate when the folder could not be read, with err set
 */
NODISCARD ov_tribool paths_is_in_program_files(wchar_t const *const exe_path, struct ov_error *const err);

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
