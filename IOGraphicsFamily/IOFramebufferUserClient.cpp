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

#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <libkern/c++/OSContainers.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOHibernatePrivate.h>
#include <IOKit/assert.h>

#define IOFRAMEBUFFER_PRIVATE
#include <IOKit/graphics/IOFramebufferShared.h>
#include <IOKit/graphics/IOGraphicsPrivate.h>

#include "IOFramebufferUserClient.h"

#include "IOGraphicsKTrace.h"

#define COUNT_OF(x) \
    ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))
#define IOUCACTION(f) static_cast<IOExternalMethodAction>(f)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOUserClient

OSDefineMetaClassAndStructors(IOFramebufferUserClient, IOUserClient)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define RPC_GUARD(inst, traceId)                                               \
    RpcGuard rpcGuard(inst);                                                   \
    if (rpcGuard.fReturn) {                                                    \
        IOFBUC_END(traceId, rpcGuard.fReturn, __LINE__, 0);                    \
        return rpcGuard.fReturn;                                               \
    }

IOReturn IOFramebufferUserClient::RpcGuard::rpcEnter()
{
    IOFBUC_START(rpcEnter, 0, 0, 0);

    // Prevent fActive from increasing once termination starts.
    if (fUC->fTerminating) {
        IOFBUC_END(rpcEnter, kIOReturnOffline, fUC->fActive, __LINE__);
        return kIOReturnOffline;
    }
    
    // Atomically increment fActive iff fActive >= 0 before the increment.
    int32_t was;
    do {
        was = fUC->fActive;
        if (-1 == was) { // last thread has left; no new entries allowed
            DEBG(fUC->fName, " !active\n");
            IOFBUC_END(rpcEnter, kIOReturnOffline, was, __LINE__);
            return kIOReturnOffline;
        }
    } while (!OSCompareAndSwap(was, was + 1, &fUC->fActive));

    IOFBUC_END(rpcEnter, kIOReturnSuccess, was, __LINE__);
    return kIOReturnSuccess;
}

void IOFramebufferUserClient::RpcGuard::rpcLeave()
{
    IOFBUC_START(rpcLeave, 0, 0, 0);

    if (fReturn) {
        IOFBUC_END(rpcLeave, __LINE__, 0, 0);
        return;
    }

    // Allow provider to continue termination if this is the last thread out.
    int was;
    if (0 == (was = OSDecrementAtomic(&fUC->fActive))) { // was 0, now -1: last thread out
        bool defer = false;
        DEBG(fUC->fName, " didTerminate(%p)\n", fUC->fOwner);
        fUC->super::didTerminate(fUC->fOwner, 0, &defer);
    }

    IOFBUC_END(rpcLeave, __LINE__, was, 0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOFramebufferUserClient * IOFramebufferUserClient::withTask( task_t owningTask )
{
    IOFBUC_START(withTask,0,0,0);

    DEBG("IOGfbUC", "\n");

    IOFramebufferUserClient * inst = NULL;

    inst = new IOFramebufferUserClient;

    if (inst && !inst->init())
    {
        inst->release();
        inst = NULL;
    }

    IOFBUC_END(withTask,0,0,0);
    return (inst);
}

void IOFramebufferUserClient::free(void)
{
    IOFBUC_START(free,0,0,0);
    DEBG(fName, "\n");
    OSSafeReleaseNULL(fOther);
    super::free();
    IOFBUC_END(free,0,0,0);
}

bool IOFramebufferUserClient::start( IOService * _owner )
{
    IOFBUC_START(start,0,0,0);
    fOwner = OSDynamicCast(IOFramebuffer, _owner);
    if (NULL != fOwner)
    {
        fOwner->fServerConnect = this;
#if RLOG
        snprintf(fName, sizeof(fName), "%s-UC", fOwner->thisName);
#endif
        DEBG(fName, "\n");
        IOFBUC_END(start,true,0,0);
        return (true);
    }
    IOFBUC_END(start,false,0,0);
    return (false);
}

IOReturn IOFramebufferUserClient::
registerNotificationPort(mach_port_t         port,
                         UInt32              type,
                         UInt32              refCon )
{
    IOFBUC_START(registerNotificationPort,port,type,0);
    DEBG(fName, "\n");
    RPC_GUARD(this, registerNotificationPort);
    IOReturn status = (fOwner->extRegisterNotificationPort(port, type, refCon));
    IOFBUC_END(registerNotificationPort,status,0,0);
    return status;
}

// The window server is going away.
// Purposely not RPC_GUARD'd.
// Must be safe to race stop().
IOReturn IOFramebufferUserClient::clientClose( void )
{
    IOFBUC_START(clientClose,0,0,0);
    DEBG(fName, "\n");
    IOFramebuffer *owner = fOwner;
    if (owner && OSCompareAndSwapPtr(owner, NULL, &fOwner)) {
        owner->close();
    }
    if (!isInactive()) terminate();
    IOFBUC_END(clientClose,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

bool IOFramebufferUserClient::willTerminate(IOService *provider, IOOptionBits options)
{
    IOFBUC_START(willTerminate,0,0,0);
    DEBG(fName, " provider=%p options=%#x\n", provider, (uint32_t)options);
    assert(fOwner == provider);
    fTerminating = true;
    bool status = super::willTerminate(provider, options);
    IOFBUC_END(willTerminate,status,0,0);
    return status;
}

bool IOFramebufferUserClient::didTerminate(IOService *provider, IOOptionBits options, bool *defer)
{
    IOFBUC_START(didTerminate,0,0,0);
    assert(fOwner == provider);
    // Defer if any threads are active. OSDecrementAtomic returns previous value.
    *defer = (0 != OSDecrementAtomic(&fActive));
    DEBG(fName, " provider=%p options=%#x defer=%d\n", provider, (uint32_t)options, *defer);
    bool status = IOUserClient::didTerminate(provider, options, defer);
    IOFBUC_END(didTerminate,status,0,0);
    return status;
}

// Don't terminate; we want to keep our RPCs available to WS until it lets go.
bool IOFramebufferUserClient::requestTerminate(IOService *provider, IOOptionBits options)
{
    IOFBUC_START(requestTerminate,0,0,0);
    DEBG(fName, " provider=%p options=%#x -> false\n", provider, (uint32_t)options);
    IOFBUC_END(requestTerminate,0,0,0);
    return false;
}

bool IOFramebufferUserClient::terminate(IOOptionBits options)
{
    IOFBUC_START(terminate,0,0,0);
    DEBG(fName, "\n");
    bool status = super::terminate(options);
    IOFBUC_END(terminate,status,0,0);
    return status;
}

bool IOFramebufferUserClient::finalize(IOOptionBits options)
{
    IOFBUC_START(finalize,0,0,0);
    DEBG(fName, " options=%#x\n", (uint32_t)options);
    bool status = super::finalize(options);
    IOFBUC_END(finalize,status,0,0);
    return status;
}

// Must be safe to race clientClose()
void IOFramebufferUserClient::stop(IOService *provider)
{
    IOFBUC_START(stop,0,0,0);
    DEBG(fName, " provider=%p\n", provider);
    IOFramebuffer *owner = fOwner;
    if (owner && OSCompareAndSwapPtr(owner, NULL, &fOwner)) {
        // Intentionally not calling owner->close() here. We're on the FB
        // WL so taking SYS WL might deadlock!
        owner->closeNoSys();
    }
    super::stop(provider);
    IOFBUC_END(stop,0,0,0);
}

IOService * IOFramebufferUserClient::getService( void )
{
    IOFBUC_START(getService,0,0,0);
    DEBG(fName, "\n");
    IOFBUC_END(getService,0,0,0);
    return (fOwner);
}

IOReturn IOFramebufferUserClient::clientMemoryForType( UInt32 type,
                                                      IOOptionBits * flags, IOMemoryDescriptor ** memory )
{
    IOFBUC_START(clientMemoryForType,type,0,0);
    DEBG(fName, "\n");

    static bool          havePublishedResource;
    IOMemoryDescriptor * mem = NULL;
    IOReturn             err = kIOReturnSuccess;

    RPC_GUARD(this, clientMemoryForType);

    switch (type)
    {
        case kIOFBCursorMemory:
            if (!havePublishedResource)
            {
                havePublishedResource = true;
                publishResource("WindowServer");
            }
            err = fOwner->extCopySharedCursor(&mem);
            break;

        case kIOFBVRAMMemory:
            FB_START(getVRAMRange,0,__LINE__,0);
            mem = fOwner->getVRAMRange();
            FB_END(getVRAMRange,0,__LINE__,0);
            break;

        default:
            err = fOwner->extCopyUserAccessObject(type, &mem);
            break;
    }

    if (!err) {
        *memory = mem;
        err = (mem) ? kIOReturnSuccess : kIOReturnBadArgument;
    }

    IOFBUC_END(clientMemoryForType,err,0,0);
    return (err);
}

IOReturn IOFramebufferUserClient::setProperties( OSObject * properties )
{
    IOFBUC_START(setProperties,0,0,0);
    DEBG(fName, "\n");

    OSDictionary *      props;
    IOReturn            kr = kIOReturnUnsupported;

    if (!(props = OSDynamicCast(OSDictionary, properties)))
    {
        IOFBUC_END(setProperties,kIOReturnBadArgument,0,0);
        return (kIOReturnBadArgument);
    }

    RPC_GUARD(this, setProperties);
    kr = fOwner->extSetProperties( props );

    IOFBUC_END(setProperties,kr,0,0);
    return (kr);
}

IOReturn IOFramebufferUserClient::externalMethod( uint32_t selector, IOExternalMethodArguments * args,
                                                 IOExternalMethodDispatch * dispatch, OSObject * target, void * reference )
{
    IOFBUC_START(externalMethod,selector,0,0);
    IOReturn    ret = kIOReturnSuccess;

    static const IOExternalMethodDispatch methodTemplate[] =
    {
        /*[0]*/  { (IOExternalMethodAction) &IOFramebuffer::extCreateSharedCursor,
            3, 0, 0, 0 },
        /*[1]*/  { (IOExternalMethodAction) &IOFramebuffer::extGetPixelInformation,
            3, 0, 0, sizeof(IOPixelInformation) },
        /*[2]*/  { (IOExternalMethodAction) &IOFramebuffer::extGetCurrentDisplayMode,
            0, 0, 2, 0 },
        /*[3]*/  { (IOExternalMethodAction) &IOFramebuffer::extSetStartupDisplayMode,
            2, 0, 0, 0 },
        /*[4]*/  { (IOExternalMethodAction) &IOFramebuffer::extSetDisplayMode,
            2, 0, 0, 0 },
        /*[5]*/  { (IOExternalMethodAction) &IOFramebuffer::extGetInformationForDisplayMode,
            1, 0, 0, kIOUCVariableStructureSize },
        /*[6]*/  { (IOExternalMethodAction) &IOFramebuffer::extGetDisplayModeCount,
            0, 0, 1, 0 },
        /*[7]*/  { (IOExternalMethodAction) &IOFramebuffer::extGetDisplayModes,
            0, 0, 0, kIOUCVariableStructureSize },
        /*[8]*/  { (IOExternalMethodAction) &IOFramebuffer::extGetVRAMMapOffset,
            1, 0, 1, 0 },
        /*[9]*/  { (IOExternalMethodAction) &IOFramebuffer::extSetBounds,
            0, kIOUCVariableStructureSize, 0, 0 },
        /*[10]*/ { (IOExternalMethodAction) &IOFramebuffer::extSetNewCursor,
            3, 0, 0, 0 },
        /*[11]*/ { (IOExternalMethodAction) &IOFramebuffer::extSetGammaTable,
            5, kIOUCVariableStructureSize, 0, 0 },
        /*[12]*/ { (IOExternalMethodAction) &IOFramebuffer::extSetCursorVisible,
            1, 0, 0, 0 },
        /*[13]*/ { (IOExternalMethodAction) &IOFramebuffer::extSetCursorPosition,
            2, 0, 0, 0 },
        /*[14]*/ { (IOExternalMethodAction) &IOFramebuffer::extAcknowledgeNotification,
            kIOUCVariableStructureSize, 0, 0, 0 },
        /*[15]*/ { (IOExternalMethodAction) &IOFramebuffer::extSetColorConvertTable,
            1, kIOUCVariableStructureSize, 0, 0 },
        /*[16]*/ { (IOExternalMethodAction) &IOFramebuffer::extSetCLUTWithEntries,
            2, kIOUCVariableStructureSize, 0, 0 },
        /*[17]*/ { (IOExternalMethodAction) &IOFramebuffer::extValidateDetailedTiming,
            0, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize },
        /*[18]*/ { (IOExternalMethodAction) &IOFramebuffer::extGetAttribute,
            1, 0, 1, 0 },
        /*[19]*/ { (IOExternalMethodAction) &IOFramebuffer::extSetAttribute,
            2, 0, 0, 0 },
        /*[20]*/ { (IOExternalMethodAction) &IOFramebuffer::extSetHibernateGammaTable,
            3, kIOUCVariableStructureSize, 0, 0 },
    };

    if (selector >= COUNT_OF(methodTemplate))
    {
        IOFBUC_END(externalMethod,kIOReturnBadArgument,0,0);
        return (kIOReturnBadArgument);
    }

    RPC_GUARD(this, externalMethod);
    ret = super::externalMethod(selector, args,
                    const_cast<IOExternalMethodDispatch *>(&methodTemplate[selector]), 
                    fOwner, fOther);

    IOFBUC_END(externalMethod,ret,0,0);
    return (ret);
}

IOReturn IOFramebufferUserClient::connectClient( IOUserClient * _other )
{
    IOFBUC_START(connectClient,0,0,0);
    RPC_GUARD(this, connectClient);
    OSSafeReleaseNULL(fOther);
    if (!_other) {
        IOFBUC_END(connectClient,kIOReturnSuccess,__LINE__,0);
        return kIOReturnSuccess;
    }
    fOther = OSDynamicCast(IOFramebuffer, _other->getService());
    if (!fOther) {
        IOFBUC_END(connectClient,kIOReturnBadArgument,__LINE__,0);
        return kIOReturnBadArgument;
    }
    fOther->retain();
    DEBG(fName, " %s\n", fOther->thisName);
    IOFBUC_END(connectClient,kIOReturnSuccess,__LINE__,0);
    return kIOReturnSuccess;
}

#pragma mark - IOFramebufferSharedUserClient -
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef RPC_GUARD
#define RPC_GUARD(inst, traceId)                                               \
    RpcGuard rpcGuard(inst);                                                   \
    if (rpcGuard.fReturn) {                                                    \
        IOFBSUC_END(traceId, rpcGuard.fReturn, __LINE__, 0);                   \
        return rpcGuard.fReturn;                                               \
    }

IOReturn IOFramebufferSharedUserClient::RpcGuard::rpcEnter()
{
    IOFBSUC_START(rpcEnter, 0, 0, 0);

    // Prevent fActive from increasing once termination starts.
    if (fUC->fTerminating) {
        IOFBSUC_END(rpcEnter, kIOReturnOffline, fUC->fActive, __LINE__);
        return kIOReturnOffline;
    }
    
    // Atomically increment fActive iff fActive >= 0 before the increment.
    int32_t was;
    do {
        was = fUC->fActive;
        if (-1 == was) { // last thread has left; no new entries allowed
            DEBG(fUC->fName, " !active\n");
            IOFBSUC_END(rpcEnter, kIOReturnOffline, was, __LINE__);
            return kIOReturnOffline;
        }
    } while (!OSCompareAndSwap(was, was + 1, &fUC->fActive));

    IOFBSUC_END(rpcEnter, kIOReturnSuccess, was, __LINE__);
    return kIOReturnSuccess;
}

void IOFramebufferSharedUserClient::RpcGuard::rpcLeave()
{
    IOFBSUC_START(rpcLeave, 0, 0, 0);

    if (fReturn) {
        IOFBSUC_END(rpcLeave, __LINE__, 0, 0);
        return;
    }

    // Allow provider to continue termination if this is the last thread out.
    int was;
    if (0 == (was = OSDecrementAtomic(&fUC->fActive))) { // was 0, now -1: last thread out
        bool defer = false;
        DEBG(fUC->fName, " didTerminate(%p)\n", fUC->fOwner);
        fUC->super::didTerminate(fUC->fOwner, 0, &defer);
    }

    IOFBSUC_END(rpcLeave, __LINE__, was, 0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#pragma mark - IOFramebufferSharedUserClient -
OSDefineMetaClassAndStructors(IOFramebufferSharedUserClient, IOUserClient)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOFramebufferSharedUserClient *
IOFramebufferSharedUserClient::withTask(task_t /* owningTask */)
{
    IOFBSUC_START(withTask,0,0,0);
    IOFramebufferSharedUserClient   * inst = NULL;

    DEBG("IOGSUC", "\n");

    inst = new IOFramebufferSharedUserClient;

    if (inst && !inst->init())
    {
        DEBG("IOGSUC", " init failed\n");
        OSSafeReleaseNULL(inst);
    }

    IOFBSUC_END(withTask,0,0,0);
    return (inst);
}

bool IOFramebufferSharedUserClient::start( IOService * _owner )
{
    IOFBSUC_START(start,0,0,0);

    fOwner = OSDynamicCast(IOFramebuffer, _owner);
    if (!fOwner) {
        DEBG("IOGUC", " _owner not IOFB\n");
        IOFBSUC_END(start,false,__LINE__,0);
        return false;
    }

#if RLOG
    snprintf(fName, sizeof(fName), "%s-SC", fOwner->thisName);
#endif

    DEBG(fName, "\n");

    assert(fOwner->getWorkLoop()->inGate());
    fOwner->fSharedConnect = this;

    setProperty(kIOUserClientSharedInstanceKey, kOSBooleanTrue);

    IOFBSUC_END(start,true,0,0);
    return (true);
}

void IOFramebufferSharedUserClient::free( void )
{
    IOFBSUC_START(free,0,0,0);
    DEBG(fName, "\n");
    super::free();
    IOFBSUC_END(free,0,0,0);
}

bool IOFramebufferSharedUserClient::requestTerminate(IOService *provider, IOOptionBits options)
{
    IOFBSUC_START(requestTerminate,0,0,0);
    DEBG(fName, " provider=%p options=%#x -> true\n", provider, (uint32_t)options);
    IOFBSUC_END(requestTerminate,true,0,0);
    return true;
}

bool IOFramebufferSharedUserClient::terminate(IOOptionBits options)
{
    IOFBSUC_START(terminate,0,0,0);
    DEBG(fName, "\n");
    bool status = super::terminate(options);
    IOFBSUC_END(terminate,status,0,0);
    return status;
}

bool IOFramebufferSharedUserClient::finalize(IOOptionBits options)
{
    IOFBSUC_START(finalize,0,0,0);
    DEBG(fName, " options=%#x\n", (uint32_t)options);
    bool status = super::finalize(options);
    IOFBSUC_END(finalize,status,0,0);
    return status;
}

void IOFramebufferSharedUserClient::stop(IOService *provider)
{
    IOFBSUC_START(stop,0,0,0);
    DEBG(fName, " provider=%p\n", provider);

    assert(OSDynamicCast(IOFramebuffer, provider));
    assert(provider->getWorkLoop()->inGate());

    // We hold off stop() until no user threads are inside any of the RPCs.
    // After stop() we shouldn't be calling into provider, so clear them.
    fOwner->fSharedConnect = NULL;
    fOwner = NULL;

    super::stop(provider);
    IOFBSUC_END(stop,0,0,0);
}

IOService * IOFramebufferSharedUserClient::getService( void )
{
    IOFBSUC_START(getService,0,0,0);
    DEBG(fName, "\n");
    IOFBSUC_END(getService,0,0,0);
    return (fOwner);
}

bool IOFramebufferSharedUserClient::willTerminate(IOService *provider, IOOptionBits options)
{
    IOFBSUC_START(willTerminate,0,0,0);
    DEBG(fName, " provider=%p options=%#x\n", provider, (uint32_t)options);
    assert(fOwner == provider);
    fTerminating = true;
    bool status = super::willTerminate(provider, options);
    IOFBSUC_END(willTerminate,status,0,0);
    return status;
}

bool IOFramebufferSharedUserClient::didTerminate(IOService *provider, IOOptionBits options, bool *defer)
{
    IOFBSUC_START(didTerminate,0,0,0);
    assert(fOwner == provider);
    // Defer if any threads are active. OSDecrementAtomic returns previous value.
    *defer = (0 != OSDecrementAtomic(&fActive));
    DEBG(fName, " provider=%p options=%#x defer=%d\n", provider, (uint32_t)options, *defer);
    bool status = super::didTerminate(provider, options, defer);
    IOFBSUC_END(didTerminate,status,0,0);
    return status;
}

IOReturn IOFramebufferSharedUserClient::clientMemoryForType( UInt32 type,
                                                            IOOptionBits * options, IOMemoryDescriptor ** memory )
{
    IOMemoryDescriptor *        mem = NULL;
    IOReturn                    err = kIOReturnSuccess;

    IOFBSUC_START(clientMemoryForType,type,0,0);
    RPC_GUARD(this, clientMemoryForType);
    DEBG(fName, "\n");

    switch (type)
    {
        case kIOFBCursorMemory:
            err = fOwner->extCopySharedCursor(&mem);
            *options = kIOMapReadOnly;
            break;

        case kIOFBVRAMMemory:
            if (kIOReturnSuccess == clientHasPrivilege(current_task(), kIOClientPrivilegeLocalUser))
            {
                FB_START(getVRAMRange,0,__LINE__,0);
                mem = fOwner->getVRAMRange();
                FB_END(getVRAMRange,0,__LINE__,0);
            }
            break;
    }
    
    if (!err) {
        *memory = mem;
        err = (mem) ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    
    IOFBSUC_END(clientMemoryForType,err,0,0);
    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma mark - IOFramebufferDiagnosticUserClient -
OSDefineMetaClassAndStructors(IOFramebufferDiagnosticUserClient, IOUserClient)

#undef RPC_GUARD
#define RPC_GUARD(inst, traceId)                                               \
    RpcGuard rpcGuard(inst);                                                   \
    if (rpcGuard.fReturn) {                                                    \
        IOFBDUC_END(traceId, rpcGuard.fReturn, __LINE__, 0);                   \
        return rpcGuard.fReturn;                                               \
    }

IOReturn IOFramebufferDiagnosticUserClient::RpcGuard::rpcEnter()
{
    IOFBDUC_START(rpcEnter, 0, 0, 0);

    // Prevent fActive from increasing once termination starts.
    if (fUC->fTerminating) {
        IOFBDUC_END(rpcEnter, kIOReturnOffline, fUC->fActive, __LINE__);
        return kIOReturnOffline;
    }

    // Atomically increment fActive iff fActive >= 0 before the increment.
    int32_t was;
    do {
        was = fUC->fActive;
        if (-1 == was) { // last thread has left; no new entries allowed
            DEBG(fUC->fName, " !active\n");
            IOFBDUC_END(rpcEnter, kIOReturnOffline, was, __LINE__);
            return kIOReturnOffline;
        }
    } while (!OSCompareAndSwap(was, was + 1, &fUC->fActive));

    IOFBDUC_END(rpcEnter, kIOReturnSuccess, was, __LINE__);
    return kIOReturnSuccess;
}

void IOFramebufferDiagnosticUserClient::RpcGuard::rpcLeave()
{
    IOFBDUC_START(rpcLeave, 0, 0, 0);

    if (fReturn) {
        IOFBDUC_END(rpcLeave, __LINE__, 0, 0);
        return;
    }

    // Allow provider to continue termination if this is the last thread out.
    int was;
    if (0 == (was = OSDecrementAtomic(&fUC->fActive))) {
        // was 0, now -1: last thread out
        bool defer = false;
        DEBG(fUC->fName, " didTerminate(%p)\n", fUC->fOwner);
        fUC->super::didTerminate(fUC->fOwner, 0, &defer);
    }

    IOFBDUC_END(rpcLeave, __LINE__, was, 0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOFramebufferDiagnosticUserClient *IOFramebufferDiagnosticUserClient::
client()
{
    IOFBDUC_START(client,0,0,0);
    IOFramebufferDiagnosticUserClient   * inst = NULL;

    DEBG("IOGDUC", "\n");

    inst = new IOFramebufferDiagnosticUserClient;
    if (inst && !inst->init())
    {
        DEBG("IOGDUC", " init failed\n");
        OSSafeReleaseNULL(inst);
    }

    IOFBDUC_END(client,0,0,0);
    return (inst);
}

bool IOFramebufferDiagnosticUserClient::start(IOService *provider)
{
    IOFBDUC_START(start,0,0,0);

    fOwner = OSDynamicCast(IOFramebuffer, provider);
    bool ret = fOwner;
    if (fOwner) {
#if RLOG
        snprintf(fName, sizeof(fName), "%s-DC", fOwner->thisName);
        DEBG(fName, "\n");
#endif
    }
    else
        DEBG("IOGDUC", " _owner not IOFB\n");

    IOFBDUC_END(start,ret,0,0);
    return ret;
}

IOReturn IOFramebufferDiagnosticUserClient::clientClose()
{
    IOFBDUC_START(clientClose,0,0,0);
    DEBG(fName, "\n");
    if (!isInactive())
        terminate();
    IOFBDUC_END(clientClose,kIOReturnSuccess,0,0);
    return kIOReturnSuccess;
}

// Override IOService::requestTerminates recursive terminate we must be a leaf
bool IOFramebufferDiagnosticUserClient::
requestTerminate(IOService *provider, IOOptionBits options)
{
    IOFBDUC_START(requestTerminate,0,0,0);
    DEBG(fName, " provider=%p options=%#x -> true\n",
         provider, (uint32_t)options);
    IOFBDUC_END(requestTerminate,true,0,0);
    return true;
}

void IOFramebufferDiagnosticUserClient::stop(IOService *provider)
{
    IOFBDUC_START(stop,0,0,0);
    DEBG(fName, " provider=%p\n", provider);

    assert(OSDynamicCast(IOFramebuffer, provider));
    assert(provider->getWorkLoop()->inGate());

    fOwner = NULL;

    super::stop(provider);
    IOFBDUC_END(stop,0,0,0);
}

bool IOFramebufferDiagnosticUserClient::
willTerminate(IOService *provider, IOOptionBits options)
{
    IOFBDUC_START(willTerminate,0,0,0);
    DEBG(fName, " provider=%p options=%#x\n", provider, (uint32_t)options);
    assert(fOwner == provider);
    fTerminating = true;
    bool status = super::willTerminate(provider, options);
    IOFBDUC_END(willTerminate,status,0,0);
    return status;
}

bool IOFramebufferDiagnosticUserClient::
didTerminate(IOService *provider, IOOptionBits options, bool *defer)
{
    IOFBDUC_START(didTerminate,0,0,0);
    assert(fOwner == provider);
    // Defer if any threads are active. OSDecrementAtomic returns previous value
    *defer = (0 != OSDecrementAtomic(&fActive));
    DEBG(fName, " provider=%p options=%#x defer=%d\n",
         provider, (uint32_t)options, *defer);
    bool status = super::didTerminate(provider, options, defer);
    IOFBDUC_END(didTerminate,status,0,0);
    return status;
}

IOReturn IOFramebufferDiagnosticUserClient::
externalMethod(uint32_t selector, IOExternalMethodArguments *args,
               IOExternalMethodDispatch *dispatch, OSObject *target,
               void *reference)
{
    static const IOExternalMethodDispatch methodTemplate[] =
    {
        // Private
        /*[0]*/ { (IOExternalMethodAction) &IOFramebuffer::extDiagnose,
            2, 0, 0, sizeof(IOGDiagnose) },
        /*[1]*/ { (IOExternalMethodAction) &IOFramebuffer::extReservedB,
            0, 0, 0, 0 },
        /*[2]*/ { (IOExternalMethodAction) &IOFramebuffer::extReservedC,
            0, 0, 0, 0 },
        /*[3]*/ { (IOExternalMethodAction) &IOFramebuffer::extReservedD,
            0, 0, 0, 0 },
        /*[4]*/ { (IOExternalMethodAction) &IOFramebuffer::extReservedE,
            4, 0, 0, kIOUCVariableStructureSize },
    };

    IOFBDUC_START(externalMethod,selector,0,0);
    IOReturn ret = kIOReturnSuccess;

    const auto& method = methodTemplate[selector]; // alias
    if (selector >= COUNT_OF(methodTemplate)
    ||  !static_cast<bool>(method.function))
        ret = kIOReturnBadArgument;
    else {
        // Due to the architecture of shared user clients and specifically
        // their lifetime, Admin priviledge determination only works on the
        // first instance, first call.  Local works every time, but limits tool
        // usage for those cases where SSH is required.  Data is compressed
        // internally and decompressed by the tool, so we'll avoid any checks
        // at the moment.
#if 0
        ret = clientHasPrivilege(current_task(),
                                 kIOClientPrivilegeAdministrator);
#elif 0
        ret = clientHasPrivilege(current_task(), kIOClientPrivilegeLocalUser);
#endif

        dispatch
            = const_cast<IOExternalMethodDispatch *>(&methodTemplate[selector]);
        RPC_GUARD(this, externalMethod);
        ret = super::externalMethod(selector, args, dispatch, fOwner, NULL);
    }

    DEBG(fName, "(selector %u) -> ret %x\n", selector, ret);
    IOFBDUC_END(externalMethod,ret,0,0);
    return (ret);
}
