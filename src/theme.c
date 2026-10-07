#include "theme.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <commctrl.h>
#include <string.h>

#include <ovbase.h>

#include "layout.h"
#include "theme_button.h"
#include "theme_header.h"
#include "theme_internal.h"

/**
 * @brief Ordinals of the uxtheme entry points that are not exported by name
 */
#define ORD_REFRESH_IMMERSIVE_COLOR_POLICY MAKEINTRESOURCEA(104)
#define ORD_SHOULD_APPS_USE_DARK_MODE MAKEINTRESOURCEA(132)
#define ORD_ALLOW_DARK_MODE_FOR_WINDOW MAKEINTRESOURCEA(133)
#define ORD_PREFERRED_APP_MODE MAKEINTRESOURCEA(135)
#define ORD_FLUSH_MENU_THEMES MAKEINTRESOURCEA(136)

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#  define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define PREFERRED_APP_MODE_ALLOW_DARK 1

typedef int(WINAPI *set_preferred_app_mode_fn)(int);
typedef BOOL(WINAPI *allow_dark_mode_for_app_fn)(BOOL);
typedef BOOL(WINAPI *should_apps_use_dark_mode_fn)(void);
typedef BOOL(WINAPI *allow_dark_mode_for_window_fn)(HWND, BOOL);
typedef void(WINAPI *refresh_immersive_color_policy_fn)(void);
typedef void(WINAPI *flush_menu_themes_fn)(void);

/**
 * @brief The state of the theme of the process
 */
struct theme {
  bool resolved;
  bool enabled;
  bool dark;
  HBRUSH surface;
  HBRUSH control;
  should_apps_use_dark_mode_fn should_apps_use_dark_mode;
  allow_dark_mode_for_window_fn allow_dark_mode_for_window;
  refresh_immersive_color_policy_fn refresh_policy;
  flush_menu_themes_fn flush_menus;
};

static struct theme g_theme;

/**
 * @brief The colour of the surface a control sits on
 *
 * A light theme answers with the system colour, so a component never has to branch on
 * dark or light.  A window without the theme answers with the surface a dialog of the
 * system is drawn on: every control of the system paints itself in the colours of the
 * system on it, which is the traditional look such a window asks for.
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_surface(void) {
  if (!g_theme.enabled) {
    return GetSysColor(COLOR_BTNFACE);
  }
  return g_theme.dark ? RGB(0x20, 0x20, 0x20) : GetSysColor(COLOR_WINDOW);
}

/**
 * @brief The colour of a caption
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_text(void) { return g_theme.dark ? RGB(0xFF, 0xFF, 0xFF) : GetSysColor(COLOR_WINDOWTEXT); }

/**
 * @brief The colour of a caption that cannot be acted on
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_text_disabled(void) { return g_theme.dark ? RGB(0x9A, 0x9A, 0x9A) : GetSysColor(COLOR_GRAYTEXT); }

/**
 * @brief The colour of a frame line
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_border(void) { return g_theme.dark ? RGB(0x39, 0x39, 0x39) : GetSysColor(COLOR_3DSHADOW); }

/**
 * @brief The colour of the face of a control
 *
 * @return the colour to paint with
 */
static COLORREF palette_control(void) { return g_theme.dark ? RGB(0x2B, 0x2B, 0x2B) : GetSysColor(COLOR_BTNFACE); }

/**
 * @brief The colour of the face of a push button
 *
 * @param state the state of the button: hot, pressed or at rest
 * @param enabled FALSE takes the colour a disabled button has
 * @return the colour to paint with
 */
COLORREF theme_palette_button(int const state, BOOL const enabled) {
  if (!g_theme.dark) {
    return GetSysColor(COLOR_BTNFACE);
  }
  if (!enabled) {
    return RGB(0x29, 0x29, 0x29);
  }
  switch (state) {
  case PBS_PRESSED:
    return RGB(0x28, 0x28, 0x28);
  case PBS_HOT:
    return RGB(0x35, 0x35, 0x35);
  default:
    return RGB(0x2D, 0x2D, 0x2D);
  }
}

/**
 * @brief The colour of the border of a push button
 *
 * @return the colour to paint with
 */
COLORREF theme_palette_button_border(void) { return g_theme.dark ? RGB(0x3D, 0x3D, 0x3D) : GetSysColor(COLOR_3DSHADOW); }

/**
 * @brief Is the application running with the dark palette
 *
 * @return TRUE when the palette is the dark one
 */
bool theme_state_dark(void) { return g_theme.dark; }

/**
 * @brief The brush of the surface a control sits on
 *
 * The brush stays owned by the theme, the caller must not delete it.
 *
 * @return the brush to paint with
 */
HBRUSH theme_state_surface(void) { return g_theme.surface; }

/**
 * @brief The brush of the face of a control
 *
 * The brush stays owned by the theme, the caller must not delete it.
 *
 * @return the brush to paint with
 */
HBRUSH theme_state_control(void) { return g_theme.control; }

/**
 * @brief The dpi a window is drawn for
 *
 * @param hwnd the window to ask
 * @return the dpi of the window
 */
int theme_window_dpi(HWND const hwnd) {
  UINT const dpi = GetDpiForWindow(hwnd);
  return (dpi != 0) ? (int)dpi : USER_DEFAULT_SCREEN_DPI;
}

/**
 * @brief The font this window draws its captions with
 *
 * @param hwnd the window to ask
 * @return the font of the window
 */
HFONT theme_window_font(HWND const hwnd) {
  LRESULT const font = SendMessageW(hwnd, WM_GETFONT, 0, 0);
  return (HFONT)(void *)font;
}

/**
 * @brief Put the theme class the palette wants onto a window
 *
 * The caption colour of a themed control comes from the theme definition, so the theme
 * class has to be swapped.  DarkMode_DarkTheme is what the Windows 11 shell uses, but on
 * a build that does not resolve it the control disappears, so the explorer variant is the
 * one that is safe to ask for.
 *
 * @param hwnd the window to swap the class of
 */
static void swap_theme_class(HWND const hwnd) { SetWindowTheme(hwnd, g_theme.dark ? L"DarkMode_Explorer" : NULL, NULL); }

/**
 * @brief Take a progress bar away from the theme and colour it with the palette
 *
 * The dark class set of the theme has no progress parts at all, so no theme class can turn
 * the bar dark.  The theme therefore comes off the bar in the dark palette and the palette
 * colours it instead, which only works without a theme: PBM_SETBARCOLOR and
 * PBM_SETBKCOLOR are ignored while visual styles are enabled.  With the light palette the
 * system theme gets the bar back and the colours are reset to CLR_DEFAULT, the value the
 * control draws its stock look with.
 *
 * @param hwnd the progress bar to switch
 */
static void attach_progress(HWND const hwnd) {
  if (g_theme.dark) {
    SetWindowTheme(hwnd, L"", L"");
    SendMessageW(hwnd, PBM_SETBARCOLOR, 0, (LPARAM)theme_palette_text());
    SendMessageW(hwnd, PBM_SETBKCOLOR, 0, (LPARAM)theme_palette_surface());
    return;
  }
  SetWindowTheme(hwnd, NULL, NULL);
  SendMessageW(hwnd, PBM_SETBARCOLOR, 0, (LPARAM)CLR_DEFAULT);
  SendMessageW(hwnd, PBM_SETBKCOLOR, 0, (LPARAM)CLR_DEFAULT);
}

/**
 * @brief Let one window draw itself in the dark palette
 *
 * @param hwnd the window to switch
 */
static void allow_window(HWND const hwnd) {
  BOOL dark = g_theme.dark ? TRUE : FALSE;

  if (g_theme.allow_dark_mode_for_window != NULL) {
    g_theme.allow_dark_mode_for_window(hwnd, dark);
  }
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

/**
 * @brief The build number of Windows
 *
 * GetVersionExW reports 6.2 unless the manifest declares the release, so it cannot be
 * used to pick the calling convention of an undocumented ordinal.
 *
 * @return the build number, 0 when it could not be read
 */
static DWORD build_number(void) {
  HMODULE ntdll = NULL;
  void(WINAPI * rtl_get_nt_version_numbers)(DWORD *, DWORD *, DWORD *);
  DWORD major = 0;
  DWORD minor = 0;
  DWORD build = 0;

  ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == NULL) {
    return 0;
  }
  rtl_get_nt_version_numbers = (void(WINAPI *)(DWORD *, DWORD *, DWORD *))(void *)GetProcAddress(ntdll, "RtlGetNtVersionNumbers");
  if (rtl_get_nt_version_numbers == NULL) {
    return 0;
  }
  rtl_get_nt_version_numbers(&major, &minor, &build);
  return build & ~0xF0000000UL;
}

/**
 * @brief Is the system running in high contrast
 *
 * High contrast is a palette of its own and must not be painted over.
 *
 * @return TRUE when the user asked for high contrast
 */
static bool high_contrast_on(void) {
  HIGHCONTRASTW hc;

  memset(&hc, 0, sizeof(hc));
  hc.cbSize = sizeof(hc);
  if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, FALSE)) {
    return false;
  }
  return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

/**
 * @brief What the system wants right now
 *
 * Re-asked whenever the user changes the setting.
 *
 * @return TRUE when the palette is the dark one
 */
static bool theme_query_dark(void) {
  if (g_theme.should_apps_use_dark_mode == NULL) {
    return false;
  }
  return g_theme.should_apps_use_dark_mode() && !high_contrast_on();
}

/**
 * @brief Look the undocumented entry points up and switch the process to the palette of
 *        the system
 *
 * The module is only borrowed for the lookup.
 *
 * @param build the build number of Windows, it picks the calling convention
 */
static void resolve_uxtheme(DWORD const build) {
  HMODULE uxtheme = NULL;
  FARPROC mode135 = NULL;
  refresh_immersive_color_policy_fn refresh = NULL;
  flush_menu_themes_fn flush = NULL;

  uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (uxtheme == NULL) {
    goto cleanup;
  }
  g_theme.should_apps_use_dark_mode = (should_apps_use_dark_mode_fn)(void *)GetProcAddress(uxtheme, ORD_SHOULD_APPS_USE_DARK_MODE);
  g_theme.allow_dark_mode_for_window = (allow_dark_mode_for_window_fn)(void *)GetProcAddress(uxtheme, ORD_ALLOW_DARK_MODE_FOR_WINDOW);
  refresh = (refresh_immersive_color_policy_fn)(void *)GetProcAddress(uxtheme, ORD_REFRESH_IMMERSIVE_COLOR_POLICY);
  flush = (flush_menu_themes_fn)(void *)GetProcAddress(uxtheme, ORD_FLUSH_MENU_THEMES);
  g_theme.refresh_policy = refresh;
  g_theme.flush_menus = flush;
  mode135 = GetProcAddress(uxtheme, ORD_PREFERRED_APP_MODE);

  if (g_theme.should_apps_use_dark_mode != NULL && g_theme.allow_dark_mode_for_window != NULL && mode135 != NULL) {
    if (build >= 18362) {
      ((set_preferred_app_mode_fn)(void *)mode135)(PREFERRED_APP_MODE_ALLOW_DARK);
    } else {
      ((allow_dark_mode_for_app_fn)(void *)mode135)(TRUE);
    }
    if (refresh != NULL) {
      refresh();
    }
    if (flush != NULL) {
      flush();
    }
    g_theme.dark = theme_query_dark();
  }

cleanup:
  if (uxtheme != NULL) {
    FreeLibrary(uxtheme);
    uxtheme = NULL;
  }
}

/**
 * @brief Resolve the theme entry points and switch to the palette of the system
 *
 * Must be called before the first window of the process is created.  A system without the
 * entry points is not an error, the application then stays light.
 *
 * @param err reserved for a future failure
 * @return always true, the signature exists so that a future failure can be reported
 */
bool theme_init(bool const enabled, struct ov_error *const err) {
  (void)err;
  if (g_theme.resolved) {
    return true;
  }
  g_theme.resolved = true;
  g_theme.enabled = enabled;
  if (enabled) {
    resolve_uxtheme(build_number());
  }
  g_theme.surface = CreateSolidBrush(theme_palette_surface());
  g_theme.control = CreateSolidBrush(palette_control());
  return true;
}

/**
 * @brief Delete the resources that theme_init() and theme_attach() created
 */
void theme_release(void) {
  if (g_theme.surface != NULL) {
    DeleteObject(g_theme.surface);
  }
  if (g_theme.control != NULL) {
    DeleteObject(g_theme.control);
  }
  memset(&g_theme, 0, sizeof(g_theme));
}

/**
 * @brief Is the application running with the dark palette
 *
 * @return TRUE when the palette is the dark one
 */
bool theme_dark(void) { return g_theme.dark; }

/**
 * @brief Attach the theme to one window
 *
 * Depending on its window class and button style the window gets a theme class, a subclass
 * that paints it, or nothing at all.  Call it right after CreateWindowExW().  Attaching a
 * list view before its columns exist leaves the header unthemed, use theme_attach_list()
 * for that.
 *
 * @param hwnd the window to attach to
 */
void theme_attach(HWND const hwnd) {
  wchar_t cls[32];
  LONG_PTR const type = GetWindowLongPtrW(hwnd, GWL_STYLE) & 0xF;

  if (!g_theme.enabled || (hwnd == NULL)) {
    return;
  }
  allow_window(hwnd);
  memset(cls, 0, sizeof(cls));
  GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0])));

  if (lstrcmpiW(cls, L"BUTTON") == 0) {
    switch (type) {
    case BS_PUSHBUTTON:
    case BS_DEFPUSHBUTTON:
      theme_button_attach(hwnd);
      break;
    case BS_CHECKBOX:
    case BS_AUTOCHECKBOX:
    case BS_3STATE:
    case BS_AUTO3STATE:
    case BS_RADIOBUTTON:
    case BS_AUTORADIOBUTTON:
      swap_theme_class(hwnd);
      break;
    default:
      break;
    }
    return;
  }
  if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
    swap_theme_class(hwnd);
    return;
  }
  if (lstrcmpiW(cls, PROGRESS_CLASSW) == 0) {
    attach_progress(hwnd);
    return;
  }
}

/**
 * @brief Give a list view the colours of the palette
 *
 * @param list the list view to colour
 */
static void theme_list_colors(HWND const list) {
  HTHEME items = NULL;
  COLORREF fg = theme_palette_text();
  COLORREF bg = theme_palette_surface();

  items = OpenThemeData(NULL, L"ItemsView");
  if (items == NULL) {
    goto cleanup;
  }
  if (SUCCEEDED(GetThemeColor(items, 0, 0, TMT_TEXTCOLOR, &fg))) {
    ListView_SetTextColor(list, fg);
  }
  if (SUCCEEDED(GetThemeColor(items, 0, 0, TMT_FILLCOLOR, &bg))) {
    ListView_SetBkColor(list, bg);
    ListView_SetTextBkColor(list, bg);
  }

cleanup:
  if (items != NULL) {
    CloseThemeData(items);
    items = NULL;
  }
}

/**
 * @brief Attach the theme to a list view and to its header
 *
 * Call it after the columns were inserted, otherwise the header window does not exist
 * yet.
 *
 * @param list the list view to attach to
 */
void theme_attach_list(HWND const list) {
  HWND header = NULL;

  if (!g_theme.enabled || (list == NULL)) {
    return;
  }
  if (!g_theme.dark) {
    {
      LRESULT const old = SendMessageW(list, LVM_GETHEADER, 0, 0);
      theme_header_detach((HWND)(void *)old);
    }
    SetWindowTheme(list, NULL, NULL);
    ListView_SetTextColor(list, theme_palette_text());
    ListView_SetBkColor(list, theme_palette_surface());
    ListView_SetTextBkColor(list, theme_palette_surface());
    return;
  }
  SetWindowTheme(list, L"DarkMode_Explorer", NULL);
  theme_list_colors(list);
  {
    LRESULT const hdr = SendMessageW(list, LVM_GETHEADER, 0, 0);
    header = (HWND)(void *)hdr;
  }
  if (header != NULL) {
    allow_window(header);
    theme_header_attach(header);
  }
}

/**
 * @brief Give the window procedure of the application a chance to answer theme messages
 *
 * Handles WM_CTLCOLORSTATIC, WM_CTLCOLOREDIT, WM_CTLCOLORBTN, WM_ERASEBKGND, WM_NOTIFY
 * with NM_CUSTOMDRAW and WM_DESTROY.  Painting a group frame stays with the caller, it is
 * the only one that knows where the frame goes, use theme_frame() for it.
 *
 * @param hwnd the window the message is for
 * @param msg the message to answer
 * @param wparam the first message parameter
 * @param lparam the second message parameter
 * @param result receives the value to return from the window procedure
 * @param handled receives TRUE when the message was consumed, then the caller has to
 *                return result.  Untouched when the message was not consumed.
 * @param err reserved for a future failure
 * @return always true, the signature exists so that a future failure can be reported
 */
bool theme_message(HWND const hwnd,
                   UINT const msg,
                   WPARAM const wparam,
                   LPARAM const lparam,
                   LRESULT *const result,
                   bool *const handled,
                   struct ov_error *const err) {
  (void)err;

  *handled = false;
  *result = 0;
  if (!g_theme.enabled) {
    return true; // every message stays with the system
  }

  switch (msg) {
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLOREDIT: {
    HDC const dc = (HDC)wparam;
    HWND const wnd = (HWND)lparam;

    SetBkMode(dc, OPAQUE);
    SetTextColor(dc, IsWindowEnabled(wnd) ? theme_palette_text() : theme_palette_text_disabled());
    SetBkColor(dc, theme_palette_surface());
    *result = (LRESULT)g_theme.surface;
    *handled = true;
    return true;
  }
  case WM_CTLCOLORBTN: {
    HDC const dc = (HDC)wparam;

    SetBkMode(dc, OPAQUE);
    SetTextColor(dc, theme_palette_text());
    SetBkColor(dc, theme_palette_surface());
    *result = (LRESULT)g_theme.surface;
    *handled = true;
    return true;
  }
  case WM_ERASEBKGND:
    if (g_theme.dark) {
      RECT rc;
      GetClientRect(hwnd, &rc);
      FillRect((HDC)wparam, &rc, g_theme.surface);
      *result = 1;
      *handled = true;
    }
    return true;
  case WM_SETTINGCHANGE:
    if ((lparam != 0) && (CSTR_EQUAL == CompareStringOrdinal((LPCWCH)lparam, -1, L"ImmersiveColorSet", -1, TRUE))) {
      theme_refresh(NULL);
      theme_reapply(hwnd);
      *result = 0;
      *handled = true;
    }
    return true;
  case WM_NOTIFY: {
    LPNMHDR const nm = (LPNMHDR)lparam;

    if (nm->code == NM_CUSTOMDRAW && g_theme.dark) {
      LPNMCUSTOMDRAW const cd = (LPNMCUSTOMDRAW)lparam;

      switch (cd->dwDrawStage) {
      case CDDS_PREPAINT:
      case CDDS_ITEMPREPAINT:
        SetTextColor(cd->hdc, IsWindowEnabled(cd->hdr.hwndFrom) ? theme_palette_text() : theme_palette_text_disabled());
        *result = CDRF_NEWFONT;
        *handled = true;
        return true;
      default:
        break;
      }
    }
    return true;
  }
  default:
    break;
  }
  return true;
}

/**
 * @brief Make the brushes the painting works with
 */
static void theme_rebuild_brushes(void) {
  HBRUSH surface = NULL;
  HBRUSH control = NULL;
  HBRUSH stale_surface = NULL;
  HBRUSH stale_control = NULL;

  surface = CreateSolidBrush(theme_palette_surface());
  if (surface == NULL) {
    goto cleanup;
  }
  control = CreateSolidBrush(palette_control());
  if (control == NULL) {
    goto cleanup;
  }
  stale_surface = g_theme.surface;
  stale_control = g_theme.control;
  g_theme.surface = surface;
  g_theme.control = control;
  surface = NULL;
  control = NULL;

cleanup:
  if (surface != NULL) {
    DeleteObject(surface);
    surface = NULL;
  }
  if (control != NULL) {
    DeleteObject(control);
    control = NULL;
  }
  if (stale_surface != NULL) {
    DeleteObject(stale_surface);
    stale_surface = NULL;
  }
  if (stale_control != NULL) {
    DeleteObject(stale_control);
    stale_control = NULL;
  }
}

/**
 * @brief Hand one child of a window back to the theme
 *
 * @param child the child the walk found
 * @param param the window the walk started from
 * @return TRUE to walk on
 */
static BOOL CALLBACK each_child(HWND const child, LPARAM const param) {
  wchar_t cls[32];

  (void)param;
  memset(cls, 0, sizeof(cls));
  GetClassNameW(child, cls, (int)(sizeof(cls) / sizeof(cls[0])));
  if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
    theme_attach_list(child);
  }
  theme_attach(child);
  SendMessageW(child, WM_THEMECHANGED, 0, 0);
  return TRUE;
}

/**
 * @brief Re-read the palette from the system
 *
 * Call it when the user changed the colour setting (WM_SETTINGCHANGE with
 * "ImmersiveColorSet").  The theme stays in AllowDark, so the system decides and this
 * only asks again what that means right now.
 *
 * @param err reserved for a future failure
 * @return TRUE when the palette changed and the windows have to be re-attached
 */
bool theme_refresh(struct ov_error *const err) {
  bool dark = false;

  (void)err;
  if (!g_theme.enabled) {
    return false;
  }
  if (g_theme.refresh_policy != NULL) {
    g_theme.refresh_policy();
  }
  dark = theme_query_dark();
  if (dark == g_theme.dark) {
    return false;
  }
  g_theme.dark = dark;
  theme_rebuild_brushes();
  if (g_theme.flush_menus != NULL) {
    g_theme.flush_menus();
  }
  return true;
}

/**
 * @brief Hand a window and all of its children back to the theme
 *
 * Call it after theme_refresh() reported a change: theme classes are swapped, painting
 * subclasses are attached or removed, the whole tree is redrawn.
 *
 * @param top the window to start from
 */
void theme_reapply(HWND const top) {
  if (!g_theme.enabled || (top == NULL)) {
    return;
  }
  theme_attach(top);
  EnumChildWindows(top, each_child, 0);
  SendMessageW(top, WM_THEMECHANGED, 0, 0);
  RedrawWindow(top, NULL, NULL, RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
}

/**
 * @brief The brush to give to WNDCLASSEXW::hbrBackground
 *
 * The brush stays owned by the theme, the caller must not delete it.
 *
 * @return the brush of the palette
 */
HBRUSH theme_class_brush(void) { return g_theme.surface; }

/**
 * @brief Draw the frame of a group of controls
 *
 * BS_GROUPBOX paints its caption with the colour of the theme definition, which cannot be
 * reached from outside, so the caption is a plain static control and the frame is drawn
 * here.
 *
 * @param dc the context to draw in
 * @param rect the frame rectangle in the client coordinates of the window
 * @param caption the rectangle of the caption, in the same coordinates.  The top line of
 *                the frame is broken around it, the way a group box does.  NULL draws a
 *                frame without a break.
 * @param dpi the dpi of the window, it scales the line and the break
 */
void theme_frame(HDC const dc, RECT const *const rect, RECT const *const caption, int const dpi) {
  UINT const d = (UINT)dpi;
  int const g = layout_scale(ADC_LAYOUT_FRAME_LINE, d);
  int const pad = layout_scale(ADC_LAYOUT_FRAME_BREAK, d);
  HPEN pen = NULL;
  HPEN old_pen = NULL;
  int const top = rect->top + g;
  int break_left = rect->left;
  int break_right = rect->right;

  pen = CreatePen(PS_SOLID, g, theme_palette_border());
  if (pen == NULL) {
    goto cleanup;
  }
  old_pen = (HPEN)SelectObject(dc, pen);
  if (caption != NULL) {
    break_left = caption->left - pad;
    break_right = caption->right + pad;
    if (break_left < rect->left) {
      break_left = rect->left;
    }
    if (break_right > rect->right) {
      break_right = rect->right;
    }
  }
  if (break_left > rect->left) {
    MoveToEx(dc, rect->left, top, NULL);
    LineTo(dc, break_left, top);
  }
  if (break_right < rect->right) {
    MoveToEx(dc, break_right, top, NULL);
    LineTo(dc, rect->right - g, top);
  }
  MoveToEx(dc, rect->right - g, top, NULL);
  LineTo(dc, rect->right - g, rect->bottom - g);
  LineTo(dc, rect->left + g, rect->bottom - g);
  LineTo(dc, rect->left + g, top);

cleanup:
  if (old_pen != NULL) {
    SelectObject(dc, old_pen);
    old_pen = NULL;
  }
  if (pen != NULL) {
    DeleteObject(pen);
    pen = NULL;
  }
}
