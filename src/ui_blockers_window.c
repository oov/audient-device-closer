/**
 * The window that names the processes that hold the device.
 *
 * The veto records are shown in a window of their own: the main window carries the state
 * and the settings, and a run that was vetoed opens this one over it.  The window is
 * self-contained -- its geometry, its columns and its theme live here -- so a test can
 * make it against any window of its own and read the list.
 */
#include "ui_blockers_window.h"

#include "theme.h"

#include <commctrl.h>

#include <ovmo.h>
#include <ovprintf.h>

#include "layout.h"
#include "veto_event.h"

#define IDC_BLOCKERS_LABEL 3
#define IDC_BLOCKERS_LIST 4

#define LX(r) ((r).left)
#define LY(r) ((r).top)
#define LW(r) ((r).right - (r).left)
#define LH(r) ((r).bottom - (r).top)

static wchar_t const ph[] = L"%1$s";

/** @brief The name the window keeps its state under */
static wchar_t const state_prop[] = L"audient_device_closer_blockers_state";

/**
 * @brief The geometry of the blockers window, in logical units
 *
 * Unlike the main window this one is resizable: the list is the point of the window, and
 * the reader makes it larger to read a long path.  The height below is the one it opens
 * with; the list takes whatever the size leaves.
 */
enum blockers_layout_size {
  ADC_BLOCKERS_W = 560, /* initial client size of the window */
  ADC_BLOCKERS_H = 320,
  ADC_BLOCKERS_MIN_W = 320, /* the smallest client area the list stays readable in */
  ADC_BLOCKERS_MIN_H = 200,
  ADC_BLOCKERS_LABEL_H = 20, /* the sentence that names the outcome of the run */
  ADC_BLOCKERS_OK_H = 30,    /* the button that takes the window down */
  ADC_BLOCKERS_OK_W = 120,
  ADC_BLOCKERS_SCROLL_W = 17, /* width of the vertical scroll bar of the list */
  /* comctl32 grows the header only with the font height when the dpi changes, it does not
     scale it, so the height of the header is a layout value. */
  ADC_BLOCKERS_HEADER_H = 24,
};

/**
 * @brief The columns the list holds
 *
 * The order is PID, process, device: the pid and the image name of the same process are
 * read as one piece of information, and the device of the record is what one looks at last.
 */
enum { ADC_BLOCKERS_COLUMN_COUNT = 3 };

/**
 * @brief The shares the columns take of the usable list width
 *
 * They add up to the total below.  An absolute width would not follow the size of the
 * window, and the reader may resize this one.
 */
enum { ADC_BLOCKERS_COLUMN_TOTAL = 1000 };
static int const blockers_column_share[ADC_BLOCKERS_COLUMN_COUNT] = {100, 575, 325};

/**
 * @brief One rectangle of the window that shows the blocking processes
 *
 * The window holds the sentence that says why the close did not go through, the list of
 * the veto records under it and the button that takes it down.
 */
struct blockers_layout {
  RECT label;                            /* the sentence above the list */
  RECT list;                             /* the list view, under the label */
  RECT ok;                               /* the button that takes the window down */
  int column[ADC_BLOCKERS_COLUMN_COUNT]; /* widths of the list columns */
  int header_height;                     /* the header of the list view */
};

/**
 * @brief Compute the geometry of the window that shows the blocking processes
 *
 * The label sits at the top, the list fills the window under it and above the button.
 *
 * @param width  the client width of the window in pixels
 * @param height the client height of the window in pixels
 * @param dpi    the dpi of the window
 * @param out    receives the geometry
 */
static void blockers_compute(int const width, int const height, UINT const dpi, struct blockers_layout *const out) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
  int const ok_h = layout_scale(ADC_BLOCKERS_OK_H, dpi);
  int const label_bottom =
      layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi) + layout_scale(ADC_BLOCKERS_LABEL_H, dpi) + layout_scale(ADC_LAYOUT_GAP, dpi);
  int const list_bottom =
      height - (layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi) + layout_scale(ADC_BLOCKERS_OK_H, dpi) + layout_scale(ADC_LAYOUT_GAP, dpi));
  int const list_usable = width - 2 * margin - layout_scale(ADC_BLOCKERS_SCROLL_W, dpi);
  int used = 0;
  int i = 0;

  memset(out, 0, sizeof(*out));
  if ((width < 2 * margin) || (list_bottom < label_bottom)) {
    return;
  }
  out->label = layout_rect(margin, layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi), width - 2 * margin, layout_scale(ADC_BLOCKERS_LABEL_H, dpi));
  out->list = layout_rect(margin, label_bottom, width - 2 * margin, list_bottom - label_bottom);
  out->ok = layout_rect(width - margin - layout_scale(ADC_BLOCKERS_OK_W, dpi),
                        list_bottom + layout_scale(ADC_LAYOUT_GAP, dpi),
                        layout_scale(ADC_BLOCKERS_OK_W, dpi),
                        ok_h);
  out->header_height = layout_scale(ADC_BLOCKERS_HEADER_H, dpi);
  if (list_usable > 0) {
    for (i = 0; i < ADC_BLOCKERS_COLUMN_COUNT - 1; i++) {
      out->column[i] = MulDiv(list_usable, blockers_column_share[i], ADC_BLOCKERS_COLUMN_TOTAL);
      used += out->column[i];
    }
    out->column[ADC_BLOCKERS_COLUMN_COUNT - 1] = list_usable - used;
  }
}

/** @brief The styles of the window: the reader may resize it */
static DWORD const blockers_window_style = WS_OVERLAPPEDWINDOW;
static DWORD const blockers_window_extra = WS_EX_DLGMODALFRAME;

/**
 * @brief The window and the two controls it is made of
 *
 * The window owns its font and deletes it with the window: the font of the owner is
 * deleted when the owner moves on to a monitor of its own dpi.
 */
struct blockers_window {
  HWND hwnd;
  HWND label;
  HWND list;
  HWND ok_btn;
  int dpi;
  HFONT font;
  struct veto_event const *events; // borrowed, the records the list shows
  uint32_t const *closed;          // borrowed, the ids this run closed
  size_t closed_count;             // how many of them there are
  struct ov_error *err;            // borrowed, why the window could not be made
  bool failed;                     // the controls could not be made
};

/**
 * @brief Bring the blockers window onto a font of one dpi
 *
 * @param bw   the window to draw with
 * @param dpi  the dpi to draw for
 * @param err  receives the failure of the font or of one of the controls
 * @return false when the window could not be brought onto that font
 */
static NODISCARD bool apply_font(struct blockers_window *const bw, int const dpi, struct ov_error *const err) {
  HFONT stale = bw->font;
  HFONT font = layout_make_font((UINT)dpi);

  bw->dpi = dpi;
  if (font == NULL) {
    // the geometry follows the monitor, the window keeps the font it has
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    return false;
  }
  bw->font = font;
  SendMessageW(bw->label, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(bw->list, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(bw->ok_btn, WM_SETFONT, (WPARAM)font, TRUE);
  if ((stale != NULL) && !DeleteObject(stale)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

/**
 * @brief The room a text takes in the font of the window
 *
 * @param bw the window that draws the text
 * @param text the text to measure
 * @param err receives the failure of the measurement
 * @param out receives the width in pixels
 * @return false when the text could not be measured
 */
static NODISCARD bool
text_width(struct blockers_window const *const bw, wchar_t const *const text, int *const out, struct ov_error *const err) {
  HDC dc = NULL;
  HFONT old = NULL;
  SIZE size = {0, 0};
  bool success = false;

  *out = 0;
  dc = GetDC(bw->hwnd);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (bw->font != NULL) {
    old = (HFONT)SelectObject(dc, bw->font);
    if ((old == NULL) || (old == HGDI_ERROR)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
      old = NULL;
      goto cleanup;
    }
  }
  if (!GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  *out = size.cx;
  success = true;

cleanup:
  if (dc != NULL) {
    if ((old != NULL) && (SelectObject(dc, old) == NULL)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    if (!ReleaseDC(bw->hwnd, dc)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
  }
  return success;
}

/**
 * @brief Make the window wide enough for the sentence it opens with
 *
 * The reader may resize the window, but the sentence that says what happened has to be read
 * whole from the first moment, in every language and at every dpi.
 *
 * @param bw the window to widen when its sentence needs more room
 * @param sentence the text of the label
 * @param err receives the failure of the measurement or of the resize
 * @return false when the window could not be widened
 */
static NODISCARD bool fit(struct blockers_window *const bw, wchar_t const *const sentence, struct ov_error *const err) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, (UINT)bw->dpi);
  int const least = layout_scale(ADC_BLOCKERS_W, (UINT)bw->dpi);
  int width = 0;
  int client = 0;
  RECT outer;
  RECT now;

  if (!text_width(bw, sentence, &width, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  client = ((width + 2 * margin) > least) ? (width + 2 * margin) : least;
  layout_window_rect(
      client, layout_scale(ADC_BLOCKERS_H, (UINT)bw->dpi), (UINT)bw->dpi, blockers_window_style, blockers_window_extra, &outer);
  if (!GetWindowRect(bw->hwnd, &now)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if (((now.right - now.left) == (outer.right - outer.left)) && ((now.bottom - now.top) == (outer.bottom - outer.top))) {
    return true; // already wide enough for the sentence
  }
  if (!SetWindowPos(bw->hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

/**
 * @brief Place the controls of the blockers window
 *
 * The geometry itself lives in blockers_compute(), this only hands the rectangles
 * to the window manager.
 *
 * @param bw the blockers window to place
 * @param err receives the failure of the placement
 * @return false when a control could not be placed
 */
static NODISCARD bool layout(struct blockers_window *const bw, struct ov_error *const err) {
  struct blockers_layout g;
  RECT rc;

  if (!GetClientRect(bw->hwnd, &rc)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  blockers_compute(rc.right - rc.left, rc.bottom - rc.top, (UINT)bw->dpi, &g);
  if (!MoveWindow(bw->label, LX(g.label), LY(g.label), LW(g.label), LH(g.label), TRUE) ||
      !MoveWindow(bw->list, LX(g.list), LY(g.list), LW(g.list), LH(g.list), TRUE) ||
      !MoveWindow(bw->ok_btn, LX(g.ok), LY(g.ok), LW(g.ok), LH(g.ok), TRUE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  {
    int i = 0;

    for (i = 0; i < ADC_BLOCKERS_COLUMN_COUNT; i++) {
      SendMessageW(bw->list, LVM_SETCOLUMNWIDTH, (WPARAM)i, (LPARAM)g.column[i]);
    }
  }
  return true;
}

/**
 * @brief Whether this run closed the process the record names
 *
 * @param closed the ids this run closed
 * @param count how many there are
 * @param pid the id the record names
 * @return true when it is one of them
 */
static bool closed_by_this_run(uint32_t const *const closed, size_t const count, uint32_t const pid) {
  for (size_t i = 0; i < count; i++) {
    if (closed[i] == pid) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Fill the list with the processes that block the close
 *
 * The text of every record is copied by the list view into its own store, so the caller
 * may release the records as soon as this returns.
 *
 * @param bw the blockers window that shows them
 * @param events the records to show, NULL shows none
 * @param closed the ids this run closed, their records are gone with the process
 * @param closed_count how many there are
 * @param err receives the failure of the list
 * @return false when a row could not be put in the list
 */
static NODISCARD bool fill(struct blockers_window *const bw,
                           struct veto_event const *const events,
                           uint32_t const *const closed,
                           size_t const closed_count,
                           struct ov_error *const err) {
  struct veto_event_iter it = {0};
  int row = 0;
  bool success = false;
  // The columns take the text of the record as it stands; the list itself copies what it is
  // handed, so only the number has to be made into a text here.  The empty text a record
  // without one is shown with cannot be the literal L"": the column wants a writable text.
  wchar_t empty[1] = {L'\0'};

  if (SendMessageW(bw->list, LVM_DELETEALLITEMS, 0, 0) == (LRESULT)-1) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  while (veto_event_next(events, &it)) {
    wchar_t pid[32];
    LVITEMW item;

    if (closed_by_this_run(closed, closed_count, it.pid)) {
      continue;
    }

    OV_SNPRINTF(pid, sizeof(pid) / sizeof(pid[0]), L"%1$lu", L"%1$lu", (unsigned long)it.pid);

    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = 0;
    item.pszText = pid;
    row = (int)SendMessageW(bw->list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    if (row == -1) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    item.iSubItem = 1;
    item.pszText = (it.path != NULL) ? it.path : empty;
    if (!SendMessageW(bw->list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    item.iSubItem = 2;
    item.pszText = (it.device_instance != NULL) ? it.device_instance : empty;
    if (!SendMessageW(bw->list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }

    row++;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief The columns of the list
 *
 * The pid and the image name of a process are read as one piece of information, so the
 * device of the record is the last column.
 *
 * @param bw the blockers window that holds the list
 * @param err receives the failure of a column
 * @return false when a column could not be put in the list
 */
static NODISCARD bool add_columns(struct blockers_window *const bw, struct ov_error *const err) {
  LVCOLUMNW col;
  wchar_t titles[ADC_BLOCKERS_COLUMN_COUNT][64];
  int i = 0;

  OV_SNPRINTF(titles[0], sizeof(titles[0]) / sizeof(titles[0][0]), ph, ph, gettext("PID"));
  OV_SNPRINTF(titles[1], sizeof(titles[1]) / sizeof(titles[1][0]), ph, ph, gettext("Process"));
  OV_SNPRINTF(titles[2], sizeof(titles[2]) / sizeof(titles[2][0]), ph, ph, gettext("Blocked device"));
  memset(&col, 0, sizeof(col));
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  for (i = 0; i < ADC_BLOCKERS_COLUMN_COUNT; i++) {
    col.iSubItem = i;
    col.cx = 0; // layout() gives the real width
    col.pszText = titles[i];
    if (SendMessageW(bw->list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col) == (LRESULT)-1) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      return false;
    }
  }
  return true;
}

/**
 * @brief Make the controls of the blockers window and fill the list
 *
 * Called from WM_CREATE.  The caller makes the window and shows it; the controls, the
 * font, the theme, the columns and the rows are in place by then.
 *
 * @param bw the window to fill, its hwnd, dpi, events and closed ids are already set
 * @param err receives the reason a control could not be made
 * @return false when the window could not be filled, with the reason in err
 */
static bool build(struct blockers_window *const bw, struct ov_error *const err) {
  wchar_t label_text[256] = {0};
  wchar_t text[128] = {0};
  bool success = false;

  OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("The device cannot be closed because it is in use"));
  if (!SetWindowTextW(bw->hwnd, text)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  OV_SNPRINTF(label_text,
              sizeof(label_text) / sizeof(label_text[0]),
              ph,
              ph,
              gettext("The processes below have the device open, so the close was refused."));
  bw->label = CreateWindowExW(
      0, L"STATIC", label_text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, bw->hwnd, (HMENU)(INT_PTR)IDC_BLOCKERS_LABEL, NULL, NULL);
  bw->list = CreateWindowExW(0,
                             WC_LISTVIEWW,
                             L"",
                             WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                             0,
                             0,
                             0,
                             0,
                             bw->hwnd,
                             (HMENU)(INT_PTR)IDC_BLOCKERS_LIST,
                             NULL,
                             NULL);
  OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("OK"));
  bw->ok_btn = CreateWindowExW(
      0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, bw->hwnd, (HMENU)(INT_PTR)IDOK, NULL, NULL);
  if ((bw->label == NULL) || (bw->list == NULL) || (bw->ok_btn == NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  ListView_SetView(bw->list, LV_VIEW_DETAILS);
  SendMessageW(bw->list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_DOUBLEBUFFER, LVS_EX_DOUBLEBUFFER);
  if (!add_columns(bw, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!theme_attach(bw->hwnd, err) || !theme_attach(bw->label, err) || !theme_attach(bw->ok_btn, err) ||
      !theme_attach_list(bw->list, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!apply_font(bw, bw->dpi, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!fit(bw, label_text, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!fill(bw, bw->events, bw->closed, bw->closed_count, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!layout(bw, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief The window procedure of the blockers window
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @return the answer the message carries
 */
static LRESULT CALLBACK proc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct blockers_window *bw = (struct blockers_window *)GetPropW(hwnd, state_prop);

  switch (msg) {
  case WM_NCCREATE:
    if (SetPropW(hwnd, state_prop, (HANDLE)((CREATESTRUCTW const *)lparam)->lpCreateParams) == 0) {
      struct ov_error err = {0};

      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
      OV_ERROR_REPORT(&err, NULL);
      return FALSE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  case WM_CREATE:
    if (bw != NULL) {
      bw->hwnd = hwnd;
      if (!build(bw, bw->err)) {
        bw->failed = true;
      }
    }
    return 0;
  case WM_DESTROY: {
    HWND const owner = GetWindow(hwnd, GW_OWNER);

    if (owner != NULL) {
      EnableWindow(owner, TRUE); // never leave the owner locked
      SetActiveWindow(owner);    // the focus the popup held goes back to the owner
    }
    if (bw != NULL) {
      if (bw->font != NULL) {
        if (!DeleteObject(bw->font)) {
          struct ov_error err = {0};

          OV_ERROR_SET_GENERIC(&err, ov_error_generic_fail);
          OV_ERROR_REPORT(&err, NULL);
        }
        bw->font = NULL;
      }
      if (!RemovePropW(hwnd, state_prop)) {
        struct ov_error err = {0};

        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&err, NULL);
      }
      OV_FREE(&bw);
    }
    return 0;
  }
  case WM_GETMINMAXINFO: {
    MINMAXINFO *const mmi = (MINMAXINFO *)(void *)lparam;
    UINT const dpi = ((bw != NULL) && (bw->dpi > 0)) ? (UINT)bw->dpi : GetDpiForWindow(hwnd);
    RECT smallest;

    layout_window_rect(layout_scale(ADC_BLOCKERS_MIN_W, dpi),
                       layout_scale(ADC_BLOCKERS_MIN_H, dpi),
                       dpi,
                       blockers_window_style,
                       blockers_window_extra,
                       &smallest);
    mmi->ptMinTrackSize.x = smallest.right - smallest.left;
    mmi->ptMinTrackSize.y = smallest.bottom - smallest.top;
    return 0;
  }
  case WM_DPICHANGED: {
    RECT const *const rc = (RECT const *)(void *)lparam;
    struct ov_error err = {0};
    bool ok = false;

    /* the reader chose this size: the monitor's suggestion of it is kept, only the font
     * and the geometry follow the new dpi */
    if (!SetWindowPos(hwnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE)) {
      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
    } else if (!apply_font(bw, (int)LOWORD(wparam), &err)) {
      OV_ERROR_ADD_TRACE(&err);
    } else if (!layout(bw, &err)) {
      OV_ERROR_ADD_TRACE(&err);
    } else {
      ok = true;
    }
    if (!ok) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
  case WM_SIZE: {
    struct ov_error err = {0};

    if (!layout(bw, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
  case WM_COMMAND:
    // IsDialogMessageW sends IDCANCEL, with no notification code, for ESC
    if ((LOWORD(wparam) == (WORD)IDCANCEL) || ((LOWORD(wparam) == (WORD)IDOK) && (HIWORD(wparam) == BN_CLICKED))) {
      DestroyWindow(hwnd);
    }
    return 0;
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  default:
    break;
  }
  {
    LRESULT handled_result = 0;
    struct ov_error err = {0};
    ov_tribool const handled = theme_message(hwnd, msg, wparam, lparam, &handled_result, &err);

    if (handled == ov_indeterminate) {
      OV_ERROR_REPORT(&err, NULL);
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (handled == ov_true) {
      return handled_result;
    }
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/**
 * @brief Register the window class of the blockers window
 *
 * The class is registered once per process, the way the main window does it.
 *
 * @param err receives the failure of the registration
 * @return false when the class could not be registered
 */
static NODISCARD bool register_class(struct ov_error *const err) {
  static ATOM atom = 0;

  if (atom != 0) {
    return true;
  }
  {
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    // the system deletes the background brush of a class when the class is unregistered,
    // so the class gets a brush of its own
    wc.hbrBackground = theme_class_brush(err);
    if (wc.hbrBackground == NULL) {
      OV_ERROR_ADD_TRACE(err);
      return false;
    }
    wc.lpszClassName = L"audient_device_closer_blockers";
    atom = RegisterClassExW(&wc);
  }
  if (atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

NODISCARD bool ui_blockers_window_show(HWND const owner,
                                       struct veto_event const *const events,
                                       uint32_t const *const closed,
                                       size_t const closed_count,
                                       struct ov_error *const err) {
  struct blockers_window *bw = NULL;
  HWND window = NULL; // the handle of the window while the modal loop is on it
  bool success = false;

  if (owner == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  if (!register_class(err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!OV_REALLOC(&bw, 1, sizeof(*bw))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *bw = (struct blockers_window){0};
  bw->dpi = (int)GetDpiForWindow(owner);
  bw->events = events;
  bw->closed = closed;
  bw->closed_count = closed_count;
  bw->err = err;
  {
    RECT owner_rc = {0, 0, 0, 0};
    RECT outer;
    int width = 0;
    int height = 0;

    if (!GetWindowRect(owner, &owner_rc)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    layout_window_rect(layout_scale(ADC_BLOCKERS_W, (UINT)bw->dpi),
                       layout_scale(ADC_BLOCKERS_H, (UINT)bw->dpi),
                       (UINT)bw->dpi,
                       blockers_window_style,
                       blockers_window_extra,
                       &outer);
    width = outer.right - outer.left;
    height = outer.bottom - outer.top;
    bw->hwnd = CreateWindowExW(blockers_window_extra,
                               L"audient_device_closer_blockers",
                               L"",
                               blockers_window_style,
                               owner_rc.left + ((owner_rc.right - owner_rc.left) - width) / 2,
                               owner_rc.top + ((owner_rc.bottom - owner_rc.top) - height) / 2,
                               width,
                               height,
                               owner,
                               NULL,
                               GetModuleHandleW(NULL),
                               bw);
  }
  if (bw->hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (bw->failed) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  window = bw->hwnd;
  bw = NULL;
  EnableWindow(owner, FALSE); // the dialog holds its owner until the reader is done
  // ShowWindow answers the state the window was in and does not set the last error, so
  // there is nothing to read from it
  ShowWindow(window, SW_SHOW);
  if (!UpdateWindow(window)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  {
    MSG msg;

    while (IsWindow(window) && GetMessageW(&msg, NULL, 0, 0) > 0) {
      if (!IsDialogMessageW(window, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  }
  success = true;

cleanup:
  if (bw != NULL) {
    if (bw->hwnd != NULL) {
      // answers through WM_DESTROY, which frees the state
      if (!DestroyWindow(bw->hwnd)) {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
    } else {
      OV_FREE(&bw);
    }
  }
  return success;
}
