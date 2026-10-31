/**
 * \author Mr.Nobody
 * \file Sbs_Reg.h
 * \ingroup Sbs
 * \brief Sbs module Reg component header file.
 *
 * This component contains functionality for register operations. It is private
 * to the module (the register access is not shared with other modules).
 *
 */

#ifndef SBS_REG_SBS_REG_H
#define SBS_REG_SBS_REG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Sbs_Types.h"                      /* Module types definition        */
/* ============================= TYPEDEFS =================================== */

/** \brief List of SBS registers used by the module
 *
 * All registers exist on all STM32H5 lines (the layout of the registers used by the module is common, the
 * bits differ per line, see \ref Sbs_Types.h). The other registers of the SBS (HDPL control, next HDPL,
 * RSS command, EPOCH selection, security configuration, CPU locks, flash ECC NMI mask, OTG_HS PHY tuning,
 * compensation cell value) are not used by the module.
 */
typedef enum
{
    SBS_REG_HDPLSR = 0u, /**< Temporal isolation status register (HDPL)                  */
    SBS_REG_DBGCR      , /**< Debug control register                                     */
    SBS_REG_DBGLOCKR   , /**< Debug lock register                                        */
    SBS_REG_PMCR       , /**< Product mode and configuration register (FM+, Ethernet)    */
    SBS_REG_FPUIMR     , /**< FPU interrupt mask register                                */
    SBS_REG_MESR       , /**< Memory erase status register                               */
    SBS_REG_CCCSR      , /**< Compensation cell control and status register              */
    SBS_REG_CCSWCR     , /**< Compensation cell software code register                   */
    SBS_REG_CFGR2      , /**< Class B register (fault sources to the timer break inputs) */
    SBS_REG_CNT          /**< Count of available SBS registers                           */
}   sbs_RegId_t;


/** \brief Access of the module to a register */
typedef enum
{
    SBS_REG_ACCESS_READ = 0u, /**< Register is read only for the module, writes are refused */
    SBS_REG_ACCESS_READ_WRITE /**< Register can be read and written                         */
}   sbs_RegAccess_t;

/* ========================= SYMBOLIC CONSTANTS ============================= */

/* ========================= EXPORTED MACROS ================================ */

/* ========================= EXPORTED VARIABLES ============================= */

/* ======================== EXPORTED FUNCTIONS ============================== */

/* ---------------------------- Generic accessors --------------------------- */

void                        Sbs_Set_RegBit              ( sbs_RegId_t regId, uint32_t bitMask );
void                        Sbs_Reset_RegBit            ( sbs_RegId_t regId, uint32_t bitMask );
uint32_t                    Sbs_Get_RegBit              ( sbs_RegId_t regId, uint32_t bitMask );

void                        Sbs_Set_RegVal              ( sbs_RegId_t regId, uint32_t regMask, uint32_t regValue );
uint32_t                    Sbs_Get_RegVal              ( sbs_RegId_t regId, uint32_t regMask );

/* ----------------------------- Register protocol -------------------------- */

sbs_RequestState_t          Sbs_Reg_Set_Verified        ( sbs_RegId_t regId, uint32_t regMask, uint32_t regValue );
sbs_RequestState_t          Sbs_Reg_Set_BitState        ( sbs_RegId_t regId,
                                                          uint32_t bitMask,
                                                          sbs_FunctionState_t state );
sbs_RequestState_t          Sbs_Reg_Wait_RegVal         ( sbs_RegId_t regId, uint32_t regMask, uint32_t regValue );

#ifdef __cplusplus
}
#endif

#endif /* SBS_REG_SBS_REG_H */
