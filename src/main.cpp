#include "review-application.hpp"

int main(int argc, char** argv) {
  AdwApplication* app = review::application_new();
  const int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
