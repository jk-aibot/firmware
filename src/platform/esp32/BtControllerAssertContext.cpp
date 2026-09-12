#include <cstdint>

#include "architecture.h"
#include "sdkconfig.h"

#if defined(ARCH_ESP32)

extern "C" int esp_rom_printf(const char *format, ...);
extern "C" void __real_r_assert_err(const char *condition, const char *file, int line);
extern "C" void __real_r_assert_param(uint32_t param0, uint32_t param1, const char *file, int line);

#if defined(CONFIG_IDF_TARGET_ESP32)
extern "C" uint32_t ea_env[];
extern "C" void *ld_sco_env[];
extern "C" uint8_t sw_to_hw[];

namespace
{
constexpr uintptr_t EA_PROG_TIMER_ASSERT_RETURN_PC = 0x400150b6U;
constexpr uintptr_t EA_FINE_TARGET_INT_MASK_REG = 0x3ff7100cU;
constexpr uintptr_t EA_FINE_TARGET_REG = 0x3ff710b8U;
constexpr uintptr_t ESP32_DRAM_START = 0x3ffae000U;
constexpr uintptr_t ESP32_DRAM_LAST_RECORD = 0x3fffffe0U;
constexpr uintptr_t SCO_AUDIO_ASSERT_RETURN_PC = 0x40037edcU;
constexpr uint8_t DISABLED_SCO_SLOT = UINT8_MAX;
bool spuriousScoIsrReported;

bool isSpuriousScoAudioIsrAssert(uintptr_t callerPc, int line)
{
    return callerPc == SCO_AUDIO_ASSERT_RETURN_PC && line == 7098 && ld_sco_env[0] == nullptr && ld_sco_env[1] == nullptr &&
           ld_sco_env[2] == nullptr && sw_to_hw[14] == DISABLED_SCO_SLOT;
}

bool isEaProgTimerAssert(uintptr_t callerPc, int line)
{
    return callerPc == EA_PROG_TIMER_ASSERT_RETURN_PC && line == 497;
}

bool isReadableEaRecord(uintptr_t address)
{
    return address >= ESP32_DRAM_START && address <= ESP32_DRAM_LAST_RECORD;
}

uint32_t readReg32(uintptr_t address)
{
    return *reinterpret_cast<volatile const uint32_t *>(address);
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
    if (isEaProgTimerAssert(callerPc, line)) {
        const uintptr_t primary = ea_env[0];
        const uintptr_t secondary = ea_env[5];
        const uintptr_t active = ea_env[16];
        const auto primaryBytes = reinterpret_cast<volatile const uint8_t *>(primary);
        const auto secondaryBytes = reinterpret_cast<volatile const uint8_t *>(secondary);
        const auto activeWords = reinterpret_cast<volatile const uint32_t *>(active);
        const uint32_t missing = UINT32_MAX;

        esp_rom_printf("BT_CTRL_EA_CONTEXT off0=%08x off20=%08x off64=%08x\n", static_cast<unsigned>(primary),
                       static_cast<unsigned>(secondary), static_cast<unsigned>(active));
        esp_rom_printf("BT_CTRL_EA_TIMER mask=%08x target=%08x\n", static_cast<unsigned>(readReg32(EA_FINE_TARGET_INT_MASK_REG)),
                       static_cast<unsigned>(readReg32(EA_FINE_TARGET_REG)));
        esp_rom_printf(
            "BT_CTRL_EA_RECORD p8=%08x p16=%04x p22=%u p25=%u s22=%u s23=%u s24=%u a4=%08x\n",
            isReadableEaRecord(primary) ? static_cast<unsigned>(*reinterpret_cast<volatile const uint32_t *>(primary + 8))
                                        : static_cast<unsigned>(missing),
            isReadableEaRecord(primary) ? static_cast<unsigned>(*reinterpret_cast<volatile const uint16_t *>(primary + 16))
                                        : static_cast<unsigned>(UINT16_MAX),
            isReadableEaRecord(primary) ? static_cast<unsigned>(primaryBytes[22]) : static_cast<unsigned>(UINT8_MAX),
            isReadableEaRecord(primary) ? static_cast<unsigned>(primaryBytes[25]) : static_cast<unsigned>(UINT8_MAX),
            isReadableEaRecord(secondary) ? static_cast<unsigned>(secondaryBytes[22]) : static_cast<unsigned>(UINT8_MAX),
            isReadableEaRecord(secondary) ? static_cast<unsigned>(secondaryBytes[23]) : static_cast<unsigned>(UINT8_MAX),
            isReadableEaRecord(secondary) ? static_cast<unsigned>(secondaryBytes[24]) : static_cast<unsigned>(UINT8_MAX),
            isReadableEaRecord(active) ? static_cast<unsigned>(activeWords[1]) : static_cast<unsigned>(missing));
    }
    if (line == 7098) {
        esp_rom_printf("BT_CTRL_SCO_CONTEXT env=%08x,%08x,%08x sw_to_hw14=%u\n",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(ld_sco_env[0])),
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(ld_sco_env[1])),
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(ld_sco_env[2])), static_cast<unsigned>(sw_to_hw[14]));
    }
#endif
    __real_r_assert_err(condition, file, line);
}

extern "C" void __wrap_r_assert_param(uint32_t param0, uint32_t param1, const char *file, int line)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
    const uintptr_t callerRaw = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const uintptr_t callerPc = (callerRaw & 0x3fffffffU) | 0x40000000U;

    if (param0 == 0U && param1 == 2U && line == 2411) {
        esp_rom_printf("BT_CTRL_ASSERT_PARAM caller=0x%08x file=%s line=%d p0=%u p1=%u\n", static_cast<unsigned>(callerPc),
                       file != nullptr ? file : "?", line, static_cast<unsigned>(param0), static_cast<unsigned>(param1));
    }
#endif
    __real_r_assert_param(param0, param1, file, line);
}

#endif
