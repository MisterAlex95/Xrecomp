// The two functions of DXVK's src/util/util_env.cpp its logger needs (getEnvVar, getExeBaseName) and the logger
// instance (DXVK defines it in each API's *_main.cpp), in place of that
// file for the LSFG translator: its thread-naming code does not build on macOS. Part of the DXVK-derived shim
// (zlib, NOTICE).
#include <cstdlib>
#include <string>

#include "../util/util_env.h"

namespace dxvk::env {

  std::string getEnvVar(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
  }

  std::string getExeBaseName() {
    return "lsfg_translate";
  }

}

#include "../util/log/log.h"

namespace dxvk {

  // DXVK defines its logger in each API's entry file (d3d11_main.cpp, ...); the translator has none.
  Logger Logger::s_instance("lsfg_translate.log");

}
