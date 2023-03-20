# Godalming's DWL dotfiles

## Building
### Dependency;s
Dwl has only two dependencies: `wlroots` and `wayland-protocols`. Simply install these (and their `-devel` versions if your distro has separate development packages). You also need a c compiler (such as gcc). As well as `make` and `pkg-config` to build dwl.
EG for Arch Linux:
```
sudo pacman -S wlroots wayland-protocols
```
or for void linux:
```
sudo xbps-install -S wlroots-devel pkg-config make gcc mesa-dri
```
### Compiling
Now run `make` from the `src/` directry. If you wish to build with a Git version of wlroots, check out the [wlroots-next branch].

## Running dwl
Dwl can be run on any of the backends supported by wlroots. This means you can run it as a separate window inside either an X11 or Wayland session, as well as directly from a VT console. Depending on your distro's setup, you may need to add your user to the `video` and `input` groups before you can run dwl on a VT. If you are using `elogind` or `systemd-logind` you need to install polkit; otherwise you need to add yourself in the `seat` group and enable/start the seatd daemon.

Wayland requires a valid `XDG_RUNTIME_DIR`, which is usually set up by a
session manager such as `elogind` or `systemd-logind`.  If your system doesn't
do this automatically, you will need to configure it prior to launching `dwl`,
e.g.:
```
export XDG_RUNTIME_DIR=/run/user/$(id -u)
sudo mkdir -p $XDG_RUNTIME_DIR
dwl
```

## ToDo list
### ToDo
#### Short term
 - Their are a few bugs with the implementation of using one key to run a launcher and using keybinds that use that key as well
	- When you use mouse bindings the launcher is also ran
	- When switching VT's when you switch back the launcher has been ran
#### Long term
 - add tabs for the monocule layout
 - cleanup code
 - add a custom menu so I do not have to have one as a dependency
 - If I have made my own menu I expect I can also make a bar
 - And mayber add a dropdown menu terminal

## Acknowledgements
Dwl began by extending the TinyWL example provided (CC0) by the sway/wlroots
developers. This was made possible in many cases by looking at how sway
accomplished something, then trying to do the same in as suckless a way as
possible.

Many thanks to suckless.org and the dwm developers and community for the
inspiration, and to the various contributors to the project, including:

- Alexander Courtis for the XWayland implementation
- Guido Cella for the layer-shell protocol implementation, patch maintenance, and for helping to keep the project running
- Stivvo for output management and fullscreen support, and patch maintenance

[Wayland]: https://wayland.freedesktop.org/
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots/
