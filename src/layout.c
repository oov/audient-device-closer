#include "layout.h"

/**
 * @brief The size of the font every window draws with, in points
 *
 * A window whose font differs from the one of the next would make the two look like
 * different programs, so the size is named once here and reached through
 * layout_font_height() by every window and by the tests that read a font back.
 */
enum { ADC_LAYOUT_FONT_POINT = 9 };

/**
 * @brief Turn one logical value into physical pixels for the given dpi
 *
 * @param logical the value in logical units (1/96 inch)
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the value in physical pixels
 */
int layout_scale(int const logical, UINT const dpi) {
  static UINT const base = USER_DEFAULT_SCREEN_DPI;
  UINT const d = (dpi != 0) ? dpi : base;
  return MulDiv(logical, (int)d, (int)base);
}

/**
 * @brief A rectangle from its left, top and size
 *
 * @param left the left edge
 * @param top the top edge
 * @param width how wide the rectangle is
 * @param height how tall the rectangle is
 * @return the rectangle
 */
RECT layout_rect(int const left, int const top, int const width, int const height) {
  RECT r;
  r.left = left;
  r.top = top;
  r.right = left + width;
  r.bottom = top + height;
  return r;
}

/**
 * @brief The outer rectangle of a window whose client area has one fixed size
 *
 * @param client_width  the width of the client area in physical pixels
 * @param client_height the height of the client area in physical pixels
 * @param dpi           the dpi of the window, 0 falls back to the base of 96
 * @param style         the window style the frame is drawn with
 * @param ex_style      the extended window style of the same frame
 * @param out           receives the outer rectangle, its top left corner at the origin
 */
void layout_window_rect(
    int const client_width, int const client_height, UINT const dpi, DWORD const style, DWORD const ex_style, RECT *const out) {
  SetRect(out, 0, 0, client_width, client_height);
  if (!AdjustWindowRectExForDpi(out, style, FALSE, ex_style, dpi)) {
    /* the margins of a plain window stand in for a frame that could not be asked for */
    int const side = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
    int const caption = layout_scale(ADC_LAYOUT_CAPTION_H, dpi);
    SetRect(out, -side, -caption, client_width + side, client_height + caption);
  }
}

/**
 * @brief The height argument of CreateFontW for one dpi
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the height to give to CreateFontW
 */
int layout_font_height(UINT const dpi) {
  UINT const d = (dpi != 0) ? dpi : USER_DEFAULT_SCREEN_DPI;
  return -MulDiv(ADC_LAYOUT_FONT_POINT, (int)d, 72);
}

/**
 * @brief The font the controls of a window draw with, made for one dpi
 *
 * @param dpi the dpi of the window, 0 falls back to the base of 96
 * @return the font, NULL when it could not be made; the caller deletes it
 */
HFONT layout_make_font(UINT const dpi) {
  return CreateFontW(layout_font_height(dpi),
                     0,
                     0,
                     0,
                     FW_NORMAL,
                     FALSE,
                     FALSE,
                     FALSE,
                     DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE,
                     L"Segoe UI");
}
