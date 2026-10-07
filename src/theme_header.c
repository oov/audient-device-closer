#include "theme_header.h"

#include <commctrl.h>

#include <string.h>

#include "layout.h"
#include "theme_internal.h"

/**
 * @brief Paint the strip that holds the column captions
 *
 * The header of a list view: its captions are drawn with DrawThemeText, which reads the
 * colour from the theme definition and ignores SetTextColor, so the whole strip is
 * painted here.
 *
 * @param hwnd the header to paint
 */
static void header_paint(HWND const hwnd) {
  PAINTSTRUCT ps;
  HDC dc = NULL;
  HFONT old_font = NULL;
  HPEN pen = NULL;
  HPEN old_pen = NULL;
  int const dpi = theme_window_dpi(hwnd);
  int const divider = layout_scale(ADC_LAYOUT_FRAME_LINE, (UINT)dpi);
  int const inset = layout_scale(ADC_LAYOUT_HEADER_DIVIDER_INSET, (UINT)dpi);
  RECT full;
  int const count = Header_GetItemCount(hwnd);
  int x = 0;

  dc = BeginPaint(hwnd, &ps);
  if (dc == NULL) {
    goto cleanup;
  }
  old_font = (HFONT)SelectObject(dc, theme_window_font(hwnd));
  pen = CreatePen(PS_SOLID, divider, theme_palette_border());
  if (pen == NULL) {
    goto cleanup;
  }
  old_pen = (HPEN)SelectObject(dc, pen);

  GetClientRect(hwnd, &full);
  FillRect(dc, &full, theme_state_control());
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, theme_palette_text());
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
      box.left += layout_scale(ADC_LAYOUT_HEADER_TEXT_L, (UINT)dpi);
      box.right -= layout_scale(ADC_LAYOUT_HEADER_TEXT_R, (UINT)dpi);
      DrawTextW(dc, text, -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (i + 1 < count) {
      MoveToEx(dc, cell.right - divider, cell.top + inset, NULL);
      LineTo(dc, cell.right - divider, cell.bottom - inset);
    }
    x += (int)hi.cxy;
  }

cleanup:
  if (old_pen != NULL) {
    SelectObject(dc, old_pen);
    old_pen = NULL;
  }
  if (pen != NULL) {
    DeleteObject(pen);
    pen = NULL;
  }
  if (old_font != NULL) {
    SelectObject(dc, old_font);
    old_font = NULL;
  }
  if (dc != NULL) {
    EndPaint(hwnd, &ps);
    dc = NULL;
  }
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
header_subclass(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, UINT_PTR const id, DWORD_PTR const ref) {
  (void)ref;
  switch (msg) {
  case WM_NCDESTROY:
    RemoveWindowSubclass(hwnd, header_subclass, id);
    return DefSubclassProc(hwnd, msg, wparam, lparam);
  case WM_PAINT:
    header_paint(hwnd);
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
 */
void theme_header_attach(HWND const header) {
  if (header == NULL) {
    return;
  }
  if (!theme_state_dark()) {
    theme_header_detach(header);
    return;
  }
  SetWindowSubclass(header, header_subclass, 1, 0);
}

/**
 * @brief Give the header of a list view back to the theme of the system
 *
 * @param header the header to detach from, NULL is allowed
 */
void theme_header_detach(HWND const header) {
  if (header != NULL) {
    RemoveWindowSubclass(header, header_subclass, 1);
  }
}
