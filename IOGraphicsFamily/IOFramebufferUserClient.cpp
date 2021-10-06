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

#define IOFRAMEBUFFER_PRIVATE
#include <IOKit/graphics/IOFramebufferShared.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <libkern/c++/OSContainers.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#include <IOKit/IOPlatformExpert.h>

#include <IOKit/assert.h>

#include "IOFramebufferUserClient.h"

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOUserClient

OSDefineMetaClassAndStructors(IOFramebufferUserClient, IOUserClient)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOFramebufferUserClient * IOFramebufferUserClient::withTask( task_t owningTask )
{
    IOFramebufferUserClient * inst;

    inst = new IOFramebufferUserClient;

    if (inst && !inst->init())
    {
        inst->release();
        inst = 0;
    }

    return (inst);
}

bool IOFramebufferUserClient::start( IOService * _owner )
{
    if (!super::start(_owner))
        return (false);

    owner = (IOFramebuffer *) _owner;
    owner->serverConnect = this;

    return (true);
}

IOReturn IOFramebufferUserClient::registerNotificationPort(
    mach_port_t 	port,
    UInt32		type,
    UInt32		refCon )
{
    return (owner->extRegisterNotificationPort(port, type, refCon));
}

IOReturn IOFramebufferUserClient::getNotificationSemaphore(
    UInt32 interruptType, semaphore_t * semaphore )
{
    return (owner->getNotificationSemaphore(interruptType, semaphore));
}

// The window server is going away.

IOReturn IOFramebufferUserClient::clientClose( void )
{
    owner->close();
    detach( owner);

    return (kIOReturnSuccess);
}

IOService * IOFramebufferUserClient::getService( void )
{
    return (owner);
}

IOReturn IOFramebufferUserClient::clientMemoryForType( UInt32 type,
        IOOptionBits * flags, IOMemoryDescriptor ** memory )
{
    IOMemoryDescriptor *	mem;
    IOReturn		err;

    switch (type)
    {
        case kIOFBCursorMemory:
            mem = owner->sharedCursor;
            mem->retain();
            break;

        case kIOFBVRAMMemory:
            mem = owner->getVRAMRange();
            break;

        default:
            mem = (IOMemoryDescriptor *) owner->userAccessRanges->getObject( type );
            mem->retain();
            break;
    }

    *memory = mem;
    if (mem)
        err = kIOReturnSuccess;
    else
        err = kIOReturnBadArgument;

    return (err);
}

IOReturn IOFramebufferUserClient::setProperties( OSObject * properties )
{
    OSDictionary *	props;
    IOReturn		kr = kIOReturnUnsupported;

    if (!(props = OSDynamicCast(OSDictionary, properties)))
        return (kIOReturnBadArgument);

    kr = owner->extSetProperties( props );

    return (kr);
}

IOExternalMethod * IOFramebufferUserClient::getTargetAndMethodForIndex(
    IOService ** targetP, UInt32 index )
{
    static const IOExternalMethod methodTemplate[] =
    {
	/* 0 */  { NULL, (IOMethod) &IOFramebuffer::extCreateSharedCursor,
		    kIOUCScalarIScalarO, 3, 0 },
	/* 1 */  { NULL, (IOMethod) &IOFramebuffer::extGetPixelInformation,
		    kIOUCScalarIStructO, 3, sizeof( IOPixelInformation) },
	/* 2 */  { NULL, (IOMethod) &IOFramebuffer::extGetCurrentDisplayMode,
		    kIOUCScalarIScalarO, 0, 2 },
	/* 3 */  { NULL, (IOMethod) &IOFramebuffer::extSetStartupDisplayMode,
		    kIOUCScalarIScalarO, 2, 0 },
	/* 4 */  { NULL, (IOMethod) &IOFramebuffer::extSetDisplayMode,
		    kIOUCScalarIScalarO, 2, 0 },
	/* 5 */  { NULL, (IOMethod) &IOFramebuffer::extGetInformationForDisplayMode,
		    kIOUCScalarIStructO, 1, 0xffffffff },
	/* 6 */  { NULL, (IOMethod) &IOFramebuffer::extGetDisplayModeCount,
		    kIOUCScalarIScalarO, 0, 1 },
	/* 7 */  { NULL, (IOMethod) &IOFramebuffer::extGetDisplayModes,
		    kIOUCStructIStructO, 0, 0xffffffff },
	/* 8 */  { NULL, (IOMethod) &IOFramebuffer::extGetVRAMMapOffset,
		    kIOUCScalarIScalarO, 1, 1 },
	/* 9 */  { NULL, (IOMethod) &IOFramebuffer::extSetBounds,
		    kIOUCStructIStructO, sizeof( Bounds), 0 },
	/* 10 */  { NULL, (IOMethod) &IOFramebuffer::extSetNewCursor,
		    kIOUCScalarIScalarO, 3, 0 },
	/* 11 */  { NULL, (IOMethod) &IOFramebuffer::extSetGammaTable,
		    kIOUCScalarIStructI, 3, 0xffffffff },
	/* 12 */  { NULL, (IOMethod) &IOFramebuffer::extSetCursorVisible,
		    kIOUCScalarIScalarO, 1, 0 },
	/* 13 */  { NULL, (IOMethod) &IOFramebuffer::extSetCursorPosition,
		    kIOUCScalarIScalarO, 2, 0 },
	/* 14 */  { NULL, (IOMethod) &IOFramebuffer::extAcknowledgeNotification,
		    kIOUCScalarIScalarO, 0, 0 },
	/* 15 */  { NULL, (IOMethod) &IOFramebuffer::extSetColorConvertTable,
		    kIOUCScalarIStructI, 1, 0xffffffff },
	/* 16 */  { NULL, (IOMethod) &IOFramebuffer::extSetCLUTWithEntries,
		    kIOUCScalarIStructI, 2, 0xffffffff },
	/* 17 */  { NULL, (IOMethod) &IOFramebuffer::extValidateDetailedTiming,
		    kIOUCStructIStructO, 0xffffffff, 0xffffffff },
	/* 18 */  { NULL, (IOMethod) &IOFramebufferUserClient::getAttribute,
		    kIOUCScalarIScalarO, 1, 1 },
	/* 19 */  { NULL, (IOMethod) &IOFramebufferUserClient::setAttribute,
		    kIOUCScalarIScalarO, 2, 0 },
    };

    if (index > (sizeof(methodTemplate) / sizeof(methodTemplate[0])))
        return (NULL);

    if ((1 << index) & ((1<<18)|(1<<19)))
        *targetP = this;
    else
        *targetP = owner;

    return ((IOExternalMethod *)(methodTemplate + index));
}

IOReturn IOFramebufferUserClient::connectClient( IOUserClient * _other )
{
    other = OSDynamicCast(IOFramebuffer, _other->getService());

    if (_other && !other)
        return (kIOReturnBadArgument);
    else
        return (kIOReturnSuccess);
}

IOReturn IOFramebufferUserClient::getAttribute(
    IOSelect attribute, UInt32 * value )
{
    return (owner->extGetAttribute(attribute, value, other));
}

IOReturn IOFramebufferUserClient::setAttribute(
    IOSelect attribute, UInt32 value )
{
    return (owner->extSetAttribute(attribute, value, other));
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDefineMetaClassAndStructors(IOFramebufferSharedUserClient, IOUserClient)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOFramebufferSharedUserClient * IOFramebufferSharedUserClient::withTask(
    task_t owningTask )
{
    IOFramebufferSharedUserClient * inst;

    inst = new IOFramebufferSharedUserClient;

    if (inst && !inst->init())
    {
        inst->release();
        inst = 0;
    }

    return (inst);
}

bool IOFramebufferSharedUserClient::start( IOService * _owner )
{
    static const IOExternalMethod methodTemplate[] = {
            };

    if (!super::start(_owner))
        return (false);

    owner = (IOFramebuffer *) _owner;

    bcopy( methodTemplate, externals, sizeof( methodTemplate ));

    return (true);
}

void IOFramebufferSharedUserClient::free( void )
{
    retain();
    retain();
    owner->sharedConnect = 0;
    detach( owner);
    super::free();
}

void IOFramebufferSharedUserClient::release() const
{
    super::release(2);
}

IOReturn IOFramebufferSharedUserClient::clientClose( void )
{
    return (kIOReturnSuccess);
}

IOService * IOFramebufferSharedUserClient::getService( void )
{
    return (owner);
}

IOReturn IOFramebufferSharedUserClient::clientMemoryForType( UInt32 type,
        IOOptionBits * options, IOMemoryDescriptor ** memory )
{
    IOMemoryDescriptor *	mem = 0;
    IOReturn			err;

    switch (type)
    {
        case kIOFBCursorMemory:
            mem = owner->sharedCursor;
            mem->retain();
            *options = kIOMapReadOnly;
            break;

        case kIOFBVRAMMemory:
            mem = owner->getVRAMRange();
            break;
    }

    *memory = mem;
    if (mem)
        err = kIOReturnSuccess;
    else
        err = kIOReturnBadArgument;

    return (err);
}

IOReturn IOFramebufferSharedUserClient::getNotificationSemaphore(
    UInt32 interruptType, semaphore_t * semaphore )
{
    return (owner->getNotificationSemaphore(interruptType, semaphore));
}

IOExternalMethod * IOFramebufferSharedUserClient::getExternalMethodForIndex( UInt32 index )
{
    if (index < (sizeof(externals) / sizeof(externals[0])))
        return (externals + index);
    else
        return (NULL);
}

