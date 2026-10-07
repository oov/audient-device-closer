#include "theme_button.h"

#include <vsstyle.h>

#include <commctrl.h>
#include <uxtheme.h>

#include <string.h>

#include <ovbase.h>

#include "layout.h"
#include "theme_internal.h"

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
static void button_paint_destroy(struct button_paint *paint) {
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
static int button_state(HWND const button, BOOL const hot) {
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
 */
static void draw_focus_cue(HDC const dc, HWND const hwnd, RECT const *const text_box) {
  RECT cue;

  if (GetFocus() != hwnd) {
    return;
  }
  if ((SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0) {
    return;
  }
  cue = *text_box;
  InflateRect(&cue, 1, 1);
  DrawFocusRect(dc, &cue);
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
static void button_paint_face(HDC const dc, HWND const hwnd, BOOL const hot) {
  HFONT old_font = NULL;
  HBRUSH fill = NULL;
  HPEN pen = NULL;
  HBRUSH old_brush = NULL;
  HPEN old_pen = NULL;
  int const dpi = theme_window_dpi(hwnd);
  int const radius = layout_scale(ADC_LAYOUT_BUTTON_CORNER, (UINT)dpi);
  RECT rc;
  BOOL const enabled = IsWindowEnabled(hwnd);
  int const state = button_state(hwnd, hot);
  wchar_t text[128];

  old_font = (HFONT)SelectObject(dc, theme_window_font(hwnd));
  GetClientRect(hwnd, &rc);

  FillRect(dc, &rc, theme_state_surface());
  fill = CreateSolidBrush(theme_palette_button(state, enabled));
  pen = CreatePen(PS_SOLID, layout_scale(ADC_LAYOUT_BUTTON_EDGE, (UINT)dpi), theme_palette_button_border());
  if ((fill == NULL) || (pen == NULL)) {
    goto cleanup;
  }
  old_brush = (HBRUSH)SelectObject(dc, fill);
  old_pen = (HPEN)SelectObject(dc, pen);
  RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

  memset(text, 0, sizeof(text));
  GetWindowTextW(hwnd, text, (int)(sizeof(text) / sizeof(text[0])));
  {
    int const margin = layout_scale(ADC_LAYOUT_BUTTON_MARGIN, (UINT)dpi);
    int const width = rc.right - rc.left;
    RECT measure;
    RECT box;
    int text_width = 0;
    int text_height = 0;
    int x = 0;

    memset(&measure, 0, sizeof(measure));
    measure.right = width;
    measure.bottom = rc.bottom - rc.top;
    DrawTextW(dc, text, -1, &measure, DT_CALCRECT | DT_SINGLELINE);
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

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? theme_palette_text() : theme_palette_text_disabled());
    DrawTextW(dc, text, -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    box.left = x;
    box.right = x + text_width;
    box.top = rc.top + ((rc.bottom - rc.top) - text_height) / 2;
    box.bottom = box.top + text_height;
    draw_focus_cue(dc, hwnd, &box);
  }

cleanup:
  if (old_pen != NULL) {
    SelectObject(dc, old_pen);
    old_pen = NULL;
  }
  if (old_brush != NULL) {
    SelectObject(dc, old_brush);
    old_brush = NULL;
  }
  if (pen != NULL) {
    DeleteObject(pen);
    pen = NULL;
  }
  if (fill != NULL) {
    DeleteObject(fill);
    fill = NULL;
  }
  if (old_font != NULL) {
    SelectObject(dc, old_font);
    old_font = NULL;
  }
}

/**
 * @brief Paint one button in the palette of the theme
 *
 * @param hwnd the button to paint
 * @param hot the hover state the subclass kept
 */
static void button_paint(HWND const hwnd, BOOL const hot) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);

  if (dc != NULL) {
    button_paint_face(dc, hwnd, hot);
    EndPaint(hwnd, &ps);
  }
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
button_subclass(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, UINT_PTR const id, DWORD_PTR const ref) {
  struct button_paint *paint = (struct button_paint *)(void *)ref;

  switch (msg) {
  case WM_NCDESTROY:
    RemoveWindowSubclass(hwnd, button_subclass, id);
    button_paint_destroy(paint);
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
      TrackMouseEvent(&tme);
      InvalidateRect(hwnd, NULL, FALSE);
    }
    break;
  case WM_MOUSELEAVE:
    if (paint != NULL) {
      paint->hot = FALSE;
      InvalidateRect(hwnd, NULL, FALSE);
    }
    break;
  case WM_ENABLE:
    if (paint != NULL) {
      paint->hot = FALSE;
    }
    {
      LRESULT const result = DefWindowProcW(hwnd, msg, wparam, lparam);
      InvalidateRect(hwnd, NULL, FALSE);
      return result;
    }
  case WM_CAPTURECHANGED:
    if (paint != NULL) {
      paint->hot = (GetCapture() == hwnd) ? paint->hot : FALSE;
    }
    InvalidateRect(hwnd, NULL, FALSE);
    break;
  case WM_THEMECHANGED:
  case WM_UPDATEUISTATE:
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    InvalidateRect(hwnd, NULL, FALSE);
    break;
  case WM_PAINT:
    button_paint(hwnd, (paint != NULL) ? paint->hot : FALSE);
    return 0;
  case WM_PRINTCLIENT:
    button_paint_face((HDC)wparam, hwnd, (paint != NULL) ? paint->hot : FALSE);
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
static struct button_paint *ensure_subclass(HWND const button, struct ov_error *const err) {
  struct button_paint *paint = NULL;
  DWORD_PTR existing = 0;
  bool success = false;

  if (GetWindowSubclass(button, button_subclass, 1, &existing) && existing != 0) {
    return (struct button_paint *)(void *)existing;
  }
  if (!OV_REALLOC(&paint, 1, sizeof(*paint))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  paint->hot = FALSE;
  if (!SetWindowSubclass(button, button_subclass, 1, (DWORD_PTR)(void *)paint)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  SetWindowTheme(button, L"DarkMode_Explorer", NULL);
  success = true;

cleanup:
  if (!success) {
    OV_FREE(&paint);
    paint = NULL;
  }
  return paint;
}

/**
 * @brief Attach the theme to a push button
 *
 * The button keeps the face of the explorer theme and stays usable when the paint state
 * cannot be made.
 *
 * @param button the button to attach to, NULL is allowed
 */
void theme_button_attach(HWND const button) {
  struct ov_error err = {0};

  if (button == NULL) {
    return;
  }
  if (!theme_state_dark()) {
    theme_button_detach(button);
    return;
  }
  if (ensure_subclass(button, &err) == NULL) {
    OV_ERROR_REPORT(&err, NULL);
  }
}

/**
 * @brief Give a push button back to the theme of the system
 *
 * @param button the button to detach from, NULL is allowed
 */
void theme_button_detach(HWND const button) {
  DWORD_PTR existing = 0;

  if (button == NULL) {
    return;
  }
  if (GetWindowSubclass(button, button_subclass, 1, &existing) && existing != 0) {
    struct button_paint *paint = (struct button_paint *)(void *)existing;
    RemoveWindowSubclass(button, button_subclass, 1);
    button_paint_destroy(paint);
    SetWindowTheme(button, NULL, NULL);
  }
}
