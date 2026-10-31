/**
 * \author Mr.Nobody
 * \file Sbs_Types.h
 * \ingroup Sbs
 * \brief Sbs module global types definition
 *
 * This file contains the types definitions used across the module and are
 * available for other modules through Port file.
 *
 * The lists of the selectable items (\ref sbs_FmPlusPin_t, \ref sbs_EthPhyInterface_t, the break sources, the
 * FPU interrupts, the debug state) contain exactly the items of the device line: an item exists only if the
 * bit or the field of the item exists in the SBS registers (and the peripheral exists on the line) according
 * to the CMSIS device header and the reference manuals RM0481, RM0492 and RM0517.
 *
 */

/**
 * \defgroup Sbs Sbs
 * \brief Sbs module
 */

#ifndef SBS_SBS_TYPES_H
#define SBS_SBS_TYPES_H
/* ============================== INCLUDES ================================== */
#include "stdint.h"                         /* Module types definition        */
#include "Stm32.h"                          /* MCU device header (SBS bits)   */
/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Null pointer definition */
#define SBS_NULL_PTR                         ( ( void* ) 0u )

/** Boot address reported by \ref Sbs_Get_BootState when the SBS holds no boot address (STM32H5: the boot
 *  addresses are option bytes of the flash interface, the SBS has no register with them) */
#define SBS_BOOT_ADDRESS_NONE                ( 0xFFFFFFFFu )

/** Highest value of the NMOS and of the PMOS code of an I/O compensation cell (4 bits each) */
#define SBS_IO_COMP_CODE_MAX                 ( 0xFu )

/** NMOS code applied by the mode \ref SBS_IO_COMP_MODE_DISABLED (reset value of SBS_CCSWCR.SW_ANSRCx) */
#define SBS_IO_COMP_CODE_NMOS_DEFAULT        ( 0x8u )

/** PMOS code applied by the mode \ref SBS_IO_COMP_MODE_DISABLED (reset value of SBS_CCSWCR.SW_APSRCx) */
#define SBS_IO_COMP_CODE_PMOS_DEFAULT        ( 0x7u )

/* Break sources (bit mask of \ref sbs_BreakSource_t) ------------------------ */

/** No fault source is connected to the break inputs of the timers */
#define SBS_BREAK_NONE                       ( 0x00000000u )

/** Core lockup output of the Cortex-M33 (SBS_CFGR2.CLL) */
#define SBS_BREAK_LOCKUP                     ( 0x00000001u )

/** SRAM double ECC error (SBS_CFGR2.SEL) */
#define SBS_BREAK_SRAM_ECC                   ( 0x00000002u )

/** Flash memory double ECC error (SBS_CFGR2.ECCL) */
#define SBS_BREAK_FLASH_ECC                  ( 0x00000004u )

/** Programmable voltage detector, the PVD configuration of the PWR becomes read only (SBS_CFGR2.PVDL) */
#define SBS_BREAK_PVD                        ( 0x00000008u )

/** All break sources of the device */
#define SBS_BREAK_ALL                        ( SBS_BREAK_LOCKUP    | \
                                               SBS_BREAK_SRAM_ECC  | \
                                               SBS_BREAK_FLASH_ECC | \
                                               SBS_BREAK_PVD         )

/* FPU interrupts (bit mask of \ref sbs_FpuInterrupt_t) ---------------------- */

/** No FPU exception raises the FPU interrupt */
#define SBS_FPU_INT_NONE                     ( 0x00000000u )

/** Invalid operation (SBS_FPUIMR.FPU_IE[0]) */
#define SBS_FPU_INT_INVALID_OPERATION        ( 0x00000001u )

/** Divide-by-zero (SBS_FPUIMR.FPU_IE[1]) */
#define SBS_FPU_INT_DIVIDE_BY_ZERO           ( 0x00000002u )

/** Underflow (SBS_FPUIMR.FPU_IE[2]) */
#define SBS_FPU_INT_UNDERFLOW                ( 0x00000004u )

/** Overflow (SBS_FPUIMR.FPU_IE[3]) */
#define SBS_FPU_INT_OVERFLOW                 ( 0x00000008u )

/** Input denormal, "input abnormal" of the reference manual (SBS_FPUIMR.FPU_IE[4]) */
#define SBS_FPU_INT_INPUT_DENORMAL           ( 0x00000010u )

/** Inexact (SBS_FPUIMR.FPU_IE[5]) */
#define SBS_FPU_INT_INEXACT                  ( 0x00000020u )

/** All FPU exceptions */
#define SBS_FPU_INT_ALL                      ( SBS_FPU_INT_INVALID_OPERATION | \
                                               SBS_FPU_INT_DIVIDE_BY_ZERO    | \
                                               SBS_FPU_INT_UNDERFLOW         | \
                                               SBS_FPU_INT_OVERFLOW          | \
                                               SBS_FPU_INT_INPUT_DENORMAL    | \
                                               SBS_FPU_INT_INEXACT             )

/** State of the hardware after the reset: all exceptions except the inexact one */
#define SBS_FPU_INT_DEFAULT                  ( SBS_FPU_INT_ALL & ~SBS_FPU_INT_INEXACT )

/* Erase state (bit mask of \ref sbs_EraseState_t) --------------------------- */

/** Nothing is reported as erased (the erase is on going, or the status was cleared by software) */
#define SBS_ERASE_STATE_NONE                 ( 0x00000000u )

/** Device memories erased after the power-on reset, a tamper detection or a product state regression: SRAM2,
 *  BKPSRAM and the caches (SBS_MESR.MCLR, the bit survives a system reset) */
#define SBS_ERASE_STATE_SRAM                 ( 0x00000001u )

/** ICACHE (and PKA RAM) erased after a tamper detection or a product state regression (SBS_MESR.IPMEE) */
#define SBS_ERASE_STATE_CACHE                ( 0x00000002u )

/* Debug state (bit mask of \ref sbs_DebugState_t) ---------------------------- */

/** The debug configuration of the SBS opens nothing and is not locked */
#define SBS_DEBUG_STATE_NONE                 ( 0x00000000u )

/** The access port is unlocked (SBS_DBGCR.AP_UNLOCK holds the unlock code) */
#define SBS_DEBUG_STATE_AP_UNLOCKED          ( 0x00000001u )

/** The debug opens when the temporal isolation level of SBS_DBGCR.DBG_AUTH_HDPL is reached
 *  (SBS_DBGCR.DBG_UNLOCK holds the unlock code) */
#define SBS_DEBUG_STATE_DEBUG_UNLOCKED       ( 0x00000002u )

#if defined(SBS_DBGCR_DBG_AUTH_SEC)
/** The authenticated debug opens the secure and the nonsecure part (SBS_DBGCR.DBG_AUTH_SEC holds the unlock code) */
#define SBS_DEBUG_STATE_SECURE_DEBUG         ( 0x00000004u )
#endif

/** The debug configuration is locked until the next reset (SBS_DBGLOCKR.DBGCFG_LOCK differs from the unlock code) */
#define SBS_DEBUG_STATE_CONFIG_LOCKED        ( 0x00000008u )

/* Ethernet PHY interface availability ---------------------------------------- */

#if defined(ETH) && \
    defined(SBS_PMCR_ETH_SEL_PHY)
/** Defined if the device line has an Ethernet MAC and the selection of its PHY interface in SBS_PMCR */
#define SBS_ETH_PHY_AVAILABLE
#endif

/* ========================== EXPORTED MACROS =============================== */

/* ============================== TYPEDEFS ================================== */

/** \brief Type signaling major version of SW module */
typedef uint8_t sbs_MajorVersion_t;


/** \brief Type signaling minor version of SW module */
typedef uint8_t sbs_MinorVersion_t;


/** \brief Type signaling patch version of SW module */
typedef uint8_t sbs_PatchVersion_t;


/** \brief Type signaling actual version of SW module */
typedef struct
{
    sbs_MajorVersion_t Major; /**< Major version */
    sbs_MinorVersion_t Minor; /**< Minor version */
    sbs_PatchVersion_t Patch; /**< Patch version */
}   sbs_ModuleVersion_t;


/** Function status enumeration */
typedef enum
{
    SBS_FUNCTION_INACTIVE = 0u, /**< Function status is inactive */
    SBS_FUNCTION_ACTIVE         /**< Function status is active   */
}   sbs_FunctionState_t;


/** Enumeration used to signal request processing state */
typedef enum
{
    SBS_REQUEST_ERROR = 0u, /**< Processing request failed  */
    SBS_REQUEST_OK          /**< Processing request succeed */
}   sbs_RequestState_t;


/** Flag states enumeration */
typedef enum
{
    SBS_FLAG_INACTIVE = 0u, /**< Inactive flag state */
    SBS_FLAG_ACTIVE         /**< Active flag state   */
}   sbs_FlagState_t;


/** \brief Type used to store a set of connected fault sources, bit mask of SBS_BREAK_xxx */
typedef uint32_t sbs_BreakSource_t;


/** \brief Type used to store the enabled FPU exceptions, bit mask of SBS_FPU_INT_xxx */
typedef uint32_t sbs_FpuInterrupt_t;


/** \brief Type used to store the erase state of the memories, bit mask of SBS_ERASE_STATE_xxx */
typedef uint32_t sbs_EraseState_t;


/** \brief Type used to store the debug state, bit mask of SBS_DEBUG_STATE_xxx */
typedef uint32_t sbs_DebugState_t;


/** \brief Type used to store the boot address (\ref SBS_BOOT_ADDRESS_NONE when the device has none) */
typedef uint32_t sbs_BootAddress_t;


/** \brief Type used to store a code of the I/O compensation cell (\ref SBS_IO_COMP_CODE_MAX at most) */
typedef uint8_t sbs_IoCompCode_t;


/** \brief Type used to store the bit position of a code field of the I/O compensation cell in SBS_CCSWCR */
typedef uint32_t sbs_IoCompPos_t;


/** \brief Pins with the Fast-mode Plus high-drive capability (SBS_PMCR.PBx_FMP).
 *
 * The bit enables the high drive of the pin (the speed control is bypassed), it is a property of the pin and
 * not of an I2C signal: the pin can be used by an I2C peripheral or as a general purpose output (LED). The list
 * contains only the pins of the device line (PB9 does not exist on STM32H503).
 */
typedef enum
{
    SBS_FMPLUS_PIN_PB6    = 0u,                /**< Fast-mode Plus on PB6 (SBS_PMCR.PB6_FMP) */
    SBS_FMPLUS_PIN_PB7,                        /**< Fast-mode Plus on PB7 (SBS_PMCR.PB7_FMP) */
    SBS_FMPLUS_PIN_PB8,                        /**< Fast-mode Plus on PB8 (SBS_PMCR.PB8_FMP) */
#if defined(SBS_PMCR_PB9_FMP)
    SBS_FMPLUS_PIN_PB9,                        /**< Fast-mode Plus on PB9 (SBS_PMCR.PB9_FMP) */
#endif
    SBS_FMPLUS_PIN_CNT,                        /**< Count of the pins of the device line     */
    SBS_FMPLUS_PIN_UNUSED = SBS_FMPLUS_PIN_CNT /**< No pin                                   */
}   sbs_FmPlusPin_t;


/** \brief Ethernet PHY interface (SBS_PMCR.ETH_SEL_PHY).
 *
 * The list contains the interfaces of the device line, it is empty on the lines without Ethernet
 * (STM32H503, STM32H523, STM32H533, STM32H562).
 */
typedef enum
{
#if defined(SBS_ETH_PHY_AVAILABLE)
    SBS_ETH_PHY_MII    = 0u,             /**< MII (the reference manual names it "GMII or MII", ETH_SEL_PHY = 0) */
    SBS_ETH_PHY_RMII,                    /**< RMII (ETH_SEL_PHY = 4)                                             */
#endif
    SBS_ETH_PHY_CNT,                     /**< Count of the PHY interfaces of the device line                     */
    SBS_ETH_PHY_UNUSED = SBS_ETH_PHY_CNT /**< No interface                                                       */
}   sbs_EthPhyInterface_t;


/** \brief Remap of the memory at the boot address.
 *
 * STM32H5 has no memory remap (the boot memory is selected by the option bytes and the BOOT0 pin), the list is empty.
 */
typedef enum
{
    SBS_MEMORY_REMAP_CNT    = 0u,                  /**< Count of the remap options (none on STM32H5) */
    SBS_MEMORY_REMAP_UNUSED = SBS_MEMORY_REMAP_CNT /**< No remap                                     */
}   sbs_MemoryRemap_t;


/** \brief Temporal isolation level (hide protection level, SBS_HDPLSR.HDPL).
 *
 * The level is a monotonic counter incremented during the boot stages, it is reset by a system reset.
 */
typedef enum
{
    SBS_HDPL_0 = 0u,            /**< HDPL0: code and data of the manufacturer (root secure services)   */
    SBS_HDPL_1,                 /**< HDPL1: immutable root of trust (iRoT)                             */
    SBS_HDPL_2,                 /**< HDPL2: updatable root of trust (uRoT)                             */
    SBS_HDPL_3,                 /**< HDPL3: application                                                */
    SBS_HDPL_CNT                /**< Count of the temporal isolation levels                            */
}   sbs_Hdpl_t;


/** \brief Product (life cycle) state of the device.
 *
 * The state is an option byte of the flash interface (FLASH_OPTSR_CUR.PRODUCT_STATE), the SBS has no
 * register with it: \ref Sbs_Get_ProductState returns \ref SBS_REQUEST_ERROR on STM32H5. The list holds the states of
 * the line for the common interface.
 */
typedef enum
{
    SBS_PRODUCT_STATE_OPEN = 0u,        /**< Open: no protection, everything can be programmed          */
    SBS_PRODUCT_STATE_PROVISIONING,     /**< Provisioning: the keys and the root of trust are installed */
    SBS_PRODUCT_STATE_IROT_PROVISIONED, /**< iRoT-Provisioned: the immutable root of trust is installed */
#if defined(SBS_SECCFGR_SBSSEC)
    SBS_PRODUCT_STATE_TZ_CLOSED,        /**< TZ-Closed: TrustZone product, secure debug closed          */
#endif
    SBS_PRODUCT_STATE_CLOSED,           /**< Closed: debug disabled, regression possible                */
    SBS_PRODUCT_STATE_LOCKED,           /**< Locked: no transition, no policy change, no debug          */
    SBS_PRODUCT_STATE_REGRESSION,       /**< Regression: transition to Open started by the debug authentication */
#if defined(SBS_SECCFGR_SBSSEC)
    SBS_PRODUCT_STATE_NS_REGRESSION,    /**< NS-Regression: transition to TZ-Closed by the debug authentication */
#endif
    SBS_PRODUCT_STATE_CNT               /**< Count of the product states of the device line             */
}   sbs_ProductState_t;


/** \brief Compensation cells of the I/Os (SBS_CCCSR, SBS_CCVALR, SBS_CCSWCR).
 *
 * Both cells exist on all STM32H5 lines (RM0481, RM0492, RM0517). A cell is useful when the supply of the
 * I/Os is in the range of the compensation (2.7 V ... 3.6 V or 1.62 V ... 2 V).
 */
typedef enum
{
    SBS_IO_COMP_CELL_VDDIO = 0u,    /**< Cell of the I/Os supplied by VDDIO  (EN1, CS1, RDY1, ANSRC1, APSRC1) */
    SBS_IO_COMP_CELL_VDDIO2,        /**< Cell of the I/Os supplied by VDDIO2 (EN2, CS2, RDY2, ANSRC2, APSRC2) */
    SBS_IO_COMP_CELL_CNT            /**< Count of the compensation cells                                      */
}   sbs_IoCompCell_t;


/** \brief Mode of a compensation cell. */
typedef enum
{
    SBS_IO_COMP_MODE_DISABLED = 0u, /**< Cell off, the fixed code of the reset state is applied to the I/Os
                                         (\ref SBS_IO_COMP_CODE_NMOS_DEFAULT, \ref SBS_IO_COMP_CODE_PMOS_DEFAULT) */
    SBS_IO_COMP_MODE_AUTOMATIC,     /**< Cell on, the code of the cell is applied when the cell is ready (the CSI
                                         oscillator is needed) */
    SBS_IO_COMP_MODE_MANUAL,        /**< Cell off, the code of the configuration is applied to the I/Os */
    SBS_IO_COMP_MODE_CNT            /**< Count of the compensation modes */
}   sbs_IoCompMode_t;


/** \brief Configuration of one compensation cell. */
typedef struct
{
    sbs_IoCompMode_t Mode;          /**< Mode of the cell                                                      */
    sbs_IoCompCode_t NmosCode;      /**< NMOS code of \ref SBS_IO_COMP_MODE_MANUAL (ignored in other modes)    */
    sbs_IoCompCode_t PmosCode;      /**< PMOS code of \ref SBS_IO_COMP_MODE_MANUAL (ignored in other modes)    */
}   sbs_IoCompCellConfig_t;


/** \brief Configuration of the compensation cells of the I/Os. */
typedef struct
{
    /** Configuration of the cells, indexed by \ref sbs_IoCompCell_t */
    sbs_IoCompCellConfig_t Cell[ SBS_IO_COMP_CELL_CNT ];
}   sbs_IoCompConfig_t;


/** \brief Boot state: the temporal isolation level and the boot address. */
typedef struct
{
    sbs_Hdpl_t        Hdpl;        /**< Current temporal isolation level                    */
    sbs_BootAddress_t BootAddress; /**< Boot address, \ref SBS_BOOT_ADDRESS_NONE on STM32H5 */
}   sbs_BootState_t;


/** \brief Type used to store SBS configuration */
typedef struct
{
    /** Fast-mode Plus high drive of the pins, indexed by \ref sbs_FmPlusPin_t */
    sbs_FunctionState_t FmPlus[ SBS_FMPLUS_PIN_CNT ];

    /** Voltage booster of the analog switches. STM32H5 has it in the PWR module (PWR_PMCR.BOOSTE), not in the SBS:
     *  \ref SBS_FUNCTION_ACTIVE is refused. */
    sbs_FunctionState_t AnalogBooster;

    /** Compensation cells of the I/Os */
    sbs_IoCompConfig_t  IoCompensation;

    /** Fault sources connected to the break inputs of the timers (locked until the reset: the set shall contain
     *  the sources which are connected already) */
    sbs_BreakSource_t   BreakSource;

    /** FPU exceptions which raise the FPU interrupt */
    sbs_FpuInterrupt_t  FpuInterruptMask;
}   sbs_Config_t;

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */


#endif /* SBS_SBS_TYPES_H */
