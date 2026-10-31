/**
 * \author Mr.Nobody
 * \file Sbs_Reg.c
 * \ingroup Sbs
 * \brief Sbs module Reg component functionality.
 *
 * Register access of the module: generic accessors (bounds checked register ID,
 * writes to read only registers are refused), the verified write of the register
 * bits and the bounded wait for a flag. The registers are written only here, the
 * masks come from the CMSIS device header.
 *
 * Properties of the registers, which the protocol respects (RM0481, RM0492, RM0517 - SBS chapter):
 * - The SBS registers are accessed through the alias of the security state of the code (the CMSIS
 *   macro SBS), a nonsecure access to a secure register or bit is read-as-zero and write-ignored.
 * - Bits of SBS_CFGR2 are set by software and cleared only by a system reset (a write of 0 is ignored).
 * - SBS_DBGCR and SBS_DBGLOCKR are read-as-zero and write-ignored for the temporal isolation levels
 *   2 and 3, SBS_FPUIMR needs a privileged access.
 * A write which does not stay is therefore found by the read-back after every write.
 *
 */
/* ============================== INCLUDES ================================== */
#include "Sbs_Reg.h"                        /* Self include                   */
#include "Sbs_Types.h"                      /* Module types definitions       */
#include "Stm32.h"                          /* SBS registers definition       */
/* ============================== TYPEDEFS ================================== */

/** \brief Structure type used to store physical address of SBS registers.
 * This array is used to reduce size of configuration array. With this style
 * the registers are referenced through enumeration.
 *
 */
typedef struct
{
    sbs_RegId_t         RegId;   /**< Peripheral register ID                   */
    volatile uint32_t * RegAddr; /**< Peripheral register address              */
    sbs_RegAccess_t     Access;  /**< Access of the module to the register     */
}   sbs_RegList_t;

/* ======================== FORWARD DECLARATIONS ============================ */

static sbs_RequestState_t   Sbs_Reg_Poll_RegVal     ( sbs_RegId_t regId,
                                                      uint32_t regMask,
                                                      uint32_t regValue,
                                                      uint32_t timeoutRaw );
static sbs_RequestState_t   Sbs_Reg_Check_Write     ( sbs_RegId_t regId );

/* ========================== SYMBOLIC CONSTANTS ============================ */

/** \brief Count of read-back iterations of a written register value */
#define SBS_REG_READBACK_TIMEOUT_RAW        ( 0x1000u )

/** \brief Count of iterations of the wait for a flag set by the hardware (ready flag of the compensation cell) */
#define SBS_REG_READY_TIMEOUT_RAW           ( 0x10000u )

/* =============================== MACROS =================================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* =========================== LOCAL VARIABLES ============================== */

/** \brief SBS registers configuration structure
 *
 * This structure is used to store addresses of SBS registers.
 * It is used to reduce code size while referring to SBS registers.
 *
 * The module never writes SBS_HDPLSR, SBS_DBGCR, SBS_DBGLOCKR and SBS_MESR (its bits are write-1-to-clear): the
 * access of the module to them is read only.
 */
static const sbs_RegList_t              sbs_RegList[] =
{
    { .RegId = SBS_REG_HDPLSR  , .RegAddr = &(SBS->HDPLSR  ), .Access = SBS_REG_ACCESS_READ       },
    { .RegId = SBS_REG_DBGCR   , .RegAddr = &(SBS->DBGCR   ), .Access = SBS_REG_ACCESS_READ       },
    { .RegId = SBS_REG_DBGLOCKR, .RegAddr = &(SBS->DBGLOCKR), .Access = SBS_REG_ACCESS_READ       },
    { .RegId = SBS_REG_PMCR    , .RegAddr = &(SBS->PMCR    ), .Access = SBS_REG_ACCESS_READ_WRITE },
    { .RegId = SBS_REG_FPUIMR  , .RegAddr = &(SBS->FPUIMR  ), .Access = SBS_REG_ACCESS_READ_WRITE },
    { .RegId = SBS_REG_MESR    , .RegAddr = &(SBS->MESR    ), .Access = SBS_REG_ACCESS_READ       },
    { .RegId = SBS_REG_CCCSR   , .RegAddr = &(SBS->CCCSR   ), .Access = SBS_REG_ACCESS_READ_WRITE },
    { .RegId = SBS_REG_CCSWCR  , .RegAddr = &(SBS->CCSWCR  ), .Access = SBS_REG_ACCESS_READ_WRITE },
    { .RegId = SBS_REG_CFGR2   , .RegAddr = &(SBS->CFGR2   ), .Access = SBS_REG_ACCESS_READ_WRITE },
};

_Static_assert( (sizeof(sbs_RegList) / sizeof(sbs_RegList_t)) == SBS_REG_CNT,
                "Sbs_Reg: sbs_RegList has incorrect size." );

/* ========================= EXPORTED FUNCTIONS ============================= */

/**
 * \brief Sets the register bit with given mask.
 *
 * \note  Writes to a read only register are refused.
 *
 * \param regId   [in]: SBS register ID
 * \param bitMask [in]: Mask to apply to the register
 */
void Sbs_Set_RegBit( sbs_RegId_t regId, uint32_t bitMask )
{
    const sbs_RequestState_t writeState = Sbs_Reg_Check_Write( regId );

    if( SBS_REQUEST_OK == writeState )
    {
        *sbs_RegList[regId].RegAddr |= bitMask;
    }
    else
    {
        /* Invalid register ID or read only register, no action required */
    }
}


/**
 * \brief Resets the register bit with given mask.
 *
 * \note  Writes to a read only register are refused.
 *
 * \param regId   [in]: SBS register ID
 * \param bitMask [in]: Mask to apply to the register
 */
void Sbs_Reset_RegBit( sbs_RegId_t regId, uint32_t bitMask )
{
    const sbs_RequestState_t writeState = Sbs_Reg_Check_Write( regId );

    if( SBS_REQUEST_OK == writeState )
    {
        *sbs_RegList[regId].RegAddr &= ~bitMask;
    }
    else
    {
        /* Invalid register ID or read only register, no action required */
    }
}


/**
 * \brief Gets the register bit value with given mask.
 *
 * \param regId   [in]: SBS register ID
 * \param bitMask [in]: Mask to apply to the register
 * \return Register bit value with applied mask, 0 for invalid register ID
 */
uint32_t Sbs_Get_RegBit( sbs_RegId_t regId, uint32_t bitMask )
{
    uint32_t retValue = 0u;

    if( SBS_REG_CNT > regId )
    {
        retValue = ( *sbs_RegList[regId].RegAddr & bitMask );
    }
    else
    {
        /* Invalid register ID, zero is returned */
        retValue = 0u;
    }

    return ( retValue );
}


/**
 * \brief Updates the register value with given mask and value.
 *
 * \note  Writes to a read only register are refused.
 *
 * \param regId    [in]: SBS register ID
 * \param regMask  [in]: Mask to apply to the register
 * \param regValue [in]: Value to set in the register
 */
void Sbs_Set_RegVal( sbs_RegId_t regId, uint32_t regMask, uint32_t regValue )
{
    const sbs_RequestState_t writeState = Sbs_Reg_Check_Write( regId );

    if( SBS_REQUEST_OK == writeState )
    {
        /* Set the register value with mask */
        *sbs_RegList[regId].RegAddr = ( ( *sbs_RegList[regId].RegAddr & ~regMask ) |
                                        ( regValue & regMask ) );
    }
    else
    {
        /* Invalid register ID or read only register, no action required */
    }
}


/**
 * \brief Gets the register value with given mask.
 *
 * \param regId   [in]: SBS register ID.
 * \param regMask [in]: Mask to apply to the register
 * \return Register value with applied mask, 0 for invalid register ID
 */
uint32_t Sbs_Get_RegVal( sbs_RegId_t regId, uint32_t regMask )
{
    uint32_t retValue = 0u;

    if( SBS_REG_CNT > regId )
    {
        /* Get the register value with mask */
        retValue = ( *sbs_RegList[regId].RegAddr & regMask );
    }
    else
    {
        /* Invalid register ID, zero is returned */
        retValue = 0u;
    }

    return ( retValue );
}


/**
 * \brief Writes the bits of the register with given mask and verifies them by read-back.
 *
 * The bits outside of the mask keep their value (read-modify-write). The value is
 * checked before the write: a value with a bit outside of the mask is refused.
 *
 * \param regId    [in]: SBS register ID
 * \param regMask  [in]: Mask of the written bits
 * \param regValue [in]: Value of the masked bits (already positioned)
 *
 * \return Returns \ref SBS_REQUEST_OK if the value was read back within the timeout.
 *         Returns \ref SBS_REQUEST_ERROR for an invalid register ID, a read only
 *         register or a value outside of the mask (nothing is written), or when the
 *         read-back does not match (the write was ignored by the hardware).
 */
sbs_RequestState_t Sbs_Reg_Set_Verified( sbs_RegId_t regId, uint32_t regMask, uint32_t regValue )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t writeState = Sbs_Reg_Check_Write( regId );

    if( ( SBS_REQUEST_OK == writeState              ) &&
        ( 0u             == ( regValue & ~regMask ) )    )
    {
        Sbs_Set_RegVal( regId, regMask, regValue );

        retState = Sbs_Reg_Poll_RegVal( regId, regMask, regValue, SBS_REG_READBACK_TIMEOUT_RAW );
    }
    else
    {
        /* Invalid register, or value out of the mask, nothing is written */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Sets or clears the register bits with given mask and verifies them by read-back.
 *
 * \param regId   [in]: SBS register ID
 * \param bitMask [in]: Mask of the bits
 * \param state   [in]: \ref SBS_FUNCTION_ACTIVE sets the bits, \ref SBS_FUNCTION_INACTIVE clears them
 *
 * \return Returns \ref SBS_REQUEST_OK if the bits read back as requested within the timeout.
 *         Returns \ref SBS_REQUEST_ERROR for an invalid register ID, a read only register
 *         or an invalid state (nothing is written), or when the read-back does not match.
 */
sbs_RequestState_t Sbs_Reg_Set_BitState( sbs_RegId_t regId, uint32_t bitMask, sbs_FunctionState_t state )
{
    sbs_RequestState_t       retState   = SBS_REQUEST_ERROR;
    const sbs_RequestState_t writeState = Sbs_Reg_Check_Write( regId );

    if( SBS_REQUEST_OK == writeState )
    {
        if( SBS_FUNCTION_ACTIVE == state )
        {
            Sbs_Set_RegBit( regId, bitMask );

            retState = Sbs_Reg_Poll_RegVal( regId, bitMask, bitMask, SBS_REG_READBACK_TIMEOUT_RAW );
        }
        else if( SBS_FUNCTION_INACTIVE == state )
        {
            Sbs_Reset_RegBit( regId, bitMask );

            retState = Sbs_Reg_Poll_RegVal( regId, bitMask, 0u, SBS_REG_READBACK_TIMEOUT_RAW );
        }
        else
        {
            /* Invalid state, nothing is written */
            retState = SBS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Invalid register or read only register, nothing is written */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}


/**
 * \brief Waits until a flag of the register reads the required value (bounded wait, no write).
 *
 * Used for the flags set by the hardware, e.g. the ready flag of the compensation cell.
 *
 * \param regId    [in]: SBS register ID
 * \param regMask  [in]: Mask of the checked bits
 * \param regValue [in]: Required value of the masked bits
 *
 * \return Returns \ref SBS_REQUEST_OK if the value was read within the timeout.
 *         Otherwise (also for an invalid register ID) returns \ref SBS_REQUEST_ERROR.
 */
sbs_RequestState_t Sbs_Reg_Wait_RegVal( sbs_RegId_t regId, uint32_t regMask, uint32_t regValue )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

    if( SBS_REG_CNT > regId )
    {
        retState = Sbs_Reg_Poll_RegVal( regId, regMask, regValue, SBS_REG_READY_TIMEOUT_RAW );
    }
    else
    {
        /* Invalid register ID */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}

/* =========================== LOCAL FUNCTIONS ============================== */

/**
 * \brief Waits until the register (bit field) reads the required value.
 *
 * \param regId      [in]: SBS register ID
 * \param regMask    [in]: Mask of the checked bits
 * \param regValue   [in]: Required value of the masked bits
 * \param timeoutRaw [in]: Count of the iterations (register reads) of the wait
 *
 * \return Returns \ref SBS_REQUEST_OK if the value was read within the
 *         timeout. Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Reg_Poll_RegVal( sbs_RegId_t regId,
                                               uint32_t regMask,
                                               uint32_t regValue,
                                               uint32_t timeoutRaw )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

    for( uint32_t iterationCnt = 0u; timeoutRaw > iterationCnt; iterationCnt ++ )
    {
        const uint32_t readValue = Sbs_Get_RegVal( regId, regMask );

        if( ( regValue & regMask ) == readValue )
        {
            retState = SBS_REQUEST_OK;
            break;
        }
        else
        {
            /* Register has not been updated yet */
            retState = SBS_REQUEST_ERROR;
        }
    }

    return ( retState );
}


/**
 * \brief Checks that the register can be written by the module.
 *
 * \param regId [in]: SBS register ID
 *
 * \return Returns \ref SBS_REQUEST_OK for a valid register ID of a writable register.
 *         Otherwise returns \ref SBS_REQUEST_ERROR.
 */
static sbs_RequestState_t Sbs_Reg_Check_Write( sbs_RegId_t regId )
{
    sbs_RequestState_t retState = SBS_REQUEST_ERROR;

    if( SBS_REG_CNT > regId )
    {
        if( SBS_REG_ACCESS_READ_WRITE == sbs_RegList[regId].Access )
        {
            retState = SBS_REQUEST_OK;
        }
        else
        {
            /* Read only register */
            retState = SBS_REQUEST_ERROR;
        }
    }
    else
    {
        /* Invalid register ID */
        retState = SBS_REQUEST_ERROR;
    }

    return ( retState );
}

/* =========================== INTERRUPT HANDLERS =========================== */

/* ================================ TASKS =================================== */
