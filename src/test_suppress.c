#include <windows.h>

#include <commctrl.h>

#include <ovbase.h>

#include "test_suppress.h"

#undef TaskDialogIndirect
#undef MessageBoxW

/**
 * @brief The hooks that catch a message on its way to and from a window procedure
 *
 * A hook is for one thread, so every thread that makes a window of a test has one of its
 * own: a test makes its window on a thread apart from the one that reads the result, and a
 * hook of the reading thread would never see it.
 */
static _Thread_local HHOOK g_hook = NULL;
static _Thread_local HHOOK g_hook_ret = NULL;

/**
 * @brief The styles that keep a window out of the reader's way
 *
 * A window without WS_EX_LAYERED cannot be given an opacity at all, one without
 * WS_EX_TRANSPARENT takes the press that was meant for what is under it, and one without
 * WS_EX_NOACTIVATE takes the focus away from what the reader is doing.
 */
enum {
  test_window_extra = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
};

/**
 * @brief Make a finished window show nothing and take no press
 *
 * The extended style of a window is fixed once it exists, so it is only put on when it is
 * not there yet.  The opacity is set apart: a window that is given one while its WM_CREATE
 * is running does not keep it, which is why this is called again once the making of the
 * window is over.
 *
 * @param hwnd the window to make invisible
 */
static void make_invisible(HWND const hwnd) {
  LONG_PTR const style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

  if ((style & test_window_extra) != test_window_extra) {
    // SetWindowLongPtrW answers the value the style had, so a window that had none answers
    // zero; the call is read back rather than judged by what it answered
    SetLastError(ERROR_SUCCESS);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | test_window_extra);
    if (GetLastError() != ERROR_SUCCESS) {
      return; // the window cannot take the styles, and it is shown as it is
    }
  }
  SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
}

/**
 * @brief The hook that puts the styles on a window as it is made
 *
 * @param code the kind of message
 * @param wparam the first parameter
 * @param lparam the message, its window is the window it is for
 * @return the answer the next hook has to give
 */
static LRESULT CALLBACK hook_proc(int const code, WPARAM const wparam, LPARAM const lparam) {
  if (code == HC_ACTION) {
    CWPSTRUCT const *const msg = (CWPSTRUCT const *)(void *)lparam;

    if ((msg != NULL) && (msg->message == WM_CREATE)) {
      make_invisible(msg->hwnd);
    }
  }
  return CallNextHookEx(g_hook, code, wparam, lparam);
}

/**
 * @brief The hook that puts the opacity on a window once it is made
 *
 * WM_CREATE is seen here after the window procedure answered it, which is the first moment
 * the window keeps an opacity.
 *
 * @param code the kind of message
 * @param wparam the first parameter
 * @param lparam the message that was answered, its window is the window it was for
 * @return the answer the next hook has to give
 */
static LRESULT CALLBACK hook_proc_ret(int const code, WPARAM const wparam, LPARAM const lparam) {
  if (code == HC_ACTION) {
    CWPRETSTRUCT const *const msg = (CWPRETSTRUCT const *)(void *)lparam;

    if ((msg != NULL) && (msg->message == WM_CREATE)) {
      make_invisible(msg->hwnd);
    }
  }
  return CallNextHookEx(g_hook_ret, code, wparam, lparam);
}

bool test_suppress_install(struct ov_error *const err) {
  if (g_hook != NULL) {
    return true;
  }
  g_hook = SetWindowsHookExW(WH_CALLWNDPROC, hook_proc, NULL, GetCurrentThreadId());
  if (g_hook == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  g_hook_ret = SetWindowsHookExW(WH_CALLWNDPROCRET, hook_proc_ret, NULL, GetCurrentThreadId());
  if (g_hook_ret == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
    return false;
  }
  return true;
}

void test_suppress_install_quiet(void) {
  struct ov_error err = {0};

  if (!test_suppress_install(&err)) {
    OV_ERROR_REPORT(&err, NULL);
  }
}

void test_suppress_release(void) {
  if (g_hook_ret != NULL) {
    UnhookWindowsHookEx(g_hook_ret);
    g_hook_ret = NULL;
  }
  if (g_hook != NULL) {
    UnhookWindowsHookEx(g_hook);
    g_hook = NULL;
  }
}

bool test_suppress_opacity(HWND const hwnd, BYTE *const out, struct ov_error *const err) {
  COLORREF key = 0;
  BYTE alpha = 0;
  DWORD flags = 0;

  *out = 0;
  if ((hwnd == NULL) || !IsWindow(hwnd)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_invalid_argument);
    return false;
  }
  if (!GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    return false;
  }
  if ((flags & LWA_ALPHA) == 0) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found); // the window is layered without an opacity
    return false;
  }
  *out = alpha;
  return true;
}

HRESULT test_suppress_task(TASKDIALOGCONFIG const *config, int *button, int *radio, BOOL *verification) {
  (void)config;
  if (button != NULL) {
    *button = IDOK;
  }
  if (radio != NULL) {
    *radio = 0;
  }
  if (verification != NULL) {
    *verification = FALSE;
  }
  return S_OK;
}

int test_suppress_message(HWND const owner, LPCWSTR const text, LPCWSTR const caption, UINT const type) {
  (void)owner;
  (void)text;
  (void)caption;
  (void)type;
  return IDOK;
}
