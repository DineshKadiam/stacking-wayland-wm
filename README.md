# CoW -- Compostitor On Wayland

`cow` is a stacking window manager.  Unlike other compositors based on
wlroots, `cow` is a window manager using [river](https://codeberg.org/river/river)
as the compositor.

This allows for more focus on writing the window manager part without having
to worry too much about wayland protocol handling, etc., effectively lumping
that in with being a wayland display server.

## Inspiration

`cow`'s aims are to provide the following:

* The look-and-feel of `fvwm` and `mwm`.
* To provide a sensible mechanism for configuration, divorcing the need for
  complex configuration syntaxes, instead using dedicated commands that can be
  used both as a configuration file, and via IPC (runtime).

The commands are inspired by tmux(1).

## Where to Start?

There's been an attempt at some documentation:

* `cow(1)` -- documents the commands/config settings which can be used to
  configure `cow`.
* `moocow(1)` -- the CLI used to communicate with `cow` from shell-scripts,
  etc.
* `cowbar(1)` -- JSON structure of the state of `cow` -- primarily used to
  provide information to `waybar`, but can be used for any bar.

More concretely though, have a look at:

* [cow config file](./config/cow.conf) -- this is documented and should help
  you start making changes.  Copy it to `~/.config/cow` to get started making
  changes,
* [waybar](./config/waybar/) -- this can be copied verbatim to `~/.config/` if
  desired to it can be configured to your liking.

## What does it look like?

![](./cow-ss.png)

There's also support for containers -- a top-level window which can group
windows together into tabs.

![](./cow-container.png)

## How do I build CoW?

Because `cow` is a window manager reliant on `river`, then the binary for
`river` is required:

* [river](https://codeberg.org/river/river)

For the wayland parts:

* wayland_client
* xkbcommon
* pangocairo
* cairo
* libbsd (for non-BSD systems; linux, etc.)

Note that the names of the libraries above will vary across systems.

Documentation is written in [scdoc](https://git.sr.ht/~sircmpwn/scdoc):

* scdoc

The build system itself uses `meson`, and hence the following command will
configure, build, and install `cow`:

```
meson setup build
meson compile -C build && meson install -C build
```

## How do I run CoW?

There's a command called `cow-start` which you can run thus:

```
river -c 'cow-start'
```

This is via a tty.

Alternatively, a globally-installed `cow.desktop` file is installed allowing
cow to be used with wayland login managers.

## Contact and Support

You can email me at: thomas at xteddy dot org

I've setup an IRC channel on `libra.chat` -- `#cow-wayland`

IRC is preferred over other forms of communication.  Maybe I'll see you there?

-----

# FAQ

Here's some of the questions that have never been asked, frequently or
otherwise...

## Why is this called Cow?

Because originally, `cow` was going to be an all-in-one Compositor On Wayland
(CoW), but now that it uses river to do all the heavy-lifting, this nae is a
slight misnomer.

That said though, I like cows and, I'm going to stick with that name for now.

## Moocow?  Seriously?

Well, the way I see it, if you want `cow` to do anything, you need to "moo
(at) cow", so `moocow` as an external command seemed appropriate.

## Are multi-monitors supported?

Yes.  For now, one will need to edit [the start script](./tools/start.sh) to
run `wlr-randr` with the appropriate values.

Note that this is controlled via river(1) and `cow` has no means of being able
to set monitors directly.  To get a more visual overview of things, the
`wdisplays` command is helpful, and is similar to `arandr` from xrandr.
