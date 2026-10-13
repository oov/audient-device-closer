#ifndef ADC_THEME_COMBO_H
#define ADC_THEME_COMBO_H

#include <windows.h>

/**
 * Painting of a drop down list combo box.  Internal to the theme implementation, see
 * theme_internal.h.
 */

/**
 * @brief Take over the painting of a drop down list combo box and the look of its list
 *
 * The field of a CBS_DROPDOWNLIST is painted from the theme, which is light in every
 * state, and the style does not ask its parent for colours the way an edit control does,
 * so neither a theme class nor the colours of the parent make the field dark.  With the
 * dark palette this component therefore paints the field, the line the reader chose and
 * the arrow itself, and it gives the theme class to the drop down list, which is the
 * part the explorer theme draws dark on its own, and paints the frame of that list, which
 * the theme of the system leaves in the light border of the classic frame.  With the light palette nothing happens
 * and the control keeps its stock look.  A combo of another style is left to the theme of
 * the system.
 *
 * Call it right after CreateWindowExW().  A window that is not a combo box, and a combo
 * of another style than CBS_DROPDOWNLIST, are left alone.
 *
 * @param combo the window to attach to, NULL is allowed
 */
void theme_combo_attach(HWND const combo);

#endif /* ADC_THEME_COMBO_H */
