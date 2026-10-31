/**
 * \author Mr.Nobody
 * \file Sbs.c
 * \ingroup Sbs
 * \brief Sbs module common functionality
 *
 * # Introduction #
 * The system configuration, boot and security block (SBS) of STM32H5 holds the settings which belong to
 * no single peripheral: the Fast-mode Plus high drive of the pins PB6 - PB9, the compensation cells of the
 * I/Os, the connection of the fault sources (core lockup, SRAM and flash double ECC error, PVD) to the break
 * inputs of the timers, the FPU interrupt mask, the selection of the Ethernet PHY interface and the read-only
 * state of the boot (temporal isolation level), the debug configuration and the erase of the memories. The
 * SBS is clocked by the AHB bus clock (enable bit SBSEN in the RCC, requested from the Rcc module), it has no
 * interrupt (the module uses no Nvic: the FPU interrupt line is enabled by the code which provides its handler,
 * the module only selects the exceptions which raise it).
 *
 * # Initialization #
 * Every function except \ref Sbs_Get_ModuleVersion, \ref Sbs_Get_DefaultConfig, \ref Sbs_Init, \ref Sbs_Deinit
 * and \ref Sbs_Task needs a successful \ref Sbs_Init. The getters have no side effects: they never enable the
 * clock and return an error when the module is not initialized. \ref Sbs_Init checks the whole configuration
 * before the first SBS register is written, writes the registers (the irreversible connection of the break
 * sources as the last one) and verifies every write by a read-back; on a mismatch the registers which can be
 * cleared are cleared again, the module is deinitialized and an error is returned.
 *
 * # Functions without a meaning on STM32H5 #
 * They keep their signature, write no register and return \ref SBS_REQUEST_ERROR (the list follows):
 * - Analog switch booster: it is controlled by PWR_PMCR (BOOSTE, AVD_READY), not by the SBS (Pwr module).
 * - Memory remap: STM32H5 has no memory remap, the boot memory is selected by the option bytes and BOOT0.
 * - Product state: it is the option byte PRODUCT_STATE of the flash interface, the SBS has no register with it.
 * - Ethernet PHY interface on the lines without Ethernet (STM32H503, STM32H523, STM32H533, STM32H562).
 *
 * # Break sources #
 * The connections are set by software and cleared only by a system reset (a write of 0 is ignored). A request
 * is the complete set of the connected sources: a request which does not contain a source connected earlier
 * would clear it and is refused (nothing is written), a request of the connected set writes nothing.
 *
 * # Register security #
 * The registers are accessed through the alias of the security state of the code (CMSIS macro SBS). A write
 * which the hardware ignores (nonsecure access to a secure bit, bit locked by a reset-only lock) is found by
 * the read-back and reported as an error.
 *
 */
/* ============================== INCLUDES ================================== */
#include "Sbs.h"                            /* Self include                   */
#include "Sbs_Port.h"                       /* Own port file include          */
#include "Sbs_Types.h"                      /* Module types definitions       */
#include "Sbs_Reg.h"                        /* Register access                */
#include "Rcc_Port.h"                       /* RCC handler functionality      */
#include "Stm32.h"                          /* SBS registers definition       */
/* ============================== TYPEDEFS ================================== */

/** \brief Runtime data of the module */
typedef struct
{
    sbs_FunctionState_t InitState; /**< The module is initialized (set by Sbs_Init) */
}   sbs_RuntimeData_t;

/* ======================== FORWARD DECLARATIONS ============================ */

static sbs_RequestState_t   Sbs_Check_Ready             ( void );
static sbs_RequestState_t   Sbs_Check_FunctionState     ( sbs_FunctionState_t state );
static sbs_RequestState_t   Sbs_Check_IoCompConfig      ( const sbs_IoCompConfig_t * const compConfig );
static sbs_RequestState_t   Sbs_Check_Config            ( const sbs_Config_t * const sbsConfig );
static sbs_RequestState_t   Sbs_Check_BreakRequest      ( sbs_BreakSource_t breakSource );

static sbs_RequestState_t   Sbs_Set_Config              ( const sbs_Config_t * const sbsConfig );
static sbs_RequestState_t   Sbs_Set_FmPlusTable         ( const sbs_FunctionState_t * const fmPlusTable );
static sbs_RequestState_t   Sbs_Set_IoCompConfig        ( const sbs_IoCompConfig_t * const compConfig );
static sbs_RequestState_t   Sbs_Set_IoCompCell          ( sbs_IoCompCell_t cellId,
                                                          const sbs_IoCompCellConfig_t * const cellConfig );
static sbs_RequestState_t   Sbs_Set_BreakConnection     ( sbs_BreakSource_t breakSource );
static sbs_RequestState_t   Sbs_Reset_Registers         ( void );

static sbs_BreakSource_t    Sbs_Get_BreakConnected      ( void );
static sbs_FunctionState_t  Sbs_Get_BitState            ( sbs_RegId_t regId, uint32_t bitMask );
static sbs_FunctionState_t  Sbs_Get_CodeState           ( sbs_RegId_t regId, uint32_t fieldMask, uint32_t fieldPos );
static sbs_RequestState_t   Sbs_Get_Hdpl                ( sbs_Hdpl_t * const hdpl );

/* ========================== SYMBOLIC CONSTANTS ============================ */

/** Value of major version of SW module */
#define SBS_MAJOR_VERSION               ( 0u )

/** Value of minor version of SW module */
#define SBS_MINOR_VERSION               ( 1u )

/** Value of patch version of SW module */
#define SBS_PATCH_VERSION               ( 0u )

/** Empty bit mask, no bit of the register */
#define SBS_BITS_NONE                   ( 0u )

/** Code of the fields of the debug registers which opens / unlocks (SBS_DBGCR: AP_UNLOCK, DBG_UNLOCK, DBG_AUTH_SEC,
 *  SBS_DBGLOCKR: DBGCFG_LOCK, the lock reads as 0x6A) */
#define SBS_CODE_OPEN                   ( 0xB4u )

/** Code of the temporal isolation level 0 in SBS_HDPLSR.HDPL */
#define SBS_HDPL_CODE_0                 ( 0xB4u )

/** Code of the temporal isolation level 1 in SBS_HDPLSR.HDPL */
#define SBS_HDPL_CODE_1                 ( 0x51u )

/** Code of the temporal isolation level 2 in SBS_HDPLSR.HDPL */
#define SBS_HDPL_CODE_2                 ( 0x8Au )

/** Code of the temporal isolation level 3 in SBS_HDPLSR.HDPL */
#define SBS_HDPL_CODE_3                 ( 0x6Fu )

/** Value of the Ethernet PHY interface MII ("GMII or MII") in SBS_PMCR.ETH_SEL_PHY */
#define SBS_ETH_SEL_PHY_MII             ( 0u )

#if defined(SBS_PMCR_PB9_FMP)
/** All Fast-mode Plus bits of SBS_PMCR of the device line */
#define SBS_FMPLUS_BITS_ALL             ( SBS_PMCR_PB6_FMP | \
                                          SBS_PMCR_PB7_FMP | \
                                          SBS_PMCR_PB8_FMP | \
                                          SBS_PMCR_PB9_FMP   )
#else
/** All Fast-mode Plus bits of SBS_PMCR of the device line */
#define SBS_FMPLUS_BITS_ALL             ( SBS_PMCR_PB6_FMP | \
                                          SBS_PMCR_PB7_FMP | \
                                          SBS_PMCR_PB8_FMP   )
#endif

/** All connection bits of SBS_CFGR2 */
#define SBS_BREAK_BITS_ALL              ( SBS_CFGR2_CLL  | \
                                          SBS_CFGR2_SEL  | \
                                          SBS_CFGR2_PVDL | \
                                          SBS_CFGR2_ECCL   )

/* =============================== MACROS =================================== */

/** Converts the mask of the FPU exceptions of the interface to the value of the FPU_IE field of SBS_FPUIMR */
#define SBS_FPU_TO_REG( fpuMask )       ( ( ( fpuMask ) << SBS_FPUIMR_FPU_IE_Pos ) & SBS_FPUIMR_FPU_IE )

/** Converts the value of the FPU_IE field of SBS_FPUIMR to the mask of the FPU exceptions of the interface */
#define SBS_REG_TO_FPU( regVal )        ( ( ( regVal ) & SBS_FPUIMR_FPU_IE ) >> SBS_FPUIMR_FPU_IE_Pos )

/* ========================== EXPORTED VARIABLES ============================ */

/* =========================== LOCAL VARIABLES ============================== */

/** \brief Fast-mode Plus bits of SBS_PMCR, indexed by \ref sbs_FmPlusPin_t */
static const uint32_t sbs_FmPlusBitLut[] =
{
    [ SBS_FMPLUS_PIN_PB6 ] = SBS_PMCR_PB6_FMP,  /**< PB6 */
    [ SBS_FMPLUS_PIN_PB7 ] = SBS_PMCR_PB7_FMP,  /**< PB7 */
    [ SBS_FMPLUS_PIN_PB8 ] = SBS_PMCR_PB8_FMP,  /**< PB8 */
#if defined(SBS_PMCR_PB9_FMP)
    [ SBS_FMPLUS_PIN_PB9 ] = SBS_PMCR_PB9_FMP,  /**< PB9 */
#endif
};

_Static_assert( SBS_FMPLUS_PIN_CNT == ( sizeof( sbs_FmPlusBitLut ) / sizeof( uint32_t ) ),
                "Sbs: sbs_FmPlusBitLut has incorrect size." );


/** \brief Connection bits of SBS_CFGR2 of the break sources */
static const sbs_BreakItem_t sbs_BreakLut[] =
{
    { .Source = SBS_BREAK_LOCKUP    , .BitMask = SBS_CFGR2_CLL  }, /**< Core lockup          */
    { .Source = SBS_BREAK_SRAM_ECC  , .BitMask = SBS_CFGR2_SEL  }, /**< SRAM double ECC error */
    { .Source = SBS_BREAK_FLASH_ECC , .BitMask = SBS_CFGR2_ECCL }, /**< Flash double ECC error */
    { .Source = SBS_BREAK_PVD       , .BitMask = SBS_CFGR2_PVDL }, /**< Voltage detector     */
};

#define SBS_BREAK_ITEM_CNT              ( sizeof( sbs_BreakLut ) / sizeof( sbs_BreakItem_t ) )


/** \brief Bits of the compensation cells, indexed by \ref sbs_IoCompCell_t */
static const sbs_IoCompItem_t sbs_IoCompLut[] =
{
    {
        .CellId      = SBS_IO_COMP_CELL_VDDIO,
        .EnableMask  = SBS_CCCSR_EN1,
        .CodeSelMask = SBS_CCCSR_CS1,
        .ReadyMask   = SBS_CCCSR_RDY1,
        .NmosMask    = SBS_CCSWCR_SW_ANSRC1,
        .NmosPos     = SBS_CCSWCR_SW_ANSRC1_Pos,
        .PmosMask    = SBS_CCSWCR_SW_APSRC1,
        .PmosPos     = SBS_CCSWCR_SW_APSRC1_Pos,
    },
    {
        .CellId      = SBS_IO_COMP_CELL_VDDIO2,
        .EnableMask  = SBS_CCCSR_EN2,
        .CodeSelMask = SBS_CCCSR_CS2,
        .ReadyMask   = SBS_CCCSR_RDY2,
        .NmosMask    = SBS_CCSWCR_SW_ANSRC2,
        .NmosPos     = SBS_CCSWCR_SW_ANSRC2_Pos,
        .PmosMask    = SBS_CCSWCR_SW_APSRC2,
        .PmosPos     = SBS_CCSWCR_SW_APSRC2_Pos,
    },
};

_Static_assert( SBS_IO_COMP_CELL_CNT == ( sizeof( sbs_IoCompLut ) / sizeof( sbs_IoCompItem_t ) ),
                "Sbs: sbs_IoCompLut has incorrect size." );


/** \brief Codes of SBS_HDPLSR.HDPL, indexed by \ref sbs_Hdpl_t */
static const uint32_t sbs_HdplCodeLut[] =
{
    [ SBS_HDPL_0 ] = SBS_HDPL_CODE_0,   /**< HDPL0 */
    [ SBS_HDPL_1 ] = SBS_HDPL_CODE_1,   /**< HDPL1 */
    [ SBS_HDPL_2 ] = SBS_HDPL_CODE_2,   /**< HDPL2 */
    [ SBS_HDPL_3 ] = SBS_HDPL_CODE_3,   /**< HDPL3 */
};

_Static_assert( SBS_HDPL_CNT == ( sizeof( sbs_HdplCodeLut ) / sizeof( uint32_t ) ),
                "Sbs: sbs_HdplCodeLut has incorrect size." );


#if defined(SBS_ETH_PHY_AVAILABLE)
/** \brief Values of SBS_PMCR.ETH_SEL_PHY, indexed by \ref sbs_EthPhyInterface_t */
static const uint32_t sbs_EthPhyRegLut[] =
{
    [ SBS_ETH_PHY_MII  ] = SBS_ETH_SEL_PHY_MII,         /**< MII (GMII or MII), ETH_SEL_PHY = 0 */
    [ SBS_ETH_PHY_RMII ] = SBS_PMCR_ETH_SEL_PHY_2,      /**< RMII, ETH_SEL_PHY = 4              */
};

_Static_assert( SBS_ETH_PHY_CNT == ( sizeof( sbs_EthPhyRegLut ) / sizeof( uint32_t ) ),
                "Sbs: sbs_EthPhyRegLut has incorrect size." );
#endif


/** \brief Runtime data of the module */
static volatile sbs_RuntimeData_t sbs_RuntimeData =
{
    .InitState = SBS_FUNCTION_INACTIVE,
};

/* ========================= EXPORTED FUNCTIONS ============================= */

/**
 * \brief Returns module SW version
 *
 * \return Module SW version
 */
sbs_ModuleVersion_t Sbs_Get_ModuleVersion( void )
{
    sbs_ModuleVersion_t retVersion;

    retVersion.Major = SBS_MAJOR_VERSION;
    retVersion.Minor = SBS_MINOR_VERSION;
    retVersion.Patch = SBS_PATCH_VERSION;

    return (retVersion);
}


/**
 * \brief Initializes the module and applies the configuration.
 *
 * The configuration is checked completely before the first SBS register is written. Then the clock of the SBS is
 * requested from the Rcc module and the state of the module is written: the Fast-mode Plus bits of all pins of the
 * device (an inactive entry clears the bit), the FPU interrupt mask, the compensation cells (the CSI oscillator is
 * requested from the Rcc module for the automatic mode) and, as the last step, the break sources. Every written
 * register is verified by a read-back.
 *
 * \note  The module owns the Fast-mode Plus bits and the FPU mask, the values set by other code are overwritten
 *        by the configuration. The break sources are connected until the next reset and can not be removed.
 * \note  Initialization is called once by the startup code of the application before the modules which use
 *        the setters of the module.
 *
 * \pre   Configuration shall be valid: not NULL, every state in its enumeration, the analog booster inactive,
 *        the modes of the compensation cells in their list and the manual codes in the range of the cell, the break
 *        sources and the FPU exceptions in their sets, the break sources connected earlier contained in the request;
 *        otherwise \ref SBS_REQUEST_ERROR is returned and no SBS register is written. The same holds when the Rcc
 *        module refuses the clock of the SBS. Failure after the first write (CSI oscillator refused by the Rcc module,
 *        compensation cell not ready, mismatch of a read-back) clears the Fast-mode Plus bits, the compensation cells
 *        and the FPU mask again (as \ref Sbs_Deinit), deinitializes the module and returns \ref SBS_REQUEST_ERROR.
 *
 * \param sbsConfig [in]: Pointer to SBS configuration, see \ref sbs_Config_t. Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Init( const sbs_Config_t * const sbsConfig )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

    if( SBS_NULL_PTR != sbsConfig )
    {
        /*------------------------ Configuration check -----------------------*/
        retState = Sbs_Check_Config( sbsConfig );

        /*-------------------------- Clock of the SBS ------------------------*/
        if( SBS_REQUEST_OK == retState )
        {
            const rcc_RequestState_t rccState = Rcc_Set_PeriphActive( RCC_PERIPH_SBS );

            if( RCC_REQUEST_OK == rccState )
            {
                retState = Sbs_Check_BreakRequest( sbsConfig->BreakSource );
            }
            else
            {
                /* Clock of the SBS is not available, nothing is written */
                retState = SBS_REQUEST_ERROR;
            }
        }
        else
        {
            /* Invalid configuration, nothing is written */
        }

        /*------------------------ Register configuration --------------------*/
        if( SBS_REQUEST_OK == retState )
        {
            retState = Sbs_Set_Config( sbsConfig );

            if( SBS_REQUEST_OK == retState )
            {
                sbs_RuntimeData.InitState = SBS_FUNCTION_ACTIVE;
            }
            else
            {
                /* Self-verification failed - reset state of the module */
                (void)Sbs_Reset_Registers();
                sbs_RuntimeData.InitState = SBS_FUNCTION_INACTIVE;
            }
        }
        else
        {
            /* Invalid configuration or clock not available, nothing is written */
        }
    }
    else
    {
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Deinitializes module Sbs
 *
 * Clears the Fast-mode Plus bits of all pins, switches the compensation cells off (the fixed code of the reset
 * state is applied) and restores the FPU interrupt mask of the reset state, every register is verified by a
 * read-back. The module is deinitialized even if the verification fails. A module which is not initialized
 * has written nothing: the function writes no register then. The clock of the SBS stays enabled (other
 * modules use the SBS registers).
 *
 * \note  The connected break sources can not be removed before the next reset, they are not touched.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Deinit( void )
{
    sbs_RequestState_t retState = SBS_REQUEST_OK;

    if( SBS_FUNCTION_ACTIVE == sbs_RuntimeData.InitState )
    {
        retState = Sbs_Reset_Registers();
    }
    else
    {
        /* Module is not initialized, nothing has been written */
        retState = SBS_REQUEST_OK;
    }

    sbs_RuntimeData.InitState = SBS_FUNCTION_INACTIVE;

    return ( retState );
}


/**
 * \brief Main task of module Sbs
 *
 * The module has nothing to process periodically, the function is kept for the
 * common interface of the modules.
 */
void Sbs_Task( void )
{
    return;
}


/**
 * \brief Fills configuration structure with default values.
 *
 * Default configuration: no Fast-mode Plus pin, analog booster inactive, both compensation cells disabled (fixed
 * code of the reset state), no break source, FPU interrupts of the reset state (all exceptions except the inexact one).
 *
 * \param sbsConfig [out]: Pointer to store default configuration. Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_DefaultConfig( sbs_Config_t * const sbsConfig )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

    if( SBS_NULL_PTR != sbsConfig )
    {
        for( uint32_t pinIdx = 0u; SBS_FMPLUS_PIN_CNT > pinIdx; pinIdx ++ )
        {
            sbsConfig->FmPlus[ pinIdx ] = SBS_FUNCTION_INACTIVE;
        }

        for( uint32_t cellIdx = 0u; SBS_IO_COMP_CELL_CNT > cellIdx; cellIdx ++ )
        {
            sbsConfig->IoCompensation.Cell[ cellIdx ].Mode     = SBS_IO_COMP_MODE_DISABLED;
            sbsConfig->IoCompensation.Cell[ cellIdx ].NmosCode = SBS_IO_COMP_CODE_NMOS_DEFAULT;
            sbsConfig->IoCompensation.Cell[ cellIdx ].PmosCode = SBS_IO_COMP_CODE_PMOS_DEFAULT;
        }

        sbsConfig->AnalogBooster   = SBS_FUNCTION_INACTIVE;
        sbsConfig->BreakSource     = SBS_BREAK_NONE;
        sbsConfig->FpuInterruptMask = SBS_FPU_INT_DEFAULT;

        retState = SBS_REQUEST_OK;
    }
    else
    {
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Enables or disables the Fast-mode Plus high drive of a pin.
 *
 * Writes the bit of the pin in SBS_PMCR and verifies it by a read-back (the write of the bit of a pin which is
 * secure in the GPIO is ignored for a nonsecure access, then the function returns an error). The bit enables the
 * high drive and bypasses the speed control of the pin, whether the pin is used by an I2C peripheral or as a
 * general purpose output.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param fmPlusPin [in]: Pin, see \ref sbs_FmPlusPin_t (only the pins of the device line)
 * \param state     [in]: \ref SBS_FUNCTION_ACTIVE enables the high drive
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Set_FmPlus( sbs_FmPlusPin_t fmPlusPin, sbs_FunctionState_t state )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();
    const sbs_RequestState_t stateCheck = Sbs_Check_FunctionState( state );

    if( ( SBS_REQUEST_OK   == readyState ) &&
        ( SBS_FMPLUS_PIN_CNT >  fmPlusPin  ) &&
        ( SBS_REQUEST_OK   == stateCheck )    )
    {
        retState = Sbs_Reg_Set_BitState( SBS_REG_PMCR, sbs_FmPlusBitLut[ fmPlusPin ], state );
    }
    else
    {
        /* Module not initialized or invalid parameter, nothing is written */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns the Fast-mode Plus high drive of a pin.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param fmPlusPin [in]:  Pin, see \ref sbs_FmPlusPin_t
 * \param state     [out]: Pointer to store the state, \ref SBS_FUNCTION_ACTIVE if the high drive is enabled.
 *                         Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_FmPlus( sbs_FmPlusPin_t fmPlusPin, sbs_FunctionState_t * const state )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK     == readyState ) &&
        ( SBS_FMPLUS_PIN_CNT >  fmPlusPin  ) &&
        ( SBS_NULL_PTR       != state      )    )
    {
        *state   = Sbs_Get_BitState( SBS_REG_PMCR, sbs_FmPlusBitLut[ fmPlusPin ] );
        retState = SBS_REQUEST_OK;
    }
    else
    {
        /* Module not initialized or invalid parameter */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Enables or disables the voltage booster of the analog switches (no meaning on STM32H5).
 *
 * The booster of the I/O analog switches of STM32H5 is controlled by PWR_PMCR (BOOSTE, AVD_READY) and not by the
 * SBS. The function writes no register and always returns \ref SBS_REQUEST_ERROR.
 *
 * \param state [in]: Not used
 *
 * \return Always \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Set_AnalogBooster( sbs_FunctionState_t state )
{
    (void)state;

    return ( SBS_REQUEST_ERROR );
}


/**
 * \brief Returns the voltage booster of the analog switches (no meaning on STM32H5).
 *
 * See \ref Sbs_Set_AnalogBooster. The function reads no register, does not touch the output and always returns
 * \ref SBS_REQUEST_ERROR.
 *
 * \param state [out]: Not used
 *
 * \return Always \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_AnalogBooster( sbs_FunctionState_t * const state )
{
    (void)state;

    return ( SBS_REQUEST_ERROR );
}


/**
 * \brief Configures the compensation cells of the I/Os.
 *
 * The configuration is checked before the first register is written. Both cells are configured in the order
 * of \ref sbs_IoCompCell_t, the first failure stops the request (the cells configured before keep the new
 * state). Sequence of a cell in \ref SBS_IO_COMP_MODE_AUTOMATIC - the cell is enabled, the ready flag is awaited
 * (bounded loop), then the code of the cell is selected. Sequence in \ref SBS_IO_COMP_MODE_MANUAL and
 * \ref SBS_IO_COMP_MODE_DISABLED - the software code is written (the fixed code of the reset state in the disabled
 * mode), then it is selected and the cell is disabled. Every register write is verified by a read-back.
 *
 * \note  The compensation cell works only with the CSI oscillator enabled: the module requests it from the Rcc
 *        module before the first register is written when a cell is in the automatic mode (the oscillator is not
 *        disabled by the module). The cell can be used only for the supply of the I/Os in the ranges
 *        2.7 V - 3.6 V and 1.62 V - 2 V. A cell whose supply is missing does not become ready: the function
 *        returns an error after the timeout and the cell stays enabled.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param compConfig [in]: Pointer to the configuration of the cells, see \ref sbs_IoCompConfig_t. Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Set_IoCompensation( const sbs_IoCompConfig_t * const compConfig )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState  ) &&
        ( SBS_NULL_PTR   != compConfig  )    )
    {
        retState = Sbs_Check_IoCompConfig( compConfig );

        if( SBS_REQUEST_OK == retState )
        {
            retState = Sbs_Set_IoCompConfig( compConfig );
        }
        else
        {
            /* Invalid configuration, nothing is written */
        }
    }
    else
    {
        /* Module not initialized or invalid parameter, nothing is written */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns the state of the compensation cells of the I/Os.
 *
 * The state is \ref SBS_FUNCTION_ACTIVE if at least one cell is enabled and every enabled cell reports its
 * ready flag, \ref SBS_FUNCTION_INACTIVE otherwise (also when no cell is enabled: the cells in the manual and the
 * disabled mode apply a software code and have no ready flag).
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param readyState [out]: Pointer to store the state. Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_IoCompensationState( sbs_FunctionState_t * const readyState )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t initState  = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == initState  ) &&
        ( SBS_NULL_PTR   != readyState )    )
    {
        uint32_t            enabledCnt = 0u;
        sbs_FunctionState_t allReady   = SBS_FUNCTION_ACTIVE;

        for( uint32_t cellIdx = 0u; SBS_IO_COMP_CELL_CNT > cellIdx; cellIdx ++ )
        {
            const sbs_FunctionState_t enableState = Sbs_Get_BitState( SBS_REG_CCCSR,
                                                                      sbs_IoCompLut[ cellIdx ].EnableMask );
            const sbs_FunctionState_t readyFlag   = Sbs_Get_BitState( SBS_REG_CCCSR,
                                                                      sbs_IoCompLut[ cellIdx ].ReadyMask );

            if( SBS_FUNCTION_ACTIVE == enableState )
            {
                enabledCnt ++;

                if( SBS_FUNCTION_ACTIVE != readyFlag )
                {
                    allReady = SBS_FUNCTION_INACTIVE;
                }
                else
                {
                    /* Enabled cell is ready */
                }
            }
            else
            {
                /* Cell is not enabled, it has no ready flag to wait for */
            }
        }

        if( ( 0u                  == enabledCnt ) ||
            ( SBS_FUNCTION_ACTIVE != allReady   )    )
        {
            *readyState = SBS_FUNCTION_INACTIVE;
        }
        else
        {
            *readyState = SBS_FUNCTION_ACTIVE;
        }

        retState = SBS_REQUEST_OK;
    }
    else
    {
        /* Module not initialized or invalid parameter */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Connects fault sources to the break inputs of the timers.
 *
 * The request is the complete set of the connected sources (bit mask of SBS_BREAK_xxx). The connection is set by
 * software and cleared only by a system reset: a request which does not contain a source connected earlier would
 * clear it, the request is refused and nothing is written. A request equal to the connected set writes nothing. The
 * written bits are verified by a read-back (the write of a nonsecure access to a secure register is ignored). The
 * break input of the timer is enabled by the Tim module. The connection of \ref SBS_BREAK_PVD makes the PVD
 * configuration of the PWR read only.
 *
 * \note  To add a source without knowing the connected set, read it by \ref Sbs_Get_BreakSource and add the new one.
 * \note  Reference manuals (RM0481, RM0517) name the connection of the flash ECC error bit \c ECCL "set and cleared
 *        by software" and the access type of the register "set only"; RM0492 gives the access type "read/write" for
 *        all bits. The module treats all sources as locked until the reset and never writes 0 to a connected bit.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param breakSource [in]: Set of the sources, bit mask of SBS_BREAK_xxx (SBS_BREAK_NONE means no source)
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Set_BreakSource( sbs_BreakSource_t breakSource )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState                 ) &&
        ( SBS_BITS_NONE  == ( breakSource & ~SBS_BREAK_ALL ) )    )
    {
        retState = Sbs_Check_BreakRequest( breakSource );

        if( SBS_REQUEST_OK == retState )
        {
            retState = Sbs_Set_BreakConnection( breakSource );
        }
        else
        {
            /* The request would remove a connected source, nothing is written */
        }
    }
    else
    {
        /* Module not initialized or invalid parameter, nothing is written */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns the fault sources connected to the break inputs of the timers.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param breakSource [out]: Pointer to store the connected set, bit mask of SBS_BREAK_xxx. Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_BreakSource( sbs_BreakSource_t * const breakSource )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState  ) &&
        ( SBS_NULL_PTR   != breakSource )    )
    {
        *breakSource = Sbs_Get_BreakConnected();
        retState     = SBS_REQUEST_OK;
    }
    else
    {
        /* Module not initialized or invalid parameter */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Selects the Ethernet PHY interface (MII or RMII).
 *
 * Writes SBS_PMCR.ETH_SEL_PHY and verifies it by a read-back. The interface shall be selected before the Ethernet
 * MAC is enabled: the function asks the Rcc module for the clock of the MAC and returns an error when it is running
 * (also for the interface which is selected already). On the lines without Ethernet (STM32H503, STM32H523,
 * STM32H533, STM32H562) the function writes no register and returns \ref SBS_REQUEST_ERROR.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param phyInterface [in]: PHY interface, see \ref sbs_EthPhyInterface_t
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Set_EthPhyInterface( sbs_EthPhyInterface_t phyInterface )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

#if defined(SBS_ETH_PHY_AVAILABLE)
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK  == readyState   ) &&
        ( SBS_ETH_PHY_CNT >  phyInterface )    )
    {
        rcc_FunctionState_t      macState = RCC_FUNCTION_ACTIVE;
        const rcc_RequestState_t rccState = Rcc_Get_PeriphState( RCC_PERIPH_ETH, &macState );

        if( ( RCC_REQUEST_OK       == rccState ) &&
            ( RCC_FUNCTION_INACTIVE == macState )    )
        {
            retState = Sbs_Reg_Set_Verified( SBS_REG_PMCR, SBS_PMCR_ETH_SEL_PHY, sbs_EthPhyRegLut[ phyInterface ] );
        }
        else
        {
            /* The MAC is running (or its state is not known), the interface can not be changed */
            retState = SBS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Module not initialized or invalid parameter, nothing is written */
        retState = SBS_REQUEST_ERROR;
    }
#else
    (void)phyInterface;

    /* Device line without Ethernet */
    retState = SBS_REQUEST_ERROR;
#endif

    return ( retState );
}


/**
 * \brief Returns the selected Ethernet PHY interface.
 *
 * A reserved value of SBS_PMCR.ETH_SEL_PHY (neither MII nor RMII) is reported as an error. On the lines without
 * Ethernet the function reads no register and returns \ref SBS_REQUEST_ERROR.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param phyInterface [out]: Pointer to store the PHY interface. Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_EthPhyInterface( sbs_EthPhyInterface_t * const phyInterface )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

#if defined(SBS_ETH_PHY_AVAILABLE)
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState    ) &&
        ( SBS_NULL_PTR   != phyInterface  )    )
    {
        const uint32_t selValue = Sbs_Get_RegVal( SBS_REG_PMCR, SBS_PMCR_ETH_SEL_PHY );

        for( uint32_t ifIdx = 0u; SBS_ETH_PHY_CNT > ifIdx; ifIdx ++ )
        {
            if( sbs_EthPhyRegLut[ ifIdx ] == selValue )
            {
                *phyInterface = (sbs_EthPhyInterface_t)ifIdx;
                retState      = SBS_REQUEST_OK;
                break;
            }
            else
            {
                /* Another interface of the list */
            }
        }
    }
    else
    {
        /* Module not initialized or invalid parameter */
        retState = SBS_REQUEST_ERROR;
    }
#else
    (void)phyInterface;

    /* Device line without Ethernet */
    retState = SBS_REQUEST_ERROR;
#endif

    return ( retState );
}


/**
 * \brief Selects the FPU exceptions which raise the FPU interrupt.
 *
 * Writes the FPU_IE field of SBS_FPUIMR and verifies it by a read-back (the register needs a privileged access
 * and is secure when SBS_SECCFGR.FPUSEC is set, an access which the hardware ignores is found by the read-back).
 * The module only selects the exceptions, the FPU interrupt line (FPU_IRQ) is enabled by the code which provides
 * its handler.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param fpuInterrupt [in]: Set of the enabled exceptions, bit mask of SBS_FPU_INT_xxx
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Set_FpuInterruptMask( sbs_FpuInterrupt_t fpuInterrupt )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState                         ) &&
        ( SBS_BITS_NONE  == ( fpuInterrupt & ~SBS_FPU_INT_ALL ) )    )
    {
        retState = Sbs_Reg_Set_Verified( SBS_REG_FPUIMR, SBS_FPUIMR_FPU_IE, SBS_FPU_TO_REG( fpuInterrupt ) );
    }
    else
    {
        /* Module not initialized or invalid parameter, nothing is written */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns the FPU exceptions which raise the FPU interrupt.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param fpuInterrupt [out]: Pointer to store the set of the enabled exceptions, bit mask of SBS_FPU_INT_xxx.
 *                            Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_FpuInterruptMask( sbs_FpuInterrupt_t * const fpuInterrupt )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState    ) &&
        ( SBS_NULL_PTR   != fpuInterrupt  )    )
    {
        const uint32_t regValue = Sbs_Get_RegVal( SBS_REG_FPUIMR, SBS_FPUIMR_FPU_IE );

        *fpuInterrupt = SBS_REG_TO_FPU( regValue );
        retState      = SBS_REQUEST_OK;
    }
    else
    {
        /* Module not initialized or invalid parameter */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Selects the memory at the boot address (no meaning on STM32H5).
 *
 * STM32H5 has no memory remap (the boot memory is selected by the option bytes and the BOOT0 pin). The function
 * writes no register and always returns \ref SBS_REQUEST_ERROR.
 *
 * \param memoryRemap [in]: Not used
 *
 * \return Always \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Set_MemoryRemap( sbs_MemoryRemap_t memoryRemap )
{
    (void)memoryRemap;

    return ( SBS_REQUEST_ERROR );
}


/**
 * \brief Returns the memory at the boot address (no meaning on STM32H5).
 *
 * See \ref Sbs_Set_MemoryRemap. The function reads no register, does not touch the output and always returns
 * \ref SBS_REQUEST_ERROR.
 *
 * \param memoryRemap [out]: Not used
 *
 * \return Always \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_MemoryRemap( sbs_MemoryRemap_t * const memoryRemap )
{
    (void)memoryRemap;

    return ( SBS_REQUEST_ERROR );
}


/**
 * \brief Returns the boot state: the temporal isolation level and the boot address.
 *
 * The level is read from SBS_HDPLSR (no access restriction). A value of the register which is not one of the four
 * documented codes is reported as an error. The SBS holds no boot address on STM32H5 (the addresses are option
 * bytes of the flash interface), \ref sbs_BootState_t::BootAddress is \ref SBS_BOOT_ADDRESS_NONE.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param bootState [out]: Pointer to store the boot state. Must not be NULL, not touched on an error.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_BootState( sbs_BootState_t * const bootState )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState ) &&
        ( SBS_NULL_PTR   != bootState  )    )
    {
        sbs_Hdpl_t hdpl = SBS_HDPL_0;

        retState = Sbs_Get_Hdpl( &hdpl );

        if( SBS_REQUEST_OK == retState )
        {
            bootState->Hdpl        = hdpl;
            bootState->BootAddress = SBS_BOOT_ADDRESS_NONE;
        }
        else
        {
            /* Reserved value of the temporal isolation level */
        }
    }
    else
    {
        /* Module not initialized or invalid parameter */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns the product (life cycle) state (no meaning on STM32H5).
 *
 * The product state is the option byte PRODUCT_STATE of the flash interface, the SBS has no register with it.
 * The function reads no register, does not touch the output and always returns \ref SBS_REQUEST_ERROR.
 *
 * \param productState [out]: Not used
 *
 * \return Always \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_ProductState( sbs_ProductState_t * const productState )
{
    (void)productState;

    return ( SBS_REQUEST_ERROR );
}


/**
 * \brief Returns the debug configuration of the SBS.
 *
 * The state is a bit mask of SBS_DEBUG_STATE_xxx built from SBS_DBGCR (the fields hold the unlock code: access port
 * unlocked, debug unlocked at the temporal isolation level of DBG_AUTH_HDPL, secure debug) and SBS_DBGLOCKR (the
 * configuration is locked until the reset). It reports what is programmed in the SBS, not whether a debugger is
 * connected and not the effective opening, which depends on the product state and the temporal isolation level.
 *
 * \note  SBS_DBGCR and SBS_DBGLOCKR are read-as-zero for the temporal isolation levels 2 and 3 (a lock would read as
 *        locked): the function returns an error then and does not touch the output.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param debugState [out]: Pointer to store the debug state. Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_DebugState( sbs_DebugState_t * const debugState )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState ) &&
        ( SBS_NULL_PTR   != debugState )    )
    {
        sbs_Hdpl_t               hdpl      = SBS_HDPL_0;
        const sbs_RequestState_t hdplState = Sbs_Get_Hdpl( &hdpl );

        if( ( SBS_REQUEST_OK == hdplState ) &&
            ( SBS_HDPL_2     >  hdpl      )    )
        {
            sbs_DebugState_t          stateMask = SBS_DEBUG_STATE_NONE;
            const sbs_FunctionState_t apState   = Sbs_Get_CodeState( SBS_REG_DBGCR,
                                                                     SBS_DBGCR_AP_UNLOCK,
                                                                     SBS_DBGCR_AP_UNLOCK_Pos );
            const sbs_FunctionState_t dbgState  = Sbs_Get_CodeState( SBS_REG_DBGCR,
                                                                     SBS_DBGCR_DBG_UNLOCK,
                                                                     SBS_DBGCR_DBG_UNLOCK_Pos );
            const sbs_FunctionState_t lockState = Sbs_Get_CodeState( SBS_REG_DBGLOCKR,
                                                                     SBS_DBGLOCKR_DBGCFG_LOCK,
                                                                     SBS_DBGLOCKR_DBGCFG_LOCK_Pos );

            if( SBS_FUNCTION_ACTIVE == apState )
            {
                stateMask |= SBS_DEBUG_STATE_AP_UNLOCKED;
            }
            else
            {
                /* Access port is not unlocked by the configuration */
            }

            if( SBS_FUNCTION_ACTIVE == dbgState )
            {
                stateMask |= SBS_DEBUG_STATE_DEBUG_UNLOCKED;
            }
            else
            {
                /* Debug is not unlocked by the configuration */
            }

#if defined(SBS_DBGCR_DBG_AUTH_SEC)
            {
                const sbs_FunctionState_t secState = Sbs_Get_CodeState( SBS_REG_DBGCR,
                                                                        SBS_DBGCR_DBG_AUTH_SEC,
                                                                        SBS_DBGCR_DBG_AUTH_SEC_Pos );

                if( SBS_FUNCTION_ACTIVE == secState )
                {
                    stateMask |= SBS_DEBUG_STATE_SECURE_DEBUG;
                }
                else
                {
                    /* Secure debug is not opened by the configuration */
                }
            }
#endif

            if( SBS_FUNCTION_ACTIVE != lockState )
            {
                stateMask |= SBS_DEBUG_STATE_CONFIG_LOCKED;
            }
            else
            {
                /* Debug configuration is not locked */
            }

            *debugState = stateMask;
            retState    = SBS_REQUEST_OK;
        }
        else
        {
            /* Registers are read-as-zero at this temporal isolation level */
            retState = SBS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Module not initialized or invalid parameter */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Returns the erase state of the memories.
 *
 * The state is a bit mask of SBS_ERASE_STATE_xxx built from SBS_MESR: MCLR (the device memories SRAM2, BKPSRAM and
 * the caches are erased after a power-on reset, a tamper detection or a product state regression, the bit survives a
 * system reset) and IPMEE (ICACHE and PKA RAM erased after a tamper detection or a product state regression). Both
 * bits are cleared by software by writing 1, which the module never does: a bit which was cleared by the application
 * reads as 0, which is the same as an erase in progress. The register is secure when SBS_SECCFGR.SBSSEC is set.
 *
 * \pre   The module shall be initialized by \ref Sbs_Init.
 *
 * \param eraseState [out]: Pointer to store the erase state. Must not be NULL.
 *
 * \return Function processing state. Returns \ref SBS_REQUEST_OK if request
 *         was processed without problems. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Get_EraseState( sbs_EraseState_t * const eraseState )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t readyState = Sbs_Check_Ready();

    if( ( SBS_REQUEST_OK == readyState ) &&
        ( SBS_NULL_PTR   != eraseState )    )
    {
        sbs_EraseState_t          stateMask  = SBS_ERASE_STATE_NONE;
        const sbs_FunctionState_t memState   = Sbs_Get_BitState( SBS_REG_MESR, SBS_MESR_MCLR );
        const sbs_FunctionState_t cacheState = Sbs_Get_BitState( SBS_REG_MESR, SBS_MESR_IPMEE );

        if( SBS_FUNCTION_ACTIVE == memState )
        {
            stateMask |= SBS_ERASE_STATE_SRAM;
        }
        else
        {
            /* Memory erase is on going or the status was cleared by software */
        }

        if( SBS_FUNCTION_ACTIVE == cacheState )
        {
            stateMask |= SBS_ERASE_STATE_CACHE;
        }
        else
        {
            /* Cache erase is on going, was not requested, or the status was cleared by software */
        }

        *eraseState = stateMask;
        retState    = SBS_REQUEST_OK;
    }
    else
    {
        /* Module not initialized or invalid parameter */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}

/* =========================== LOCAL FUNCTIONS ============================== */

/**
 * \brief Checks that the module was initialized.
 *
 * \return Returns \ref SBS_REQUEST_OK if \ref Sbs_Init succeeded (and \ref Sbs_Deinit was not
 *         called after it). Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Check_Ready( void )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

    if( SBS_FUNCTION_ACTIVE == sbs_RuntimeData.InitState )
    {
        retState = SBS_REQUEST_OK;
    }
    else
    {
        /* Module is not initialized */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks that the function state is a member of its enumeration.
 *
 * \param state [in]: Function state to check
 *
 * \return Returns \ref SBS_REQUEST_OK for \ref SBS_FUNCTION_INACTIVE and \ref SBS_FUNCTION_ACTIVE.
 *         Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Check_FunctionState( sbs_FunctionState_t state )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

    if( ( SBS_FUNCTION_INACTIVE == state ) ||
        ( SBS_FUNCTION_ACTIVE   == state )    )
    {
        retState = SBS_REQUEST_OK;
    }
    else
    {
        /* Value is not a member of the enumeration */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks the configuration of the compensation cells, no register is accessed.
 *
 * \param compConfig [in]: Pointer to the configuration of the cells, must not be NULL
 *
 * \return Returns \ref SBS_REQUEST_OK if every mode is in its list and the codes of the manual mode are in the
 *         range of the cell. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Check_IoCompConfig( const sbs_IoCompConfig_t * const compConfig )
{
    sbs_RequestState_t retState = SBS_REQUEST_OK;

    for( uint32_t cellIdx = 0u; SBS_IO_COMP_CELL_CNT > cellIdx; cellIdx ++ )
    {
        const sbs_IoCompCellConfig_t * const cellConfig = &compConfig->Cell[ cellIdx ];

        if( SBS_IO_COMP_MODE_CNT > cellConfig->Mode )
        {
            if( SBS_IO_COMP_MODE_MANUAL == cellConfig->Mode )
            {
                if( ( SBS_IO_COMP_CODE_MAX < cellConfig->NmosCode ) ||
                    ( SBS_IO_COMP_CODE_MAX < cellConfig->PmosCode )    )
                {
                    retState = SBS_REQUEST_ERROR;
                }
                else
                {
                    /* Codes are in the range of the cell */
                }
            }
            else
            {
                /* The codes of the other modes are not used */
            }
        }
        else
        {
            /* Mode is not in the list */
            retState = SBS_REQUEST_ERROR;
        }

        if( SBS_REQUEST_OK != retState )
        {
            break;
        }
        else
        {
            /* Cell is valid, check the next one */
        }
    }

    return ( retState );
}


/**
 * \brief Checks the configuration, no register is accessed.
 *
 * \param sbsConfig [in]: Pointer to SBS configuration, must not be NULL
 *
 * \return Returns \ref SBS_REQUEST_OK for a valid configuration (see \ref Sbs_Init), the connected break
 *         sources are not checked here. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Check_Config( const sbs_Config_t * const sbsConfig )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    sbs_RequestState_t       fmPlusState = SBS_REQUEST_OK;
    const sbs_RequestState_t compState  = Sbs_Check_IoCompConfig( &sbsConfig->IoCompensation );

    for( uint32_t pinIdx = 0u; SBS_FMPLUS_PIN_CNT > pinIdx; pinIdx ++ )
    {
        const sbs_RequestState_t itemState = Sbs_Check_FunctionState( sbsConfig->FmPlus[ pinIdx ] );

        if( SBS_REQUEST_OK != itemState )
        {
            fmPlusState = SBS_REQUEST_ERROR;
            break;
        }
        else
        {
            /* Item of the Fast-mode Plus table is valid */
        }
    }

    if( ( SBS_REQUEST_OK        == fmPlusState                                 ) &&
        ( SBS_REQUEST_OK        == compState                                   ) &&
        ( SBS_FUNCTION_INACTIVE == sbsConfig->AnalogBooster                    ) &&
        ( SBS_BITS_NONE         == ( sbsConfig->BreakSource & ~SBS_BREAK_ALL ) ) &&
        ( SBS_BITS_NONE         == ( sbsConfig->FpuInterruptMask & ~SBS_FPU_INT_ALL ) ) )
    {
        retState = SBS_REQUEST_OK;
    }
    else
    {
        /* A member is out of its list, or the analog booster (no meaning in the SBS) is requested */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Checks that a request of the break sources does not remove a connected source.
 *
 * \param breakSource [in]: Requested set of the break sources (valid mask)
 *
 * \return Returns \ref SBS_REQUEST_OK if every connected source is contained in the request.
 *         Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Check_BreakRequest( sbs_BreakSource_t breakSource )
{
    sbs_RequestState_t      retState  = SBS_REQUEST_ERROR;
    const sbs_BreakSource_t connected = Sbs_Get_BreakConnected();

    if( SBS_BITS_NONE == ( connected & ~breakSource ) )
    {
        retState = SBS_REQUEST_OK;
    }
    else
    {
        /* A connected source is missing in the request: it can not be removed before the reset */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Writes the configuration to the registers.
 *
 * Order: Fast-mode Plus bits, FPU interrupt mask, compensation cells, break sources (irreversible, the last one).
 *
 * \param sbsConfig [in]: Pointer to SBS configuration, checked
 *
 * \return Returns \ref SBS_REQUEST_OK if all registers read back as written.
 *         Otherwise returns \ref SBS_REQUEST_ERROR, the first failure stops the sequence.
 */
static sbs_RequestState_t Sbs_Set_Config( const sbs_Config_t * const sbsConfig )
{
    sbs_RequestState_t retState = Sbs_Set_FmPlusTable( sbsConfig->FmPlus );

    if( SBS_REQUEST_OK == retState )
    {
        retState = Sbs_Reg_Set_Verified( SBS_REG_FPUIMR,
                                         SBS_FPUIMR_FPU_IE,
                                         SBS_FPU_TO_REG( sbsConfig->FpuInterruptMask ) );
    }
    else
    {
        /* Fast-mode Plus bits were not written as required */
    }

    if( SBS_REQUEST_OK == retState )
    {
        retState = Sbs_Set_IoCompConfig( &sbsConfig->IoCompensation );
    }
    else
    {
        /* Previous step failed */
    }

    if( SBS_REQUEST_OK == retState )
    {
        retState = Sbs_Set_BreakConnection( sbsConfig->BreakSource );
    }
    else
    {
        /* Previous step failed, the break sources are not connected */
    }

    return ( retState );
}


/**
 * \brief Writes the Fast-mode Plus bits of all pins of the device line.
 *
 * \param fmPlusTable [in]: Fast-mode Plus state of the pins, indexed by \ref sbs_FmPlusPin_t
 *                          (\ref SBS_FMPLUS_PIN_CNT items), the items shall be valid
 *
 * \return Returns \ref SBS_REQUEST_OK if SBS_PMCR read back as written. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Set_FmPlusTable( const sbs_FunctionState_t * const fmPlusTable )
{
    uint32_t regValue = SBS_BITS_NONE;

    for( uint32_t pinIdx = 0u; SBS_FMPLUS_PIN_CNT > pinIdx; pinIdx ++ )
    {
        if( SBS_FUNCTION_ACTIVE == fmPlusTable[ pinIdx ] )
        {
            regValue |= sbs_FmPlusBitLut[ pinIdx ];
        }
        else
        {
            /* High drive of the pin is not requested, its bit stays cleared */
        }
    }

    return ( Sbs_Reg_Set_Verified( SBS_REG_PMCR, SBS_FMPLUS_BITS_ALL, regValue ) );
}


/**
 * \brief Configures the compensation cells.
 *
 * The CSI oscillator is requested from the Rcc module before the first register is written, when a cell is in the
 * automatic mode.
 *
 * \param compConfig [in]: Pointer to the configuration of the cells, checked
 *
 * \return Returns \ref SBS_REQUEST_OK if all cells were configured. Otherwise returns \ref SBS_REQUEST_ERROR,
 *         the first failure stops the sequence.
 */
static sbs_RequestState_t Sbs_Set_IoCompConfig( const sbs_IoCompConfig_t * const compConfig )
{
    sbs_RequestState_t    retState    = SBS_REQUEST_OK;
    sbs_FunctionState_t   needCsi     = SBS_FUNCTION_INACTIVE;

    for( uint32_t cellIdx = 0u; SBS_IO_COMP_CELL_CNT > cellIdx; cellIdx ++ )
    {
        if( SBS_IO_COMP_MODE_AUTOMATIC == compConfig->Cell[ cellIdx ].Mode )
        {
            needCsi = SBS_FUNCTION_ACTIVE;
        }
        else
        {
            /* The cell needs no oscillator */
        }
    }

    if( SBS_FUNCTION_ACTIVE == needCsi )
    {
        const rcc_RequestState_t rccState = Rcc_Set_OscActive( RCC_OSC_CSI );

        if( RCC_REQUEST_OK != rccState )
        {
            retState = SBS_REQUEST_ERROR;
        }
        else
        {
            /* CSI oscillator is enabled */
        }
    }
    else
    {
        /* No cell in the automatic mode */
    }

    for( uint32_t cellIdx = 0u;
         ( SBS_IO_COMP_CELL_CNT >  cellIdx ) &&
         ( SBS_REQUEST_OK       == retState );
         cellIdx ++ )
    {
        retState = Sbs_Set_IoCompCell( (sbs_IoCompCell_t)cellIdx, &compConfig->Cell[ cellIdx ] );
    }

    return ( retState );
}


/**
 * \brief Configures one compensation cell (sequence in the description of \ref Sbs_Set_IoCompensation).
 *
 * \param cellId     [in]: Compensation cell
 * \param cellConfig [in]: Configuration of the cell, checked
 *
 * \return Returns \ref SBS_REQUEST_OK if all register writes read back as written and the cell in the automatic
 *         mode became ready. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Set_IoCompCell( sbs_IoCompCell_t cellId, const sbs_IoCompCellConfig_t * const cellConfig )
{
    sbs_RequestState_t      retState = SBS_REQUEST_ERROR;
    const sbs_IoCompItem_t * item    = &sbs_IoCompLut[ cellId ];

    if( SBS_IO_COMP_MODE_AUTOMATIC == cellConfig->Mode )
    {
        retState = Sbs_Reg_Set_BitState( SBS_REG_CCCSR, item->EnableMask, SBS_FUNCTION_ACTIVE );

        if( SBS_REQUEST_OK == retState )
        {
            retState = Sbs_Reg_Wait_RegVal( SBS_REG_CCCSR, item->ReadyMask, item->ReadyMask );
        }
        else
        {
            /* Cell was not enabled */
        }

        if( SBS_REQUEST_OK == retState )
        {
            retState = Sbs_Reg_Set_BitState( SBS_REG_CCCSR, item->CodeSelMask, SBS_FUNCTION_INACTIVE );
        }
        else
        {
            /* Cell is not ready, its code is not selected */
        }
    }
    else
    {
        uint32_t nmosCode = SBS_IO_COMP_CODE_NMOS_DEFAULT;
        uint32_t pmosCode = SBS_IO_COMP_CODE_PMOS_DEFAULT;

        if( SBS_IO_COMP_MODE_MANUAL == cellConfig->Mode )
        {
            nmosCode = cellConfig->NmosCode;
            pmosCode = cellConfig->PmosCode;
        }
        else
        {
            /* Disabled mode: the fixed code of the reset state */
        }

        retState = Sbs_Reg_Set_Verified( SBS_REG_CCSWCR,
                                         ( item->NmosMask | item->PmosMask ),
                                         ( ( nmosCode << item->NmosPos ) | ( pmosCode << item->PmosPos ) ) );

        if( SBS_REQUEST_OK == retState )
        {
            retState = Sbs_Reg_Set_BitState( SBS_REG_CCCSR, item->CodeSelMask, SBS_FUNCTION_ACTIVE );
        }
        else
        {
            /* Software code was not written */
        }

        if( SBS_REQUEST_OK == retState )
        {
            retState = Sbs_Reg_Set_BitState( SBS_REG_CCCSR, item->EnableMask, SBS_FUNCTION_INACTIVE );
        }
        else
        {
            /* Software code is not selected, the cell stays as it is */
        }
    }

    return ( retState );
}


/**
 * \brief Writes the connection bits of the break sources and verifies them.
 *
 * \param breakSource [in]: Requested set of the break sources, valid and containing the connected ones
 *
 * \return Returns \ref SBS_REQUEST_OK if the connected set equals the request (a request of the connected
 *         set writes nothing). Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Set_BreakConnection( sbs_BreakSource_t breakSource )
{
    sbs_RequestState_t      retState  = SBS_REQUEST_OK;
    const sbs_BreakSource_t connected = Sbs_Get_BreakConnected();

    if( connected != breakSource )
    {
        uint32_t regValue = SBS_BITS_NONE;

        for( uint32_t itemIdx = 0u; SBS_BREAK_ITEM_CNT > itemIdx; itemIdx ++ )
        {
            if( SBS_BITS_NONE != ( breakSource & sbs_BreakLut[ itemIdx ].Source ) )
            {
                regValue |= sbs_BreakLut[ itemIdx ].BitMask;
            }
            else
            {
                /* Source is not requested, its bit stays cleared */
            }
        }

        retState = Sbs_Reg_Set_Verified( SBS_REG_CFGR2, SBS_BREAK_BITS_ALL, regValue );
    }
    else
    {
        /* The requested set is connected already, nothing to write */
        retState = SBS_REQUEST_OK;
    }

    return ( retState );
}


/**
 * \brief Writes the reset state to the registers which can be cleared (the connected break sources stay).
 *
 * Clears the Fast-mode Plus bits, switches both compensation cells off with the fixed code and restores the
 * FPU interrupt mask. All registers are written, a failure does not stop the sequence.
 *
 * \return Returns \ref SBS_REQUEST_OK if all registers read back as written.
 *         Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Reset_Registers( void )
{
    sbs_RequestState_t        retState = SBS_REQUEST_OK;
    sbs_RequestState_t        itemState = SBS_REQUEST_ERROR;
    sbs_IoCompCellConfig_t    cellConfig;

    cellConfig.Mode     = SBS_IO_COMP_MODE_DISABLED;
    cellConfig.NmosCode = SBS_IO_COMP_CODE_NMOS_DEFAULT;
    cellConfig.PmosCode = SBS_IO_COMP_CODE_PMOS_DEFAULT;

    itemState = Sbs_Reg_Set_Verified( SBS_REG_PMCR, SBS_FMPLUS_BITS_ALL, SBS_BITS_NONE );

    if( SBS_REQUEST_OK != itemState )
    {
        retState = SBS_REQUEST_ERROR;
    }
    else
    {
        /* Fast-mode Plus bits are cleared */
    }

    itemState = Sbs_Reg_Set_Verified( SBS_REG_FPUIMR, SBS_FPUIMR_FPU_IE, SBS_FPU_TO_REG( SBS_FPU_INT_DEFAULT ) );

    if( SBS_REQUEST_OK != itemState )
    {
        retState = SBS_REQUEST_ERROR;
    }
    else
    {
        /* FPU interrupt mask is restored */
    }

    for( uint32_t cellIdx = 0u; SBS_IO_COMP_CELL_CNT > cellIdx; cellIdx ++ )
    {
        itemState = Sbs_Set_IoCompCell( (sbs_IoCompCell_t)cellIdx, &cellConfig );

        if( SBS_REQUEST_OK != itemState )
        {
            retState = SBS_REQUEST_ERROR;
        }
        else
        {
            /* Compensation cell is switched off */
        }
    }

    return ( retState );
}


/**
 * \brief Returns the break sources connected in SBS_CFGR2.
 *
 * \return Set of the connected sources, bit mask of SBS_BREAK_xxx
 */
static sbs_BreakSource_t Sbs_Get_BreakConnected( void )
{
    sbs_BreakSource_t connected = SBS_BREAK_NONE;

    for( uint32_t itemIdx = 0u; SBS_BREAK_ITEM_CNT > itemIdx; itemIdx ++ )
    {
        const uint32_t bitValue = Sbs_Get_RegBit( SBS_REG_CFGR2, sbs_BreakLut[ itemIdx ].BitMask );

        if( SBS_BITS_NONE != bitValue )
        {
            connected |= sbs_BreakLut[ itemIdx ].Source;
        }
        else
        {
            /* Source is not connected */
        }
    }

    return ( connected );
}


/**
 * \brief Returns the state of the register bits.
 *
 * \param regId   [in]: SBS register ID
 * \param bitMask [in]: Mask of the bits
 *
 * \return Returns \ref SBS_FUNCTION_ACTIVE if any bit of the mask is set.
 *         Otherwise returns \ref SBS_FUNCTION_INACTIVE.
 */
static sbs_FunctionState_t Sbs_Get_BitState( sbs_RegId_t regId, uint32_t bitMask )
{
    sbs_FunctionState_t retState = SBS_FUNCTION_INACTIVE;
    const uint32_t      bitValue = Sbs_Get_RegBit( regId, bitMask );

    if( SBS_BITS_NONE != bitValue )
    {
        retState = SBS_FUNCTION_ACTIVE;
    }
    else
    {
        retState = SBS_FUNCTION_INACTIVE;
    }

    return ( retState );
}


/**
 * \brief Returns whether a code field of a debug register holds the code which opens / unlocks.
 *
 * \param regId     [in]: SBS register ID
 * \param fieldMask [in]: Mask of the field
 * \param fieldPos  [in]: Position of the field
 *
 * \return Returns \ref SBS_FUNCTION_ACTIVE if the field holds \ref SBS_CODE_OPEN.
 *         Otherwise returns \ref SBS_FUNCTION_INACTIVE.
 */
static sbs_FunctionState_t Sbs_Get_CodeState( sbs_RegId_t regId, uint32_t fieldMask, uint32_t fieldPos )
{
    sbs_FunctionState_t retState   = SBS_FUNCTION_INACTIVE;
    const uint32_t      fieldValue = ( Sbs_Get_RegVal( regId, fieldMask ) >> fieldPos );

    if( SBS_CODE_OPEN == fieldValue )
    {
        retState = SBS_FUNCTION_ACTIVE;
    }
    else
    {
        retState = SBS_FUNCTION_INACTIVE;
    }

    return ( retState );
}


/**
 * \brief Reads the temporal isolation level from SBS_HDPLSR.
 *
 * \param hdpl [out]: Pointer to store the level, not touched when the register holds none of the four codes
 *
 * \return Returns \ref SBS_REQUEST_OK if the register holds the code of a level.
 *         Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Get_Hdpl( sbs_Hdpl_t * const hdpl )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;
    const uint32_t     hdplCode = ( Sbs_Get_RegVal( SBS_REG_HDPLSR, SBS_HDPLSR_HDPL ) >> SBS_HDPLSR_HDPL_Pos );

    for( uint32_t levelIdx = 0u; SBS_HDPL_CNT > levelIdx; levelIdx ++ )
    {
        if( sbs_HdplCodeLut[ levelIdx ] == hdplCode )
        {
            *hdpl    = (sbs_Hdpl_t)levelIdx;
            retState = SBS_REQUEST_OK;
            break;
        }
        else
        {
            /* Another level of the list */
        }
    }

    return ( retState );
}

/* =========================== INTERRUPT HANDLERS =========================== */

/* ================================ TASKS =================================== */
