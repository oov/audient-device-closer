#include "theme_button.h"

#include <vsstyle.h>

#include <commctrl.h>
#include <uxtheme.h>

#include <string.h>

#include <ovbase.h>

#include "layout.h"
#include "theme_internal.h"

/**
 * @brief The sizes the button paints with, in logical units
 *
 * The border and the corner radius of the box it draws, and the room its caption keeps
 * away from that border.
 */
enum theme_button_size {
  ADC_THEME_BUTTON_EDGE = 1,   /* border width of a push button */
  ADC_THEME_BUTTON_CORNER = 5, /* corner radius of a push button */
  ADC_THEME_BUTTON_MARGIN = 12 /* caption margin inside a push button */
};

/**
 * @brief What the subclass paints a button with
 *
 * The hover state is not exposed by the control, so the subclass keeps it.
 */
struct button_paint {
  BOOL hot;
};

/**
 * @brief Give the paint state back
 *
 * The paint state dies with the window or when the theme gives the button back.
 *
 * @param paint the state to release, NULL is allowed
 */
static void paint_destroy(struct button_paint *paint) {
  if (paint == NULL) {
    return;
  }
  OV_FREE(&paint);
}

/**
 * @brief The state of the face of a button
 *
 * The hover state is not exposed by the control, so the subclass keeps it and hands it
 * in; without it the hot colour of the palette is unreachable.
 *
 * @param button the button to look at
 * @param hot the hover state the subclass kept
 * @return the state of the face: hot, pressed or at rest
 */
static int visual_state(HWND const button, BOOL const hot) {
  if (!IsWindowEnabled(button)) {
    return PBS_DISABLED;
  }
  if (GetCapture() == button) {
    return PBS_PRESSED;
  }
  if (hot) {
    return PBS_HOT;
  }
  return PBS_NORMAL;
}

/**
 * @brief Whether a press of ENTER answers with this button
 *
 * @param button the button to look at
 * @return true when the button carries the style of the default button
 */
static bool is_default(HWND const button) { return (GetWindowLongPtrW(button, GWL_STYLE) & 0xF) == BS_DEFPUSHBUTTON; }

/**
 * @brief Draw the keyboard cue around the caption
 *
 * The keyboard cue is drawn around the caption, the way comctl32 draws it, and only while
 * the keyboard is in use: WM_QUERYUISTATE reports whether the last input was a keystroke.
 * The rectangle of the caption is handed in, not the client rectangle: a cue around the
 * whole button ignores the rounded corners and does not look like a focus cue at all.
 *
 * @param dc the context to draw in
 * @param hwnd the button that has the cue
 * @param text_box the rectangle of the caption
 * @param err receives the failure of the drawing
 * @return false when the cue could not be drawn
 */
static NODISCARD bool draw_focus_cue(HDC const dc, HWND const hwnd, RECT const *const text_box, struct ov_error *const err) {
  RECT cue;

  if (GetFocus() != hwnd) {
    return true;
  }
  if ((SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0) {
    return true;
  }
  cue = *text_box;
  InflateRect(&cue, 1, 1);
  if (!DrawFocusRect(dc, &cue)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    return false;
  }
  return true;
}

/**
 * @brief Paint the face and the caption of a push button
 *
 * A themed caption draws itself with DrawThemeText and ignores SetTextColor, so the whole
 * button is painted here.  dc comes from BeginPaint for WM_PAINT and from the caller for
 * WM_PRINTCLIENT, so the painting itself is one function.
 *
 * @param dc the context to draw in
 * @param hwnd the button to paint
 * @param hot the hover state the subclass kept
 */
static NODISCARD bool paint_face(HDC const dc, HWND const hwnd, BOOL const hot, struct ov_error *const err) {
  HFONT old_font = NULL;
  HBRUSH fill = NULL;
  HPEN pen = NULL;
  HBRUSH old_brush = NULL;
  HPEN old_pen = NULL;
  int const dpi = theme_window_dpi(hwnd);
  int const radius = layout_scale(ADC_THEME_BUTTON_CORNER, (UINT)dpi);
  RECT rc;
  BOOL const enabled = IsWindowEnabled(hwnd);
  int const state = visual_state(hwnd, hot);
  wchar_t text[128];
  bool success = false;

  if (!GetClientRect(hwnd, &rc)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  old_font = (HFONT)SelectObject(dc, theme_window_font(hwnd));
  if ((old_font == NULL) || (old_font == HGDI_ERROR)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    old_font = NULL;
    goto cleanup;
  }
  if (!FillRect(dc, &rc, theme_state_surface())) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  fill = CreateSolidBrush(theme_palette_button(state, enabled));
  pen = CreatePen(PS_SOLID, layout_scale(ADC_THEME_BUTTON_EDGE, (UINT)dpi), theme_palette_button_border());
  if ((fill == NULL) || (pen == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  old_brush = (HBRUSH)SelectObject(dc, fill);
  if ((old_brush == NULL) || (old_brush == HGDI_ERROR)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    old_brush = NULL;
    goto cleanup;
  }
  old_pen = (HPEN)SelectObject(dc, pen);
  if ((old_pen == NULL) || (old_pen == HGDI_ERROR)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    old_pen = NULL;
    goto cleanup;
  }
  if (!RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  if (is_default(hwnd)) {
    // the frame of the default button is one edge wider than the frame of the others
    int const inset = layout_scale(ADC_THEME_BUTTON_EDGE, (UINT)dpi);

    if (!RoundRect(dc, rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset, radius, radius)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
  }

  memset(text, 0, sizeof(text));
  if (GetWindowTextW(hwnd, text, (int)(sizeof(text) / sizeof(text[0]))) == 0) {
    // an empty caption is not a failure of the drawing
    text[0] = L'\0';
  }
  {
    int const margin = layout_scale(ADC_THEME_BUTTON_MARGIN, (UINT)dpi);
    int const width = rc.right - rc.left;
    RECT measure;
    RECT box;
    int text_width = 0;
    int text_height = 0;
    int x = 0;

    memset(&measure, 0, sizeof(measure));
    measure.right = width;
    measure.bottom = rc.bottom - rc.top;
    if ((DrawTextW(dc, text, -1, &measure, DT_CALCRECT | DT_SINGLELINE) == 0) && (text[0] != L'\0')) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
    text_width = measure.right - measure.left;
    text_height = measure.bottom - measure.top;

    x = rc.left + (width - text_width) / 2;
    if (x < rc.left + margin) {
      x = rc.left + margin;
    }
    box.left = x;
    box.right = rc.right - margin; // the caption may use everything that is left
    box.top = rc.top;
    box.bottom = rc.bottom;

    if (!SetBkMode(dc, TRANSPARENT)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
    if (SetTextColor(dc, enabled ? theme_palette_text() : theme_palette_text_disabled()) == CLR_INVALID) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
    if ((DrawTextW(dc, text, -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) == 0) && (text[0] != L'\0')) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }

    box.left = x;
    box.right = x + text_width;
    box.top = rc.top + ((rc.bottom - rc.top) - text_height) / 2;
    box.bottom = box.top + text_height;
    if (!draw_focus_cue(dc, hwnd, &box, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
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
  if (old_brush != NULL) {
    if (SelectObject(dc, old_brush) == NULL) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    old_brush = NULL;
  }
  if (pen != NULL) {
    if (!DeleteObject(pen)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    pen = NULL;
  }
  if (fill != NULL) {
    if (!DeleteObject(fill)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    fill = NULL;
  }
  if (old_font != NULL) {
    if (SelectObject(dc, old_font) == NULL) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    old_font = NULL;
  }
  return success;
}

/**
 * @brief Paint one button in the palette of the theme
 *
 * @param hwnd the button to paint
 * @param hot the hover state the subclass kept
 * @param err receives the failure of the painting
 * @return false when the button could not be painted
 */
static NODISCARD bool paint_window(HWND const hwnd, BOOL const hot, struct ov_error *const err) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  bool success = false;

  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = paint_face(dc, hwnd, hot, err);

cleanup:
  if (dc != NULL) {
    if (!EndPaint(hwnd, &ps)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
      success = false;
    }
  }
  return success;
}

/**
 * @brief The subclass that paints a button and keeps its hover state
 *
 * @param hwnd the button the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @param id the subclass to answer for
 * @param ref the paint state of the button
 * @return the answer of the default procedure when the message was not consumed
 */
static LRESULT CALLBACK
subclass(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, UINT_PTR const id, DWORD_PTR const ref) {
  struct button_paint *paint = (struct button_paint *)(void *)ref;

  switch (msg) {
  case WM_NCDESTROY:
    if (!RemoveWindowSubclass(hwnd, subclass, id)) {
      struct ov_error err = {0};

      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&err, NULL);
    }
    paint_destroy(paint);
    return DefSubclassProc(hwnd, msg, wparam, lparam);
  case WM_ERASEBKGND:
    return 1;
  case WM_MOUSEMOVE:
    if (paint != NULL && !paint->hot) {
      TRACKMOUSEEVENT tme;
      paint->hot = TRUE;
      memset(&tme, 0, sizeof(tme));
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd;
      if (!TrackMouseEvent(&tme)) {
        struct ov_error err = {0};

        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&err, NULL);
      }
      if (!InvalidateRect(hwnd, NULL, FALSE)) {
        struct ov_error err = {0};

        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&err, NULL);
      }
    }
    break;
  case WM_MOUSELEAVE:
    if (paint != NULL) {
      paint->hot = FALSE;
      if (!InvalidateRect(hwnd, NULL, FALSE)) {
        struct ov_error err = {0};

        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&err, NULL);
      }
    }
    break;
  case WM_ENABLE:
    if (paint != NULL) {
      paint->hot = FALSE;
    }
    {
      LRESULT const result = DefWindowProcW(hwnd, msg, wparam, lparam);

      if (!InvalidateRect(hwnd, NULL, FALSE)) {
        struct ov_error err = {0};

        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&err, NULL);
      }
      return result;
    }
  case WM_CAPTURECHANGED:
    if (paint != NULL) {
      paint->hot = (GetCapture() == hwnd) ? paint->hot : FALSE;
    }
    if (!InvalidateRect(hwnd, NULL, FALSE)) {
      struct ov_error err = {0};

      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&err, NULL);
    }
    break;
  case WM_THEMECHANGED:
  case WM_UPDATEUISTATE:
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    if (!InvalidateRect(hwnd, NULL, FALSE)) {
      struct ov_error err = {0};

      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&err, NULL);
    }
    break;
  case WM_PAINT: {
    struct ov_error err = {0};

    if (!paint_window(hwnd, (paint != NULL) ? paint->hot : FALSE, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
  }
    return 0;
  case WM_PRINTCLIENT: {
    struct ov_error err = {0};

    if (!paint_face((HDC)wparam, hwnd, (paint != NULL) ? paint->hot : FALSE, &err)) {
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
 * @brief Make sure one button is painted by this component
 *
 * @param button the button to subclass
 * @param err receives the failure of the allocation or of the subclass
 * @return the paint state of the button, NULL when it could not be subclassed
 */
static NODISCARD struct button_paint *ensure_subclass(HWND const button, struct ov_error *const err) {
  struct button_paint *paint = NULL;
  struct button_paint *result = NULL;
  DWORD_PTR existing = 0;
  bool success = false;

  if (GetWindowSubclass(button, subclass, 1, &existing) && existing != 0) {
    result = (struct button_paint *)(void *)existing;
    goto cleanup;
  }
  if (!OV_REALLOC(&paint, 1, sizeof(*paint))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  paint->hot = FALSE;
  if (!SetWindowSubclass(button, subclass, 1, (DWORD_PTR)(void *)paint)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  {
    HRESULT const hr = SetWindowTheme(button, L"DarkMode_Explorer", NULL);

    if (FAILED(hr)) {
      OV_ERROR_SET_HRESULT(err, hr);
      goto cleanup;
    }
  }
  success = true;

cleanup:
  if (success) {
    result = paint;
  } else {
    OV_FREE(&paint);
  }
  return result;
}

/**
 * @brief Give a push button back to the theme of the system
 *
 * @param button the button to detach from, NULL is allowed
 * @param err receives the failure of the detach
 * @return false when the theme could not be given back
 */
static NODISCARD bool detach(HWND const button, struct ov_error *const err) {
  DWORD_PTR existing = 0;
  bool success = false;

  if (button == NULL) {
    return true;
  }
  if (!GetWindowSubclass(button, subclass, 1, &existing) || (existing == 0)) {
    return true;
  }
  {
    struct button_paint *const paint = (struct button_paint *)(void *)existing;
    HRESULT const hr = SetWindowTheme(button, NULL, NULL);

    if (!RemoveWindowSubclass(button, subclass, 1)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    paint_destroy(paint);
    if (FAILED(hr)) {
      OV_ERROR_SET_HRESULT(err, hr);
      goto cleanup;
    }
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Attach the theme to a push button
 *
 * The button keeps the face of the explorer theme and stays usable when the paint state
 * cannot be made.
 *
 * @param button the button to attach to, NULL is allowed
 * @param err receives the failure of the subclass
 * @return false when the painting could not be taken over
 */
NODISCARD bool theme_button_attach(HWND const button, struct ov_error *const err) {
  if (button == NULL) {
    return true;
  }
  if (!theme_state_dark()) {
    if (!detach(button, err)) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    return true;
  }
  if (ensure_subclass(button, err) == NULL) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  return true;
}
