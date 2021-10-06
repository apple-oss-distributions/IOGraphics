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


#include <IOKit/IOLib.h>
#include <libkern/c++/OSContainers.h>
#include <libkern/OSByteOrder.h>

#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#define IOFRAMEBUFFER_PRIVATE
#include <IOKit/graphics/IOGraphicsPrivate.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>

#include "IOFramebufferUserClient.h"
#include "IODisplayWrangler.h"
#include "IOFramebufferReallyPrivate.h"
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>

#include <string.h>
#include <IOKit/assert.h>
#include <sys/kdebug.h>

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define DOANIO	      0
#define VRAM_SAVE     1
#define VRAM_COMPRESS 1

#if VRAM_COMPRESS
#include "bmcompress.h"
#endif

#if DOANIO
#include <sys/uio.h>
#include <sys/conf.h>
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static OSArray *	gAllFramebuffers;
static OSArray *	gRunawayFramebuffers;
static class IOFBGate *	gIOFBGate;
static thread_call_t	gIOFBSleepCallout;
static IOWorkLoop *	gIOFBWorkLoop;
static IONotifier *	gIOFBRootNotifier;
static IOService *	gIOFBSystemPowerAckTo;
static UInt32		gIOFBSystemPowerAckRef;
bool			gIOFBSystemPower = true;
static bool		gIOFBSleepThread;
static thread_call_t	gIOFBClamshellCallout;
static SInt32		gIOFBClamshellEnable;
static IOOptionBits	gIOFBClamshellState;
static SInt32		gIOFBSuspendCount;
static IOFramebuffer *  gIOFBConsoleFramebuffer;
bool			gIOFBDesktopModeAllowed = true;
IOOptionBits		gIOFBLastClamshellState;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct IOFramebufferPrivate
{
    IOGSize			maxWaitCursorSize;
    UInt32			numCursorFrames;
    UInt8 *			cursorFlags;
    volatile unsigned char **	cursorImages;
    volatile unsigned char **	cursorMasks;

    class IOFramebufferParameterHandler * paramHandler;

    OSArray *			cursorAttributes;
    IOFBCursorControlAttribute	cursorControl;
    IOInterruptEventSource *	cursorThread;
    IOOptionBits 		cursorToDo;
    UInt32			framePending;
    SInt32			xPending;
    SInt32			yPending;
    IOGPoint			cursorHotSpotAdjust[ 2 ];

    IOByteCount			gammaHeaderSize;
    UInt32			desiredGammaDataWidth;
    UInt32			desiredGammaDataCount;

    IOInterruptEventSource *	deferredCLUTSetEvent;
    IOByteCount			gammaDataLen;
    UInt8 *			gammaData;
    UInt32			gammaChannelCount;
    UInt32			gammaDataCount;
    UInt32			gammaDataWidth;
    IOByteCount			clutDataLen;
    UInt8 *			clutData;
    UInt32			clutIndex;
    UInt32			clutOptions;

    UInt32			framebufferWidth;
    UInt32			framebufferHeight;
    UInt32			saveLength;
    void *			saveFramebuffer;

    UInt8			visiblePending;
    UInt8			testingCursor;
    UInt8			disabledForConnectChange;
    UInt8			index;

    UInt8			cursorSlept;
    UInt8			cursorPanning;
    UInt8			mirrorState;
    UInt8			pendingSpeedChange;

    UInt32			reducedSpeed;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define	GetShmem(instance)	((StdFBShmem_t *)(instance->priv))

#define KICK_CURSOR(thread)	\
            thread->interruptOccurred(0, 0, 0);

#define CLEARSEMA(shmem, inst)				\
        if( inst->__private->cursorToDo ) {		\
            KICK_CURSOR(inst->__private->cursorThread);	\
        }						\
        ev_unlock(&shmem->cursorSema)

#define SETSEMA(shmem)		\
        if (!ev_try_lock(&shmem->cursorSema)) return;
#define TOUCHBOUNDS(one, two) \
        (((one.minx < two.maxx) && (two.minx < one.maxx)) && \
        ((one.miny < two.maxy) && (two.miny < one.maxy)))

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOFBGate : public IOCommandGate
{
    OSDeclareDefaultStructors(IOFBGate)
public:
    static IOFBGate *gate(IOService *owner);
    inline void closeGate()
    {
        IOCommandGate::closeGate();
    };
    inline void openGate()
    {
        IOCommandGate::openGate();
    };
};

#define super IOCommandGate
OSDefineMetaClassAndStructors(IOFBGate, IOCommandGate)

IOFBGate * IOFBGate::gate(IOService *inOwner)
{
    IOFBGate *me = new IOFBGate;

    if (me && !me->init(inOwner, 0))
    {
        me->free();
        return (0);
    }

    return (me);
}

#define FBLOCK()	\
    gIOFBGate->closeGate();
#define FBUNLOCK()	\
    gIOFBGate->openGate();

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOFramebufferParameterHandler : public IODisplayParameterHandler
{
    OSDeclareDefaultStructors(IOFramebufferParameterHandler)

    OSDictionary *	fDisplayParams;
    IOFramebuffer * 	fFramebuffer;
    IODisplay *		fDisplay;

public:
    static IOFramebufferParameterHandler * withFramebuffer( IOFramebuffer * framebuffer );
    virtual void free();

    virtual bool setDisplay( IODisplay * display );
    virtual bool doIntegerSet( OSDictionary * params,
                               const OSSymbol * paramName, UInt32 value );
    virtual bool doDataSet( const OSSymbol * paramName, OSData * value );
    virtual bool doUpdate( void );

    void displayModeChange( void );
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOGraphicsDevice

OSDefineMetaClass( IOFramebuffer, IOGraphicsDevice )
OSDefineAbstractStructors( IOFramebuffer, IOGraphicsDevice )

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Cursor rendering
 */

#include "IOCursorBlits.h"

inline void IOFramebuffer::StdFBDisplayCursor( IOFramebuffer * inst )
{
    StdFBShmem_t *shmem;
    Bounds saveRect;
    volatile unsigned char *vramPtr;	/* screen data pointer */
    unsigned int cursStart;
    unsigned int cursorWidth;
    int width;
    int height;

    shmem = GetShmem(inst);
    saveRect = shmem->cursorRect;
    /* Clip saveRect vertical within screen bounds */
    if (saveRect.miny < shmem->screenBounds.miny)
        saveRect.miny = shmem->screenBounds.miny;
    if (saveRect.maxy > shmem->screenBounds.maxy)
        saveRect.maxy = shmem->screenBounds.maxy;
    if (saveRect.minx < shmem->screenBounds.minx)
        saveRect.minx = shmem->screenBounds.minx;
    if (saveRect.maxx > shmem->screenBounds.maxx)
        saveRect.maxx = shmem->screenBounds.maxx;
    shmem->saveRect = saveRect; /* Remember save rect for RemoveCursor */

    vramPtr = inst->frameBuffer +
              (inst->rowBytes * (saveRect.miny - shmem->screenBounds.miny)) +
              (inst->bytesPerPixel * (saveRect.minx - shmem->screenBounds.minx));

    width = saveRect.maxx - saveRect.minx;
    height = saveRect.maxy - saveRect.miny;
    cursorWidth = shmem->cursorSize[0 != shmem->frame].width;

    cursStart = (saveRect.miny - shmem->cursorRect.miny) * cursorWidth +
                (saveRect.minx - shmem->cursorRect.minx);

    if (inst->cursorBlitProc)
        inst->cursorBlitProc( inst,
                              (void *) shmem,
                              vramPtr,
                              cursStart,
                              inst->totalWidth - width,   /* vramRow */
                              cursorWidth - width,	/* cursRow */
                              width,
                              height);
}

// Description:	RemoveCursor erases the cursor by replacing the background
//		image that was saved by the previous call to DisplayCursor.
//		If the frame buffer is cacheable, flush at the end of the
//		drawing operation.

inline void IOFramebuffer::StdFBRemoveCursor( IOFramebuffer * inst )
{
    StdFBShmem_t *shmem;
    volatile unsigned char *vramPtr;	/* screen data pointer */
    unsigned int vramRow;
    int width;
    int height;

    shmem = GetShmem(inst);

    vramRow = inst->totalWidth;	/* Scanline width in pixels */

    vramPtr = inst->frameBuffer +
              (inst->rowBytes * (shmem->saveRect.miny - shmem->screenBounds.miny))
              + (inst->bytesPerPixel *
                 (shmem->saveRect.minx - shmem->screenBounds.minx));

    width = shmem->saveRect.maxx - shmem->saveRect.minx;
    height = shmem->saveRect.maxy - shmem->saveRect.miny;
    vramRow -= width;

    if (inst->cursorRemoveProc)
        inst->cursorRemoveProc( inst, (void *)shmem,
                                vramPtr, vramRow, width, height);
}

inline void IOFramebuffer::RemoveCursor( IOFramebuffer * inst )
{
    StdFBShmem_t *	shmem = GetShmem(inst);

    if (shmem->hardwareCursorActive)
    {
        Point *		hs;

        hs = &shmem->hotSpot[0 != shmem->frame];
        inst->_setCursorState(
            shmem->cursorLoc.x - hs->x - shmem->screenBounds.minx,
            shmem->cursorLoc.y - hs->y - shmem->screenBounds.miny, false );
    }
    else
        StdFBRemoveCursor(inst);
}

inline void IOFramebuffer::DisplayCursor( IOFramebuffer * inst )
{
    Point 	 *	hs;
    StdFBShmem_t *	shmem = GetShmem(inst);
    SInt32		x, y;

    hs = &shmem->hotSpot[0 != shmem->frame];
    x  = shmem->cursorLoc.x - hs->x;
    y  = shmem->cursorLoc.y - hs->y;

    if (shmem->hardwareCursorActive)
        inst->_setCursorState( x - shmem->screenBounds.minx,
                               y - shmem->screenBounds.miny, true );
    else
    {
        shmem->cursorRect.maxx = (shmem->cursorRect.minx = x)
                                 + shmem->cursorSize[0 != shmem->frame].width;
        shmem->cursorRect.maxy = (shmem->cursorRect.miny = y)
                                 + shmem->cursorSize[0 != shmem->frame].height;
        StdFBDisplayCursor(inst);
        shmem->oldCursorRect = shmem->cursorRect;
    }
}

inline void IOFramebuffer::SysHideCursor( IOFramebuffer * inst )
{
    if (!GetShmem(inst)->cursorShow++)
        RemoveCursor(inst);
}

inline void IOFramebuffer::SysShowCursor( IOFramebuffer * inst )
{
    StdFBShmem_t *shmem;

    shmem = GetShmem(inst);

    if (shmem->cursorShow)
        if (!--(shmem->cursorShow))
            DisplayCursor(inst);
}

inline void IOFramebuffer::CheckShield( IOFramebuffer * inst )
{
    Point *		hs;
    int 		intersect;
    Bounds 		tempRect;
    StdFBShmem_t *	shmem = GetShmem(inst);

    /* Calculate temp cursorRect */
    hs = &shmem->hotSpot[0 != shmem->frame];
    tempRect.maxx = (tempRect.minx = (shmem->cursorLoc).x - hs->x)
                    + shmem->cursorSize[0 != shmem->frame].width;
    tempRect.maxy = (tempRect.miny = (shmem->cursorLoc).y - hs->y)
                    + shmem->cursorSize[0 != shmem->frame].height;

    intersect = TOUCHBOUNDS(tempRect, shmem->shieldRect);
    if (intersect != shmem->shielded)
        (shmem->shielded = intersect) ?
        SysHideCursor(inst) : SysShowCursor(inst);
}

/**
 ** external methods
 **/

void IOFramebuffer::setupCursor( IOPixelInformation * info )
{
    StdFBShmem_t *		shmem	= GetShmem(this);
    volatile unsigned char *	bits;
    IOByteCount			cursorImageBytes, waitCursorImageBytes;

    rowBytes = info->bytesPerRow;
    totalWidth = (rowBytes * 8) / info->bitsPerPixel;
    bytesPerPixel = info->bitsPerPixel / 8;
    frameBuffer = (volatile unsigned char *) vramMap->getVirtualAddress();
    __private->framebufferWidth  = info->activeWidth;
    __private->framebufferHeight = info->activeHeight;

    if (shmem)
    {
        if ((shmem->screenBounds.maxx == shmem->screenBounds.minx)
                || (shmem->screenBounds.maxy == shmem->screenBounds.miny))
        {
            // a default if no one calls IOFBSetBounds()
            shmem->screenBounds.minx = 0;
            shmem->screenBounds.miny = 0;
            shmem->screenBounds.maxx = info->activeWidth;
            shmem->screenBounds.maxy = info->activeHeight;
        }

        shmem->cursorSize[0] = maxCursorSize;
        shmem->cursorSize[1] = __private->maxWaitCursorSize;
        shmem->cursorSize[2] = __private->maxWaitCursorSize;
        shmem->cursorSize[3] = __private->maxWaitCursorSize;

        cursorImageBytes = maxCursorSize.width * maxCursorSize.height
                           * bytesPerPixel;
        waitCursorImageBytes = __private->maxWaitCursorSize.width * __private->maxWaitCursorSize.height
                               * bytesPerPixel;
        bits = shmem->cursor;

        for (UInt32 i = 0; i < __private->numCursorFrames; i++)
        {
            __private->cursorFlags[i] = kIOFBCursorImageNew;
            __private->cursorImages[i] = bits;
            bits += i ? waitCursorImageBytes : cursorImageBytes;
        }
        if (info->bitsPerPixel <= 8)
        {
            for (UInt32 i = 0; i < __private->numCursorFrames; i++)
            {
                __private->cursorMasks[i] = bits;
                bits += i ? waitCursorImageBytes : cursorImageBytes;
            }
        }
        cursorSave = bits;
    }

    switch (info->bitsPerPixel)
    {
        case 8:
            if (colorConvert.t._bm256To38SampleTable
                    && colorConvert.t._bm38To256SampleTable)
            {
                cursorBlitProc = (CursorBlitProc) StdFBDisplayCursor8P;
                cursorRemoveProc = (CursorRemoveProc) StdFBRemoveCursor8;
            }
            break;
        case 16:
            if (colorConvert.t._bm34To35SampleTable
                    && colorConvert.t._bm35To34SampleTable)
            {
                cursorBlitProc = (CursorBlitProc) StdFBDisplayCursor555;
                cursorRemoveProc = (CursorRemoveProc) StdFBRemoveCursor16;
            }
            break;
        case 32:
            if (colorConvert.t._bm256To38SampleTable
                    && colorConvert.t._bm38To256SampleTable)
            {
                cursorBlitProc = (CursorBlitProc) StdFBDisplayCursor32Axxx;
                cursorRemoveProc = (CursorRemoveProc) StdFBRemoveCursor32;
            }
            break;
        default:
            IOLog("%s: can't do cursor at depth %ld\n",
                  getName(), info->bitsPerPixel);
            cursorBlitProc = (CursorBlitProc) NULL;
            cursorRemoveProc = (CursorRemoveProc) NULL;
            break;
    }
}

void IOFramebuffer::stopCursor( void )
{
    cursorBlitProc = (CursorBlitProc) NULL;
    cursorRemoveProc = (CursorRemoveProc) NULL;
}

IOReturn IOFramebuffer::extCreateSharedCursor(
    int version, int maxWidth, int maxWaitWidth )
{
    IOReturn ret;

    FBLOCK();

    ret = createSharedCursor( version, maxWidth, maxWidth );

    FBUNLOCK();

    return (ret);
}

IOReturn IOFramebuffer::extGetPixelInformation(
    IODisplayModeID displayMode, IOIndex depth,
    IOPixelAperture aperture, IOPixelInformation * pixelInfo )
{
    IOReturn ret;

    FBLOCK();

    ret = getPixelInformation(  displayMode, depth, aperture, pixelInfo );

    FBUNLOCK();

    return (ret);
}

IOReturn IOFramebuffer::extGetCurrentDisplayMode(
    IODisplayModeID * displayMode, IOIndex * depth )
{
    IOReturn ret;

    FBLOCK();

    ret = getCurrentDisplayMode( displayMode, depth );

    FBUNLOCK();

    return (ret);
}

IOReturn IOFramebuffer::extSetStartupDisplayMode(
    IODisplayModeID displayMode, IOIndex depth )
{
    IOReturn ret;

    FBLOCK();

    ret = setStartupDisplayMode( displayMode, depth );

    FBUNLOCK();

    return (ret);
}

IOReturn IOFramebuffer::extSetGammaTable(
    UInt32 channelCount, UInt32 dataCount,
    UInt32 dataWidth, void * data )
{
    IOReturn	ret = kIOReturnBadArgument;
    UInt32	expandCount = 0;
    IOByteCount	dataLen;
    UInt32      tryWidth;
    UInt8 *	table;
    bool	needAlloc;

    FBLOCK();

    do
    {
	if (!__private->desiredGammaDataWidth)
	{
	    __private->desiredGammaDataWidth = dataWidth;
	    __private->desiredGammaDataCount = dataCount;
	}
    
	if (dataWidth < __private->desiredGammaDataWidth)
	    continue;
    
	if (dataCount < __private->desiredGammaDataCount)
	{
	    expandCount = __private->desiredGammaDataCount / dataCount;
	    if ((expandCount * dataCount) == __private->desiredGammaDataCount)
	    {
		dataCount = __private->desiredGammaDataCount;
		expandCount--;
	    }
	    else
		expandCount = 0;
	}
	dataLen  = (dataWidth + 7) / 8;
	dataLen *= dataCount * channelCount;
	dataLen += __private->gammaHeaderSize;

	needAlloc = (0 == __private->gammaDataLen);
	if (!needAlloc)
	{
	    table = __private->gammaData;
	    if (__private->gammaDataLen != dataLen)
	    {
		IODelete(table, UInt8, __private->gammaDataLen);
		needAlloc = true;
	    }
	    __private->gammaDataLen = 0;
	}

	if (needAlloc)
	{
	    table = IONew(UInt8, dataLen);
	    if (!table)
	    {
		ret = kIOReturnNoMemory;
		continue;
	    }
	    __private->gammaData     = table;
	}
    
	__private->gammaChannelCount = channelCount;
	__private->gammaDataCount    = dataCount;
    
	table += __private->gammaHeaderSize;
    
	tryWidth = __private->desiredGammaDataWidth;

	if (!expandCount && (tryWidth == dataWidth))
	    bcopy(data, table, dataLen - __private->gammaHeaderSize);
	else
	{
	    UInt32 pin, pt5, value;
    
	    pin = (1 << tryWidth) - 1;
	    pt5 = (1 << (dataWidth - tryWidth - 1));
    
	    for (UInt32 in = 0, out = 0; out < (dataCount * channelCount);)
	    {
		value = (((UInt16 *) data)[in++] + pt5) >> (dataWidth - tryWidth);
		if (value > pin)
		    value = pin;
		for (UInt32 i = 0; i <= expandCount; i++)
		{
		    if (tryWidth <= 8)
			((UInt8 *) table)[out++] = (value & 0xff);
		    else
			((UInt16 *) table)[out++] = value;
		}
	    }
	}
	__private->gammaDataWidth = tryWidth;
    
	if (__private->deferredCLUTSetEvent)
	{
	    __private->gammaDataLen = dataLen;
	    ret = kIOReturnSuccess;
	}
	else
	{
	    ret = setGammaTable( __private->gammaChannelCount, __private->gammaDataCount, 
				 __private->gammaDataWidth, __private->gammaData );
    
	    IODelete(__private->gammaData, UInt8, dataLen);
	}
    }
    while (false);

    FBUNLOCK();

    return (ret);
}

IOReturn IOFramebuffer::extSetCLUTWithEntries( UInt32 index, IOOptionBits options,
        IOColorEntry * colors, IOByteCount dataLen )
{
    IOReturn	ret = kIOReturnBadArgument;
    UInt8 *	table;
    bool	needAlloc;

    FBLOCK();

    if (__private->deferredCLUTSetEvent)
    {
	do
	{
	    needAlloc = (0 == __private->clutDataLen);
	    if (!needAlloc)
	    {
		if (index || (__private->clutDataLen != dataLen))
		{
		    checkDeferredCLUTSet();
		    needAlloc = true;
		}
		__private->clutDataLen = 0;
	    }
    
	    if (needAlloc)
	    {
		table = IONew(UInt8, dataLen);
		if (!table)
		{
		    ret = kIOReturnNoMemory;
		    continue;
		}
		__private->clutData = table;
	    }
	
	    __private->clutIndex   = index;
	    __private->clutOptions = options;
	    __private->clutDataLen = dataLen;
    
	    bcopy( colors, table, dataLen);

	    ret = kIOReturnSuccess;
	}
	while (false);
    }
    else
	ret = setCLUTWithEntries( colors, index,
				  dataLen / sizeof( IOColorEntry), options );

    FBUNLOCK();

    return (ret);
}

void IOFramebuffer::deferredCLUTSetInterrupt( OSObject * owner,
					      IOInterruptEventSource * evtSrc, int intCount )
{
    IOFramebuffer *	self = (IOFramebuffer *) owner;

    self->checkDeferredCLUTSet();
}

void IOFramebuffer::checkDeferredCLUTSet( void )
{
    IOReturn    ret;
    IOByteCount gammaLen = __private->gammaDataLen;
    IOByteCount clutLen  = __private->clutDataLen;

    if( !gammaLen && !clutLen)
	return;

    __private->gammaDataLen = 0;
    __private->clutDataLen  = 0;

    if (gammaLen)
    {
	ret = setGammaTable( __private->gammaChannelCount, __private->gammaDataCount, 
			     __private->gammaDataWidth, __private->gammaData );

	IODelete(__private->gammaData, UInt8, gammaLen);
    }

    if (clutLen)
    {
	ret = setCLUTWithEntries( (IOColorEntry *) __private->clutData, __private->clutIndex,
				  clutLen / sizeof( IOColorEntry), __private->clutOptions );

	IODelete(__private->clutData, UInt8, clutLen);
    }
}

IOReturn IOFramebuffer::createSharedCursor(
    int version, int maxWidth, int maxWaitWidth )
{
    StdFBShmem_t *	shmem;
    UInt32		shmemVersion;
    IOByteCount		size, maxImageSize, maxWaitImageSize;
    UInt32		numCursorFrames;

    DEBG(thisIndex, " vers = %08x, %d x %d\n",
         version, maxWidth, maxWaitWidth);

    shmemVersion = version & kIOFBShmemVersionMask;

    if (shmemVersion == kIOFBTenPtTwoShmemVersion)
    {
        numCursorFrames = (kIOFBShmemCursorNumFramesMask & version) >> kIOFBShmemCursorNumFramesShift;

        setProperty(kIOFBWaitCursorFramesKey, (numCursorFrames - 1), 32);
        setProperty(kIOFBWaitCursorPeriodKey, 33333333, 32);	/* 30 fps */
    }
    else if (shmemVersion == kIOFBTenPtOneShmemVersion)
    {
        numCursorFrames = 4;
    }
    else
        return (kIOReturnUnsupported);

    shmemClientVersion = shmemVersion;

    if (__private->cursorFlags)
    {
        IODelete( __private->cursorFlags, UInt8, __private->numCursorFrames );
        __private->cursorFlags = 0;
    }
    if (__private->cursorImages)
    {
        IODelete( __private->cursorImages, volatile unsigned char *, __private->numCursorFrames );
        __private->cursorImages = 0;
    }
    if (__private->cursorMasks)
    {
        IODelete( __private->cursorMasks, volatile unsigned char *, __private->numCursorFrames );
        __private->cursorMasks = 0;
    }
    __private->numCursorFrames = numCursorFrames;
    __private->cursorFlags     = IONew( UInt8, numCursorFrames );
    __private->cursorImages    = IONew( volatile unsigned char *, numCursorFrames );
    __private->cursorMasks     = IONew( volatile unsigned char *, numCursorFrames );

    if (!__private->cursorFlags || !__private->cursorImages || !__private->cursorMasks)
        return (kIOReturnNoMemory);

    maxImageSize = (maxWidth * maxWidth * kIOFBMaxCursorDepth) / 8;
    maxWaitImageSize = (maxWaitWidth * maxWaitWidth * kIOFBMaxCursorDepth) / 8;

    size = sizeof( StdFBShmem_t)
           + maxImageSize
           + max(maxImageSize, maxWaitImageSize)
           + ((numCursorFrames - 1) * maxWaitImageSize);

    if (!sharedCursor || (size != sharedCursor->getLength()))
    {
        IOBufferMemoryDescriptor * newDesc;

        priv = 0;
        newDesc = IOBufferMemoryDescriptor::withOptions(
                      kIODirectionNone | kIOMemoryKernelUserShared, size );
        if (!newDesc)
            return (kIOReturnNoMemory);

        if (sharedCursor)
            sharedCursor->release();
        sharedCursor = newDesc;
    }
    shmem = (StdFBShmem_t *) sharedCursor->getBytesNoCopy();
    priv = shmem;

    // Init shared memory area
    bzero( shmem, size );
    shmem->version = shmemClientVersion;
    shmem->structSize = size;
    shmem->cursorShow = 1;
    shmem->hardwareCursorCapable = haveHWCursor;
    for (UInt32 i = 0; i < numCursorFrames; i++)
        __private->cursorFlags[i] = kIOFBCursorImageNew;

    maxCursorSize.width = maxWidth;
    maxCursorSize.height = maxWidth;
    __private->maxWaitCursorSize.width = maxWaitWidth;
    __private->maxWaitCursorSize.height = maxWaitWidth;

    doSetup( false );

    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::setBoundingRect( Bounds * bounds )
{
    StdFBShmem_t *shmem;

    shmem = GetShmem(this);
    if (NULL == shmem)
        return (kIOReturnUnsupported);

    shmem->screenBounds = *bounds;

    return (kIOReturnSuccess);
}

/**
 ** IOUserClient methods
 **/

IOReturn IOFramebuffer::newUserClient(  task_t		owningTask,
                                        void * 		security_id,
                                        UInt32  	type,
                                        IOUserClient **	handler )

{
    IOReturn		err = kIOReturnSuccess;
    IOUserClient *	newConnect = 0;
    IOUserClient *	theConnect = 0;

    switch (type)
    {
        case kIOFBServerConnectType:
            if (serverConnect)
                err = kIOReturnExclusiveAccess;
            else
            {
                if (this == gIOFBConsoleFramebuffer)
                    getPlatform()->setConsoleInfo( 0, kPEReleaseScreen);

                err = open();
                if (kIOReturnSuccess == err)
                    newConnect = IOFramebufferUserClient::withTask(owningTask);
            }
            break;

        case kIOFBSharedConnectType:
            if (sharedConnect)
            {
                theConnect = sharedConnect;
                theConnect->retain();
            }
            else if (serverConnect)
                newConnect = IOFramebufferSharedUserClient::withTask(owningTask);
            else
                err = kIOReturnNotOpen;
            break;

        default:
            err = kIOReturnBadArgument;
    }

    if (newConnect)
    {
        if ((false == newConnect->attach(this))
                || (false == newConnect->start(this)))
        {
            newConnect->detach( this );
            newConnect->release();
        }
        else
            theConnect = newConnect;
    }

    *handler = theConnect;
    return (err);
}

IOReturn IOFramebuffer::extGetDisplayModeCount( IOItemCount * count )
{
    FBLOCK();

    *count = dead ? 0 : getDisplayModeCount();

    FBUNLOCK();

    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::extGetDisplayModes( IODisplayModeID * allModes, IOByteCount * size )
{
    IOReturn		err;
    IOByteCount		outSize;

    FBLOCK();

    outSize = getDisplayModeCount() * sizeof( IODisplayModeID);

    if (*size >= outSize)
    {
        *size = outSize;
        err = getDisplayModes( allModes );
    }
    else
        err = kIOReturnBadArgument;

    FBUNLOCK();

    return (err);
}

IOReturn IOFramebuffer::extGetVRAMMapOffset( IOPixelAperture /* aperture */,
        IOByteCount * offset )
{
    FBLOCK();

    *offset = vramMapOffset;

    FBUNLOCK();

    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::extSetBounds( Bounds * bounds )
{
    StdFBShmem_t *shmem;

    FBLOCK();

    shmem = GetShmem(this);
    if (shmem)
        shmem->screenBounds = *bounds;

    FBUNLOCK();

    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::extValidateDetailedTiming(
    void * description, void * outDescription,
    IOByteCount inSize, IOByteCount * outSize )
{
    IOReturn	err;

    if (*outSize != inSize)
        return (kIOReturnBadArgument);

    FBLOCK();

    err = validateDetailedTiming( description, inSize );

    if (kIOReturnSuccess == err)
        bcopy( description, outDescription, inSize );

    FBUNLOCK();

    return (err);
}


IOReturn IOFramebuffer::extSetColorConvertTable( UInt32 select,
        UInt8 * data, IOByteCount length )
{
    static const IOByteCount checkLength[] = {
                16 * sizeof( UInt8),
                32 * sizeof( UInt8),
                256 * sizeof( UInt32),
                5 * 256 * sizeof( UInt8) };

    IOReturn		err;
    UInt8 *		table;
    IODisplayModeID	mode;
    IOIndex		depth;
    IOPixelInformation	info;

    if (select > 3)
        return (kIOReturnBadArgument);

    if (length != checkLength[select])
        return (kIOReturnBadArgument);

    FBLOCK();

    do
    {
        err = kIOReturnNoMemory;
        table = colorConvert.tables[select];
        if (0 == table)
        {
            table = (UInt8 *) IOMalloc( length );
            colorConvert.tables[select] = table;
        }
        if (!table)
            continue;

        bcopy( data, table, length );
        if (select == 3)
            white = data[data[255] + data[511] + data[767] + 1024];

        if ((NULL == cursorBlitProc)
                && colorConvert.tables[0] && colorConvert.tables[1]
                && colorConvert.tables[2] && colorConvert.tables[3]
                && vramMap
                && (kIOReturnSuccess == getCurrentDisplayMode(&mode, &depth))
                && (kIOReturnSuccess == getPixelInformation(mode, depth, kIOFBSystemAperture, &info)))
            setupCursor( &info );

        err = kIOReturnSuccess;
    }
    while (false);

    FBUNLOCK();

    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool IOFramebuffer::requestTerminate( IOService * provider, IOOptionBits options )
{
    FBLOCK();

    if (opened)
    {
	if (!gRunawayFramebuffers)
	    gRunawayFramebuffers = OSArray::withCapacity(4);
	if (gRunawayFramebuffers)
	    gRunawayFramebuffers->setObject(this);
    }

    FBUNLOCK();

    return (false);
}

void IOFramebuffer::stop( IOService * provider )
{

    if (opened)
    {
	FBLOCK();

	dead = true;

	for (UInt32 i = 0; i < pm_vars->theNumberOfPowerStates; i++)
	    pm_vars->thePowerStates[i].capabilityFlags &= ~kIOPMPreventSystemSleep;

	setAttribute( kIOSystemPowerAttribute, kIOMessageSystemWillPowerOff );

	FBUNLOCK();

	temporaryPowerClampOn();	// only to clear out kIOPMPreventSystemSleep
	PMstop();

	connectChangeInterrupt(this, 0);
    }

    return (super::stop(provider));
}

void IOFramebuffer::free()
{
    if (vblSemaphore)
        semaphore_destroy(kernel_task, vblSemaphore);
    if (__private)
    {
        IODelete( __private, IOFramebufferPrivate, 1 );
        __private = 0;
    }
    super::free();
}

IOService * IOFramebuffer::probe( IOService * provider, SInt32 * score )
{
    IOFramebuffer * replace = 0;

    if (gRunawayFramebuffers)
    {
        FBLOCK();

	replace = (IOFramebuffer *) gRunawayFramebuffers->getObject(0);
	gRunawayFramebuffers->removeObject(0);

	FBUNLOCK();
    }

    return (replace ? replace : this);
}

void IOFramebuffer::initialize()
{
}

bool IOFramebuffer::start( IOService * provider )
{
    bool runaway;

    if (!super::start(provider))
        return (false);

    runaway = (__private != 0);

    if (!__private)
    {
	__private = IONew( IOFramebufferPrivate, 1 );
	if (!__private)
	    return (false);
	bzero( __private, sizeof(IOFramebufferPrivate) );

	userAccessRanges = OSArray::withCapacity( 1 );
	if (!userAccessRanges)
	    return (false);
    
	serverMsg = IOMalloc( sizeof (mach_msg_header_t) );
	if (!serverMsg)
	    return (false);
	bzero( serverMsg, sizeof (mach_msg_header_t));
    }

    // initialize superclass power management variables
    PMinit();
    // attach into the power management hierarchy
    provider->joinPMtree(this);

    if (runaway)
    {
	IOReturn err;

	FBLOCK();
	err = enableController();
	if (kIOReturnSuccess == err)
	{
	    opened = true;
	    dead = false;
	    isUsable = true;
            configPending = serverState ? 0 : 1;

	    connectChangeInterrupt(this, 0);
	}
	FBUNLOCK();
    }
    else
	registerService();

    return (true);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

//
// BEGIN:	Implementation of the evScreen protocol
//

void IOFramebuffer::hideCursor( void )
{
    StdFBShmem_t *shmem = GetShmem(this);

    SETSEMA(shmem);
    SysHideCursor(this);
    CLEARSEMA(shmem, this);
}

void IOFramebuffer::deferredMoveCursor( IOFramebuffer * inst )
{
    StdFBShmem_t *	shmem = GetShmem(inst);
    IOReturn		err = kIOReturnSuccess;

    if (shmem->hardwareCursorActive)
    {
        if (shmem->cursorObscured)
        {
            shmem->cursorObscured = 0;
            if (shmem->cursorShow)
                --shmem->cursorShow;
        }
        if (shmem->hardwareCursorShields && shmem->shieldFlag)
            CheckShield(inst);
        if (!shmem->cursorShow)
        {
            Point * hs;
            hs = &shmem->hotSpot[0 != shmem->frame];
            err = inst->_setCursorState(
                      shmem->cursorLoc.x - hs->x - shmem->screenBounds.minx,
                      shmem->cursorLoc.y - hs->y - shmem->screenBounds.miny, true );
        }
    }
    else
    {
        if (!shmem->cursorShow++)
            RemoveCursor(inst);
        if (shmem->cursorObscured)
        {
            shmem->cursorObscured = 0;
            if (shmem->cursorShow)
                --shmem->cursorShow;
        }
        if (shmem->shieldFlag)
            CheckShield(inst);
        if (shmem->cursorShow)
            if (!--shmem->cursorShow)
                DisplayCursor(inst);

        inst->flushCursor();

	if (inst->__private->cursorPanning)
        {
            Point * hs;
            hs = &shmem->hotSpot[0 != shmem->frame];
            err = inst->setCursorState(
                      shmem->cursorLoc.x - hs->x - shmem->screenBounds.minx,
                      shmem->cursorLoc.y - hs->y - shmem->screenBounds.miny, false );
        }
    }
    inst->needCursorService = (kIOReturnBusy == err);
}

void IOFramebuffer::cursorWork( OSObject * p0, IOInterruptEventSource * evtSrc, int intCount )
{
    IOFramebuffer *		inst = (IOFramebuffer *) p0;
    StdFBShmem_t *		shmem = GetShmem(inst);
    struct IOFramebufferPrivate * __private = inst->__private;
    IOFBCursorControlAttribute	* cursorControl = &__private->cursorControl;
    IOReturn			ret;
    IOHardwareCursorDescriptor	desc;

    IOOptionBits todo = inst->__private->cursorToDo;

    while (todo)
    {
        if (2 & todo)
        {
            desc.majorVersion 	= kHardwareCursorDescriptorMajorVersion;
            desc.minorVersion 	= kHardwareCursorDescriptorMinorVersion;
            desc.height 	= shmem->cursorSize[0 != __private->framePending].height;
            desc.width 		= shmem->cursorSize[0 != __private->framePending].width;
            desc.bitDepth 	= inst->bytesPerPixel * 8;
            desc.maskBitDepth 	= 0;
            desc.colorEncodings = 0;
            desc.flags 		= 0;
            desc.supportedSpecialEncodings = kTransparentEncodedPixel;

            ret = (*cursorControl->callouts->setCursorImage) (
                      cursorControl->self, cursorControl->ref,
                      &desc, (void *) __private->framePending );
        }
        if (1 & todo)
            ret = (*cursorControl->callouts->setCursorState) (
                      cursorControl->self, cursorControl->ref,
                      __private->xPending, __private->yPending, __private->visiblePending );

        todo = __private->cursorToDo & ~todo;
        __private->cursorToDo = todo;
    }
}

IOOptionBits IOFramebuffer::_setCursorImage( UInt32 frame )
{
    IOOptionBits flags;

    flags = (kIOReturnSuccess == setCursorImage( (void *) frame ))
            ? kIOFBHardwareCursorActive : 0;

    if (!flags && __private->cursorThread && (bytesPerPixel >= 2))
    {
        __private->framePending = frame;
        __private->cursorToDo |= 2;
        flags = kIOFBHardwareCursorActive | kIOFBHardwareCursorInVRAM;
    }

    return (flags);
}

IOReturn IOFramebuffer::_setCursorState( SInt32 x, SInt32 y, bool visible )
{
    StdFBShmem_t *shmem = GetShmem(this);
    IOReturn ret = kIOReturnUnsupported;

    x -= __private->cursorHotSpotAdjust[ 0 ].x;
    y -= __private->cursorHotSpotAdjust[ 0 ].y;

    if (kIOFBHardwareCursorActive == shmem->hardwareCursorActive)
        ret = setCursorState( x, y, visible );
    else if (__private->cursorThread)
    {
        __private->cursorToDo |= 1;
        __private->xPending = x;
        __private->yPending = y;
        __private->visiblePending = visible;
    }

    return (ret);
}

void IOFramebuffer::moveCursor( Point * cursorLoc, int frame )
{
    nextCursorLoc = *cursorLoc;
    nextCursorFrame = frame;
    needCursorService  = true;
    UInt32 hwCursorActive;

    StdFBShmem_t *shmem = GetShmem(this);

    SETSEMA(shmem);

    if (frame != shmem->frame)
    {
        if (__private->cursorFlags[frame] && pagingState)
        {
            hwCursorActive = _setCursorImage( frame );
            __private->cursorFlags[frame] = hwCursorActive ? kIOFBCursorHWCapable : 0;
        }
        else
            hwCursorActive = 0;

        shmem->frame = frame;
        if (shmem->hardwareCursorActive != hwCursorActive)
        {
            SysHideCursor( this );
            shmem->hardwareCursorActive = hwCursorActive;
            if (shmem->shieldFlag
                    && ((0 == hwCursorActive) || (shmem->hardwareCursorShields)))
                CheckShield(this);
            SysShowCursor( this );
        }
    }

    if (!haveVBLService)
    {
        shmem->cursorLoc = *cursorLoc;
        shmem->frame = frame;
        deferredMoveCursor( this );
    }

    CLEARSEMA(shmem, this);
}

void IOFramebuffer::handleVBL( IOFramebuffer * inst, void * ref )
{
    StdFBShmem_t *	shmem = GetShmem(inst);
    AbsoluteTime	now;

    if (!shmem)
        return ;

    clock_get_uptime( &now );
    shmem->vblDelta = now;
    SUB_ABSOLUTETIME( &shmem->vblDelta, &shmem->vblTime );
    shmem->vblTime = now;

    KERNEL_DEBUG(0xc000030 | DBG_FUNC_NONE,
                 shmem->vblDelta.hi, shmem->vblDelta.lo, 0, 0, 0);

    if (inst->vblSemaphore)
        semaphore_signal_all(inst->vblSemaphore);

    SETSEMA(shmem);

    if (inst->needCursorService)
    {
        shmem->cursorLoc = inst->nextCursorLoc;
        shmem->frame = inst->nextCursorFrame;
        deferredMoveCursor( inst );
    }

    CLEARSEMA(shmem, inst);

    if (inst->__private->deferredCLUTSetEvent 
     && (inst->__private->gammaDataLen || inst->__private->clutDataLen))
	inst->__private->deferredCLUTSetEvent->interruptOccurred(0, 0, 0);
}

void IOFramebuffer::showCursor( Point * cursorLoc, int frame )
{
    StdFBShmem_t *shmem;
    UInt32 hwCursorActive;

    shmem = GetShmem(this);
    SETSEMA(shmem);

    if (frame != shmem->frame)
    {
        if (__private->cursorFlags[frame])
        {
            hwCursorActive = _setCursorImage( frame );
            __private->cursorFlags[frame] = hwCursorActive ? kIOFBCursorHWCapable : 0;
        }
        else
            hwCursorActive = 0;
        shmem->frame = frame;
        shmem->hardwareCursorActive = hwCursorActive;
    }

    shmem->cursorLoc = *cursorLoc;
    if (shmem->shieldFlag
            && ((0 == shmem->hardwareCursorActive) || (shmem->hardwareCursorShields)))
        CheckShield(this);

    SysShowCursor(this);
    CLEARSEMA(shmem, this);
}

void IOFramebuffer::resetCursor( void )
{
    StdFBShmem_t *	shmem;
    int			frame;

    shmem = GetShmem(this);
    //    hwCursorLoaded = false;
    if (!shmem)
        return ;

    shmem->hardwareCursorActive = 0;
    frame = shmem->frame;
    shmem->frame = frame ^ 1;
    showCursor( &shmem->cursorLoc, frame );
}

void IOFramebuffer::getVBLTime( AbsoluteTime * time, AbsoluteTime * delta )
{
    StdFBShmem_t *shmem;

    shmem = GetShmem(this);
    if (shmem)
    {
        *time = shmem->vblTime;
        *delta = shmem->vblDelta;
    }
    else
        time->hi = time->lo = 0;
}

void IOFramebuffer::getBoundingRect( Bounds ** bounds )
{
    StdFBShmem_t *shmem;

    shmem = GetShmem(this);
    if (NULL == shmem)
        *bounds = NULL;
    else
        *bounds = &shmem->screenBounds;
}

//
// END:		Implementation of the evScreen protocol
//

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IOFramebuffer::getNotificationSemaphore(
    IOSelect interruptType, semaphore_t * semaphore )
{
    kern_return_t	kr;
    semaphore_t		sema;

    if (interruptType != kIOFBVBLInterruptType)
        return (kIOReturnUnsupported);

    if (!haveVBLService)
        return (kIOReturnNoResources);

    if (MACH_PORT_NULL == vblSemaphore)
    {
        kr = semaphore_create(kernel_task, &sema, SYNC_POLICY_FIFO, 0);
        if (kr == KERN_SUCCESS)
            vblSemaphore = sema;
    }
    else
        kr = KERN_SUCCESS;

    if (kr == KERN_SUCCESS)
        *semaphore = vblSemaphore;

    return (kr);
}

IOReturn IOFramebuffer::extSetCursorVisible( bool visible )
{
    IOReturn		err;
    Point *		hs;
    StdFBShmem_t *	shmem;

    FBLOCK();

    shmem = GetShmem(this);
    if (shmem->hardwareCursorActive)
    {
        hs = &shmem->hotSpot[0 != shmem->frame];
        err = _setCursorState(
                  shmem->cursorLoc.x - hs->x - shmem->screenBounds.minx,
                  shmem->cursorLoc.y - hs->y - shmem->screenBounds.miny,
                  visible );

        if (__private->cursorToDo)
            KICK_CURSOR(__private->cursorThread);
    }
    else
        err = kIOReturnBadArgument;

    FBUNLOCK();

    return (err);
}

IOReturn IOFramebuffer::extSetCursorPosition( SInt32 x, SInt32 y )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::extSetNewCursor( void * cursor, IOIndex frame,
        IOOptionBits options )
{
    StdFBShmem_t *	shmem;
    IOReturn		err;
    UInt32		hwCursorActive;

    FBLOCK();

    shmem = GetShmem(this);
    // assumes called with cursorSema held
    if (cursor || options || (((UInt32) frame) >= __private->numCursorFrames))
        err = kIOReturnBadArgument;
    else
    {
        if ((shmem->cursorSize[0 != frame].width > maxCursorSize.width)
                || (shmem->cursorSize[0 != frame].height > maxCursorSize.height))
            err = kIOReturnBadArgument;

        else if (haveHWCursor)
        {
            if (frame == shmem->frame)
            {
                hwCursorActive = _setCursorImage( frame );
                shmem->hardwareCursorActive = hwCursorActive;
                __private->cursorFlags[frame] = hwCursorActive ? kIOFBCursorHWCapable : 0;
            }
            else
            {
                __private->cursorFlags[frame] = kIOFBCursorImageNew;
            }
            err = kIOReturnSuccess;		// I guess
        }
        else
            err = kIOReturnUnsupported;
    }
    if (__private->cursorToDo)
        KICK_CURSOR(__private->cursorThread);

    FBUNLOCK();

    return (err);
}

bool IOFramebuffer::convertCursorImage( void * cursorImage,
                                        IOHardwareCursorDescriptor * hwDesc,
                                        IOHardwareCursorInfo * hwCursorInfo )
{
    StdFBShmem_t *		shmem = GetShmem(this);
    UInt8 *			dataOut = hwCursorInfo->hardwareCursorData;
    IOColorEntry *		clut = hwCursorInfo->colorMap;
    UInt32			maxColors = hwDesc->numColors;
    int				frame = (int) cursorImage;

    volatile unsigned short *	cursPtr16;
    volatile unsigned int *	cursPtr32;
    SInt32 			x, lastx, y, lasty;
    UInt32			width, height, lineBytes;
    UInt32			index, numColors = 0;
    UInt32			alpha, red, green, blue;
    UInt16			s16;
    UInt32			s32;
    UInt32			pixel = 0;
    UInt32			data = 0;
    UInt32			bits = 0;
    bool			ok = true;
    bool			isDirect;

    if (__private->testingCursor)
    {
        IOHardwareCursorDescriptor copy;

        if ((hwDesc->numColors == 0) && (hwDesc->bitDepth > 8) && (hwDesc->bitDepth < 32))
        {
            copy = *hwDesc;
            hwDesc = &copy;
            copy.bitDepth = 32;
        }

	OSData * data = OSData::withBytes( hwDesc, sizeof(IOHardwareCursorDescriptor) );
        if (data)
        {
            __private->cursorAttributes->setObject( data );
            data->release();
        }

        return (false);
    }
    else if (!hwCursorInfo || !hwCursorInfo->hardwareCursorData)
        return (false);

    assert( frame < __private->numCursorFrames );

    if (bytesPerPixel == 4)
    {
        cursPtr32 = (volatile unsigned int *) __private->cursorImages[ frame ];
        cursPtr16 = 0;
    }
    else if (bytesPerPixel == 2)
    {
        cursPtr32 = 0;
        cursPtr16 = (volatile unsigned short *) __private->cursorImages[ frame ];
    }
    else
        return (false);

    x = shmem->cursorSize[0 != frame].width;
    y = shmem->cursorSize[0 != frame].height;
    if ((x > (SInt32) hwDesc->width) || (y > (SInt32) hwDesc->height))
        return (false);
    isDirect = (hwDesc->bitDepth > 8);
    if (isDirect && (hwDesc->bitDepth != 32) && (hwDesc->bitDepth != 16))
        return (false);

    width  = hwDesc->width;
    height = hwDesc->height;

    // matrox workaround - 2979661
    if ((maxColors > 1) && (&clut[1] == (IOColorEntry *) hwCursorInfo))
        width = height = 16;
    // --

    SInt32 adjX = 4 - shmem->hotSpot[ 0 != frame ].x;
    SInt32 adjY = 4 - shmem->hotSpot[ 0 != frame ].y;
    if ((adjX < 0) || ((UInt32)(x + adjX) > width))
        adjX = 0;
    else
        x += adjX;
    if ((adjY < 0) || ((UInt32)(y + adjY) > height))
        adjY = 0;

    __private->cursorHotSpotAdjust[ 0 != frame ].x = adjX;
    __private->cursorHotSpotAdjust[ 0 != frame ].y = adjY;

    while ((width >> 1) >= (UInt32) x)
        width >>= 1;
    while ((UInt32)(height >> 1) >= (UInt32)(y + adjY))
        height >>= 1;

    hwCursorInfo->cursorWidth  = width;
    hwCursorInfo->cursorHeight = height;

    lastx = x - width - 1;

    if (isDirect && adjY)
    {
        lineBytes = width * (hwDesc->bitDepth >> 3);
        // top lines
        bzero_nc( dataOut, adjY * lineBytes );
        dataOut += adjY * lineBytes;
        // bottom lines
        adjY    = height - shmem->cursorSize[0 != frame].height - adjY;
        lasty   = -1;
    }
    else
    {
        y += adjY;
        lasty = y - height - 1;
    }

    while (ok && (--y != lasty))
    {
        x = shmem->cursorSize[0 != frame].width + adjX;
        while (ok && (--x != lastx))
        {
            if ((x < 0)
                    || (y < 0)
                    || (x >= shmem->cursorSize[0 != frame].width)
                    || (y >= shmem->cursorSize[0 != frame].height))
                alpha = red = green = blue = 0;

            else if (cursPtr32)
            {
                s32 = *(cursPtr32++);
                alpha = (s32 >> 24) & 0xff;
                red = (s32 >> 16) & 0xff;
                green = (s32 >> 8) & 0xff;
                blue = (s32) & 0xff;
            }
            else
            {
#define RMASK16	0xF000
#define GMASK16	0x0F00
#define BMASK16	0x00F0
#define AMASK16	0x000F
                s16 = *(cursPtr16++);
                alpha = s16 & AMASK16;
                alpha |= (alpha << 4);
                red = (s16 & RMASK16) >> 8;
                red |= (red >> 4);
                green = (s16 & GMASK16) >> 4;
                green |= (green >> 4);
                blue = s16 & BMASK16;
                blue |= (blue >> 4);
            }

            if (isDirect)
            {
                if (alpha == 0)
                {
                    if (0xff == (red & green & blue))
                    {
                        /* Transparent white area.  Invert dst. */
                        if (kInvertingEncodedPixel
                                & hwDesc->supportedSpecialEncodings)
                            pixel = hwDesc->specialEncodings[kInvertingEncoding];
                        else
                            ok = false;
                    }
                    else
                        pixel = 0;

                    if (hwDesc->bitDepth == 32)
                        *((UInt32 *)dataOut)++ = pixel;
                    else
                        *((UInt16 *)dataOut)++ = pixel;
                }
                else
                {
                    if (0xff != alpha)
                    {
                        red   = 0xff * red   / alpha;
                        green = 0xff * green / alpha;
                        blue  = 0xff * blue  / alpha;
                    }
                    if (hwDesc->bitDepth == 32)
                    {
                        pixel =   (alpha << 24)
                                  | ((red   & 0xff) << 16)
                                  | ((green & 0xff) << 8)
                                  | (blue   & 0xff);

                        *((UInt32 *)dataOut)++ = pixel;
                    }
                    else
                    {
                        pixel =   ((alpha & 0xf0) << 8)
                                  | ((red   & 0xf0) << 4)
                                  | ((green & 0xf0) << 0)
                                  | ((blue  & 0xf0) >> 4);

                        *((UInt16 *)dataOut)++ = pixel;
                    }
                }
            }
            else
            {
                /* Indexed pixels */

                if (alpha == 0)
                {
                    if (0 == (red | green | blue))
                    {
                        /* Transparent black area.  Leave dst as is. */
                        if (kTransparentEncodedPixel
                                & hwDesc->supportedSpecialEncodings)
                            pixel = hwDesc->specialEncodings[kTransparentEncoding];
                        else
                            ok = false;
                    }
                    else if (0xff == (red & green & blue))
                    {
                        /* Transparent white area.  Invert dst. */
                        if (kInvertingEncodedPixel
                                & hwDesc->supportedSpecialEncodings)
                            pixel = hwDesc->specialEncodings[kInvertingEncoding];
                        else
                            ok = false;
                    }
                    else
                        ok = false;
                }
                else if (alpha == 0xff)
                {
                    red   |= (red << 8);
                    green |= (green << 8);
                    blue  |= (blue << 8);

                    /* Opaque cursor pixel.  Mark it. */
                    for (index = 0; index < numColors; index++)
                    {
                        if ((red   == clut[ index ].red)
                                && (green == clut[ index ].green)
                                && (blue  == clut[ index ].blue))
                        {
                            pixel = clut[ index ].index;
                            break;
                        }
                    }
                    if (index == numColors)
                    {
                        ok = (numColors < maxColors);
                        if (ok)
                        {
                            pixel = hwDesc->colorEncodings[ numColors++ ];
                            clut[ index ].red   = red;
                            clut[ index ].green = green;
                            clut[ index ].blue  = blue;
                            clut[ index ].index = pixel;
                        }
                    }
                }
                else
                {
                    /* Alpha is not 0 or 1.0.  Sover the cursor. */
                    ok = false;
                    break;
                }
                data <<= hwDesc->bitDepth;
                data |= pixel;
                bits += hwDesc->bitDepth;
                if (0 == (bits & 31))
                    *((UInt32 *)dataOut)++ = data;
            }
        } /* x */
    } /* y */

    if (ok && isDirect && adjY)
    {
        // bottom lines
        bzero_nc( dataOut, adjY * lineBytes );
        dataOut += adjY * lineBytes;
    }

#if 0
    if (ok)
    {
        static UInt32 lastWidth;
        static UInt32 lastHeight;

        if ((width != lastWidth) || (height != lastHeight))
        {
            lastWidth = width;
            lastHeight = height;
            IOLog("[%d,%d]", width, height);
        }

        if (((UInt32)(dataOut - hwCursorInfo->hardwareCursorData)
                != ((hwCursorInfo->cursorHeight * hwCursorInfo->cursorWidth * hwDesc->bitDepth) >> 3)))
            IOLog("dataOut %p, %p @ %d\n", dataOut, hwCursorInfo->hardwareCursorData, hwDesc->bitDepth );
    }
#endif

    return (ok);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// Apple standard 8-bit CLUT

#if 1
extern UInt8 appleClut8[ 256 * 3 ];
#else
UInt8 appleClut8[ 256 * 3 ] =
{
    // 00
    0xFF,0xFF,0xFF, 0xFF,0xFF,0xCC,	0xFF,0xFF,0x99,	0xFF,0xFF,0x66,
    0xFF,0xFF,0x33, 0xFF,0xFF,0x00,	0xFF,0xCC,0xFF,	0xFF,0xCC,0xCC,
    0xFF,0xCC,0x99, 0xFF,0xCC,0x66,	0xFF,0xCC,0x33,	0xFF,0xCC,0x00,
    0xFF,0x99,0xFF, 0xFF,0x99,0xCC,	0xFF,0x99,0x99,	0xFF,0x99,0x66,
    // 10
    0xFF,0x99,0x33, 0xFF,0x99,0x00,	0xFF,0x66,0xFF,	0xFF,0x66,0xCC,
    0xFF,0x66,0x99, 0xFF,0x66,0x66,	0xFF,0x66,0x33,	0xFF,0x66,0x00,
    0xFF,0x33,0xFF, 0xFF,0x33,0xCC,	0xFF,0x33,0x99,	0xFF,0x33,0x66,
    0xFF,0x33,0x33, 0xFF,0x33,0x00,	0xFF,0x00,0xFF,	0xFF,0x00,0xCC,
    // 20
    0xFF,0x00,0x99, 0xFF,0x00,0x66,	0xFF,0x00,0x33,	0xFF,0x00,0x00,
    0xCC,0xFF,0xFF, 0xCC,0xFF,0xCC,	0xCC,0xFF,0x99,	0xCC,0xFF,0x66,
    0xCC,0xFF,0x33, 0xCC,0xFF,0x00,	0xCC,0xCC,0xFF,	0xCC,0xCC,0xCC,
    0xCC,0xCC,0x99, 0xCC,0xCC,0x66,	0xCC,0xCC,0x33,	0xCC,0xCC,0x00,
    // 30
    0xCC,0x99,0xFF, 0xCC,0x99,0xCC,	0xCC,0x99,0x99,	0xCC,0x99,0x66,
    0xCC,0x99,0x33, 0xCC,0x99,0x00,	0xCC,0x66,0xFF,	0xCC,0x66,0xCC,
    0xCC,0x66,0x99, 0xCC,0x66,0x66,	0xCC,0x66,0x33,	0xCC,0x66,0x00,
    0xCC,0x33,0xFF, 0xCC,0x33,0xCC,	0xCC,0x33,0x99,	0xCC,0x33,0x66,
    // 40
    0xCC,0x33,0x33, 0xCC,0x33,0x00,	0xCC,0x00,0xFF,	0xCC,0x00,0xCC,
    0xCC,0x00,0x99, 0xCC,0x00,0x66,	0xCC,0x00,0x33,	0xCC,0x00,0x00,
    0x99,0xFF,0xFF, 0x99,0xFF,0xCC,	0x99,0xFF,0x99,	0x99,0xFF,0x66,
    0x99,0xFF,0x33, 0x99,0xFF,0x00,	0x99,0xCC,0xFF,	0x99,0xCC,0xCC,
    // 50
    0x99,0xCC,0x99, 0x99,0xCC,0x66,	0x99,0xCC,0x33,	0x99,0xCC,0x00,
    0x99,0x99,0xFF, 0x99,0x99,0xCC,	0x99,0x99,0x99,	0x99,0x99,0x66,
    0x99,0x99,0x33, 0x99,0x99,0x00,	0x99,0x66,0xFF,	0x99,0x66,0xCC,
    0x99,0x66,0x99, 0x99,0x66,0x66,	0x99,0x66,0x33,	0x99,0x66,0x00,
    // 60
    0x99,0x33,0xFF, 0x99,0x33,0xCC,	0x99,0x33,0x99,	0x99,0x33,0x66,
    0x99,0x33,0x33, 0x99,0x33,0x00,	0x99,0x00,0xFF,	0x99,0x00,0xCC,
    0x99,0x00,0x99, 0x99,0x00,0x66,	0x99,0x00,0x33,	0x99,0x00,0x00,
    0x66,0xFF,0xFF, 0x66,0xFF,0xCC,	0x66,0xFF,0x99,	0x66,0xFF,0x66,
    // 70
    0x66,0xFF,0x33, 0x66,0xFF,0x00,	0x66,0xCC,0xFF,	0x66,0xCC,0xCC,
    0x66,0xCC,0x99, 0x66,0xCC,0x66,	0x66,0xCC,0x33,	0x66,0xCC,0x00,
    0x66,0x99,0xFF, 0x66,0x99,0xCC,	0x66,0x99,0x99,	0x66,0x99,0x66,
    0x66,0x99,0x33, 0x66,0x99,0x00,	0x66,0x66,0xFF,	0x66,0x66,0xCC,
    // 80
    0x66,0x66,0x99, 0x66,0x66,0x66,	0x66,0x66,0x33,	0x66,0x66,0x00,
    0x66,0x33,0xFF, 0x66,0x33,0xCC,	0x66,0x33,0x99,	0x66,0x33,0x66,
    0x66,0x33,0x33, 0x66,0x33,0x00,	0x66,0x00,0xFF,	0x66,0x00,0xCC,
    0x66,0x00,0x99, 0x66,0x00,0x66,	0x66,0x00,0x33,	0x66,0x00,0x00,
    // 90
    0x33,0xFF,0xFF, 0x33,0xFF,0xCC,	0x33,0xFF,0x99,	0x33,0xFF,0x66,
    0x33,0xFF,0x33, 0x33,0xFF,0x00,	0x33,0xCC,0xFF,	0x33,0xCC,0xCC,
    0x33,0xCC,0x99, 0x33,0xCC,0x66,	0x33,0xCC,0x33,	0x33,0xCC,0x00,
    0x33,0x99,0xFF, 0x33,0x99,0xCC,	0x33,0x99,0x99,	0x33,0x99,0x66,
    // a0
    0x33,0x99,0x33, 0x33,0x99,0x00,	0x33,0x66,0xFF,	0x33,0x66,0xCC,
    0x33,0x66,0x99, 0x33,0x66,0x66,	0x33,0x66,0x33,	0x33,0x66,0x00,
    0x33,0x33,0xFF, 0x33,0x33,0xCC,	0x33,0x33,0x99,	0x33,0x33,0x66,
    0x33,0x33,0x33, 0x33,0x33,0x00,	0x33,0x00,0xFF,	0x33,0x00,0xCC,
    // b0
    0x33,0x00,0x99, 0x33,0x00,0x66,	0x33,0x00,0x33,	0x33,0x00,0x00,
    0x00,0xFF,0xFF, 0x00,0xFF,0xCC,	0x00,0xFF,0x99,	0x00,0xFF,0x66,
    0x00,0xFF,0x33, 0x00,0xFF,0x00,	0x00,0xCC,0xFF,	0x00,0xCC,0xCC,
    0x00,0xCC,0x99, 0x00,0xCC,0x66,	0x00,0xCC,0x33,	0x00,0xCC,0x00,
    // c0
    0x00,0x99,0xFF, 0x00,0x99,0xCC,	0x00,0x99,0x99,	0x00,0x99,0x66,
    0x00,0x99,0x33, 0x00,0x99,0x00,	0x00,0x66,0xFF,	0x00,0x66,0xCC,
    0x00,0x66,0x99, 0x00,0x66,0x66,	0x00,0x66,0x33,	0x00,0x66,0x00,
    0x00,0x33,0xFF, 0x00,0x33,0xCC,	0x00,0x33,0x99,	0x00,0x33,0x66,
    // d0
    0x00,0x33,0x33, 0x00,0x33,0x00,	0x00,0x00,0xFF,	0x00,0x00,0xCC,
    0x00,0x00,0x99, 0x00,0x00,0x66,	0x00,0x00,0x33,	0xEE,0x00,0x00,
    0xDD,0x00,0x00, 0xBB,0x00,0x00,	0xAA,0x00,0x00,	0x88,0x00,0x00,
    0x77,0x00,0x00, 0x55,0x00,0x00,	0x44,0x00,0x00,	0x22,0x00,0x00,
    // e0
    0x11,0x00,0x00, 0x00,0xEE,0x00,	0x00,0xDD,0x00,	0x00,0xBB,0x00,
    0x00,0xAA,0x00, 0x00,0x88,0x00,	0x00,0x77,0x00,	0x00,0x55,0x00,
    0x00,0x44,0x00, 0x00,0x22,0x00,	0x00,0x11,0x00,	0x00,0x00,0xEE,
    0x00,0x00,0xDD, 0x00,0x00,0xBB,	0x00,0x00,0xAA,	0x00,0x00,0x88,
    // f0
    0x00,0x00,0x77, 0x00,0x00,0x55,	0x00,0x00,0x44,	0x00,0x00,0x22,
    0x00,0x00,0x11, 0xEE,0xEE,0xEE,	0xDD,0xDD,0xDD,	0xBB,0xBB,0xBB,
    0xAA,0xAA,0xAA, 0x88,0x88,0x88,	0x77,0x77,0x77,	0x55,0x55,0x55,
    0x44,0x44,0x44, 0x22,0x22,0x22,	0x11,0x11,0x11,	0x00,0x00,0x00
};
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifdef __ppc__
extern
#endif
    int killprint;
extern "C"
{
    int kmputc( int c );
}

#if DOANIO

#warning ** DO AN IO **

static unsigned long doaniobuf[256];

static void doanio( void )
{
    struct uio uio;
    struct iovec iovec;
    int err;
    dev_t device = makedev( 14, 0 );

    iovec.iov_base = (char *) &doaniobuf[0];
    iovec.iov_len  = 1024;

    uio.uio_iov = &iovec;
    uio.uio_iovcnt = 1;
    uio.uio_rw = UIO_READ;
    uio.uio_segflg = UIO_SYSSPACE;
    uio.uio_offset = 0;
    uio.uio_resid = 1024;

    DEBG(0, "\n");
    err = ((*cdevsw[major(device)].d_read)(device, &uio, 0));
    DEBG(0, " done(%08lx)\n", doaniobuf[0]);
}

#endif

IOReturn IOFramebuffer::handleEvent( IOIndex event, void * info )
{
    IOReturn ret;

    DEBG(thisIndex, "(%ld, %d)\n", event, pagingState);

    switch (event)
    {
        case kIOFBNotifyWillPowerOff:
            if (this == gIOFBConsoleFramebuffer)
            {
//		getPlatform()->setConsoleInfo( 0, kPEDisableScreen);
                killprint = 1;
            }

            if (pagingState)
            {
                pagingState = false;
                deliverFramebufferNotification( kIOFBNotifyWillSleep, info );
            }

            ret = deliverFramebufferNotification( event, info );
            configPending = true;
            break;

        case kIOFBNotifyDidPowerOn:

            if (this == gIOFBConsoleFramebuffer)
            {
//		getPlatform()->setConsoleInfo( 0, kPEEnableScreen);
                killprint = 0;
                kmputc( 033 );
                kmputc( 'c' );
            }
            ret = deliverFramebufferNotification( event, info );

            if (!pagingState && gIOFBSystemPower)
            {
                pagingState = true;

                if (connectChange)
                {
                    if (!__private->disabledForConnectChange)
                    {
                        __private->disabledForConnectChange = true;
                        deliverFramebufferNotification( kIOFBNotifyDisplayModeWillChange );
                    }
                }

                deliverFramebufferNotification( kIOFBNotifyDidWake, info );
            }

#if DOANIO
            doanio();
#endif

            configPending = false;
//          checkConnectionChange();
            break;

#if DOANIO
        case kIOFBNotifyWillSleep:
            if (!info)
            {
                doanio();
            }
            // else its the redirection notify
            ret = deliverFramebufferNotification( event, info );
            break;
#endif

#if VRAM_SAVE
        case kIOFBNotifyDidWake:
            if (info)
            {
                // restore vram content
                if (__private->saveLength)
                {
                    if (!suspended)
                    {
#if VRAM_COMPRESS
                        DecompressData( (UInt8 *) __private->saveFramebuffer, (UInt8 *) frameBuffer,
                                        0, 0, __private->framebufferWidth, __private->framebufferHeight, rowBytes);
#else
                        bcopy_nc( __private->saveFramebuffer, (void *) frameBuffer, __private->saveLength );
#endif
                        DEBG(thisIndex, " screen drawn\n");
                    }

                    IOFreePageable( __private->saveFramebuffer, __private->saveLength );

                    __private->saveFramebuffer = 0;
                    __private->saveLength      = 0;
                }
            }
            ret = deliverFramebufferNotification( event, info );
            break;
#endif /* VRAM_SAVE */

        default:
            ret = deliverFramebufferNotification( event, info );
            break;
    }

    return (ret);
}

IOReturn IOFramebuffer::notifyServer( UInt8 state )
{
    mach_msg_header_t * msgh = (mach_msg_header_t *) serverMsg;
    IOReturn		err = kIOReturnSuccess;

    if (serverNotified != state)
    {
        serverNotified = state;

        DEBG(thisIndex, "(%p, %d->%d, %d)\n", msgh->msgh_remote_port,
             serverState, serverNotified, serverPendingAck);

        msgh->msgh_id = state;
        if ((MACH_PORT_NULL == msgh->msgh_remote_port)
                || (KERN_SUCCESS != mach_msg_send_from_kernel(msgh, msgh->msgh_size)))
        {
            serverState = serverNotified;
        }
    }

    return (err);
}

bool IOFramebuffer::getIsUsable( void )
{
    return (dead || (0 != isUsable));
}

IOReturn IOFramebuffer::postWake( IOOptionBits state )
{
    UInt32 value;

    sleepConnectCheck = false;

    configPending = false;

    return (getAttributeForConnection(0, kConnectionPostWake, &value));
}

void IOFramebuffer::notifyServerAll( UInt8 state )
{
    unsigned int	index;
    IOFramebuffer *	fb;
    bool		doNotify = true;

    FBLOCK();

    if (state)
    {
        doNotify = gIOFBSystemPower;
        for (index = 0;
                doNotify && (fb = (IOFramebuffer *) gAllFramebuffers->getObject(index));
                index++)
        {
            doNotify = fb->getIsUsable();
        }
        if (doNotify)
            IOSleep(20);
    }

    if (doNotify)
    {
        if (state)
	{
            for (index = 0;
                    (fb = (IOFramebuffer *) gAllFramebuffers->getObject(index));
                    index++)
            {
                fb->postWake( state );
            }
	}
        for (index = 0;
                (fb = (IOFramebuffer *) gAllFramebuffers->getObject(index));
                index++)
        {
            fb->notifyServer( state );
        }
    }

    FBUNLOCK();
}

enum {
    kIOFBDidWork = 0x00000001,
    kIOFBPaging  = 0x00000002,
};

void IOFramebuffer::sleepWork( void * arg )
{
    unsigned int	index;
    IOFramebuffer *	fb;
    IOOptionBits	allState;

    DEBG(0, "\n");

    FBLOCK();

    do
    {
        for (index = 0, allState = 0;
                (fb = (IOFramebuffer *) gAllFramebuffers->getObject(index));
                index++)
        {
            allState |= fb->checkPowerWork();
        }

        if ((0 == (kIOFBPaging & allState)) && gIOFBSystemPowerAckTo)
        {
            DEBG(0, " allowPowerChange(%ld)\n", gIOFBSystemPowerAckRef);

            IOService * ackTo = gIOFBSystemPowerAckTo;
            UInt32      ackRef = gIOFBSystemPowerAckRef;
            gIOFBSystemPowerAckTo = 0;
            FBUNLOCK();

            ackTo->allowPowerChange( ackRef );

            DEBG(0, " did allowPowerChange()\n");

            FBLOCK();
            allState |= kIOFBDidWork;
        }
    }
    while (kIOFBDidWork & allState);

    gIOFBSleepThread = false;

    FBUNLOCK();
}

IOOptionBits IOFramebuffer::checkPowerWork( void )
{
    UInt32	 newState;
    IOOptionBits ourState = kIOFBPaging;

    DEBG(thisIndex, "(%d, %d)\n",
         gIOFBSystemPower, pendingPowerChange);

    if (!gIOFBSystemPower)
    {
        notifyServer( false );

        if (!serverState)
        {
            bool doSave = (0 != gIOFBSystemPowerAckTo);

            if (pagingState)
            {
                pagingState = false;
                FBUNLOCK();

                handleEvent( kIOFBNotifyWillSleep );

                FBLOCK();
                ourState |= kIOFBDidWork;
            }

#if VRAM_SAVE
            // vram content is being lost
            UInt32		value;
            if (doSave
                    && !__private->saveLength
                    && (kIOReturnSuccess == getAttribute(kIOVRAMSaveAttribute, &value)) && value)
            {
                vm_size_t sLen;
                sLen = __private->framebufferHeight * rowBytes;

                /*
                * dLen should account for possible growth. (e.g. run-length encoding noise)
                * 	Add 5 bytes for header,
                * 	12% for RLE growth
                * 	2 bytes per line for line spans,
                * 	1 additional escape code byte for trailing pixel in each line
                */
#if VRAM_COMPRESS
                vm_size_t dLen;

                dLen = 5 + sLen + ((sLen + 7) >> 3) + (__private->framebufferHeight * 3);
                dLen = round_page_32(dLen);
                __private->saveLength = dLen;
#else
                __private->saveLength = round_page_32(sLen);
#endif
                __private->saveFramebuffer = IOMallocPageable( __private->saveLength, page_size );

                if (__private->saveFramebuffer)
                {
#if VRAM_COMPRESS
                    dLen = CompressData( (UInt8 *) frameBuffer, bytesPerPixel,
                                         __private->framebufferWidth, __private->framebufferHeight, rowBytes,
                                         (UInt8 *) __private->saveFramebuffer, __private->saveLength );

                    DEBG(thisIndex, " compressed to %d%%\n", (dLen * 100) / sLen);

                    dLen = round_page_32( dLen );
                    if (__private->saveLength > dLen)
                    {
                        IOFreePageable( (void *) (((UInt32) __private->saveFramebuffer) + dLen),
                                        __private->saveLength - dLen );
                        __private->saveLength = dLen;
                    }
#else
                    bcopy_nc( (void *) frameBuffer, __private->saveFramebuffer, sLen );
#endif
                    if (__private->saveLength)
                    {
#if RLOG
                        kern_return_t kr = 
#endif
			vm_map_wire( IOPageableMapForAddress( (vm_address_t) __private->saveFramebuffer ),
				     (vm_address_t) __private->saveFramebuffer,
				     ((vm_address_t) __private->saveFramebuffer) + __private->saveLength,
				     VM_PROT_READ | VM_PROT_WRITE, FALSE );
                        DEBG(thisIndex, " vm_map_wire(%x)\n", kr);
                    }
                }
                else
                    __private->saveLength = 0;
            }
#endif /* VRAM_SAVE */
            ourState &= ~kIOFBPaging;
        }
    }

    if (pendingPowerChange)
    {
        pendingPowerChange = false;
        newState = pendingPowerState;

        //        FBUNLOCK();

        DEBG(thisIndex, " kIOPowerAttribute(%ld)\n", newState);
        setAttribute( kIOPowerAttribute, newState );

        FBUNLOCK();

        DEBG(thisIndex, " acknowledgeSetPowerState\n");
        acknowledgeSetPowerState();

        FBLOCK();
        ourState |= kIOFBDidWork;
    }

    if (__private->pendingSpeedChange)
    {
        __private->pendingSpeedChange = false;
	setAttribute(kIOFBSpeedAttribute, __private->reducedSpeed);
    }

    return (ourState);
}

static void startThread( void )
{
    if (!gIOFBSleepThread)
    {
        gIOFBSleepThread = true;
        thread_call_enter1( gIOFBSleepCallout, (thread_call_param_t) 0);
    }
}

IOReturn IOFramebuffer::setPowerState( unsigned long powerStateOrdinal,
                                       IOService * whichDevice )
{
    bool now;

    DEBG(thisIndex, " (%ld)\n", powerStateOrdinal);

    FBLOCK();

    pendingPowerState = powerStateOrdinal;

    now = (0xffffffff == gAllFramebuffers->getNextIndexOfObject( this, 0 ));

    if (!now)
    {
        pendingPowerChange = true;
        startThread();
    }

    if (now)
        setAttribute( kIOPowerAttribute, powerStateOrdinal );

    FBUNLOCK();

    return (now ? 0 : 45 * 1000 * 1000);
}

IOReturn IOFramebuffer::powerStateWillChangeTo( IOPMPowerFlags flags,
        unsigned long state, IOService * whatDevice )
{
    IOReturn ret;

    DEBG(thisIndex, " (%08lx)\n", flags);

    FBLOCK();

    if (state && !pm_vars->myCurrentState)
    {
        gIOFBSystemPower = true;
        sleepConnectCheck = true;
        gIOFBLastClamshellState = 0;
    }

    if (IOPMDeviceUsable & flags)
        ret = IOPMAckImplied;

    else
    {
        notifyServerAll( false );

        if (serverState != serverNotified)
        {
            // server will ack within ten seconds
            serverPendingAck = true;
            ret = 10 * 1000 * 1000;
        }
        else
            ret = IOPMAckImplied;
    }

    FBUNLOCK();

    return (ret);
}

IOReturn IOFramebuffer::powerStateDidChangeTo( IOPMPowerFlags flags,
        unsigned long, IOService* whatDevice )
{
    DEBG(thisIndex, " (%08lx)\n", flags);

    FBLOCK();

    isUsable = (0 != (IOPMDeviceUsable & flags));

    serverState = serverNotified;

    if (isUsable)
        notifyServerAll( true );

    FBUNLOCK();

    return (kIOReturnSuccess);
}

void IOFramebuffer::clamshellWork( thread_call_param_t p0, thread_call_param_t p1 )
{
    clamshellEnable( (SInt32) p1 );
}

void IOFramebuffer::clamshellEnable( SInt32 delta )
{
    UInt32	change;
    bool	desktopMode;
    bool	notSuspended;
    OSObject *  state;

    FBLOCK();
    gIOFBClamshellEnable += delta;
    notSuspended = gIOFBSystemPower && (0 == gIOFBSuspendCount);
    desktopMode = notSuspended && gIOFBDesktopModeAllowed && (gIOFBClamshellEnable <= 0);
    FBUNLOCK();

    if (delta < 0)
        change = kIOPMDisableClamshell;
    else if (notSuspended)
        change = kIOPMEnableClamshell | kIOPMSetDesktopMode | (desktopMode ? kIOPMSetValue : 0);
    else
        return ;

    gIOFBClamshellState = change;
    getPMRootDomain()->receivePowerNotification( change );

    if ((kIOPMEnableClamshell & change)
            && (state = getPMRootDomain()->getProperty(kAppleClamshellStateKey)))
    {
        publishResource(kAppleClamshellStateKey, gIOFBLastClamshellState ? kOSBooleanTrue : kOSBooleanFalse );
    }
}

IOOptionBits IOFramebuffer::clamshellState( void )
{
    return (gIOFBClamshellState);
}

IOReturn IOFramebuffer::systemPowerChange( void * target, void * refCon,
        UInt32 messageType, IOService * service,
        void * messageArgument, vm_size_t argSize )
{
    IOReturn ret;
    IOPowerStateChangeNotification * params = (IOPowerStateChangeNotification *) messageArgument;

    DEBG(0, "(%08lx, %ld)\n",
         messageType, (UInt32) params->powerRef);

    switch (messageType)
    {
        case kIOMessageSystemWillSleep:

            gIOFBClamshellState = kIOPMDisableClamshell;
            getPMRootDomain()->receivePowerNotification( kIOPMDisableClamshell );

            FBLOCK();

            gIOFBSystemPower       = false;
            gIOFBSystemPowerAckRef = (UInt32) params->powerRef;
            gIOFBSystemPowerAckTo  = service;

            startThread();

            FBUNLOCK();

            // We will ack within 20 seconds
            params->returnValue = 20 * 1000 * 1000;
            ret 		= kIOReturnSuccess;
            break;

        case kIOMessageSystemHasPoweredOn:
            params->returnValue = 0;
            ret 		= kIOReturnSuccess;
            break;

	case kIOMessageSystemWillRestart:
        case kIOMessageSystemWillPowerOff:

            FBLOCK();
            if (gAllFramebuffers)
            {
		IOFramebuffer * fb;
                for (UInt32 index = 0;
                        (fb = (IOFramebuffer *) gAllFramebuffers->getObject(index));
                        index++)
                    fb->setAttribute( kIOSystemPowerAttribute, messageType );
            }
            FBUNLOCK();

            params->returnValue = 0;
            ret 		= kIOReturnSuccess;
            break;

        default:
            ret = kIOReturnUnsupported;
            break;
    }

    return (ret);
}

IOReturn IOFramebuffer::setAggressiveness( unsigned long type, unsigned long newLevel )
{
    UInt32 reducedSpeed;

    if (gIOFBGate && (type == (unsigned long) kIOFBLowPowerAggressiveness))
    {
	FBLOCK();
    
	reducedSpeed = newLevel;
	if (reducedSpeed != __private->reducedSpeed)
	{
	    __private->reducedSpeed = reducedSpeed;
	    __private->pendingSpeedChange = true;
	    startThread();
	}
    
	FBUNLOCK();
    }

    super::setAggressiveness(type, newLevel);

    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::getAggressiveness( unsigned long type, unsigned long * currentLevel )
{
    IOReturn ret;

    if (gIOFBGate && (type == (unsigned long) kIOFBLowPowerAggressiveness))
    {
	FBLOCK();
	*currentLevel = __private->reducedSpeed;
	FBUNLOCK();
	ret = kIOReturnSuccess;
    }
    else
	ret = super::getAggressiveness(type, currentLevel);

    return (ret);
}

IOReturn
IOFramebuffer::extAcknowledgeNotification( void )
{
    bool needConnectCheck, needAck;

    FBLOCK();

    DEBG(thisIndex, " (%d->%d, %d)\n",
         serverState, serverNotified, serverPendingAck);

    needConnectCheck = (serverState != serverNotified);
    serverState = serverNotified;

    needAck = serverPendingAck;
    serverPendingAck = false;

    if (serverState && __private->cursorSlept)
    {
	resetCursor();
	__private->cursorSlept = false;
    }
    else if (!serverState && !__private->cursorSlept)
    {
	hideCursor();
	__private->cursorSlept = true;
    }

    checkDeferredCLUTSet();

    if (needConnectCheck)
        checkConnectionChange();

    startThread();

    FBUNLOCK();

    if (needAck)
        acknowledgePowerChange(this);

    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::extRegisterNotificationPort(
    mach_port_t 	port,
    UInt32		type,
    UInt32		refCon )
{
    mach_msg_header_t * msgh;
    UInt8		currentState;

    FBLOCK();

    msgh = (mach_msg_header_t *) serverMsg;
    bzero( msgh, sizeof(mach_msg_header_t) );

    msgh->msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msgh->msgh_size        = sizeof(mach_msg_header_t);
    msgh->msgh_remote_port = port;

    currentState     = serverNotified;
    serverState      = true;			// server assumes so at startup
    serverNotified   = true;
    serverPendingAck = false;

    notifyServer( currentState );

    msgh->msgh_id = 0x87654321;
    mach_msg_send_from_kernel( msgh, msgh->msgh_size );

    FBUNLOCK();

    return (kIOReturnSuccess);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void IOFramebuffer::connectChangeInterrupt( IOFramebuffer * inst, void * ref )
{
    OSIncrementAtomic( &inst->connectChange);

    DEBG(inst->thisIndex, "(%ld)\n", inst->connectChange);

    if (0 == inst->configPending)
        inst->deferredEvents->interruptOccurred(0, 0, 0);
}

void IOFramebuffer::deferredInterrupt( OSObject * owner,
                                       IOInterruptEventSource * evtSrc, int intCount )
{
    IOFramebuffer * self = (IOFramebuffer *) owner;

    self->checkConnectionChange();
}

void IOFramebuffer::checkConnectionChange( bool message )
{
    bool nowSuspended;

    DEBG(thisIndex, " count(%ld), susp(%d), sleep(%d), capt(%d)\n",
         connectChange, suspended, sleepConnectCheck, captured);

    if ((gIOFBDesktopModeAllowed || !gIOFBLastClamshellState)
	&& (connectChange && (sleepConnectCheck || !captured)))
    {
        FBLOCK();
        nowSuspended = !suspended;
        if (nowSuspended)
        {
            suspended = true;
            messaged = false;
            connectChange = 0;
            gIOFBSuspendCount++;
        }

        FBUNLOCK();

        if (message)
        {
            IOFramebuffer * next = this;
            while ((next = next->getNextDependent()) && (next != this))
            {
                next->checkConnectionChange(false);
            }
        }

        if (nowSuspended)
        {
            if (message)
                messageClients( kIOMessageServiceIsSuspended, (void *) true );
        }
        else
            DEBG(thisIndex, " spurious\n");
    }
    sleepConnectCheck = false;
    clamshellEnable(0);
}

IOReturn IOFramebuffer::open( void )
{
    IOReturn		err = kIOReturnSuccess;
    UInt32		value;
    void *		vblInterrupt;
    void *		connectInterrupt;
    IOFramebuffer *	next;
    OSNumber * 		num;
    bool		firstOpen;

    do
    {
        if (opened)
            continue;
        if (dead)
        {
            err = kIOReturnNotOpen;
            continue;
        }
        if (!gAllFramebuffers)
        {
            gAllFramebuffers = OSArray::withCapacity(1);

            IORegistryEntry   * root;
            OSData *	        data = 0;
            if ((root = IORegistryEntry::fromPath("/", gIOServicePlane)))
            {
                data = OSDynamicCast(OSData, root->getProperty("graphic-options"));
                root->release();
            }
            gIOFBDesktopModeAllowed = !data || (0 != (8 & *((UInt32 *) data->getBytesNoCopy())));
        }

        if (!gAllFramebuffers)
            continue;
        if (!gIOFBRootNotifier)
            gIOFBRootNotifier = getPMRootDomain()->registerInterest(
                                    gIOPriorityPowerStateInterest, &systemPowerChange, 0, 0 );
        if (!gIOFBRootNotifier)
            continue;
        if (!gIOFBSleepCallout)
            gIOFBSleepCallout = thread_call_allocate( (thread_call_func_t)&sleepWork,
                                (thread_call_param_t) 0);
        if (!gIOFBSleepCallout)
            continue;
        if (!gIOFBClamshellCallout)
            gIOFBClamshellCallout = thread_call_allocate( &clamshellWork,
                                    (thread_call_param_t) 0);
        if (!gIOFBClamshellCallout)
            continue;
        if (!gIOFBWorkLoop)
        {
            OSIterator * iter = getMatchingServices( nameMatching("IOHIDSystem") );
            if (iter)
            {
                IOService * hidsystem;
                if ((hidsystem = OSDynamicCast(IOService, iter->getNextObject())))
                {
                    gIOFBWorkLoop = hidsystem->getWorkLoop();
                    if (gIOFBWorkLoop)
                        gIOFBWorkLoop->retain();
                }
                iter->release();
            }
        }
        if (!gIOFBWorkLoop)
            gIOFBWorkLoop = IOWorkLoop::workLoop();
        if (!gIOFBWorkLoop)
            continue;
        if (!gIOFBGate)
            gIOFBGate = IOFBGate::gate( this );
        if (!gIOFBGate)
            continue;
        gIOFBWorkLoop->addEventSource( gIOFBGate );

        FBLOCK();

        serverNotified   = true;
        serverState      = true;

        // tell the console if it's on this display, it's going away
        if (isConsoleDevice())
        {
            gIOFBConsoleFramebuffer = this;
            getPlatform()->setConsoleInfo( 0, kPEDisableScreen);
        }

        deliverFramebufferNotification( kIOFBNotifyDisplayModeWillChange );

        err = enableController();
        if (kIOReturnSuccess != err)
        {
            dead = true;
            if (nextDependent)
            {
                nextDependent->setNextDependent( NULL );
                nextDependent = NULL;
            }
            deliverFramebufferNotification( kIOFBNotifyDisplayModeDidChange );
            continue;
        }

        pagingState = true;

        thisIndex = gAllFramebuffers->getCount();
        gAllFramebuffers->setObject(this);
        DEBG(thisIndex, " this %p \"%s\" \"%s\"\n", this, getName(), getProvider()->getName());

        err = registerForInterruptType( kIOFBVBLInterruptType,
                                        (IOFBInterruptProc) &handleVBL,
                                        this, priv, &vblInterrupt );
        haveVBLService = (err == kIOReturnSuccess );

        deferredEvents = IOInterruptEventSource::interruptEventSource(this, deferredInterrupt);
        if (deferredEvents)
            getWorkLoop()->addEventSource(deferredEvents);

        err = registerForInterruptType( kIOFBConnectInterruptType,
                                        (IOFBInterruptProc) &connectChangeInterrupt,
                                        this, priv, &connectInterrupt );

        err = getAttribute( kIOHardwareCursorAttribute, &value );
	haveHWCursor = ((err == kIOReturnSuccess) && (0 != (kIOFBHWCursorSupported & value)));

	if ((num = OSDynamicCast(OSNumber, getProperty(kIOFBGammaWidthKey))))
	    __private->desiredGammaDataWidth = num->unsigned32BitValue();
	if ((num = OSDynamicCast(OSNumber, getProperty(kIOFBGammaCountKey))))
	    __private->desiredGammaDataCount = num->unsigned32BitValue();
	if ((num = OSDynamicCast(OSNumber, getProperty(kIOFBGammaHeaderSizeKey))))
	    __private->gammaHeaderSize = num->unsigned32BitValue();

	if (haveVBLService
	 && (kIOReturnSuccess == getAttribute( kIODeferCLUTSetAttribute, &value ))
	 && value)
	{
	    __private->deferredCLUTSetEvent = IOInterruptEventSource::interruptEventSource(
									this, deferredCLUTSetInterrupt);
	    if (__private->deferredCLUTSetEvent)
	    {
		getWorkLoop()->addEventSource(__private->deferredCLUTSetEvent);
		setProperty(kIOFBCLUTDeferKey, kOSBooleanTrue);
	    }
	}

        num = OSDynamicCast( OSNumber, getProperty(kIOFBDependentIDKey) );
        firstOpen = num && !nextDependent;

        if (firstOpen)
	{
            do
            {
                OSDictionary  * matching;
                OSDictionary  * propMatch;
                OSIterator    * iter;

                matching = serviceMatching("IOFramebuffer");
                if (!matching)
                    continue;
                propMatch = OSDictionary::withCapacity(1);
                if (!propMatch)
                    continue;
                propMatch->setObject( kIOFBDependentIDKey, num );
                matching->setObject( gIOPropertyMatchKey, propMatch );
                propMatch->release();
                iter = getMatchingServices( matching );
                matching->release();
                if (iter)
                {
                    IOFramebuffer * first = 0;
                    IOFramebuffer * last = 0;

                    while ((next = (IOFramebuffer *) iter->getNextObject()))
                    {
                        if (!first)
                            first = next;
                        else if (last)
                            last->setNextDependent( next );
                        last = next;
                    }
                    if (first && last && (first != last))
                        last->setNextDependent( first );
                    iter->release();
                }
            }
            while (false);
	}

        opened = true;

        UInt32 connectEnabled;
        err = getAttributeForConnection( 0, kConnectionEnable, &connectEnabled );
        if (kIOReturnSuccess != err)
            connectEnabled = true;

	__private->paramHandler = IOFramebufferParameterHandler::withFramebuffer(this);
	if (__private->paramHandler)
	    setProperty(gIODisplayParametersKey, __private->paramHandler);

        if (connectEnabled)
            IODisplayWrangler::makeDisplayConnects(this);

        if (firstOpen)
        {
            next = this;
            while ((next = next->getNextDependent()) && (next != this))
            {
                next->open();
            }
        }

        if (connectEnabled)
        {
            setupForCurrentConfig();
            err = kIOReturnSuccess;
        }
        else
            deliverFramebufferNotification( kIOFBNotifyDisplayModeDidChange, 0 );

        // if( firstOpen)
        {
            next = this;
            do
            {
                next->postOpen();
            }
            while ((next = next->getNextDependent()) && (next != this));
        }
    }
    while (false);

    checkConnectionChange();

    if (gIOFBGate)
        FBUNLOCK();

    return (err);
}

IOReturn IOFramebuffer::postOpen( void )
{
    if (__private->cursorAttributes)
    {
        __private->cursorAttributes->release();
        __private->cursorAttributes = 0;
    }

    __private->cursorAttributes = OSArray::withCapacity(2);
    if (!__private->cursorAttributes)
        return (kIOReturnNoMemory);

    __private->testingCursor = true;

    setCursorImage( (void *) 0 );

    if (__private->cursorThread)
    {
        IOHardwareCursorDescriptor desc;

        desc.majorVersion 	= kHardwareCursorDescriptorMajorVersion;
        desc.minorVersion 	= kHardwareCursorDescriptorMinorVersion;
        desc.height 		= 256;
        desc.width 		= 256;
        desc.bitDepth 		= 32;
        desc.maskBitDepth 	= 0;
        desc.colorEncodings	= 0;
        desc.flags 		= 0;
        desc.supportedSpecialEncodings = kTransparentEncodedPixel;

        (*__private->cursorControl.callouts->setCursorImage) (
            __private->cursorControl.self, __private->cursorControl.ref,
            &desc, (void *) 0 );

        if (gIOFBWorkLoop)
            gIOFBWorkLoop->addEventSource(__private->cursorThread);
    }
    __private->testingCursor = false;

    setProperty( kIOFBCursorInfoKey, __private->cursorAttributes );

    return (kIOReturnSuccess);
}

IOWorkLoop * IOFramebuffer::getWorkLoop() const
{
    return (gIOFBWorkLoop);
}

void IOFramebuffer::setCaptured( bool isCaptured )
{
    captured = isCaptured;
}

void IOFramebuffer::setNextDependent( IOFramebuffer * dependent )
{
    nextDependent = dependent;
}

IOFramebuffer * IOFramebuffer::getNextDependent( void )
{
    return (nextDependent);
}

void IOFramebuffer::close( void )	// called by the user client when
{					// the window server exits
    mach_msg_header_t * msgh;
    unsigned int	idx;

    if (this == gIOFBConsoleFramebuffer)
        getPlatform()->setConsoleInfo( 0, kPEAcquireScreen);

    msgh = (mach_msg_header_t *) serverMsg;
    if (msgh)
	msgh->msgh_remote_port = MACH_PORT_NULL;

    serverConnect = 0;
    captured = false;

    if (gRunawayFramebuffers)
    {
	FBLOCK();

	idx = gAllFramebuffers->getNextIndexOfObject( this, 0 );
	if (idx != (unsigned int) -1)
	    gAllFramebuffers->removeObject(idx);
    
	idx = gRunawayFramebuffers->getNextIndexOfObject( this, 0 );
	if (idx != (unsigned int) -1)
	    gRunawayFramebuffers->removeObject(idx);
    
	FBUNLOCK();

	if (idx != (unsigned int) -1)
	{
	    terminate();
	}
    }
}

IODeviceMemory * IOFramebuffer::getVRAMRange( void )
{
    return (getApertureRange(kIOFBSystemAperture));
}

IOReturn IOFramebuffer::setUserRanges( void )
{
#if RLOG
    // print ranges
    UInt32		i, numRanges;
    IODeviceMemory *	mem;
    numRanges = userAccessRanges->getCount();
    DEBG(thisIndex, " ranges num:%ld\n", numRanges);
    for (i = 0; i < numRanges; i++)
    {
        mem = (IODeviceMemory *) userAccessRanges->getObject( i );
        if (0 == mem)
            continue;
        DEBG(thisIndex, " start:%lx size:%lx\n",
             mem->getPhysicalAddress(), mem->getLength() );
    }
#endif

    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::setupForCurrentConfig( void )
{
    return (doSetup(true));
}

IOReturn IOFramebuffer::doSetup( bool full )
{
    IOReturn			err;
    IODisplayModeID		mode;
    IOIndex			depth;
    IOPixelInformation		info;
    IODisplayModeInformation	dmInfo;
    IODeviceMemory *		mem;
    IODeviceMemory *		fbRange;
    IOPhysicalAddress		base;
    UInt32			value;
    PE_Video			newConsole;

    err = getAttribute( kIOHardwareCursorAttribute, &value );
    __private->cursorPanning = ((err == kIOReturnSuccess) && (0 != (kIOFBCursorPans & value)));

    err = getCurrentDisplayMode( &mode, &depth );
    if (err)
        IOLog("%s: getCurrentDisplayMode %d\n", getName(), err);

    err = getPixelInformation( mode, depth, kIOFBSystemAperture, &info );
    if (err)
        IOLog("%s: getPixelInformation %d\n", getName(),  err);

    if (full && (clutValid == false) && (info.pixelType == kIOCLUTPixels))
    {
        IOColorEntry	*	tempTable;
        int			i;

        tempTable = (IOColorEntry *) IOMalloc( 256 * sizeof( *tempTable));
        if (tempTable)
        {
            for (i = 0; i < 256; i++)
            {
                if (currentMono)
                {
                    UInt32	lum;

                    lum = 0x0101 * i;
                    tempTable[ i ].red   = lum;
                    tempTable[ i ].green = lum;
                    tempTable[ i ].blue  = lum;
                }
                else
                {
                    tempTable[ i ].red   = (appleClut8[ i * 3 + 0 ] << 8)
                                           | appleClut8[ i * 3 + 0 ];
                    tempTable[ i ].green = (appleClut8[ i * 3 + 1 ] << 8)
                                           | appleClut8[ i * 3 + 1 ];
                    tempTable[ i ].blue  = (appleClut8[ i * 3 + 2 ] << 8)
                                           | appleClut8[ i * 3 + 2 ];
                }
            }
            setCLUTWithEntries( tempTable, 0, 256, 1 * kSetCLUTImmediately );
            IOFree( tempTable, 256 * sizeof( *tempTable));
        }
        clutValid = true;
    }

    fbRange = getApertureRange( kIOFBSystemAperture );

    if (full && fbRange)
    {
        userAccessRanges->removeObject( kIOFBSystemAperture );
        userAccessRanges->setObject( kIOFBSystemAperture, fbRange );
        err = setUserRanges();

        base = fbRange->getPhysicalAddress();
        if ((mem = getVRAMRange()))
        {
            vramMapOffset = base - mem->getPhysicalAddress();
            if (vramMapOffset > mem->getLength())
                vramMapOffset &= (mem->getLength() - 1);
            setProperty( kIOFBMemorySizeKey, mem->getLength(), 32 );
            mem->release();
        }

        if (vramMap)
            vramMap->release();
        vramMap = fbRange->map( kIOMapInhibitCache );
        assert( vramMap );
        if (vramMap)
            base = vramMap->getVirtualAddress();

        // console now available
        if (info.activeWidth >= 128 && (this == gIOFBConsoleFramebuffer) || !gIOFBConsoleFramebuffer)
        {
            newConsole.v_baseAddr	= base;
            newConsole.v_rowBytes	= info.bytesPerRow;
            newConsole.v_width		= info.activeWidth;
            newConsole.v_height		= info.activeHeight;
            newConsole.v_depth		= info.bitsPerPixel;
            //	strcpy( consoleInfo->v_pixelFormat, "PPPPPPPP");
            getPlatform()->setConsoleInfo( &newConsole, kPEReleaseScreen );
            getPlatform()->setConsoleInfo( &newConsole, kPEEnableScreen );
            gIOFBConsoleFramebuffer	= this;
        }

        (void) getInformationForDisplayMode( mode, &dmInfo );
        DEBG(thisIndex, " using (%ldx%ld@%ldHz,%ld bpp)\n",
             info.activeWidth, info.activeHeight,
             (dmInfo.refreshRate + 0x8000) >> 16, info.bitsPerPixel );
    }

    if (full)
        deliverFramebufferNotification( kIOFBNotifyDisplayModeDidChange, 0 );

    if (fbRange)
        fbRange->release();
    if (vramMap)
        setupCursor( &info );

    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::extSetDisplayMode( IODisplayModeID displayMode,
        IOIndex depth )
{
    IOReturn	err;
    bool	wasSuspended;

    FBLOCK();

    stopCursor();

    checkDeferredCLUTSet();

    if (this == gIOFBConsoleFramebuffer)
    {
        getPlatform()->setConsoleInfo( 0, kPEDisableScreen);
	gIOFBConsoleFramebuffer = 0;
    }

    wasSuspended = (suspended && messaged);	// this mode set is during connection change ack

    DEBG(thisIndex, " susp(%d)\n", wasSuspended);

    if (!wasSuspended)
        deliverFramebufferNotification( kIOFBNotifyDisplayModeWillChange );

    err = setDisplayMode( displayMode, depth );

    clutValid = false;

    setupForCurrentConfig();

    if (__private->paramHandler)
	__private->paramHandler->displayModeChange();

    if (wasSuspended)
    {
        suspended = false;
        --gIOFBSuspendCount;
        __private->disabledForConnectChange = false;
    }

    if (connectChange)
        checkConnectionChange();

    if (wasSuspended && !suspended)
    {
        AbsoluteTime deadline;

        clock_interval_to_deadline( 10*1000, kMillisecondScale, &deadline );
        thread_call_enter1_delayed( gIOFBClamshellCallout,
                                    (thread_call_param_t) 0, deadline );
    }

    FBUNLOCK();

    return (err);
}

IOReturn IOFramebuffer::extSetAttribute(
    IOSelect attribute, UInt32 value, IOFramebuffer * other )
{
    IOReturn	err;
    UInt32	data[2];

    FBLOCK();

    switch (attribute)
    {
        case kIOMirrorAttribute:

            if (suspended)
            {
                err = kIOReturnSuccess;
                break;
            }

	    value = (value != 0);
	    if (value == __private->mirrorState)
            {
                err = kIOReturnSuccess;
                break;
            }

            stopCursor();

	    checkDeferredCLUTSet();

            if (this == gIOFBConsoleFramebuffer)
                getPlatform()->setConsoleInfo( 0, kPEDisableScreen);

            DEBG(thisIndex, " kIOMirrorAttribute(%ld)\n", value);

            deliverFramebufferNotification( kIOFBNotifyDisplayModeWillChange );

            data[0] = value;
            data[1] = (UInt32) other;
            err = setAttribute( attribute, (UInt32) &data );
	    if (kIOReturnSuccess == err)
		__private->mirrorState = value;

            clutValid = false;

            setupForCurrentConfig();
            break;

        default:
            err = setAttribute( attribute, value );
            break;
    }

    FBUNLOCK();

    return (err);
}

IOReturn IOFramebuffer::extGetAttribute(
    IOSelect attribute, UInt32 * value, IOFramebuffer * other )
{
    IOReturn	err = kIOReturnSuccess;

    FBLOCK();

    switch (attribute)
    {
        case kConnectionChanged:
            {
                UInt32	connectEnabled;

                DEBG(thisIndex, " kConnectionChanged susp(%d)\n", suspended);

                if (!suspended)
                    break;

		checkDeferredCLUTSet();

                if (!__private->disabledForConnectChange)
                {
                    __private->disabledForConnectChange = true;
                    deliverFramebufferNotification( kIOFBNotifyDisplayModeWillChange );
                }

                err = getAttributeForConnection( 0, kConnectionChanged, (UInt32 *) &connectChange );

		if (__private->paramHandler)
		    __private->paramHandler->setDisplay(0);

                temporaryPowerClampOn();
                FBUNLOCK();
                IODisplayWrangler::destroyDisplayConnects( this );
                FBLOCK();

                err = getAttributeForConnection( 0, kConnectionEnable, &connectEnabled );
                if (!dead && ((kIOReturnSuccess != err) || connectEnabled))
                    IODisplayWrangler::makeDisplayConnects( this );

                messaged = true;

                err = kIOReturnSuccess;
            }
            break;

        default:
            {
                *value = (UInt32) other;
                err = getAttribute( attribute, value );
            }
            break;
    }

    FBUNLOCK();

    return (err);
}

IOReturn IOFramebuffer::extGetInformationForDisplayMode(
    IODisplayModeID mode, void * info, IOByteCount length )
{
    UInt32			 flags = 0;
    IOReturn			 err;
    bool			 getTiming;
    IOFBDisplayModeDescription * out = (IOFBDisplayModeDescription *) info;

    if (length < sizeof(IODisplayModeInformation))
        return (kIOReturnBadArgument);

    FBLOCK();

    err = getInformationForDisplayMode( mode, &out->info );
    if (kIOReturnSuccess == err)
    {
        err = IODisplayWrangler::getFlagsForDisplayMode( this, mode, &flags);
        if (kIOReturnSuccess == err)
        {
            out->info.flags &= ~kDisplayModeSafetyFlags;
            out->info.flags |= flags;
        }
        getTiming = (length >= sizeof(IOFBDisplayModeDescription));
        out->timingInfo.flags = getTiming ? kIODetailedTimingValid : 0;
        if (kIOReturnSuccess != getTimingInfoForDisplayMode(mode, &out->timingInfo))
        {
            out->timingInfo.flags &= ~kIODetailedTimingValid;
            out->timingInfo.appleTimingID = 0;
        }
    }

    FBUNLOCK();

    return (err);
}

IOReturn IOFramebuffer::extSetProperties( OSDictionary * props )
{
    OSDictionary * dict;
    OSArray *      array;
    OSNumber *     num;
    IOReturn       kr = kIOReturnUnsupported;

    FBLOCK();

    if ((dict = OSDynamicCast(OSDictionary, props->getObject(kIOFBConfigKey))))
    {
        setProperty( kIOFBConfigKey, dict );

        if ((num = OSDynamicCast(OSNumber,
                                 dict->getObject(kIODisplayConnectFlagsKey))))
            setAttributeForConnection( 0, kConnectionFlags, num->unsigned32BitValue() );

        if ((array = OSDynamicCast(OSArray,
                                   dict->getObject(kIOFBDetailedTimingsKey))))
            kr = setDetailedTimings( array );
        else
            kr = kIOReturnSuccess;
    }

    FBUNLOCK();

    return (kr);
}

//// Controller attributes

IOReturn IOFramebuffer::setAttribute( IOSelect attribute, UInt32 value )
{
    IOReturn	    ret;
    IOFramebuffer * next;
    bool	    wasCaptured;

    switch (attribute)
    {
        case kIOCapturedAttribute:
            {
                wasCaptured = captured;

                DEBG(thisIndex, " kIOCapturedAttribute(%ld)\n", value);

                next = this;
                do
                {
                    next->setCaptured( 0 != value );
                }
                while ((next = next->getNextDependent()) && (next != this));

                if (wasCaptured && !captured)
                {
                    next = this;
                    do
                    {
                        next->checkConnectionChange();
                    }
                    while ((next = next->getNextDependent()) && (next != this));
                }

                ret = kIOReturnSuccess;
                break;
            }

        case kIOCursorControlAttribute:
            {
                IOFBCursorControlAttribute * crsrControl;

                crsrControl = (IOFBCursorControlAttribute *) value;

                if (__private->cursorThread)
                {
                    __private->cursorThread->release();
                    __private->cursorThread = 0;
                }

                if (crsrControl && crsrControl->callouts)
                {
                    __private->cursorControl = *((IOFBCursorControlAttribute *) value);
                    __private->cursorThread = IOInterruptEventSource::interruptEventSource(this, &cursorWork);
                    if (gIOFBWorkLoop && __private->cursorThread)
                        gIOFBWorkLoop->addEventSource(__private->cursorThread);
                }
                ret = kIOReturnSuccess;
                break;
            }

        default:
            ret = kIOReturnUnsupported;
            break;
    }

    return (ret);
}

IOReturn IOFramebuffer::getAttribute( IOSelect attribute, UInt32 * value )
{
    return (kIOReturnUnsupported);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool IOFramebuffer::setNumber( OSDictionary * dict, const char * key,
                               UInt32 value )
{
    OSNumber *	num;
    bool	ok;

    num = OSNumber::withNumber( value, 32 );
    if (!num)
        return (false);

    ok = dict->setObject( key, num );
    num->release();

    return (ok);
}

bool IOFramebuffer::serializeInfo( OSSerialize * s )
{
    IOReturn			err;
    IODisplayModeInformation	info;
    IOPixelInformation		pixelInfo;
    IODisplayModeID *		modeIDs;
    IOItemCount			modeCount, modeNum, aperture;
    IOIndex			depthNum;
    OSDictionary *		infoDict;
    OSDictionary *		modeDict;
    OSDictionary *		pixelDict;
    char			keyBuf[12];
    bool			ok = true;

    modeCount = getDisplayModeCount();
    modeIDs = IONew( IODisplayModeID, modeCount );
    if (!modeIDs)
        return (false);

    err = getDisplayModes( modeIDs );
    if (err)
        return (false);

    infoDict = OSDictionary::withCapacity( 10 );
    if (!infoDict)
        return (false);

    for (modeNum = 0; modeNum < modeCount; modeNum++)
    {
        err = getInformationForDisplayMode( modeIDs[ modeNum ], &info );
        if (err)
            continue;

        modeDict = OSDictionary::withCapacity( 10 );
        if (!modeDict)
            break;

        ok = setNumber( modeDict, kIOFBWidthKey,
                        info.nominalWidth )
             && setNumber( modeDict, kIOFBHeightKey,
                           info.nominalHeight )
             && setNumber( modeDict, kIOFBRefreshRateKey,
                           info.refreshRate )
             && setNumber( modeDict, kIOFBFlagsKey,
                           info.flags );
        if (!ok)
            break;

        for (depthNum = 0; depthNum < info.maxDepthIndex; depthNum++)
        {
            for (aperture = 0; ; aperture++)
            {
                err = getPixelInformation( modeIDs[ modeNum ], depthNum,
                                           aperture, &pixelInfo );
                if (err)
                    break;

                pixelDict = OSDictionary::withCapacity( 10 );
                if (!pixelDict)
                    continue;

                ok = setNumber( pixelDict, kIOFBBytesPerRowKey,
                                pixelInfo.bytesPerRow )
                     && setNumber( pixelDict, kIOFBBytesPerPlaneKey,
                                   pixelInfo.bytesPerPlane )
                     && setNumber( pixelDict, kIOFBBitsPerPixelKey,
                                   pixelInfo.bitsPerPixel )
                     && setNumber( pixelDict, kIOFBComponentCountKey,
                                   pixelInfo.componentCount )
                     && setNumber( pixelDict, kIOFBBitsPerComponentKey,
                                   pixelInfo.bitsPerComponent )
                     && setNumber( pixelDict, kIOFBFlagsKey,
                                   pixelInfo.flags )
                     && setNumber( pixelDict, kIOFBWidthKey,
                                   pixelInfo.activeWidth )
                     && setNumber( pixelDict, kIOFBHeightKey,
                                   pixelInfo.activeHeight );
                if (!ok)
                    break;

                sprintf( keyBuf, "%lx", depthNum + (aperture << 16) );
                modeDict->setObject( keyBuf, pixelDict );
                pixelDict->release();
            }
        }

        sprintf( keyBuf, "%lx", modeIDs[ modeNum ] );
        infoDict->setObject( keyBuf, modeDict );
        modeDict->release();
    }

    IODelete( modeIDs, IODisplayModeID, modeCount );

    ok &= infoDict->serialize( s );
    infoDict->release();

    return (ok);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDefineMetaClassAndStructors(_IOFramebufferNotifier, IONotifier)
#define LOCKNOTIFY()
#define UNLOCKNOTIFY()

void _IOFramebufferNotifier::remove()
{
    LOCKNOTIFY();

    if (whence)
    {
        whence->removeObject( (OSObject *) this );
        whence = 0;
    }

    fEnable = false;

    UNLOCKNOTIFY();

    release();
}

bool _IOFramebufferNotifier::disable()
{
    bool	ret;

    LOCKNOTIFY();
    ret = fEnable;
    fEnable = false;
    UNLOCKNOTIFY();

    return (ret);
}

void _IOFramebufferNotifier::enable( bool was )
{
    LOCKNOTIFY();
    fEnable = was;
    UNLOCKNOTIFY();
}

IONotifier * IOFramebuffer::addFramebufferNotification(
    IOFramebufferNotificationHandler handler,
    OSObject * self, void * ref)
{
    _IOFramebufferNotifier *	notify = 0;

    notify = new _IOFramebufferNotifier;
    if (notify && !notify->init())
    {
        notify->release();
        notify = 0;
    }

    if (notify)
    {
        notify->handler = handler;
        notify->self = self;
        notify->ref = ref;
        notify->fEnable = true;

        if (0 == fbNotifications)
            fbNotifications = OSSet::withCapacity(1);

        notify->whence = fbNotifications;
        if (fbNotifications)
            fbNotifications->setObject( notify );
    }

    return (notify);
}

IOReturn IOFramebuffer::deliverFramebufferNotification(
    IOIndex event, void * info )
{
    OSIterator *		iter;
    _IOFramebufferNotifier *	notify;
    IOReturn			ret = kIOReturnSuccess;
    IOReturn			r;

    DEBG(thisIndex, "(%ld)\n", event);

    LOCKNOTIFY();

    iter = OSCollectionIterator::withCollection( fbNotifications );

    if (iter)
    {
        while ((notify = (_IOFramebufferNotifier *) iter->getNextObject()))
        {
            if (notify->fEnable)
            {
                r = (*notify->handler)( notify->self, notify->ref, this,
                                        event, info );
                if (kIOReturnSuccess != r)
                    ret = r;
            }
        }
        iter->release();
    }

    UNLOCKNOTIFY();

    return (ret);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// Some stubs

IOReturn IOFramebuffer::enableController ( void )
{
    return (kIOReturnSuccess);
}

bool IOFramebuffer::isConsoleDevice( void )
{
    return (false);
}

// Set display mode and depth
IOReturn IOFramebuffer::setDisplayMode( IODisplayModeID /* displayMode */,
                                        IOIndex /* depth */ )
{
    return (kIOReturnUnsupported);
}

// For pages
IOReturn IOFramebuffer::setApertureEnable(
    IOPixelAperture /* aperture */, IOOptionBits /* enable */ )
{
    return (kIOReturnUnsupported);
}

// Display mode and depth for startup
IOReturn IOFramebuffer::setStartupDisplayMode(
    IODisplayModeID /* displayMode */, IOIndex /* depth */ )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::getStartupDisplayMode(
    IODisplayModeID * /* displayMode */, IOIndex * /* depth */ )
{
    return (kIOReturnUnsupported);
}

//// CLUTs

IOReturn IOFramebuffer::setCLUTWithEntries(
    IOColorEntry * /* colors */, UInt32 /* index */,
    UInt32 /* numEntries */, IOOptionBits /* options */ )
{
    return (kIOReturnUnsupported);
}

//// Gamma

IOReturn IOFramebuffer::setGammaTable( UInt32 /* channelCount */,
                                       UInt32 /* dataCount */, UInt32 /* dataWidth */, void * /* data */ )
{
    return (kIOReturnUnsupported);
}


//// Display mode timing information

IOReturn IOFramebuffer::getTimingInfoForDisplayMode(
    IODisplayModeID /* displayMode */,
    IOTimingInformation * /* info */ )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::validateDetailedTiming(
    void * description, IOByteCount descripSize )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::setDetailedTimings( OSArray * array )
{
    return (kIOReturnUnsupported);
}

//// Connections

IOItemCount IOFramebuffer::getConnectionCount( void )
{
    return (1);
}

IOReturn IOFramebuffer::setAttributeForConnection( IOIndex connectIndex,
        IOSelect attribute, UInt32 info )
{
    return (kIOReturnSuccess);
}

IOReturn IOFramebuffer::getAttributeForConnection( IOIndex /* connectIndex */,
        IOSelect /* attribute */, UInt32  * /* value */ )
{
    return (kIOReturnUnsupported);
}

//// HW Cursors

IOReturn IOFramebuffer::setCursorImage( void * cursorImage )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::setCursorState( SInt32 x, SInt32 y, bool visible )
{
    return (kIOReturnUnsupported);
}

void IOFramebuffer::flushCursor( void )
{}

//// Interrupts

IOReturn IOFramebuffer::registerForInterruptType( IOSelect interruptType,
        IOFBInterruptProc proc, OSObject * target, void * ref,
        void ** interruptRef )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::unregisterInterrupt( void * interruptRef )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::setInterruptState( void * interruptRef, UInt32 state )
{
    return (kIOReturnUnsupported);
}

// Apple sensing

IOReturn IOFramebuffer::getAppleSense(
    IOIndex  /* connectIndex */,
    UInt32 * /* senseType */,
    UInt32 * /* primary */,
    UInt32 * /* extended */,
    UInt32 * /* displayType */ )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::connectFlags( IOIndex /* connectIndex */,
                                      IODisplayModeID /* displayMode */, IOOptionBits * /* flags */ )
{
    return (kIOReturnUnsupported);
}

//// IOLowLevelDDCSense

void IOFramebuffer::setDDCClock( IOIndex /* connectIndex */, UInt32 /* value */ )
{}

void IOFramebuffer::setDDCData( IOIndex /* connectIndex */, UInt32 /* value */ )
{}

bool IOFramebuffer::readDDCClock( IOIndex /* connectIndex */ )
{
    return (false);
}

bool IOFramebuffer::readDDCData( IOIndex /* connectIndex */ )
{
    return (false);
}

IOReturn IOFramebuffer::enableDDCRaster( bool /* enable */ )
{
    return (kIOReturnUnsupported);
}


//// IOHighLevelDDCSense

bool IOFramebuffer::hasDDCConnect( IOIndex /* connectIndex */ )
{
    return (kIOReturnUnsupported);
}

IOReturn IOFramebuffer::getDDCBlock( IOIndex /* connectIndex */, UInt32 /* blockNumber */,
                                     IOSelect /* blockType */, IOOptionBits /* options */,
                                     UInt8 * /* data */, IOByteCount * /* length */ )
{
    return (kIOReturnUnsupported);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IODisplayParameterHandler

OSDefineMetaClassAndStructors(IOFramebufferParameterHandler, IODisplayParameterHandler)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOFramebufferParameterHandler * IOFramebufferParameterHandler::withFramebuffer( IOFramebuffer * framebuffer )
{
    IOFramebufferParameterHandler * handler;
    UInt32			    count = 0;

    if ((kIOReturnSuccess != framebuffer->getAttributeForConnection(
				    0, kConnectionDisplayParameterCount, &count)))
	return (0);

    handler = new IOFramebufferParameterHandler;
    if (handler && !handler->init())
    {
	handler->release();
	handler = 0;
    }
    if (handler)
	handler->fFramebuffer = framebuffer;

    return (handler);
}

void IOFramebufferParameterHandler::free()
{
    if (fDisplayParams)
	fDisplayParams->release();

    super::free();
}

bool IOFramebufferParameterHandler::setDisplay( IODisplay * display )
{
    IOReturn	     ret;
    UInt32	     count = 0;
    UInt32	     str[2];
    UInt32 *	     attributes;
    OSDictionary *   allParams;
    OSDictionary *   newDict = 0;
    OSDictionary *   oldParams;
    const OSSymbol * key;
    OSIterator *     iter;

    fDisplay = display;
    if (!fDisplay)
	return (false);

    allParams = OSDynamicCast(OSDictionary, display->copyProperty(gIODisplayParametersKey));
    if (allParams)
    {
	newDict = OSDictionary::withDictionary(allParams);
	allParams->release();
    }

    ret = fFramebuffer->getAttributeForConnection(
			    0, kConnectionDisplayParameterCount, &count);
    if (kIOReturnSuccess != ret)
	count = 0;

    oldParams = fDisplayParams;
    do
    {
	if (count)
	    fDisplayParams = OSDictionary::withCapacity(count);
	else
	    fDisplayParams = 0;
	if (!fDisplayParams)
	    continue;

	attributes = IONew(UInt32, count);
	if (!attributes)
	    continue;
    
	if (kIOReturnSuccess != fFramebuffer->getAttributeForConnection(
					0, kConnectionDisplayParameters, attributes))
	    continue;
    
	str[1] = 0;
	for (UInt32 i = 0; i < count; i++)
	{
	    const OSSymbol * sym;
	    UInt32		 value[16];

	    if (attributes[i] < 0x00ffffff)
		continue;

	    str[0] = attributes[i];
	    sym = OSSymbol::withCString((const char *) str);
	    if (!sym)
		continue;

	    if (kIOReturnSuccess == fFramebuffer->getAttributeForConnection(
					    0, attributes[i], &value[0]))
	    {
		IODisplay::addParameter(fDisplayParams, sym, value[1], value[2]);
		IODisplay::setParameter(fDisplayParams, sym, value[0]);
	    }
	    sym->release();
	}
    
	IODelete(attributes, UInt32, count);
    }
    while (false);

    if (oldParams)
    {
	if (newDict)
	{
	    iter = OSCollectionIterator::withCollection(oldParams);
	    if (iter)
	    {
		while ((key = (const OSSymbol *) iter->getNextObject()))
		{
		    if (!fDisplayParams || !fDisplayParams->getObject(key))
			newDict->removeObject(key);
		}
		iter->release();
	    }
	}
	oldParams->release();
    }

    if (newDict)
    {
	if (fDisplayParams)
	    newDict->merge(fDisplayParams);
	display->setProperty(gIODisplayParametersKey, newDict);
	newDict->release();
    }
    else if (fDisplayParams)
	display->setProperty(gIODisplayParametersKey, fDisplayParams);

    return (true);
}

void IOFramebufferParameterHandler::displayModeChange( void )
{
    if (fDisplay)
	setDisplay( fDisplay );
}

bool IOFramebufferParameterHandler::doIntegerSet( OSDictionary * params,
                               const OSSymbol * paramName, UInt32 value )
{
    UInt32	attribute;
    bool	ok;

    if (fDisplayParams && fDisplayParams->getObject(paramName))
    {
	attribute = *((UInt32 *) paramName->getCStringNoCopy());
    
	ok = (kIOReturnSuccess == fFramebuffer->setAttributeForConnection(
					0, attribute, value));
    }
    else
	ok = false;

    return (ok);
}

bool IOFramebufferParameterHandler::doDataSet( const OSSymbol * paramName, OSData * value )
{
    return (false);
}

bool IOFramebufferParameterHandler::doUpdate( void )
{
    bool ok = true;

    return (ok);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSMetaClassDefineReservedUnused(IOFramebuffer, 0);
OSMetaClassDefineReservedUnused(IOFramebuffer, 1);
OSMetaClassDefineReservedUnused(IOFramebuffer, 2);
OSMetaClassDefineReservedUnused(IOFramebuffer, 3);
OSMetaClassDefineReservedUnused(IOFramebuffer, 4);
OSMetaClassDefineReservedUnused(IOFramebuffer, 5);
OSMetaClassDefineReservedUnused(IOFramebuffer, 6);
OSMetaClassDefineReservedUnused(IOFramebuffer, 7);
OSMetaClassDefineReservedUnused(IOFramebuffer, 8);
OSMetaClassDefineReservedUnused(IOFramebuffer, 9);
OSMetaClassDefineReservedUnused(IOFramebuffer, 10);
OSMetaClassDefineReservedUnused(IOFramebuffer, 11);
OSMetaClassDefineReservedUnused(IOFramebuffer, 12);
OSMetaClassDefineReservedUnused(IOFramebuffer, 13);
OSMetaClassDefineReservedUnused(IOFramebuffer, 14);
OSMetaClassDefineReservedUnused(IOFramebuffer, 15);
OSMetaClassDefineReservedUnused(IOFramebuffer, 16);
OSMetaClassDefineReservedUnused(IOFramebuffer, 17);
OSMetaClassDefineReservedUnused(IOFramebuffer, 18);
OSMetaClassDefineReservedUnused(IOFramebuffer, 19);
OSMetaClassDefineReservedUnused(IOFramebuffer, 20);
OSMetaClassDefineReservedUnused(IOFramebuffer, 21);
OSMetaClassDefineReservedUnused(IOFramebuffer, 22);
OSMetaClassDefineReservedUnused(IOFramebuffer, 23);
OSMetaClassDefineReservedUnused(IOFramebuffer, 24);
OSMetaClassDefineReservedUnused(IOFramebuffer, 25);
OSMetaClassDefineReservedUnused(IOFramebuffer, 26);
OSMetaClassDefineReservedUnused(IOFramebuffer, 27);
OSMetaClassDefineReservedUnused(IOFramebuffer, 28);
OSMetaClassDefineReservedUnused(IOFramebuffer, 29);
OSMetaClassDefineReservedUnused(IOFramebuffer, 30);
OSMetaClassDefineReservedUnused(IOFramebuffer, 31);

