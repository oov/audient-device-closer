#pragma once

#include <ovbase.h>

#include <windows.h>

struct device_entry;
struct device_target;
struct settings;
struct ov_error;

/**
 * @brief The window that asks which device to repair
 *
 * None of the connected devices is in the state this tool cures, so the
 * press of the button has nothing to do on its own.  The reader may still
 * want the release, so the window names the connected devices and waits for
 * the one to repair.  The window is self-contained -- its geometry, its
 * class and its theme live here -- so a test can make it against any window
 * of its own and read the combo.
 */

/**
 * @brief Ask which device to repair and wait for the answer
 *
 * The connected devices are listed, the reader picks one and answers with
 * the button that repairs it.  The button that answers nothing takes the
 * window down without a word.  The owner is held while the window is up,
 * the way a dialog holds it.
 *
 * @param owner the window that asks, held while the reader answers
 * @param s the settings that name the devices this program looks at
 * @param targets the units that are connected, the reader picks one
 * @param out_target receives the device the reader picked, a list of one
 *        the caller releases with device_targets_release, NULL when the
 *        reader answered nothing
 * @param err receives the failure of the start-up
 * @return false when the window could not be made
 */
bool ui_device_picker_show(HWND const owner,
                           struct settings const *const s,
                           struct device_target const *const targets,
                           struct device_target **const out_target,
                           struct ov_error *const err);
