# Examples

This directory contains:

- `river/`:
 - `init`: runs after River starts. Contains examples for a fixed output
   layout or starting kanshi, followed by CoW. Variables needed by River
   itself must be set in the environment that launches River.

- `cow/`:
 - `cow.conf` -- an example configuration, and one which is installed
   system-wide.  It is suggested to copy this to `~/.config/cow/` (create as
   appropriate) and edit there for any user changes.
 - `cowbar.conf` -- the default styles and formats used by cowbar's Waybar
   output.  A user copy may override individual settings.

- `waybar/` -- scripts and config to show how to extract information out of
  cow, and rendered into `waybar`.
