// DXBC -> SPIR-V through DXVK's DXBC compiler (lsfg_dxbc.h). Ours; calls DXVK's src/dxbc as a library, the way
// lsfg-vk 1.0 did with its DXVK subset: read the container, compile it, take the SPIR-V. The bindings are fixed up
// afterwards by lsfg_spv_renumber() (the cache does it), not here.
#include "lsfg_dxbc.h"

#include "dxbc/dxbc_module.h"
#include "dxbc/dxbc_reader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifndef LSFG_DXVK_VERSION
#define LSFG_DXVK_VERSION "unknown"
#endif

extern "C" int lsfg_dxbc_translate(const uint8_t *dxbc, size_t len, uint32_t base_id, uint8_t **spv,
                                   size_t *spv_len, char *err, size_t errlen, void *user) {
    (void)base_id;
    (void)user;
    *spv = nullptr;
    *spv_len = 0;
    try {
        dxvk::DxbcReader reader(reinterpret_cast<const char *>(dxbc), len);
        dxvk::DxbcModule module(reader);
        auto prog = module.programInfo();
        if (!prog || prog->type() != dxvk::DxbcProgramType::ComputeShader) {
            std::snprintf(err, errlen, "not a compute shader");
            return 1;
        }
        const dxvk::DxbcModuleInfo info{};
        dxvk::Rc<dxvk::DxvkShader> shader = module.compile(info, "lsfg");
        const dxvk::SpirvCodeBuffer &code = shader->code();
        size_t bytes = code.size();
        auto *out = static_cast<uint8_t *>(std::malloc(bytes ? bytes : 1));
        if (!out) {
            std::snprintf(err, errlen, "out of memory");
            return 1;
        }
        std::memcpy(out, code.data(), bytes);
        *spv = out;
        *spv_len = bytes;
        return 0;
    } catch (const dxvk::DxvkError &e) {
        std::snprintf(err, errlen, "%s", e.message().c_str());
    } catch (const std::exception &e) {
        std::snprintf(err, errlen, "%s", e.what());
    } catch (...) {
        std::snprintf(err, errlen, "unknown exception");
    }
    return 1;
}

extern "C" const char *lsfg_dxbc_translator_name(void) {
    return "dxvk-" LSFG_DXVK_VERSION;
}
