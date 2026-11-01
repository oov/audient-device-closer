#ifndef ADC_THEME_INTERNAL_H
#define ADC_THEME_INTERNAL_H

#include <windows.h>

#include <ovbase.h>

/**
 * Private interface between the parts of the theme implementation.
 *
 * theme.c owns the palette and the process wide state.  A component that paints one
 * kind of control (theme_button.c, theme_header.c, ...) asks for colours and helpers
 * through this header and is never referenced by the application code.
 *
 * Do not include this from outside the theme implementation.
 */

/**
 * @brief Is the dark palette active
 *
 * @return TRUE when the dark palette is active
 */
bool theme_state_dark(void);

/**
 * @brief The brushes of the palette
 *
 * They are owned by theme.c, a component must not delete them, and it may keep the
 * pointer for as long as the window lives.
 *
 * @return the brush of the surface a control sits on
 */
HBRUSH theme_state_surface(void);

/**
 * @brief The brush of the face of a control
 *
 * @return the brush to paint with
 */
HBRUSH theme_state_control(void);

/**
 * @brief The colour of the surface a control sits on
 *
 * A light theme answers with the system colour, so a component never has to branch on
 * dark or light.
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_surface(void);

/**
 * @brief The colour of a caption
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_text(void);

/**
 * @brief The colour of a control, lists included, in the palette of the theme
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_control(void);

/**
 * @brief The colour of a caption that cannot be acted on
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_text_disabled(void);

/**
 * @brief The colour of a frame line
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_border(void);

/**
 * @brief The colour of the face of a push button
 *
 * The push button of the explorer theme paints a light face in every state, so its states
 * are not taken from the theme at all.
 *
 * @param state the state of the button: hot, pressed or at rest
 * @param enabled FALSE takes the colour a disabled button has
 * @return the colour to paint with
 */
COLORREF theme_palette_button(int const state, BOOL const enabled);

/**
 * @brief The colour of the border of a push button
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_button_border(void);

/**
 * @brief The dpi a window is drawn for
 *
 * @param hwnd the window to ask
 * @return the dpi of the window
 */
int theme_window_dpi(HWND const hwnd);

/**
 * @brief The font this window draws its captions with
 *
 * @param hwnd the window to ask
 * @return the font of the window
 */
HFONT theme_window_font(HWND const hwnd);

/**
 * @brief The sizes the theme paints with, in logical units
 *
 * They are dimensions like every other one: no component draws from a literal, and all of
 * them scale with the dpi of the window.  The parts of the theme sit in separate files but
 * draw one surface, so the hairline they share is named once, here, beside the palette.
 */
enum theme_frame_size {
  ADC_THEME_FRAME_LINE = 1, /* hairline: group frame and header divider */
};

#endif /* ADC_THEME_INTERNAL_H */
