/*
 * Copyright (c) 2026 Thomas Adam
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "cow/titlebar.h"

#include <string.h>

#include <cairo.h>
#include <pango/pangocairo.h>

#include "cow/colour.h"
#include "cow/compat.h"
#include "cow/config.h"
#include "cow/font.h"
#include "cow/vector.h"

int cow_titlebar_compute_thickness(struct cow_font *font) {
  int raw_height = font->height - 8;
  if (raw_height < 12)
    raw_height = 12;
  return raw_height + COW_TITLEBAR_PADDING * 2;
}

/*
 * Draw FVWM-style raised relief: 1px highlight on top+left,
 * 1px shadow on bottom+right, matching fvwm's corner ownership.
 */
/*
 * Draw 2-ring concentric FVWM relief matching the original titlebar.c
 * draw_fvwm_relief (2px raised bevel = hiout + shin layers).
 */
static void draw_fvwm_relief(uint32_t *pixels, int stride, int rx, int ry,
                             int rw, int rh, uint32_t px_lt, uint32_t px_dk,
                             bool sunken) {
  uint32_t ul = sunken ? px_dk : px_lt;
  uint32_t br = sunken ? px_lt : px_dk;

  for (int r = 0; r < 2 && r < rw / 2 && r < rh / 2; r++) {
    /* Top edge */
    for (int x = rx + r; x < rx + rw - r; x++)
      pixels[(ry + r) * stride + x] = ul;
    /* Left edge */
    for (int y = ry + r; y < ry + rh - r; y++)
      pixels[y * stride + (rx + r)] = ul;
    /* Bottom edge */
    for (int x = rx + r; x < rx + rw - r; x++)
      pixels[(ry + rh - 1 - r) * stride + x] = br;
    /* Right edge */
    for (int y = ry + r; y < ry + rh - r; y++)
      pixels[y * stride + (rx + rw - 1 - r)] = br;
  }
}

void cow_titlebar_render(uint32_t *pixels, int stride_px, int frame_w,
                         int border_width, int title_thickness,
                         const char *title, bool active, struct cow_font *font,
                         int pressed_button, int icon_inverted_button,
                         bool title_pressed, const float *style_colour,
                         const float *style_fg, bool sticky,
                         const char **tab_titles, int n_tabs, int active_tab,
                         bool drop_target) {
  if (title_thickness <= 0 || frame_w <= 2 * border_width)
    return;

  /* Titlebar colors: use style override if provided,
   * otherwise fall back to config */
  float base[4], fg[4];
  const struct cow_titlebar_config *tb_cfg =
      cow_config ? &cow_config->titlebar : NULL;
  if (style_colour != NULL) {
    memcpy(base, style_colour, sizeof(float) * 4);
    if (style_fg != NULL)
      memcpy(fg, style_fg, sizeof(float) * 4);
    else if (tb_cfg != NULL)
      memcpy(fg, active ? tb_cfg->fg_active : tb_cfg->fg_inactive,
             sizeof(float) * 4);
    else
      cow_colour_convert(fg, 0x000000);
  } else if (tb_cfg != NULL) {
    memcpy(base, active ? tb_cfg->active_colour : tb_cfg->inactive_colour,
           sizeof(float) * 4);
    memcpy(fg, active ? tb_cfg->fg_active : tb_cfg->fg_inactive,
           sizeof(float) * 4);
  } else {
    cow_colour_convert(base, active ? 0xC0C0C0 : 0xA0A0A0);
    cow_colour_convert(fg, 0x000000);
  }

  float lt[4], dk[4];
  cow_colour_lighten(lt, base);
  cow_colour_darken(dk, base);

  uint32_t px_base = cow_colour_to_pixel(base);
  uint32_t px_lt = cow_colour_to_pixel(lt);
  uint32_t px_dk = cow_colour_to_pixel(dk);
  uint32_t px_fg = cow_colour_to_pixel(fg);

  /* Titlebar geometry within the frame */
  int tb_x = border_width;
  int tb_y = border_width;
  int tb_w = frame_w - 2 * border_width;
  int tb_h = title_thickness;

  if (tb_w <= 0)
    return;

  /* Fill titlebar base colour */
  for (int y = tb_y; y < tb_y + tb_h; y++) {
    for (int x = tb_x; x < tb_x + tb_w; x++) {
      pixels[y * stride_px + x] = px_base;
    }
  }

  /* Button size = titlebar height */
  int btn_s = tb_h;
  int left_btns = COW_TITLEBAR_LEFT_BUTTONS;
  int right_btns = COW_TITLEBAR_RIGHT_BUTTONS;

  /* Vector button colour palette */
  uint32_t colors[5];
  colors[COW_VECTOR_SHADOW] = px_dk;
  colors[COW_VECTOR_HIGHLIGHT] = px_lt;
  colors[COW_VECTOR_BG] = px_base;
  colors[COW_VECTOR_FG] = px_fg;
  colors[COW_VECTOR_INVISIBLE] = 0;

  /* Get button vectors from config or use defaults */
  const struct cow_vector_button *buttons[3];
  static const struct cow_vector_button default_buttons[3] = {
      {.num_points = 5,
       .points = {{25, 25, 0, 0, -1},
                  {75, 75, 0, 0, 3},
                  {75, 25, 0, 0, -1},
                  {25, 75, 0, 0, 3},
                  {25, 25, 0, 0, -1}}},
      {.num_points = 5,
       .points = {{25, 25, 0, 0, -1},
                  {25, 75, 0, 0, 3},
                  {75, 75, 0, 0, 3},
                  {75, 25, 0, 0, 3},
                  {25, 25, 0, 0, 3}}},
      {.num_points = 5,
       .points = {{25, 65, 0, 0, -1},
                  {75, 65, 0, 0, 3},
                  {75, 75, 0, 0, 3},
                  {25, 75, 0, 0, 3},
                  {25, 65, 0, 0, 3}}},
  };
  for (int i = 0; i < 3; i++) {
    if (tb_cfg != NULL && tb_cfg->buttons[i].vector.num_points > 0)
      buttons[i] = &tb_cfg->buttons[i].vector;
    else
      buttons[i] = &default_buttons[i];
  }

  /* Render left button(s) */
  for (int i = 0; i < left_btns && i < 3; i++) {
    int bx = tb_x + i * btn_s;
    int by = tb_y;
    if (bx + btn_s > tb_x + tb_w)
      break;

    bool btn_sunken = (pressed_button == i);

    /* Fill button background */
    for (int py = by; py < by + btn_s; py++)
      for (int px = bx; px < bx + btn_s; px++)
        pixels[py * stride_px + px] = px_base;

    /* FVWM relief on button: sunken if pressed */
    draw_fvwm_relief(pixels, stride_px, bx, by, btn_s, btn_s, px_lt, px_dk,
                     btn_sunken);

    /* Draw vector into temp buffer then blit */
    uint32_t btn_buf[btn_s * btn_s];
    for (int p = 0; p < btn_s * btn_s; p++)
      btn_buf[p] = px_base;

    bool icon_inverted = (icon_inverted_button == i) || btn_sunken;
    cow_vector_button_draw(buttons[i], btn_buf, btn_s, btn_s, btn_s, btn_s,
                           colors, icon_inverted);

    for (int py = 1; py < btn_s - 1; py++) {
      for (int px = 1; px < btn_s - 1; px++) {
        uint32_t c = btn_buf[py * btn_s + px];
        if (c != px_base)
          pixels[(by + py) * stride_px + (bx + px)] = c;
      }
    }
  }

  /* Render right button(s) */
  for (int i = 0; i < right_btns && (left_btns + i) < 3; i++) {
    int btn_idx = left_btns + i;
    int from_right = right_btns - 1 - i;
    int bx = tb_x + tb_w - (from_right + 1) * btn_s;
    int by = tb_y;
    if (bx < tb_x)
      break;

    bool btn_sunken = (pressed_button == btn_idx);

    /* Fill button background */
    for (int py = by; py < by + btn_s; py++)
      for (int px = bx; px < bx + btn_s; px++)
        pixels[py * stride_px + px] = px_base;

    /* FVWM relief on button: sunken if pressed */
    draw_fvwm_relief(pixels, stride_px, bx, by, btn_s, btn_s, px_lt, px_dk,
                     btn_sunken);

    uint32_t btn_buf[btn_s * btn_s];
    for (int p = 0; p < btn_s * btn_s; p++)
      btn_buf[p] = px_base;

    bool icon_inv = (icon_inverted_button == btn_idx) || btn_sunken;
    cow_vector_button_draw(buttons[btn_idx], btn_buf, btn_s, btn_s, btn_s,
                           btn_s, colors, icon_inv);

    for (int py = 1; py < btn_s - 1; py++) {
      for (int px = 1; px < btn_s - 1; px++) {
        uint32_t c = btn_buf[py * btn_s + px];
        if (c != px_base)
          pixels[(by + py) * stride_px + (bx + px)] = c;
      }
    }
  }

  /* Title text area: between left and right buttons */
  int title_x = tb_x + left_btns * btn_s;
  int title_w = tb_w - (left_btns + right_btns) * btn_s;
  int rendered_text_w = 0; /* actual rendered text width */

  /* Render title text with cairo/pango */
  if (title != NULL && title_w > 4 && font != NULL) {
    int text_inset = 2 + COW_TITLEBAR_PADDING;

    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, title_w, tb_h);
    cairo_t *cr = cairo_create(surface);

    /* Fill with base colour */
    cairo_set_source_rgba(cr, base[0], base[1], base[2], base[3]);
    cairo_paint(cr);

    /* Render text */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font->desc);
    pango_layout_set_text(layout, title, -1);

    int avail_w = title_w - 2 * text_inset;
    if (avail_w > 0)
      pango_layout_set_width(layout, avail_w * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    rendered_text_w = text_w;
    int text_y = (tb_h - text_h) / 2;
    if (text_y < 0)
      text_y = 0;

    cairo_move_to(cr, text_inset, text_y);
    cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_surface_flush(surface);

    /* Copy rendered text into the frame buffer, preserving 2px
     * relief */
    unsigned char *sdata = cairo_image_surface_get_data(surface);
    int sstride = cairo_image_surface_get_stride(surface) / 4;
    int relief = 2;

    for (int py = relief; py < tb_h - relief; py++) {
      for (int px = relief; px < title_w - relief; px++) {
        uint32_t c = ((uint32_t *)sdata)[py * sstride + px];
        if ((c & 0xFF000000) != 0) {
          pixels[(tb_y + py) * stride_px + (title_x + px)] = c;
        }
      }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
  }

  /* Draw stipple for sticky windows in the empty areas
   * either side of the title text. */
  if (sticky && title_w > 4) {
    int relief = 2;
    int pad = COW_TITLEBAR_PADDING;

    /* Centre the text gap within the title area */
    int text_centre = title_x + title_w / 2;
    int text_start = text_centre - rendered_text_w / 2 - pad;
    int text_end = text_centre + (rendered_text_w + 1) / 2 + pad;

    int sx1 = title_x + relief;
    int sx2 = title_x + title_w - relief;

    for (int y = tb_y + relief; y < tb_y + tb_h - relief; y++) {
      /* Left stipple area */
      for (int x = sx1; x < text_start && x < sx2; x++) {
        if ((x + y) % 2 == 0)
          pixels[y * stride_px + x] = px_lt;
        else
          pixels[y * stride_px + x] = px_dk;
      }
      /* Right stipple area */
      for (int x = text_end > sx1 ? text_end : sx1; x < sx2; x++) {
        if ((x + y) % 2 == 0)
          pixels[y * stride_px + x] = px_lt;
        else
          pixels[y * stride_px + x] = px_dk;
      }
    }
  }

  /*
   * Container tab strip.  When n_tabs > 1, divide the title area into
   * equal-width segments.  Active/inactive tab colours come from
   * cow_config->container so users can style them via
   * 'set container.tab.*'.  Falls back to the titlebar base/dk colours
   * when no config is available.
   */
  if (n_tabs > 1 && tab_titles != NULL && title_w > 0) {
    /*
     * Resolve tab colours.  The container config allows explicit
     * overrides; the defaults use the titlebar's own base/dk for
     * backgrounds and the titlebar's fg for both label colours so
     * that text remains readable regardless of how dark the inactive
     * (dk) background is.
     */
    const struct cow_container_config *cc =
        cow_config ? &cow_config->container : NULL;

    float tab_active_f[4], tab_inactive_f[4];
    float tab_fg_active_f[4], tab_fg_inactive_f[4];

    /*
     * Use configured value if explicitly set, otherwise inherit from
     * the titlebar's own base/dk/fg so tabs match the window style.
     *
     * The overrides are only applied when the window is focused
     * (active=true).  When unfocused, base/dk/fg already reflect the
     * inactive titlebar colours (set above from tb_cfg->inactive_colour
     * etc.), so all tabs correctly adopt the unfocused palette instead
     * of the configured focused colours bleeding through.
     */
    /*
     * When unfocused (!active), base already holds titlebar.inactive_colour.
     * All tabs — both the active container tab and inactive ones — use the
     * same base colour, matching the flat single-colour look of a normal
     * unfocused titlebar.  Configured container.tab.* overrides are only
     * applied when the window is focused.
     */
    const float *unfocused_bg = base; /* = titlebar.inactive_colour */
    const float *unfocused_fg = fg;   /* = titlebar.fg_inactive     */
    memcpy(tab_active_f,
           (active && cc && cc->has_tab_active)
               ? cc->tab_active
               : (active ? base : unfocused_bg),
           sizeof(float) * 4);
    memcpy(tab_inactive_f,
           (active && cc && cc->has_tab_inactive)
               ? cc->tab_inactive
               : (active ? dk : unfocused_bg),
           sizeof(float) * 4);
    memcpy(tab_fg_active_f,
           (active && cc && cc->has_tab_fg_active)
               ? cc->tab_fg_active
               : (active ? fg : unfocused_fg),
           sizeof(float) * 4);
    memcpy(tab_fg_inactive_f,
           (active && cc && cc->has_tab_fg_inactive)
               ? cc->tab_fg_inactive
               : (active ? fg : unfocused_fg),
           sizeof(float) * 4);

    /* Pre-compute bevel colours for active and inactive tabs */
    float act_lt[4], act_dk[4], inact_lt[4], inact_dk[4];
    cow_colour_lighten(act_lt, tab_active_f);
    cow_colour_darken(act_dk, tab_active_f);
    cow_colour_lighten(inact_lt, tab_inactive_f);
    cow_colour_darken(inact_dk, tab_inactive_f);
    uint32_t px_act = cow_colour_to_pixel(tab_active_f);
    uint32_t px_inact = cow_colour_to_pixel(tab_inactive_f);
    uint32_t px_act_lt = cow_colour_to_pixel(act_lt);
    uint32_t px_act_dk = cow_colour_to_pixel(act_dk);
    uint32_t px_inact_lt = cow_colour_to_pixel(inact_lt);
    uint32_t px_inact_dk = cow_colour_to_pixel(inact_dk);

    int tab_w = title_w / n_tabs;
    if (tab_w < 1)
      tab_w = 1;

    for (int ti = 0; ti < n_tabs; ti++) {
      int tx = title_x + ti * tab_w;
      int tw = (ti == n_tabs - 1) ? (title_x + title_w - tx) : tab_w;
      bool is_active = (ti == active_tab);

      uint32_t tab_bg_px = is_active ? px_act : px_inact;
      uint32_t tab_bv_lt = is_active ? px_act_lt : px_inact_lt;
      uint32_t tab_bv_dk = is_active ? px_act_dk : px_inact_dk;
      float *tab_bg_f = is_active ? tab_active_f : tab_inactive_f;
      float *tab_fg = is_active ? tab_fg_active_f : tab_fg_inactive_f;

      /* Tab background */
      for (int py = tb_y; py < tb_y + tb_h; py++)
        for (int px = tx; px < tx + tw; px++)
          pixels[py * stride_px + px] = tab_bg_px;

      /* Tab bevel — active is always raised; inactive is sunken only
       * when container.tab.sunken is true (default: flat). */
      bool sunken = !is_active && cc != NULL && cc->tab_sunken_inactive;
      draw_fvwm_relief(pixels, stride_px, tx, tb_y, tw, tb_h, tab_bv_lt,
                       tab_bv_dk, sunken);

      /* Tab label text */
      const char *lbl = tab_titles[ti];
      if (lbl != NULL && tw > 6 && font != NULL) {
        int text_inset = 2 + COW_TITLEBAR_PADDING;
        cairo_surface_t *ts =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, tb_h);
        cairo_t *tcr = cairo_create(ts);
        cairo_set_source_rgba(tcr, tab_bg_f[0], tab_bg_f[1], tab_bg_f[2],
                              tab_bg_f[3]);
        cairo_paint(tcr);

        PangoLayout *tl = pango_cairo_create_layout(tcr);
        pango_layout_set_font_description(tl, font->desc);
        pango_layout_set_text(tl, lbl, -1);
        int avail = tw - 2 * text_inset;
        if (avail > 0)
          pango_layout_set_width(tl, avail * PANGO_SCALE);
        pango_layout_set_ellipsize(tl, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment(tl, PANGO_ALIGN_CENTER);
        int tw2, th2;
        pango_layout_get_pixel_size(tl, &tw2, &th2);
        int ty2 = (tb_h - th2) / 2;
        if (ty2 < 0)
          ty2 = 0;
        cairo_move_to(tcr, text_inset, ty2);
        cairo_set_source_rgba(tcr, tab_fg[0], tab_fg[1], tab_fg[2], tab_fg[3]);
        pango_cairo_show_layout(tcr, tl);
        g_object_unref(tl);
        cairo_surface_flush(ts);

        unsigned char *sd2 = cairo_image_surface_get_data(ts);
        int ss2 = cairo_image_surface_get_stride(ts) / 4;
        for (int py = 2; py < tb_h - 2; py++) {
          for (int px2 = 2; px2 < tw - 2; px2++) {
            uint32_t c = ((uint32_t *)sd2)[py * ss2 + px2];
            if ((c & 0xFF000000) != 0)
              pixels[(tb_y + py) * stride_px + (tx + px2)] = c;
          }
        }
        cairo_destroy(tcr);
        cairo_surface_destroy(ts);
      }
    }
  } else {
    /* Draw relief on title text area LAST so it isn't overwritten.
     * Sunken when title is being dragged, raised otherwise. */
    if (title_w > 2) {
      draw_fvwm_relief(pixels, stride_px, title_x, tb_y, title_w, tb_h, px_lt,
                       px_dk, title_pressed);
    }
  }

  /*
   * Drop-target indicator: draw a 2px bright highlight ring around
   * the full titlebar area to signal that dropping here will
   * containerise the two windows.
   */
  if (drop_target) {
    /* Use a bright accent colour — white with an orange tinge */
    uint32_t px_hi = 0xFFFF8800u;
    int x0 = tb_x, y0 = tb_y, w0 = tb_w, h0 = tb_h;
    for (int r = 0; r < 2 && r < w0 / 2 && r < h0 / 2; r++) {
      for (int x = x0 + r; x < x0 + w0 - r; x++) {
        pixels[(y0 + r) * stride_px + x] = px_hi;
        pixels[(y0 + h0 - 1 - r) * stride_px + x] = px_hi;
      }
      for (int y = y0 + r; y < y0 + h0 - r; y++) {
        pixels[y * stride_px + (x0 + r)] = px_hi;
        pixels[y * stride_px + (x0 + w0 - 1 - r)] = px_hi;
      }
    }
  }
}
