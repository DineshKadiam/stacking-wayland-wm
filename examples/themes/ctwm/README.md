# CTWM-inspired theme

Load and apply this pack like the other CoW themes:

```conf
include /path/to/examples/themes/ctwm/theme.conf
decor -a ctwm
```

It defines cyan raised borders, compact titlebars, menus and icons, and sets a
4x2 page grid. Desk 0 gets the eight backgrounds from the CTWM demonstration:
cork, plaid, brown and midnight blue across the top; firebrick, cyan, green and
midnight blue below. Other desks retain their background settings. Image paths
are resolved relative to theme.conf, with no dependency on the working
directory.

Start the matching pager separately:

```sh
cowpager -f /path/to/examples/themes/ctwm/cowpager.conf
```

It shows the current desk on the current output, inherits the page colours and
images from CoW, and hides window icons. The theme's style places it
bottom-left without a titlebar. Window miniatures use cyan and a rose focus
highlight. The configuration does not launch programs or replace your
navigation bindings.

This is an approximation using CoW decorations, not an implementation of CTWM's
workspace manager: the eight cells are pages of desk 0, not eight separate
desks.

## Assets

`xpm/background8.xpm` is the background shipped by
[CTWM](https://github.com/ctwm-wm/ctwm/blob/master/xpm/background8.xpm). Its
upstream notices are preserved in [COPYRIGHT](COPYRIGHT). `xpm/plaid.xpm` is
the X11 plaid bitmap converted to XPM using white and firebrick colours. The
decoration vectors and configuration are CoW examples.
