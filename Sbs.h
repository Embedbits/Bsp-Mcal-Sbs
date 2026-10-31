/**
 * \author Mr.Nobody
 * \file Sbs.h
 * \ingroup Sbs
 * \brief Sbs module common functionality header file.
 *
 * This file contains the common functionality used internally by the module,
 * and shall provide interface between the module and the application.
 *
 */

#ifndef SBS_SBS_H
#define SBS_SBS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================= INCLUDES =================================== */
#include "Sbs_Types.h"                      /* Module types definitions       */
#include "Sbs_Reg.h"                        /* Register identifiers           */
/* ============================= TYPEDEFS =================================== */

/** \brief Break source item: the source of the interface and its connection bit in SBS_CFGR2 */
typedef struct
{
    sbs_BreakSource_t Source;  /**< Fault source (position of the item in the table)       */
    uint32_t          BitMask; /**< Connection (lock) bit of the source in SBS_CFGR2       */
}   sbs_BreakItem_t;


/** \brief Compensation cell item: the bits of the cell in SBS_CCCSR and SBS_CCSWCR */
typedef struct
{
    sbs_IoCompCell_t CellId;      /**< Cell (position of the item in the table)                   */
    uint32_t         EnableMask;  /**< Cell enable bit in SBS_CCCSR (ENx)                         */
    uint32_t         CodeSelMask; /**< Code selection bit in SBS_CCCSR (CSx), set: software code */
    uint32_t         ReadyMask;   /**< Cell ready flag in SBS_CCCSR (RDYx)                        */
    uint32_t         NmosMask;    /**< NMOS code field in SBS_CCSWCR (SW_ANSRCx)                  */
    sbs_IoCompPos_t  NmosPos;    /**< Position of the NMOS code field                            */
    uint32_t         PmosMask;    /**< PMOS code field in SBS_CCSWCR (SW_APSRCx)                  */
    sbs_IoCompPos_t  PmosPos;    /**< Position of the PMOS code field                            */
}   sbs_IoCompItem_t;

/* ========================= SYMBOLIC CONSTANTS ============================= */

/* ========================= EXPORTED MACROS ================================ */

/* ========================= EXPORTED VARIABLES ============================= */

/* ======================== EXPORTED FUNCTIONS ============================== */

#ifdef __cplusplus
}
#endif

#endif /* SBS_SBS_H */
