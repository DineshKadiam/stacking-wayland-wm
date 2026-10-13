# CoW Theme Examples

Each directory is a self-contained theme pack:

```
    include /path/to/theme/theme.conf
    decor -a theme-name
```

Image paths inside a theme are relative to that theme's theme.conf file.

See [windowdecors.md](windowdecors.md) for xterm preview screenshots of each
theme.

Included examples:

|Theme Name|Description|
|----------|-----------|
|afterstep | brown/blue AfterStep-style colours
|awol      | grey raised FVWM-style colours
|blackbox  | Blackbox-style titlebar pixmaps
|brushedmetal | brushed metal pixmaps
|canoe    | lavender, purple, and grey Canoe-inspired Motif styling
|cde       | CDE-style geometry and colours
|ctwm      | cyan CTWM-style furniture, eight page backgrounds and a matching pager
|celticbow | red celtic titlebar pixmaps
|default   | default fvwm-themes decoration
|e17       | Enlightenment-style titlebar pixmaps
|ice       | pale blue titlebar pixmaps
|luthien   | dark green FVWM-style colours
|metal     | metal titlebar and border pixmaps
|steel     | steel/grey SVG textures
|mech      | dark metal/stone SVG textures
|migo      | translucent migo-inspired colours
|minimal   | minimal FVWM-style decoration
|mango     | light floral PNG textures
|nanogui   | compact NanoGUI-style pixmaps
|olicha    | dark olive titlebar colours
|osx       | Aqua-style gradients
|plainbow  | simple red celtic colours
|qnx       | QNX-style titlebar pixmaps
|skyred    | red stone SVG textures
|sa        | shaded grey FVWM-style colours
|spruce    | green spruce titlebar pixmaps
|transparent | translucent-style fallback colours
|twm       | classic Debian twm maroon styling
|unsafe    | dark red unsafe titlebar pixmaps
|redmondxp | glossy blue Windows XP-style SVG textures
|redmond98 | flat blue/grey Windows 98-style SVG textures

The `ctwm` pack sets a 4x2 page grid and backgrounds for desk 0, and includes a
matching cowpager configuration. See [ctwm/README.md](ctwm/README.md).

The `twm` pack also includes a matching cowiconman configuration. Run it with:

```
    cowiconman -c /path/to/twm/cowiconman.conf
```
