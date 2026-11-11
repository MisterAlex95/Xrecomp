// Stand-in for DXVK's src/dxvk/dxvk_device.h for the LSFG translator (see dxvk_shader.h): src/dxbc/dxbc_options.h
// only needs the names. Part of the DXVK-derived shim (zlib, NOTICE).
#pragma once

#include <vulkan/vulkan.h>

#include "../util/util_flags.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {
  class DxvkDevice;
  class DxvkAdapter;
}
