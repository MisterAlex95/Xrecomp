// Stand-in for DXVK's src/dxvk/dxvk_shader.h when DXVK's DXBC compiler (src/dxbc) is built alone for the LSFG
// translator (src/framegen/lsfg_dxbc.cmake stages it next to DXVK's src/dxbc, src/spirv and src/util).
//
// Declares only what src/dxbc of DXVK v2.6.2 uses: DxvkAccessOp, DxvkBindingInfo, DxvkShaderCreateInfo and a
// DxvkShader that simply keeps the compiled SPIR-V. The type layouts follow DXVK's own declarations
// (src/dxvk/dxvk_pipelayout.h, dxvk_shader.h, dxvk_limits.h of v2.6.2), so this file is a derivative of DXVK:
// zlib licence, Copyright (c) 2017 Philip Rebohle, 2019 Joshua Ashton, 2019 Robin Kertels, 2023 Jeffrey
// Ellison (NOTICE); altered: reduced to the declarations the DXBC compiler needs. No pipeline, device
// or descriptor machinery: none is needed to turn DXBC into SPIR-V.
#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "../spirv/spirv_code_buffer.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/util_bit.h"

namespace dxvk::bit {
  // macOS: size_t (unsigned long) is neither uint32_t nor uint64_t (unsigned long long), so util_small_vector.h's
  // lzcnt(size_t) is ambiguous there. Linux and Android never see this overload.
  template<typename T, std::enable_if_t<std::is_same_v<T, unsigned long> && !std::is_same_v<unsigned long, uint64_t>
                                        && !std::is_same_v<unsigned long, uint32_t>, int> = 0>
  inline uint32_t lzcnt(T n) { return lzcnt(uint64_t(n)); }
}

#include "../util/util_small_vector.h"

namespace dxvk {

  enum DxvkLimits : uint32_t {
    MaxNumRenderTargets = 8,
    MaxNumXfbBuffers    = 4,
    MaxNumXfbStreams    = 4,
  };

  struct DxvkAccessOp {
    enum OpType : uint16_t {
      None    = 0x0u,
      Or      = 0x1u,
      And     = 0x2u,
      Xor     = 0x3u,
      Add     = 0x4u,
      IMin    = 0x5u,
      IMax    = 0x6u,
      UMin    = 0x7u,
      UMax    = 0x8u,
      StoreF  = 0xdu,
      StoreUi = 0xeu,
      StoreSi = 0xfu,
    };

    DxvkAccessOp() = default;
    DxvkAccessOp(OpType t) : op(uint16_t(t)) { }
    DxvkAccessOp(OpType t, uint16_t constant) : op(uint16_t(uint16_t(t) | (constant << 4u))) { }

    uint16_t op = 0u;

    bool operator == (const DxvkAccessOp& t) const { return op == t.op; }
    bool operator != (const DxvkAccessOp& t) const { return op != t.op; }

    template<typename T, std::enable_if_t<std::is_integral_v<T>, bool> = true>
    explicit operator T() const { return op; }
  };

  struct DxvkBindingInfo {
    VkDescriptorType      descriptorType  = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t              resourceBinding = 0u;
    VkImageViewType       viewType        = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    VkShaderStageFlagBits stage           = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    VkAccessFlags         access          = 0u;
    DxvkAccessOp          accessOp        = DxvkAccessOp::None;
    bool                  uboSet          = false;
    bool                  isMultisampled  = false;
  };

  struct DxvkShaderCreateInfo {
    VkShaderStageFlagBits stage;
    uint32_t bindingCount = 0;
    const DxvkBindingInfo* bindings = nullptr;
    uint32_t inputMask = 0;
    uint32_t outputMask = 0;
    uint32_t flatShadingInputs = 0;
    VkShaderStageFlags pushConstStages = 0;
    uint32_t pushConstSize = 0;
    int32_t xfbRasterizedStream = 0;
    uint32_t patchVertexCount = 0;
    uint32_t xfbStrides[MaxNumXfbBuffers] = { };
    VkPrimitiveTopology inputTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    VkPrimitiveTopology outputTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  };

  // The compiled shader: the SPIR-V and a copy of the binding list (the compiler's own goes away with it).
  class DxvkShader : public RcObject {
  public:
    DxvkShader(const DxvkShaderCreateInfo& info, SpirvCodeBuffer&& code)
    : m_info(info), m_code(std::move(code)),
      m_bindings(info.bindings, info.bindings + info.bindingCount) {
      m_info.bindings = m_bindings.data();
    }

    const DxvkShaderCreateInfo& info() const { return m_info; }
    const SpirvCodeBuffer& code() const { return m_code; }
    const std::vector<DxvkBindingInfo>& bindings() const { return m_bindings; }

  private:
    DxvkShaderCreateInfo m_info;
    SpirvCodeBuffer m_code;
    std::vector<DxvkBindingInfo> m_bindings;
  };

}
