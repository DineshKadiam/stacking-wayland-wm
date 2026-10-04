# CoW -- Compositor on Wayland

<p align="center">
  <img src="./logos/CoW-final.svg">
</p>

<p align="center">
  <a href="https://cow-wm.codeberg.page/cow/">Website</a> ·
  <a href="https://codeberg.org/cow-wm/cow/wiki/Home">Wiki</a> ·
  <a href="https://codeberg.org/cow-wm/cow/issues">Issues</a>
</p>

`cow` is a stacking window manager. Unlike other compositors based on wlroots,
`cow` is a window manager using [river](https://codeberg.org/river/river) as
the compositor.

This allows for more focus on writing the window manager part without having to
worry too much about wayland protocol handling, etc., effectively lumping that
in with being a wayland display server.

## Inspiration

`cow`'s aims are to provide the following:

* The look-and-feel of `fvwm` and `mwm`.
* To provide a sensible mechanism for configuration, divorcing the need for
  complex configuration syntaxes, instead using dedicated commands that can be
  used both as a configuration file, and via IPC (runtime).

The commands are inspired by tmux(1).

## Architecture / Design

See [this document](./architecture.md) which explains how CoW and River work
together, and hence the overall technical design considerations.

## What does it look like?

![](./ss/cow-ss.png)

In this screenshot you can see:

* The focused window in teal, is also a container window which has the shell,
  and midnight commander in the next tab.
* cowpager: an overview of the current desk, showing window positions and the
  highlighted page.
* cowbuttons: a mock-up of a laucher.
* [wteddy](https://codeberg.org/thomasadam/wteddy): an xteddy clone.
* cowident: showing information about wteddy.
* cowbuttons: a telephone widget
* cowiconman: an overview of windows on this screen; note iconified windows are
  shown, indicated by the grey box.
* An open menu, selecting this would allow the user to pick a window to operate
  on.
* Two icons on the bottom left.

The window labelled `[9c3368e4] xterm` is actually owned by desktop 1 (as
indicated by `waybar` at top of the screen. The `1` button with an orange
background is showing windows from that desk. The window labelled
`[9c3368e4] xterm` is actually owned by desktop 1 (as indicated by `waybar` at
top of the screen. The `1` button with an orange background is showing windows
from that desk. See the `-c` option to the `desk` command.

## How do I build CoW?

### Nix development environment

The flake provides the compiler, build dependencies, manual-page tools, and
Markdown formatter used by the project. From the repository root, enter it
with:

```
nix develop
```

To run a single command without opening a shell, use, for example:

```
nix develop --command meson setup build
```

The older `nix-shell` interface is also supported:

```
nix-shell
```

No installation outside the Nix store is required. Enabling Nix's `flakes` and
`nix-command` experimental features may be necessary on older Nix
installations.

### Other development environments

Required packages:

* wayland_client
* xkbcommon
* pangocairo
* cairo
* libbsd (for non-BSD systems; linux, etc.)
* bison (>= 3.0)
* flex (>= 2.6)

Optional packages:

* lowdown

Note that the names of the libraries above will vary across systems.

The build system itself uses `meson`, and hence the following command will
configure, build, and install `cow`:

```
meson setup build
meson compile -C build && meson install -C build
```

Markdown sources are formatted to 79 columns by a small Python 3 tool which
only reflows blocks where doing so cannot change their meaning. Python 3 is
therefore the only additional formatting dependency on systems without Nix.
Format all Markdown sources, or merely check them, with:

```
tools/format-markdown.py
tools/format-markdown.py --check
```

Use this formatter rather than `mdformat` or another CommonMark formatter.
Generic formatters may remove escapes required by lowdown and alter the
generated manual pages.

Paragraphs are wrapped at 79 columns. Tables, headings, and literal examples
may remain wider where Markdown cannot split them safely.

Because `cow` is a window manager reliant on `river`, then the binary for
`river` is required:

* [river](https://codeberg.org/river/river)

## How do I run CoW?

Start CoW through River:

```
river -c cow
```

It's recommended that you copy the [river init file](config/river/init) to
`~/.config/river/init`, make it executable, and customise it so that you can
just run:

```
river
```

River runs this file after starting. Use it for session setup and output
configuration before launching CoW. Environment variables needed by River
itself, such as `XKB_DEFAULT_LAYOUT`, must be set before launching River.

Alternatively, a globally-installed `cow.desktop` file is installed allowing
cow to be used with wayland login managers.

## CoW Themes

CoW has support for themes -- see [the themes directory](examples/themes) for
more information about how to use these. They're currently a feeble and
incomplete attempt at converting
[fvwm-themes to cow](https://fvwm-themes.sourceforge.net/windowdecors/)

## Waybar

*CoW* has no support for a bar, like waybar. For that, see the
[example file](./config/waybar/) -- this can be copied verbatim to `~/.config/`
if desired to it can be configured to your liking.

It is also suggested to look at cowbar(1) for sending information to waybar.

## Wiki

See: https://codeberg.org/cow-wm/cow/wiki/Home

## Old Screenshots

Mostly for historical/development nostalgia:

See: https://codeberg.org/cow-wm/cow/wiki/Screenshots

## Packages

CoW is packaged by the following entries
[found on the wiki](https://codeberg.org/cow-wm/cow/wiki/Packages)

## Contact and Support

You can email me at: thomas at xteddy dot org

Or, use the IRC channel on `libera.chat` -- `#cow-wayland`

IRC is preferred over other forms of communication. Maybe I'll see you there?

-----

# FAQ

Here's some of the questions that have never been asked, frequently or
otherwise...

## Why is this called Cow?

Because originally, `cow` was going to be an all-in-one Compositor On Wayland
(CoW), but now that it uses river to do all the heavy-lifting, this name is a
slight misnomer.

That said though, I like cows and, I'm going to stick with that name for now.

## The cow logo isn't a cow.  Why?

Astute observers will have noticed that the logo on the Codeberg page actually
shows a Hereford bull, rather than a cow (which is female). Although this is
considered wholly incorrect use of grammar, I reserve the right to use the word
`cow` to mean both cows and bulls. Even though I should say "cattle".

For more information see:

https://bsd.network/web/@thomasadam/116766630953558702

Yes, this is all tongue-in-cheek.

:)

## Moocow?  Seriously?

Well, the way I see it, if you want `cow` to do anything, you need to "moo (at)
cow", so `moocow` as an external command seemed appropriate.

## Are multi-monitors supported?

Yes. Use one `wlr-randr` invocation in [River's init](config/river/init) to
configure a fixed layout before starting CoW, or use
[kanshi profiles](config/kanshi/config) for better management.

To get a more visual overview of things, the `wdisplays` command is helpful,
and is similar to `arandr` from xrandr.

## Is AI used in this project?

No, it is not.

# Logo and mascot.

Meet our mascot, Anvil the cow.

![](./logos/Only-CoW.svg)

Luis Sobalvarro kindly created the CoW logo for this project.


![](./logos/CoW-final-inverted.svg)

and...

![](./logos/CoW-final.svg)

Massive thanks to you, Luis!

These logos are released under the same licence as CoW itself -- that is to
say, they're free to use as long as attribution is given to Luis Sobalvarro.
