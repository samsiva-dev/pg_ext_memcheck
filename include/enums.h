#ifndef ENUMS_H
#define ENUMS_H

/* 
    Enum definitions for the extension.
*/

/*
    MemCheckMode: Enumeration for memory checking modes in the extension.
     - MEMCHECK_ALL: Check all memory allocations and deallocations i.e. 
       both in the planner, executor and other parts of the system.
     - MEMCHECK_EXECUTOR: Check memory allocations and deallocations in the executor.
     - MEMCHECK_NONE: Disable memory checking.
*/
typedef enum
{
    MEMCHECK_ALL,        
    MEMCHECK_EXECUTOR,
    MEMCHECK_NONE
} MemCheckMode;


#endif /* ENUMS_H */