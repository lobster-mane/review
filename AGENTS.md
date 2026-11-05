# AGENTS.md

Review is a native GTK4/libadwaita PDF and image viewer written in C++20. One
executable, `review`. Most UI is built directly in C++; only the main menu is a
GtkBuilder file (`data/ui/menu.ui`).

## Build and run

`build/` is gitignored and already configured in this checkout; prefer
incremental builds.

```sh
meson compile -C build            # or: meson setup build && meson compile -C build
./build/src/review file.pdf       # needs a graphical session
```

On this NixOS checkout `meson`, `ninja`, `g++`, and `qpdf` are not on the
default PATH, and the flake is packaging-only (no devShell). Use `nix build .#`
to build and test the package, or start an interactive environment with:

```sh
nix-shell -p meson ninja pkg-config gcc qpdf gtk4 libadwaita poppler cairo gdk-pixbuf glib
```

When running a locally built binary, set `GSETTINGS_SCHEMA_DIR` to include
GTK4's `glib-2.0/schemas` directory; otherwise GTK aborts when opening a color
picker (`org.gtk.gtk4.Settings.ColorChooser` is missing). The packaged wrapper
handles this automatically. On Ubuntu, install the dependencies listed in
`README.md`.

Packaged usage: `nix build .#` or `nix run .#`. Nix flakes only see files
tracked by git.

Debian package: `dpkg-buildpackage -us -uc -b` (deps in `debian/control`).

## Tests

```sh
meson test -C build --print-errorlogs
./build/tests/document-test -l                                  # list cases
./build/tests/document-test -p /document/save-annotations       # single case
```

Tests are headless and only exercise `Document` (`src/review-document.cpp`) —
there is no UI test suite. They require `qpdf` on PATH and create fixtures in
the system temp directory. Set `REVIEW_KEEP_TEST_OUTPUT=1` to keep generated
PDFs. `tests/document-test.cpp` compiles `review-document.cpp` directly, so any
new model `.cpp` must be added to `tests/meson.build` as well as
`src/meson.build`.

## Where things live

- `src/review-document.*` — UI-independent model: Poppler loading/rendering,
  in-memory page and annotation state, undo/redo, and PDF/image saving.
- `src/review-window.*` — all GTK/libadwaita widgets, actions, shortcuts, page
  sidebar, canvas gestures, and dialogs.
- `src/signature-store.*` — reusable signatures under
  `$XDG_DATA_HOME/review/signatures`; preferences and recents live under
  `$XDG_CONFIG_HOME/review` / `$XDG_DATA_HOME/review`.
- `data/` — `io.github.mika.Review.gresource.xml` is compiled into the binary.
  Add new `data/ui` or `data/icons/scalable/actions` resources there; install
  rules are in `data/meson.build`.
- Multi-window is one process. The page clipboard and drag state are
  file-scope globals in `review-window.cpp`, shared across windows.
- Continuous mode renders all pages in one drawing area. `page_top` and
  `page_at_canvas_y` are the layout source of truth; keep drawing, drag
  mapping, and scroll sync consistent with them.
- Inline text editing is an overlay inside `canvas_overlay_`. Call
  `finish_inline_text_edit()` before changing page, zoom, view mode, or layout.

## PDF and editing constraints

- Saving shells out to `qpdf` (`--empty --pages`, `--qdf`, `--check`). `qpdf`
  is a runtime dependency, not just a test dependency. Keep the temp-file,
  `qpdf --check`, and atomic-rename flow in `Document::save`.
- Canvas coordinates are top-left based; PDF annotation coordinates are
  bottom-left. Conversion happens in `write_pdf_annotations`.
- Text formatting lives on `Annotation`; per-word overrides are stored in
  `text_spans`. `review-text.hpp` is the shared Pango layout/wrap code, and
  `render_page` accepts a skip-annotation id so the inline editor can hide the
  document copy and draw itself in place. The editor uses GtkTextTags and
  extracts spans on commit. Keep `fit_text_bounds`, `draw_annotations`, the
  editor tag logic, and the FreeText `/DA` export in sync when changing it.
- `Document` holds page references into source files; page edits are
  serialized at save time, and rasterized/secure-cropped pages are rebuilt
  from renders. Secure crop and rasterize are destructive.
- `HAVE_POPPLER_INK_API` and `HAVE_POPPLER_BORDER_WIDTH` are never defined by
  `meson.build`, so ink is saved as a stamp fallback. The test accepts either;
  define these in cpp args only if you intentionally change that behavior.
- There is no formatter, linter, CI, or `-Werror`. Match the existing style
  (2-space indent, `review` namespace, trailing-underscore members) and keep
  `meson.build` warning level at 2 unless there is a reason to change it.
