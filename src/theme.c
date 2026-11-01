#include "theme.h"

#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <commctrl.h>
#include <string.h>

#include <ovbase.h>

#include "layout.h"
#include "theme_button.h"
#include "theme_combo.h"
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

/** @brief The slack between the caption and the frame line, in logical units */
enum { theme_frame_break = 4 };

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
  bool can_go_dark; //!< the entry points that bring the dark palette were all found
  HBRUSH surface;
  HBRUSH control;
  should_apps_use_dark_mode_fn should_apps_use_dark_mode;
  allow_dark_mode_for_window_fn allow_dark_mode_for_window;
  refresh_immersive_color_policy_fn refresh_policy;
  flush_menu_themes_fn flush_menus;
};

static struct theme g_theme;

/**
 * @brief Who says which palette the system asked for
 *
 * NULL asks the system.  A test puts its own answer here, so that both palettes are walked
 * without the machine having to be set to them, and so that a palette change is a change of
 * this answer rather than a change of the machine.
 */
static bool (*g_prefers_dark)(void);

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
COLORREF theme_palette_control(void) { return g_theme.dark ? RGB(0x2B, 0x2B, 0x2B) : GetSysColor(COLOR_BTNFACE); }

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
 * @param err receives the failure of the swap
 * @return false when the class could not be swapped
 */
static NODISCARD bool swap_theme_class(HWND const hwnd, struct ov_error *const err) {
  HRESULT const hr = SetWindowTheme(hwnd, g_theme.dark ? L"DarkMode_Explorer" : NULL, NULL);

  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    return false;
  }
  return true;
}

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
 * @param err receives the failure of the swap
 * @return false when the theme could not be taken off the bar
 */
static NODISCARD bool attach_progress(HWND const hwnd, struct ov_error *const err) {
  HRESULT hr = S_OK;

  hr = SetWindowTheme(hwnd, g_theme.dark ? L"" : NULL, g_theme.dark ? L"" : NULL);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    return false;
  }
  if (g_theme.dark) {
    SendMessageW(hwnd, PBM_SETBARCOLOR, 0, (LPARAM)theme_palette_text());
    SendMessageW(hwnd, PBM_SETBKCOLOR, 0, (LPARAM)theme_palette_surface());
    return true;
  }
  SendMessageW(hwnd, PBM_SETBARCOLOR, 0, (LPARAM)CLR_DEFAULT);
  SendMessageW(hwnd, PBM_SETBKCOLOR, 0, (LPARAM)CLR_DEFAULT);
  return true;
}

/**
 * @brief Let one window draw itself in the dark palette
 *
 * DwmSetWindowAttribute answers an error on a system that does not know the attribute,
 * which is not a failure of the window: the window is a themed window either way.
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
 * @param out receives the build number
 * @param err receives the failure of the lookup
 * @return false when the build number could not be read
 */
static NODISCARD bool build_number(DWORD *const out, struct ov_error *const err) {
  HMODULE ntdll = NULL;
  void(WINAPI * rtl_get_nt_version_numbers)(DWORD *, DWORD *, DWORD *);
  DWORD major = 0;
  DWORD minor = 0;
  DWORD build = 0;
  bool success = false;

  ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  rtl_get_nt_version_numbers = (void(WINAPI *)(DWORD *, DWORD *, DWORD *))(void *)GetProcAddress(ntdll, "RtlGetNtVersionNumbers");
  if (rtl_get_nt_version_numbers == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
    goto cleanup;
  }
  rtl_get_nt_version_numbers(&major, &minor, &build);
  *out = build & ~0xF0000000UL;
  success = true;

cleanup:
  return success;
}

/**
 * @brief Is the system running in high contrast
 *
 * High contrast is a palette of its own and must not be painted over.
 *
 * @param out receives the answer
 * @param err receives the failure of the question
 * @return false when the system could not be asked
 */
static NODISCARD bool high_contrast_on(bool *const out, struct ov_error *const err) {
  HIGHCONTRASTW hc = {0};
  bool success = false;

  hc.cbSize = sizeof(hc);
  if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, FALSE)) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  *out = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
  success = true;

cleanup:
  return success;
}

/**
 * @brief What the system wants right now
 *
 * Re-asked whenever the user changes the setting.
 *
 * ShouldAppsUseDarkMode answers the app mode of this process, not the setting of the
 * desktop: in a process that has not asked for the dark palette yet it answers FALSE even
 * when the desktop is dark.  Asking it before resolve_uxtheme() has allowed the palette
 * and refreshed the policy therefore reads the wrong answer.
 *
 * @param out receives the answer
 * @param err receives the failure of the question
 * @return false when the system could not be asked
 */
static NODISCARD bool query_dark(bool *const out, struct ov_error *const err) {
  bool high = false;
  bool success = false;

  if (g_prefers_dark != NULL) {
    *out = g_prefers_dark();
    success = true;
    goto cleanup;
  }
  if (!g_theme.can_go_dark) {
    *out = false;
    success = true;
    goto cleanup;
  }
  if (!high_contrast_on(&high, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  *out = g_theme.should_apps_use_dark_mode() && !high;
  success = true;

cleanup:
  return success;
}

/**
 * @brief Look the undocumented entry points up and switch the process to the palette of
 *        the system
 *
 * The module is only borrowed for the lookup.
 *
 * @param build the build number of Windows, it picks the calling convention
 * @param err receives the failure of the lookup
 * @return false when the module or one of the entry points it needs is not there
 */
static NODISCARD bool resolve_uxtheme(DWORD const build, struct ov_error *const err) {
  HMODULE uxtheme = NULL;
  FARPROC mode135 = NULL;
  refresh_immersive_color_policy_fn refresh = NULL;
  flush_menu_themes_fn flush = NULL;
  bool success = false;

  uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (uxtheme == NULL) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  g_theme.should_apps_use_dark_mode = (should_apps_use_dark_mode_fn)(void *)GetProcAddress(uxtheme, ORD_SHOULD_APPS_USE_DARK_MODE);
  g_theme.allow_dark_mode_for_window = (allow_dark_mode_for_window_fn)(void *)GetProcAddress(uxtheme, ORD_ALLOW_DARK_MODE_FOR_WINDOW);
  refresh = (refresh_immersive_color_policy_fn)(void *)GetProcAddress(uxtheme, ORD_REFRESH_IMMERSIVE_COLOR_POLICY);
  flush = (flush_menu_themes_fn)(void *)GetProcAddress(uxtheme, ORD_FLUSH_MENU_THEMES);
  g_theme.refresh_policy = refresh;
  g_theme.flush_menus = flush;
  mode135 = GetProcAddress(uxtheme, ORD_PREFERRED_APP_MODE);

  if ((g_theme.should_apps_use_dark_mode == NULL) || (g_theme.allow_dark_mode_for_window == NULL) || (mode135 == NULL)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
    goto cleanup;
  }
  // The order is the answer: the palette has to be allowed and the policy refreshed
  // before anything is asked, or ShouldAppsUseDarkMode still reports the light app mode
  // of a process that never asked, and a dark desktop is painted light.
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
  g_theme.can_go_dark = true;
  success = true;

cleanup:
  if (uxtheme != NULL) {
    if (!FreeLibrary(uxtheme)) {
      OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
      success = false;
    }
    uxtheme = NULL;
  }
  return success;
}

/**
 * @brief Resolve the theme entry points and switch to the palette of the system
 *
 * Must be called before the first window of the process is created.
 *
 * @param enabled whether the palette of the system is wanted at all
 * @param err receives the failure of the lookup or of the brushes
 * @return false when the theme could not be taken up; the caller decides what that means
 */
NODISCARD bool theme_init(bool const enabled, struct ov_error *const err) {
  bool success = false;
  bool dark = false;
  DWORD build = 0;

  if (g_theme.resolved) {
    success = true; // there is nothing left to do after the first call
    goto cleanup;
  }
  g_theme.resolved = true;
  g_theme.enabled = enabled;
  if (enabled) {
    if (!build_number(&build, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!resolve_uxtheme(build, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    if (!query_dark(&dark, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  g_theme.dark = dark;
  g_theme.surface = CreateSolidBrush(theme_palette_surface());
  if (g_theme.surface == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why it refused the brush
    goto cleanup;
  }
  g_theme.control = CreateSolidBrush(theme_palette_control());
  if (g_theme.control == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief Delete the resources that theme_init() and theme_attach() created
 *
 * The theme is dropped whether or not the objects could be deleted.  A delete that fails
 * is reported, so the caller knows the objects were left behind.
 *
 * @param err receives the failure of a delete, NULL is allowed
 * @return false when an object could not be deleted
 */
NODISCARD bool theme_release(struct ov_error *const err) {
  bool success = true;

  if ((g_theme.surface != NULL) && !DeleteObject(g_theme.surface)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why it refused
    success = false;
  }
  if ((g_theme.control != NULL) && !DeleteObject(g_theme.control)) {
    if (success) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    } else {
      OV_ERROR_PUSH(err, ov_error_type_generic, ov_error_generic_fail, NULL);
    }
    success = false;
  }
  memset(&g_theme, 0, sizeof(g_theme));
  g_prefers_dark = NULL;
  return success;
}

/**
 * @brief Attach the theme to one window
 *
 * Depending on its window class and button style the window gets a theme class, a subclass
 * that paints it, or nothing at all.  Call it right after CreateWindowExW().  Attaching a
 * list view before its columns exist leaves the header unthemed, use theme_attach_list()
 * for that.
 *
 * @param hwnd the window to attach to
 * @param err receives the failure of the attach
 * @return false when the theme could not be put on the window
 */
NODISCARD bool theme_attach(HWND const hwnd, struct ov_error *const err) {
  wchar_t cls[32];
  LONG_PTR type = 0;
  bool success = false;

  if (!g_theme.enabled || (hwnd == NULL)) {
    return true;
  }
  type = GetWindowLongPtrW(hwnd, GWL_STYLE) & 0xF;
  allow_window(hwnd);
  memset(cls, 0, sizeof(cls));
  if (GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0]))) == 0) {
    OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }

  if (lstrcmpiW(cls, L"BUTTON") == 0) {
    switch (type) {
    case BS_PUSHBUTTON:
    case BS_DEFPUSHBUTTON:
      success = theme_button_attach(hwnd, err);
      break;
    case BS_CHECKBOX:
    case BS_AUTOCHECKBOX:
    case BS_3STATE:
    case BS_AUTO3STATE:
    case BS_RADIOBUTTON:
    case BS_AUTORADIOBUTTON:
      success = swap_theme_class(hwnd, err);
      break;
    default:
      success = true;
      break;
    }
    goto cleanup;
  }
  if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
    success = swap_theme_class(hwnd, err);
    goto cleanup;
  }
  if (lstrcmpiW(cls, WC_COMBOBOXW) == 0) {
    success = theme_combo_attach(hwnd, err);
    goto cleanup;
  }
  if (lstrcmpiW(cls, PROGRESS_CLASSW) == 0) {
    success = attach_progress(hwnd, err);
    goto cleanup;
  }
  success = true;

cleanup:
  if (!success) {
    OV_ERROR_ADD_TRACE(err);
  }
  return success;
}

/**
 * @brief Give a list view the colours of the palette
 *
 * @param list the list view to colour
 * @param err receives the failure of the theme data
 * @return false when the colours could not be read
 */
static NODISCARD bool list_colors(HWND const list, struct ov_error *const err) {
  HTHEME items = NULL;
  COLORREF fg = theme_palette_text();
  COLORREF bg = theme_palette_surface();
  HRESULT hr = S_OK;
  bool success = false;

  items = OpenThemeData(NULL, L"ItemsView");
  if (items == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_not_found);
    goto cleanup;
  }
  hr = GetThemeColor(items, 0, 0, TMT_TEXTCOLOR, &fg);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  ListView_SetTextColor(list, fg);
  hr = GetThemeColor(items, 0, 0, TMT_FILLCOLOR, &bg);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  ListView_SetBkColor(list, bg);
  ListView_SetTextBkColor(list, bg);
  success = true;

cleanup:
  if (items != NULL) {
    if (FAILED(CloseThemeData(items))) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    items = NULL;
  }
  return success;
}

/**
 * @brief Attach the theme to a list view and to its header
 *
 * Call it after the columns were inserted, otherwise the header window does not exist
 * yet.
 *
 * @param list the list view to attach to
 * @param err receives the failure of the attach
 * @return false when the theme could not be put on the list or its header
 */
NODISCARD bool theme_attach_list(HWND const list, struct ov_error *const err) {
  HWND header = NULL;
  HRESULT hr = S_OK;
  bool success = false;

  if (!g_theme.enabled || (list == NULL)) {
    return true;
  }
  if (!g_theme.dark) {
    LRESULT const old = SendMessageW(list, LVM_GETHEADER, 0, 0);

    if (!theme_header_detach((HWND)(void *)old, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
    hr = SetWindowTheme(list, NULL, NULL);
    if (FAILED(hr)) {
      OV_ERROR_SET_HRESULT(err, hr);
      goto cleanup;
    }
    ListView_SetTextColor(list, theme_palette_text());
    ListView_SetBkColor(list, theme_palette_surface());
    ListView_SetTextBkColor(list, theme_palette_surface());
    success = true;
    goto cleanup;
  }
  hr = SetWindowTheme(list, L"DarkMode_Explorer", NULL);
  if (FAILED(hr)) {
    OV_ERROR_SET_HRESULT(err, hr);
    goto cleanup;
  }
  if (!list_colors(list, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  {
    LRESULT const hdr = SendMessageW(list, LVM_GETHEADER, 0, 0);

    header = (HWND)(void *)hdr;
  }
  if (header != NULL) {
    allow_window(header);
    if (!theme_header_attach(header, err)) {
      OV_ERROR_ADD_TRACE(err);
      goto cleanup;
    }
  }
  success = true;

cleanup:
  return success;
}

/**
 * @brief The state the walk of the children of a window carries
 */
struct child_walk {
  struct ov_error *err; //!< the failure of the walk, NULL while none was met
  bool failed;          //!< whether a child could not be attached
};

/**
 * @brief Hand one child of a window back to the theme
 *
 * The walk cannot be stopped with an error, so the first failure is kept in the state and
 * the walk goes on: the children that could not be attached are already left unthemed, and
 * stopping the walk would leave the ones after them the same way.
 *
 * @param child the child the walk found
 * @param param the state the walk carries
 * @return TRUE to walk on
 */
static BOOL CALLBACK each_child(HWND const child, LPARAM const param) {
  struct child_walk *const walk = (struct child_walk *)(void *)param;
  wchar_t cls[32];

  memset(cls, 0, sizeof(cls));
  if (GetClassNameW(child, cls, (int)(sizeof(cls) / sizeof(cls[0]))) == 0) {
    if (!walk->failed) {
      OV_ERROR_SET_HRESULT(walk->err, HRESULT_FROM_WIN32(GetLastError()));
      walk->failed = true;
    }
    return TRUE;
  }
  if (lstrcmpiW(cls, WC_LISTVIEWW) == 0) {
    if (!theme_attach_list(child, walk->err)) {
      walk->failed = true;
      return TRUE;
    }
  }
  if (!theme_attach(child, walk->err)) {
    walk->failed = true;
    return TRUE;
  }
  SendMessageW(child, WM_THEMECHANGED, 0, 0);
  return TRUE;
}

/**
 * @brief Make the brushes the painting works with
 *
 * @param err receives the failure of the brushes
 * @return false when either brush could not be made; the old ones stay in place
 */
static NODISCARD bool rebuild_brushes(struct ov_error *const err) {
  HBRUSH surface = NULL;
  HBRUSH control = NULL;
  HBRUSH stale_surface = NULL;
  HBRUSH stale_control = NULL;
  bool success = false;

  surface = CreateSolidBrush(theme_palette_surface());
  if (surface == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why it refused the brush
    goto cleanup;
  }
  control = CreateSolidBrush(theme_palette_control());
  if (control == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
  }
  stale_surface = g_theme.surface;
  stale_control = g_theme.control;
  g_theme.surface = surface;
  g_theme.control = control;
  surface = NULL;
  control = NULL;
  success = true;

cleanup:
  if (surface != NULL) {
    if (!DeleteObject(surface)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    surface = NULL;
  }
  if (control != NULL) {
    if (!DeleteObject(control)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    control = NULL;
  }
  if (stale_surface != NULL) {
    if (!DeleteObject(stale_surface)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    stale_surface = NULL;
  }
  if (stale_control != NULL) {
    if (!DeleteObject(stale_control)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    stale_control = NULL;
  }
  return success;
}

/**
 * @brief Re-read the palette from the system
 *
 * Call it when the user changed the colour setting (WM_SETTINGCHANGE with
 * "ImmersiveColorSet").  The theme stays in AllowDark, so the system decides and this
 * only asks again what that means right now.
 *
 * @param err receives the failure of the brushes
 * @return ov_true when the palette changed and the windows have to be re-attached,
 *         ov_false when it did not change, ov_indeterminate when it could not be re-read,
 *         with err set
 */
static NODISCARD ov_tribool theme_refresh(struct ov_error *const err) {
  ov_tribool ret = ov_false;
  bool dark = false;

  if (!g_theme.enabled) {
    goto cleanup;
  }
  if (g_theme.refresh_policy != NULL) {
    g_theme.refresh_policy();
  }
  if (!query_dark(&dark, err)) {
    OV_ERROR_ADD_TRACE(err);
    ret = ov_indeterminate;
    goto cleanup;
  }
  if (dark == g_theme.dark) {
    goto cleanup;
  }
  g_theme.dark = dark;
  if (!rebuild_brushes(err)) {
    OV_ERROR_ADD_TRACE(err);
    ret = ov_indeterminate;
    goto cleanup;
  }
  if (g_theme.flush_menus != NULL) {
    g_theme.flush_menus();
  }
  ret = ov_true;

cleanup:
  return ret;
}

/**
 * @brief Hand a window and all of its children back to the theme
 *
 * Call it after theme_refresh() reported a change: theme classes are swapped, painting
 * subclasses are attached or removed, the whole tree is redrawn.
 *
 * @param top the window to start from
 * @param err receives the failure of the attach
 * @return false when a window or one of its children could not be attached
 */
static NODISCARD bool theme_reapply(HWND const top, struct ov_error *const err) {
  struct child_walk walk = {.err = err};
  bool success = false;

  if (!g_theme.enabled || (top == NULL)) {
    return true;
  }
  if (!theme_attach(top, err)) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  EnumChildWindows(top, each_child, (LPARAM)(void *)&walk);
  if (walk.failed) {
    OV_ERROR_ADD_TRACE(err);
    goto cleanup;
  }
  SendMessageW(top, WM_THEMECHANGED, 0, 0);
  RedrawWindow(top, NULL, NULL, RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
  success = true;

cleanup:
  return success;
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
 * @param err receives the failure of a message the theme has to answer
 * @return ov_true when the message was consumed and result holds what the window procedure
 *         has to answer, ov_false when it was not consumed, ov_indeterminate when the
 *         message could not be answered, with err set
 */
NODISCARD ov_tribool theme_message(
    HWND const hwnd, UINT const msg, WPARAM const wparam, LPARAM const lparam, LRESULT *const result, struct ov_error *const err) {
  ov_tribool ret = ov_false;

  *result = 0;
  if (!g_theme.enabled) {
    goto cleanup; // every message stays with the system
  }

  switch (msg) {
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORLISTBOX: {
    HDC const dc = (HDC)wparam;
    bool const list = (msg == WM_CTLCOLORLISTBOX);
    bool const disabled = (msg == WM_CTLCOLORSTATIC) && !IsWindowEnabled((HWND)lparam);

    if (!SetBkMode(dc, OPAQUE)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      ret = ov_indeterminate;
      goto cleanup;
    }
    if (SetTextColor(dc, disabled ? theme_palette_text_disabled() : theme_palette_text()) == CLR_INVALID) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      ret = ov_indeterminate;
      goto cleanup;
    }
    if (SetBkColor(dc, list ? theme_palette_control() : theme_palette_surface()) == CLR_INVALID) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      ret = ov_indeterminate;
      goto cleanup;
    }
    *result = (LRESULT)(list ? g_theme.control : g_theme.surface);
    ret = ov_true;
    break;
  }
  case WM_ERASEBKGND:
    if (!g_theme.dark) {
      break;
    }
    {
      RECT rc = {0};

      if (!GetClientRect(hwnd, &rc)) {
        OV_ERROR_SET_HRESULT(err, HRESULT_FROM_WIN32(GetLastError()));
        ret = ov_indeterminate;
        goto cleanup;
      }
      if (!FillRect((HDC)wparam, &rc, g_theme.surface)) {
        OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
        ret = ov_indeterminate;
        goto cleanup;
      }
      *result = 1;
      ret = ov_true;
    }
    break;
  case WM_SETTINGCHANGE:
    if ((lparam == 0) || (CSTR_EQUAL != CompareStringOrdinal((LPCWCH)lparam, -1, L"ImmersiveColorSet", -1, TRUE))) {
      break;
    }
    if (theme_refresh(err) == ov_indeterminate) {
      OV_ERROR_ADD_TRACE(err);
      ret = ov_indeterminate;
      goto cleanup;
    }
    // the windows are re-attached either way: the palette of the system is asked for on
    // every change of the setting, whether it moved to another one or not
    if (!theme_reapply(hwnd, err)) {
      OV_ERROR_ADD_TRACE(err);
      ret = ov_indeterminate;
      goto cleanup;
    }
    *result = 0;
    ret = ov_true;
    break;
  case WM_NOTIFY: {
    LPNMHDR const nm = (LPNMHDR)lparam;
    LPNMCUSTOMDRAW cd = NULL;

    if ((nm->code != NM_CUSTOMDRAW) || !g_theme.dark) {
      break;
    }
    cd = (LPNMCUSTOMDRAW)lparam;
    if ((cd->dwDrawStage != CDDS_PREPAINT) && (cd->dwDrawStage != CDDS_ITEMPREPAINT)) {
      break;
    }
    if (SetTextColor(cd->hdc, IsWindowEnabled(cd->hdr.hwndFrom) ? theme_palette_text() : theme_palette_text_disabled()) == CLR_INVALID) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      ret = ov_indeterminate;
      goto cleanup;
    }
    *result = CDRF_NEWFONT;
    ret = ov_true;
    break;
  }
  default:
    break;
  }

cleanup:
  return ret;
}

/**
 * @brief Make the brush to give to WNDCLASSEXW::hbrBackground
 *
 * The system deletes the background brush of a class when the class is unregistered, so
 * every class needs a brush of its own: the one the palette paints with is the theme's to
 * keep.  The brush has the colour the palette paints the surface with, so a class and the
 * messages of the theme agree.
 *
 * @param err receives the failure of the brush
 * @return the brush, NULL when it could not be made
 */
HBRUSH theme_class_brush(struct ov_error *const err) {
  HBRUSH const brush = CreateSolidBrush(theme_palette_surface());

  if (brush == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why it refused
  }
  return brush;
}

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
 * @param err receives the failure of the drawing
 * @return false when the frame could not be drawn
 */
NODISCARD bool theme_frame(HDC const dc, RECT const *const rect, RECT const *const caption, int const dpi, struct ov_error *const err) {
  UINT const d = (UINT)dpi;
  int const g = layout_scale(ADC_THEME_FRAME_LINE, d);
  int const pad = layout_scale(theme_frame_break, d);
  HPEN pen = NULL;
  HPEN old_pen = NULL;
  int const top = rect->top + g;
  int break_left = rect->left;
  int break_right = rect->right;
  bool success = false;

  pen = CreatePen(PS_SOLID, g, theme_palette_border());
  if (pen == NULL) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail); // GDI does not say why
    goto cleanup;
  }
  old_pen = (HPEN)SelectObject(dc, pen);
  if ((old_pen == NULL) || (old_pen == HGDI_ERROR)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    old_pen = NULL;
    goto cleanup;
  }
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
    if (!MoveToEx(dc, rect->left, top, NULL) || !LineTo(dc, break_left, top)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
  }
  if (break_right < rect->right) {
    if (!MoveToEx(dc, break_right, top, NULL) || !LineTo(dc, rect->right - g, top)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      goto cleanup;
    }
  }
  if (!MoveToEx(dc, rect->right - g, top, NULL) || !LineTo(dc, rect->right - g, rect->bottom - g) ||
      !LineTo(dc, rect->left + g, rect->bottom - g) || !LineTo(dc, rect->left + g, top)) {
    OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
    goto cleanup;
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
  if (pen != NULL) {
    if (!DeleteObject(pen)) {
      OV_ERROR_SET_GENERIC(err, ov_error_generic_fail);
      success = false;
    }
    pen = NULL;
  }
  return success;
}
