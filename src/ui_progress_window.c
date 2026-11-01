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

/**
 * @brief The geometry of the progress window, in logical units
 *
 * The least width the status line reads in, the bar of comctl32 at 96dpi and the button
 * that asks the run to stop.  The height of the window is no number here: the stack the
 * window holds decides it, see progress_compute().
 */
enum progress_layout_size {
  ADC_PROGRESS_W = 320,       /* the least width the status line reads in */
  ADC_PROGRESS_LABEL_H = 20,  /* the one line label that names the stage */
  ADC_PROGRESS_BAR_H = 23,    /* the progress bar of comctl32 at 96dpi */
  ADC_PROGRESS_CANCEL_H = 26, /* the button that asks the run to stop */
  ADC_PROGRESS_CANCEL_W = 120,
};

/**
 * @brief Every rectangle of the progress window for one width and dpi
 *
 * All values are physical pixels in the client coordinates of the progress window.  The
 * window holds the stage label and, under it, the progress bar, and nothing else.
 */
struct progress_layout {
  RECT label;        /* the text of the stage the run is at */
  RECT bar;          /* the progress bar under it */
  RECT cancel;       /* the button that asks the run to stop */
  int needed_height; /* the client height the stack needs, the window is made that tall */
};

/**
 * @brief Compute the geometry of the progress window
 *
 * The window is as wide as the room the widest line of the run needs, see apply_size, so
 * the width is the one the caller sized the window to rather than a constant.
 *
 * @param width the client width of the window in pixels
 * @param dpi   the dpi of the window
 * @param out   receives the geometry and the height it needs
 */
static void progress_compute(int const width, UINT const dpi, struct progress_layout *const out) {
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, dpi);
  int const usable = (width > 2 * margin) ? (width - 2 * margin) : 0;

  memset(out, 0, sizeof(*out));
  out->label = layout_rect(margin, layout_scale(ADC_LAYOUT_MARGIN_TOP, dpi), usable, layout_scale(ADC_PROGRESS_LABEL_H, dpi));
  out->bar = layout_rect(margin, out->label.bottom + layout_scale(ADC_LAYOUT_GAP, dpi), usable, layout_scale(ADC_PROGRESS_BAR_H, dpi));
  out->cancel = layout_rect(margin + (usable - layout_scale(ADC_PROGRESS_CANCEL_W, dpi)) / 2,
                            out->bar.bottom + layout_scale(ADC_LAYOUT_GAP, dpi),
                            layout_scale(ADC_PROGRESS_CANCEL_W, dpi),
                            layout_scale(ADC_PROGRESS_CANCEL_H, dpi));
  out->needed_height = out->cancel.bottom + layout_scale(ADC_LAYOUT_MARGIN_BOTTOM, dpi);
}

#define LX(r) ((r).left)
#define LY(r) ((r).top)
#define LW(r) ((r).right - (r).left)
#define LH(r) ((r).bottom - (r).top)

static wchar_t const ph[] = L"%1$s";

/** @brief The name the window keeps its state under */
static wchar_t const state_prop[] = L"audient_device_closer_progress_state";

/** @brief The line the window shows: the stage of the run and its step */
static wchar_t const line_ph[] = L"%1$s (%2$d/%3$d)";

/** @brief The styles of the window: a frame it is not resized with */
static DWORD const progress_window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
static DWORD const progress_window_extra = WS_EX_DLGMODALFRAME;

/**
 * @brief The window and the three controls it is made of
 *
 * The event that asks the run to stop is not part of the window: the caller owns it, the
 * window only signals it.  The caller hands it to the run, so the run and the window
 * share the one ask without knowing one another.
 */
struct progress_window {
  HWND hwnd;
  HWND status;
  HWND bar;
  HWND cancel_btn;
  int dpi;
  HFONT font;                              // the font of this window, deleted with it
  wchar_t widest[ADC_PROGRESS_TEXT_CHARS]; // the line the run may report that needs the most room
  HANDLE cancel;                           // the ask to stop, signaled by the window thread, read by the run
  progress_cancel_fn on_cancel;            // borrowed, what the window does when the ask is made, may be NULL
  void *on_cancel_userdata;                // handed to on_cancel
  size_t steps;                            // the number of steps the run counts
  char const *first_text;                  // borrowed, the text the label starts with
  char const *widest_text;                 // borrowed, the stage text that needs the most room
  struct ov_error *err;                    // borrowed, why the window could not be made
  bool failed;                             // the controls could not be made
};

size_t ui_progress_window_steps(bool const wait_first, size_t const targets) {
  return (targets * REPAIR_STEPS_PER_TARGET) + (wait_first ? 1 : 0);
}

/**
 * @brief The line the window shows when a stage has no text of its own
 *
 * @return the translated text, UTF-8
 */
char const *ui_progress_window_default_line(void) { return gettext("Closing the device..."); }

/**
 * @brief The line the window shows for one stage of the run
 *
 * gettext() has to see the literal at the call site, that is what src/i18n/start.bash
 * collects, so the mapping of the stages to the texts lives here and not with the repair.
 *
 * @param stage the stage to name
 * @param s the settings that name the mixer, may be NULL for the defaults
 * @param out receives the translated text, UTF-8, always terminated
 * @param out_size room of out
 */
void ui_progress_window_line(enum repair_stage const stage, struct settings const *const s, char *const out, size_t const out_size) {
  if ((out == NULL) || (out_size == 0)) {
    return;
  }
  out[0] = '\0';
  switch (stage) {
  case REPAIR_STAGE_WAIT:
    strncpy(out, gettext("Waiting..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_PROBE:
    strncpy(out, gettext("Checking the device state..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_VETO_RECORD:
    strncpy(out, gettext("Checking what is blocking the close..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_MIXER:
    repair_stage_mixer_caption(s, out, out_size);
    return;
  case REPAIR_STAGE_PAUSE:
    strncpy(out, gettext("Pausing the Windows Audio service..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_START:
    strncpy(out, gettext("Starting the Windows Audio service..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  case REPAIR_STAGE_REMOVE:
    break;
  case REPAIR_STAGE_CONFIRM:
    strncpy(out, gettext("Waiting for the device to come back..."), out_size - 1);
    out[out_size - 1] = '\0';
    return;
  }
  strncpy(out, ui_progress_window_default_line(), out_size - 1);
  out[out_size - 1] = '\0';
}

/**
 * @brief Raise the abort flag of the run and grey the ways out
 *
 * The flag goes up once and the ways to raise it go grey at the same moment: a reader who
 * pressed the button sees that the run heard it, and a second press has nothing left to
 * press.  The run reads the flag between its steps and quits, it is never interrupted in
 * the middle of one.  A window the caller named no call for takes no ask at all: the flag
 * would go up where nobody reads it, and the run would go on while the window said it had
 * been asked.
 *
 * @param pw the progress window that asks
 * @param err receives the failure of the ask
 * @return false when the ask could not be raised
 */
static NODISCARD bool cancel(struct progress_window *const pw, struct ov_error *const err) {
  bool success = false;

  if (pw->on_cancel == NULL) {
    return true; // a window the caller named no call for takes no ask at all
  }
  {
    DWORD const answered = WaitForSingleObject(pw->cancel, 0);

    if (answered == WAIT_OBJECT_0) {
      return true; // the ask is already on its way, the greyed button says so
    }
    if (answered == WAIT_FAILED) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      return false;
    }
  }
  if (!SetEvent(pw->cancel)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  EnableWindow(pw->cancel_btn, FALSE);
  {
    wchar_t buf[256];

    OV_SNPRINTF(buf, sizeof(buf) / sizeof(buf[0]), ph, ph, gettext("Aborting..."));
    if (!SetWindowTextW(pw->status, buf)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
  }
  success = true;

cleanup:
  // the run is often taken forward by the window it reports to, so what the ask does to that
  // window is the caller's to say.  It is told even when the label of the button could not be
  // set: the flag is up, and a run that is never taken forward is worse than a label that
  // still reads what it read
  pw->on_cancel(pw->on_cancel_userdata);
  return success;
}

/**
 * @brief Place the controls of the progress window
 *
 * The geometry itself lives in progress_compute(), this only hands the rectangles
 * to the window manager.
 *
 * @param pw the progress window to place
 * @param err receives the failure of the placement
 * @return false when a control could not be placed
 */
static NODISCARD bool layout(struct progress_window *const pw, struct ov_error *const err) {
  struct progress_layout g;
  RECT rc;

  if (!GetClientRect(pw->hwnd, &rc)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  progress_compute(rc.right - rc.left, (UINT)pw->dpi, &g);
  if (!MoveWindow(pw->status, LX(g.label), LY(g.label), LW(g.label), LH(g.label), TRUE) ||
      !MoveWindow(pw->bar, LX(g.bar), LY(g.bar), LW(g.bar), LH(g.bar), TRUE) ||
      !MoveWindow(pw->cancel_btn, LX(g.cancel), LY(g.cancel), LW(g.cancel), LH(g.cancel), TRUE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

/**
 * @brief The room the widest line of the run takes in the font of the window
 *
 * @param pw the progress window that draws the line
 * @param out receives the width in pixels
 * @param err receives the failure of the measurement
 * @return false when the line could not be measured
 */
static NODISCARD bool text_width(struct progress_window const *const pw, int *const out, struct ov_error *const err) {
  HDC dc = NULL;
  HFONT old = NULL;
  SIZE size = {0, 0};
  bool success = false;

  *out = 0;
  dc = GetDC(pw->hwnd);
  if (dc == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (pw->font != NULL) {
    old = (HFONT)SelectObject(dc, pw->font);
    if ((old == NULL) || (old == HGDI_ERROR)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
      old = NULL;
      goto cleanup;
    }
  }
  if (!GetTextExtentPoint32W(dc, pw->widest, (int)wcslen(pw->widest), &size)) {
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
    if (!ReleaseDC(pw->hwnd, dc)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
  }
  return success;
}

/**
 * @brief Put the progress window at the size its layout asks for
 *
 * The only place that changes the size of the window.  The width is the least width of the
 * dialog or the room the widest line of the run needs, whichever is wider: no report of a
 * run is cut off.
 *
 * @param pw the progress window to size
 * @param err receives the failure of the measurement or of the resize
 * @return false when the window could not be put at that size
 */
static NODISCARD bool apply_size(struct progress_window *const pw, struct ov_error *const err) {
  struct progress_layout g;
  RECT outer;
  RECT now;
  int const margin = layout_scale(ADC_LAYOUT_MARGIN_X, (UINT)pw->dpi);
  int const least = layout_scale(ADC_PROGRESS_W, (UINT)pw->dpi);
  int room = 0;
  int needed = 0;
  int width = 0;

  if (!text_width(pw, &room, err)) {
    OV_ERROR_ADD_TRACE(err);
    return false;
  }
  needed = room + 2 * margin;
  width = (needed > least) ? needed : least;

  progress_compute(width, (UINT)pw->dpi, &g);
  layout_window_rect(width, g.needed_height, (UINT)pw->dpi, progress_window_style, progress_window_extra, &outer);
  if (!GetWindowRect(pw->hwnd, &now)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if (((now.right - now.left) == (outer.right - outer.left)) && ((now.bottom - now.top) == (outer.bottom - outer.top))) {
    return true; // already at the size the layout asks for
  }
  if (!SetWindowPos(pw->hwnd, NULL, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
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
static NODISCARD bool apply_font(struct progress_window *const pw, int const dpi, struct ov_error *const err) {
  HFONT stale = pw->font;
  HFONT font = layout_make_font((UINT)dpi);

  pw->dpi = dpi;
  if (font == NULL) {
    // the geometry follows the monitor, the window keeps the font it has
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    return false;
  }
  pw->font = font;
  SendMessageW(pw->status, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(pw->cancel_btn, WM_SETFONT, (WPARAM)font, TRUE);
  if ((stale != NULL) && !DeleteObject(stale)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    return false;
  }
  return true;
}

/**
 * @brief Make the controls of the progress window and put them in place
 *
 * Called from WM_CREATE.  The caller makes the window and shows it; the controls, the
 * font, the theme, the range of the bar and the width of the widest line are in place by
 * then.
 *
 * @param pw the window to fill, its hwnd, dpi, steps and texts are already set
 * @param err receives the reason a control could not be made
 * @return false when the window could not be filled, with the reason in err
 */
static bool build(struct progress_window *const pw, struct ov_error *const err) {
  wchar_t text[ADC_PROGRESS_TEXT_CHARS] = {0};
  char const *const fit = (pw->widest_text != NULL) ? pw->widest_text : pw->first_text;
  bool success = false;

  OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("Processing"));
  if (!SetWindowTextW(pw->hwnd, text)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (pw->first_text != NULL) {
    OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, pw->first_text);
  } else {
    OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("Closing the device..."));
  }
  pw->status = CreateWindowExW(
      0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, pw->hwnd, (HMENU)(INT_PTR)IDC_PROGRESS_STATUS, NULL, NULL);
  pw->bar = CreateWindowExW(
      0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0, pw->hwnd, (HMENU)(INT_PTR)IDC_PROGRESS_BAR, NULL, NULL);
  OV_SNPRINTF(text, sizeof(text) / sizeof(text[0]), ph, ph, gettext("Abort"));
  pw->cancel_btn = CreateWindowExW(0,
                                   L"BUTTON",
                                   text,
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
    goto cleanup;
  }
  // a window that has nobody to tell about the ask does not offer it
  if (pw->on_cancel == NULL) {
    EnableWindow(pw->cancel_btn, FALSE);
  }
  if (!theme_attach(pw->hwnd, err) || !theme_attach(pw->status, err) || !theme_attach(pw->bar, err) || !theme_attach(pw->cancel_btn, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!apply_font(pw, pw->dpi, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  OV_SNPRINTF(pw->widest,
              sizeof(pw->widest) / sizeof(pw->widest[0]),
              line_ph,
              line_ph,
              (fit != NULL) ? fit : gettext("Closing the device..."),
              (int)pw->steps,
              (int)pw->steps);
  if (!apply_size(pw, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  SendMessageW(pw->bar, PBM_SETRANGE, 0, MAKELPARAM(0, (short)pw->steps));
  SendMessageW(pw->bar, PBM_SETSTEP, 1, 0);
  if (!layout(pw, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
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
static LRESULT CALLBACK proc(HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam) {
  struct progress_window *pw = (struct progress_window *)GetPropW(hwnd, state_prop);

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
    if (pw != NULL) {
      pw->hwnd = hwnd;
      if (!build(pw, pw->err)) {
        pw->failed = true;
      }
    }
    return 0;
  case WM_DESTROY:
    if (pw != NULL) {
      if (pw->font != NULL) {
        if (!DeleteObject(pw->font)) {
          struct ov_error err = {0};

          OV_ERROR_SET_GENERIC(&err, ov_error_generic_fail);
          OV_ERROR_REPORT(&err, NULL);
        }
        pw->font = NULL;
      }
      if (!RemovePropW(hwnd, state_prop)) {
        struct ov_error err = {0};

        OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
        OV_ERROR_REPORT(&err, NULL);
      }
      OV_FREE(&pw);
    }
    return 0;
  case WM_DPICHANGED: {
    RECT const *const rc = (RECT const *)(void *)lparam;
    struct ov_error err = {0};
    bool ok = false;

    /* the monitor suggests where the window goes; how big it is, the layout says */
    if (!SetWindowPos(hwnd, NULL, rc->left, rc->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
      OV_ERROR_SET_HRESULT(&err, HRESULT_FROM_WIN32(GetLastError()));
    } else if (!apply_font(pw, (int)LOWORD(wparam), &err)) {
      OV_ERROR_ADD_TRACE(&err);
    } else if (!apply_size(pw, &err)) {
      OV_ERROR_ADD_TRACE(&err);
    } else if (!layout(pw, &err)) {
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

    if (!layout(pw, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
  case WM_COMMAND: {
    struct ov_error err = {0};

    if ((HIWORD(wparam) == BN_CLICKED) && (LOWORD(wparam) == (WORD)IDC_PROGRESS_CANCEL)) {
      if (!cancel(pw, &err)) {
        OV_ERROR_REPORT(&err, NULL);
      }
    }
    return 0;
  }
  case WM_CLOSE: {
    struct ov_error err = {0};

    if (!cancel(pw, &err)) {
      OV_ERROR_REPORT(&err, NULL);
    }
    return 0;
  }
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
 * @brief Register the window class of the progress window
 *
 * The class is registered once per process, the way the main window does it: a second
 * window in the same process reuses the registration.
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
    wc.lpszClassName = L"audient_device_closer_progress";
    atom = RegisterClassExW(&wc);
  }
  if (atom == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  return true;
}

NODISCARD bool ui_progress_window_create(HWND const owner,
                                         int const dpi,
                                         size_t const steps,
                                         HANDLE const cancel,
                                         progress_cancel_fn const on_cancel,
                                         void *const on_cancel_userdata,
                                         char const *const first_text,
                                         char const *const widest_text,
                                         HWND *const out,
                                         struct ov_error *const err) {
  struct progress_window *pw = NULL;
  bool success = false;

  if ((owner == NULL) || (cancel == NULL) || (out == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    goto cleanup;
  }
  *out = NULL;
  if (!register_class(err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  if (!OV_REALLOC(&pw, 1, sizeof(*pw))) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_out_of_memory);
    goto cleanup;
  }
  *pw = (struct progress_window){0};
  pw->dpi = dpi;
  pw->cancel = cancel;
  pw->on_cancel = on_cancel;
  pw->on_cancel_userdata = on_cancel_userdata;
  pw->steps = steps;
  pw->first_text = first_text;
  pw->widest_text = widest_text;
  pw->err = err;
  {
    RECT owner_rc = {0, 0, 0, 0};
    RECT outer = {0, 0, 0, 0};
    struct progress_layout g;
    int width = 0;
    int height = 0;

    if (!GetWindowRect(owner, &owner_rc)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      goto cleanup;
    }
    width = layout_scale(ADC_PROGRESS_W, (UINT)dpi);
    progress_compute(width, (UINT)dpi, &g);
    layout_window_rect(width, g.needed_height, (UINT)dpi, progress_window_style, progress_window_extra, &outer);
    width = outer.right - outer.left;
    height = outer.bottom - outer.top;
    pw->hwnd = CreateWindowExW(progress_window_extra,
                               L"audient_device_closer_progress",
                               L"",
                               progress_window_style,
                               owner_rc.left + ((owner_rc.right - owner_rc.left) - width) / 2,
                               owner_rc.top + ((owner_rc.bottom - owner_rc.top) - height) / 2,
                               width,
                               height,
                               owner,
                               NULL,
                               GetModuleHandleW(NULL),
                               pw);
  }
  if (pw->hwnd == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  if (pw->failed) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  EnableWindow(owner, FALSE); // the dialog holds its owner, the run owns both
  // ShowWindow answers the state the window was in and does not set the last error, so
  // there is nothing to read from it
  ShowWindow(pw->hwnd, SW_SHOW);
  if (!UpdateWindow(pw->hwnd)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  *out = pw->hwnd;
  pw = NULL;
  success = true;

cleanup:
  if (pw != NULL) {
    if (pw->hwnd != NULL) {
      // answers through WM_DESTROY, which frees the state
      if (!DestroyWindow(pw->hwnd)) {
        OV_ERROR_PUSH(err, ov_error_type_hresult, HRESULT_FROM_WIN32(GetLastError()), NULL);
      }
    } else {
      OV_FREE(&pw);
    }
  }
  return success;
}

NODISCARD bool ui_progress_window_destroy(HWND *const progress, struct ov_error *const err) {
  struct progress_window *pw = NULL;
  HWND hwnd = NULL;
  bool success = true;

  if ((progress == NULL) || (*progress == NULL)) {
    return true;
  }
  hwnd = *progress;
  *progress = NULL;
  pw = (struct progress_window *)GetPropW(hwnd, state_prop);
  if (pw != NULL) {
    HWND const owner = GetWindow(hwnd, GW_OWNER);

    if (owner != NULL) {
      EnableWindow(owner, TRUE); // never leave the owner locked
      SetActiveWindow(owner);    // the focus the window held goes back to the owner
    }
  }
  if (!DestroyWindow(hwnd)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    success = false;
  }
  return success;
}

NODISCARD bool
ui_progress_window_report(HWND const progress, size_t const step, char const *const text, size_t const steps, struct ov_error *const err) {
  struct progress_window *const pw = (struct progress_window *)GetPropW(progress, state_prop);
  wchar_t buf[256];

  if (pw == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  OV_SNPRINTF(buf, sizeof(buf) / sizeof(buf[0]), line_ph, line_ph, text, (int)step, (int)steps);
  if (!SetWindowTextW(pw->status, buf)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  SendMessageW(pw->bar, PBM_SETRANGE, 0, MAKELPARAM(0, (short)steps));
  SendMessageW(pw->bar, PBM_SETPOS, (WPARAM)step, 0);
  return true;
}
