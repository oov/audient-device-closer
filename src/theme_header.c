#include "theme_header.h"

#include <commctrl.h>

#include <string.h>

#include "layout.h"
#include "theme_internal.h"

/**
 * @brief The sizes the header paints with, in logical units
 *
 * The insets of a caption in its column, and the inset of the divider between two
 * columns.  The hairline itself is ADC_THEME_FRAME_LINE: a header divides its columns with
 * the same line a group frame is drawn with.
 */
enum theme_header_size {
  ADC_THEME_HEADER_TEXT_L = 8,        /* left inset of a header caption */
  ADC_THEME_HEADER_TEXT_R = 4,        /* right inset of a header caption */
  ADC_THEME_HEADER_DIVIDER_INSET = 4, /* vertical inset of a column divider */
};

/**
 * @brief Paint the strip that holds the column captions
 *
 * The header of a list view: its captions are drawn with DrawThemeText, which reads the
 * colour from the theme definition and ignores SetTextColor, so the whole strip is
 * painted here.
 *
 * @param hwnd the header to paint
 * @param err receives the failure of the painting
 * @return false when the header could not be painted
 */
static NODISCARD bool paint_window(HWND const hwnd, struct ov_error *const err) {
  PAINTSTRUCT ps;
  HDC dc = NULL;
  HFONT old_font = NULL;
  HPEN pen = NULL;
  HPEN old_pen = NULL;
  int const dpi = theme_window_dpi(hwnd);
  int const divider = layout_scale(ADC_THEME_FRAME_LINE, (UINT)dpi);
  int const inset = layout_scale(ADC_THEME_HEADER_DIVIDER_INSET, (UINT)dpi);
  RECT full;
  int const count = Header_GetItemCount(hwnd);
  int x = 0;
  bool success = false;

  dc = BeginPaint(hwnd, &ps);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  old_font = (HFONT)SelectObject(dc, theme_window_font(hwnd));
  if ((old_font == NULL) || (old_font == HGDI_ERROR)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    old_font = NULL;
    goto cleanup;
  }
  pen = CreatePen(PS_SOLID, divider, theme_palette_border());
  if (pen == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  old_pen = (HPEN)SelectObject(dc, pen);
  if ((old_pen == NULL) || (old_pen == HGDI_ERROR)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    old_pen = NULL;
    goto cleanup;
  }

  if (!GetClientRect(hwnd, &full)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (!FillRect(dc, &full, theme_state_control())) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  if (!SetBkMode(dc, TRANSPARENT)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  if (SetTextColor(dc, theme_palette_text()) == CLR_INVALID) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  for (int i = 0; i < count; i++) {
    HDITEMW hi;
    wchar_t text[128];
    RECT cell;

    memset(&hi, 0, sizeof(hi));
    memset(text, 0, sizeof(text));
    hi.mask = HDI_WIDTH | HDI_TEXT;
    hi.pszText = text;
    hi.cchTextMax = (int)(sizeof(text) / sizeof(text[0]));
    if (SendMessageW(hwnd, HDM_GETITEMW, (WPARAM)i, (LPARAM)&hi) == 0) {
      break;
    }
    cell.left = x;
    cell.top = full.top;
    cell.right = x + (int)hi.cxy;
    cell.bottom = full.bottom;
    {
      RECT box = cell;
      box.left += layout_scale(ADC_THEME_HEADER_TEXT_L, (UINT)dpi);
      box.right -= layout_scale(ADC_THEME_HEADER_TEXT_R, (UINT)dpi);
      if ((DrawTextW(dc, text, -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) == 0) && (text[0] != L'\0')) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
        goto cleanup;
      }
    }
    if (i + 1 < count) {
      if (!MoveToEx(dc, cell.right - divider, cell.top + inset, NULL)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
        goto cleanup;
      }
      if (!LineTo(dc, cell.right - divider, cell.bottom - inset)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
        goto cleanup;
      }
    }
    x += (int)hi.cxy;
  }
  success = true;

cleanup:
  if (old_pen != NULL) {
    if (SelectObject(dc, old_pen) == NULL) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    old_pen = NULL;
  }
  if (pen != NULL) {
    if (!DeleteObject(pen)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    pen = NULL;
  }
  if (old_font != NULL) {
    if (SelectObject(dc, old_font) == NULL) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    old_font = NULL;
  }
  if (dc != NULL) {
    if (!EndPaint(hwnd, &ps)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    dc = NULL;
  }
  return success;
}

/**
 * @brief The subclass that paints the header of a list view
 *
 * @param hwnd the header the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @param id the subclass to answer for
 * @param ref unused
 * @return the answer of the default procedure when the message was not consumed
 */
static LRESULT CALLBACK
subclass(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, UINT_PTR const id, DWORD_PTR const ref) {
  (void)ref;
  switch (msg) {
  case WM_NCDESTROY:
    if (!RemoveWindowSubclass(hwnd, subclass, id)) {
      struct ov_error err = {0};

      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&err, NULL);
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
  case WM_PAINT: {
    struct ov_error err = {0};

    if (!paint_window(hwnd, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
  }
    return 0;
  default:
    break;
  }
  return DefSubclassProc(hwnd, msg, wparam, lparam);
}

/**
 * @brief Attach the theme to the header of a list view
 *
 * @param header the header to attach to, NULL is allowed
 * @param err receives the failure of the subclass
 * @return false when the painting could not be taken over
 */
NODISCARD bool theme_header_attach(HWND const header, struct ov_error *const err) {
  if (header == NULL) {
    return true;
  }
  if (!theme_state_dark()) {
    return theme_header_detach(header, err);
  }
  if (!SetWindowSubclass(header, subclass, 1, 0)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

/**
 * @brief Give the header of a list view back to the theme of the system
 *
 * @param header the header to detach from, NULL is allowed
 * @param err receives the failure of the removal
 * @return false when the subclass could not be removed
 */
NODISCARD bool theme_header_detach(HWND const header, struct ov_error *const err) {
  DWORD_PTR ref = 0;

  if (header == NULL) {
    return true;
  }
  // a header that was never themed has no subclass to give back, which is not a failure
  if (!GetWindowSubclass(header, subclass, 1, &ref)) {
    return true;
  }
  if (!RemoveWindowSubclass(header, subclass, 1)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}
