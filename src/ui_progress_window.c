/**
 * The window the run reports into.
 *
 * The stage label, the progress bar under it and the button that asks the run to stop.
 * The window is made for one run: it holds its owner while the run is going, the cancel
 * ask goes up once and the ways to raise it go grey at the same moment.  The window is
 * self-contained -- its geometry, its class and its theme live here -- so a test can make
 * it against any window of its own and drive the cancel from the outside.
 */
#include "ui_progress_window.h"

#include "theme.h"

#include <commctrl.h>

#include <ovmo.h>
#include <ovprintf.h>

#include "layout.h"
#include "repair.h"

#define IDC_PROGRESS_STATUS 12
#define IDC_PROGRESS_BAR 13
#define IDC_PROGRESS_CANCEL 14

/** @brief The room of one line the run reports */
enum { ADC_PROGRESS_TEXT_CHARS = 256 };

#define LX(r) ((r).left)
#define LY(r) ((r).top)
#define LW(r) ((r).right - (r).left)
#define LH(r) ((r).bottom - (r).top)

static wchar_t const ph[] = L"%1$s";

/** @brief The line the window shows: the stage of the run and its step */
static wchar_t const line_ph[] = L"%1$s (%2$d/%3$d)";

/** @brief The styles of the window: a frame it is not resized with */
static DWORD const progress_window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
static DWORD const progress_window_extra = WS_EX_DLGMODALFRAME;

/**
 * @brief The window and the three controls it is made of
 *
 * The flag to cancel is not part of the window: the caller owns it, the window only
 * writes it.  The caller hands it to the run, so the run and the window share the one
 * flag without knowing one another.
 */
struct progress_window {
  HWND hwnd;
  HWND status;
  HWND bar;
  HWND cancel_btn;
  int dpi;
  HFONT font;                              // the font of this window, deleted with it
  wchar_t widest[ADC_PROGRESS_TEXT_CHARS]; // the line the run may report that needs the most room
  bool *cancel;                            // the ask to stop, written by the window thread, read by the run
};

size_t ui_progress_window_steps(bool const wait_first) { return REPAIR_STEPS_TOTAL + (wait_first ? 1 : 0); }
/**
 * @brief Raise the abort flag of the run and grey the ways out
 *
 * The flag goes up once and the ways to raise it go grey at the same moment: a reader who
 * pressed the button sees that the run heard it, and a second press has nothing left to
 * press.  The run reads the flag between its steps and quits, it is never interrupted in
 * the middle of one.
 *
 * @param pw the progress window that asks
 */
static void progress_cancel(struct progress_window *const pw) {
  if (*pw->cancel) {
    return; // the ask is already on its way, the greyed button says so
  }
  *pw->cancel = true;
  EnableWindow(pw->cancel_btn, FALSE);
  {
    wchar_t buf[256];

    OV_SNPRINTF(buf, sizeof(buf) / sizeof(WCHAR), ph, ph, gettext("Aborting..."));
    SetWindowTextW(pw->status, buf);
  }
}

/**
 * @brief Place the controls of the progress window
 *
 * The geometry itself lives in layout_progress_compute(), this only hands the rectangles
 * to the window manager.
 *
 * @param pw the progress window to place
 */
static void progress_layout(struct progress_window *const pw) {
  struct progress_layout g;
  RECT rc;

  GetClientRect(pw->hwnd, &rc);
  layout_progress_compute(rc.right - rc.left, (UINT)pw->dpi, &g);
  MoveWindow(pw->status, LX(g.label), LY(g.label), LW(g.label), LH(g.label), TRUE);
  MoveWindow(pw->bar, LX(g.bar), LY(g.bar), LW(g.bar), LH(g.bar), TRUE);
  MoveWindow(pw->cancel_btn, LX(g.cancel), LY(g.cancel), LW(g.cancel), LH(g.cancel), TRUE);
}

/**
 * @brief The room the widest line of the run takes in the font of the window
 *
 * @param pw the progress window that draws the line
 * @return the width in pixels, 0 when it could not be measured
 */
static int progress_text_width(struct progress_window const *const pw) {
  HDC dc = GetDC(pw->hwnd);
  HFONT old = NULL;
  SIZE size = {0, 0};
  int width = 0;

  if (dc == NULL) {
    return 0;
  }
  if (pw->font != NULL) {
    old = (HFONT)SelectObject(dc, pw->font);
  }
  if (GetTextExtentPoint32W(dc, pw->widest, (int)wcslen(pw->widest), &size)) {
    width = size.cx;
  }
  if (old != NULL) {
    SelectObject(dc, old);
  }
  ReleaseDC(pw->hwnd, dc);
  return width;
}

/**
 * @brief Put the progress window at the size its layout asks for
 *
 * The only place that changes the size of the window.  The width is the least width of the
 * dialog or the room the widest line of the run needs, whichever is wider: no report of a
 * run is cut off.
 *
 * @param pw the progress window to size
 */
static void progress_apply_size(struct progress_window *const pw) {
  struct progress_layout g;
  RECT outer;
  RECT now;
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, (UINT)pw->dpi);
  int const least = layout_scale(ADC_LAYOUT_PROGRESS_W, (UINT)pw->dpi);
  int const needed = progress_text_width(pw) + 2 * margin;
  int const width = (needed > least) ? needed : least;

  layout_progress_compute(width, (UINT)pw->dpi, &g);
  layout_window_rect(width, g.needed_height, (UINT)pw->dpi, progress_window_style, progress_window_extra, &outer);
  GetWindowRect(pw->hwnd, &now);
  if (((now.right - now.left) == (outer.right - outer.left)) && ((now.bottom - now.top) == (outer.bottom - outer.top))) {
    return;
  }
  SetWindowPos(pw->hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/**
 * @brief Bring the progress window onto a font of one dpi
 *
 * The window owns its font: the font of another window is deleted when that window moves
 * on to a monitor of its own dpi.
 *
 * @param pw  the progress window to draw with
 * @param dpi the dpi to draw for
 */
static void progress_apply_font(struct progress_window *const pw, int const dpi) {
  HFONT stale = pw->font;
  HFONT font = layout_make_font((UINT)dpi);

  pw->dpi = dpi;
  if (font == NULL) {
    return; // the geometry follows the monitor, the window keeps the font it has
  }
  pw->font = font;
  SendMessageW(pw->status, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(pw->cancel_btn, WM_SETFONT, (WPARAM)font, TRUE);
  if (stale != NULL) {
    DeleteObject(stale);
    stale = NULL;
  }
}

/**
 * @brief The window procedure of the progress window
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @return the answer the message carries
 */
static LRESULT CALLBACK progress_proc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct progress_window *const pw = (struct progress_window *)(INT_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

  switch (msg) {
  case WM_NCCREATE:
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW const *)lparam)->lpCreateParams);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  case WM_DESTROY: {
    struct progress_window *dead = (struct progress_window *)(INT_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (dead != NULL) {
      if (dead->font != NULL) {
        DeleteObject(dead->font);
        dead->font = NULL;
      }
      OV_FREE(&dead);
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return 0;
  }
  case WM_DPICHANGED: {
    RECT const *const rc = (RECT const *)(void *)lparam;

    /* the monitor suggests where the window goes; how big it is, the layout says */
    SetWindowPos(hwnd, NULL, rc->left, rc->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    progress_apply_font(pw, (int)LOWORD(wparam));
    progress_apply_size(pw);
    progress_layout(pw);
    return 0;
  }
  case WM_SIZE:
    progress_layout(pw);
    return 0;
  case WM_COMMAND:
    if ((HIWORD(wparam) == BN_CLICKED) && (LOWORD(wparam) == (WORD)IDC_PROGRESS_CANCEL)) {
      progress_cancel(pw);
    }
    return 0;
  case WM_CLOSE:
    progress_cancel(pw);
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
 * @brief Register the window class of the progress window
 *
 * The class is registered once per process, the way the main window does it: a second
 * window in the same process reuses the registration.
 *
 * @param err receives the failure of the registration
 * @return false when the class could not be registered
 */
static bool progress_register_class(struct ov_error *const err) {
  static ATOM atom = 0;

  if (atom != 0) {
    return true;
  }
  {
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = progress_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = theme_class_brush();
    wc.lpszClassName = L"audient_device_closer_progress";
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    atom = RegisterClassExW(&wc);
  }
  if (atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

bool ui_progress_window_create(HWND const owner,
                               int const dpi,
                               size_t const steps,
                               bool *const cancel,
                               char const *const first_text,
                               char const *const widest_text,
                               HWND *const out,
                               struct ov_error *const err) {
  struct progress_window *pw = NULL;
  wchar_t title[128];
  wchar_t text[ADC_PROGRESS_TEXT_CHARS];
  wchar_t cancel_text[128];
  char const *const fit = (widest_text != NULL) ? widest_text : first_text;
  RECT owner_rc = {0, 0, 0, 0};
  RECT outer = {0, 0, 0, 0};
  int width = 0;
  int height = 0;
  bool success = false;

  if ((owner == NULL) || (cancel == NULL) || (out == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  *out = NULL;
  if (!progress_register_class(err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  OV_SNPRINTF(title, sizeof(title) / sizeof(WCHAR), ph, ph, gettext("Processing"));
  if (first_text != NULL) {
    OV_SNPRINTF(text, sizeof(text) / sizeof(WCHAR), ph, ph, first_text);
  } else {
    OV_SNPRINTF(text, sizeof(text) / sizeof(WCHAR), ph, ph, gettext("Closing the device..."));
  }
  OV_SNPRINTF(cancel_text, sizeof(cancel_text) / sizeof(WCHAR), ph, ph, gettext("Abort"));
  if (!OV_REALLOC(&pw, 1, sizeof(*pw))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  memset(pw, 0, sizeof(*pw));
  pw->dpi = dpi;
  pw->cancel = cancel;
  GetWindowRect(owner, &owner_rc);
  {
    struct progress_layout g;

    width = layout_scale(ADC_LAYOUT_PROGRESS_W, (UINT)dpi);
    layout_progress_compute(width, (UINT)dpi, &g);
    layout_window_rect(width, g.needed_height, (UINT)dpi, progress_window_style, progress_window_extra, &outer);
    width = outer.right - outer.left;
    height = outer.bottom - outer.top;
  }
  pw->hwnd = CreateWindowExW(progress_window_extra,
                             L"audient_device_closer_progress",
                             title,
                             progress_window_style,
                             owner_rc.left + ((owner_rc.right - owner_rc.left) - width) / 2,
                             owner_rc.top + ((owner_rc.bottom - owner_rc.top) - height) / 2,
                             width,
                             height,
                             owner,
                             NULL,
                             GetModuleHandleW(NULL),
                             pw);
  if (pw->hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  pw->status = CreateWindowExW(
      0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, pw->hwnd, (HMENU)(INT_PTR)IDC_PROGRESS_STATUS, NULL, NULL);
  pw->bar = CreateWindowExW(
      0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, pw->hwnd, (HMENU)(INT_PTR)IDC_PROGRESS_BAR, NULL, NULL);
  pw->cancel_btn = CreateWindowExW(0,
                                   L"BUTTON",
                                   (wchar_t const *)cancel_text,
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                   0,
                                   0,
                                   0,
                                   0,
                                   pw->hwnd,
                                   (HMENU)(INT_PTR)IDC_PROGRESS_CANCEL,
                                   NULL,
                                   NULL);
  if ((pw->status == NULL) || (pw->bar == NULL) || (pw->cancel_btn == NULL)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  theme_attach(pw->hwnd);
  theme_attach(pw->status);
  theme_attach(pw->bar);
  theme_attach(pw->cancel_btn);
  progress_apply_font(pw, dpi);
  memset(pw->widest, 0, sizeof(pw->widest));
  OV_SNPRINTF(pw->widest,
              sizeof(pw->widest) / sizeof(WCHAR),
              line_ph,
              line_ph,
              (fit != NULL) ? fit : gettext("Closing the device..."),
              (int)steps,
              (int)steps);
  progress_apply_size(pw);
  SendMessageW(pw->bar, PBM_SETRANGE, 0, MAKELPARAM(0, (short)steps));
  SendMessageW(pw->bar, PBM_SETSTEP, 1, 0);
  progress_layout(pw);
  EnableWindow(owner, FALSE); // the dialog holds its owner, the run owns both
  ShowWindow(pw->hwnd, SW_SHOW);
  UpdateWindow(pw->hwnd);
  *out = pw->hwnd;
  success = true;

cleanup:
  if (!success) {
    if (pw->hwnd != NULL) {
      DestroyWindow(pw->hwnd);
    } else {
      OV_FREE(&pw);
    }
  }
  return success;
}

void ui_progress_window_destroy(HWND *const progress) {
  struct progress_window *pw = NULL;
  HWND hwnd = NULL;

  if ((progress == NULL) || (*progress == NULL)) {
    return;
  }
  hwnd = *progress;
  *progress = NULL;
  pw = (struct progress_window *)(INT_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (pw != NULL) {
    HWND const owner = GetWindow(hwnd, GW_OWNER);

    if (owner != NULL) {
      EnableWindow(owner, TRUE); // never leave the owner locked
      SetActiveWindow(owner);    // the focus the window held goes back to the owner
    }
  }
  DestroyWindow(hwnd);
}

void ui_progress_window_report(HWND const progress, size_t const step, char const *const text, size_t const steps) {
  struct progress_window *const pw = (struct progress_window *)(INT_PTR)GetWindowLongPtrW(progress, GWLP_USERDATA);
  wchar_t buf[256];

  if (pw == NULL) {
    return;
  }
  OV_SNPRINTF(buf, sizeof(buf) / sizeof(WCHAR), line_ph, line_ph, text, (int)step, (int)steps);
  SetWindowTextW(pw->status, buf);
  SendMessageW(pw->bar, PBM_SETPOS, (WPARAM)step, 0);
}
