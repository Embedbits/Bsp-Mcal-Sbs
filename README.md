# SBS Peripheral Driver

This module provides an abstraction layer for the **system configuration block** of STM32 MCUs - the `SBS` (system configuration,
boot and security) of the newest families and the `SYSCFG` of the older ones: the settings that belong to no single
peripheral, such as the high-drive Fast-mode Plus outputs of the I/Os, the booster of the analog switches, the I/O compensation
cell, the memory remap, the selection of the Ethernet PHY interface, the routing of the fault sources to the timer break
inputs, the interrupt of the floating-point unit and the read-out of the boot and product state. It is a low-level MCAL
module: it owns the system configuration hardware, the application and the other modules never touch the registers.

The public interface (`Sbs_Port.h`, `Sbs_Types.h`) is **identical for all supported families**. The implementation
for a family lives in its own branch (`Dev/<Family>` for development, `Releases/<Family>` for the released versions), so
a module or an application written against the interface does not change when the MCU family changes.

This README is the landing page and the **specification of the interface** (master). The family branches contain their
own README with everything that is specific to the family: the instances of the device, their settings and the status of
the implementation.

> **Status: STM32H5 implemented (version 0.1.0).** The interface below is implemented for STM32H5 in `Sbs_Port.h` /
> `Sbs_Types.h` (branch `Dev/STM32H5`), the registers are accessed only by the private `Sbs_Reg.c/h`. Verified: unit
> tests (Unity + RegMem, the Rcc module mocked, 73 test cases) on one preset of each of the 12 STM32H5 lines (H503, H523,
> H533, H543, H553, H562, H563, H573, H5E4, H5E5, H5F4, H5F5; the Ethernet tests are skipped on the lines without Ethernet
> and the test of the line without Ethernet on the others) and a compilation of the module for all 12 lines. The
> integration tests (`Tests/IntegrationTests`, 13 test cases) are only compiled (`STM32H503xB_IntegrationTest`,
> `STM32H563xI_IntegrationTest`), they have not been run on a board yet. Other families: stories and branches follow.
>
> Notes of the STM32H5 implementation (a difference from the common proposal is marked as *deviation*):
> - **Registers and clock.** The SBS is at 0x44000400 (nonsecure alias) on all 12 lines and at 0x54000400 (secure alias) on the lines with
>   TrustZone (all but STM32H503), the module uses the alias of the security state of the code (CMSIS macro `SBS`). The bus clock (`SBSEN`
>   of the RCC) is requested from the Rcc module by `Sbs_Init` (the Rcc module enables it at its own initialization too).
> - **Dependencies (*deviation*).** Rcc only: the clock of the SBS, the CSI oscillator for a compensation cell in the automatic mode
>   (the cell works only with the CSI oscillator enabled, the module never disables it) and the state of the Ethernet MAC clock
>   (`Sbs_Set_EthPhyInterface` is refused while the MAC is running). Nvic is not used: the SBS has no interrupt and the interface has
>   no callback, `Sbs_Set_FpuInterruptMask` only selects the exceptions which raise the FPU interrupt, the interrupt line (`FPU_IRQ`)
>   is enabled by the code which provides its handler.
> - **Initialization.** Every function except `Sbs_Get_ModuleVersion`, `Sbs_Get_DefaultConfig`, `Sbs_Init`, `Sbs_Deinit` and `Sbs_Task`
>   needs a successful `Sbs_Init`; the getters have no side effects (they never enable a clock and return an error when the module is not
>   initialized). `Sbs_Init` checks the whole configuration before the first SBS register is written, writes the Fast-mode Plus bits, the FPU
>   mask, the compensation cells and, as the last and irreversible step, the break sources, and verifies every write by a read-back; on a
>   failure the bits which can be cleared are cleared again, the module is deinitialized and an error is returned. `Sbs_Deinit` writes the
>   reset state of the bits which can be cleared (the connected break sources stay until a reset) and keeps the clock enabled. `Sbs_Task`
>   has nothing to do. The default configuration is the reset state of the hardware.
> - **Fast-mode Plus pins (*deviation*).** The bits are per GPIO pin (`PB6_FMP` ... `PB9_FMP`, `PB9` does not exist on STM32H503), not per
>   I2C signal, so the list `sbs_FmPlusPin_t` is `SBS_FMPLUS_PIN_PB6`, `_PB7`, `_PB8`, `_PB9` (the proposal names `SBS_FMPLUS_I2C1_SCL`; the signal
>   `I2C1_SCL` is on PB6 or PB8). The bit enables the high drive of the pin and bypasses its speed control, also for a pin which an I2C peripheral
>   does not use (an LED). A pin which is secure in the GPIO can not be written by a nonsecure access, the read-back reports it as an error.
> - **Analog booster (*deviation*).** The voltage booster of the analog switches of STM32H5 is controlled by `PWR_PMCR` (`BOOSTE`, `AVD_READY`), the
>   SBS has no bit for it. `Sbs_Set_AnalogBooster` and `Sbs_Get_AnalogBooster` return `SBS_REQUEST_ERROR` and `Sbs_Init` refuses
>   `AnalogBooster = SBS_FUNCTION_ACTIVE` (nothing is written).
> - **I/O compensation (*deviation*: members).** `sbs_IoCompConfig_t` holds one entry per cell (`SBS_IO_COMP_CELL_VDDIO`, `SBS_IO_COMP_CELL_VDDIO2`, both
>   cells exist on all 12 lines) with the mode `SBS_IO_COMP_MODE_DISABLED` (cell off, fixed code of the reset state NMOS 8 / PMOS 7), `_AUTOMATIC`
>   (cell on, the module waits for the ready flag in a bounded loop and then selects the code of the cell; the CSI oscillator is requested
>   from the Rcc module) or `_MANUAL` (cell off, the NMOS and PMOS code 0 ... 15 of the entry applied through the software code register).
>   `Sbs_Get_IoCompensationState` has no cell argument: it reports `SBS_FUNCTION_ACTIVE` if at least one cell is enabled and every enabled cell
>   is ready, otherwise `SBS_FUNCTION_INACTIVE` (so also in the manual and the disabled mode). A cell whose supply is missing never becomes
>   ready: the call fails after the timeout and the cell stays enabled.
> - **Break sources.** A request of `Sbs_Set_BreakSource` is the complete set of the connected sources (bit mask, `SBS_BREAK_NONE` ... `SBS_BREAK_ALL`):
>   a set without a source which is connected already would clear it, it is refused (nothing is written), a set equal to the connected one writes
>   nothing, so adding a source needs the connected set from `Sbs_Get_BreakSource`. The bits are set by software and cleared only by a system
>   reset, the module never writes 0 to a connected bit. The break input of the timer is enabled by the Tim module.
> - **Ethernet PHY interface.** `SBS_ETH_PHY_MII` (`ETH_SEL_PHY` = 0, "GMII or MII") and `SBS_ETH_PHY_RMII` (4) exist on the lines with an Ethernet MAC
>   (STM32H543, H553, H563, H573, H5E4, H5E5, H5F4, H5F5); on STM32H503, H523, H533 and H562 the list is empty and the functions return
>   `SBS_REQUEST_ERROR` (the field `ETH_SEL_PHY` of STM32H562 is described in the reference manual, the device header has no Ethernet there).
>   The setter asks the Rcc module for the clock of the MAC and returns an error when it is running (also for the interface already selected).
> - **FPU interrupt mask.** `sbs_FpuInterrupt_t` is a bit mask of the six exceptions (`SBS_FPU_INT_INVALID_OPERATION`, `_DIVIDE_BY_ZERO`, `_UNDERFLOW`, `_OVERFLOW`,
>   `_INPUT_DENORMAL`, `_INEXACT`), the state after the reset is `SBS_FPU_INT_DEFAULT` (all but the inexact one). The register needs a privileged access.
> - **Memory remap (*deviation*).** STM32H5 has no memory remap: `Sbs_Set_MemoryRemap` and `Sbs_Get_MemoryRemap` return `SBS_REQUEST_ERROR`,
>   the list `sbs_MemoryRemap_t` has no valid item.
> - **Boot and security state.** `sbs_BootState_t` holds the temporal isolation level (`sbs_Hdpl_t`, decoded from the codes 0xB4, 0x51, 0x8A, 0x6F of `SBS_HDPLSR`,
>   another code is an error) and the boot address, which the SBS of STM32H5 does not hold (the addresses are option bytes of the flash interface):
>   `SBS_BOOT_ADDRESS_NONE` (*deviation*). `Sbs_Get_ProductState` returns `SBS_REQUEST_ERROR` (*deviation*): the product state is the option byte `PRODUCT_STATE`
>   of the flash interface, the SBS has no register with it. `Sbs_Get_DebugState` is a bit mask built from `SBS_DBGCR` and `SBS_DBGLOCKR` (access port unlocked,
>   debug unlocked, secure debug, configuration locked): it reports the programmed configuration and not whether a debugger is connected; the registers are
>   read-as-zero at the temporal isolation levels 2 and 3, the function returns an error there. `Sbs_Get_EraseState` is a bit mask of `MCLR` (SRAM2, BKPSRAM and the
>   caches, `SBS_ERASE_STATE_SRAM`) and `IPMEE` (ICACHE and PKA RAM, `SBS_ERASE_STATE_CACHE`); the module never clears them (write 1 to clear), a bit cleared by the
>   application reads as an erase in progress.
> - **Functions without a signature in the proposal (*deviation*).** The short table lists `Sbs_Get_FmPlus`, `Sbs_Get_AnalogBooster`, `Sbs_Get_EthPhyInterface`,
>   `Sbs_Get_FpuInterruptMask`, `Sbs_Get_MemoryRemap` and `Sbs_Get_DebugState`, the C block does not declare them. The signatures follow the declared ones:
>   `Sbs_Get_FmPlus( sbs_FmPlusPin_t fmPlusPin, sbs_FunctionState_t * const state )`, `Sbs_Get_AnalogBooster( sbs_FunctionState_t * const state )`,
>   `Sbs_Get_EthPhyInterface( sbs_EthPhyInterface_t * const phyInterface )`, `Sbs_Get_FpuInterruptMask( sbs_FpuInterrupt_t * const fpuInterrupt )`,
>   `Sbs_Get_MemoryRemap( sbs_MemoryRemap_t * const memoryRemap )`, `Sbs_Get_DebugState( sbs_DebugState_t * const debugState )`.
> - **Not in the interface.** The flash ECC NMI mask (`SBS_ECCNMIR`, written by the Flash module directly), the HDPL increment, the next HDPL, the debug opening and
>   its lock, the RSS command, the EPOCH selection, the register security (`SBS_SECCFGR`), the CPU locks (`SBS_CNSLCKR`, `SBS_CSLCKR`), the OTG_HS PHY tuning
>   (STM32H5E5, H5F5) and the Ethernet status and interrupt polarity bits (`ETHINTPOL`, `ETHPDACK`, `ETHTXLPI`).
> - **Reference manual inconsistencies** (RM0481 Rev 5, RM0492 Rev 3, RM0517 Rev 1; the module follows the device header and the conservative reading):
>   the SBS chapter of RM0481 names the booster of the analog switches and the register security of `ANASWVDD`, `IO_ANA_BOOST_EN` and `SMPS_DIV_CLOCK_EN`, but
>   `SBS_PMCR` has none of these bits (the booster is in `PWR_PMCR`); the bit `ECCL` of `SBS_CFGR2` is described as "set and cleared by software" while the access
>   type of the register is "set only" (RM0481, RM0517) or "read/write" for all bits of which three are described as cleared only by a reset (RM0492), the
>   module treats all four as locked until the reset; the reset value of `CS1` and `CS2` of `SBS_CCCSR` is described as 1 while the register map gives 0 (the module
>   does not depend on it: it always writes `CS`); the comments of `SBS_CCSWCR` mix `VDD` and `VDDIO` while the cells are those of `VDDIO` and `VDDIO2`; the debug unlock is
>   effective when the temporal isolation level "equals" `DBG_AUTH_HDPL` in one place and "is equal or higher" in another, the module reports the programmed
>   fields only; `SBS_PMCR.ETH_SEL_PHY` is 3 bits in the device header and 4 bits (24:21) in the text for STM32H543, H553, H5E4, H5E5, H5F4, H5F5 (the values used by the
>   module, 0 and 4, are the same in both readings); the PMCR reset value of these lines is 0x40000000 while the bit 30 is reserved.

---

## Intent and purpose

- Several peripherals need **one bit in the system configuration block** to work as expected: the I2C Fast-mode Plus needs
  the high-drive switch on its pins, the ADC and the comparators at a low supply need the analog switch booster, the Ethernet MAC needs
  the PHY interface (MII or RMII) selected **before** it is enabled, the fast I/Os need the compensation cell. Today the
  module that needs it writes the bit itself or the bit is forgotten. The module is the **only place** that writes it, and the other modules
  call it (`Sbs_Set_FmPlus`, `Sbs_Set_EthPhyInterface`).
- The block also carries the **safety connections**: the Cortex lockup, the SRAM and flash ECC errors, the power voltage detector
  can be connected to the **break input of the advanced timers**, so that the PWM outputs are shut down by the hardware, with
  a lock that cannot be removed until the reset. The module offers them under one interface (`Sbs_Set_BreakSource`).
- The state of the product - the boot address and the temporal isolation level, the product (life-cycle) state, the debug
  state, the erase status of the SRAMs and of the caches - is **read-only** information that an application and the
  bootloaders need; the module reports it in a family independent way.
- The module hides the differences between the families: the register names (`SBS`, `SYSCFG`), the bits, the families that
  have the memory remap or the I/O compensation cell and the ones that do not differ, the interface stays the same.

What the module is **not**: it does not route the external interrupts to the EXTI lines (the Exti module does, even where the
bits are in the system configuration block), it does not program the option bytes of the boot (the Flash module) and it does not
open the debug port (the debug authentication is a protocol of the product).

---

## Concept

The module groups the functions by what they control:

| Group | Functions | Meaning |
|---|---|---|
| **I/O features** | `Sbs_Set_FmPlus`, `Sbs_Set_AnalogBooster`, `Sbs_Set_IoCompensation` | High-drive Fast-mode Plus of the selected pins (`sbs_FmPlusPin_t`), the voltage booster of the analog switches (needed at a low supply), the I/O compensation cell (manual or automatic code, with the ready flag). |
| **Break sources** | `Sbs_Set_BreakSource`, `Sbs_Get_BreakSource` | Connect the Cortex lockup, the SRAM / flash ECC error, the voltage detector to the break input of the timers (`SBS_BREAK_LOCKUP` ...). The connection is locked: a later disable returns an error until the reset. |
| **Interfaces** | `Sbs_Set_EthPhyInterface`, `Sbs_Set_FpuInterruptMask` | The Ethernet PHY interface (MII / RMII), the enabled exceptions of the floating-point unit. |
| **Memory remap** | `Sbs_Set_MemoryRemap`, `Sbs_Get_MemoryRemap` | What appears at the boot address after the remap (system memory, SRAM, an external memory) on the families that have it. |
| **Boot and security state** | `Sbs_Get_BootState`, `Sbs_Get_ProductState`, `Sbs_Get_DebugState`, `Sbs_Get_EraseState` | The boot address, the temporal isolation level, the product state, the open / closed debug, whether the SRAMs and caches were erased after the reset. |

Rules:

1. Every setter of a feature that the device does not have returns `SBS_REQUEST_ERROR` and does not write any register.
2. A setter reads the register back and compares it; a bit that is write-once or locked (the break sources, the register
   security) is reported as an error when it did not take effect.
3. `Sbs_Set_EthPhyInterface` is called **before** the Ethernet MAC is enabled (the Eth module calls it from its initialization);
   after the MAC is running the call returns an error.
4. `Sbs_Set_IoCompensation` with the automatic mode waits for the ready flag with a timeout; the compensation is requested by the
   RCC / the application when the I/O speed is raised.
5. The remap and the break sources are system wide: the module keeps the **requested** state and refuses a contradicting request
   from another module (for example two different remaps).
6. All parameters are checked before a register is written; the TrustZone aliases of the family are used by the module according
   to the security state of the caller.

---

## Supported families

Every family gets its own story, its own tasks and its own branches `Dev/<Family>` and `Releases/<Family>` cut from this
master.

| Family | Branch | Block | Status |
|---|---|---|---|
| STM32H5 | `Dev/STM32H5` | `SBS`: Fast-mode Plus of PB6 - PB9, two I/O compensation cells, break connections (lockup, SRAM and flash ECC, PVD), FPU interrupt mask, Ethernet PHY interface (MII, RMII), boot state (temporal isolation level), debug configuration state, erase status of the SRAMs and the caches; the analog switch booster (PWR), the memory remap, the product state (flash option byte) and the register security are not part of the SBS interface of this family | implemented (0.1.0) |
| STM32U3 | `Dev/STM32U3` | `SBS` | planned |
| STM32U5 | `Dev/STM32U5` | `SYSCFG`: Fast-mode Plus, analog booster, break connections, FPU interrupts, SRAM erase and parity, security configuration | planned |
| STM32G4, STM32L4 | `Dev/STM32G4`, `Dev/STM32L4` | `SYSCFG`: memory remap, Fast-mode Plus, analog booster, break connections (lockup, SRAM parity, PVD), SRAM2 erase and parity | planned |
| STM32H7 | `Dev/STM32H7` | `SYSCFG`: Fast-mode Plus, I/O compensation cell, Ethernet PHY interface, break connections | planned |
| STM32F4, STM32F7 | `Dev/STM32F4`, `Dev/STM32F7` | `SYSCFG`: memory remap, I/O compensation cell, Ethernet PHY interface (MII / RMII) | planned |

A function without a meaning in a family stays in the interface, returns `SBS_REQUEST_ERROR` and suppresses the unused
parameters (for example the compensation cell on a family without it, the temporal isolation level on a family without
the secure boot, the Ethernet PHY interface on a device without Ethernet).

---

## Public interface in short (proposal)

| Group | Functions |
|---|---|
| Module management | `Sbs_Get_ModuleVersion`, `Sbs_Init`, `Sbs_Deinit`, `Sbs_Task`, `Sbs_Get_DefaultConfig` |
| I/O features | `Sbs_Set_FmPlus` / `Get`, `Sbs_Set_AnalogBooster` / `Get`, `Sbs_Set_IoCompensation`, `Sbs_Get_IoCompensationState` |
| Break sources | `Sbs_Set_BreakSource`, `Sbs_Get_BreakSource` |
| Interfaces | `Sbs_Set_EthPhyInterface` / `Get`, `Sbs_Set_FpuInterruptMask` / `Get` |
| Memory remap | `Sbs_Set_MemoryRemap`, `Sbs_Get_MemoryRemap` |
| Boot and security state | `Sbs_Get_BootState`, `Sbs_Get_ProductState`, `Sbs_Get_DebugState`, `Sbs_Get_EraseState` |

```c
sbs_ModuleVersion_t Sbs_Get_ModuleVersion        ( void );

sbs_RequestState_t  Sbs_Init                     ( const sbs_Config_t * const sbsConfig );
sbs_RequestState_t  Sbs_Deinit                   ( void );
void                Sbs_Task                     ( void );
sbs_RequestState_t  Sbs_Get_DefaultConfig        ( sbs_Config_t * const sbsConfig );

sbs_RequestState_t  Sbs_Set_FmPlus               ( sbs_FmPlusPin_t fmPlusPin, sbs_FunctionState_t state );
sbs_RequestState_t  Sbs_Set_AnalogBooster        ( sbs_FunctionState_t state );
sbs_RequestState_t  Sbs_Set_IoCompensation       ( const sbs_IoCompConfig_t * const compConfig );
sbs_RequestState_t  Sbs_Get_IoCompensationState  ( sbs_FunctionState_t * const readyState );

sbs_RequestState_t  Sbs_Set_BreakSource          ( sbs_BreakSource_t breakSource );
sbs_RequestState_t  Sbs_Get_BreakSource          ( sbs_BreakSource_t * const breakSource );

sbs_RequestState_t  Sbs_Set_EthPhyInterface      ( sbs_EthPhyInterface_t phyInterface );
sbs_RequestState_t  Sbs_Set_FpuInterruptMask     ( sbs_FpuInterrupt_t fpuInterrupt );

sbs_RequestState_t  Sbs_Set_MemoryRemap          ( sbs_MemoryRemap_t memoryRemap );
sbs_RequestState_t  Sbs_Get_BootState            ( sbs_BootState_t * const bootState );
sbs_RequestState_t  Sbs_Get_ProductState         ( sbs_ProductState_t * const productState );
sbs_RequestState_t  Sbs_Get_EraseState           ( sbs_EraseState_t * const eraseState );
```

Every function returns `SBS_REQUEST_OK` / `SBS_REQUEST_ERROR` and checks its parameters and the state of the module
before it touches a register.

| Type | Meaning |
|---|---|
| `sbs_Config_t` | Fast-mode Plus pins, analog booster, I/O compensation, break sources, FPU interrupt mask - the defaults applied by `Sbs_Init` |
| `sbs_FmPlusPin_t` | `SBS_FMPLUS_I2C1_SCL` ... - only the pins of the device |
| `sbs_BreakSource_t` | Bit mask: `SBS_BREAK_LOCKUP`, `SBS_BREAK_SRAM_ECC`, `SBS_BREAK_FLASH_ECC`, `SBS_BREAK_PVD` - only the sources of the device |
| `sbs_EthPhyInterface_t` | `SBS_ETH_PHY_MII`, `SBS_ETH_PHY_RMII` |
| `sbs_ProductState_t` | The product (life-cycle) state of the family: open, provisioning, closed ... |
| `sbs_EraseState_t` | Bit mask: SRAM erased, caches erased |

---

## Usage

### Fast-mode Plus on the I2C pins (called by the I2c module)

```c
#include "Sbs_Port.h"

(void)Sbs_Set_FmPlus( SBS_FMPLUS_I2C1_SCL, SBS_FUNCTION_ACTIVE );
(void)Sbs_Set_FmPlus( SBS_FMPLUS_I2C1_SDA, SBS_FUNCTION_ACTIVE );
```

### Ethernet PHY interface before the MAC is started (called by the Eth module)

```c
(void)Sbs_Set_EthPhyInterface( SBS_ETH_PHY_RMII );
```

### Shut the PWM down by the hardware on a fault

```c
(void)Sbs_Set_BreakSource( SBS_BREAK_LOCKUP | SBS_BREAK_PVD );     /* locked until the next reset */
/* the Tim module enables the break input of the PWM timer */
```

---

## Behaviour and limits

- **Locked bits.** The break connections and some security bits are set-only until the reset; the module reports a request to clear
  them as an error and does not retry.
- **Order.** The Ethernet PHY interface must be selected before the MAC clock is enabled; the analog booster before the analog
  peripheral is started at a low supply; the compensation cell before the I/O speed is raised.
- **Clock.** The system configuration registers need their bus clock; the module requests it from the RCC module.
- **Remap and the boot.** The remap changes what the CPU sees at the boot address; calling it while code executes from the
  remapped area is the task of the application.
- **Not part of the first version:** the EXTI line routing (the Exti module), the programming of the boot options, the control of
  the debug opening, the hide protection level changes of the secure boot (a bootloader task).

---

## Dependencies

- **Rcc** - the clock of the system configuration registers
- **Nvic** - the FPU interrupt, where the family routes it
- **RAL (Register Abstraction Layer)** - used internally to access the registers

All mandatory RCC and NVIC configurations are handled internally. The consumers of the settings are the I2c, Eth, Tim, Adc,
Comp and Rcc modules.

---

## Testing

- **Unit tests** (host, Unity + CMock + RegMem): the acceptance of the features of every family line and the rejection of the
  missing ones, the read-back of the locked bits and the reported failure, the compensation cell sequence and its timeout, the
  remap rules, the decoding of the boot and the product state.
- **Integration tests** (target, probe-rs): the Fast-mode Plus signal edges, the PWM break by the lockup / voltage detector, the
  Ethernet PHY interface selection, the read-out of the boot and the product state on the board.

---

## Interface conventions

The module follows the common rules of the MCAL layer, so that it behaves like the other modules:

- **Lists instead of free members.** Everything that the hardware restricts (the pins, the kernel clock sources, the DMA
  channels, the triggers and the other selectable sources) is a `typedef enum` list with exactly the items that are valid
  for the device line; a pin is an item `SBS_<SIGNAL>_PIN_<INSTANCE>_P<port><pin>` and there is an `*_UNUSED` item. A choice
  that is not in the list cannot be written.
- **Own peripheral only.** The module calls only the registers / LL functions of its own peripheral. The clocks, the
  interrupts, the pins, the DMA and the EXTI lines go through the API of the Rcc, Nvic, Gpio, Gpdma and Exti modules; a
  missing function is added to that module, it is not bypassed.
- **Init verifies itself.** `Sbs_Init` checks the result at the end; when it is not as required, the module is
  deinitialized and an error is returned. The waiting for the hardware is a loop on the register state with a timeout,
  never a blind delay.
- **Transfer modes.** Where the module moves data, the POLL (served by `Sbs_Task`), ISR and DMA modes have the same
  configuration structure, the same request and the same callbacks (error, half transfer, transfer complete).
- **Flags and states are enums of the module** (`sbs_FunctionState_t`, `sbs_FlagState_t`), the limits and the constants are
  named, there are no magic numbers in the interface.
- **Unsupported functions stay.** A function without a meaning in a family keeps its signature, suppresses the unused
  parameters and returns `SBS_REQUEST_ERROR`.
- **Documentation.** Every function and type has a Doxygen description, the main header has the `\defgroup`.

---

## Repository layout and branches

The master holds this README and the license. The family branches (`Dev/<Family>`, `Releases/<Family>`) hold:

| Path | Content |
|---|---|
| `README.md` | README of the family: the instances of the device, specifics, errata, status |
| `Sbs_Port.h`, `Sbs_Types.h` | Public interface (identical in all families) |
| `Sbs*.c`, `Sbs.h` | Implementation of the family |
| `CMakeLists.txt` | Library `Sbs_Lib` of the EmBi platform |
| `Tests/UnitTests` | Unit tests (Unity + CMock + RegMem) |
| `Tests/IntegrationTests` | Integration tests on the target (probe-rs) |

A change of the public interface is made in all families.

---

## CMake integration

```cmake
target_link_libraries( User_Lib PRIVATE Sbs_Lib )
```

1. Link `Sbs_Lib` to your CMake library.
2. Include `Sbs_Port.h` in your project.
3. Configure the module as needed for your hardware.

---

## License

This project is licensed under the **Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)**.

You are free to use, modify, and share this work for **non-commercial purposes**, provided appropriate credit is given.

See [LICENSE.md](LICENSE.md) for full terms or visit [creativecommons.org/licenses/by-nc/4.0](https://creativecommons.org/licenses/by-nc/4.0/).

---

## Authors

- **Mr.Nobody** - [embedbits.com](https://embedbits.com)

Contributions are welcome! Please open a pull request.
