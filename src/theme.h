#ifndef ADC_THEME_H
#define ADC_THEME_H

#include <windows.h>

#include <ovbase.h>

/**
 * Visual theme of the application.
 *
 * Everything that has to do with the light and the dark palette lives behind this
 * interface: the undocumented uxtheme entry points, the per control workarounds, the
 * brushes and the subclassed painting.  A window procedure only has to
 *
 *   - call theme_init() once, before it creates its first window, with the
 *     switch that says whether the theme is wanted at all,
 *   - call theme_attach() after it created a window,
 *   - pass the messages listed in theme_message() to the theme, and
 *   - call theme_release() when the window is gone.
 *
 * Nothing else in the application reads a colour, holds a brush or names a theme class.
 */

/**
 * @brief Resolve the theme entry points and switch the process to the palette of the system
 *
 * Must be called before the first window of the process is created.  A system without the
 * entry points is not an error, the application then stays light.
 *
 * @param enabled FALSE detaches the whole theme: no entry point is resolved, no
 *                window is themed, and every message stays with the system.  It
 *                is the answer to a Windows that the undocumented entry points
 *                do not work on, now or in the future.
 * @param err reserved for a future failure
 * @return always true, the signature exists so that a future failure can be reported
 */
NODISCARD bool theme_init(bool const enabled, struct ov_error *const err);

/**
 * @brief Delete the resources that theme_init() and theme_attach() created
 *
 * The theme is dropped whether or not the objects could be deleted.  A delete that fails
 * is reported, so the caller knows the objects were left behind.
 *
 * @param err receives the failure of a delete, NULL is allowed
 * @return false when an object could not be deleted
 */
NODISCARD bool theme_release(struct ov_error *const err);

/**
 * @brief Attach the theme to one window
 *
 * Depending on its window class and button style the window gets a theme class, a subclass
 * that paints it, or nothing at all.  Call it right after CreateWindowExW().  Attaching a
 * list view before its columns exist leaves the header unthemed, use theme_attach_list()
 * for that.
 *
 * @param hwnd the window to attach to
 * @param err receives the failure of the attach
 * @return false when the theme could not be put on the window
 */
NODISCARD bool theme_attach(HWND const hwnd, struct ov_error *const err);

/**
 * @brief Attach the theme to a list view and to its header
 *
 * Call it after the columns were inserted, otherwise the header window does not exist yet.
 *
 * @param list the list view to attach to
 * @param err receives the failure of the attach
 * @return false when the theme could not be put on the list or its header
 */
NODISCARD bool theme_attach_list(HWND const list, struct ov_error *const err);

/**
 * @brief Give the window procedure of the application a chance to answer theme messages
 *
 * Handles WM_CTLCOLORSTATIC, WM_CTLCOLOREDIT, WM_CTLCOLORBTN, WM_ERASEBKGND, WM_NOTIFY
 * with NM_CUSTOMDRAW and WM_DESTROY.  Painting a group frame stays with the caller, it is
 * the only one that knows where the frame goes, use theme_frame() for it.
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @param result receives the value to return from the window procedure
 * @param err receives the failure of a message that the theme has to answer
 * @return ov_true when the message was consumed and result holds what the window procedure
 *         has to answer, ov_false when it was not consumed, ov_indeterminate when the
 *         message could not be answered, with err set
 */
NODISCARD ov_tribool
theme_message(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, LRESULT *const result, struct ov_error *const err);

/**
 * @brief Make the brush to give to WNDCLASSEXW::hbrBackground
 *
 * The system deletes the background brush of a class when the class is unregistered, so
 * every class needs a brush of its own: the one the palette paints with is the theme's to
 * keep.  The brush has the colour the palette paints the surface with, so a class and the
 * messages of the theme agree.
 *
 * @param err receives the failure of the brush
 * @return the brush, NULL when it could not be made
 */
HBRUSH theme_class_brush(struct ov_error *err);

/**
 * @brief Draw the frame of a group of controls
 *
 * BS_GROUPBOX paints its caption with the colour of the theme definition, which cannot be
 * reached from outside, so the caption is a plain static control and the frame is drawn
 * here.
 *
 * @param dc the context to draw in
 * @param rect the frame rectangle in the client coordinates of the window
 * @param caption the rectangle of the caption, in the same coordinates.  The top line of
 *                the frame is broken around it, the way a group box does.  NULL draws a
 *                closed frame.
 * @param dpi the dpi of the window, it scales the line and the break
 * @param err receives the failure of the drawing
 * @return false when the frame could not be drawn
 */
NODISCARD bool theme_frame(HDC const dc, RECT const *const rect, RECT const *const caption, int const dpi, struct ov_error *const err);

#endif /* ADC_THEME_H */
