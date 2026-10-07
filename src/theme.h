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
bool theme_init(bool const enabled, struct ov_error *const err);

/**
 * @brief Delete the resources that theme_init() and theme_attach() created
 */
void theme_release(void);

/**
 * @brief Is the application running with the dark palette
 *
 * @return TRUE when the palette is the dark one
 */
bool theme_dark(void);

/**
 * @brief Re-read the palette from the system
 *
 * Call it when the user changed the colour setting (WM_SETTINGCHANGE with
 * "ImmersiveColorSet").  The theme stays in AllowDark, so the system decides and this only
 * asks again what that means right now.
 *
 * @param err reserved for a future failure
 * @return TRUE when the palette changed and the windows have to be re-attached
 */
bool theme_refresh(struct ov_error *const err);

/**
 * @brief Hand a window and all of its children back to the theme
 *
 * Call it after theme_refresh() reported a change: theme classes are swapped, painting
 * subclasses are attached or removed, the whole tree is redrawn.
 *
 * @param top the window to start from
 */
void theme_reapply(HWND const top);

/**
 * @brief Attach the theme to one window
 *
 * Depending on its window class and button style the window gets a theme class, a subclass
 * that paints it, or nothing at all.  Call it right after CreateWindowExW().  Attaching a
 * list view before its columns exist leaves the header unthemed, use theme_attach_list()
 * for that.
 *
 * @param hwnd the window to attach to
 */
void theme_attach(HWND const hwnd);

/**
 * @brief Attach the theme to a list view and to its header
 *
 * Call it after the columns were inserted, otherwise the header window does not exist yet.
 *
 * @param list the list view to attach to
 */
void theme_attach_list(HWND const list);

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
 * @param handled receives TRUE when the message was consumed, then the caller has to
 *                return result.  Untouched when the message was not consumed.
 * @param err reserved for a future failure
 * @return always true, the signature exists so that a future failure can be reported
 */
bool theme_message(HWND const hwnd,
                   UINT const msg,
                   WPARAM const wparam,
                   LPARAM const lparam,
                   LRESULT *const result,
                   bool *const handled,
                   struct ov_error *const err);

/**
 * @brief The brush to give to WNDCLASSEXW::hbrBackground
 *
 * The brush stays owned by the theme, the caller must not delete it.
 *
 * @return the brush of the palette
 */
HBRUSH theme_class_brush(void);

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
 */
void theme_frame(HDC const dc, RECT const *const rect, RECT const *const caption, int const dpi);

#endif /* ADC_THEME_H */
