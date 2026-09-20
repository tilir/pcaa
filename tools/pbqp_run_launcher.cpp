// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Launches the SystemC graph runner with its non-interactive banner disabled.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include <unistd.h>

#ifndef PCAA_GRAPH_RUN_MODEL_NAME
#define PCAA_GRAPH_RUN_MODEL_NAME "pcaa_graph_run_model"
#endif

int main(int argc, char **argv) {
  const std::string invoked_as = argv[0];
  const size_t separator = invoked_as.rfind('/');
  if (separator == std::string::npos) {
    std::cerr << "pcaa_graph_run must be invoked with a path\n";
    return 127;
  }

  const std::string runner = invoked_as.substr(0, separator + 1) + PCAA_GRAPH_RUN_MODEL_NAME;
  setenv("SYSTEMC_DISABLE_COPYRIGHT_MESSAGE", "1", 0);
  argv[0] = const_cast<char *>(runner.c_str());
  execv(runner.c_str(), argv);
  std::perror("could not start PCAA graph runner");
  return 127;
}
