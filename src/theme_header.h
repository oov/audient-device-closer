#ifndef ADC_THEME_HEADER_H
#define ADC_THEME_HEADER_H

#include <windows.h>

/**
 * Painting of the header of a list view.  Internal to the theme implementation, see
 * theme_internal.h.
 */

/**
 * @brief Take over the painting of a header control
 *
 * The header draws its captions with DrawThemeText, which reads the colour from the theme
 * definition, and it only ever reports CDDS_PREPAINT, so no custom draw hook reaches the
 * text.  With the dark palette the whole strip is painted by this component.
 *
 * @param header the header to attach to, NULL is allowed
 */
void theme_header_attach(HWND const header);

/**
 * @brief Give the painting of a header back to comctl32
 *
 * @param header the header to detach from, NULL is allowed
 */
void theme_header_detach(HWND const header);

#endif /* ADC_THEME_HEADER_H */
