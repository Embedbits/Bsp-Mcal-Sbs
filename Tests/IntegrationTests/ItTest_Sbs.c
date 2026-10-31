/**
 * \author Mr.Nobody
 * \file ItTest_Sbs.c
 * \ingroup Sbs
 * \brief Integration tests of System Configuration, Boot and Security (SBS) module on target.
 *
 * Sbs module runs on the MCU with the real SBS. Tests verify behavior which cannot be verified by unit
 * tests (emulated registers): the write of the bits is accepted by the hardware (read-back of the module), the
 * compensation cell becomes ready, the break connection can not be removed before a reset, the state read from
 * the hardware (temporal isolation level, debug configuration, erase status) is consistent and the registers
 * return to their reset state after a system reset.
 *
 * Used resources:
 * - SBS (PMCR, FPUIMR, CCCSR, CCSWCR, CFGR2, MESR, HDPLSR, DBGCR, DBGLOCKR), clock enabled by the module
 * - RCC (CSI oscillator for the compensation cell, state of the Ethernet MAC clock), requested by the module
 * - no external wiring: the high drive of PB6 ... PB9 is enabled without a measurement of the pin
 * - supply of the I/Os (VDDIO) in the range of the compensation cell (2.7 V ... 3.6 V), board with 3.3 V
 *
 * \note The firmware of the tests runs with the TrustZone disabled (nonsecure alias of the SBS). A device with
 *       the TrustZone enabled needs the secure configuration of the pins and of the SBS first.
 * \note The behavior seen by a signal measurement (edges of an I2C line with the high drive, the PWM shutdown by
 *       the break connection of the core lockup or of the voltage detector) needs wiring and a measurement, it is not
 *       part of the automatic test cases.
 * \note The break connection of every test case is removed by the system reset between the test cases.
 */

/* ============================= INCLUDES =================================== */
#include "unity.h"                          /* Unity testing framework        */
#include "IntegrationTesting.h"             /* Integration testing on target  */
#include "Sbs_Port.h"                       /* Module under test              */
#include "Nvic_Port.h"                      /* System reset                   */
#include "Stm32.h"                          /* SBS registers (reset test)     */
/* ============================= TYPEDEFS =================================== */

/* ======================= FORWARD DECLARATIONS ============================= */

static void             ItSbs_Init_Default          ( void );

/* ========================= SYMBOLIC CONSTANTS ============================= */

/** Value of the FPU interrupt mask after the reset (SBS_FPUIMR.FPU_IE, all exceptions except the inexact one) */
#define IT_SBS_FPUIMR_RESET                 ( 0x0000001Fu )

/** Mask used to change every FPU interrupt bit away from the reset state */
#define IT_SBS_FPUIMR_FIELD                 ( SBS_FPUIMR_FPU_IE )

/** Manual codes of the compensation cell used by the test (NMOS, PMOS) */
#define IT_SBS_COMP_NMOS_CODE               ( 0x3u )
#define IT_SBS_COMP_PMOS_CODE               ( 0xCu )

/* ========================== LOCAL VARIABLES =============================== */

/* ============================= TEST SETUP ================================= */

void setUp( void )
{
    /* Every test case starts after a reset, the module initializes itself */
}


void tearDown( void )
{
    /* The bits which can be cleared are cleared (the break connection stays until the next reset) */
    (void)Sbs_Deinit();
}

/* =============================== TESTS ==================================== */

/*------------------------------- Initialization -----------------------------*/

/**
 * \brief   Sbs_Init() with the default configuration is accepted by the hardware and Sbs_Deinit() restores it.
 *
 * \details Initializes the module with the default configuration (every register written by the module is verified
 *          by a read-back), deinitializes it, and initializes it again.
 *
 * \par Expected results
 * - Sbs_Init() and Sbs_Deinit() return SBS_REQUEST_OK, a function which needs the initialization returns
 *   SBS_REQUEST_ERROR after the deinitialization.
 */
void It_Sbs_Init_DefaultConfig_AcceptedByHardware( void )
{
    sbs_FunctionState_t state = SBS_FUNCTION_INACTIVE;

    ItSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FmPlus( SBS_FMPLUS_PIN_PB6, &state ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, state );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Deinit() );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_FmPlus( SBS_FMPLUS_PIN_PB6, &state ) );

    ItSbs_Init_Default();
}

/*--------------------------------- Boot state -------------------------------*/

/**
 * \brief   The boot state read on the target holds a valid temporal isolation level.
 *
 * \details Reads the boot state after the initialization.
 *
 * \par Expected results
 * - Sbs_Get_BootState() returns SBS_REQUEST_OK (SBS_HDPLSR holds one of the four codes), the level is in the list
 *   (SBS_HDPL_0 ... SBS_HDPL_3), the boot address is SBS_BOOT_ADDRESS_NONE (the SBS of STM32H5 holds none).
 */
void It_Sbs_Get_BootState_OnTarget_ReportsLevelWithoutBootAddress( void )
{
    sbs_BootState_t bootState = { SBS_HDPL_CNT, 0u };

    ItSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_BootState( &bootState ) );
    TEST_ASSERT_LESS_THAN_UINT32( SBS_HDPL_CNT, (uint32_t)bootState.Hdpl );
    TEST_ASSERT_EQUAL_HEX32( SBS_BOOT_ADDRESS_NONE, bootState.BootAddress );
}


/**
 * \brief   The debug state read on the target follows the temporal isolation level.
 *
 * \details Reads the level and the debug state. The debug registers of the SBS can be read at the levels 0 and 1 only.
 *
 * \par Expected results
 * - Level 0 or 1: Sbs_Get_DebugState() returns SBS_REQUEST_OK and no bit outside of the defined bits is set.
 * - Level 2 or 3: Sbs_Get_DebugState() returns SBS_REQUEST_ERROR (the registers read as zero).
 */
void It_Sbs_Get_DebugState_OnTarget_FollowsTemporalIsolationLevel( void )
{
    sbs_BootState_t    bootState    = { SBS_HDPL_CNT, 0u };
    sbs_DebugState_t   debugState   = SBS_DEBUG_STATE_NONE;
    sbs_DebugState_t   definedBits  = ( SBS_DEBUG_STATE_AP_UNLOCKED |
                                        SBS_DEBUG_STATE_DEBUG_UNLOCKED |
                                        SBS_DEBUG_STATE_CONFIG_LOCKED );
    sbs_RequestState_t debugRequest = SBS_REQUEST_ERROR;

#if defined(SBS_DEBUG_STATE_SECURE_DEBUG)
    definedBits |= SBS_DEBUG_STATE_SECURE_DEBUG;
#endif

    ItSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_BootState( &bootState ) );

    debugRequest = Sbs_Get_DebugState( &debugState );

    if( SBS_HDPL_2 > bootState.Hdpl )
    {
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, debugRequest );
        TEST_ASSERT_BITS_LOW( ~definedBits, debugState );
    }
    else
    {
        TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, debugRequest );
    }
}


/**
 * \brief   The erase state can be read on the target.
 *
 * \details Reads the erase state. The value depends on the reset history (the bit MCLR is set after a power-on
 *          reset and stays set until it is cleared by software), only the access is checked.
 *
 * \par Expected results
 * - Sbs_Get_EraseState() returns SBS_REQUEST_OK, no bit outside of SBS_ERASE_STATE_SRAM and SBS_ERASE_STATE_CACHE
 *   is set.
 */
void It_Sbs_Get_EraseState_OnTarget_ReadsStatus( void )
{
    sbs_EraseState_t eraseState = SBS_ERASE_STATE_NONE;

    ItSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_EraseState( &eraseState ) );
    TEST_ASSERT_BITS_LOW( ~( SBS_ERASE_STATE_SRAM | SBS_ERASE_STATE_CACHE ), eraseState );
}

/*-------------------------------- I/O features ------------------------------*/

/**
 * \brief   The Fast-mode Plus bit of every pin of the device line can be set and cleared on the hardware.
 *
 * \details For every pin of the list: enables the high drive, reads the state, disables it and reads the state. The
 *          module verifies every write by a read-back, a bit which the device does not accept (pin secure in the
 *          GPIO) fails the test.
 *
 * \par Expected results
 * - Sbs_Set_FmPlus() returns SBS_REQUEST_OK for both states of every pin, Sbs_Get_FmPlus() returns the state which
 *   was set.
 */
void It_Sbs_Set_FmPlus_EveryPin_ReadsBackOnHardware( void )
{
    sbs_FunctionState_t state = SBS_FUNCTION_INACTIVE;

    ItSbs_Init_Default();

    for( uint32_t pinIdx = 0u; SBS_FMPLUS_PIN_CNT > pinIdx; pinIdx++ )
    {
        const sbs_FmPlusPin_t pin = (sbs_FmPlusPin_t)pinIdx;

        TEST_ASSERT_EQUAL_MESSAGE( SBS_REQUEST_OK,
                                   Sbs_Set_FmPlus( pin, SBS_FUNCTION_ACTIVE ),
                                   "high drive of a pin not accepted" );
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FmPlus( pin, &state ) );
        TEST_ASSERT_EQUAL( SBS_FUNCTION_ACTIVE, state );

        TEST_ASSERT_EQUAL_MESSAGE( SBS_REQUEST_OK,
                                   Sbs_Set_FmPlus( pin, SBS_FUNCTION_INACTIVE ),
                                   "release of the high drive of a pin not accepted" );
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FmPlus( pin, &state ) );
        TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, state );
    }
}


/**
 * \brief   The automatic mode of the compensation cell of VDDIO becomes ready on the hardware.
 *
 * \details Requests the automatic mode of the cell VDDIO (the module requests the CSI oscillator from the Rcc
 *          module, enables the cell, waits for the ready flag and selects the code of the cell), reads the state,
 *          then switches the cell off. The cell of VDDIO2 is not used (the supply VDDIO2 may be missing on the
 *          board).
 *
 * \par Expected results
 * - Sbs_Set_IoCompensation() returns SBS_REQUEST_OK (the cell became ready within the timeout of the module),
 *   Sbs_Get_IoCompensationState() returns SBS_FUNCTION_ACTIVE.
 * - After the disabled mode the state is SBS_FUNCTION_INACTIVE.
 */
void It_Sbs_Set_IoCompensation_Automatic_BecomesReady( void )
{
    sbs_IoCompConfig_t  compConfig;
    sbs_FunctionState_t readyState = SBS_FUNCTION_INACTIVE;
    sbs_Config_t        config;

    ItSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    compConfig = config.IoCompensation;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO ].Mode = SBS_IO_COMP_MODE_AUTOMATIC;

    TEST_ASSERT_EQUAL_MESSAGE( SBS_REQUEST_OK, Sbs_Set_IoCompensation( &compConfig ), "compensation cell not ready" );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &readyState ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_ACTIVE, readyState );

    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO ].Mode = SBS_IO_COMP_MODE_DISABLED;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_IoCompensation( &compConfig ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &readyState ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, readyState );
}


/**
 * \brief   The manual mode of the compensation cell applies the software code on the hardware.
 *
 * \details Requests the manual mode of the cell VDDIO with the NMOS code 0x3 and the PMOS code 0xC, reads
 *          SBS_CCSWCR and SBS_CCCSR directly.
 *
 * \par Expected results
 * - Sbs_Set_IoCompensation() returns SBS_REQUEST_OK, SBS_CCSWCR.SW_ANSRC1 = 0x3 and SW_APSRC1 = 0xC, the software
 *   code is selected (CS1 set) and the cell is off (EN1 cleared), Sbs_Get_IoCompensationState() returns
 *   SBS_FUNCTION_INACTIVE.
 */
void It_Sbs_Set_IoCompensation_Manual_AppliesSoftwareCode( void )
{
    sbs_IoCompConfig_t  compConfig;
    sbs_FunctionState_t readyState = SBS_FUNCTION_ACTIVE;
    sbs_Config_t        config;

    ItSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    compConfig = config.IoCompensation;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO ].Mode     = SBS_IO_COMP_MODE_MANUAL;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO ].NmosCode = IT_SBS_COMP_NMOS_CODE;
    compConfig.Cell[ SBS_IO_COMP_CELL_VDDIO ].PmosCode = IT_SBS_COMP_PMOS_CODE;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_IoCompensation( &compConfig ) );

    TEST_ASSERT_EQUAL_HEX32( IT_SBS_COMP_NMOS_CODE,
                             ( SBS->CCSWCR & SBS_CCSWCR_SW_ANSRC1 ) >> SBS_CCSWCR_SW_ANSRC1_Pos );
    TEST_ASSERT_EQUAL_HEX32( IT_SBS_COMP_PMOS_CODE,
                             ( SBS->CCSWCR & SBS_CCSWCR_SW_APSRC1 ) >> SBS_CCSWCR_SW_APSRC1_Pos );
    TEST_ASSERT_BITS_HIGH( SBS_CCCSR_CS1, SBS->CCCSR );
    TEST_ASSERT_BITS_LOW( SBS_CCCSR_EN1, SBS->CCCSR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_IoCompensationState( &readyState ) );
    TEST_ASSERT_EQUAL( SBS_FUNCTION_INACTIVE, readyState );
}

/*------------------------------- FPU interrupts -----------------------------*/

/**
 * \brief   The FPU interrupt mask is accepted by the hardware and reads back.
 *
 * \details Initializes the module (mask of the reset state), requests all exceptions, none, and the overflow with
 *          the inexact exception, reading the mask after every request. The code runs privileged (the register needs
 *          a privileged access).
 *
 * \par Expected results
 * - Sbs_Set_FpuInterruptMask() returns SBS_REQUEST_OK (the value reads back), Sbs_Get_FpuInterruptMask() returns the
 *   mask which was set. The default configuration has the mask of the reset state 0x1F.
 */
void It_Sbs_Set_FpuInterruptMask_OnTarget_ReadsBackOnHardware( void )
{
    sbs_FpuInterrupt_t mask = SBS_FPU_INT_NONE;

    ItSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FpuInterruptMask( &mask ) );
    TEST_ASSERT_EQUAL_HEX32( IT_SBS_FPUIMR_RESET, mask );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_ALL ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FpuInterruptMask( &mask ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_FPU_INT_ALL, mask );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_NONE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FpuInterruptMask( &mask ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_FPU_INT_NONE, mask );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_FpuInterruptMask( SBS_FPU_INT_OVERFLOW | SBS_FPU_INT_INEXACT ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_FpuInterruptMask( &mask ) );
    TEST_ASSERT_EQUAL_HEX32( SBS_FPU_INT_OVERFLOW | SBS_FPU_INT_INEXACT, mask );
}

/*------------------------------- Break sources ------------------------------*/

/**
 * \brief   The break connection of the core lockup can not be removed before a system reset.
 *
 * \details
 * 1. Stage 0: connects the core lockup, tries to remove it (request without the source), reads the connected set,
 *    requests system reset.
 * 2. Stage 1 (after reset): initializes the module and reads the connected set.
 *
 * \par Expected results
 * 1. Sbs_Set_BreakSource() returns SBS_REQUEST_OK for the lockup, SBS_REQUEST_ERROR for the request without a source,
 *    the connected set stays SBS_BREAK_LOCKUP. MCU is reset, code after the request is not executed.
 * 2. No source is connected after the reset.
 */
void It_Sbs_Set_BreakSource_Lockup_LockedUntilSystemReset( void )
{
    const integrationTesting_Stage_t stage       = IntegrationTesting_Get_Stage();
    sbs_BreakSource_t                breakSource = SBS_BREAK_NONE;

    ItSbs_Init_Default();

    if( 0u == stage )
    {
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_BreakSource( SBS_BREAK_LOCKUP ) );
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_BreakSource( &breakSource ) );
        TEST_ASSERT_EQUAL_HEX32( SBS_BREAK_LOCKUP, breakSource );

        TEST_ASSERT_EQUAL_MESSAGE( SBS_REQUEST_ERROR,
                                   Sbs_Set_BreakSource( SBS_BREAK_NONE ),
                                   "break connection removed without a reset" );
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_BreakSource( &breakSource ) );
        TEST_ASSERT_EQUAL_HEX32( SBS_BREAK_LOCKUP, breakSource );

        IntegrationTesting_Set_ResetExpected();

        Nvic_Set_SystemReset();

        TEST_FAIL_MESSAGE( "System reset did not occur" );
    }
    else
    {
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_BreakSource( &breakSource ) );
        TEST_ASSERT_EQUAL_HEX32( SBS_BREAK_NONE, breakSource );
    }
}

/*----------------------------- Ethernet interface ---------------------------*/

/**
 * \brief   The Ethernet PHY interface can be selected on the hardware while the MAC is stopped.
 *
 * \details Selects RMII and MII and reads the interface after every request (the clock of the MAC is not enabled by
 *          the test firmware). On a device line without Ethernet the test checks that the selection is refused.
 *
 * \par Expected results
 * - Line with Ethernet: Sbs_Set_EthPhyInterface() returns SBS_REQUEST_OK (the field reads back),
 *   Sbs_Get_EthPhyInterface() returns the interface which was selected.
 * - Line without Ethernet: SBS_REQUEST_ERROR for the selection of the unused item.
 */
void It_Sbs_Set_EthPhyInterface_OnTarget_ReadsBackOnHardware( void )
{
    ItSbs_Init_Default();

#if defined(SBS_ETH_PHY_AVAILABLE)
    {
        sbs_EthPhyInterface_t phyInterface = SBS_ETH_PHY_MII;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_RMII ) );
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_EthPhyInterface( &phyInterface ) );
        TEST_ASSERT_EQUAL( SBS_ETH_PHY_RMII, phyInterface );

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_MII ) );
        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_EthPhyInterface( &phyInterface ) );
        TEST_ASSERT_EQUAL( SBS_ETH_PHY_MII, phyInterface );
    }
#else
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_EthPhyInterface( SBS_ETH_PHY_UNUSED ) );
#endif
}

/*------------------------- Functions without a meaning ----------------------*/

/**
 * \brief   The functions without a meaning on STM32H5 return an error on the target.
 *
 * \details Calls the analog booster, the memory remap and the product state functions after the initialization.
 *
 * \par Expected results
 * - SBS_REQUEST_ERROR for every call (the booster is in the PWR module, STM32H5 has no memory remap, the product state
 *   is an option byte of the flash interface).
 */
void It_Sbs_FunctionsWithoutMeaning_OnTarget_ReturnError( void )
{
    sbs_FunctionState_t boosterState = SBS_FUNCTION_INACTIVE;
    sbs_MemoryRemap_t   memoryRemap  = SBS_MEMORY_REMAP_UNUSED;
    sbs_ProductState_t  productState = SBS_PRODUCT_STATE_OPEN;

    ItSbs_Init_Default();

    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_AnalogBooster( SBS_FUNCTION_ACTIVE ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_AnalogBooster( &boosterState ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Set_MemoryRemap( SBS_MEMORY_REMAP_UNUSED ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_MemoryRemap( &memoryRemap ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_ERROR, Sbs_Get_ProductState( &productState ) );
}

/*-------------------------------- Deinitialization --------------------------*/

/**
 * \brief   Sbs_Deinit() restores the reset state of the registers on the hardware.
 *
 * \details Enables the high drive of PB6, the FPU exceptions 0x3F and the manual code of the compensation cell VDDIO,
 *          deinitializes the module and reads the registers directly (the getters of the module need the
 *          initialization).
 *
 * \par Expected results
 * - Sbs_Deinit() returns SBS_REQUEST_OK, the Fast-mode Plus bits read 0, SBS_FPUIMR.FPU_IE reads 0x1F, the cell VDDIO
 *   is off (EN1 = 0) with the software code selected (CS1 = 1) and the code of the reset state 0x78.
 */
void It_Sbs_Deinit_AfterConfiguration_RestoresResetState( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    config.FmPlus[ SBS_FMPLUS_PIN_PB6 ]                           = SBS_FUNCTION_ACTIVE;
    config.FpuInterruptMask                                       = SBS_FPU_INT_ALL;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].Mode     = SBS_IO_COMP_MODE_MANUAL;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].NmosCode = IT_SBS_COMP_NMOS_CODE;
    config.IoCompensation.Cell[ SBS_IO_COMP_CELL_VDDIO ].PmosCode = IT_SBS_COMP_PMOS_CODE;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( &config ) );
    TEST_ASSERT_BITS_HIGH( SBS_PMCR_PB6_FMP, SBS->PMCR );

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Deinit() );

    TEST_ASSERT_BITS_LOW( SBS_PMCR_PB6_FMP, SBS->PMCR );
    TEST_ASSERT_EQUAL_HEX32( IT_SBS_FPUIMR_RESET, SBS->FPUIMR & IT_SBS_FPUIMR_FIELD );
    TEST_ASSERT_BITS_LOW( SBS_CCCSR_EN1, SBS->CCCSR );
    TEST_ASSERT_BITS_HIGH( SBS_CCCSR_CS1, SBS->CCCSR );
    TEST_ASSERT_EQUAL_HEX32( SBS_IO_COMP_CODE_NMOS_DEFAULT,
                             ( SBS->CCSWCR & SBS_CCSWCR_SW_ANSRC1 ) >> SBS_CCSWCR_SW_ANSRC1_Pos );
    TEST_ASSERT_EQUAL_HEX32( SBS_IO_COMP_CODE_PMOS_DEFAULT,
                             ( SBS->CCSWCR & SBS_CCSWCR_SW_APSRC1 ) >> SBS_CCSWCR_SW_APSRC1_Pos );
}

/*------------------------------- System reset -------------------------------*/

/**
 * \brief   The configuration registers return to the reset state after a system reset.
 *
 * \details
 * 1. Stage 0: initializes the module with the high drive of PB7 and the FPU mask 0x3F, requests system reset.
 * 2. Stage 1 (after reset): reads the registers directly (the module is not initialized after the reset).
 *
 * \par Expected results
 * 1. MCU is reset, code after the request is not executed.
 * 2. The Fast-mode Plus bit of PB7 is cleared and SBS_FPUIMR.FPU_IE is 0x1F (the registers are reset by a system
 *    reset).
 */
void It_Sbs_Registers_SystemReset_RestoreResetState( void )
{
    const integrationTesting_Stage_t stage = IntegrationTesting_Get_Stage();

    if( 0u == stage )
    {
        sbs_Config_t config;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
        config.FmPlus[ SBS_FMPLUS_PIN_PB7 ] = SBS_FUNCTION_ACTIVE;
        config.FpuInterruptMask             = SBS_FPU_INT_ALL;

        TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( &config ) );

        IntegrationTesting_Set_ResetExpected();

        Nvic_Set_SystemReset();

        TEST_FAIL_MESSAGE( "System reset did not occur" );
    }
    else
    {
        TEST_ASSERT_BITS_LOW( SBS_PMCR_PB7_FMP, SBS->PMCR );
        TEST_ASSERT_EQUAL_HEX32( IT_SBS_FPUIMR_RESET, SBS->FPUIMR & IT_SBS_FPUIMR_FIELD );
    }
}

/* ========================== LOCAL FUNCTIONS =============================== */

/**
 * \brief Initializes the module with the default configuration and checks the result.
 */
static void ItSbs_Init_Default( void )
{
    sbs_Config_t config;

    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Get_DefaultConfig( &config ) );
    TEST_ASSERT_EQUAL( SBS_REQUEST_OK, Sbs_Init( &config ) );
}
