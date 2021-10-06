/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#ifndef _IOKIT_IOGRAPHICSPRIVATE_H
#define _IOKIT_IOGRAPHICSPRIVATE_H

#include <mach/vm_param.h>
#include <libkern/version.h>
#include <libkern/OSDebug.h>
#include <IOKit/graphics/IOGraphicsTypesPrivate.h>

#if 0
#warning **LOGS**
#define RLOG 1
#define DEBG(name, fmt, args...)         \
do {                                    \
    AbsoluteTime    now;                \
    UInt64          nano;               \
    AbsoluteTime_to_scalar(&now) = mach_absolute_time();                \
    absolutetime_to_nanoseconds(now, &nano);                            \
    IOLog("%08d [%s]::", (uint32_t) (nano / 1000000ULL), name);           \
    IOLog("%s", __FUNCTION__);          \
    IOLog(fmt, ## args);                \
    kprintf("%08d [%s]::", (uint32_t) (nano / 1000000ULL), name);         \
    kprintf("%s", __FUNCTION__);        \
    kprintf(fmt, ## args);              \
} while( false )

#else
#define DEBG(idx, fmt, args...)  {}
#endif

#if 0

#warning **LOGS**
#define RLOG1 1
#define DEBG1(name, fmt, args...)                \
do {                                    \
    AbsoluteTime    now;                \
    UInt64          nano;               \
    AbsoluteTime_to_scalar(&now) = mach_absolute_time();                \
    absolutetime_to_nanoseconds( now, &nano );                          \
    IOLog("%08d [%s]::", (uint32_t) (nano / 1000000ULL), name);           \
    IOLog("%s", __FUNCTION__);          \
    IOLog(fmt, ## args);                \
    kprintf("%08d [%s]::", (uint32_t) (nano / 1000000ULL), name);           \
    kprintf("%s", __FUNCTION__);          \
    kprintf(fmt, ## args);                \
} while( false )

#else
#define DEBG1(idx, fmt, args...)  {}
#endif

#if 0

#warning **LOGS**
#define DEBG2(name, fmt, args...)                \
do {                                    \
    AbsoluteTime    now;                \
    UInt64          nano;               \
    AbsoluteTime_to_scalar(&now) = mach_absolute_time();                \
    absolutetime_to_nanoseconds( now, &nano );                          \
    IOLog("%08d [%s]::", (uint32_t) (nano / 1000000ULL), name);           \
    IOLog("%s", __FUNCTION__);          \
    IOLog(fmt, ## args);                \
    kprintf("%08d [%s]::", (uint32_t) (nano / 1000000ULL), name);           \
    kprintf("%s", __FUNCTION__);          \
    kprintf(fmt, ## args);                \
} while( false )

#else
#define DEBG2(idx, fmt, args...)  {}
#endif

#define STOREINC(_ptr_, _data_, _type_) {   \
        *((_type_ *)(_ptr_)) = _data_;                                  \
        _ptr_ = (typeof(_ptr_)) (((char *) (_ptr_)) + sizeof(_type_));  \
    }

// blue actual:0x00426bad gamma:0x00648cc3 bootx:0x00bfbfbf
#define kIOFBBootGrayValue              0x00bfbfbf
#define kIOFBGrayValue                  0x00000000

#ifndef kAppleAudioVideoJackStateKey
#define kAppleAudioVideoJackStateKey    "AppleAudioVideoJackState"
#endif
#ifndef kIOPMIsPowerManagedKey
#define kIOPMIsPowerManagedKey          "IOPMIsPowerManaged"
#endif
#ifndef kIOAGPCommandValueKey
#define kIOAGPCommandValueKey           "IOAGPCommandValue"
#endif
#ifndef kAppleClamshellStateKey
#define kAppleClamshellStateKey         "AppleClamshellState"
#endif
#ifndef kIOFBWaitCursorFramesKey
#define kIOFBWaitCursorFramesKey        "IOFBWaitCursorFrames"
#endif
#ifndef kIOFBWaitCursorPeriodKey
#define kIOFBWaitCursorPeriodKey        "IOFBWaitCursorPeriod"
#endif

#ifndef kIOUserClientSharedInstanceKey
#define kIOUserClientSharedInstanceKey  "IOUserClientSharedInstance"
#endif

#ifndef kIOHibernateOptionsKey
#define kIOHibernateOptionsKey      "IOHibernateOptions"
#endif

#ifndef kIOHibernateGfxStatusKey
#define kIOHibernateGfxStatusKey    "IOHibernateGfxStatus"
#endif

#ifndef kIOMessageSystemPagingOff
#define kIOMessageSystemPagingOff       iokit_common_msg(0x255)
#endif

extern "C" ppnum_t pmap_find_phys(pmap_t map, addr64_t va);

extern "C" vm_map_t IOPageableMapForAddress( vm_address_t address );

extern "C" IOReturn IOGetHardwareClamshellState( IOOptionBits * result );

extern bool                   gIOGraphicsSystemPower;
extern bool                   gIOFBSystemPower;
extern IOOptionBits           gIOFBCurrentClamshellState;
extern const class OSSymbol * gIOFramebufferKey;
extern class OSData *         gIOFBZero32Data;
extern int32_t                gIOFBHaveBacklight;
extern const OSSymbol *       gIOFBPMSettingDisplaySleepUsesDimKey;

#if __ppc__
extern "C" void bcopy_nc( void * from, void * to, UInt32 l );
extern "C" void bzero_nc( void * p, UInt32 l );
#else
inline void bcopy_nc( void * from, void * to, UInt32 l) { bcopy( from, to, l ); }
inline void bzero_nc( void * p, UInt32 l )              { bzero( p, l ); }
#endif

#if VERSION_MAJOR < 9
#define getPowerState() pm_vars->myCurrentState
#endif

#define thisIndex               _IOFramebuffer_reserved[4]

extern uint32_t gIOGDebugFlags;
enum
{
	kIOGDbgLidOpen         = 0x00000001,
	kIOGDbgVBLThrottle     = 0x00000002,
	kIOGDbgK59Mode         = 0x00000004,
	kIOGDbgDumbPanic       = 0x00000008,
	kIOGDbgVBLDrift        = 0x00000010,
	kIOGDbgForceBrightness = 0x00000020,
};

#ifndef kIOScreenLockStateKey

#define IOHIB_PREVIEW_V0	1

enum { kIOPreviewImageCount = 1 };

struct hibernate_preview_t
{
    uint32_t  depth;      	// Pixel Depth
    uint32_t  width;      	// Width
    uint32_t  height;     	// Height
};
typedef struct hibernate_preview_t hibernate_preview_t;

#define kIOScreenLockStateKey      "IOScreenLockState"

#endif /* ! kIOScreenLockStateKey */

// these are the private instance variables for power management
struct IODisplayPMVars
{
    UInt32              currentState;
    // highest state number normally, lowest usable state in emergency
    unsigned long       maxState;
    unsigned long       minDimState;
    // true if the display has had power lowered due to user inactivity
    bool                displayIdle;
};

#endif /* ! _IOKIT_IOGRAPHICSPRIVATE_H */

