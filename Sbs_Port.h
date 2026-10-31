/**
 * \author Mr.Nobody
 * \file Sbs_Port.h
 * \ingroup Sbs
 * \brief Sbs module public functionality
 *
 * This file contains all available public functionality, any other files shall
 * not used outside of the module.
 *
 */

#ifndef SBS_SBS_PORT_H
#define SBS_SBS_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== INCLUDES ================================== */
#include "Sbs_Types.h"                      /* Module types definition        */
/* ============================== TYPEDEFS ================================== */

/* ========================== SYMBOLIC CONSTANTS ============================ */

/* ========================== EXPORTED MACROS =============================== */

/* ========================== EXPORTED VARIABLES ============================ */

/* ========================= EXPORTED FUNCTIONS ============================= */

sbs_ModuleVersion_t         Sbs_Get_ModuleVersion           ( void );

sbs_RequestState_t          Sbs_Init                        ( const sbs_Config_t * const sbsConfig );
sbs_RequestState_t          Sbs_Deinit                      ( void );
void                        Sbs_Task                        ( void );

sbs_RequestState_t          Sbs_Get_DefaultConfig           ( sbs_Config_t * const sbsConfig );

/* ------------------------------- I/O features ------------------------------ */

sbs_RequestState_t          Sbs_Set_FmPlus                  ( sbs_FmPlusPin_t fmPlusPin, sbs_FunctionState_t state );
sbs_RequestState_t          Sbs_Get_FmPlus                  ( sbs_FmPlusPin_t fmPlusPin,
                                                              sbs_FunctionState_t * const state );

sbs_RequestState_t          Sbs_Set_AnalogBooster           ( sbs_FunctionState_t state );
sbs_RequestState_t          Sbs_Get_AnalogBooster           ( sbs_FunctionState_t * const state );

sbs_RequestState_t          Sbs_Set_IoCompensation          ( const sbs_IoCompConfig_t * const compConfig );
sbs_RequestState_t          Sbs_Get_IoCompensationState     ( sbs_FunctionState_t * const readyState );

/* ------------------------------ Break sources ------------------------------ */

sbs_RequestState_t          Sbs_Set_BreakSource             ( sbs_BreakSource_t breakSource );
sbs_RequestState_t          Sbs_Get_BreakSource             ( sbs_BreakSource_t * const breakSource );

/* -------------------------------- Interfaces ------------------------------- */

sbs_RequestState_t          Sbs_Set_EthPhyInterface         ( sbs_EthPhyInterface_t phyInterface );
sbs_RequestState_t          Sbs_Get_EthPhyInterface         ( sbs_EthPhyInterface_t * const phyInterface );

sbs_RequestState_t          Sbs_Set_FpuInterruptMask        ( sbs_FpuInterrupt_t fpuInterrupt );
sbs_RequestState_t          Sbs_Get_FpuInterruptMask        ( sbs_FpuInterrupt_t * const fpuInterrupt );

/* ------------------------------ Memory remap ------------------------------- */

sbs_RequestState_t          Sbs_Set_MemoryRemap             ( sbs_MemoryRemap_t memoryRemap );
sbs_RequestState_t          Sbs_Get_MemoryRemap             ( sbs_MemoryRemap_t * const memoryRemap );

/* -------------------------- Boot and security state ------------------------ */

sbs_RequestState_t          Sbs_Get_BootState               ( sbs_BootState_t * const bootState );
sbs_RequestState_t          Sbs_Get_ProductState            ( sbs_ProductState_t * const productState );
sbs_RequestState_t          Sbs_Get_DebugState              ( sbs_DebugState_t * const debugState );
sbs_RequestState_t          Sbs_Get_EraseState              ( sbs_EraseState_t * const eraseState );

#ifdef __cplusplus
}
#endif

#endif /* SBS_SBS_PORT_H */
