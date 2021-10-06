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

#ifndef _IOKIT_IOGRAPHICSTYPESPRIVATE_H
#define _IOKIT_IOGRAPHICSTYPESPRIVATE_H

#include <IOKit/graphics/IOGraphicsTypes.h>

enum {
    // options for IOServiceRequestProbe()
    kIOFBForceReadEDID			= 0x00000100,
    kIOFBAVProbe			= 0x00000200
};

enum {
    kFramebufferAGPFastWriteAccess	= 0x00100000
};

enum {
    kIOFBHWCursorSupported		= 0x00000001,
    kIOFBCursorPans			= 0x00010000
};

enum {
    // Controller attributes
    kIOSystemPowerAttribute		= 'spwr',
    kIOVRAMSaveAttribute		= 'vrsv',
    kIODeferCLUTSetAttribute		= 'vclt',

    kIOFBSpeedAttribute			= ' dgs',

    // Connection attributes
    kConnectionPostWake			= 'pwak',

    kConnectionInTVMode			= 'tvmd',

    kConnectionDisplayParameterCount	= 'pcnt',
    kConnectionDisplayParameters	= 'parm',

    kConnectionOverscan			= 'oscn',
    kConnectionVideoBest		= 'vbst',
    kConnectionWSSB			= 'wssb'
};

enum {
    // kConnectionInTVMode values
    kConnectionNonTVMode		= 0,
    kConnectionNTSCMode			= 1,
    kConnectionPALMode			= 2
};

#define kIOFBGammaWidthKey		"IOFBGammaWidth"
#define kIOFBGammaCountKey		"IOFBGammaCount"
#define kIOFBGammaHeaderSizeKey		"IOFBGammaHeaderSize"

#define kIOFBCLUTDeferKey		"IOFBCLUTDefer"

#define kIONDRVFramebufferGenerationKey "IONDRVFramebufferGeneration"

#define kIODisplayOverscanKey		"oscn"
#define kIODisplayVideoBestKey		"vbst"

#endif /* ! _IOKIT_IOGRAPHICSTYPESPRIVATE_H */

