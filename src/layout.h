#pragma once

#include <windows.h>

/**
 * @brief What every window of the program measures with
 *
 * A logical unit is 1/96 of an inch, that is the value Windows reports as the base dpi
 * (USER_DEFAULT_SCREEN_DPI).  Nothing in the application may place a control in physical
 * pixels: physical sizes only exist after layout_scale() turned a logical value into
 * pixels for the dpi of the window.
 *
 * Only the room every window shares lives here.  The geometry of a window and the sizes
 * only that window places belong to the window itself, next to the code that places it.
 */
enum layout_shared_size {
  ADC_LAYOUT_MARGIN_X = 12,      /* left and right of the client area */
  ADC_LAYOUT_MARGIN_TOP = 10,    /* first control to the top edge */
  ADC_LAYOUT_MARGIN_BOTTOM = 12, /* the last control to the bottom edge */
  ADC_LAYOUT_GAP = 8,            /* default distance between two controls */

  /* The room reserved for a caption when the font could not be asked.  A group of the main
     window falls back to it, and so does the frame of a window whose own frame could not be
     measured. */
  ADC_LAYOUT_CAPTION_H = 16,
};

/**
 * @brief Turn one logical value into physical pixels for the given dpi
 *
 * @param logical the value in logical units (1/96 inch)
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the value in physical pixels
 */
int layout_scale(int const logical, UINT const dpi);

/**
 * @brief A rectangle from its left, top and size
 *
 * @param left the left edge
 * @param top the top edge
 * @param width how wide the rectangle is
 * @param height how tall the rectangle is
 * @return the rectangle
 */
RECT layout_rect(int const left, int const top, int const width, int const height);

/**
 * @brief The outer rectangle of a window with one client size
 *
 * The only place that turns a client size into the size the window manager is told.
 *
 * @param client_width  the width of the client area in physical pixels
 * @param client_height the height of the client area in physical pixels
 * @param dpi           the dpi of the window, 0 falls back to the base of 96
 * @param style         the window style the frame is drawn with
 * @param ex_style      the extended window style of the same frame
 * @param out           receives the outer rectangle, its top left corner at the origin
 */
void layout_window_rect(int client_width, int client_height, UINT dpi, DWORD style, DWORD ex_style, RECT *out);

/**
 * @brief The height argument of CreateFontW for one dpi
 *
 * Negative, the way CreateFontW asks for a character height rather than a cell height.
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the height to give to CreateFontW
 */
int layout_font_height(UINT const dpi);

/**
 * @brief The font the controls of a window draw with, made for one dpi
 *
 * Every window owns its font and deletes it with the window.
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the font, NULL when it could not be made; the caller deletes it
 */
HFONT layout_make_font(UINT const dpi);
