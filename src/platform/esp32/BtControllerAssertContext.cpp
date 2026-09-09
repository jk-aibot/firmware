#include <cstdint>

#include "architecture.h"
#include "sdkconfig.h"

#if defined(ARCH_ESP32)

extern "C" int esp_rom_printf(const char *format, ...);
extern "C" void __real_r_assert_err(const char *condition, const char *file, int line);

#if defined(CONFIG_IDF_TARGET_ESP32)
extern "C" void *ld_sco_env[];
extern "C" uint8_t sw_to_hw[];

namespace
{
constexpr uintptr_t SCO_AUDIO_ASSERT_RETURN_PC = 0x40037edcU;
constexpr uint8_t DISABLED_SCO_SLOT = UINT8_MAX;
bool spuriousScoIsrReported;

bool isSpuriousScoAudioIsrAssert(uintptr_t callerPc, int line)
{
    return callerPc == SCO_AUDIO_ASSERT_RETURN_PC && line == 7098 && ld_sco_env[0] == nullptr && ld_sco_env[1] == nullptr &&
           ld_sco_env[2] == nullptr && sw_to_hw[14] == DISABLED_SCO_SLOT;
}
} // namespace
#endif

extern "C" void __wrap_r_assert_err(const char *condition, const char *file, int line)
{
    const uintptr_t callerRaw = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t callerPc = (callerRaw & 0x3fffffffU) | 0x40000000U;

#if defined(CONFIG_IDF_TARGET_ESP32)
    // ROM r_ld_sco_audio_isr calls this assertion and immediately returns when its SCO context is null. Meshtastic runs the
    // original ESP32 controller in BLE-only mode, so an all-null SCO table with disabled slot routing is a spurious dispatch.
    if (isSpuriousScoAudioIsrAssert(callerPc, line)) {
        if (!spuriousScoIsrReported) {
            spuriousScoIsrReported = true;
            esp_rom_printf("BT_CTRL_SCO_ISR_IGNORED caller=0x%08x\n", static_cast<unsigned>(callerPc));
        }
        return;
    }
#endif

    esp_rom_printf("BT_CTRL_ASSERT caller=0x%08x file=%s line=%d cond=%s\n", static_cast<unsigned>(callerPc),
                   file != nullptr ? file : "?", line, condition != nullptr ? condition : "?");
#if defined(CONFIG_IDF_TARGET_ESP32)
    if (line == 7098) {
        esp_rom_printf("BT_CTRL_SCO_CONTEXT env=%08x,%08x,%08x sw_to_hw14=%u\n",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(ld_sco_env[0])),
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(ld_sco_env[1])),
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(ld_sco_env[2])), static_cast<unsigned>(sw_to_hw[14]));
    }
#endif
    __real_r_assert_err(condition, file, line);
}

#endif
