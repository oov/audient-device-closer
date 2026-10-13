#include "theme_combo.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <vsstyle.h>

#include <stddef.h>
#include <string.h>

#include <ovbase.h>

#include "layout.h"
#include "theme_internal.h"

/**
 * @brief The room one line of a combo box is read into
 *
 * The longest line the application puts in a combo box is the name of a unit and the
 * device path of it, and a device path answers a bounded length.
 */
enum { combo_line_max = 256 };

/**
 * @brief The arrow of the field is this part of the height of the button it sits in
 */
enum { combo_arrow_height_parts = 4 };

/**
 * @brief The arrow of the field is as wide as it is tall
 */
enum { combo_arrow_width_parts = 2 };

/**
 * @brief The chosen line keeps this distance from the frame of the field, at 96 dpi
 */
enum { combo_text_inset = 2 };

/**
 * @brief What the subclass paints a combo box with
 *
 * The hover state is not exposed by the control, so the subclass keeps it; without it the
 * hot colour of the palette is unreachable.
 */
struct combo_paint {
  BOOL hot;
};

/**
 * @brief Give the paint state back
 *
 * The paint state dies with the window or when the theme gives the combo box back.
 *
 * @param paint the state to release, NULL is allowed
 */
static void combo_paint_destroy(struct combo_paint *paint) {
  if (paint == NULL) {
    return;
  }
  OV_FREE(&paint);
}

/**
 * @brief The state of the face of a combo box
 *
 * @param combo the combo box to look at
 * @param hot the hover state the subclass kept
 * @return the state of the face: hot, pressed or at rest
 */
static int combo_state(HWND const combo, BOOL const hot) {
  if (!IsWindowEnabled(combo)) {
    return PBS_DISABLED;
  }
  if (GetCapture() == combo) {
    return PBS_PRESSED;
  }
  if (hot) {
    return PBS_HOT;
  }
  return PBS_NORMAL;
}

/**
 * @brief Draw the keyboard cue around the chosen line
 *
 * The cue follows the way comctl32 draws it: only while the keyboard is in use, which
 * WM_QUERYUISTATE reports, and around the rectangle of the line, since a cue around the
 * whole field ignores the frame and does not look like a cue at all.
 *
 * @param dc the context to draw in
 * @param hwnd the combo box that has the cue
 * @param line_box the rectangle of the chosen line
 */
static void draw_focus_cue(HDC const dc, HWND const hwnd, RECT const *const line_box) {
  RECT cue;

  if (GetFocus() != hwnd) {
    return;
  }
  if ((SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0) {
    return;
  }
  cue = *line_box;
  InflateRect(&cue, 1, 1);
  DrawFocusRect(dc, &cue);
}

/**
 * @brief Paint the face, the chosen line and the arrow of a drop down list combo box
 *
 * The face is drawn in the colour of the palette, framed with a single line of the colour
 * of a frame.  The chosen line is drawn in the colour of a caption, and the line the
 * control does not have a selection for is left out, the way the control leaves the field
 * empty.  The arrow is a small triangle drawn here instead of taken from the theme, so
 * that no font decides how it looks and no theme part has to resolve.
 *
 * dc comes from BeginPaint for WM_PAINT and from the caller for WM_PRINTCLIENT, so the
 * painting itself is one function.
 *
 * @param dc the context to draw in
 * @param hwnd the combo box to paint
 * @param hot the hover state the subclass kept
 */
static void combo_paint_face(HDC const dc, HWND const hwnd, BOOL const hot) {
  COMBOBOXINFO info;
  RECT rc;
  RECT box;
  HFONT old_font = NULL;
  HBRUSH fill = NULL;
  HBRUSH arrow = NULL;
  HPEN frame = NULL;
  HPEN glyph = NULL;
  HBRUSH old_brush = NULL;
  HPEN old_pen = NULL;
  BOOL const enabled = IsWindowEnabled(hwnd);
  int const state = combo_state(hwnd, hot);
  COLORREF const caption = enabled ? theme_palette_text() : theme_palette_text_disabled();
  LRESULT const cur = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);

  memset(&info, 0, sizeof(info));
  info.cbSize = sizeof(info);
  GetComboBoxInfo(hwnd, &info);
  GetClientRect(hwnd, &rc);

  old_font = (HFONT)SelectObject(dc, theme_window_font(hwnd));
  fill = CreateSolidBrush(theme_palette_button(state, enabled));
  frame = CreatePen(PS_SOLID, 1, theme_palette_border());
  if ((fill == NULL) || (frame == NULL)) {
    goto cleanup;
  }
  old_brush = (HBRUSH)SelectObject(dc, fill);
  old_pen = (HPEN)SelectObject(dc, frame);
  Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom); // the frame and the face at once

  if (cur != CB_ERR) {
    wchar_t line[combo_line_max];
    int const len = (int)SendMessageW(hwnd, CB_GETLBTEXTLEN, (WPARAM)cur, 0);

    if (len >= 0 && (size_t)len < (sizeof(line) / sizeof(line[0]))) {
      memset(line, 0, sizeof(line));
      SendMessageW(hwnd, CB_GETLBTEXT, (WPARAM)cur, (LPARAM)line);
      box = info.rcItem;
      InflateRect(&box, -layout_scale(combo_text_inset, (UINT)theme_window_dpi(hwnd)), 0);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, caption);
      DrawTextW(dc, line, -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      draw_focus_cue(dc, hwnd, &box);
    }
  }

  {
    // the arrow is the height of the button divided, as wide as it is tall, and sits in
    // the middle of the button the control keeps for it
    LONG const height = (info.rcButton.bottom - info.rcButton.top) / combo_arrow_height_parts;
    LONG const half = height / combo_arrow_width_parts;
    LONG const cx = (info.rcButton.left + info.rcButton.right) / 2;
    LONG const cy = (info.rcButton.top + info.rcButton.bottom) / 2;
    POINT const triangle[3] = {
        {cx - half, cy - half},
        {cx + half, cy - half},
        {cx, cy + half},
    };

    arrow = CreateSolidBrush(caption);
    glyph = CreatePen(PS_SOLID, 1, caption);
    if ((arrow != NULL) && (glyph != NULL)) {
      HBRUSH const saved_brush = (HBRUSH)SelectObject(dc, arrow);
      HPEN const saved_pen = (HPEN)SelectObject(dc, glyph);

      Polygon(dc, triangle, 3);
      SelectObject(dc, saved_brush);
      SelectObject(dc, saved_pen);
    }
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
  if (glyph != NULL) {
    DeleteObject(glyph);
    glyph = NULL;
  }
  if (frame != NULL) {
    DeleteObject(frame);
    frame = NULL;
  }
  if (arrow != NULL) {
    DeleteObject(arrow);
    arrow = NULL;
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
 * @brief Paint one combo box in the palette of the theme
 *
 * @param hwnd the combo box to paint
 * @param hot the hover state the subclass kept
 */
static void combo_paint(HWND const hwnd, BOOL const hot) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);

  if (dc != NULL) {
    combo_paint_face(dc, hwnd, hot);
    EndPaint(hwnd, &ps);
  }
}

/**
 * @brief The subclass that paints the frame of the drop down list of a combo box
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @param id the subclass to answer for
 * @param ref the paint state of the list, this component keeps none
 * @return the answer of the default procedure when the message was not consumed
 */
static LRESULT CALLBACK
list_subclass(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, UINT_PTR const id, DWORD_PTR const ref);

/**
 * @brief Give the drop down list of a combo box the theme class of the palette
 *
 * The list is a window of the system the control makes for itself, and it comes into the
 * world without the theme class of its maker.  It is the one part of the combo box the
 * explorer theme draws dark on its own, so the class is all it needs.  The frame of the
 * list, which the theme of the system leaves light, is handed to the subclass here.
 *
 * @param combo the combo box whose list is given the class
 */
static void theme_combo_list(HWND const combo) {
  COMBOBOXINFO info;

  memset(&info, 0, sizeof(info));
  info.cbSize = sizeof(info);
  if (!GetComboBoxInfo(combo, &info) || (info.hwndList == NULL)) {
    return;
  }
  SetWindowTheme(info.hwndList, L"DarkMode_Explorer", NULL);
  {
    DWORD_PTR existing = 0;

    if (!(GetWindowSubclass(info.hwndList, list_subclass, 1, &existing) && existing != 0)) {
      // the paint state of the list is the combo box it belongs to
      SetWindowSubclass(info.hwndList, list_subclass, 1, (DWORD_PTR)combo);
    }
  }
}

/**
 * @brief Paint the frame of the drop down list of a combo box
 *
 * The frame of the list is the border of the classic frame, and the theme of the system
 * leaves it light in the dark palette, which is the white line that runs around the dark
 * list.  The theme paints only the client, so the frame is drawn here, over the border
 * the control painted, in the colour of a frame line of the palette.
 *
 * @param dc the context to draw in
 * @param width the width of the whole window, the frame included
 * @param height the height of the whole window, the frame included
 */
static void list_paint_frame(HDC const dc, LONG const width, LONG const height) {
  HPEN pen = NULL;
  HPEN old_pen = NULL;
  HBRUSH old_brush = NULL;

  if ((dc == NULL) || (width <= 0) || (height <= 0)) {
    return;
  }
  pen = CreatePen(PS_SOLID, 1, theme_palette_border());
  if (pen == NULL) {
    return;
  }
  old_pen = (HPEN)SelectObject(dc, pen);
  old_brush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, 0, 0, width, height);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(pen);
}

/**
 * @brief Paint the rows of the drop down list of a combo box
 *
 * The rows of the list are painted from the theme of the system, which leaves them white
 * in the dark palette: only the row the reader chose follows the system and takes the
 * colour of the selection of Windows.  The rows are therefore painted here, in the colour
 * of the face of the palette, and the chosen row in the colour of a selection, the way the
 * list of the shell does.  Only the rows the list shows are painted, the first of them is
 * asked of the list, so a list that scrolled keeps its scroll bar out of the painting.
 *
 * @param list the list window to paint
 * @param combo the combo box the list belongs to, the font and the row height are its
 * @param dc the context to draw in
 */
static void list_paint_rows(HWND const list, HWND const combo, HDC const dc) {
  RECT rc = {0, 0, 0, 0};
  int const count = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
  int const cur = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
  int const height = (int)SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0);
  int const first = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
  int const rows = (count > first) ? (count - first) : 0;
  int shown = 0;
  int i = 0;
  HBRUSH face = NULL;
  HBRUSH chosen = NULL;
  HFONT old_font = NULL;

  if ((dc == NULL) || (count <= 0) || (height <= 0) || (rows <= 0) || (first < 0)) {
    return;
  }
  GetClientRect(list, &rc);
  shown = rows;
  if (shown > ((rc.bottom - rc.top) / height)) {
    shown = (rc.bottom - rc.top) / height;
  }
  face = CreateSolidBrush(theme_palette_control());
  chosen = CreateSolidBrush(GetSysColor(COLOR_HIGHLIGHT));
  if ((face == NULL) || (chosen == NULL)) {
    goto cleanup;
  }
  old_font = (HFONT)SelectObject(dc, theme_window_font(combo));
  for (i = 0; i < shown; i++) {
    RECT row = {0, 0, 0, 0};
    wchar_t text[combo_line_max];
    int const item = first + i;
    int const len = (int)SendMessageW(list, LB_GETTEXTLEN, (WPARAM)item, 0);
    bool const is_chosen = (item == cur);

    if (SendMessageW(list, LB_GETITEMRECT, (WPARAM)item, (LPARAM)&row) == LB_ERR) {
      break;
    }
    if (len < 0 || (size_t)len >= (sizeof(text) / sizeof(text[0]))) {
      continue;
    }
    memset(text, 0, sizeof(text));
    SendMessageW(list, LB_GETTEXT, (WPARAM)item, (LPARAM)text);
    FillRect(dc, &row, is_chosen ? chosen : face);
    InflateRect(&row, -layout_scale(combo_text_inset, (UINT)theme_window_dpi(list)), 0);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, is_chosen ? GetSysColor(COLOR_HIGHLIGHTTEXT) : theme_palette_text());
    DrawTextW(dc, text, -1, &row, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  }

cleanup:
  if (old_font != NULL) {
    SelectObject(dc, old_font);
    old_font = NULL;
  }
  if (chosen != NULL) {
    DeleteObject(chosen);
    chosen = NULL;
  }
  if (face != NULL) {
    DeleteObject(face);
    face = NULL;
  }
}

/**
 * @brief The subclass that paints the drop down list of a combo box
 *
 * The list paints itself from the theme of the system, which leaves its rows white and its
 * frame light in the dark palette, so the painting is taken over: the rows, the frame and
 * the client the reader prints are all answered here.  The paint state of the list is the
 * combo box it belongs to, because the font and the height of a row are the combo's.
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @param id the subclass to answer for
 * @param ref the combo box the list belongs to
 * @return the answer of the default procedure when the message was not consumed
 */
static LRESULT CALLBACK
list_subclass(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, UINT_PTR const id, DWORD_PTR const ref) {
  switch (msg) {
  case WM_NCDESTROY:
    RemoveWindowSubclass(hwnd, list_subclass, id);
    break;
  case WM_ERASEBKGND: {
    // the area the list erases is painted in the colour of the list, so the part an animation
    // shows first is the palette and not the white of the class
    RECT rc = {0, 0, 0, 0};
    HBRUSH const brush = CreateSolidBrush(theme_palette_control());

    if (brush != NULL) {
      GetClientRect(hwnd, &rc);
      FillRect((HDC)wparam, &rc, brush);
      DeleteObject(brush);
    }
    return 1;
  }
  case WM_NCPAINT: {
    // the control draws its own border first, the frame of the palette goes over it
    LRESULT const result = DefSubclassProc(hwnd, msg, wparam, lparam);
    HDC dc = GetWindowDC(hwnd);

    if (dc != NULL) {
      RECT rc;

      GetWindowRect(hwnd, &rc);
      list_paint_frame(dc, rc.right - rc.left, rc.bottom - rc.top);
      ReleaseDC(hwnd, dc);
    }
    return result;
  }
  case WM_PAINT: {
    // the rows are painted into the context of the control, the way the list paints them
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    if (dc != NULL) {
      list_paint_rows(hwnd, (HWND)(void *)ref, dc);
      EndPaint(hwnd, &ps);
    }
    return 0;
  }
  case WM_PRINTCLIENT: {
    // the rows are read back through WM_PRINTCLIENT, which paints into a context the
    // caller holds, so the reader never depends on the list being on the screen
    if ((lparam & PRF_CLIENT) != 0) {
      list_paint_rows(hwnd, (HWND)(void *)ref, (HDC)wparam);
    }
    return 0;
  }
  case WM_PRINT: {
    // the frame and the rows are painted into the context the caller holds.  The context
    // is the first parameter and the flags are the second one, the way the message is made
    LRESULT const result = DefSubclassProc(hwnd, msg, wparam, lparam);

    if ((lparam & PRF_NONCLIENT) != 0) {
      RECT rc;

      GetWindowRect(hwnd, &rc);
      list_paint_frame((HDC)wparam, rc.right - rc.left, rc.bottom - rc.top);
    }
    if ((lparam & PRF_CLIENT) != 0) {
      list_paint_rows(hwnd, (HWND)(void *)ref, (HDC)wparam);
    }
    return result;
  }
  default:
    break;
  }
  return DefSubclassProc(hwnd, msg, wparam, lparam);
}

/**
 * @brief The subclass that paints a drop down list combo box and keeps its hover state
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @param id the subclass to answer for
 * @param ref the paint state of the combo box
 * @return the answer of the default procedure when the message was not consumed
 */
static LRESULT CALLBACK
combo_subclass(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, UINT_PTR const id, DWORD_PTR const ref) {
  struct combo_paint *paint = (struct combo_paint *)(void *)ref;

  switch (msg) {
  case WM_NCDESTROY:
    RemoveWindowSubclass(hwnd, combo_subclass, id);
    combo_paint_destroy(paint);
    return DefSubclassProc(hwnd, msg, wparam, lparam);
  case WM_ERASEBKGND:
    return 1;
  case WM_MOUSEMOVE:
    if ((paint != NULL) && !paint->hot) {
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
  case WM_ENABLE: {
    LRESULT const result = DefSubclassProc(hwnd, msg, wparam, lparam);

    if (paint != NULL) {
      paint->hot = FALSE;
    }
    InvalidateRect(hwnd, NULL, FALSE);
    return result;
  }
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
  case WM_CAPTURECHANGED:
    InvalidateRect(hwnd, NULL, FALSE);
    break;
  case WM_THEMECHANGED:
    // the list can be made anew with the theme, and the new one comes without the class
    theme_combo_list(hwnd);
    break;
  case WM_PAINT:
    combo_paint(hwnd, (paint != NULL) ? paint->hot : FALSE);
    return 0;
  case WM_PRINTCLIENT:
    combo_paint_face((HDC)wparam, hwnd, (paint != NULL) ? paint->hot : FALSE);
    return 0;
  default:
    break;
  }
  return DefSubclassProc(hwnd, msg, wparam, lparam);
}

/**
 * @brief Make sure one combo box is painted by this component
 *
 * @param combo the combo box to subclass
 * @param err receives the failure of the allocation or of the subclass
 * @return the paint state of the combo box, NULL when it could not be subclassed
 */
static struct combo_paint *ensure_subclass(HWND const combo, struct ov_error *const err) {
  struct combo_paint *paint = NULL;
  DWORD_PTR existing = 0;
  bool success = false;

  if (GetWindowSubclass(combo, combo_subclass, 1, &existing) && existing != 0) {
    return (struct combo_paint *)(void *)existing;
  }
  if (!OV_REALLOC(&paint, 1, sizeof(*paint))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  paint->hot = FALSE;
  if (!SetWindowSubclass(combo, combo_subclass, 1, (DWORD_PTR)(void *)paint)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    OV_FREE(&paint);
    paint = NULL;
  }
  return paint;
}

/**
 * @brief Give a combo box back to the theme of the system
 *
 * @param combo the combo box to detach from, NULL is allowed
 */
static void theme_combo_detach(HWND const combo) {
  DWORD_PTR existing = 0;

  if (combo == NULL) {
    return;
  }
  if (GetWindowSubclass(combo, combo_subclass, 1, &existing) && existing != 0) {
    struct combo_paint *paint = (struct combo_paint *)(void *)existing;

    RemoveWindowSubclass(combo, combo_subclass, 1);
    combo_paint_destroy(paint);
  }
  {
    COMBOBOXINFO info;

    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (GetComboBoxInfo(combo, &info) && (info.hwndList != NULL)) {
      DWORD_PTR listed = 0;

      if (GetWindowSubclass(info.hwndList, list_subclass, 1, &listed)) {
        RemoveWindowSubclass(info.hwndList, list_subclass, 1);
      }
      SetWindowTheme(info.hwndList, NULL, NULL);
    }
  }
}

/**
 * @brief Attach the theme to a drop down list combo box
 *
 * The combo box stays usable when the paint state cannot be made, only the field stays
 * light then.
 *
 * @param combo the window to attach to, NULL is allowed
 */
void theme_combo_attach(HWND const combo) {
  struct ov_error err = {0};
  LONG_PTR style = 0;

  if (combo == NULL) {
    return;
  }
  if (!theme_state_dark()) {
    theme_combo_detach(combo);
    return;
  }
  style = GetWindowLongPtrW(combo, GWL_STYLE);
  if ((style & 0x3) != CBS_DROPDOWNLIST) {
    // another style has an edit of its own, which the palette answers through
    // theme_message(), and no field this component knows how to paint
    return;
  }
  if (ensure_subclass(combo, &err) == NULL) {
    OV_ERROR_REPORT(&err, NULL);
    return;
  }
  theme_combo_list(combo);
}
