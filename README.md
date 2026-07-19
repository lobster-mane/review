# Review

Review is a native GNOME PDF and image viewer inspired by macOS Preview. The
0.1 release supports page organization, PDF annotations, reusable visual
signatures, secure raster cropping, and conversion of PDF pages to 300 DPI
full-color images.

## Build

Ubuntu 24.04 or newer is required.

```sh
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-4-dev libadwaita-1-dev libpoppler-glib-dev libcairo2-dev \
  libgdk-pixbuf-2.0-dev qpdf
meson setup build
meson compile -C build
meson test -C build
```

Run the uninstalled application with `./build/src/review`.

## Debian package

```sh
sudo apt build-dep .
dpkg-buildpackage -us -uc -b
```

The application intentionally treats encrypted and digitally signed PDFs as
view-only. Saved handwritten signatures are visual stamps, not cryptographic
signatures. They are stored unencrypted with owner-only filesystem permissions
under `$XDG_DATA_HOME/review/signatures`.

## License

GPL-3.0-or-later.
