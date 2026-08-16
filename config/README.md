# Examples

This directory contains:

- `river/`:
 - `init`: this is the configuration file for the river compositor.  Useful if
   you need to set any environment variables before river starts.  In this
   case, useful for screen-sharing.

- `cow/`:
 - `cow.conf` -- an example configuration, and one which is installed
   system-wide.  It is suggested to copy this to `~/.config/cow/` (create as
   appropriate) and edit there for any user changes.
 - `cowbar.conf` -- the default styles and formats used by cowbar's Waybar
   output.  A user copy may override individual settings.
 - `after-start.sh` -- example script showing how to run commands once cow has
   started.  Useful for configuration output layouts.

- `waybar/` -- scripts and config to show how to extract information out of
  cow, and rendered into `waybar`.
