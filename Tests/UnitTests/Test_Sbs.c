/**
 * \author Mr.Nobody
 * \file Test_Sbs.c
 * \ingroup Sbs
 * \brief Unit tests of System Configuration, Boot and Security (SBS) module.
 *
 * Sbs.c and Sbs_Reg.c are compiled unchanged, SBS registers (base address 0x44000400, inside of the
 * peripheral region of RegMem) are emulated by RegMem, the RCC module (clock of the SBS, CSI oscillator,
 * state of the Ethernet MAC clock) is mocked by CMock. The getters use no other module: an unexpected
 * RCC call in a test of a getter fails the test (CMock).
 *
 * The expected bits and values are written in the tests as numbers of the reference manuals (RM0481, RM0492,
 * RM0517), independent of the CMSIS names used by the module. Items which exist on some device lines
 * only (PB9, Ethernet, secure debug) are guarded by the macros of the CMSIS device header.
 *
 * \note Limitations of the register emulation (RegMem registers are plain memory):
 *       - SBS_HDPLSR, SBS_DBGCR, SBS_DBGLOCKR and the ready flags of SBS_CCCSR are read only on HW, the tests
 *         preset them by plain writes (the module never writes them).
 *       - Bits set by software and cleared only by a reset (SBS_CFGR2), write-1-to-clear bits (SBS_MESR) and
 *         the ready flag set by the cell are not emulated by memory: the ready flag and writes ignored by HW
 *         (secure pin, locked bit, privileged access) are created by the HW model of RegMem (UtSbs_HwModel_xxx),
 *         which runs after every store. The module never writes 0 to a set break bit and never writes SBS_MESR,
 *         the tests check that these registers keep their value.
 *       - Every test case is executed in its own process (CTest), the runtime data of the module start
 *         from the reset state (not initialized) in every test.
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "RegMem.h"                         /* Register memory emulation      */
#include "Sbs_Port.h"                       /* Module under test              */
#include "Sbs_Reg.h"                        /* Register access of the module  */
#include "MockRcc_Port.h"                   /* RCC module mock                */
#include "Stm32.h"                          /* SBS registers definition       */
/* ============================= TYPEDEFS =================================== */

/** \brief Expected Fast-mode Plus bit of a pin (independent of the table of the module) */
typedef struct
{
    sbs_FmPlusPin_t Pin;  /**< Pin of the list                       */
    uint32_t        Mask; /**< Expected bit of SBS_PMCR (RM0481)     */
}   utSbs_FmPlusItem_t;


/** \brief Expected connection bit of a break source (independent of the table of the module) */
typedef struct
{
    sbs_BreakSource_t Source; /**< Break source                      */
    uint32_t          Mask;   /**< Expected bit of SBS_CFGR2 (RM0481) */
}   utSbs_BreakItem_t;


/** \brief Expected value of the temporal isolation level */
typedef struct
{
    uint32_t   Code;  /**< Value of SBS_HDPLSR.HDPL (RM0481 Table 135) */
    sbs_Hdpl_t Level; /**< Expected level                              */
}   utSbs_HdplItem_t;

/* ======================= FORWARD DECLARATIONS ============================= */

static void             UtSbs_Expect_Init           ( void );
static void             UtSbs_Expect_InitCsi        ( void );
static void             UtSbs_Init_Default          ( void );
static void             UtSbs_Init_Config           ( const sbs_Config_t * config );
static void             UtSbs_Preset_Noise          ( uint32_t cfgr2Owned );
static void             UtSbs_Check_Noise           ( uint32_t cfgr2Owned );
static void             UtSbs_Check_InitRefused     ( const sbs_Config_t * config );
static void             UtSbs_Check_ResetState      ( void );
static sbs_IoCompConfig_t UtSbs_Get_CompConfig      ( sbs_IoCompMode_t mode1, sbs_IoCompMode_t mode2 );

#if defined(ETH)
static rcc_RequestState_t UtSbs_RccMacStateStub     ( rcc_PeriphId_t periphId,
                                                      rcc_FunctionState_t * const funcState,
                                                      int callCnt );
static void             UtSbs_Expect_MacState       ( rcc_FunctionState_t macState, rcc_RequestState_t reqState );
#endif

static void             UtSbs_HwModel_CellReady     ( void );
static void             UtSbs_HwModel_CellOrder     ( void );
static void             UtSbs_HwModel_ManualOrder   ( void );
static void             UtSbs_HwModel_Pb6NotKept    ( void );
static void             UtSbs_HwModel_FpuBit1NotKept( void );
static void             UtSbs_HwModel_LockupNotKept ( void );
static void             UtSbs_HwModel_LockupTripwire( void );
static void             UtSbs_HwModel_EthSelNotKept ( void );
static void             UtSbs_HwModel_Enable1NotKept( void );
static void             UtSbs_HwModel_CodeNotKept   ( void );
static void             UtSbs_HwModel_Pb6Sticky     ( void );
static void             UtSbs_HwModel_Enable1Sticky ( void );

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** Value outside of the enumeration of the function state */
#define UT_SBS_INVALID_STATE                ( 7u )

/** Value of the output used to find out that a function did not touch it */
#define UT_SBS_OUTPUT_PATTERN               ( 0x5A5A5A5Au )

/** Value with every bit set: preset of a register, mask of every bit */
#define UT_SBS_ALL_BITS                     ( 0xFFFFFFFFu )

/** Mask of the bit at the given position */
#define UT_SBS_BIT( pos )                   ( 0x00000001u << ( pos ) )

/** Value of a flag variable of the tests which is set */
#define UT_SBS_FLAG_SET                     ( 1u )

/** Version of the module (0.1.0) */
#define UT_SBS_VERSION_MAJOR                ( 0u )
#define UT_SBS_VERSION_MINOR                ( 1u )
#define UT_SBS_VERSION_PATCH                ( 0u )

/** Known counts of the lists of the interface (RM0481, RM0492, RM0517) */
#define UT_SBS_IO_COMP_CELL_CNT             ( 2u )
#define UT_SBS_ETH_PHY_CNT                  ( 2u )
#define UT_SBS_BREAK_SOURCE_CNT             ( 4u )
#define UT_SBS_HDPL_LEVEL_CNT               ( 4u )

/** Count of the combinations of the four connection bits of SBS_CFGR2 and of the two status bits of SBS_MESR */
#define UT_SBS_CFGR2_COMBINATION_CNT        ( 16u )
#define UT_SBS_MESR_COMBINATION_CNT         ( 4u )

/** Positions of the bits MCLR and IPMEE in the counter of the combinations of SBS_MESR */
#define UT_SBS_COMBINATION_MCLR_POS         ( 0u )
#define UT_SBS_COMBINATION_IPMEE_POS        ( 1u )

/** Count of the requests of the tests which ask the RCC module for the state of the MAC clock */
#define UT_SBS_MAC_REQUEST_CNT              ( 2u )

/** Codes of a compensation cell (NMOS in bits 3:0, PMOS in bits 7:4 of the field of the cell), RM0481 */
#define UT_SBS_CODE_NMOS_RESET              ( 8u )
#define UT_SBS_CODE_PMOS_RESET              ( 7u )
#define UT_SBS_CODE_NMOS_MANUAL             ( 0x3u )
#define UT_SBS_CODE_PMOS_MANUAL             ( 0xCu )
#define UT_SBS_CODE_NMOS_HIGH               ( 0xFu )
#define UT_SBS_CODE_PMOS_LOW                ( 0x0u )
#define UT_SBS_CODE_NMOS_PATTERN            ( 0xAu )
#define UT_SBS_CODE_PMOS_PATTERN            ( 0x5u )

/** First invalid values: a code above the 4 bits of the cell, the first bit above the break sources and above the
 *  six FPU exceptions, a bit of the upper half word */
#define UT_SBS_CODE_INVALID                 ( 0x10u )
#define UT_SBS_BREAK_INVALID                ( 0x00000010u )
#define UT_SBS_FPU_INT_INVALID              ( 0x00000040u )
#define UT_SBS_FPU_INT_UPPER_HALF           ( 0x00010000u )

/** Patterns of the registers which are read only for the module: SBS_HDPLSR, SBS_DBGCR, SBS_DBGLOCKR, SBS_MESR */
#define UT_SBS_REG_PATTERN_HDPLSR           ( 0x0000005Au )
#define UT_SBS_REG_PATTERN_DBGCR            ( 0x5A5A5A5Au )
#define UT_SBS_REG_PATTERN_DBGLOCKR         ( 0x000000A5u )
#define UT_SBS_REG_PATTERN_MESR             ( 0x00010001u )

/** Preset of the bits of SBS_HDPLSR above the field HDPL[7:0] */
#define UT_SBS_HDPLSR_UPPER_BITS            ( 0xFFFFFF00u )

/** Verified write of the register component: preset of SBS_FPUIMR, mask, value, value with a bit outside of the mask
 *  and the expected register value */
#define UT_SBS_VERIFIED_PRESET              ( 0xFFFFFF00u )
#define UT_SBS_VERIFIED_MASK                ( 0x0000000Fu )
#define UT_SBS_VERIFIED_VALUE               ( 0x00000005u )
#define UT_SBS_VERIFIED_OUTSIDE             ( 0x00000010u )
#define UT_SBS_VERIFIED_RESULT              ( UT_SBS_VERIFIED_PRESET | UT_SBS_VERIFIED_VALUE )

/* SBS_PMCR (offset 0x100): Fast-mode Plus bits PB6 ... PB9 (bits 16 ... 19) and ETH_SEL_PHY (bits 23:21) */
#define UT_SBS_PMCR_PB6_FMP                 ( 0x00010000u )
#define UT_SBS_PMCR_PB7_FMP                 ( 0x00020000u )
#define UT_SBS_PMCR_PB8_FMP                 ( 0x00040000u )
#define UT_SBS_PMCR_PB9_FMP                 ( 0x00080000u )
#define UT_SBS_PMCR_ETH_SEL_PHY             ( 0x00E00000u )
#define UT_SBS_PMCR_ETH_SEL_RMII            ( 0x00800000u )
#define UT_SBS_PMCR_ETH_SEL_RESERVED        ( 0x00200000u )

/** Fast-mode Plus bits of the device line, their count and the last pin (PB9 does not exist on STM32H503) */
#if defined(SBS_PMCR_PB9_FMP)
#define UT_SBS_FMPLUS_ALL                   ( 0x000F0000u )
#define UT_SBS_FMPLUS_PIN_CNT               ( 4u )
#define UT_SBS_FMPLUS_PIN_LAST              ( SBS_FMPLUS_PIN_PB9 )
#else
#define UT_SBS_FMPLUS_ALL                   ( 0x00070000u )
#define UT_SBS_FMPLUS_PIN_CNT               ( 3u )
#define UT_SBS_FMPLUS_PIN_LAST              ( SBS_FMPLUS_PIN_PB8 )
#endif

/** Bits of SBS_PMCR owned by the module (Fast-mode Plus) and by the Ethernet selection */
#define UT_SBS_PMCR_OWNED                   ( UT_SBS_FMPLUS_ALL | UT_SBS_PMCR_ETH_SEL_PHY )

/* SBS_FPUIMR (0x104): FPU_IE[5:0] */
#define UT_SBS_FPUIMR_OWNED                 ( 0x0000003Fu )
#define UT_SBS_FPUIMR_RESET                 ( 0x0000001Fu )
#define UT_SBS_FPU_IE0                      ( 0x00000001u )
#define UT_SBS_FPU_IE1                      ( 0x00000002u )
#define UT_SBS_FPU_IE2                      ( 0x00000004u )
#define UT_SBS_FPU_IE12                     ( 0x00000006u )
#define UT_SBS_FPUIMR_OVERFLOW_INEXACT      ( 0x00000028u )
#define UT_SBS_FPUIMR_PATTERN               ( 0x0000002Au )

/* SBS_CCCSR (0x110): EN1 (0), CS1 (1), EN2 (2), CS2 (3), RDY1 (8), RDY2 (9) */
#define UT_SBS_CCCSR_EN1                    ( 0x00000001u )
#define UT_SBS_CCCSR_CS1                    ( 0x00000002u )
#define UT_SBS_CCCSR_EN2                    ( 0x00000004u )
#define UT_SBS_CCCSR_CS2                    ( 0x00000008u )
#define UT_SBS_CCCSR_RDY1                   ( 0x00000100u )
#define UT_SBS_CCCSR_RDY2                   ( 0x00000200u )
#define UT_SBS_CCCSR_OWNED                  ( 0x0000000Fu )

/** Shift of the enable bit to the ready flag of the cell 1 (bit 0 to bit 8) and of the cell 2 (bit 2 to bit 9) */
#define UT_SBS_RDY1_SHIFT                   ( 8u )
#define UT_SBS_RDY2_SHIFT                   ( 7u )

/** Shift of the code selection bit CSx to the enable bit ENx of the same cell (bit 1 to bit 0, bit 3 to bit 2) */
#define UT_SBS_CS_TO_EN_SHIFT               ( 1u )

/* SBS_CCSWCR (0x118): SW_ANSRC1[3:0], SW_APSRC1[7:4], SW_ANSRC2[11:8], SW_APSRC2[15:12] */
#define UT_SBS_CCSWCR_OWNED                 ( 0x0000FFFFu )
#define UT_SBS_CCSWCR_RESET                 ( 0x00007878u )
#define UT_SBS_CCSWCR_CELL1_MASK            ( 0x000000FFu )
#define UT_SBS_CCSWCR_CELL2_MASK            ( 0x0000FF00u )
#define UT_SBS_CCSWCR_NMOS1_MASK            ( 0x0000000Fu )

/* Fields of SBS_CCSWCR: cell 1 with NMOS 0x3 and PMOS 0xC, cell 2 with NMOS 0xF and PMOS 0x0, and the cell 1 with
 * NMOS 0x3 and PMOS 0xC together with the cell 2 with the code of the reset state */
#define UT_SBS_CCSWCR_CELL1_MANUAL          ( 0x000000C3u )
#define UT_SBS_CCSWCR_CELL2_F0              ( 0x00000F00u )
#define UT_SBS_CCSWCR_MANUAL_AND_RESET      ( 0x000078C3u )

/* SBS_CFGR2 (0x120): CLL (0), SEL (1), PVDL (2), ECCL (3) */
#define UT_SBS_CFGR2_CLL                    ( 0x00000001u )
#define UT_SBS_CFGR2_SEL                    ( 0x00000002u )
#define UT_SBS_CFGR2_PVDL                   ( 0x00000004u )
#define UT_SBS_CFGR2_ECCL                   ( 0x00000008u )
#define UT_SBS_CFGR2_OWNED                  ( 0x0000000Fu )

/* SBS_HDPLSR (0x014): HDPL[7:0], codes of RM0481 Table 135 */
#define UT_SBS_HDPL_CODE_0                  ( 0xB4u )
#define UT_SBS_HDPL_CODE_1                  ( 0x51u )
#define UT_SBS_HDPL_CODE_2                  ( 0x8Au )
#define UT_SBS_HDPL_CODE_3                  ( 0x6Fu )
#define UT_SBS_HDPL_CODE_RESERVED           ( 0x00u )

/* SBS_DBGCR (0x020): AP_UNLOCK[7:0], DBG_UNLOCK[15:8], DBG_AUTH_HDPL[23:16], DBG_AUTH_SEC[31:24], unlock code 0xB4 */
#define UT_SBS_CODE_OPEN                    ( 0xB4u )
#define UT_SBS_CODE_LOCKED                  ( 0x6Au )
#define UT_SBS_DBGCR_AP_UNLOCK              ( UT_SBS_CODE_OPEN )
#define UT_SBS_DBGCR_DBG_UNLOCK             ( UT_SBS_CODE_OPEN << 8u )
#define UT_SBS_DBGCR_AUTH_HDPL1             ( UT_SBS_HDPL_CODE_1 << 16u )
#define UT_SBS_DBGCR_AUTH_SEC               ( UT_SBS_CODE_OPEN << 24u )
#define UT_SBS_CODE_WRONG                   ( 0xB5u )

/** SBS_DBGCR with the unlock code in every field, and with a wrong code in the fields AP_UNLOCK, DBG_UNLOCK and
 *  DBG_AUTH_SEC */
#define UT_SBS_DBGCR_ALL_UNLOCKED           ( UT_SBS_DBGCR_AP_UNLOCK  | \
                                              UT_SBS_DBGCR_DBG_UNLOCK | \
                                              UT_SBS_DBGCR_AUTH_HDPL1 | \
                                              UT_SBS_DBGCR_AUTH_SEC     )
#define UT_SBS_DBGCR_WRONG_CODES            ( UT_SBS_CODE_WRONG             | \
                                              ( UT_SBS_CODE_WRONG << 8u )   | \
                                              ( UT_SBS_CODE_WRONG << 24u )    )

/* SBS_MESR (0x108): MCLR (bit 0), IPMEE (bit 16) */
#define UT_SBS_MESR_MCLR                    ( 0x00000001u )
#define UT_SBS_MESR_IPMEE                   ( 0x00010000u )

/** Count of the FPU exceptions (FPU_IE[5:0]) */
#define UT_SBS_FPU_EXCEPTION_CNT            ( 6u )

/** Count of the iterations of the loops over the FM+ items */
#define UT_SBS_FMPLUS_ITEM_CNT              ( sizeof( utSbs_FmPlusLut ) / sizeof( utSbs_FmPlusItem_t ) )

/** Count of the break sources */
#define UT_SBS_BREAK_ITEM_CNT               ( sizeof( utSbs_BreakLut ) / sizeof( utSbs_BreakItem_t ) )

/** Count of the temporal isolation levels */
#define UT_SBS_HDPL_ITEM_CNT                ( sizeof( utSbs_HdplLut ) / sizeof( utSbs_HdplItem_t ) )

/* ========================== LOCAL VARIABLES =============================== */

/** \brief Expected mapping of the pins to the Fast-mode Plus bits (RM0481, RM0492, RM0517), in the order of the list */
static const utSbs_FmPlusItem_t utSbs_FmPlusLut[] =
{
    { SBS_FMPLUS_PIN_PB6, UT_SBS_PMCR_PB6_FMP },
    { SBS_FMPLUS_PIN_PB7, UT_SBS_PMCR_PB7_FMP },
    { SBS_FMPLUS_PIN_PB8, UT_SBS_PMCR_PB8_FMP },
#if defined(SBS_PMCR_PB9_FMP)
    { SBS_FMPLUS_PIN_PB9, UT_SBS_PMCR_PB9_FMP },
#endif
};


/** \brief Expected mapping of the break sources to the connection bits of SBS_CFGR2 */
static const utSbs_BreakItem_t utSbs_BreakLut[] =
{
    { SBS_BREAK_LOCKUP,    UT_SBS_CFGR2_CLL  },
    { SBS_BREAK_SRAM_ECC,  UT_SBS_CFGR2_SEL  },
    { SBS_BREAK_FLASH_ECC, UT_SBS_CFGR2_ECCL },
    { SBS_BREAK_PVD,       UT_SBS_CFGR2_PVDL },
};


/** \brief Expected temporal isolation levels */
static const utSbs_HdplItem_t utSbs_HdplLut[] =
{
    { UT_SBS_HDPL_CODE_0, SBS_HDPL_0 },
    { UT_SBS_HDPL_CODE_1, SBS_HDPL_1 },
    { UT_SBS_HDPL_CODE_2, SBS_HDPL_2 },
    { UT_SBS_HDPL_CODE_3, SBS_HDPL_3 },
};


/** Set by the HW models when the compensation cell sequence is not respected */
static volatile uint32_t utSbs_OrderViolation;

/** Arms the tripwire HW model (the activation of the model must not change a register) */
static volatile uint32_t utSbs_TripwireArmed;

#if defined(ETH)
/** State of the Ethernet MAC clock reported by the RCC stub and the answer of the RCC stub */
static rcc_FunctionState_t utSbs_MacState;
static rcc_RequestState_t  utSbs_MacRequest;

/** Count of the calls of the RCC stub of the MAC clock state */
static uint32_t utSbs_MacCallCnt;
#endif

/* ============================ TEST FIXTURE ================================ */

void setUp( void )
{
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Reset() );

    utSbs_OrderViolation = 0u;
    utSbs_TripwireArmed  = 0u;

#if defined(ETH)
    utSbs_MacState   = RCC_FUNCTION_INACTIVE;
    utSbs_MacRequest = RCC_REQUEST_OK;
    utSbs_MacCallCnt = 0u;
#endif
}


void tearDown( void )
{
    /* Mocks are verified by generated runner */
}

/* ========================== MODULE VERSION ================================ */

/**
 * \brief   Sbs_Get_ModuleVersion() returns version of the module.
 *
 * \details Reads the module version structure.
 *
 * \par Expected results
 * - Version is 0.1.0 (Major 0, Minor 1, Patch 0).
 */
void Ut_Sbs_Get_ModuleVersion_ReturnsVersion( void )
{
    sbs_ModuleVersion_t version = Sbs_Get_ModuleVersion();

    TEST_ASSERT_EQUAL_UINT8( UT_SBS_VERSION_MAJOR, version.Major );
    TEST_ASSERT_EQUAL_UINT8( UT_SBS_VERSION_MINOR, version.Minor );
    TEST_ASSERT_EQUAL_UINT8( UT_SBS_VERSION_PATCH, version.Patch );
}

/* =========================== DEFAULT CONFIG =============================== */

/**
 * \brief   Sbs_Get_DefaultConfig() fills default configuration.
 *
 * \details Fills the structure with a pattern, reads the default configuration, then calls
 *          the function with NULL.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, no Fast-mode Plus pin (every item), analog booster inactive, both compensation cells
 *   disabled with the codes of the reset state (NMOS 8, PMOS 7), no break source, FPU interrupts of the reset
 *   state (0x1F).
 * - NULL pointer: SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Get_DefaultConfig_ReturnsDefaults( void )
{
    sbs_Config_t config;

    for( uint32_t pinIdx = 0u; SBS_FMPLUS_PIN_CNT > pinIdx; pinIdx++ )
    {
        config.FmPlus[ pinIdx ] = SBS_FUNCTION_ACTIVE;
    }

    for( uint32_t cellIdx = 0u; SBS_IO_COMP_CELL_CNT > cellIdx; cellIdx++ )
    {
        config.IoCompensation.Cell[ cellIdx ].Mode     = SBS_IO_COMP_MODE_MANUAL;
        config.IoCompensation.Cell[ cellIdx ].NmosCode = SBS_IO_COMP_CODE_MAX;
        config.IoCompensation.Cell[ cellIdx ].PmosCode = SBS_IO_COMP_CODE_MAX;
    }

    config.AnalogBooster    = SBS_FUNCTION_ACTIVE;
    config.BreakSource      = SBS_BREAK_ALL;
    config.FpuInterruptMask = SBS_FPU_INT_NONE;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );

    for( uint32_t pinIdx = 0u; SBS_FMPLUS_PIN_CNT > pinIdx; pinIdx++ )
    {
        TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, config.FmPlus[ pinIdx ] );
    }

    for( uint32_t cellIdx = 0u; SBS_IO_COMP_CELL_CNT > cellIdx; cellIdx++ )
    {
        TEST_ASSERT_EQUAL( SBS_IO_COMP_MODE_DISABLED, config.IoCompensation.Cell[ cellIdx ].Mode );
        TEST_ASSERT_EQUAL_UINT8( UT_SBS_CODE_NMOS_RESET, config.IoCompensation.Cell[ cellIdx ].NmosCode );
        TEST_ASSERT_EQUAL_UINT8( UT_SBS_CODE_PMOS_RESET, config.IoCompensation.Cell[ cellIdx ].PmosCode );
    }

    TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, config.AnalogBooster );
    TEST_ASSERT_EQUAL_HEX32( 0u, config.BreakSource );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_FPUIMR_RESET, config.FpuInterruptMask );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_DefaultConfig( NULL ) );
}

/* ============================ DEVICE LINE ================================= */

/**
 * \brief   The lists of the device line have the items of the reference manuals.
 *
 * \details Compares the sizes of the lists with the known values of the line (RM0481, RM0492, RM0517,
 *          CMSIS device headers): Fast-mode Plus pins 3 (STM32H503) or 4, Ethernet PHY interfaces 2 on the lines
 *          with Ethernet (STM32H543, H553, H563, H573, H5E4, H5E5, H5F4, H5F5) or none, compensation cells 2,
 *          no memory remap.
 *
 * \par Expected results
 * - The counts of the lists equal the known values and the independent tables of the tests.
 */
void Ut_Sbs_Lists_DeviceLine_HaveItemsOfReferenceManual( void )
{
    TEST_ASSERT_EQUAL_UINT32( UT_SBS_FMPLUS_PIN_CNT, SBS_FMPLUS_PIN_CNT );
    TEST_ASSERT_EQUAL_UINT32( UT_SBS_FMPLUS_PIN_CNT, UT_SBS_FMPLUS_ITEM_CNT );
    TEST_ASSERT_EQUAL_UINT32( UT_SBS_IO_COMP_CELL_CNT, SBS_IO_COMP_CELL_CNT );
    TEST_ASSERT_EQUAL_UINT32( 0u, SBS_MEMORY_REMAP_CNT );

#if defined(ETH)
    TEST_ASSERT_EQUAL_UINT32( UT_SBS_ETH_PHY_CNT, SBS_ETH_PHY_CNT );
#else
    TEST_ASSERT_EQUAL_UINT32( 0u, SBS_ETH_PHY_CNT );
#endif

    TEST_ASSERT_EQUAL_UINT32( UT_SBS_BREAK_SOURCE_CNT, UT_SBS_BREAK_ITEM_CNT );
    TEST_ASSERT_EQUAL_UINT32( UT_SBS_HDPL_LEVEL_CNT, UT_SBS_HDPL_ITEM_CNT );
    TEST_ASSERT_EQUAL_UINT32( SBS_HDPL_CNT, UT_SBS_HDPL_ITEM_CNT );
}

/* ============================ INITIALIZATION ============================== */

/**
 * \brief   Sbs_Init() with a NULL configuration is refused.
 *
 * \details Presets noise to the registers, calls the function with NULL, then tries functions
 *          which need the initialization.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written, the module is not initialized, the RCC module is not used.
 */
void Ut_Sbs_Init_NullConfig_ReturnsErrorNothingWritten( void )
{
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( NULL ) );

    UtSbs_Check_Noise( 0u );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_NONE ) );
    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   Sbs_Init() with the default configuration writes the reset state of the owned bits.
 *
 * \details Presets every bit of SBS_PMCR, SBS_FPUIMR, SBS_CCCSR and SBS_CCSWCR, the connection bits of SBS_CFGR2 are
 *          cleared. Initializes the module with the default configuration.
 *
 * \par Expected results
 * - SBS_REQUEST_OK.
 * - SBS_PMCR: Fast-mode Plus bits cleared, the other bits (also ETH_SEL_PHY) kept.
 * - SBS_FPUIMR: FPU_IE = 0x1F, the other bits kept.
 * - SBS_CCCSR: EN1, EN2 cleared, CS1, CS2 set (software code), the other bits (ready flags) kept.
 * - SBS_CCSWCR: both codes 0x78 (NMOS 8, PMOS 7).
 * - SBS_CFGR2 is not written (the other bits keep their value).
 */
void Ut_Sbs_Init_DefaultConfig_WritesResetStateOfOwnedBits( void )
{
    sbs_Config_t config;

    SBS->PMCR   = UT_SBS_ALL_BITS;
    SBS->FPUIMR = UT_SBS_ALL_BITS;
    SBS->CCCSR  = UT_SBS_ALL_BITS;
    SBS->CCSWCR = UT_SBS_ALL_BITS;
    SBS->CFGR2  = ~UT_SBS_CFGR2_OWNED;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    UtSbs_Expect_Init();
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( &config ) );

    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_FMPLUS_ALL, SBS->PMCR );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_FPUIMR_OWNED ) | UT_SBS_FPUIMR_RESET, SBS->FPUIMR );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CCCSR_OWNED ) | UT_SBS_CCCSR_CS1 | UT_SBS_CCCSR_CS2, SBS->CCCSR );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CCSWCR_OWNED ) | UT_SBS_CCSWCR_RESET, SBS->CCSWCR );
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_CFGR2_OWNED, SBS->CFGR2 );
}


/**
 * \brief   Sbs_Init() refuses when the RCC module does not provide the clock of the SBS.
 *
 * \details The RCC mock returns an error for the clock request. Noise is preset to the registers.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written, the module is not initialized.
 */
void Ut_Sbs_Init_ClockRefused_ReturnsErrorNothingWritten( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ] = SBS_FUNCTION_ACTIVE;
    config.BreakSource                  = SBS_BREAK_LOCKUP;

    UtSbs_Preset_Noise( 0u );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_SBS, RCC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( &config ) );

    UtSbs_Check_Noise( 0u );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB6, SBS_FUNCTION_ACTIVE ) );
    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   Sbs_Init() refuses an invalid Fast-mode Plus state before any register is written.
 *
 * \details Sets an item of the Fast-mode Plus table outside of the enumeration.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written, the RCC module is not used, the module is not initialized.
 */
void Ut_Sbs_Init_InvalidFmPlusState_ReturnsErrorNothingWritten( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FmPlus[ UT_SBS_FMPLUS_PIN_LAST ] = (sbs_FunctionState_t)UT_SBS_INVALID_STATE;

    UtSbs_Check_InitRefused( &config );
}


/**
 * \brief   Sbs_Init() refuses the activation of the analog booster before any register is written.
 *
 * \details The analog switch booster of STM32H5 is in the PWR module, the SBS has no bit for it.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written, the RCC module is not used.
 * - An invalid state of the analog booster is refused as well.
 */
void Ut_Sbs_Init_AnalogBoosterActive_ReturnsErrorNothingWritten( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.AnalogBooster = SBS_FUNCTION_ACTIVE;

    UtSbs_Check_InitRefused( &config );

    config.AnalogBooster = (sbs_FunctionState_t)UT_SBS_INVALID_STATE;

    UtSbs_Check_InitRefused( &config );
}


/**
 * \brief   Sbs_Init() refuses an invalid compensation configuration before any register is written.
 *
 * \details Tests a mode outside of the list and a manual code above the 4 bits of the cell (NMOS and PMOS, both cells).
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for every case, no register is written, the RCC module is not used.
 */
void Ut_Sbs_Init_InvalidCompensation_ReturnsErrorNothingWritten( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].Mode = SBS_IO_COMP_MODE_CNT;

    UtSbs_Check_InitRefused( &config );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].Mode     = SBS_IO_COMP_MODE_MANUAL;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].NmosCode = UT_SBS_CODE_INVALID;

    UtSbs_Check_InitRefused( &config );

    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].NmosCode = SBS_IO_COMP_CODE_MAX;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].PmosCode = UT_SBS_CODE_INVALID;

    UtSbs_Check_InitRefused( &config );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].Mode     = SBS_IO_COMP_MODE_MANUAL;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].PmosCode = UT_SBS_CODE_INVALID;

    UtSbs_Check_InitRefused( &config );
}


/**
 * \brief   Sbs_Init() refuses an invalid break source set or FPU mask before any register is written.
 *
 * \details Sets a bit outside of the sets of the interface.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for both cases, no register is written, the RCC module is not used.
 */
void Ut_Sbs_Init_InvalidMasks_ReturnsErrorNothingWritten( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.BreakSource = UT_SBS_BREAK_INVALID;

    UtSbs_Check_InitRefused( &config );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FpuInterruptMask = UT_SBS_FPU_INT_INVALID;

    UtSbs_Check_InitRefused( &config );
}


/**
 * \brief   Sbs_Init() refuses a break source set which would remove a connected source.
 *
 * \details The core lockup is connected (SBS_CFGR2.CLL set), the configuration requests the voltage detector
 *          only. Noise is preset to the registers. The clock of the SBS is requested before the connected set
 *          is read.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written (SBS_CFGR2 keeps CLL and does not get PVDL), the module is not
 *   initialized.
 */
void Ut_Sbs_Init_BreakRequestRemovesConnected_ReturnsErrorNothingWritten( void )
{
    sbs_Config_t        config;
    sbs_FunctionState_t state = SBS_FUNCTION_INACTIVE;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.BreakSource = SBS_BREAK_PVD;

    UtSbs_Preset_Noise( UT_SBS_CFGR2_CLL );
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_SBS, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( &config ) );

    UtSbs_Check_Noise( UT_SBS_CFGR2_CLL );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FmPlus( SBS_FMPLUS_PIN_PB6, &state ) );
}


/**
 * \brief   Sbs_Init() writes the Fast-mode Plus pins and the FPU mask of the configuration.
 *
 * \details Requests the high drive of PB6 and PB8 (and PB9 where it exists) and the FPU exceptions overflow and
 *          inexact. Noise is preset to the registers.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, SBS_PMCR holds the requested pin bits and the other Fast-mode Plus bits cleared, the ETH_SEL_PHY
 *   field and the other bits are kept, SBS_FPUIMR.FPU_IE = overflow | inexact (0x28), the other bits kept.
 */
void Ut_Sbs_Init_FmPlusAndFpuMask_WritesRequestedBits( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ] = SBS_FUNCTION_ACTIVE;
    config.FmPlus[ SBS_FMPLUS_PIN_PB8 ] = SBS_FUNCTION_ACTIVE;
    config.FpuInterruptMask             = SBS_FPU_INT_OVERFLOW | SBS_FPU_INT_INEXACT;

    UtSbs_Preset_Noise( 0u );
    SBS->PMCR = UT_SBS_ALL_BITS;
    UtSbs_Expect_Init();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( &config ) );

    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_FMPLUS_ALL ) | UT_SBS_PMCR_PB6_FMP | UT_SBS_PMCR_PB8_FMP, SBS->PMCR );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_FPUIMR_OWNED ) | UT_SBS_FPUIMR_OVERFLOW_INEXACT, SBS->FPUIMR );
}


/**
 * \brief   Sbs_Init() configures the compensation cells: automatic mode of one cell, manual codes of the other.
 *
 * \details The HW model sets the ready flag of an enabled cell. The cell VDDIO is automatic, the cell VDDIO2 is
 *          manual with the NMOS code 0xF and the PMOS code 0x0. The CSI oscillator is requested from the RCC module.
 *
 * \par Expected results
 * - SBS_REQUEST_OK.
 * - Cell VDDIO: EN1 set, CS1 cleared (the code of the cell is selected), RDY1 set.
 * - Cell VDDIO2: EN2 cleared, CS2 set, SBS_CCSWCR field of the cell 2 = 0x0F (NMOS 0xF, PMOS 0x0), field of the cell 1
 *   untouched (an automatic cell gets no software code).
 */
void Ut_Sbs_Init_AutomaticAndManualCell_ConfiguresCells( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO  ].Mode     = SBS_IO_COMP_MODE_AUTOMATIC;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].Mode     = SBS_IO_COMP_MODE_MANUAL;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].NmosCode = UT_SBS_CODE_NMOS_HIGH;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].PmosCode = UT_SBS_CODE_PMOS_LOW;

    SBS->CCSWCR = 0u;
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_CellReady ) );
    UtSbs_Expect_InitCsi();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( &config ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCCSR_EN1 | UT_SBS_CCCSR_RDY1 | UT_SBS_CCCSR_CS2, SBS->CCCSR );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCSWCR_CELL2_F0, SBS->CCSWCR & UT_SBS_CCSWCR_CELL2_MASK );
    TEST_ASSERT_EQUAL_HEX32( 0u, SBS->CCSWCR & UT_SBS_CCSWCR_CELL1_MASK );
}


/**
 * \brief   Sbs_Init() connects the break sources and the module accepts the connected set again.
 *
 * \details Requests the core lockup and the voltage detector, then initializes the module once more with the same
 *          configuration (the connected set equals the request).
 *
 * \par Expected results
 * - SBS_REQUEST_OK for both calls, SBS_CFGR2 holds CLL and PVDL, SEL and ECCL are not set, the other bits are kept.
 */
void Ut_Sbs_Init_BreakSources_ConnectsRequestedSources( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.BreakSource = SBS_BREAK_LOCKUP | SBS_BREAK_PVD;

    SBS->CFGR2 = ~UT_SBS_CFGR2_OWNED;
    UtSbs_Expect_Init();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( &config ) );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL | UT_SBS_CFGR2_PVDL, SBS->CFGR2 );

    UtSbs_Expect_Init();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( &config ) );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL | UT_SBS_CFGR2_PVDL, SBS->CFGR2 );
}


/**
 * \brief   Sbs_Init() verifies the Fast-mode Plus bits and connects the break sources last.
 *
 * \details The HW model clears the bit of PB6 after every store (the pin is secure in the GPIO). The configuration
 *          requests PB6 and the core lockup. The module is initialized once before to see that the failed call
 *          deinitializes it.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, the break source is not connected (SBS_CFGR2 = 0): the irreversible step is the last one and is
 *   not reached.
 * - The Fast-mode Plus bits are cleared, the module is deinitialized (a setter returns an error).
 */
void Ut_Sbs_Init_FmPlusReadBackMismatch_ReturnsErrorBreakNotConnected( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    UtSbs_Init_Default();

    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ] = SBS_FUNCTION_ACTIVE;
    config.BreakSource                  = SBS_BREAK_LOCKUP;

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_Pb6NotKept ) );
    UtSbs_Expect_Init();

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( &config ) );

    TEST_ASSERT_EQUAL_HEX32( 0u, SBS->CFGR2 );
    TEST_ASSERT_EQUAL_HEX32( 0u, SBS->PMCR & UT_SBS_FMPLUS_ALL );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB7, SBS_FUNCTION_ACTIVE ) );
}


/**
 * \brief   Sbs_Init() verifies the FPU interrupt mask by read-back.
 *
 * \details The HW model clears the FPU_IE[1] after every store (access which the hardware ignores). The default
 *          configuration requests 0x1F.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, the module is not initialized (a getter returns an error).
 */
void Ut_Sbs_Init_FpuReadBackMismatch_ReturnsErrorDeinitialized( void )
{
    sbs_Config_t config;
    sbs_FpuInterrupt_t mask = 0u;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_FpuBit1NotKept ) );
    UtSbs_Expect_Init();

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( &config ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FpuInterruptMask( &mask ) );
}


/**
 * \brief   Sbs_Init() verifies the connection of the break sources by read-back.
 *
 * \details The HW model clears CLL after every store. The configuration requests the core lockup.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, the module is not initialized.
 */
void Ut_Sbs_Init_BreakReadBackMismatch_ReturnsErrorDeinitialized( void )
{
    sbs_Config_t config;
    sbs_BreakSource_t source = 0u;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.BreakSource = SBS_BREAK_LOCKUP;

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_LockupNotKept ) );
    UtSbs_Expect_Init();

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( &config ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_BreakSource( &source ) );
}


/**
 * \brief   Sbs_Init() fails when the RCC module refuses the CSI oscillator needed by the automatic cell.
 *
 * \details Requests PB6 and the FPU exceptions of the reset state plus inexact, and the automatic mode of the cell
 *          VDDIO. The oscillator request is refused. The registers written before are cleared again.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, the cell is not enabled (EN1 = 0), the Fast-mode Plus bit of PB6 is cleared, the FPU mask is
 *   restored to 0x1F, the module is not initialized.
 */
void Ut_Sbs_Init_CsiRefused_ReturnsErrorRegistersCleared( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ]                       = SBS_FUNCTION_ACTIVE;
    config.FpuInterruptMask                                   = SBS_FPU_INT_ALL;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].Mode = SBS_IO_COMP_MODE_AUTOMATIC;

    UtSbs_Expect_Init();
    Rcc_Set_OscActive_ExpectAndReturn( RCC_OSC_CSI, RCC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( &config ) );

    UtSbs_Check_ResetState();
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB6, SBS_FUNCTION_ACTIVE ) );
}


/**
 * \brief   Sbs_Init() fails when the automatic compensation cell does not become ready.
 *
 * \details No ready flag is set (no HW model). The cell VDDIO is automatic, the break source lockup is requested.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR after the bounded wait, the cell is disabled again by the reset of the registers (EN1 = 0,
 *   CS1 = 1), the break source is not connected, the module is not initialized.
 */
void Ut_Sbs_Init_CellNotReady_ReturnsErrorCellDisabled( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.BreakSource                                        = SBS_BREAK_LOCKUP;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].Mode = SBS_IO_COMP_MODE_AUTOMATIC;

    UtSbs_Expect_InitCsi();

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( &config ) );

    UtSbs_Check_ResetState();
    TEST_ASSERT_EQUAL_HEX32( 0u, SBS->CFGR2 );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( &config.IoCompensation ) );
}


/**
 * \brief   Sbs_Init() called twice applies the configuration again.
 *
 * \details Initializes the module with PB6, then with PB7 (the high drive of PB6 is cleared by the second call).
 *
 * \par Expected results
 * - SBS_REQUEST_OK for both calls, the second call leaves PB7 only.
 */
void Ut_Sbs_Init_CalledTwice_AppliesNewConfiguration( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ] = SBS_FUNCTION_ACTIVE;
    UtSbs_Init_Config( &config );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_PMCR_PB6_FMP, SBS->PMCR & UT_SBS_FMPLUS_ALL );

    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ] = SBS_FUNCTION_INACTIVE;
    config.FmPlus[ SBS_FMPLUS_PIN_PB7 ] = SBS_FUNCTION_ACTIVE;
    UtSbs_Init_Config( &config );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_PMCR_PB7_FMP, SBS->PMCR & UT_SBS_FMPLUS_ALL );
}

/* ============================ DEINITIALIZATION ============================ */

/**
 * \brief   Sbs_Deinit() of a module which was not initialized writes nothing.
 *
 * \details Presets noise, deinitializes.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, no register is written, the RCC module is not used.
 */
void Ut_Sbs_Deinit_NotInitialized_ReturnsOkNothingWritten( void )
{
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Deinit() );

    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   Sbs_Deinit() restores the reset state and keeps the connected break sources.
 *
 * \details Initializes the module with PB6, the FPU mask 0x3F, the manual codes of the cell VDDIO and the break
 *          source lockup, then deinitializes it.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, Fast-mode Plus bits cleared, FPU_IE = 0x1F, both cells disabled with CS = 1 and the code 0x7878,
 *   SBS_CFGR2 keeps CLL (the connection can not be removed), the module is not initialized (a getter returns an
 *   error), the clock of the SBS is not disabled (the RCC module is not used).
 */
void Ut_Sbs_Deinit_Initialized_RestoresResetStateKeepsBreak( void )
{
    sbs_Config_t config;
    sbs_FpuInterrupt_t mask = 0u;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ]                           = SBS_FUNCTION_ACTIVE;
    config.FpuInterruptMask                                       = SBS_FPU_INT_ALL;
    config.BreakSource                                            = SBS_BREAK_LOCKUP;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].Mode     = SBS_IO_COMP_MODE_MANUAL;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].NmosCode = UT_SBS_CODE_NMOS_MANUAL;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].PmosCode = UT_SBS_CODE_PMOS_MANUAL;
    UtSbs_Init_Config( &config );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCSWCR_CELL1_MANUAL, SBS->CCSWCR & UT_SBS_CCSWCR_CELL1_MASK );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Deinit() );

    UtSbs_Check_ResetState();
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CFGR2_CLL, SBS->CFGR2 );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FpuInterruptMask( &mask ) );
}


/**
 * \brief   Sbs_Deinit() reports a register which does not keep the value but deinitializes the module.
 *
 * \details Initializes the module with the default configuration, activates the HW model which clears FPU_IE[1] after
 *          every store (the restored FPU mask 0x1F does not stay) and deinitializes the module.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR (the FPU mask 0x1F does not read back), the module is deinitialized (a setter returns an error).
 */
void Ut_Sbs_Deinit_ReadBackMismatch_ReturnsErrorDeinitialized( void )
{
    UtSbs_Init_Default();

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_FpuBit1NotKept ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Deinit() );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_NONE ) );
}


/**
 * \brief   Sbs_Deinit() reports a Fast-mode Plus bit which does not clear but deinitializes the module.
 *
 * \details Initializes the module with the high drive of PB6, activates the HW model which keeps the bit of PB6 set
 *          after every store (the clear is ignored) and deinitializes the module.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR (the Fast-mode Plus bits do not read back as cleared), the other registers of the reset state are
 *   written (FPU_IE = 0x1F), the module is deinitialized (a setter returns an error).
 */
void Ut_Sbs_Deinit_FmPlusReadBackMismatch_ReturnsErrorDeinitialized( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ] = SBS_FUNCTION_ACTIVE;
    config.FpuInterruptMask             = SBS_FPU_INT_ALL;
    UtSbs_Init_Config( &config );

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_Pb6Sticky ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Deinit() );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_FPUIMR_RESET, SBS->FPUIMR & UT_SBS_FPUIMR_OWNED );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB7, SBS_FUNCTION_ACTIVE ) );
}


/**
 * \brief   Sbs_Deinit() reports a compensation cell which does not switch off but deinitializes the module.
 *
 * \details Initializes the module with the default configuration, activates the HW model which keeps the enable bit
 *          of the cell VDDIO set after every store (the clear is ignored) and deinitializes the module.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR (EN1 does not read back as cleared), the cell VDDIO2 is switched off (the sequence continues),
 *   the module is deinitialized (a setter returns an error).
 */
void Ut_Sbs_Deinit_CompensationReadBackMismatch_ReturnsErrorDeinitialized( void )
{
    UtSbs_Init_Default();

    SBS->CCCSR = UT_SBS_CCCSR_EN2;
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_Enable1Sticky ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Deinit() );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCCSR_EN1 | UT_SBS_CCCSR_CS1 | UT_SBS_CCCSR_CS2, SBS->CCCSR );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB7, SBS_FUNCTION_ACTIVE ) );
}

/* ================================== TASK ================================== */

/**
 * \brief   Sbs_Task() does nothing.
 *
 * \details Presets noise, calls the task before and after the initialization.
 *
 * \par Expected results
 * - No register is written, the RCC module is not used.
 */
void Ut_Sbs_Task_DoesNothing( void )
{
    UtSbs_Preset_Noise( 0u );

    Sbs_Task();

    UtSbs_Check_Noise( 0u );
}

/* ====================== FUNCTIONS BEFORE INITIALIZATION =================== */

/**
 * \brief   The setters return an error before Sbs_Init() and write nothing.
 *
 * \details Presets noise, calls every setter with valid parameters without the initialization.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for every setter, no register is written, the RCC module is not used (CMock).
 */
void Ut_Sbs_Setters_BeforeInit_ReturnErrorNothingWritten( void )
{
    const sbs_IoCompConfig_t compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_MANUAL, SBS_IO_COMP_MODE_DISABLED );

    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB6, SBS_FUNCTION_ACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_AnalogBooster( SBS_FUNCTION_INACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( &compConfig ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_BreakSource( SBS_BREAK_NONE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_ALL ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_MemoryRemap( SBS_MEMORY_REMAP_UNUSED ) );

#if defined(ETH)
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_RMII ) );
#else
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_UNUSED ) );
#endif

    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   The getters return an error before Sbs_Init() and have no side effects.
 *
 * \details Presets noise and a pattern to every output, calls every getter without the initialization.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for every getter, the outputs are not touched, no register is written, the clock is not
 *   enabled and no other RCC function is called (CMock).
 */
void Ut_Sbs_Getters_BeforeInit_ReturnErrorNoSideEffects( void )
{
    sbs_FunctionState_t    state          = (sbs_FunctionState_t)UT_SBS_OUTPUT_PATTERN;
    sbs_FunctionState_t    readyState     = (sbs_FunctionState_t)UT_SBS_OUTPUT_PATTERN;
    sbs_BreakSource_t      breakSource    = UT_SBS_OUTPUT_PATTERN;
    sbs_FpuInterrupt_t     fpuInterrupt   = UT_SBS_OUTPUT_PATTERN;
    sbs_EthPhyInterface_t  phyInterface   = (sbs_EthPhyInterface_t)UT_SBS_OUTPUT_PATTERN;
    sbs_MemoryRemap_t      memoryRemap    = (sbs_MemoryRemap_t)UT_SBS_OUTPUT_PATTERN;
    sbs_BootState_t        bootState      = { (sbs_Hdpl_t)UT_SBS_OUTPUT_PATTERN, UT_SBS_OUTPUT_PATTERN };
    sbs_ProductState_t     productState   = (sbs_ProductState_t)UT_SBS_OUTPUT_PATTERN;
    sbs_DebugState_t       debugState     = UT_SBS_OUTPUT_PATTERN;
    sbs_EraseState_t       eraseState     = UT_SBS_OUTPUT_PATTERN;

    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FmPlus( SBS_FMPLUS_PIN_PB6, &state ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_AnalogBooster( &state ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_IoCompensationState( &readyState ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_BreakSource( &breakSource ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_EthPhyInterface( &phyInterface ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FpuInterruptMask( &fpuInterrupt ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_MemoryRemap( &memoryRemap ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_BootState( &bootState ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_ProductState( &productState ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_DebugState( &debugState ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_EraseState( &eraseState ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, state );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, readyState );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, breakSource );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, fpuInterrupt );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, phyInterface );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, memoryRemap );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, bootState.Hdpl );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, bootState.BootAddress );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, productState );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, debugState );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, eraseState );

    UtSbs_Check_Noise( 0u );
}

/* ============================== FAST-MODE PLUS ============================ */

/**
 * \brief   Sbs_Set_FmPlus() sets and clears the bit of every pin of the line.
 *
 * \details For every pin of the list: sets the high drive, checks SBS_PMCR against the independent table of the
 *          reference manuals (PB6 ... PB9 = bits 16 ... 19), clears it again. The bits of the other pins and the
 *          bits outside of the Fast-mode Plus bits are preset to 1 and shall be kept.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, exactly the bit of the pin changes, the other bits of SBS_PMCR keep their value.
 */
void Ut_Sbs_Set_FmPlus_EveryPin_SetsAndClearsOnlyItsBit( void )
{
    UtSbs_Init_Default();

    for( uint32_t itemIdx = 0u; UT_SBS_FMPLUS_ITEM_CNT > itemIdx; itemIdx++ )
    {
        const uint32_t mask = utSbs_FmPlusLut[ itemIdx ].Mask;

        SBS->PMCR = ~mask;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FmPlus( utSbs_FmPlusLut[ itemIdx ].Pin, SBS_FUNCTION_ACTIVE ) );
        TEST_ASSERT_EQUAL_HEX32( UT_SBS_ALL_BITS, SBS->PMCR );

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FmPlus( utSbs_FmPlusLut[ itemIdx ].Pin, SBS_FUNCTION_INACTIVE ) );
        TEST_ASSERT_EQUAL_HEX32( ~mask, SBS->PMCR );
    }
}


/**
 * \brief   Sbs_Set_FmPlus() refuses an invalid pin or state before any register is written.
 *
 * \details Initializes the module, presets noise, requests a pin outside of the list (the count and the
 *          unused item) and a state outside of the enumeration.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for every request, no register is written.
 */
void Ut_Sbs_Set_FmPlus_InvalidParameter_ReturnsErrorNothingWritten( void )
{
    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_CNT, SBS_FUNCTION_ACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_UNUSED, SBS_FUNCTION_ACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR,
                       Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB6, (sbs_FunctionState_t)UT_SBS_INVALID_STATE ) );

    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   Sbs_Set_FmPlus() reports a bit which does not stay (pin secure in the GPIO).
 *
 * \details The HW model clears the bit of PB6 after every store. Requests the high drive of PB6 and of PB7.
 *
 * \par Expected results
 * - PB6: SBS_REQUEST_ERROR (read-back mismatch), PB7: SBS_REQUEST_OK.
 */
void Ut_Sbs_Set_FmPlus_ReadBackMismatch_ReturnsError( void )
{
    UtSbs_Init_Default();

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_Pb6NotKept ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB6, SBS_FUNCTION_ACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FmPlus( SBS_FMPLUS_PIN_PB7, SBS_FUNCTION_ACTIVE ) );
}


/**
 * \brief   Sbs_Get_FmPlus() reads the bit of every pin and has no side effects.
 *
 * \details Presets every bit of SBS_PMCR to 1, then the bit of one pin to 0 and reads every pin. The RCC module is
 *          not expected to be used. Then checks the refusal of invalid parameters.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, the pin with the cleared bit reads SBS_FUNCTION_INACTIVE, the others SBS_FUNCTION_ACTIVE.
 * - Pin outside of the list or NULL: SBS_REQUEST_ERROR, the output is not touched.
 */
void Ut_Sbs_Get_FmPlus_EveryPin_ReadsBit( void )
{
    sbs_FunctionState_t state = SBS_FUNCTION_INACTIVE;

    UtSbs_Init_Default();

    for( uint32_t itemIdx = 0u; UT_SBS_FMPLUS_ITEM_CNT > itemIdx; itemIdx++ )
    {
        SBS->PMCR = ~utSbs_FmPlusLut[ itemIdx ].Mask;

        for( uint32_t pinIdx = 0u; UT_SBS_FMPLUS_ITEM_CNT > pinIdx; pinIdx++ )
        {
            state = (sbs_FunctionState_t)UT_SBS_INVALID_STATE;

            TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FmPlus( utSbs_FmPlusLut[ pinIdx ].Pin, &state ) );

            if( pinIdx == itemIdx )
            {
                TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, state );
            }
            else
            {
                TEST_ASSERT_EQUAL( SBS_FUNCTION_ACTIVE, state );
            }
        }
    }

    state = (sbs_FunctionState_t)UT_SBS_OUTPUT_PATTERN;

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FmPlus( SBS_FMPLUS_PIN_CNT, &state ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, state );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FmPlus( SBS_FMPLUS_PIN_PB6, NULL ) );
}

/* ============================== ANALOG BOOSTER ============================ */

/**
 * \brief   The analog booster functions have no meaning on STM32H5 and write nothing.
 *
 * \details Initializes the module, presets noise and a pattern to the output, calls the setter with both states and
 *          an invalid one and the getter.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for every call (the booster is controlled by PWR_PMCR), no register is written, the output is
 *   not touched, the RCC module is not used.
 */
void Ut_Sbs_AnalogBooster_Initialized_ReturnsErrorNothingWritten( void )
{
    sbs_FunctionState_t state = (sbs_FunctionState_t)UT_SBS_OUTPUT_PATTERN;

    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_AnalogBooster( SBS_FUNCTION_ACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_AnalogBooster( SBS_FUNCTION_INACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_AnalogBooster( (sbs_FunctionState_t)UT_SBS_INVALID_STATE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_AnalogBooster( &state ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_AnalogBooster( NULL ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, state );
    UtSbs_Check_Noise( 0u );
}

/* ======================== I/O COMPENSATION CELLS ========================== */

/**
 * \brief   Sbs_Set_IoCompensation() refuses a NULL pointer and an invalid configuration before any write.
 *
 * \details Initializes the module, presets noise, calls the function with NULL, with a mode outside of the list and
 *          with a manual code above 15.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for every call, no register is written, the RCC module is not used.
 */
void Ut_Sbs_Set_IoCompensation_InvalidParameter_ReturnsErrorNothingWritten( void )
{
    sbs_IoCompConfig_t compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_AUTOMATIC, SBS_IO_COMP_MODE_CNT );

    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( NULL ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( &compConfig ) );

    compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_MANUAL, SBS_IO_COMP_MODE_DISABLED );
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO ].PmosCode = UT_SBS_CODE_INVALID;

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( &compConfig ) );

    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   Sbs_Set_IoCompensation() in the manual mode writes the code of the configuration and disables the cell.
 *
 * \details The cell VDDIO is enabled with the code selection cleared (as after an automatic mode), SBS_CCSWCR is
 *          cleared. Requests the manual mode of the cell VDDIO with NMOS 0x3, PMOS 0xC and the disabled mode of the
 *          cell VDDIO2. The HW model finds out whether the software code is selected before the code is written.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, the code of the cell 1 is 0xC3 (PMOS in bits 7:4, NMOS in bits 3:0), the cell 2 has the fixed
 *   code 0x78, CS1 and CS2 are set, EN1 and EN2 are cleared, the CSI oscillator is not requested, the software code
 *   is never selected before it is written.
 */
void Ut_Sbs_Set_IoCompensation_Manual_WritesCodeThenSelectsAndDisables( void )
{
    const sbs_IoCompConfig_t compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_MANUAL, SBS_IO_COMP_MODE_DISABLED );
    sbs_IoCompConfig_t       config     = compConfig;

    config.Cell[ SBS_IO_COMP_CELL_VDDIO ].NmosCode = UT_SBS_CODE_NMOS_MANUAL;
    config.Cell[ SBS_IO_COMP_CELL_VDDIO ].PmosCode = UT_SBS_CODE_PMOS_MANUAL;

    UtSbs_Init_Default();

    SBS->CCCSR  = UT_SBS_CCCSR_EN1 | UT_SBS_CCCSR_EN2;
    SBS->CCSWCR = 0u;
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_ManualOrder ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_IoCompensation( &config ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCSWCR_MANUAL_AND_RESET, SBS->CCSWCR );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCCSR_CS1 | UT_SBS_CCCSR_CS2, SBS->CCCSR );
    TEST_ASSERT_EQUAL_UINT32( 0u, utSbs_OrderViolation );
}


/**
 * \brief   Sbs_Set_IoCompensation() in the automatic mode enables the cell and selects its code when it is ready.
 *
 * \details The HW model sets the ready flag of an enabled cell and records a selection of the code of the cell 1
 *          (CS1 cleared) while the cell is not enabled. Requests the automatic mode of both cells. The CSI oscillator
 *          is requested once from the RCC module.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, EN1, EN2 set, CS1, CS2 cleared, RDY1, RDY2 set, the code of the cell is never selected before
 *   the cell is enabled, the CSI oscillator is requested exactly once.
 */
void Ut_Sbs_Set_IoCompensation_Automatic_EnablesCellThenSelectsCode( void )
{
    const sbs_IoCompConfig_t compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_AUTOMATIC,
                                                                SBS_IO_COMP_MODE_AUTOMATIC );

    UtSbs_Init_Default();

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_CellOrder ) );
    Rcc_Set_OscActive_ExpectAndReturn( RCC_OSC_CSI, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_IoCompensation( &compConfig ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCCSR_EN1 | UT_SBS_CCCSR_EN2 | UT_SBS_CCCSR_RDY1 | UT_SBS_CCCSR_RDY2, SBS->CCCSR );
    TEST_ASSERT_EQUAL_UINT32( 0u, utSbs_OrderViolation );
}


/**
 * \brief   Sbs_Set_IoCompensation() in the automatic mode fails when the cell does not become ready.
 *
 * \details No ready flag is set (no HW model). Requests the automatic mode of the cell VDDIO2 only.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR after the bounded wait, the cell VDDIO stays as it was, the cell VDDIO2 stays enabled and its
 *   software code stays selected (CS2 = 1: the unready code of the cell is never applied).
 */
void Ut_Sbs_Set_IoCompensation_Automatic_NotReady_ReturnsErrorCodeNotSelected( void )
{
    const sbs_IoCompConfig_t compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_DISABLED, SBS_IO_COMP_MODE_AUTOMATIC );

    UtSbs_Init_Default();

    Rcc_Set_OscActive_ExpectAndReturn( RCC_OSC_CSI, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( &compConfig ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCCSR_EN2 | UT_SBS_CCCSR_CS1 | UT_SBS_CCCSR_CS2, SBS->CCCSR );
}


/**
 * \brief   Sbs_Set_IoCompensation() writes nothing when the CSI oscillator is refused.
 *
 * \details Presets noise, the RCC mock refuses the oscillator.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written.
 */
void Ut_Sbs_Set_IoCompensation_Automatic_CsiRefused_ReturnsErrorNothingWritten( void )
{
    const sbs_IoCompConfig_t compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_AUTOMATIC, SBS_IO_COMP_MODE_DISABLED );

    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    Rcc_Set_OscActive_ExpectAndReturn( RCC_OSC_CSI, RCC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( &compConfig ) );

    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   Sbs_Set_IoCompensation() verifies the enable of the cell by read-back.
 *
 * \details The HW model clears EN1 after every store. Requests the automatic mode of the cell VDDIO.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR (EN1 does not read back as set).
 */
void Ut_Sbs_Set_IoCompensation_Automatic_EnableReadBackMismatch_ReturnsError( void )
{
    const sbs_IoCompConfig_t compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_AUTOMATIC, SBS_IO_COMP_MODE_DISABLED );

    UtSbs_Init_Default();

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_Enable1NotKept ) );
    Rcc_Set_OscActive_ExpectAndReturn( RCC_OSC_CSI, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( &compConfig ) );
}


/**
 * \brief   Sbs_Set_IoCompensation() verifies the software code by read-back.
 *
 * \details The HW model clears the NMOS code of the cell 1 after every store. Requests the manual mode of the cell
 *          VDDIO with the NMOS code 0xF.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, the code is not selected (CS1 stays as it was, set).
 */
void Ut_Sbs_Set_IoCompensation_Manual_CodeReadBackMismatch_ReturnsError( void )
{
    sbs_IoCompConfig_t compConfig = UtSbs_Get_CompConfig( SBS_IO_COMP_MODE_MANUAL, SBS_IO_COMP_MODE_DISABLED );

    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO ].NmosCode = UT_SBS_CODE_NMOS_HIGH;

    UtSbs_Init_Default();

    SBS->CCCSR = UT_SBS_CCCSR_EN1;
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_CodeNotKept ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_IoCompensation( &compConfig ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCCSR_EN1, SBS->CCCSR );
}


/**
 * \brief   Sbs_Get_IoCompensationState() reports the ready flags of the enabled cells.
 *
 * \details Reads the state with: no cell enabled (ready flags set), one cell enabled and ready, both enabled and
 *          one ready, both enabled and ready, one enabled but not ready. The RCC module is not expected to be used.
 *
 * \par Expected results
 * - SBS_FUNCTION_ACTIVE only when at least one cell is enabled and every enabled cell is ready; INACTIVE for the
 *   other cases (also for no enabled cell).
 * - NULL pointer: SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Get_IoCompensationState_ReportsReadyOfEnabledCells( void )
{
    sbs_FunctionState_t state = SBS_FUNCTION_INACTIVE;

    UtSbs_Init_Default();

    SBS->CCCSR = UT_SBS_CCCSR_RDY1 | UT_SBS_CCCSR_RDY2;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &state ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, state );

    SBS->CCCSR = UT_SBS_CCCSR_EN1 | UT_SBS_CCCSR_RDY1;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &state ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_ACTIVE, state );

    SBS->CCCSR = UT_SBS_CCCSR_EN2 | UT_SBS_CCCSR_RDY2;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &state ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_ACTIVE, state );

    SBS->CCCSR = UT_SBS_CCCSR_EN1 | UT_SBS_CCCSR_EN2 | UT_SBS_CCCSR_RDY1;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &state ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, state );

    SBS->CCCSR = UT_SBS_CCCSR_EN1 | UT_SBS_CCCSR_EN2 | UT_SBS_CCCSR_RDY1 | UT_SBS_CCCSR_RDY2;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &state ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_ACTIVE, state );

    SBS->CCCSR = UT_SBS_CCCSR_EN1;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &state ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, state );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_IoCompensationState( NULL ) );
}

/* =============================== BREAK SOURCES ============================ */

/**
 * \brief   Sbs_Set_BreakSource() connects every source to its own bit of SBS_CFGR2.
 *
 * \details For every source of the independent table (lockup bit 0, SRAM ECC bit 1, PVD bit 2, flash ECC bit 3): clears
 *          the registers, requests the source alone, then the sum of all sources.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, SBS_CFGR2 holds exactly the bit of the source (all bits of the sources for the sum).
 */
void Ut_Sbs_Set_BreakSource_EverySource_ConnectsItsBit( void )
{
    UtSbs_Init_Default();

    for( uint32_t itemIdx = 0u; UT_SBS_BREAK_ITEM_CNT > itemIdx; itemIdx++ )
    {
        TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Reset() );

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_BreakSource( utSbs_BreakLut[ itemIdx ].Source ) );
        TEST_ASSERT_EQUAL_HEX32( utSbs_BreakLut[ itemIdx ].Mask, SBS->CFGR2 );
    }

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Reset() );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_BreakSource( SBS_BREAK_ALL ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CFGR2_OWNED, SBS->CFGR2 );
}


/**
 * \brief   Sbs_Set_BreakSource() adds a source and keeps the connected one.
 *
 * \details The core lockup is connected, the other bits of SBS_CFGR2 are 1. Requests the lockup and the PVD, then
 *          the lockup, the PVD and the SRAM ECC.
 *
 * \par Expected results
 * - SBS_REQUEST_OK for both requests, the bits are added, the connected bit stays set, the reserved bits keep their
 *   value.
 */
void Ut_Sbs_Set_BreakSource_AddSource_KeepsConnected( void )
{
    UtSbs_Init_Default();

    SBS->CFGR2 = ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_BreakSource( SBS_BREAK_LOCKUP | SBS_BREAK_PVD ) );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL | UT_SBS_CFGR2_PVDL, SBS->CFGR2 );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_BreakSource( SBS_BREAK_LOCKUP | SBS_BREAK_PVD | SBS_BREAK_SRAM_ECC ) );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL | UT_SBS_CFGR2_PVDL | UT_SBS_CFGR2_SEL,
                             SBS->CFGR2 );
}


/**
 * \brief   Sbs_Set_BreakSource() refuses a request which would remove a connected source.
 *
 * \details The lockup and the PVD are connected (noise in the other bits). Requests the lockup only, the flash ECC
 *          only and no source.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for every request, SBS_CFGR2 is not written (keeps CLL and PVDL, no ECCL).
 */
void Ut_Sbs_Set_BreakSource_RemoveConnected_ReturnsErrorNothingWritten( void )
{
    UtSbs_Init_Default();

    SBS->CFGR2 = ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL | UT_SBS_CFGR2_PVDL;

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_BreakSource( SBS_BREAK_LOCKUP ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_BreakSource( SBS_BREAK_FLASH_ECC ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_BreakSource( SBS_BREAK_NONE ) );

    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL | UT_SBS_CFGR2_PVDL, SBS->CFGR2 );
}


/**
 * \brief   Sbs_Set_BreakSource() of the connected set writes nothing.
 *
 * \details The lockup is connected (noise in the other bits). The HW model is a tripwire: after it is armed, every
 *          store clears CLL. Requests the lockup (the connected set).
 *
 * \par Expected results
 * - SBS_REQUEST_OK, the register keeps its value (CLL is still set: no store was executed).
 */
void Ut_Sbs_Set_BreakSource_ConnectedSet_ReturnsOkNothingWritten( void )
{
    UtSbs_Init_Default();

    SBS->CFGR2 = ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL;
    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_LockupTripwire ) );
    utSbs_TripwireArmed = UT_SBS_FLAG_SET;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_BreakSource( SBS_BREAK_LOCKUP ) );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CFGR2_OWNED ) | UT_SBS_CFGR2_CLL, SBS->CFGR2 );
}


/**
 * \brief   Sbs_Set_BreakSource() with no source and nothing connected is accepted.
 *
 * \details Presets noise to SBS_CFGR2 (no source connected) and requests SBS_BREAK_NONE.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, SBS_CFGR2 is not written.
 */
void Ut_Sbs_Set_BreakSource_NoneWithNothingConnected_ReturnsOkNothingWritten( void )
{
    UtSbs_Init_Default();

    SBS->CFGR2 = ~UT_SBS_CFGR2_OWNED;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_BreakSource( SBS_BREAK_NONE ) );
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_CFGR2_OWNED, SBS->CFGR2 );
}


/**
 * \brief   Sbs_Set_BreakSource() refuses a bit outside of the set of the sources.
 *
 * \details Requests a bit above the sources, and the sum of all sources with an extra bit.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, SBS_CFGR2 is not written.
 */
void Ut_Sbs_Set_BreakSource_InvalidMask_ReturnsErrorNothingWritten( void )
{
    UtSbs_Init_Default();

    SBS->CFGR2 = ~UT_SBS_CFGR2_OWNED;

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_BreakSource( UT_SBS_BREAK_INVALID ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_BreakSource( SBS_BREAK_LOCKUP | UT_SBS_BREAK_INVALID ) );
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_CFGR2_OWNED, SBS->CFGR2 );
}


/**
 * \brief   Sbs_Set_BreakSource() reports a connection which does not stay.
 *
 * \details The HW model clears CLL after every store (nonsecure access to a secure register). Requests the lockup.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR (read-back mismatch).
 */
void Ut_Sbs_Set_BreakSource_ReadBackMismatch_ReturnsError( void )
{
    UtSbs_Init_Default();

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_LockupNotKept ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_BreakSource( SBS_BREAK_LOCKUP ) );
}


/**
 * \brief   Sbs_Get_BreakSource() decodes the connection bits.
 *
 * \details Presets every combination of the four bits (the other bits of the register are 1) and reads the set.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, the set holds exactly the sources of the set bits (bit 0 lockup, bit 1 SRAM ECC, bit 2 PVD,
 *   bit 3 flash ECC), the reserved bits are ignored.
 * - NULL pointer: SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Get_BreakSource_EveryCombination_DecodesBits( void )
{
    sbs_BreakSource_t source = 0u;

    UtSbs_Init_Default();

    for( uint32_t combination = 0u; UT_SBS_CFGR2_COMBINATION_CNT > combination; combination++ )
    {
        sbs_BreakSource_t expected = SBS_BREAK_NONE;

        for( uint32_t itemIdx = 0u; UT_SBS_BREAK_ITEM_CNT > itemIdx; itemIdx++ )
        {
            if( 0u != ( combination & utSbs_BreakLut[ itemIdx ].Mask ) )
            {
                expected |= utSbs_BreakLut[ itemIdx ].Source;
            }
            else
            {
                /* Source is not connected in this combination */
            }
        }

        SBS->CFGR2 = ( ~UT_SBS_CFGR2_OWNED ) | combination;
        source     = UT_SBS_OUTPUT_PATTERN;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_BreakSource( &source ) );
        TEST_ASSERT_EQUAL_HEX32( expected, source );
    }

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_BreakSource( NULL ) );
}

/* ============================ ETHERNET INTERFACE ========================== */

/**
 * \brief   Sbs_Set_EthPhyInterface() selects MII and RMII before the MAC is enabled.
 *
 * \details Initializes the module, presets noise, the RCC mock reports the MAC clock inactive. Selects RMII, then MII.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, ETH_SEL_PHY = 0b100 (bit 23) for RMII and 0b000 for MII (the reference manuals: 000 GMII or MII,
 *   100 RMII), the other bits of SBS_PMCR keep their value.
 * - The RCC module is asked for the state of the MAC clock once per call.
 * - Line without Ethernet: the test is ignored (see Ut_Sbs_EthPhyInterface_LineWithoutEthernet_ReturnsError).
 */
void Ut_Sbs_Set_EthPhyInterface_MacStopped_SelectsInterface( void )
{
#if defined(ETH)
    UtSbs_Init_Default();

    SBS->PMCR = ~UT_SBS_PMCR_ETH_SEL_PHY;

    UtSbs_Expect_MacState( RCC_FUNCTION_INACTIVE, RCC_REQUEST_OK );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_RMII ) );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_PMCR_ETH_SEL_PHY ) | UT_SBS_PMCR_ETH_SEL_RMII, SBS->PMCR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_MII ) );
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_PMCR_ETH_SEL_PHY, SBS->PMCR );

    TEST_ASSERT_EQUAL_UINT32( UT_SBS_MAC_REQUEST_CNT, utSbs_MacCallCnt );
#else
    TEST_IGNORE_MESSAGE( "MCU without Ethernet" );
#endif
}


/**
 * \brief   Sbs_Set_EthPhyInterface() refuses the change when the MAC is running.
 *
 * \details The RCC mock reports the MAC clock active, noise is preset to the registers.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written (also for an interface which is selected already).
 */
void Ut_Sbs_Set_EthPhyInterface_MacRunning_ReturnsErrorNothingWritten( void )
{
#if defined(ETH)
    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    UtSbs_Expect_MacState( RCC_FUNCTION_ACTIVE, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_RMII ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_MII ) );

    UtSbs_Check_Noise( 0u );
    TEST_ASSERT_EQUAL_UINT32( UT_SBS_MAC_REQUEST_CNT, utSbs_MacCallCnt );
#else
    TEST_IGNORE_MESSAGE( "MCU without Ethernet" );
#endif
}


/**
 * \brief   Sbs_Set_EthPhyInterface() refuses when the RCC module does not report the state of the MAC clock.
 *
 * \details The RCC mock returns an error for the state request, noise is preset to the registers.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written.
 */
void Ut_Sbs_Set_EthPhyInterface_RccError_ReturnsErrorNothingWritten( void )
{
#if defined(ETH)
    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    UtSbs_Expect_MacState( RCC_FUNCTION_INACTIVE, RCC_REQUEST_ERROR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_RMII ) );

    UtSbs_Check_Noise( 0u );
#else
    TEST_IGNORE_MESSAGE( "MCU without Ethernet" );
#endif
}


/**
 * \brief   Sbs_Set_EthPhyInterface() refuses an interface outside of the list before the RCC module is asked.
 *
 * \details Requests the count and the unused item of the list.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, no register is written, the RCC module is not used.
 */
void Ut_Sbs_Set_EthPhyInterface_InvalidInterface_ReturnsErrorNothingWritten( void )
{
#if defined(ETH)
    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_CNT ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_UNUSED ) );

    UtSbs_Check_Noise( 0u );
    TEST_ASSERT_EQUAL_UINT32( 0u, utSbs_MacCallCnt );
#else
    TEST_IGNORE_MESSAGE( "MCU without Ethernet" );
#endif
}


/**
 * \brief   Sbs_Set_EthPhyInterface() reports a selection which does not stay.
 *
 * \details The HW model clears ETH_SEL_PHY after every store. Requests RMII.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR (read-back mismatch).
 */
void Ut_Sbs_Set_EthPhyInterface_ReadBackMismatch_ReturnsError( void )
{
#if defined(ETH)
    UtSbs_Init_Default();

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_EthSelNotKept ) );
    UtSbs_Expect_MacState( RCC_FUNCTION_INACTIVE, RCC_REQUEST_OK );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_RMII ) );
#else
    (void)UtSbs_HwModel_EthSelNotKept;

    TEST_IGNORE_MESSAGE( "MCU without Ethernet" );
#endif
}


/**
 * \brief   Sbs_Get_EthPhyInterface() decodes ETH_SEL_PHY and has no side effects.
 *
 * \details Presets MII (000), RMII (100) and the reserved value 001 (RGMII of the reference manual), noise in the other
 *          bits. The RCC module is not expected to be used.
 *
 * \par Expected results
 * - SBS_ETH_PHY_MII, SBS_ETH_PHY_RMII (SBS_REQUEST_OK); the reserved value: SBS_REQUEST_ERROR, output not touched.
 * - NULL pointer: SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Get_EthPhyInterface_DecodesField( void )
{
#if defined(ETH)
    sbs_EthPhyInterface_t phyInterface = SBS_ETH_PHY_MII;

    UtSbs_Init_Default();

    SBS->PMCR = ~UT_SBS_PMCR_ETH_SEL_PHY;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_EthPhyInterface( &phyInterface ) );
    TEST_ASSERT_EQUAL( SBS_ETH_PHY_MII, phyInterface );

    SBS->PMCR = ( ~UT_SBS_PMCR_ETH_SEL_PHY ) | UT_SBS_PMCR_ETH_SEL_RMII;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_EthPhyInterface( &phyInterface ) );
    TEST_ASSERT_EQUAL( SBS_ETH_PHY_RMII, phyInterface );

    phyInterface = (sbs_EthPhyInterface_t)UT_SBS_OUTPUT_PATTERN;
    SBS->PMCR    = ( ~UT_SBS_PMCR_ETH_SEL_PHY ) | UT_SBS_PMCR_ETH_SEL_RESERVED;
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_EthPhyInterface( &phyInterface ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, phyInterface );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_EthPhyInterface( NULL ) );
#else
    TEST_IGNORE_MESSAGE( "MCU without Ethernet" );
#endif
}


/**
 * \brief   The Ethernet PHY interface functions of a line without Ethernet return an error and write nothing.
 *
 * \details Initializes the module, presets noise and a pattern to the output, calls the setter with the unused item and
 *          the getter (STM32H503, H523, H533, H562 have no Ethernet MAC).
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for both calls, no register is written, the output is not touched, the RCC module is not used.
 * - Line with Ethernet: the test is ignored.
 */
void Ut_Sbs_EthPhyInterface_LineWithoutEthernet_ReturnsError( void )
{
#if !defined(ETH)
    sbs_EthPhyInterface_t phyInterface = (sbs_EthPhyInterface_t)UT_SBS_OUTPUT_PATTERN;

    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_UNUSED ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_EthPhyInterface( &phyInterface ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, phyInterface );
    UtSbs_Check_Noise( 0u );
#else
    TEST_IGNORE_MESSAGE( "MCU with Ethernet" );
#endif
}

/* ============================== FPU INTERRUPTS ============================ */

/**
 * \brief   Sbs_Set_FpuInterruptMask() writes every FPU exception to its own bit of FPU_IE.
 *
 * \details For every exception (invalid operation bit 0, divide-by-zero 1, underflow 2, overflow 3, input denormal 4,
 *          inexact 5): requests the exception alone with the other bits of SBS_FPUIMR preset to 1, then all
 *          exceptions and none.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, FPU_IE holds exactly the requested bit, the bits outside of the field keep their value.
 */
void Ut_Sbs_Set_FpuInterruptMask_EveryException_WritesItsBit( void )
{
    UtSbs_Init_Default();

    for( uint32_t bitIdx = 0u; UT_SBS_FPU_EXCEPTION_CNT > bitIdx; bitIdx++ )
    {
        const uint32_t exceptionMask = UT_SBS_BIT( bitIdx );

        SBS->FPUIMR = UT_SBS_ALL_BITS;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FpuInterruptMask( exceptionMask ) );
        TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_FPUIMR_OWNED ) | exceptionMask, SBS->FPUIMR );
    }

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_ALL ) );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_FPUIMR_OWNED ) | UT_SBS_FPUIMR_OWNED, SBS->FPUIMR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_NONE ) );
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_FPUIMR_OWNED, SBS->FPUIMR );
}


/**
 * \brief   Sbs_Set_FpuInterruptMask() refuses a bit outside of the six exceptions before any write.
 *
 * \details Initializes the module, presets noise and requests bit 6 and a bit of the upper half word.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for both requests, no register is written.
 */
void Ut_Sbs_Set_FpuInterruptMask_InvalidMask_ReturnsErrorNothingWritten( void )
{
    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FpuInterruptMask( UT_SBS_FPU_INT_INVALID ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FpuInterruptMask( UT_SBS_FPU_INT_UPPER_HALF ) );

    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   Sbs_Set_FpuInterruptMask() reports a value which does not stay (access which the hardware ignores).
 *
 * \details The HW model clears FPU_IE[1] after every store. Requests all exceptions, then only the invalid operation.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for the first request, SBS_REQUEST_OK for the second one (the cleared bit is not requested).
 */
void Ut_Sbs_Set_FpuInterruptMask_ReadBackMismatch_ReturnsError( void )
{
    UtSbs_Init_Default();

    TEST_ASSERT_EQUAL( REGMEM_REQUEST_OK, RegMem_Set_ModelActive( UtSbs_HwModel_FpuBit1NotKept ) );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_ALL ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_INVALID_OPERATION ) );
}


/**
 * \brief   Sbs_Get_FpuInterruptMask() decodes FPU_IE and has no side effects.
 *
 * \details Presets FPU_IE = 0x2A with the other bits set to 1, reads the mask, then NULL.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, mask 0x2A (divide-by-zero, overflow, inexact), the bits outside of the field are ignored.
 * - NULL pointer: SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Get_FpuInterruptMask_DecodesField( void )
{
    sbs_FpuInterrupt_t mask = 0u;

    UtSbs_Init_Default();

    SBS->FPUIMR = ( ~UT_SBS_FPUIMR_OWNED ) | UT_SBS_FPUIMR_PATTERN;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FpuInterruptMask( &mask ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_FPU_INT_DIVIDE_BY_ZERO | SBS_FPU_INT_OVERFLOW | SBS_FPU_INT_INEXACT, mask );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_FPUIMR_PATTERN, mask );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FpuInterruptMask( NULL ) );
}

/* ================================ MEMORY REMAP ============================ */

/**
 * \brief   The memory remap functions have no meaning on STM32H5 and write nothing.
 *
 * \details Initializes the module, presets noise and a pattern to the output, calls the setter with the unused item and
 *          the getter.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for both calls, no register is written, the output is not touched, the RCC module is not used.
 */
void Ut_Sbs_MemoryRemap_Initialized_ReturnsErrorNothingWritten( void )
{
    sbs_MemoryRemap_t memoryRemap = (sbs_MemoryRemap_t)UT_SBS_OUTPUT_PATTERN;

    UtSbs_Init_Default();
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_MemoryRemap( SBS_MEMORY_REMAP_UNUSED ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_MemoryRemap( &memoryRemap ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_MemoryRemap( NULL ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, memoryRemap );
    UtSbs_Check_Noise( 0u );
}

/* ========================= BOOT AND SECURITY STATE ======================== */

/**
 * \brief   Sbs_Get_BootState() decodes the four codes of the temporal isolation level.
 *
 * \details For every code of the reference manual (0xB4 HDPL0, 0x51 HDPL1, 0x8A HDPL2, 0x6F HDPL3) presets SBS_HDPLSR
 *          (the upper bits are 1) and reads the boot state.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, the level of the code, the boot address is SBS_BOOT_ADDRESS_NONE (the SBS of STM32H5 holds none).
 * - The RCC module is not used.
 */
void Ut_Sbs_Get_BootState_EveryLevel_DecodesCode( void )
{
    sbs_BootState_t bootState;

    UtSbs_Init_Default();

    for( uint32_t itemIdx = 0u; UT_SBS_HDPL_ITEM_CNT > itemIdx; itemIdx++ )
    {
        SBS->HDPLSR           = UT_SBS_HDPLSR_UPPER_BITS | utSbs_HdplLut[ itemIdx ].Code;
        bootState.Hdpl        = (sbs_Hdpl_t)UT_SBS_OUTPUT_PATTERN;
        bootState.BootAddress = 0u;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_BootState( &bootState ) );
        TEST_ASSERT_EQUAL( utSbs_HdplLut[ itemIdx ].Level, bootState.Hdpl );
        TEST_ASSERT_EQUAL_HEX32( SBS_BOOT_ADDRESS_NONE, bootState.BootAddress );
    }
}


/**
 * \brief   Sbs_Get_BootState() reports a reserved code and refuses a NULL pointer.
 *
 * \details Presets SBS_HDPLSR = 0 (not one of the four codes) and a pattern to the output.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR, the output is not touched.
 * - NULL pointer: SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Get_BootState_ReservedCode_ReturnsErrorOutputUntouched( void )
{
    sbs_BootState_t bootState = { (sbs_Hdpl_t)UT_SBS_OUTPUT_PATTERN, UT_SBS_OUTPUT_PATTERN };

    UtSbs_Init_Default();

    SBS->HDPLSR = UT_SBS_HDPL_CODE_RESERVED;

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_BootState( &bootState ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, bootState.Hdpl );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, bootState.BootAddress );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_BootState( NULL ) );
}


/**
 * \brief   Sbs_Get_ProductState() has no meaning on STM32H5 and reads nothing.
 *
 * \details Initializes the module, presets a pattern to the output and calls the getter.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR (the product state is an option byte of the flash interface), the output is not touched.
 */
void Ut_Sbs_Get_ProductState_Initialized_ReturnsErrorOutputUntouched( void )
{
    sbs_ProductState_t productState = (sbs_ProductState_t)UT_SBS_OUTPUT_PATTERN;

    UtSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_ProductState( &productState ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, productState );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_ProductState( NULL ) );
}


/**
 * \brief   Sbs_Get_DebugState() decodes the debug configuration at the temporal isolation levels 0 and 1.
 *
 * \details For HDPL0 and HDPL1 presets SBS_DBGCR with the unlock code in AP_UNLOCK, DBG_UNLOCK and (where it exists)
 *          DBG_AUTH_SEC, SBS_DBGLOCKR = 0xB4 (writes of SBS_DBGCR allowed), then the lock (0xC3 reads as 0x6A) and
 *          the other values of the fields.
 *
 * \par Expected results
 * - SBS_REQUEST_OK, the mask holds: access port unlocked, debug unlocked, secure debug (not STM32H503) and
 *   configuration locked, exactly for the fields which hold 0xB4 (lock: the field differs from 0xB4).
 */
void Ut_Sbs_Get_DebugState_Hdpl0And1_DecodesFields( void )
{
    const uint32_t hdplCode[] = { UT_SBS_HDPL_CODE_0, UT_SBS_HDPL_CODE_1 };
    sbs_DebugState_t debugState = SBS_DEBUG_STATE_NONE;
    sbs_DebugState_t expected   = SBS_DEBUG_STATE_NONE;

    UtSbs_Init_Default();

    for( uint32_t levelIdx = 0u; ( sizeof( hdplCode ) / sizeof( uint32_t ) ) > levelIdx; levelIdx++ )
    {
        SBS->HDPLSR   = hdplCode[ levelIdx ];
        SBS->DBGCR    = UT_SBS_DBGCR_ALL_UNLOCKED;
        SBS->DBGLOCKR = UT_SBS_CODE_OPEN;

        expected = SBS_DEBUG_STATE_AP_UNLOCKED | SBS_DEBUG_STATE_DEBUG_UNLOCKED;
#if defined(SBS_DEBUG_STATE_SECURE_DEBUG)
        expected |= SBS_DEBUG_STATE_SECURE_DEBUG;
#endif

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DebugState( &debugState ) );
        TEST_ASSERT_EQUAL_HEX32( expected, debugState );

        SBS->DBGLOCKR = UT_SBS_CODE_LOCKED;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DebugState( &debugState ) );
        TEST_ASSERT_EQUAL_HEX32( expected | SBS_DEBUG_STATE_CONFIG_LOCKED, debugState );

        SBS->DBGCR    = 0u;
        SBS->DBGLOCKR = UT_SBS_CODE_OPEN;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DebugState( &debugState ) );
        TEST_ASSERT_EQUAL_HEX32( SBS_DEBUG_STATE_NONE, debugState );
    }
}


/**
 * \brief   Sbs_Get_DebugState() decodes every field separately.
 *
 * \details At HDPL1 presets only one field of SBS_DBGCR with the unlock code at a time (AP_UNLOCK, DBG_UNLOCK, and
 *          DBG_AUTH_SEC where it exists) and a field with a wrong code (0xB5).
 *
 * \par Expected results
 * - The mask holds exactly the bit of the field with the code 0xB4; the wrong code and DBG_AUTH_HDPL alone set nothing.
 */
void Ut_Sbs_Get_DebugState_EveryField_DecodesOnlyUnlockCode( void )
{
    sbs_DebugState_t debugState = SBS_DEBUG_STATE_NONE;

    UtSbs_Init_Default();

    SBS->HDPLSR   = UT_SBS_HDPL_CODE_1;
    SBS->DBGLOCKR = UT_SBS_CODE_OPEN;

    SBS->DBGCR = UT_SBS_DBGCR_AP_UNLOCK;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DebugState( &debugState ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_DEBUG_STATE_AP_UNLOCKED, debugState );

    SBS->DBGCR = UT_SBS_DBGCR_DBG_UNLOCK;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DebugState( &debugState ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_DEBUG_STATE_DEBUG_UNLOCKED, debugState );

    SBS->DBGCR = UT_SBS_DBGCR_AUTH_HDPL1;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DebugState( &debugState ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_DEBUG_STATE_NONE, debugState );

    SBS->DBGCR = UT_SBS_DBGCR_WRONG_CODES;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DebugState( &debugState ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_DEBUG_STATE_NONE, debugState );

#if defined(SBS_DEBUG_STATE_SECURE_DEBUG)
    SBS->DBGCR = UT_SBS_DBGCR_AUTH_SEC;
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DebugState( &debugState ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_DEBUG_STATE_SECURE_DEBUG, debugState );
#endif
}


/**
 * \brief   Sbs_Get_DebugState() refuses to read the registers which are read-as-zero.
 *
 * \details Presets SBS_DBGCR and SBS_DBGLOCKR to 0 (the value read at HDPL2 and HDPL3, a lock would read as locked),
 *          a reserved code in SBS_HDPLSR, and a pattern to the output.
 *
 * \par Expected results
 * - HDPL2 and HDPL3 and the reserved code: SBS_REQUEST_ERROR, the output is not touched.
 * - NULL pointer: SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Get_DebugState_Hdpl2And3_ReturnsErrorOutputUntouched( void )
{
    const uint32_t hdplCode[] = { UT_SBS_HDPL_CODE_2, UT_SBS_HDPL_CODE_3, UT_SBS_HDPL_CODE_RESERVED };
    sbs_DebugState_t debugState = UT_SBS_OUTPUT_PATTERN;

    UtSbs_Init_Default();

    SBS->DBGCR    = 0u;
    SBS->DBGLOCKR = 0u;

    for( uint32_t levelIdx = 0u; ( sizeof( hdplCode ) / sizeof( uint32_t ) ) > levelIdx; levelIdx++ )
    {
        SBS->HDPLSR = hdplCode[ levelIdx ];

        TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_DebugState( &debugState ) );
        TEST_ASSERT_EQUAL_HEX32( UT_SBS_OUTPUT_PATTERN, debugState );
    }

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_DebugState( NULL ) );
}


/**
 * \brief   Sbs_Get_EraseState() decodes the erase status bits and never writes them.
 *
 * \details Presets SBS_MESR with every combination of MCLR (bit 0) and IPMEE (bit 16), the other bits set to 1 (the
 *          write-1-to-clear bits would be cleared by a write of the module).
 *
 * \par Expected results
 * - SBS_REQUEST_OK, SBS_ERASE_STATE_SRAM for MCLR, SBS_ERASE_STATE_CACHE for IPMEE, the register keeps its value.
 * - NULL pointer: SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Get_EraseState_EveryCombination_DecodesBitsNothingWritten( void )
{
    sbs_EraseState_t eraseState = SBS_ERASE_STATE_NONE;

    UtSbs_Init_Default();

    for( uint32_t combination = 0u; UT_SBS_MESR_COMBINATION_CNT > combination; combination++ )
    {
        const uint32_t mclrSet    = ( ( combination >> UT_SBS_COMBINATION_MCLR_POS ) & UT_SBS_BIT( 0u ) );
        const uint32_t ipmeeSet   = ( ( combination >> UT_SBS_COMBINATION_IPMEE_POS ) & UT_SBS_BIT( 0u ) );
        const uint32_t regValue   = ( ~( UT_SBS_MESR_MCLR | UT_SBS_MESR_IPMEE ) ) |
                                    ( mclrSet  * UT_SBS_MESR_MCLR ) |
                                    ( ipmeeSet * UT_SBS_MESR_IPMEE );
        const sbs_EraseState_t expected = ( mclrSet  * SBS_ERASE_STATE_SRAM ) |
                                          ( ipmeeSet * SBS_ERASE_STATE_CACHE );

        SBS->MESR  = regValue;
        eraseState = UT_SBS_OUTPUT_PATTERN;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_EraseState( &eraseState ) );

        TEST_ASSERT_EQUAL_HEX32( expected, eraseState );
        TEST_ASSERT_EQUAL_HEX32( regValue, SBS->MESR );
    }

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_EraseState( NULL ) );
}

/* =========================== REGISTER COMPONENT =========================== */

/**
 * \brief   The register component refuses writes to the registers which are read only for the module.
 *
 * \details Presets SBS_HDPLSR, SBS_DBGCR, SBS_DBGLOCKR and SBS_MESR to a pattern and calls every write function of the
 *          component for them.
 *
 * \par Expected results
 * - The registers keep their value, the verified writes return SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Reg_ReadOnlyRegisters_WritesRefused( void )
{
    SBS->HDPLSR   = UT_SBS_REG_PATTERN_HDPLSR;
    SBS->DBGCR    = UT_SBS_REG_PATTERN_DBGCR;
    SBS->DBGLOCKR = UT_SBS_REG_PATTERN_DBGLOCKR;
    SBS->MESR     = UT_SBS_REG_PATTERN_MESR;

    Sbs_Set_RegBit( SBS_REG_HDPLSR, UT_SBS_ALL_BITS );
    Sbs_Reset_RegBit( SBS_REG_DBGCR, UT_SBS_ALL_BITS );
    Sbs_Set_RegVal( SBS_REG_DBGLOCKR, UT_SBS_ALL_BITS, 0u );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Reg_Set_Verified( SBS_REG_MESR, UT_SBS_ALL_BITS, 0u ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR,
                       Sbs_Reg_Set_BitState( SBS_REG_MESR, UT_SBS_REG_PATTERN_MESR, SBS_FUNCTION_INACTIVE ) );

    TEST_ASSERT_EQUAL_HEX32( UT_SBS_REG_PATTERN_HDPLSR, SBS->HDPLSR );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_REG_PATTERN_DBGCR, SBS->DBGCR );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_REG_PATTERN_DBGLOCKR, SBS->DBGLOCKR );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_REG_PATTERN_MESR, SBS->MESR );
}


/**
 * \brief   The register component handles an invalid register identifier safely.
 *
 * \details Calls the accessors with the count of the registers (no valid identifier), noise is preset to the registers.
 *
 * \par Expected results
 * - The readers return 0, the writers write nothing, the verified protocols and the wait return SBS_REQUEST_ERROR.
 */
void Ut_Sbs_Reg_InvalidRegisterId_ReturnsZeroOrErrorNothingWritten( void )
{
    const sbs_RegId_t invalidId = SBS_REG_CNT;

    UtSbs_Preset_Noise( 0u );

    Sbs_Set_RegBit( invalidId, UT_SBS_ALL_BITS );
    Sbs_Reset_RegBit( invalidId, UT_SBS_ALL_BITS );
    Sbs_Set_RegVal( invalidId, UT_SBS_ALL_BITS, UT_SBS_ALL_BITS );

    TEST_ASSERT_EQUAL_HEX32( 0u, Sbs_Get_RegBit( invalidId, UT_SBS_ALL_BITS ) );
    TEST_ASSERT_EQUAL_HEX32( 0u, Sbs_Get_RegVal( invalidId, UT_SBS_ALL_BITS ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Reg_Set_Verified( invalidId, UT_SBS_ALL_BITS, 0u ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Reg_Set_BitState( invalidId, UT_SBS_ALL_BITS, SBS_FUNCTION_ACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Reg_Wait_RegVal( invalidId, UT_SBS_ALL_BITS, 0u ) );

    UtSbs_Check_Noise( 0u );
}


/**
 * \brief   The verified write refuses a value outside of the mask and keeps the bits outside of the mask.
 *
 * \details Presets SBS_FPUIMR = 0xFFFFFF00, writes the field 0x0F with the value 0x05, then a value with a bit outside
 *          of the mask.
 *
 * \par Expected results
 * - First write: SBS_REQUEST_OK, SBS_FPUIMR = 0xFFFFFF05 (the bits of the mask changed, the others kept).
 * - Second write: SBS_REQUEST_ERROR, the register is not written.
 */
void Ut_Sbs_Reg_SetVerified_ValueOutsideMask_ReturnsErrorNothingWritten( void )
{
    SBS->FPUIMR = UT_SBS_VERIFIED_PRESET;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK,
                       Sbs_Reg_Set_Verified( SBS_REG_FPUIMR, UT_SBS_VERIFIED_MASK, UT_SBS_VERIFIED_VALUE ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_VERIFIED_RESULT, SBS->FPUIMR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR,
                       Sbs_Reg_Set_Verified( SBS_REG_FPUIMR, UT_SBS_VERIFIED_MASK, UT_SBS_VERIFIED_OUTSIDE ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_VERIFIED_RESULT, SBS->FPUIMR );
}


/**
 * \brief   The bit state protocol refuses a state outside of the enumeration and verifies the bits.
 *
 * \details Sets and clears bits of SBS_FPUIMR with the valid states, then requests a state outside of the enumeration.
 *
 * \par Expected results
 * - Valid states: SBS_REQUEST_OK and the bits change. Invalid state: SBS_REQUEST_ERROR, nothing is written.
 */
void Ut_Sbs_Reg_SetBitState_InvalidState_ReturnsErrorNothingWritten( void )
{
    SBS->FPUIMR = 0u;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Reg_Set_BitState( SBS_REG_FPUIMR, UT_SBS_FPU_IE12, SBS_FUNCTION_ACTIVE ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_FPU_IE12, SBS->FPUIMR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Reg_Set_BitState( SBS_REG_FPUIMR, UT_SBS_FPU_IE1, SBS_FUNCTION_INACTIVE ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_FPU_IE2, SBS->FPUIMR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR,
                       Sbs_Reg_Set_BitState( SBS_REG_FPUIMR,
                                             UT_SBS_FPU_IE0,
                                             (sbs_FunctionState_t)UT_SBS_INVALID_STATE ) );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_FPU_IE2, SBS->FPUIMR );
}


/**
 * \brief   The wait for a flag succeeds when the flag is set and ends by the timeout when it is not.
 *
 * \details Waits for the ready flag RDY1 of SBS_CCCSR with the flag set and cleared, and for the cleared flag.
 *
 * \par Expected results
 * - Flag set: SBS_REQUEST_OK. Flag never set: SBS_REQUEST_ERROR (the loop is bounded). Waiting for a cleared flag
 *   which is cleared: SBS_REQUEST_OK.
 */
void Ut_Sbs_Reg_WaitRegVal_FlagSetAndNotSet_ReturnsOkAndTimeoutError( void )
{
    SBS->CCCSR = UT_SBS_CCCSR_RDY1;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Reg_Wait_RegVal( SBS_REG_CCCSR, UT_SBS_CCCSR_RDY1, UT_SBS_CCCSR_RDY1 ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Reg_Wait_RegVal( SBS_REG_CCCSR, UT_SBS_CCCSR_RDY1, 0u ) );

    SBS->CCCSR = 0u;

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Reg_Wait_RegVal( SBS_REG_CCCSR, UT_SBS_CCCSR_RDY1, UT_SBS_CCCSR_RDY1 ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Reg_Wait_RegVal( SBS_REG_CCCSR, UT_SBS_CCCSR_RDY1, 0u ) );
}

/* ========================== LOCAL FUNCTIONS =============================== */

/** Expects the request of the clock of the SBS to the RCC module (the first step of Sbs_Init) */
static void UtSbs_Expect_Init( void )
{
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_SBS, RCC_REQUEST_OK );
}


/** Expects the clock request and the request of the CSI oscillator (a compensation cell in the automatic mode) */
static void UtSbs_Expect_InitCsi( void )
{
    Rcc_Set_PeriphActive_ExpectAndReturn( RCC_PERIPH_SBS, RCC_REQUEST_OK );
    Rcc_Set_OscActive_ExpectAndReturn( RCC_OSC_CSI, RCC_REQUEST_OK );
}


/** Initializes the module with the default configuration and checks the result */
static void UtSbs_Init_Default( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );

    UtSbs_Init_Config( &config );
}


/** Initializes the module with the given configuration (no automatic cell) and checks the result */
static void UtSbs_Init_Config( const sbs_Config_t * config )
{
    UtSbs_Expect_Init();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( config ) );
}


/** Presets the bits outside of the bits owned by the module (the noise), the owned bits are cleared. The connected
 *  break sources (owned bits of SBS_CFGR2) are given by the argument. */
static void UtSbs_Preset_Noise( uint32_t cfgr2Owned )
{
    SBS->PMCR   = ~UT_SBS_PMCR_OWNED;
    SBS->FPUIMR = ~UT_SBS_FPUIMR_OWNED;
    SBS->CCCSR  = ~UT_SBS_CCCSR_OWNED;
    SBS->CCSWCR = ~UT_SBS_CCSWCR_OWNED;
    SBS->CFGR2  = ( ~UT_SBS_CFGR2_OWNED ) | cfgr2Owned;
    SBS->MESR   = UT_SBS_ALL_BITS;
}


/** Checks that the registers equal the noise of UtSbs_Preset_Noise (nothing was written) */
static void UtSbs_Check_Noise( uint32_t cfgr2Owned )
{
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_PMCR_OWNED,   SBS->PMCR );
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_FPUIMR_OWNED, SBS->FPUIMR );
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_CCCSR_OWNED,  SBS->CCCSR );
    TEST_ASSERT_EQUAL_HEX32( ~UT_SBS_CCSWCR_OWNED, SBS->CCSWCR );
    TEST_ASSERT_EQUAL_HEX32( ( ~UT_SBS_CFGR2_OWNED ) | cfgr2Owned, SBS->CFGR2 );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_ALL_BITS,      SBS->MESR );
}


/** Checks that Sbs_Init() refuses the configuration without any register write and without any RCC call */
static void UtSbs_Check_InitRefused( const sbs_Config_t * config )
{
    UtSbs_Preset_Noise( 0u );

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Init( config ) );

    UtSbs_Check_Noise( 0u );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_NONE ) );
    UtSbs_Check_Noise( 0u );
}


/** Checks the reset state written by Sbs_Deinit() (and by the failed Sbs_Init()): the Fast-mode Plus bits cleared, the
 *  FPU mask 0x1F, both compensation cells off with the fixed code selected */
static void UtSbs_Check_ResetState( void )
{
    TEST_ASSERT_EQUAL_HEX32( 0u, SBS->PMCR & UT_SBS_FMPLUS_ALL );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_FPUIMR_RESET, SBS->FPUIMR & UT_SBS_FPUIMR_OWNED );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCCSR_CS1 | UT_SBS_CCCSR_CS2, SBS->CCCSR & UT_SBS_CCCSR_OWNED );
    TEST_ASSERT_EQUAL_HEX32( UT_SBS_CCSWCR_RESET, SBS->CCSWCR & UT_SBS_CCSWCR_OWNED );
}


/** Returns the configuration of the compensation cells with the given modes and the codes NMOS 0xA and PMOS 0x5 */
static sbs_IoCompConfig_t UtSbs_Get_CompConfig( sbs_IoCompMode_t mode1, sbs_IoCompMode_t mode2 )
{
    sbs_IoCompConfig_t compConfig;

    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO  ].Mode     = mode1;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO  ].NmosCode = UT_SBS_CODE_NMOS_PATTERN;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO  ].PmosCode = UT_SBS_CODE_PMOS_PATTERN;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].Mode     = mode2;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].NmosCode = UT_SBS_CODE_NMOS_PATTERN;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO2 ].PmosCode = UT_SBS_CODE_PMOS_PATTERN;

    return ( compConfig );
}

#if defined(ETH)
/** RCC stub: reports the state of the clock of the MAC set by the test and counts the calls */
static rcc_RequestState_t UtSbs_RccMacStateStub( rcc_PeriphId_t periphId,
                                                 rcc_FunctionState_t * const funcState,
                                                 int callCnt )
{
    (void)callCnt;

    TEST_ASSERT_EQUAL( RCC_PERIPH_ETH, periphId );

    *funcState = utSbs_MacState;
    utSbs_MacCallCnt++;

    return ( utSbs_MacRequest );
}


/** Prepares the answer of the RCC module for the state of the clock of the MAC */
static void UtSbs_Expect_MacState( rcc_FunctionState_t macState, rcc_RequestState_t reqState )
{
    utSbs_MacState   = macState;
    utSbs_MacRequest = reqState;

    Rcc_Get_PeriphState_Stub( UtSbs_RccMacStateStub );
}
#endif

/* ============================== HW MODELS ================================= */

/** HW model: the ready flag of a compensation cell follows its enable bit (RDY1 = EN1, RDY2 = EN2) */
static void UtSbs_HwModel_CellReady( void )
{
    const uint32_t cccsr = SBS->CCCSR;
    const uint32_t ready = ( ( cccsr & UT_SBS_CCCSR_EN1 ) << UT_SBS_RDY1_SHIFT ) |
                           ( ( cccsr & UT_SBS_CCCSR_EN2 ) << UT_SBS_RDY2_SHIFT );

    (void)__atomic_and_fetch( &SBS->CCCSR, ~( UT_SBS_CCCSR_RDY1 | UT_SBS_CCCSR_RDY2 ), __ATOMIC_SEQ_CST );
    (void)__atomic_or_fetch( &SBS->CCCSR, ready, __ATOMIC_SEQ_CST );
}


/** HW model: ready flags as above, and a violation is recorded when the code of a cell is selected (CS cleared)
 *  while the cell is not enabled (EN cleared) */
static void UtSbs_HwModel_CellOrder( void )
{
    const uint32_t cccsr    = SBS->CCCSR;
    const uint32_t csClear1 = ( ( ~cccsr & UT_SBS_CCCSR_CS1 ) >> UT_SBS_CS_TO_EN_SHIFT ) &
                              ( ~cccsr & UT_SBS_CCCSR_EN1 );
    const uint32_t csClear2 = ( ( ~cccsr & UT_SBS_CCCSR_CS2 ) >> UT_SBS_CS_TO_EN_SHIFT ) &
                              ( ~cccsr & UT_SBS_CCCSR_EN2 );

    utSbs_OrderViolation |= ( csClear1 | csClear2 );

    UtSbs_HwModel_CellReady();
}


/** HW model: a violation is recorded when the software code of the cell 1 is selected (CS1 set) while the code field
 *  of the cell 1 does not hold the expected manual code 0xC3 */
static void UtSbs_HwModel_ManualOrder( void )
{
    const uint32_t csSet    = SBS->CCCSR & UT_SBS_CCCSR_CS1;
    const uint32_t codeWrong = ( SBS->CCSWCR & UT_SBS_CCSWCR_CELL1_MASK ) ^ UT_SBS_CCSWCR_CELL1_MANUAL;

    if( ( 0u != csSet     ) &&
        ( 0u != codeWrong )    )
    {
        utSbs_OrderViolation = UT_SBS_FLAG_SET;
    }
    else
    {
        /* Code is selected after it is written, or not selected */
    }
}


/** HW model: the Fast-mode Plus bit of PB6 stays cleared (pin secure in the GPIO, nonsecure access) */
static void UtSbs_HwModel_Pb6NotKept( void )
{
    (void)__atomic_and_fetch( &SBS->PMCR, ~UT_SBS_PMCR_PB6_FMP, __ATOMIC_SEQ_CST );
}


/** HW model: FPU_IE[1] stays cleared (the write is ignored) */
static void UtSbs_HwModel_FpuBit1NotKept( void )
{
    (void)__atomic_and_fetch( &SBS->FPUIMR, ~UT_SBS_FPU_IE1, __ATOMIC_SEQ_CST );
}


/** HW model: the connection bit of the core lockup stays cleared (the write is ignored) */
static void UtSbs_HwModel_LockupNotKept( void )
{
    (void)__atomic_and_fetch( &SBS->CFGR2, ~UT_SBS_CFGR2_CLL, __ATOMIC_SEQ_CST );
}


/** HW model: after it is armed, every store clears the connection bit of the core lockup (a tripwire of a write) */
static void UtSbs_HwModel_LockupTripwire( void )
{
    if( 0u != utSbs_TripwireArmed )
    {
        (void)__atomic_and_fetch( &SBS->CFGR2, ~UT_SBS_CFGR2_CLL, __ATOMIC_SEQ_CST );
    }
    else
    {
        /* Not armed: the activation of the model changes nothing */
    }
}


/** HW model: the field ETH_SEL_PHY stays cleared (the write is ignored) */
static void UtSbs_HwModel_EthSelNotKept( void )
{
    (void)__atomic_and_fetch( &SBS->PMCR, ~UT_SBS_PMCR_ETH_SEL_PHY, __ATOMIC_SEQ_CST );
}


/** HW model: the enable bit of the compensation cell 1 stays cleared (the write is ignored) */
static void UtSbs_HwModel_Enable1NotKept( void )
{
    (void)__atomic_and_fetch( &SBS->CCCSR, ~UT_SBS_CCCSR_EN1, __ATOMIC_SEQ_CST );
}


/** HW model: the Fast-mode Plus bit of PB6 stays set (the clear is ignored) */
static void UtSbs_HwModel_Pb6Sticky( void )
{
    (void)__atomic_or_fetch( &SBS->PMCR, UT_SBS_PMCR_PB6_FMP, __ATOMIC_SEQ_CST );
}


/** HW model: the enable bit of the compensation cell 1 stays set (the clear is ignored) */
static void UtSbs_HwModel_Enable1Sticky( void )
{
    (void)__atomic_or_fetch( &SBS->CCCSR, UT_SBS_CCCSR_EN1, __ATOMIC_SEQ_CST );
}


/** HW model: the NMOS code of the cell 1 stays cleared (the write is ignored) */
static void UtSbs_HwModel_CodeNotKept( void )
{
    (void)__atomic_and_fetch( &SBS->CCSWCR, ~UT_SBS_CCSWCR_NMOS1_MASK, __ATOMIC_SEQ_CST );
}
