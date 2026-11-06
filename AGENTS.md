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
  `$XDG_CONFIG_HOME/review` / `$XDG_DATA_HOME/review`. The toolbar's signature
  menu is rebuilt by `rebuild_signature_menu()` and places entries through
  `insert_signature()`.
- `data/` — `io.github.mika.Review.gresource.xml` is compiled into the binary.
  Add new `data/ui` or `data/icons/scalable/actions` resources there; install
  rules are in `data/meson.build`.
- Multi-window is one process. The page clipboard and drag state are
  file-scope globals in `review-window.cpp`, shared across windows.
- Continuous mode renders all pages in one drawing area. `page_top` and
  `page_at_canvas_y` are the layout source of truth; keep drawing, drag
  mapping, and scroll sync consistent with them.
- Single-page mode uses the edge-scroll velocity thresholds for page turns.
  Continuous mode must return before that page-turn logic and let the scrolled
  window handle scrolling normally.
- The sidebar supports multi-page selection (`Ctrl`/`Shift`). `set_current_page`
  must not clear a multi-row selection; batch actions consume `selected_pages()`.
- Dragging an annotation across a page boundary in continuous mode transfers it
  with `Document::move_annotation()`; keep the source/target page offsets in
  sync with `page_top` and the centered page x offset.
- The highlight tool is a text selector. `Document::highlight_rects()` maps the
  drag start/end points to continuous character ranges in Poppler's text
  layout, groups them into per-line rectangles, stores them in
  `Annotation::text_rects`, and writes them as multiple highlight quads. The
  character layout is cached per page and invalidated by `revision()`; keep it
  cached so live highlight previews stay smooth. `Document::erase_highlights()`
  subtracts a new selection from existing highlights so the marker acts as a
  toggle.
- Pinch zoom is handled by a `GtkGestureZoom` on the canvas. It keeps the page
  point under the gesture in place and switches off fit width. While pinching,
  `cached_page()` reuses the previous zoom's surfaces and `draw_page()` scales
  them; rerender at the final zoom only when the gesture ends. On a touchscreen,
  a one-finger drag on empty canvas with the selection tool pans the scrolled
  window instead of selecting.
- Per-tool stroke/fill defaults live in `Window::tool_colors_`; the marker
  (highlight) defaults to translucent yellow and other tools to black with a
  transparent fill. Color controls are shown through `property_revealer_`.
- Signatures are inserted at the center of the current view; compute the view
  center with the canvas bounds rather than assuming the page is aligned with
  the viewport. Signature resize handles preserve the annotation's aspect
  ratio.
- Rendered page surfaces are cached in `Window`; `Document::revision()` and
  the zoom level invalidate the cache. Call `Document::invalidate_rendering()`
  for live annotation mutations (moves/resizes) so cached text follows the box.
  Missing full-resolution pages render in idle time and are painted as plain
  white page placeholders until ready. The canvas paints only the visible
  slice of its very tall drawing area, and scroll events queue full canvas and
  overlay redraws so partial damage cannot leave dark regions. Set
  `REVIEW_DEBUG_LOADING=1` to log scroll/draw/cache/render timing to stderr.
- Inline text editing is an overlay inside `canvas_overlay_`. Call
  `finish_inline_text_edit()` before changing page, zoom, view mode, or layout.
  Newly typed text inherits the current typing style through the editor's
  `insert-text` handler.

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
