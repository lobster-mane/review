document_test = executable(
  'document-test',
  'document-test.cpp',
  files('../src/review-document.cpp'),
  include_directories: include_directories('../src'),
  dependencies: review_deps,
  install: false,
)

test(
  'document',
  document_test,
  timeout: 120,
)
