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

#define IDC_BLOCKERS_LABEL 3
#define IDC_BLOCKERS_LIST 4
#define IDC_BLOCKERS_OK 15

#define LX(r) ((r).left)
#define LY(r) ((r).top)
#define LW(r) ((r).right - (r).left)
#define LH(r) ((r).bottom - (r).top)

static wchar_t const ph[] = L"%1$s";

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
};

/**
 * @brief Bring the blockers window onto a font of one dpi
 *
 * @param bw   the window to draw with
 * @param dpi  the dpi to draw for
 */
static void blockers_apply_font(struct blockers_window *const bw, int const dpi) {
  HFONT stale = bw->font;
  HFONT font = layout_make_font((UINT)dpi);

  bw->dpi = dpi;
  if (font == NULL) {
    return; // the geometry follows the monitor, the window keeps the font it has
  }
  bw->font = font;
  SendMessageW(bw->label, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(bw->list, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(bw->ok_btn, WM_SETFONT, (WPARAM)font, TRUE);
  if (stale != NULL) {
    DeleteObject(stale);
    stale = NULL;
  }
}

/**
 * @brief The room a text takes in the font of the window
 *
 * @param bw the window that draws the text
 * @param text the text to measure
 * @return the width in pixels, 0 when it could not be measured
 */
static int blockers_text_width(struct blockers_window const *const bw, wchar_t const *const text) {
  HDC dc = GetDC(bw->hwnd);
  HFONT old = NULL;
  SIZE size = {0, 0};
  int width = 0;

  if (dc == NULL) {
    return 0;
  }
  if (bw->font != NULL) {
    old = (HFONT)SelectObject(dc, bw->font);
  }
  if (GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size)) {
    width = size.cx;
  }
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(bw->hwnd, dc);
  return width;
}

/**
 * @brief Make the window wide enough for the sentence it opens with
 *
 * The reader may resize the window, but the sentence that says what happened has to be read
 * whole from the first moment, in every language and at every dpi.
 *
 * @param bw the window to widen when its sentence needs more room
 * @param sentence the text of the label
 */
static void blockers_fit(struct blockers_window *const bw, wchar_t const *const sentence) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, (UINT)bw->dpi);
  int const least = layout_scale(ADC_LAYOUT_BLOCKERS_W, (UINT)bw->dpi);
  int const needed = blockers_text_width(bw, sentence) + 2 * margin;
  int const client = (needed > least) ? needed : least;
  RECT outer;
  RECT now;

  layout_window_rect(
      client, layout_scale(ADC_LAYOUT_BLOCKERS_H, (UINT)bw->dpi), (UINT)bw->dpi, blockers_window_style, blockers_window_extra, &outer);
  GetWindowRect(bw->hwnd, &now);
  if (((now.right - now.left) == (outer.right - outer.left)) && ((now.bottom - now.top) == (outer.bottom - outer.top))) {
    return;
  }
  SetWindowPos(bw->hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/**
 * @brief Place the controls of the blockers window
 *
 * The geometry itself lives in layout_blockers_compute(), this only hands the rectangles
 * to the window manager.
 *
 * @param bw the blockers window to place
 */
static void blockers_layout(struct blockers_window *const bw) {
  struct blockers_layout g;
  RECT rc;

  GetClientRect(bw->hwnd, &rc);
  layout_blockers_compute(rc.right - rc.left, rc.bottom - rc.top, (UINT)bw->dpi, &g);
  MoveWindow(bw->label, LX(g.label), LY(g.label), LW(g.label), LH(g.label), TRUE);
  MoveWindow(bw->list, LX(g.list), LY(g.list), LW(g.list), LH(g.list), TRUE);
  MoveWindow(bw->ok_btn, LX(g.ok), LY(g.ok), LW(g.ok), LH(g.ok), TRUE);
  {
    int i = 0;

    for (i = 0; i < ADC_LAYOUT_COLUMN_COUNT; i++) {
      SendMessageW(bw->list, LVM_SETCOLUMNWIDTH, (WPARAM)i, (LPARAM)g.column[i]);
    }
  }
}

/**
 * @brief Fill the list with the processes that block the close
 *
 * The text of every record is copied by the list view into its own store, so the caller
 * may release the records as soon as this returns.
 *
 * @param bw the blockers window that shows them
 * @param culprits the records to show, NULL shows none
 */
static void blockers_fill(struct blockers_window *const bw, struct culprit const *const culprits) {
  size_t const n = culprit_count(culprits);
  size_t i = 0;
  int row = 0;

  SendMessageW(bw->list, LVM_DELETEALLITEMS, 0, 0);
  for (i = 0; i < n; i++) {
    wchar_t pid[32];
    wchar_t dev[256];
    wchar_t path[256];
    LVITEMW item;

    if (culprits[i].pid == 0) {
      continue;
    }
    OV_SNPRINTF(pid, sizeof(pid) / sizeof(WCHAR), L"%1$lu", L"%1$lu", (unsigned long)culprits[i].pid);
    OV_SNPRINTF(dev, sizeof(dev) / sizeof(WCHAR), ph, ph, culprits[i].device_instance ? culprits[i].device_instance : "");
    OV_SNPRINTF(path, sizeof(path) / sizeof(WCHAR), ph, ph, culprits[i].path ? culprits[i].path : "");

    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = 0;
    item.pszText = pid;
    row = (int)SendMessageW(bw->list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    item.iSubItem = 1;
    item.pszText = path;
    SendMessageW(bw->list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item);
    item.iSubItem = 2;
    item.pszText = dev;
    SendMessageW(bw->list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item);

    row++;
  }
}

/**
 * @brief The columns of the list
 *
 * The pid and the image name of a process are read as one piece of information, so the
 * device of the record is the last column.
 *
 * @param bw the blockers window that holds the list
 */
static void blockers_add_columns(struct blockers_window *const bw) {
  LVCOLUMNW col;
  wchar_t titles[ADC_LAYOUT_COLUMN_COUNT][64];
  int i = 0;

  OV_SNPRINTF(titles[0], sizeof(titles[0]) / sizeof(WCHAR), ph, ph, gettext("PID"));
  OV_SNPRINTF(titles[1], sizeof(titles[1]) / sizeof(WCHAR), ph, ph, gettext("Process"));
  OV_SNPRINTF(titles[2], sizeof(titles[2]) / sizeof(WCHAR), ph, ph, gettext("Blocked device"));
  memset(&col, 0, sizeof(col));
  col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  for (i = 0; i < ADC_LAYOUT_COLUMN_COUNT; i++) {
    col.iSubItem = i;
    col.cx = 0; // blockers_layout() gives the real width
    col.pszText = titles[i];
    SendMessageW(bw->list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
  }
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
static LRESULT CALLBACK blockers_proc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct blockers_window *bw = (struct blockers_window *)(INT_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

  switch (msg) {
  case WM_NCCREATE:
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW const *)lparam)->lpCreateParams);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  case WM_DESTROY: {
    HWND const owner = GetWindow(hwnd, GW_OWNER);

    if (owner != NULL) {
      EnableWindow(owner, TRUE); // never leave the owner locked
      SetActiveWindow(owner);    // the focus the popup held goes back to the owner
    }
    if (bw != NULL) {
      if (bw->font != NULL) {
        DeleteObject(bw->font);
        bw->font = NULL;
      }
      OV_FREE(&bw);
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return 0;
  }
  case WM_GETMINMAXINFO: {
    MINMAXINFO *const mmi = (MINMAXINFO *)(void *)lparam;
    UINT const dpi = (bw != NULL && bw->dpi > 0) ? (UINT)bw->dpi : GetDpiForWindow(hwnd);
    RECT smallest;

    layout_window_rect(layout_scale(ADC_LAYOUT_BLOCKERS_MIN_W, dpi),
                       layout_scale(ADC_LAYOUT_BLOCKERS_MIN_H, dpi),
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

    /* the reader chose this size: the monitor's suggestion of it is kept, only the font
     * and the geometry follow the new dpi */
    SetWindowPos(hwnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
    blockers_apply_font(bw, (int)LOWORD(wparam));
    blockers_layout(bw);
    return 0;
  }
  case WM_SIZE:
    blockers_layout(bw);
    return 0;
  case WM_COMMAND:
    if ((HIWORD(wparam) == BN_CLICKED) && (LOWORD(wparam) == (WORD)IDC_BLOCKERS_OK)) {
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
    bool handled = false;

    if (!theme_message(hwnd, msg, wparam, lparam, &handled_result, &handled, NULL)) {
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (handled) {
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
static bool blockers_register_class(struct ov_error *const err) {
  static ATOM atom = 0;

  if (atom != 0) {
    return true;
  }
  {
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = blockers_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = theme_class_brush();
    wc.lpszClassName = L"audient_device_closer_blockers";
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    atom = RegisterClassExW(&wc);
  }
  if (atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

bool ui_blockers_window_show(HWND const owner, struct culprit const *const culprits, struct ov_error *const err) {
  struct blockers_window *bw = NULL;
  wchar_t title[128];
  wchar_t label_text[256];
  wchar_t ok_text[64];
  RECT owner_rc = {0, 0, 0, 0};
  int dpi = 0;
  int width = 0;
  int height = 0;
  HWND window = NULL; // the handle of the window while the modal loop is on it
  bool success = false;

  if (owner == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!blockers_register_class(err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  dpi = (int)GetDpiForWindow(owner);
  OV_SNPRINTF(title, sizeof(title) / sizeof(WCHAR), ph, ph, gettext("The device cannot be closed because it is in use"));
  OV_SNPRINTF(label_text,
              sizeof(label_text) / sizeof(WCHAR),
              ph,
              ph,
              gettext("The processes below have the device open, so the close was refused."));
  OV_SNPRINTF(ok_text, sizeof(ok_text) / sizeof(WCHAR), ph, ph, gettext("OK"));
  if (!OV_REALLOC(&bw, 1, sizeof(*bw))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  memset(bw, 0, sizeof(*bw));
  bw->dpi = dpi;
  GetWindowRect(owner, &owner_rc);
  {
    RECT outer;

    layout_window_rect(layout_scale(ADC_LAYOUT_BLOCKERS_W, (UINT)dpi),
                       layout_scale(ADC_LAYOUT_BLOCKERS_H, (UINT)dpi),
                       (UINT)dpi,
                       blockers_window_style,
                       blockers_window_extra,
                       &outer);
    width = outer.right - outer.left;
    height = outer.bottom - outer.top;
  }
  bw->hwnd = CreateWindowExW(blockers_window_extra,
                             L"audient_device_closer_blockers",
                             title,
                             blockers_window_style,
                             owner_rc.left + ((owner_rc.right - owner_rc.left) - width) / 2,
                             owner_rc.top + ((owner_rc.bottom - owner_rc.top) - height) / 2,
                             width,
                             height,
                             owner,
                             NULL,
                             GetModuleHandleW(NULL),
                             bw);
  if (bw->hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  bw->label = CreateWindowExW(0,
                              L"STATIC",
                              (wchar_t const *)label_text,
                              WS_CHILD | WS_VISIBLE | SS_LEFT,
                              0,
                              0,
                              0,
                              0,
                              bw->hwnd,
                              (HMENU)(INT_PTR)IDC_BLOCKERS_LABEL,
                              NULL,
                              NULL);
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
  bw->ok_btn = CreateWindowExW(0,
                               L"BUTTON",
                               (wchar_t const *)ok_text,
                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                               0,
                               0,
                               0,
                               0,
                               bw->hwnd,
                               (HMENU)(INT_PTR)IDC_BLOCKERS_OK,
                               NULL,
                               NULL);
  if ((bw->label == NULL) || (bw->list == NULL) || (bw->ok_btn == NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  ListView_SetView(bw->list, LV_VIEW_DETAILS);
  SendMessageW(bw->list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_DOUBLEBUFFER, LVS_EX_DOUBLEBUFFER);
  blockers_add_columns(bw);
  theme_attach(bw->hwnd);
  theme_attach(bw->label);
  theme_attach(bw->ok_btn);
  theme_attach_list(bw->list);
  blockers_apply_font(bw, dpi);
  blockers_fit(bw, label_text);
  blockers_fill(bw, culprits);
  blockers_layout(bw);
  EnableWindow(owner, FALSE); // the dialog holds its owner until the reader is done
  ShowWindow(bw->hwnd, SW_SHOW);
  UpdateWindow(bw->hwnd);
  window = bw->hwnd;
  bw = NULL;
  success = true;

cleanup:
  if ((bw != NULL) && (bw->hwnd != NULL)) {
    DestroyWindow(bw->hwnd); // answers through WM_DESTROY, which frees the state
  } else if (bw != NULL) {
    if (bw->font != NULL) {
      DeleteObject(bw->font);
      bw->font = NULL;
    }
    OV_FREE(&bw);
  }
  if (!success) {
    return false;
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
  return true;
}
