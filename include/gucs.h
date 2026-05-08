#ifndef GUCS_H
#define GUCS_H

#include "enums.h"

/* 
    GUCs (Grand Unified Configuration) definitions,
    including custom configuration variables for the extension.
*/

extern void DefineCustomGUCs(void);

// internal GUC variables


// external GUC variables
// integer GUCs
// GUC variable for memory checking mode
extern MemCheckMode memcheck_mode; 


// boolean GUCs

// string GUCs

#endif /* GUCS_H */