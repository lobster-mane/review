review_sources = files(
  'main.cpp',
  'review-application.cpp',
  'review-document.cpp',
  'review-window.cpp',
  'signature-store.cpp',
)

executable(
  'review',
  review_sources,
  review_resources,
  include_directories: include_directories('.'),
  dependencies: review_deps,
  install: true,
)
