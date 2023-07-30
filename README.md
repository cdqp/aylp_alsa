Anyloop plugin for sound card output via ALSA
=============================================


libaylp dependency
------------------

Symlink or copy the `libaylp` directory from anyloop to `libaylp`. For example:

```sh
ln -s $HOME/git/anyloop/libaylp libaylp
```


alsa dependency
---------------

Install `alsa-lib` (Archlinux), `libasound2` and `libasound2-dev` (Raspbian),
or whatever else the package is called.


Building
--------

Use meson:

```sh
meson setup build
meson compile -C build
```

