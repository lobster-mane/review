project(
  'review',
  'cpp',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

gnome = import('gnome')

gtk = dependency('gtk4', version: '>=4.14')
adwaita = dependency('libadwaita-1', version: '>=1.5')
poppler = dependency('poppler-glib', version: '>=24.02')
cairo = dependency('cairo', version: '>=1.16')
gdk_pixbuf = dependency('gdk-pixbuf-2.0', version: '>=2.42')
gio = dependency('gio-2.0')
glib = dependency('glib-2.0')

review_deps = [
  gtk,
  adwaita,
  poppler,
  cairo,
  gdk_pixbuf,
  gio,
  glib,
]

review_resources = gnome.compile_resources(
  'review-resources',
  'data/io.github.mika.Review.gresource.xml',
  source_dir: 'data',
  c_name: 'review_resources',
)

subdir('data')
subdir('src')
subdir('tests')
