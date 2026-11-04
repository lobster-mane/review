# Review

**A friendly PDF and image viewer for Linux, inspired by macOS Preview.**

Review is a native GNOME application for reading PDFs and images, signing and
annotating documents, organizing pages, and making quick image edits without a
full office suite. It is licensed under [GPL-3.0-or-later](LICENSE).

![Review showing a PDF with page thumbnails, highlights, freehand notes, and annotation tools](screenshots/app.png)

## Features

- Read PDFs and common image formats.
- Scroll continuously through PDF pages.
- Highlight text and add text, freehand ink, rectangles, ellipses, and arrows.
- Draw a reusable signature, or import a photo of one.
- Move, resize, recolor, or delete annotations; save editable or flattened copies.
- Reorder, duplicate, rotate, delete, cut, copy, and paste PDF pages.
- Import pages from another PDF and move pages between windows.
- Convert pages to 300 DPI color images, or securely crop them.
- Open each document in its own window.

## Install

### Ubuntu and Debian

Add the repository by following [mikakalevi.com/repo](https://mikakalevi.com/repo), then:

```sh
sudo apt install review
```

Requires Ubuntu 24.04 or newer on `amd64`, with a GNOME-compatible desktop.
Remove it with `sudo apt remove review`; saved signatures and configuration are
kept.

### Nix

The repository includes a packaging-only flake (there is no flake devShell).
With flakes enabled, from a checkout:

```sh
nix run .# -- document.pdf
nix build .#          # binary at ./result/bin/review
nix profile install .#
```

The flake supplies the GTK schemas, MIME data, and `qpdf` that Review needs at
runtime. For development on NixOS, use `nix-shell` with `shell.nix` instead.

## Getting started

1. Open **Review** from the application menu, or run `review file.pdf`.
2. Choose **Open…** to select a PDF or image.
3. Use the thumbnail sidebar to move through pages.
4. Use the toolbar to annotate, sign, or crop.
5. Choose **Save** to update the file or **Save As…** to create a copy.

Scroll down at the bottom of a page to open the next page, or up at the top to
open the previous one. The sidebar selection and page counter follow the visible
page.

## Signatures

Draw a signature once with **Draw Signature…**, or import an image with
**Import Signature…**. Review removes the near-white background from imported
images; dark ink on clean, evenly lit white paper works best.

Select the signature button in the annotation toolbar to place a saved signature,
then use the selection tool to move or resize it.

Signatures are stored per user under `$XDG_DATA_HOME/review/signatures`, normally
`~/.local/share/review/signatures`. Review restricts these files to the owner but
does not encrypt them.

> **Important:** this is a visual signature, not a certificate-backed
> cryptographic signature. Whether it is suitable depends on the document and
> the receiving organization.

## Annotating

Review includes highlight, text, freehand ink, rectangle, ellipse, arrow, and
signature tools. A tool stays active after use, so you can add several
annotations in a row.

Use the **hand tool** to pan without editing. Use the **selection tool** to
select an annotation, move or resize it, edit its text, or change its colors and
opacity. Press `Delete` to remove the selected annotation.

A normal **Save** keeps supported annotations editable in other PDF viewers.
Choose **Export Flattened Copy…** to paint the visible result permanently into
the page, for example when sending it to software that handles annotations
poorly.

## Pages and images

Select one or more thumbnails, then use the **Pages** menu to cut, copy, paste,
duplicate, delete, rotate, convert to a scanned page, or securely crop. Use
`Ctrl` for individual pages and `Shift` for a range. Drag selected pages to
reorder them, or drop a PDF into the sidebar to import its pages.

**Convert to Scanned Page** replaces selected pages with 300 DPI color images.
It bakes in visible annotations and removes text, links, forms, and other
interactive content from those pages. It is not OCR.

**Secure Crop** renders only the chosen rectangle into a new page, so content
outside it is removed rather than merely hidden. It is not a general redaction
tool for several unrelated areas in the middle of a page.

Both operations are destructive; use **Save As…** or **Export Flattened Copy…**
to keep the original.

Review opens PNG, JPEG, WebP, TIFF, BMP, and GIF images (depending on the
installed image libraries) and supports zoom, pan, rotate, crop, and export.

## Saving safely

Review writes PDF changes to a temporary file, validates the result, and then
replaces the destination. If saving fails, the original file is left untouched.
Undo and redo work while the document is open; closing it ends the editing
session.

Password-protected PDFs and PDFs containing cryptographic signatures open in
view-only mode. PDF forms are displayed but cannot be edited in Review 0.1.

## Keyboard shortcuts

| Action | Shortcut |
| --- | --- |
| Open | `Ctrl+O` |
| Save | `Ctrl+S` |
| Save As | `Ctrl+Shift+S` |
| Undo | `Ctrl+Z` |
| Redo | `Ctrl+Shift+Z` |
| Copy / cut / paste pages | `Ctrl+C` / `Ctrl+X` / `Ctrl+V` |
| Zoom in / out | `Ctrl++` / `Ctrl+-` |
| Print | `Ctrl+P` |
| New window | `Ctrl+N` |
| Quit | `Ctrl+Q` |

## Troubleshooting

- **Review is missing from the menu:** log out and back in, or run
  `/usr/bin/review`.
- **Files open in another viewer:** use **Open With → Review** and set it as the
  default for that file type.
- **A signature has a visible background:** import a tightly cropped image with
  dark ink on an evenly lit white background.
- **A recipient sees annotations incorrectly:** send a flattened copy.
- **The original must stay unchanged:** use **Save As…**, especially before
  Secure Crop or Convert to Scanned Page.

## Development

Review is written in C++20 and uses GTK4, libadwaita, Poppler, Cairo, GdkPixbuf,
qpdf, Meson, and debhelper.

### Build on Ubuntu

```sh
sudo apt install build-essential meson ninja-build pkg-config \
  libgtk-4-dev libadwaita-1-dev libpoppler-glib-dev libcairo2-dev \
  libgdk-pixbuf-2.0-dev qpdf
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
./build/src/review
```

On NixOS, enter `nix-shell` (see `shell.nix`) for Meson, Ninja, the compiler,
and `qpdf`. The flake is packaging-only; use the shell for development.

### Build the Debian package

```sh
sudo apt build-dep .
dpkg-buildpackage -us -uc -b
```

The package and executable are both named `review`. Files on the command line
open in separate windows:

```sh
review document.pdf picture.png
```

## Project information

- Application ID: `io.github.mika.Review`
- Package and executable: `review`
- Repository: [github.com/mika/review](https://github.com/mika/review)
- Author: [Mika Hämäläinen](https://mikakalevi.com)
- License: [GPL-3.0-or-later](LICENSE)

Review is an independent Linux application inspired by macOS Preview. It is not
affiliated with Apple.
