/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#ifndef _IOKIT_IODISPLAYWRANGLER_H
#define _IOKIT_IODISPLAYWRANGLER_H

#include <IOKit/IOService.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>

class IODisplayWrangler : public IOService
{
    OSDeclareDefaultStructors( IODisplayWrangler );

private:
    bool	fOpen;
    IOLock *	fMatchingLock;
    OSSet *	fFramebuffers;
    OSSet *	fDisplays;

    // from control panel: number of idle minutes before dimming
    UInt32	fMinutesToDim;
    // false: use minutesToDim unless in emergency situation
    bool	fUseGeneralAggressiveness;

private:

    virtual void initForPM( void );
    virtual IOReturn setAggressiveness( unsigned long, unsigned long );
    virtual bool activityTickle( unsigned long, unsigned long );
    virtual IOReturn setPowerState( unsigned long powerStateOrdinal, IOService* whatDevice );

    virtual unsigned long initialPowerStateForDomainState( IOPMPowerFlags domainState );

    virtual void makeDisplaysUsable( void );
    virtual void idleDisplays( void );
      
    static bool _displayHandler( void * target, void * ref,
                            IOService * newService );
    static bool _displayConnectHandler( void * target, void * ref,
                            IOService * newService );

    virtual bool displayHandler( OSSet * set, IODisplay * newDisplay);
    virtual bool displayConnectHandler( void * ref, IODisplayConnect * connect);

    virtual IODisplayConnect * getDisplayConnect(
		IOFramebuffer * fb, IOIndex connect );

    virtual IOReturn getConnectFlagsForDisplayMode(
		IODisplayConnect * connect,
		IODisplayModeID mode, UInt32 * flags );

public:
    
    virtual bool start(IOService * provider);

    static bool makeDisplayConnects( IOFramebuffer * fb );
    static void destroyDisplayConnects( IOFramebuffer * fb );

    static IOReturn getFlagsForDisplayMode(
		IOFramebuffer * fb,
		IODisplayModeID mode, UInt32 * flags );
                
};

#endif /* _IOKIT_IODISPLAYWRANGLER_H */
