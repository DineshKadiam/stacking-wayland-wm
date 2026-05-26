# Build requirements

`cow` itself is built with `meson` and a C99-capable compiler.  The
following runtime dependencies are linked into the produced binary:

* `wayland-client`
* `xkbcommon`
* `pangocairo` (and `cairo`)
* `libbsd` on systems without native BSD libc extensions (most Linux
  distributions need this; FreeBSD does not)
* `scdoc` for generating man pages

In addition, two build-host tools are required so the embedded
scripting language used by `script { ... }` blocks can be compiled:

* `bison` (>= 3.0) -- generates the script parser
* `flex`  (>= 2.6) -- generates the script lexer

Both must be available on the build host; they are not needed at
runtime.  Typical installation:

| System         | Command                                         |
|----------------|-------------------------------------------------|
| Debian/Ubuntu  | `apt install bison flex`                        |
| Fedora/RHEL    | `dnf install bison flex`                        |
| Arch           | `pacman -S bison flex`                          |
| FreeBSD        | `pkg install bison flex`                        |
| macOS (brew)   | `brew install bison flex`                       |

The configure step (`meson setup build`) will fail with a clear
error if either is missing.

## Quick build

```
meson setup build
meson compile -C build
meson install -C build
```

## Running tests

There are no automated tests at present; functional verification is
done by running cow under a nested compositor (e.g. via `wayvnc`)
and exercising the relevant commands.
