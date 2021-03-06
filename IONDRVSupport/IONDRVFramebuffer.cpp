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

#include <IOKit/IOLib.h>
#include <IOKit/platform/ApplePlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOHibernatePrivate.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#include <IOKit/pci/IOAGPDevice.h>
#pragma clang diagnostic pop
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/assert.h>

#include <libkern/c++/OSContainers.h>
#include <string.h>

#include <IOKit/graphics/IOGraphicsPrivate.h>
#include <IOKit/graphics/IOGraphicsInterfaceTypes.h>
#include <IOKit/ndrvsupport/IONDRVFramebuffer.h>
#include <IOKit/i2c/IOI2CInterface.h>

#include "IONDRV.h"
#include "IONDRVFramebufferPrivate.h"

#include "IOGraphicsKTrace.h"

#define sizeof32(x) ((int)sizeof(x))

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

extern "C" IOReturn _IONDRVLibrariesInitialize( IOService * provider );
extern "C" IOReturn _IONDRVLibrariesFinalize( IOService * provider );

#if RLOG
#define IONDRVCHECK     1
#endif
#define IONDRVCHECK     1
#define IONDRVI2CLOG    0
#define TIME_LOGS       0

#define kFirstDepth     kDepthMode1

enum
{
    kModePreflight = 1,
    kDisplayModeIDPreflight = kDisplayModeIDReservedBase + 1000
};

#define arbMode2Index(index)    \
    (index & 0x3ff)

#if TIME_LOGS

#define TIMESTART()									\
{													\
    AbsoluteTime startTime, endTime;				\
    uint64_t nsec;									\
	AbsoluteTime_to_scalar(&startTime) = mach_absolute_time();

#define TIMEEND(name, fmt, args...)								\
    AbsoluteTime_to_scalar(&endTime) = mach_absolute_time();    \
    absolutetime_to_nanoseconds(endTime, &nsec);                  \
    kprintf("%08d [%s]: ", (uint32_t) (nsec / 1000000ULL), name); \
    SUB_ABSOLUTETIME(&endTime, &startTime);			\
    absolutetime_to_nanoseconds(endTime, &nsec);	\
    nsec /= 1000000ULL;								\
    kprintf(fmt, ## args , nsec);     \
}


#else	/* !TIME_LOGS */

#define TIMESTART()
#define TIMEEND(name, fmt, args...)

#endif	


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct IONDRVFramebufferPrivate
{
    IOOptionBits                displayConnectFlags;
    unsigned int                ackConnectChange:1;
    unsigned int                postWakeProbe:1;
    unsigned int                i2cPowerState:1;
    unsigned int                deferCLUTSet:1;
    unsigned int                removable:1;
    unsigned int                pad1:27;
    UInt32                      desiredGammaWidth;
    UInt32                      desiredGammaCount;
    IOTimerEventSource *        probeInterrupt;
    UInt32                      currentModeTiming;
    UInt32                      reducedSpeed;
    IODisplayModeID             depthMapModeID;
    UInt8                       indexToDepthMode[kDepthMode6 - kDepthMode1 + 1];
    UInt8                       depthModeToIndex[kDepthMode6 - kDepthMode1 + 1];
    IOPhysicalAddress64         physicalFramebuffer;

};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOBootNDRV : public IONDRV
{
    OSDeclareDefaultStructors(IOBootNDRV)

public:
    enum { kIOBootNDRVDisplayMode = 100 };

    IOMemoryDescriptor * fVRAMDesc;
    UInt32               fRowBytes;
    UInt32               fWidth;
    UInt32               fHeight;
    UInt32               fBitsPerPixel;

    static IONDRV * fromRegistryEntry( IORegistryEntry * regEntry );

    virtual void free( void ) APPLE_KEXT_OVERRIDE;

    virtual IOReturn getSymbol( const char * symbolName,
                                IOLogicalAddress * address ) APPLE_KEXT_OVERRIDE;

    virtual const char * driverName( void ) APPLE_KEXT_OVERRIDE;

    virtual IOReturn doDriverIO( UInt32 commandID, void * contents,
                                 UInt32 commandCode, UInt32 commandKind ) APPLE_KEXT_OVERRIDE;

private:

    static bool getUInt32Property( IORegistryEntry * regEntry, const char * name,
                                   UInt32 * result );
    IOReturn doControl( UInt32 code, void * params );
    IOReturn doStatus( UInt32 code, void * params );
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* generic nub for multihead devices */

class IONDRVDevice : public IOPlatformDevice
{
    OSDeclareDefaultStructors(IONDRVDevice)

public:
    virtual bool compareName( OSString * name, OSString ** matched = 0 ) const APPLE_KEXT_OVERRIDE;
    virtual IOService * matchLocation( IOService * client ) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getResources( void ) APPLE_KEXT_OVERRIDE;
    virtual void joinPMtree( IOService * driver ) APPLE_KEXT_OVERRIDE;
};

#undef super
#define super IOPlatformDevice

OSDefineMetaClassAndStructors(IONDRVDevice, IOPlatformDevice)

void IONDRVDevice::joinPMtree( IOService * driver )
{
    IOService * realDevice;
    realDevice = OSDynamicCast( IOService, getParentEntry(gIODTPlane) );
    if (realDevice)
        realDevice->addPowerChild(driver);
    else
        super::joinPMtree( driver );
}

bool IONDRVDevice::compareName( OSString * name,
                                        OSString ** matched ) const
{
    return (IODTCompareNubName(this, name, matched)
          || IORegistryEntry::compareName(name, matched));
}

IOService * IONDRVDevice::matchLocation( IOService * client )
{
    return (this);
}

IOReturn IONDRVDevice::getResources( void )
{
    if (getDeviceMemory())
        return (kIOReturnSuccess);

    IODTResolveAddressing(this, "reg", 0);

    return (kIOReturnSuccess);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct _VSLService
{
    class IONDRVFramebuffer *   framebuffer;
    IOSelect                    type;
    IOFBInterruptProc           handler;
    OSObject *                  target;
    void *                      ref;
    _VSLService *               next;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// IONDRVFramebuffer has three power states: sleep, doze, wake.

enum {
    kNDRVFramebufferSleepState          = 0,
    kNDRVFramebufferDozeState           = 1,
    kNDRVFramebufferWakeState           = 2,
    kIONDRVFramebufferPowerStateCount   = 3,
    kIONDRVFramebufferPowerStateMax     = kIONDRVFramebufferPowerStateCount - 1
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOFramebuffer

OSDefineMetaClassAndStructors(IONDRVFramebuffer, IOFramebuffer)

static int gIONDRVFramebufferGeneration[2] = { 0, 0 };

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOService * IONDRVFramebuffer::probe( IOService * provider, SInt32 * score )
{
    IONDRVFB_START(probe,0,0,0);
    IOService *  inst;

    if (0 != provider->getProperty(kIONDRVIgnoreKey))
    {
        IONDRVFB_END(probe,-1,__LINE__,0);
        return (NULL);
    }

    inst = super::probe(provider, score);
    if (!inst)
    {
        IONDRVFB_END(probe,-1,__LINE__,0);
        return (NULL);
    }

    if (getProperty(gIONameMatchedKey))
    {
        // matched
        provider->setProperty(kIONDRVForXKey, gIOFBOne32Data);
    }

    IONDRVFB_END(probe,0,0,0);
    return (inst);
}

IOReturn IONDRVFramebuffer::setProperties( OSObject * properties )
{
    IONDRVFB_START(setProperties,0,0,0);
    OSDictionary *      dict;
    OSData *            data;
    IOReturn            kr = kIOReturnUnsupported;

    if (!(dict = OSDynamicCast(OSDictionary, properties)))
    {
        IONDRVFB_END(setProperties,kIOReturnBadArgument,0,0);
        return (kIOReturnBadArgument);
    }

    if ((data = OSDynamicCast(OSData,
                              dict->getObject("driver,AAPL,MacOS,PowerPC"))))
    {
        gIONDRVFramebufferGeneration[1] = gIONDRVFramebufferGeneration[0];
        if (ndrvState)
        {
            IONDRVFB_END(setProperties,kIOReturnStillOpen,0,0);
            return (kIOReturnStillOpen);
        }

        OSSafeReleaseNULL(fNdrv);
        kr = kIOReturnSuccess;
    }
    else
        kr = super::setProperties(properties);

    IONDRVFB_END(setProperties,kr,0,0);
    return (kr);
}

void IONDRVFramebuffer::stop( IOService * provider )
{
    IONDRVFB_START(stop,0,0,0);
    UInt32 state = ndrvState;

    super::stop( provider );

    if (state)
    {
        IOMemoryDescriptor * vram;
        if ((vram = getVRAMRange()))
        {
            vram->redirect( kernel_task, false );
            vram->release();
        }
        online = false;
        fDevice = NULL;
        fNub   = NULL;
        _IONDRVLibrariesFinalize(provider);
    }
    provider->removeProperty(kIONDRVForXKey);

    IONDRVFB_END(stop,0,0,0);
}

bool IONDRVFramebuffer::start( IOService * provider )
{
    IONDRVFB_START(start,0,0,0);
    bool                ok = false;
    IOService *         parent = 0;
    OSData *            data;

    if (0 == getProperty(gIONameMatchedKey))
    {
        // !matched
        IORegistryIterator * iter;
        IORegistryEntry *    next;
        IOService *          nub;
        OSArray *            toDo = 0;
        bool                 firstLevel;

        if (0 == provider->getProperty("AAPL,ndrv-dev"))
        {
            provider->setProperty("AAPL,ndrv-dev", kOSBooleanTrue);

            if (provider->getProperty("@0,name"))
            {
                OSDictionary *         dict;
                OSCollectionIterator * keys;
                const OSSymbol *       key;
                char                   buffer[80];
                const char *           keyChrs;
                size_t                 len;
                char                   c;

                dict = provider->dictionaryWithProperties();
                keys = OSCollectionIterator::withCollection(dict);
                if (dict)
                    dict->release();
                if (keys)
                {
                    while ((key = OSDynamicCast(OSSymbol, keys->getNextObject())))
                    {
                        keyChrs = key->getCStringNoCopy();
                        if ('@' != keyChrs[0])
                            continue;

                        len = 0;
                        do
                        {
                            c = keyChrs[len];
                            if (!c || (c == ','))
                                break;
                            buffer[len] = c;
                            len++;
                        }
                        while (len < (sizeof(buffer) - 1));
                        if (!c)
                            continue;

                        buffer[len] = 0;
                        keyChrs += len + 1;

                        next = provider->childFromPath(buffer, gIODTPlane);
                        if (!next)
                        {
                            next = new IOService;
                            if (next && !next->init())
                            {
                                next->release();
                                next = 0;
                            }
                            if (!next)
                                continue;
                            next->setLocation(&buffer[1]);
                            if (!next->attachToParent(provider, gIODTPlane))
                                continue;
                        }

                        OSObject * obj = dict->getObject(key);
                        next->setProperty(keyChrs, dict->getObject(key));
                        if (!strcmp(keyChrs, "name"))
                        {
                            OSData * tdata = OSDynamicCast(OSData, obj);
                            if (tdata)
                                next->setName((const char *) tdata->getBytesNoCopy());
                        }
                        next->release();
                        provider->removeProperty(key);
                    }
                    keys->release();
                }
            }

            iter = IORegistryIterator::iterateOver( provider, gIODTPlane, 0 );
            toDo = OSArray::withCapacity(2);

            if (iter && toDo)
            {
                bool haveDoneLibInit = false;
                UInt32 index = 0;
                do
                {
                    while ((next = (IORegistryEntry *) iter->getNextObject()))
                    {
                        firstLevel = (provider == next->getParentEntry(gIODTPlane));
                        if (firstLevel)
                        {
                            data = OSDynamicCast( OSData, next->getProperty("device_type"));
                            if (!data || (0 != strcmp("display", (char *) data->getBytesNoCopy())))
                                continue;

                            if (!haveDoneLibInit)
                            {
                                haveDoneLibInit = (kIOReturnSuccess == _IONDRVLibrariesInitialize(provider));
                                if (!haveDoneLibInit)
                                    continue;
                            }
                            next->setProperty( kIOFBDependentIDKey, provider->getRegistryEntryID(), 64 );
                            next->setProperty( kIOFBDependentIndexKey, index, 32 );
                            index++;
                        }

                        toDo->setObject( next );
                        iter->enterEntry();
                    }
                }
                while (iter->exitEntry());
            }
            if (iter)
                iter->release();

            if (toDo)
            {
                OSObject * obj;
                OSArray  * deviceMemory;
                obj = provider->copyProperty(gIODeviceMemoryKey);
                deviceMemory = OSDynamicCast(OSArray, obj);

                for (unsigned int i = 0;
                        (next = (IORegistryEntry *) toDo->getObject(i));
                        i++)
                {
                    nub = new IONDRVDevice;
                    if (!nub)
                        continue;
                    if (!nub->init(next, gIODTPlane))
                    {
                        nub->free();
                        nub = 0;
                        continue;
                    }
                    if (deviceMemory)
                        nub->setDeviceMemory(deviceMemory);
                    nub->attach(provider);
                    nub->registerService(kIOServiceSynchronous);
                }
                if (obj)
                    obj->release();
                toDo->release();
            }
        }
        IONDRVFB_END(start,false,__LINE__,0);
        return (false);
    }

    data = OSDynamicCast(OSData, provider->getProperty("device_type"));
    if (data && (0 != strcmp("display", (char *) data->getBytesNoCopy())))
    {
        IONDRVFB_END(start,false,__LINE__,0);
        return (false);
    }

    do
    {
        cachedVDResolution.csDisplayModeID = kDisplayModeIDInvalid;

        if (!__private)
        {
            __private = IONew( IONDRVFramebufferPrivate, 1 );
            if (!__private)
                continue;
            bzero( __private, sizeof(IONDRVFramebufferPrivate) );
        }

        __private->depthMapModeID = kDisplayModeIDInvalid;

        fNub = provider;

        gIONDRVFramebufferGeneration[0]++;
        data = OSData::withBytesNoCopy(&gIONDRVFramebufferGeneration, sizeof32(gIONDRVFramebufferGeneration));
        if (data)
        {
            setProperty(kIONDRVFramebufferGenerationKey, data);
            getRegistryRoot()->setProperty(kIONDRVFramebufferGenerationKey, data);
            data->release();
        }

        if ((data = OSDynamicCast(OSData, getProperty("driver,AAPL,MacOS,PowerPC"))))
            fNub->setProperty("driver,AAPL,MacOS,PowerPC", data);

        fNdrv = NULL;
        if (fNdrv)
            setName( fNdrv->driverName());
        consoleDevice = (0 != provider->getProperty("AAPL,boot-display"));

        powerState = kIONDRVFramebufferPowerStateMax;

        if (OSDynamicCast(IONDRVDevice, fNub))
            parent = OSDynamicCast(IOService, fNub->getParentEntry(gIODTPlane));
        fDevice = (parent) ? parent : fNub;

        RegEntryID regEntry;
        MAKE_REG_ENTRY( &regEntry, fNub);
        fNub->setProperty( kAAPLRegEntryIDKey, &regEntry, sizeof32(regEntry) );

        OSObject * obj;
        if ((obj = fNub->getProperty(kIOFBDependentIDKey)))
            setProperty( kIOFBDependentIDKey, obj );
        if ((obj = fNub->getProperty(kIOFBDependentIndexKey)))
            setProperty( kIOFBDependentIndexKey, obj );

        __private->removable = (0 != fDevice->metaCast("IOCardBusDevice"));

        // default flags can be overriden
        accessFlags = 0;
        if (0 == strncmp("3Dfx", provider->getName(), strlen("3Dfx")))
            accessFlags |= kFramebufferDisableAltivecAccess;

        if ((data = OSDynamicCast(OSData, provider->getProperty(kIOFBHostAccessFlagsKey))))
            accessFlags = *((UInt32 *) data->getBytesNoCopy());

        platformSleep = false;
        if (false == super::start(fNub))
            continue;

        IOOptionBits flags = getPMRootDomain()->getSleepSupported();
        getPMRootDomain()->setSleepSupported(flags & ~kFrameBufferDeepSleepSupported);

        ok = true;                      // Success
    }
    while (false);

    IONDRVFB_END(start,ok,0,0);
    return (ok);
}

bool IONDRVFramebuffer::isConsoleDevice( void )
{
    IONDRVFB_START(isConsoleDevice,0,0,0);
    IONDRVFB_END(isConsoleDevice,consoleDevice,0,0);
    return (consoleDevice);
}

IOReturn IONDRVFramebuffer::enableController( void )
{
    IONDRVFB_START(enableController,0,0,0);
    IOReturn            err;
    const char *        logname;

    logname = getProvider()->getName();
    do
    {
        gIONDRVFramebufferGeneration[1] = gIONDRVFramebufferGeneration[0];

        getProvider()->setProperty("AAPL,ndrv-dev", kOSBooleanTrue);

        // wait for accelerator module, display parameter drivers
        // fDevice->waitQuiet();
        // find out about onboard audio/video jack state
        // OSObject * notify =
        addMatchingNotification( gIOPublishNotification,
                                 resourceMatching(kAppleAudioVideoJackStateKey),
                                 _videoJackStateChangeHandler, this, 0 );

        err = checkDriver();
        if (err)
        {
            IOLog("%s: Not usable\n", logname );
            if (err == -999)
                IOLog("%s: driver incompatible.\n", logname );
            continue;
        }
        uintptr_t isOnline = true;
        if ((kIOReturnSuccess != getAttributeForConnection(0, kConnectionCheckEnable, &isOnline))
                || isOnline || true)
        {
            getCurrentConfiguration();

            if (!fDevice->getProperty("IONVRAMProperty"))
                setStartupDisplayMode( currentDisplayMode, currentDepth );

            if (currentDisplayMode == kDisplayModeIDBootProgrammable)
            {
                VDScalerRec scaler;
    
                bzero( &scaler, sizeof( VDScalerRec) );
                scaler.csScalerSize = sizeof32(VDScalerRec);
                scaler.csDisplayModeID = kDisplayModeIDBootProgrammable;
                if (noErr == _doStatus(this, cscGetScaler, &scaler))
                {
                    DEBG(thisName, " boot scalerFlags 0x%x\n", (uint32_t) scaler.csScalerFlags);
                    if (kIOScaleRotateFlags & scaler.csScalerFlags)
                        setProperty(kIOFBTransformKey, kIOScaleRotateFlags & scaler.csScalerFlags, 64);
                }
            }
        }
        online = static_cast<unsigned int>(isOnline);
//        vramMemory = findVRAM();

        OSData * data;
        if ((data = OSDynamicCast(OSData, fDevice->getProperty(kIOAGPCommandValueKey))))
            accessFlags |= (kIOAGPFastWrite & (*((UInt32 *) data->getBytesNoCopy())))
                           ? kFramebufferAGPFastWriteAccess : 0;

        // initialize power management of the device
        initForPM();
        fDevice->setProperty(kIOPMIsPowerManagedKey, true);

        if ((data = OSDynamicCast(OSData, getPMRootDomain()->getProperty(kIOHibernateStateKey))))
            fDevice->setProperty(kIOHibernateStateKey, gIOFBZero32Data);
    }
    while (false);

    IONDRVFB_END(enableController,err,0,0);
    return (err);
}

bool IONDRVFramebuffer::_videoJackStateChangeHandler( void * target, void * ref,
        IOService * resourceService, IONotifier * notifier )
{
    IONDRVFB_START(_videoJackStateChangeHandler,0,0,0);
    IONDRVFramebuffer * fb = (IONDRVFramebuffer *) target;
    uint32_t            jackData;

    OSObject * jackValue = resourceService->getProperty(kAppleAudioVideoJackStateKey);
    if (!jackValue)
    {
        IONDRVFB_END(_videoJackStateChangeHandler,true,__LINE__,0);
        return (true);
    }

    jackData = (jackValue == kOSBooleanTrue);
    IOLog(kAppleAudioVideoJackStateKey " %d\n", jackData);

    fb->fNub->setProperty( kAppleAudioVideoJackStateKey, &jackData, sizeof32(jackData) );
    resourceService->removeProperty(kAppleAudioVideoJackStateKey);

    if (!fb->__private->probeInterrupt)
    {
        fb->__private->probeInterrupt = IOTimerEventSource::timerEventSource(
                                            fb, &IONDRVFramebuffer::_avProbeAction);
        if (fb->__private->probeInterrupt)
            fb->getControllerWorkLoop()->addEventSource(fb->__private->probeInterrupt);
    }
    if (fb->__private->probeInterrupt && (fb->avJackState != jackData))
    {
        fb->avJackState = jackData;
        fb->__private->probeInterrupt->setTimeoutMS(kIONDRVAVJackProbeDelayMS);
    }

    IONDRVFB_END(_videoJackStateChangeHandler,true,0,0);
    return (true);
}

void IONDRVFramebuffer::_avProbeAction( OSObject * p0, IOTimerEventSource * evtSrc )
{
    IONDRVFB_START(_avProbeAction,0,0,0);
    _probeAction( (IONDRVFramebuffer *) p0, kIOFBAVProbe );
    IONDRVFB_END(_avProbeAction,0,0,0);
}

IOReturn IONDRVFramebuffer::_probeAction( IONDRVFramebuffer * fb, IOOptionBits options )
{
    IONDRVFB_START(_probeAction,options,0,0);
    IOReturn err = kIOReturnSuccess;

    if (fb->captured)
    {
        err = kIOReturnBusy;
    }
    else if (options & (kIOFBUserRequestProbe | kIOFBAVProbe))
    {
        if ((options & kIOFBUserRequestProbe) && !fb->supportsProbe)
        {
            err = kIOReturnUnsupported;
        }
        else
        {
#if 1
            err = fb->_doControl( fb, cscProbeConnection, 0 );
#else
            do
            {
                OSNumber * num = OSDynamicCast(OSNumber, fb->getProperty(kIOFBDependentIndexKey));
                if (num && (0 != num->unsigned32BitValue()))
                    continue;

				IONDRVFramebuffer * next = fb;
				do
				{
					next->_doControl( next, cscProbeConnection, 0 );
					next = OSDynamicCast(IONDRVFramebuffer, next->nextDependent);
				}
				while (next && (next != fb));
            }
            while (false);
#endif
        }
    }
    else if (options & kIOFBForceReadEDID)
    {
        if (!fb->forceReadEDID)
        {
            _VSLService *       service;
            IOFBInterruptProc   proc;

            fb->forceReadEDID = 1;

            for (service = fb->vslServices;
                    service && (kIOFBConnectInterruptType != service->type);
                    service = service->next)
            {}
            if (service && (proc = service->handler))
                (*proc) (service->target, service->ref);
        }
        err = kIOReturnSuccess;
    }
    else
        err = kIOReturnBadArgument;

    IONDRVFB_END(_probeAction,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::requestProbe( IOOptionBits options )
{
    IONDRVFB_START(requestProbe,options,0,0);
    IOReturn     err;

    err = super::requestProbe(options);

    IONDRVFB_END(requestProbe,err,0,0);
    return (err);
}

IODeviceMemory * IONDRVFramebuffer::getVRAMRange( void )
{
    IONDRVFB_START(getVRAMRange,0,0,0);
    if (ndrvState && !vramMemory)
        vramMemory = findVRAM();

    if (vramMemory)
        vramMemory->retain();

    IONDRVFB_END(getVRAMRange,0,0,0);
    return (vramMemory);
}

const IOTVector * IONDRVFramebuffer::_undefinedSymbolHandler( void * obj,
        const char * libraryName, const char * symbolName )
{
    IONDRVFB_START(_undefinedSymbolHandler,0,0,0);
    const IOTVector * vc = ((IONDRVFramebuffer *)obj)->undefinedSymbolHandler(libraryName, symbolName);
    IONDRVFB_END(_undefinedSymbolHandler,0,0,0);
    return (vc);
}

const IOTVector * IONDRVFramebuffer::undefinedSymbolHandler( const char * libraryName,
        const char * symbolName )
{
    IONDRVFB_START(undefinedSymbolHandler,0,0,0);
    IONDRVFB_END(undefinedSymbolHandler,0,0,0);
    return (NULL);
}

void IONDRVFramebuffer::free( void )
{
    IONDRVFB_START(free,0,0,0);
    if (__private)
    {
        IODelete( __private, IONDRVFramebufferPrivate, 1 );
        __private = NULL;
    }
    super::free();
    IONDRVFB_END(free,0,0,0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IONDRVFramebuffer::registerForInterruptType( IOSelect interruptType,
        IOFBInterruptProc proc, OSObject * target, void * ref,
        void ** interruptRef )

{
    IONDRVFB_START(registerForInterruptType,interruptType,0,0);
    _VSLService * service;
    IOReturn      err;

    if ((interruptType == kIOFBVBLInterruptType)
            && (getProvider()->getProperty("Ignore VBL")))
    {
        IONDRVFB_END(registerForInterruptType,kIOReturnUnsupported,0,0);
        return (kIOReturnUnsupported);
    }

    for (service = vslServices;
            service && (service->type != interruptType);
            service = service->next)
        {}

    if (service)
    {
        if (service->handler)
            err = kIOReturnBusy;
        else
        {
            service->target     = target;
            service->ref        = ref;
            service->handler    = proc;
            *interruptRef       = service;
            err                 = kIOReturnSuccess;
        }
    }
    else
    {
        err = super::registerForInterruptType(interruptType, proc, target, ref, interruptRef);
    }

    IONDRVFB_END(registerForInterruptType,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::unregisterInterrupt( void * interruptRef )
{
    IONDRVFB_START(unregisterInterrupt,0,0,0);
    _VSLService * service;
    IOReturn      err;

    for (service = vslServices;
            service && (service != interruptRef);
            service = service->next)
        {}

    if (service)
    {
        service->handler = 0;
        err = kIOReturnSuccess;
    }
    else
    {
        err = super::unregisterInterrupt(interruptRef);
    }

    IONDRVFB_END(unregisterInterrupt,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setInterruptState( void * interruptRef,
        UInt32 state )
{
    IONDRVFB_START(setInterruptState,0,0,0);
    _VSLService * service;
    IOReturn      err;

    for (service = vslServices;
            service && (service != interruptRef);
            service = service->next)
        {}

    if (service)
    {
        err = kIOReturnUnsupported;
    }
    else
    {
        err = super::setInterruptState(interruptRef, state);
    }

    IONDRVFB_END(setInterruptState,err,0,0);
    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

//// VSL calls

OSStatus IONDRVFramebuffer::VSLNewInterruptService(
    void * entryID,
    IOSelect serviceType,
    _VSLService ** vslService )
{
    IONDRVFB_START(VSLNewInterruptService,serviceType,0,0);
    IORegistryEntry *   regEntry;
    IOService *         device;
    IOService *         client = NULL;
    IONDRVFramebuffer * fb = NULL;
    _VSLService *       service;
    IOReturn            err = kIOReturnSuccess;

    REG_ENTRY_TO_OBJ( (const RegEntryID *) entryID, regEntry)

    if ((device = OSDynamicCast(IOService, regEntry)))
        client = device->copyClientWithCategory(gIOFramebufferKey);

    if ((fb = OSDynamicCast(IONDRVFramebuffer, client)))
    {
        service = IONew( _VSLService, 1 );

        if (service)
        {
            service->framebuffer        = fb;
            service->type               = serviceType;
            service->handler            = 0;
            service->next = fb->vslServices;
            fb->vslServices = service;

            *vslService = service;
        }
        else
            err = kIOReturnNoMemory;
    }
    else
        err = kIOReturnBadArgument;

    if (client)
        client->release();
    
    IONDRVFB_END(VSLNewInterruptService,err,0,0);
    return (err);
}

OSStatus IONDRVFramebuffer::VSLDisposeInterruptService(_VSLService * vslService)
{
    IONDRVFB_START(VSLDisposeInterruptService,0,0,0);
    IONDRVFramebuffer * fb;
    _VSLService *       next;
    _VSLService *       prev;

    if (vslService)
    {
        fb = vslService->framebuffer;

        prev = fb->vslServices;
        if (prev == vslService)
            fb->vslServices = vslService->next;
        else
        {
            while (((next = prev->next) != vslService) && next)
                prev = next;
            if (next)
                prev->next = vslService->next;
        }

        IODelete( vslService, _VSLService, 1 );
    }

    IONDRVFB_END(VSLDisposeInterruptService,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

OSStatus IONDRVFramebuffer::VSLDoInterruptService( _VSLService * vslService )
{
    IONDRVFB_START(VSLDoInterruptService,0,0,0);
    IOReturn          ret = kIOReturnSuccess;
    IOFBInterruptProc proc;

    if (vslService)
    {
        if (kIOFBConnectInterruptType == vslService->type)
            vslService->framebuffer->__private->ackConnectChange = true;
        else if (kIOFBWakeInterruptType == vslService->type)
            ret = vslService->framebuffer->handleEvent(kIOFBNotifyVRAMReady);

        if ((proc = vslService->handler))
            (*proc) (vslService->target, vslService->ref);
    }

    IONDRVFB_END(VSLDoInterruptService,ret,0,0);
    return (ret);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct _VSLCursorRef
{
    IOFramebuffer *     framebuffer;
    void *              cursorImage;
};

Boolean IONDRVFramebuffer::VSLPrepareCursorForHardwareCursor(
    void * cursorRef,
    IOHardwareCursorDescriptor * hwDesc,
    IOHardwareCursorInfo * hwCursorInfo )
{
    IONDRVFB_START(VSLPrepareCursorForHardwareCursor,0,0,0);
    _VSLCursorRef *     cursor = (_VSLCursorRef *) cursorRef;
    bool                ok;

    if (hwCursorInfo->colorMap)
        hwCursorInfo->colorMap += 1;
    ok = cursor->framebuffer->convertCursorImage(
             cursor->cursorImage, hwDesc, hwCursorInfo );
    if (hwCursorInfo->colorMap)
        hwCursorInfo->colorMap -= 1;

    IONDRVFB_END(VSLPrepareCursorForHardwareCursor,ok,0,0);
    return (ok);
}

IOReturn IONDRVFramebuffer::setCursorImage( void * cursorImage )
{
    IONDRVFB_START(setCursorImage,0,0,0);
    _VSLCursorRef               cursorRef;
    VDSetHardwareCursorRec      setCursor;
    IOReturn                    err;

    if (0 == powerState)
    {
        IONDRVFB_END(setCursorImage,kIOReturnSuccess,__LINE__,0);
        return (kIOReturnSuccess);
    }
    if (!online)
    {
        IONDRVFB_END(setCursorImage,kIOReturnSuccess,__LINE__,0);
        return (kIOReturnSuccess);
    }

    cursorRef.framebuffer = this;
    cursorRef.cursorImage = cursorImage;

    setCursor.csCursorRef = (void *) &cursorRef;
    setCursor.csReserved1 = 0;
    setCursor.csReserved2 = 0;

    err = _doControl( this, cscSetHardwareCursor, &setCursor );

    mirrorSWCursor = (mirrored && !mirrorPrimary && (kIOReturnSuccess != err));
    if (mirrorSWCursor)
        err = kIOReturnSuccess;

    IONDRVFB_END(setCursorImage,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setCursorState( SInt32 x, SInt32 y, bool visible )
{
    IONDRVFB_START(setCursorState,x,y,visible);
    VDDrawHardwareCursorRec     drawCursor;
    IOReturn                    err;

    if (mirrorSWCursor || !online)
    {
        IONDRVFB_END(setCursorState,kIOReturnSuccess,__LINE__,0);
        return (kIOReturnSuccess);
    }

    if (0 == powerState)
    {
        IONDRVFB_END(setCursorState,kIOReturnSuccess,__LINE__,0);
        return (kIOReturnSuccess);
    }

    if (0 == OSIncrementAtomic(&ndrvEnter))
    {
        drawCursor.csCursorX    = x;
        drawCursor.csCursorY    = y;
        drawCursor.csCursorVisible      = visible;
        drawCursor.csReserved1  = 0;
        drawCursor.csReserved2  = 0;

        err = _doControl( this, cscDrawHardwareCursor, &drawCursor );
    }
    else
    {
        err = kIOReturnBusy;
    }

    OSDecrementAtomic( &ndrvEnter );

    IONDRVFB_END(setCursorState,err,0,0);
    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IONDRVFramebuffer::doDriverIO( UInt32 commandID, void * contents,
                                        UInt32 commandCode, UInt32 commandKind )
{
    IONDRVFB_START(doDriverIO,commandID,commandCode,commandKind);
    IOReturn err;

    if (kIONDRVInitializeCommand == commandCode)
    {
        if (!fNdrv)
        {
			uint32_t debugFlags;
			if (PE_parse_boot_argn("iog", &debugFlags, sizeof(debugFlags))
					&& (kIOGDbgDumbPanic & debugFlags))
			{
				panic("dumb FB start");
			}
            fNdrv = IOBootNDRV::fromRegistryEntry( fNub );
            if (fNdrv)
                setName( fNdrv->driverName());
        }
    }

    if (fNdrv)
    {
        OSIncrementAtomic( &ndrvEnter );
        err = fNdrv->doDriverIO( commandID, contents, commandCode, commandKind );
        OSDecrementAtomic( &ndrvEnter );
    }
    else
        err = kIOReturnUnsupported;

    IONDRVFB_END(doDriverIO,err,0,0);
    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IONDRVFramebuffer::_doControl( IONDRVFramebuffer * fb, UInt32 code, void * params )
{
    IONDRVFB_START(_doControl,code,0,0);
    IOReturn                    err;
    IONDRVControlParameters     pb;

#if IONDRVCHECK
    IOWorkLoop * wl;
    if (!ml_at_interrupt_context() && (wl = fb->getControllerWorkLoop()) && !wl->inGate())
        OSReportWithBacktrace("%s::_doControl(%d) not gated\n", fb->thisName, code);
#endif

    if (fb->ndrvState == 0)
    {
        IONDRVFB_END(_doControl,kIOReturnNotOpen,0,0);
        return (kIOReturnNotOpen);
    }

    pb.code = code;
    pb.params = params;

    err = fb->doDriverIO( /*ID*/ 1, &pb,
                                   kIONDRVControlCommand, kIONDRVImmediateIOCommandKind );

#if FORCE_CONNECT_CHANGE
    if (cscProbeConnection == code)
    {
        _VSLService * vslService  = fb->vslServices;
        while (vslService && (kIOFBConnectInterruptType != vslService->type))
            vslService = vslService->next;
        if (vslService)
            VSLDoInterruptService(vslService);
    }
#endif

    IONDRVFB_END(_doControl,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::_doStatus( IONDRVFramebuffer * fb, UInt32 code, void * params )
{
    IONDRVFB_START(_doStatus,code,0,0);
    IOReturn                    err;
    IONDRVControlParameters     pb;

    if (fb->ndrvState == 0)
    {
        IONDRVFB_END(_doStatus,kIOReturnNotOpen,0,0);
        return (kIOReturnNotOpen);
    }

#if IONDRVCHECK
    IOWorkLoop * wl;
    if (!ml_at_interrupt_context() && (wl = fb->getControllerWorkLoop()) && !wl->inGate())
        OSReportWithBacktrace("%s::_doStatus(%d) not gated\n", fb->thisName, code);
#endif

    pb.code = code;
    pb.params = params;

    err = fb->doDriverIO( /*ID*/ 1, &pb,
                                   kIONDRVStatusCommand, kIONDRVImmediateIOCommandKind );

    IONDRVFB_END(_doStatus,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::extControl( OSObject * owner, void * code, void * params )
{
    IONDRVFB_START(extControl,0,0,0);
    IONDRVFramebuffer * fb = (IONDRVFramebuffer *) owner;
    IOReturn            err;

    if (fb->powerState)
        err = _doControl( fb, (UInt32)(uintptr_t) code, params );
    else
        err = kIOReturnNotReady;

    IONDRVFB_END(extControl,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::extStatus( OSObject * owner, void * code, void * params )
{
    IONDRVFB_START(extStatus,0,0,0);
    IONDRVFramebuffer * fb = (IONDRVFramebuffer *) owner;
    IOReturn            err;

    if (fb->powerState)
        err = _doStatus( fb, (UInt32)(uintptr_t) code, params );
    else
        err = kIOReturnNotReady;

    IONDRVFB_END(extStatus,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::doControl( UInt32 code, void * params )
{
    IONDRVFB_START(doControl,code,0,0);
    IOReturn err = getControllerWorkLoop()->runAction((IOWorkLoop::Action) &extControl, this, (void *)(uintptr_t) code, params);
    IONDRVFB_END(doControl,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::doStatus( UInt32 code, void * params )
{
    IONDRVFB_START(doStatus,code,0,0);
    IOReturn err = getControllerWorkLoop()->runAction((IOWorkLoop::Action) &extStatus, this, (void *)(uintptr_t) code, params);
    IONDRVFB_END(doStatus,err,0,0);
    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IONDRVFramebuffer::checkDriver( void )
{
    IONDRVFB_START(checkDriver,0,0,0);
    OSStatus                    err = noErr;
    IONDRVControlParameters     pb;
    VDClutBehavior              clutSetting;
    VDGammaRecord               gammaRec;
    VDSwitchInfoRec             switchInfo;
    VDPageInfo                  pageInfo;

    if (ndrvState == 0)
    {
        do
        {
            err = doDriverIO( 0, fNub,
                              kIONDRVInitializeCommand, kIONDRVImmediateIOCommandKind );
            if (err)
                continue;
            err = doDriverIO( 0, &pb,
                              kIONDRVOpenCommand, kIONDRVImmediateIOCommandKind );
        }
        while (false);
        if (err)
        {
            IONDRVFB_END(checkDriver,err,__LINE__,0);
            return (err);
        }

        // allow calls to ndrv
        ndrvState = 2;

        createI2C();

#if IONDRVI2CLOG
        do
        {
            VDCommunicationInfoRec      commInfo;

            bzero( &commInfo, sizeof( commInfo));
            commInfo.csBusID = kVideoDefaultBus;

            err = _doStatus( this, cscGetCommunicationInfo, &commInfo );
            IOLog("%s: cscGetCommunicationInfo: ", getName());
            if (kIOReturnSuccess != err)
            {
                IOLog("fails with %ld\n", err);
                continue;
            }
            IOLog("csBusType %lx, csMinBus %lx, csMaxBus %lx\n"
                  "csSupportedTypes %lx, csSupportedCommFlags %lx\n",
                  commInfo.csBusType,
                  commInfo.csMinBus, commInfo.csMaxBus,
                  commInfo.csSupportedTypes, commInfo.csSupportedCommFlags);

            if (commInfo.csSupportedTypes & (1<<kVideoDDCciReplyType))
                IOLog("supports kVideoDDCciReplyType, ");
            if (commInfo.csSupportedTypes & (1<<kVideoSimpleI2CType))
            {
                IOLog("supports kVideoSimpleI2CType");
                VDCommunicationRec      comm;
                UInt8                   edidData[132];
                UInt8                   edidRequest[2];

                memset( edidData, 0xAA, sizeof( edidData));

                edidRequest[0]          = 0;
                edidRequest[1]          = 0;

                comm.csBusID            = kVideoDefaultBus;
                comm.csCommFlags        = 0;
                comm.csMinReplyDelay    = 0;
                comm.csReserved2        = 0;

                comm.csSendAddress      = 0xA0;
                comm.csSendType         = kVideoSimpleI2CType;
                comm.csSendBuffer       = &edidRequest[0];
                comm.csSendSize         = 0x01;

                comm.csReplyAddress     = 0xA1;
                comm.csReplyType        = kVideoSimpleI2CType;
                comm.csReplyBuffer      = &edidData[0];
                comm.csReplySize        = 128;

                comm.csReserved3        = 0;
                comm.csReserved4        = 0;
                comm.csReserved5        = 0;
                comm.csReserved6        = 0;

                do
                {
                    err = _doControl( this, cscDoCommunication, &comm );
                    IOLog("\nI2C read block[%x](%ld)\n", edidRequest[0], err);
                    if (kIOReturnSuccess != err)
                        break;
                    IOLog("    0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F");
                    for (int i = 0; i < 128; i++)
                    {
                        if (0 == (i & 15))
                            IOLog("\n%02x: ", i);
                        IOLog("%02x ", edidData[i]);
                    }
                    IOLog("\n");
                    if (edidRequest[0] || (0 == edidData[126]))
                        break;
                    edidRequest[0] = 0x80;
                }
                while (true);
            }
        }
        while (false);
#endif /* IONDRVI2CLOG */

        // duplicate QD InitGDevice
        _doStatus( this, cscGetCurMode, &switchInfo );
        pageInfo.csMode = switchInfo.csMode;
        pageInfo.csData = 0;
        pageInfo.csPage = 0;
        _doControl( this, cscGrayPage, &pageInfo);

        clutSetting = kSetClutAtSetEntries;
        lastClutSetting = clutSetting;
        __private->deferCLUTSet = (kIOReturnSuccess == 
                _doControl( this, cscSetClutBehavior, &clutSetting));

        setInfoProperties();

        VDGetGammaListRec scan;
        GammaTbl *        table;

        err = _doStatus( this, cscGetGamma, &gammaRec );
        table = (GammaTbl *) gammaRec.csGTable;
        if (table && (err == kIOReturnSuccess))
        {
            __private->desiredGammaWidth = table->gDataWidth;
            __private->desiredGammaCount = table->gDataCnt;
        }
        else
        {
            __private->desiredGammaWidth = 8;
            __private->desiredGammaCount = (1 << 8);
        }

        for (scan.csPreviousGammaTableID = kGammaTableIDFindFirst;
                ;
                scan.csPreviousGammaTableID = scan.csGammaTableID)
        {
            VDRetrieveGammaRec  get;
            char                name[ 64 ];

            scan.csGammaTableName = name;
            err = _doStatus( this, cscGetGammaInfoList, &scan);
            if (err || (scan.csGammaTableID == (GammaTableID) kGammaTableIDNoMoreTables))
                break;

            table = (GammaTbl *) IOMallocData(scan.csGammaTableSize);
            if (0 == table)
                continue;
            get.csGammaTableID  = scan.csGammaTableID;
            get.csGammaTablePtr = table;

            err = _doStatus( this, cscRetrieveGammaTable, &get );
            if (noErr == err)
            {
                DEBG(thisName, " gamma %s: %d * %d\n", scan.csGammaTableName,
                     table->gDataWidth, table->gDataCnt);
                if (((UInt32) table->gDataWidth) > __private->desiredGammaWidth)
                {
                    __private->desiredGammaWidth = table->gDataWidth;
                    __private->desiredGammaCount = table->gDataCnt;
                }
            }

            IOFreeData( table, scan.csGammaTableSize);
        }

        setProperty(kIOFBGammaWidthKey, __private->desiredGammaWidth, 32);
        setProperty(kIOFBGammaCountKey, __private->desiredGammaCount, 32);
        setProperty(kIOFBGammaHeaderSizeKey, sizeof32(GammaTbl) - sizeof32(table->gFormulaData), 32);
    }
    IONDRVFB_END(checkDriver,noErr,0,0);
    return (noErr);
}

void IONDRVFramebuffer::setInfoProperties( void )
{
    IONDRVFB_START(setInfoProperties,0,0,0);
    IOReturn                    err;
    VDDisplayTimingRangeRec     rangeRec;
    VDScalerInfoRec             scalerRec;
    UInt32                      probeOptions = 0;

//    UInt8                       probeType[32];
//    if (noErr == _doStatus(this, cscProbeConnection, &probeType))
    {
        probeOptions |= kIOFBUserRequestProbe;
        supportsProbe = true;
    }

    removeProperty( kIOFBTimingRangeKey );
    bzero( &rangeRec, sizeof( rangeRec));
    rangeRec.csRangeSize = sizeof32( rangeRec);
    err = _doStatus( this, cscGetTimingRanges, &rangeRec );
    if (kIOReturnSuccess == err)
        setProperty( kIOFBTimingRangeKey, &rangeRec, sizeof32( rangeRec));

    removeProperty( kIOFBScalerInfoKey );
    bzero( &scalerRec, sizeof( scalerRec));
    scalerRec.csScalerInfoSize = sizeof32( scalerRec);
    err = _doStatus( this, cscGetScalerInfo, &scalerRec );
    if (kIOReturnSuccess == err)
    {
        setProperty( kIOFBScalerInfoKey, &scalerRec, sizeof32( scalerRec));
        if (kScaleCanRotateMask & scalerRec.csScalerFeatures)
            probeOptions |= kIOFBSetTransform;
    }

    if (probeOptions)
        setProperty( kIOFBProbeOptionsKey, probeOptions, 32);

    fNub->setProperty(kIONDRVDisplayConnectFlagsKey,
        &__private->displayConnectFlags, sizeof32(__private->displayConnectFlags));

    IONDRVFB_END(setInfoProperties,0,0,0);
}

UInt32 IONDRVFramebuffer::iterateAllModes( IODisplayModeID * displayModeIDs )
{
    IONDRVFB_START(iterateAllModes,0,0,0);
    VDResolutionInfoRec info;
    UInt32              num = 0;

    info.csPreviousDisplayModeID = kDisplayModeIDFindFirstResolution;

    while (
        (noErr == _doStatus(this, cscGetNextResolution, &info))
        && ((SInt32) info.csDisplayModeID > 0))
    {
        if (displayModeIDs)
            displayModeIDs[ num ] = info.csDisplayModeID;

        info.csPreviousDisplayModeID = info.csDisplayModeID;
        num++;
    }

    IONDRVFB_END(iterateAllModes,num,0,0);
    return (num);
}

IOIndex IONDRVFramebuffer::mapDepthIndex( 
        IODisplayModeID modeID, IOIndex depth, bool fromDepthMode )
{
    IONDRVFB_START(mapDepthIndex,modeID,depth,fromDepthMode);
    VDVideoParametersInfoRec    pixelParams;
    VPBlock                     pixelInfo;
    IOIndex                     mapped, index, lastDepth, lastIndex;
    IOReturn                    err;

    if ((modeID == kDisplayModeIDPreflight)
        || (modeID != __private->depthMapModeID))
    {
        lastDepth = kDepthMode1;
        lastIndex = 0;
        for (mapped = kDepthMode1, index = 0; mapped <= kDepthMode6; mapped++)
        {
            pixelParams.csDisplayModeID = modeID;
            pixelParams.csDepthMode     = mapped;
            pixelParams.csVPBlockPtr    = &pixelInfo;
            err = _doStatus( this, cscGetVideoParameters, &pixelParams );
            if (kIOReturnSuccess == err)
            {
                __private->indexToDepthMode[index] = mapped;
                lastDepth = mapped;
                lastIndex = index;
                index++;
            }
            __private->depthModeToIndex[mapped - kDepthMode1] = lastIndex;
        }
    
        for (; index <= (kDepthMode6 - kDepthMode1); index++)
            __private->indexToDepthMode[index] = lastDepth;
    
        __private->depthMapModeID = modeID;
    }

    if (fromDepthMode)
    {
        depth = iog::gclamp(static_cast<IOIndex>(kDepthMode1), depth,
            static_cast<IOIndex>(kDepthMode6));
        mapped = __private->depthModeToIndex[depth - kDepthMode1];
    }
    else
    {
        depth = iog::gclamp(0, depth, kDepthMode6 - kDepthMode1);
        mapped = __private->indexToDepthMode[depth];
    }

    IONDRVFB_END(mapDepthIndex,mapped,0,0);
    return (mapped);
}

static IOFixed1616
DetailedRefreshRate(IODetailedTimingInformationV2 * detailed)
{
    UInt64 pixelCount;
    pixelCount = ((detailed->verticalActive + detailed->verticalBlanking)
                * (detailed->horizontalActive + detailed->horizontalBlanking));
    return (static_cast<IOFixed1616>(pixelCount ? ((detailed->pixelClock * 65536ULL) / pixelCount) : 0));
}

IOReturn IONDRVFramebuffer::getResInfoForDetailed( 
        IODisplayModeID modeID,
        VDDetailedTimingRec * detailed,
        IODisplayModeInformation * info )
{
    IONDRVFB_START(getResInfoForDetailed,modeID,0,0);
    IODetailedTimingInformationV2 * desc = (IODetailedTimingInformationV2 *) detailed;

    info->maxDepthIndex = mapDepthIndex(modeID, kDepthMode6, true);
//    DEBG(thisName, " %x maxDepthIndex %x\n", modeID, info->maxDepthIndex);

    if (desc->horizontalScaled && desc->verticalScaled)
    {
        info->nominalWidth      = desc->horizontalScaled;
        info->nominalHeight     = desc->verticalScaled;
    }
    else
    {
        info->nominalWidth      = detailed->csHorizontalActive;
        info->nominalHeight     = detailed->csVerticalActive;
    }
    info->refreshRate   = DetailedRefreshRate(desc);

    if (kIOInterlacedCEATiming & detailed->csSignalConfig)
        info->refreshRate *= 2;

    IONDRVFB_END(getResInfoForDetailed,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

IOReturn IONDRVFramebuffer::getResInfoForArbMode( IODisplayModeID modeID,
        IODisplayModeInformation * info )
{
    IONDRVFB_START(getResInfoForArbMode,modeID,0,0);
    IOReturn              err;
    VDDetailedTimingRec * detailed;
    VDDetailedTimingRec   _detailed;

    if (modeID == kDisplayModeIDBootProgrammable)
    {
        detailed = &_detailed;
        bzero(detailed, sizeof(VDDetailedTimingRec));
        detailed->csTimingSize    = sizeof32(VDDetailedTimingRec);
        detailed->csDisplayModeID = kDisplayModeIDBootProgrammable;
        err = _doStatus(this, cscGetDetailedTiming, detailed);
    }
    else
        err = validateDisplayMode( modeID, 0, &detailed );

    if (kIOReturnSuccess == err)
        err = getResInfoForDetailed(modeID, detailed, info);

    IONDRVFB_END(getResInfoForArbMode,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::getResInfoForMode( IODisplayModeID modeID,
        IODisplayModeInformation * info )
{
    IONDRVFB_START(getResInfoForMode,modeID,0,0);
    bzero( info, sizeof( *info));

    if ((UInt32) modeID >= (UInt32) kDisplayModeIDReservedBase)
    {
        IOReturn err = getResInfoForArbMode(modeID, info);
        IONDRVFB_END(getResInfoForMode,err,0,0);
        return (err);
    }

    // unfortunately, there is no "kDisplayModeIDFindSpecific"
    if (cachedVDResolution.csDisplayModeID != modeID)
    {
        // try the next after cached mode
        cachedVDResolution.csPreviousDisplayModeID = cachedVDResolution.csDisplayModeID;
        if ((noErr != _doStatus(this, cscGetNextResolution, &cachedVDResolution))
                || (cachedVDResolution.csDisplayModeID != modeID))
        {
            // else full blown iterate
            cachedVDResolution.csPreviousDisplayModeID = kDisplayModeIDFindFirstResolution;
            while (
                (noErr == _doStatus(this, cscGetNextResolution, &cachedVDResolution))
                && (cachedVDResolution.csDisplayModeID != modeID)
                && ((SInt32) cachedVDResolution.csDisplayModeID > 0))
            {
                cachedVDResolution.csPreviousDisplayModeID = cachedVDResolution.csDisplayModeID;
            }
        }
    }

    if (cachedVDResolution.csDisplayModeID != modeID)
    {
        cachedVDResolution.csDisplayModeID = kDisplayModeIDInvalid;
        IONDRVFB_END(getResInfoForMode,kIOReturnUnsupportedMode,0,0);
        return (kIOReturnUnsupportedMode);
    }
    else
    {
        info->nominalWidth  = cachedVDResolution.csHorizontalPixels;
        info->nominalHeight = cachedVDResolution.csVerticalLines;
        info->refreshRate   = cachedVDResolution.csRefreshRate;

        info->maxDepthIndex = mapDepthIndex(modeID, kDepthMode6, true);
//      DEBG(thisName, " %x maxDepthIndex %x\n", modeID, info->maxDepthIndex);

        IONDRVFB_END(getResInfoForMode,noErr,0,0);
        return (noErr);
    }
}

IOReturn IONDRVFramebuffer::setDetailedTiming(
    IODisplayModeID mode, IOOptionBits options,
    void * _desc, IOByteCount descripSize )
{
    IONDRVFB_START(setDetailedTiming,mode,options,0);
    IOReturn                        err;
    VDResolutionInfoRec             info;
    IODetailedTimingInformationV2 * desc = (IODetailedTimingInformationV2 *)_desc;
    VDDetailedTimingRec             look;
    VDDetailedTimingRec             newTiming;
    VDSwitchInfoRec                 switchInfo;
    IOIndex                         index;
    UInt32                          checkCurrent;
    UInt32                          checkBoot;
    bool                            notPreflight = (0 == (options & kModePreflight));
    bool                            hasScale;

    index = arbMode2Index(mode);

    bzero( &look, sizeof( VDDetailedTimingRec) );
    look.csTimingSize = sizeof32( VDDetailedTimingRec);

    // current must be ok
    if ((mode == currentDisplayMode)
            && notPreflight
        && (detailedTimingsCurrent[index] == detailedTimingsSeed))
    {
        IONDRVFB_END(setDetailedTiming,kIOReturnSuccess,0,0);
        return (kIOReturnSuccess);
    }

    err = _doStatus( this, cscGetCurMode, &switchInfo );
    if ((err == noErr) && (switchInfo.csData == kDisplayModeIDBootProgrammable))
        checkBoot = (UInt32) kDisplayModeIDBootProgrammable;
    else
        checkBoot = 0xffffffff;
    checkCurrent = (UInt32) currentDisplayMode;

    // look for a programmable
    for (
        info.csPreviousDisplayModeID = kDisplayModeIDFindFirstProgrammable;
        (noErr == (err = _doStatus(this, cscGetNextResolution, &info)));
        info.csPreviousDisplayModeID = info.csDisplayModeID)
    {
        if ((SInt32) info.csDisplayModeID < 0)
        {
            err = kIOReturnNoResources;
            break;
        }

        look.csDisplayModeID = info.csDisplayModeID;
        err = _doStatus( this, cscGetDetailedTiming, &look );
        if (err != kIOReturnSuccess)
            continue;

        // don't toss current
        if (look.csDisplayModeAlias == checkCurrent)
        {
            checkCurrent = 0xffffffff;
            continue;
        }
        // don't toss kDisplayModeIDBootProgrammable if the driver is in that mode
        if (look.csDisplayModeAlias == checkBoot)
        {
            checkBoot = 0xffffffff;
            continue;
        }

        // see if already set to the right timing
        if ((look.csDisplayModeAlias == (UInt32) mode)
                && (look.csDisplayModeState == kDMSModeReady)
                && (notPreflight)
                && (detailedTimingsCurrent[index] == detailedTimingsSeed))
            break;

        // set it free
        if (look.csDisplayModeState != kDMSModeFree)
        {
            look.csDisplayModeID    = info.csDisplayModeID;
            look.csDisplayModeAlias = 0;
            look.csDisplayModeState = kDMSModeFree;
            err = _doControl( this, cscSetDetailedTiming, &look );
            if (err != kIOReturnSuccess)
                continue;
        }

        // set it
        hasScale = (desc->horizontalScaled && desc->verticalScaled);


        newTiming = *((VDDetailedTimingRec *) desc);
        newTiming.csTimingSize       = sizeof32(VDDetailedTimingRec);
        newTiming.csDisplayModeID    = info.csDisplayModeID;
        newTiming.csDisplayModeAlias = mode;
        newTiming.csDisplayModeSeed  = look.csDisplayModeSeed;
        newTiming.csDisplayModeState = hasScale ? kDMSModeNotReady : kDMSModeReady;
        err = _doControl( this, cscSetDetailedTiming, &newTiming );

        if (hasScale && (kIOReturnSuccess == err))
        {
            VDScalerRec scaler;

            // set scale
            bzero( &scaler, sizeof( scaler));
            scaler.csScalerSize       = sizeof32( scaler);
            scaler.csScalerFlags      = desc->scalerFlags;
            scaler.csHorizontalPixels = desc->horizontalScaled;
            scaler.csVerticalPixels   = desc->verticalScaled;
            scaler.csHorizontalInset  = desc->horizontalScaledInset;
            scaler.csVerticalInset    = desc->verticalScaledInset;
            scaler.csDisplayModeID    = info.csDisplayModeID;
            scaler.csDisplayModeSeed  = newTiming.csDisplayModeSeed;
            scaler.csDisplayModeState = kDMSModeReady;

            err = _doControl( this, cscSetScaler, &scaler );

            newTiming.csDisplayModeSeed  = scaler.csDisplayModeSeed;
        }

        if (kIOReturnSuccess == err)
        {
            err = _doStatus( this, cscGetDetailedTiming, &newTiming );

            if (!notPreflight && (kIOReturnSuccess == err))
            {
                desc->signalConfig              = newTiming.csSignalConfig;
                desc->signalLevels              = newTiming.csSignalLevels;

                desc->pixelClock                = newTiming.csPixelClock;
                desc->minPixelClock             = newTiming.csMinPixelClock;
                desc->maxPixelClock             = newTiming.csMaxPixelClock;

                desc->horizontalActive          = newTiming.csHorizontalActive;
                desc->horizontalBlanking        = newTiming.csHorizontalBlanking;
                desc->horizontalSyncOffset      = newTiming.csHorizontalSyncOffset;
                desc->horizontalSyncPulseWidth  = newTiming.csHorizontalSyncPulseWidth;

                desc->verticalActive            = newTiming.csVerticalActive;
                desc->verticalBlanking          = newTiming.csVerticalBlanking;
                desc->verticalSyncOffset        = newTiming.csVerticalSyncOffset;
                desc->verticalSyncPulseWidth    = newTiming.csVerticalSyncPulseWidth;

                desc->horizontalBorderLeft      = newTiming.csHorizontalBorderLeft;
                desc->horizontalBorderRight     = newTiming.csHorizontalBorderRight;
                desc->verticalBorderTop         = newTiming.csVerticalBorderTop;
                desc->verticalBorderBottom      = newTiming.csVerticalBorderBottom;

                desc->horizontalSyncConfig      = newTiming.csHorizontalSyncConfig;
                desc->horizontalSyncLevel       = newTiming.csHorizontalSyncLevel;
                desc->verticalSyncConfig        = newTiming.csVerticalSyncConfig;
                desc->verticalSyncLevel         = newTiming.csVerticalSyncLevel;
            }
        }
        if (notPreflight && (kIOReturnSuccess == err))
            detailedTimingsCurrent[index] = detailedTimingsSeed;

        break;
    }

    IONDRVFB_END(setDetailedTiming,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::validateDisplayMode(
    IODisplayModeID _mode, IOOptionBits flags,
    VDDetailedTimingRec ** detailed )
{
    IONDRVFB_START(validateDisplayMode,_mode,flags,0);
    UInt32              mode = _mode;
    IOReturn            err = kIOReturnSuccess;
    OSData *            data;
    const void *        bytes;

    if (detailed)
        *detailed = (VDDetailedTimingRec *) 0;

    if (mode >= (UInt32) kDisplayModeIDReservedBase)
    {
        do
        {
            if (mode == (UInt32) kDisplayModeIDBootProgrammable)
                continue;

            err = kIOReturnBadArgument;
            if (!detailedTimings)
                continue;

            data = OSDynamicCast( OSData, detailedTimings->getObject(
                                      arbMode2Index(mode)));
            if (!data)
                continue;

            bytes = data->getBytesNoCopy();
            err = setDetailedTiming( mode, 0, (void *) bytes, data->getLength() );
            if (err != kIOReturnSuccess)
                continue;

            if (detailed)
                *detailed = (VDDetailedTimingRec *) bytes;
        }
        while (false);
    }

    if (err)
        DEBG(thisName, " failed (%x) %x\n", (int32_t) mode, err);

    IONDRVFB_END(validateDisplayMode,err,0,0);
    return (err);
}

void IONDRVFramebuffer::getCurrentConfiguration( void )
{
    IONDRVFB_START(getCurrentConfiguration,0,0,0);
    IOReturn            err;
    VDSwitchInfoRec     switchInfo;
    VDTimingInfoRec     timingInfo;
    VDGrayRecord        grayRec;

    grayRec.csMode = 0;                 // turn off luminance map
    err = _doControl( this, cscSetGray, &grayRec );
    // driver refused => mono display
    grayMode = ((noErr == err) && (0 != grayRec.csMode));

#if 0
    VDPageInfo          pageInfo;
    if (noErr == _doStatus(this, cscGetMode, &pageInfo))
    {
        _doControl( this, cscSetMode, &pageInfo);
        _doControl( this, cscGrayPage, &pageInfo);
    }
#endif

    err = _doStatus( this, cscGetCurMode, &switchInfo );
    if (err != noErr)
    {
        DEBG(thisName, " cscGetCurMode failed\n");
    }
    else
    {
        if ((kDisplayModeIDBootProgrammable != switchInfo.csData) || !currentDisplayMode)
        {
			currentDisplayMode = switchInfo.csData;
		}
        currentDepth       = mapDepthIndex(currentDisplayMode, (IOIndex) switchInfo.csMode, true);
        currentPage        = switchInfo.csPage;
    
        timingInfo.csTimingMode   = currentDisplayMode;
        timingInfo.csTimingFormat = kDeclROMtables;
        err = _doStatus( this, cscGetModeTiming, &timingInfo );
        if ((noErr == err) && (kDeclROMtables == timingInfo.csTimingFormat))
            __private->currentModeTiming = timingInfo.csTimingData;
        else
            __private->currentModeTiming = timingInvalid;

        mach_vm_address_t vAddr = (mach_vm_address_t) switchInfo.csBaseAddr;
        ppnum_t           page  = 0;


        if (!vramMemory)
            vramMemory = getVRAMRange();

        if (vAddr)
        {
			if (vAddr & 1) page = static_cast<ppnum_t>(atop_64(vAddr));
			else           page = pmap_find_phys(kernel_pmap, vAddr);
            if (!page) panic("pmap_find_phys %qx", vAddr);
            __private->physicalFramebuffer = ptoa_64(page) + (PAGE_MASK & vAddr & ~1);
        }
    }
    IONDRVFB_END(getCurrentConfiguration,0,0,0);
}

IOReturn IONDRVFramebuffer::setupForCurrentConfig( void )
{
    IONDRVFB_START(setupForCurrentConfig,0,0,0);
	getCurrentConfiguration();
    IOReturn err = super::setupForCurrentConfig();
    IONDRVFB_END(setupForCurrentConfig,err,0,0);
	return (err);
}

IODeviceMemory * IONDRVFramebuffer::makeSubRange(
    IOPhysicalAddress64   start,
    IOPhysicalLength64    length )
{
    IONDRVFB_START(makeSubRange,length,0,0);
    IOMemoryDescriptor * mem = 0;
    UInt32               numMaps, i;
    IOService *          device;

    device = fNub;
    numMaps = device->getDeviceMemoryCount();

    for (i = 0; (!mem) && (i < numMaps); i++)
    {
        mem = device->getDeviceMemoryWithIndex(i);
        if (!mem)
            continue;

        mem = IOSubMemoryDescriptor::withSubRange(
                             mem,
                             start - mem->getPhysicalSegment(0, 0, kIOMemoryMapperNone),
                             length, kIODirectionNone);
    }
    if (!mem)
        mem = IOMemoryDescriptor::withAddressRange(
			start, length, kIODirectionNone | kIOMemoryMapperNone, NULL);

    IONDRVFB_END(makeSubRange,0,0,0);
    return ((IODeviceMemory *) mem);
}

IODeviceMemory * IONDRVFramebuffer::getApertureRange( IOPixelAperture aper )
{
    IONDRVFB_START(getApertureRange,0,0,0);
    IOReturn                    err;
    IOPixelInformation          info;
    IOByteCount                 bytes;

    if (!__private->physicalFramebuffer)
    {
        IONDRVFB_END(getApertureRange,-1,__LINE__,0);
        return (NULL);
    }

    err = getPixelInformation( currentDisplayMode, currentDepth, aper,
                               &info );
    if (err)
    {
        DEBG(thisName, " getPixelInformation(%x) 0x%x\n", err, (uint32_t )currentDisplayMode);
        IONDRVFB_END(getApertureRange,-1,__LINE__,0);
        return (NULL);
    }

    // <rdar://problem/30217326> CD-IOP Regression - J45G - Launching Pixelmator/Chess causes a hang
    // <rdar://problem/29671853> CD-IOP Regression - J45G - Most Space - repeatedly checking/unchecking AGS causes systems to sometimes hang
    VDSwitchInfoRec     switchInfo;
    err = _doStatus( this, cscGetCurMode, &switchInfo );
    if (err != noErr)
    {
        DEBG(thisName, " cscGetCurMode failed\n");
        IONDRVFB_END(getApertureRange,-2,__LINE__,0);
        return (NULL);
    }
    else if (0 == switchInfo.csBaseAddr)
    {
        IONDRVFB_END(getApertureRange,-3,__LINE__,0);
        return (NULL);
    }

    bytes = (info.bytesPerRow * info.activeHeight) + 128;

    IODeviceMemory * iodm = makeSubRange(__private->physicalFramebuffer, bytes);
    IONDRVFB_END(getApertureRange,0,0,0);
    return (iodm);
}

IODeviceMemory * IONDRVFramebuffer::findVRAM( void )
{
    IONDRVFB_START(findVRAM,0,0,0);
    IOBootNDRV * bootndrv;
    if ((bootndrv = OSDynamicCast(IOBootNDRV, fNdrv)))
    {
        bootndrv->fVRAMDesc->retain();
        IODeviceMemory * iodm = (IODeviceMemory *) bootndrv->fVRAMDesc;
        IONDRVFB_END(findVRAM,0,0,0);
        return (iodm);
    }

    IONDRVFB_END(findVRAM,-1,__LINE__,0);
    return (NULL);
}

const char * IONDRVFramebuffer::getPixelFormats( void )
{
    IONDRVFB_START(getPixelFormats,0,0,0);
    static const char * ndrvPixelFormats =
        IO1BitIndexedPixels "\0"
        IO2BitIndexedPixels "\0"
        IO4BitIndexedPixels "\0"
        IO8BitIndexedPixels "\0"
        IO16BitDirectPixels "\0"
        IO32BitDirectPixels "\0"
        "\0";

    IONDRVFB_END(getPixelFormats,0,0,0);
    return (ndrvPixelFormats);
}

IOItemCount IONDRVFramebuffer::getDisplayModeCount( void )
{
    IONDRVFB_START(getDisplayModeCount,0,0,0);
    IOItemCount num = iterateAllModes(0);
    IONDRVFB_END(getDisplayModeCount,num,0,0);
    return (num);
}

IOReturn IONDRVFramebuffer::getDisplayModes( IODisplayModeID * allDisplayModes )
{
    IONDRVFB_START(getDisplayModes,0,0,0);
    iterateAllModes( allDisplayModes );
    IONDRVFB_END(getDisplayModes,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

IOReturn IONDRVFramebuffer::validateDetailedTiming(
    void * _desc, IOByteCount descripSize )
{
    IONDRVFB_START(validateDetailedTiming,0,0,0);
    IOReturn err;

    if (descripSize == sizeof(IOFBDisplayModeDescription))
    {
        IOFBDisplayModeDescription * desc = (IOFBDisplayModeDescription *) _desc;
        VDDetailedTimingRec *        detailed = (VDDetailedTimingRec *) &desc->timingInfo.detailedInfo.v2;
    
        err = setDetailedTiming( kDisplayModeIDPreflight,
                                kModePreflight, detailed, sizeof(VDDetailedTimingRec));
    
        if (kIOReturnSuccess == err)
            err = getResInfoForDetailed(kDisplayModeIDPreflight, detailed, &desc->info);
    }
    else
        err = setDetailedTiming( kDisplayModeIDPreflight,
                                 kModePreflight, _desc, descripSize);

    IONDRVFB_END(validateDetailedTiming,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setDetailedTimings( OSArray * array )
{
    IONDRVFB_START(setDetailedTimings,0,0,0);
    IOReturn    err;
    UInt32 *    newCurrent;
    IOItemCount newCount;

    if (!array)
    {
        if (detailedTimings)
        {
            IODelete( detailedTimingsCurrent, UInt32, detailedTimings->getCount());
            detailedTimingsCurrent = 0;
        }
        removeProperty( kIOFBDetailedTimingsKey );
        detailedTimings = 0;
        detailedTimingsSeed++;
        IONDRVFB_END(setDetailedTimings,kIOReturnSuccess,0,0);
        return (kIOReturnSuccess);
    }

    newCount = array->getCount();
    newCurrent = IONew(UInt32, newCount);
    if (newCurrent)
    {
        if (detailedTimings)
            IODelete( detailedTimingsCurrent, UInt32, detailedTimings->getCount());
        detailedTimingsCurrent = newCurrent;
        bzero( newCurrent, newCount * sizeof( UInt32));
        setProperty( kIOFBDetailedTimingsKey, array );  // retains
        detailedTimings = array;
        detailedTimingsSeed++;

//      if (((UInt32) currentDisplayMode) >= ((UInt32) kDisplayModeIDReservedBase))
        if (currentDisplayMode == kDisplayModeIDBootProgrammable)
        {
            VDDetailedTimingRec                 look;
            VDScalerRec                         scaler;
            IODetailedTimingInformationV2 *     detailed;
            OSData *                            data;
            IODisplayModeID                     newDisplayMode;
            bool                                bootScaled;

            newDisplayMode = currentDisplayMode;

            bzero( &look, sizeof( VDDetailedTimingRec) );
            look.csTimingSize = sizeof32( VDDetailedTimingRec);
            look.csDisplayModeID = currentDisplayMode;
            err = _doStatus( this, cscGetDetailedTiming, &look );

            bzero( &scaler, sizeof( VDScalerRec) );
            scaler.csScalerSize = sizeof32( VDScalerRec);
            scaler.csDisplayModeID = currentDisplayMode;
            bootScaled = (noErr == _doStatus( this, cscGetScaler, &scaler ));

            if (bootScaled 
              && (scaler.csHorizontalPixels == look.csHorizontalActive)
              && (scaler.csVerticalPixels   == look.csVerticalActive)
              && (!(kIOScaleRotateFlags      & scaler.csScalerFlags))
              && (!scaler.csHorizontalInset)
              && (!scaler.csVerticalInset))
            {
                scaler.csHorizontalPixels = 0;
                scaler.csVerticalPixels   = 0;
                scaler.csScalerFlags      = 0;
            }

            if (kIOReturnSuccess == err)
            {
                for (int i = 0;
                        (data = OSDynamicCast(OSData, detailedTimings->getObject(i)));
                        i++)
                {
                    detailed = (IODetailedTimingInformationV2 *) data->getBytesNoCopy();

                    if ((detailed->horizontalActive != look.csHorizontalActive)
                            || (detailed->verticalActive != look.csVerticalActive))
                        continue;

                    if (!bootScaled || (!scaler.csHorizontalPixels && !scaler.csVerticalPixels))
                    {
                        IOFixed1616 refreshRate1, refreshRate2;
                        refreshRate1 = DetailedRefreshRate(detailed);
                        refreshRate2 = DetailedRefreshRate((IODetailedTimingInformationV2 *) &look);
                        refreshRate1 = (refreshRate1 + 0x8000) >> 16;
                        refreshRate2 = (refreshRate2 + 0x8000) >> 16;
                        if (refreshRate1 != refreshRate2)
                            continue;
                    }

                    if (bootScaled
                            && ((detailed->horizontalScaled      != scaler.csHorizontalPixels)
                             || (detailed->verticalScaled        != scaler.csVerticalPixels)
                             || (detailed->horizontalScaledInset != scaler.csHorizontalInset)
                             || (detailed->verticalScaledInset   != scaler.csVerticalInset)
                             || (detailed->scalerFlags           != scaler.csScalerFlags)))
                        continue;

                    newDisplayMode = detailed->detailedTimingModeID;
                    break;
                }
            }

            if (newDisplayMode != currentDisplayMode)
            {
                err = validateDisplayMode( newDisplayMode, 0, 0 );
                currentDisplayMode = newDisplayMode;
            }
        }

        err = kIOReturnSuccess;
    }
    else
        err = kIOReturnNoMemory;

    IONDRVFB_END(setDetailedTimings,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::getInformationForDisplayMode(
    IODisplayModeID displayMode, IODisplayModeInformation * info )
{
    IONDRVFB_START(getInformationForDisplayMode,displayMode,0,0);
    IOReturn                    err;

    err = getResInfoForMode( displayMode, info );
    if (err)
        err = kIOReturnUnsupportedMode;

    IONDRVFB_END(getInformationForDisplayMode,err,0,0);
    return (err);
}


UInt64 IONDRVFramebuffer::getPixelFormatsForDisplayMode(
    IODisplayModeID /* displayMode */, IOIndex depthIndex )
{
    IONDRVFB_START(getPixelFormatsForDisplayMode,depthIndex,0,0);
    IONDRVFB_END(getPixelFormatsForDisplayMode,1 << depthIndex,0,0);
    return (1 << depthIndex);
}

IOReturn IONDRVFramebuffer::getPixelInformation(
    IODisplayModeID displayMode, IOIndex depth,
    IOPixelAperture aperture, IOPixelInformation * info )
{
    IONDRVFB_START(getPixelInformation,displayMode,depth,aperture);
    SInt32                      err;
    VDVideoParametersInfoRec    pixelParams;
    VPBlock                     pixelInfo;
    UInt32                      pixelType;
    const char *                pixelFormat;
    

    bzero( info, sizeof( *info));

    if (aperture)
    {
        IONDRVFB_END(getPixelInformation,kIOReturnUnsupportedMode,__LINE__,0);
        return (kIOReturnUnsupportedMode);
    }

    err = validateDisplayMode( displayMode, 0, 0 );
    if (err)
    {
        IONDRVFB_END(getPixelInformation,err,__LINE__,0);
        return (err);
    }

    do
    {
        pixelParams.csDisplayModeID = displayMode;
        pixelParams.csDepthMode     = mapDepthIndex(displayMode, depth, false);
        pixelParams.csVPBlockPtr    = &pixelInfo;
        err = _doStatus( this, cscGetVideoParameters, &pixelParams );
        if (err)
            continue;

        info->flags             = accessFlags;

        info->activeWidth       = pixelInfo.vpBounds.right;
        info->activeHeight      = pixelInfo.vpBounds.bottom;
#if __LP64__
        info->bytesPerRow       = pixelInfo.vpRowBytes;
#else
        info->bytesPerRow       = pixelInfo.vpRowBytes & 0x7fff;
#endif
        info->bytesPerPlane     = pixelInfo.vpPlaneBytes;
        info->bitsPerPixel      = pixelInfo.vpPixelSize;

        switch (pixelInfo.vpPixelSize / 8)
        {
            default:
                pixelInfo.vpPixelSize = 8;
            case 0:
            case 1:
                strlcpy(info->pixelFormat, IO8BitIndexedPixels, sizeof(info->pixelFormat));
                info->pixelType = kIOCLUTPixels;
                info->componentMasks[0] = (1 << pixelInfo.vpPixelSize) - 1;
                info->bitsPerPixel = pixelInfo.vpPixelSize;
                info->componentCount = 1;
                info->bitsPerComponent = pixelInfo.vpPixelSize;
                break;

            case 2:
                strlcpy(info->pixelFormat, IO16BitDirectPixels, sizeof(info->pixelFormat));
                info->pixelType = kIORGBDirectPixels;
                info->componentMasks[0] = 0x7c00;
                info->componentMasks[1] = 0x03e0;
                info->componentMasks[2] = 0x001f;
                info->bitsPerPixel = 16;
                info->componentCount = 3;
                info->bitsPerComponent = 5;
                break;

            case 4:
            case 8:
                info->bitsPerPixel     = pixelInfo.vpPixelSize;
                info->componentCount   = pixelInfo.vpCmpCount;
                info->bitsPerComponent = pixelInfo.vpCmpSize;

                pixelType = kIORGBDirectPixels;
                switch (pixelInfo.vpPixelType)
                {
                    case kIORGBSignedFloatingPointPixels:
                        pixelType = pixelInfo.vpPixelType;
                        if (info->bitsPerComponent == 16)
                            pixelFormat = kIO16BitFloatPixels;
                        else
                            pixelFormat = kIO32BitFloatPixels;
                        break;

                    case kIORGBSignedDirectPixels:
                        pixelType = pixelInfo.vpPixelType;
                        // fall thru
                    default:
                        if (info->bitsPerComponent == 10)
                            pixelFormat = kIO30BitDirectPixels;
                        else if (info->bitsPerComponent == 16)
                            pixelFormat = kIO64BitDirectPixels;
                        else
                            pixelFormat = IO32BitDirectPixels;
                        break;
                }

                strlcpy(info->pixelFormat, pixelFormat, sizeof(info->pixelFormat));
                info->pixelType = pixelType;

                switch (info->bitsPerComponent)
                {
                    default:
                    case 8:
                        info->componentMasks[0] = 0x00ff0000;
                        info->componentMasks[1] = 0x0000ff00;
                        info->componentMasks[2] = 0x000000ff;
                        break;

                    case 10:
                        info->componentMasks[0] = 0x3ff00000;
                        info->componentMasks[1] = 0x000ffc00;
                        info->componentMasks[2] = 0x000003ff;
                        break;

                    case 16:
                        info->componentMasks[0] = 0x0000ffff;
//                      info->componentMasks[1] = 0x00000000;

//                      info->componentMasks[2] = 0x00000000;
                        info->componentMasks[3] = 0xffff0000;

//                      info->componentMasks[4] = 0x00000000;
                        info->componentMasks[5] = 0x0000ffff;
                        break;
                    
                    case 32:
//                      info->componentMasks[0]  = 0x00000000;
                        info->componentMasks[1]  = 0xffffffff;
//                      info->componentMasks[2]  = 0x00000000;
//                      info->componentMasks[3]  = 0x00000000;

//                      info->componentMasks[4]  = 0x00000000;
//                      info->componentMasks[5]  = 0x00000000;
                        info->componentMasks[6]  = 0xffffffff;
//                      info->componentMasks[7]  = 0x00000000;

//                      info->componentMasks[8]  = 0x00000000;
//                      info->componentMasks[9]  = 0x00000000;
//                      info->componentMasks[10] = 0x00000000;
                        info->componentMasks[11] = 0xffffffff;
                        break;
                }
                break;
        }
    }
    while (false);

    IONDRVFB_END(getPixelInformation,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::getTimingInfoForDisplayMode(
    IODisplayModeID displayMode, IOTimingInformation * info )
{
    IONDRVFB_START(getTimingInfoForDisplayMode,displayMode,0,0);
    VDTimingInfoRec             timingInfo;

    OSStatus                    err;

    err = validateDisplayMode( displayMode, 0, 0 );
    if (err)
    {
        IONDRVFB_END(getTimingInfoForDisplayMode,err,0,0);
        return (err);
    }

    timingInfo.csTimingMode = displayMode;
    // in case the driver doesn't do it:
    timingInfo.csTimingFormat = kDeclROMtables;
    err = _doStatus( this, cscGetModeTiming, &timingInfo);
    if (err == noErr)
    {
        if (timingInfo.csTimingFormat == kDeclROMtables)
            info->appleTimingID = timingInfo.csTimingData;
        else
            info->appleTimingID = timingInvalid;

        if (info->flags & kIODetailedTimingValid)
        {
            VDDetailedTimingRec * look = (VDDetailedTimingRec *) &info->detailedInfo.v2;
            VDScalerRec           scaler;

            bzero( look, sizeof( VDDetailedTimingRec) );
            look->csTimingSize = sizeof32( VDDetailedTimingRec);
            look->csDisplayModeID = displayMode;
            err = _doStatus( this, cscGetDetailedTiming, look );
            if (kIOReturnSuccess != err)
                info->flags &= ~kIODetailedTimingValid;
            else
            {
                bzero( &info->detailedInfo.v2.__reservedA[0], sizeof( info->detailedInfo.v2.__reservedA));
                bzero( &info->detailedInfo.v2.__reservedB[0], sizeof( info->detailedInfo.v2.__reservedB));

                bzero( &scaler, sizeof( VDScalerRec) );
                scaler.csScalerSize = sizeof32( VDScalerRec);
                scaler.csDisplayModeID = displayMode;
                err = _doStatus( this, cscGetScaler, &scaler );
                if (kIOReturnSuccess == err)
                {
                    info->flags |= kIOScalingInfoValid;
                    info->detailedInfo.v2.scalerFlags           = scaler.csScalerFlags;
                    info->detailedInfo.v2.horizontalScaled      = scaler.csHorizontalPixels;
                    info->detailedInfo.v2.verticalScaled        = scaler.csVerticalPixels;
                    info->detailedInfo.v2.horizontalScaledInset = scaler.csHorizontalInset;
                    info->detailedInfo.v2.verticalScaledInset   = scaler.csVerticalInset;
                }
                else
                {
                    info->detailedInfo.v2.scalerFlags      = 0;
                    info->detailedInfo.v2.horizontalScaled = 0;
                    info->detailedInfo.v2.verticalScaled   = 0;
                }
            }
        }

        IONDRVFB_END(getTimingInfoForDisplayMode,kIOReturnSuccess,0,0);
        return (kIOReturnSuccess);
    }

    IONDRVFB_END(getTimingInfoForDisplayMode,kIOReturnUnsupportedMode,0,0);
    return (kIOReturnUnsupportedMode);
}

IOReturn IONDRVFramebuffer::getCurrentDisplayMode(
    IODisplayModeID * displayMode, IOIndex * depth )
{
    IONDRVFB_START(getCurrentDisplayMode,0,0,0);
    if (displayMode)
        *displayMode = currentDisplayMode;
    if (depth)
        *depth = currentDepth;

    IONDRVFB_END(getCurrentDisplayMode,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

IOReturn IONDRVFramebuffer::setDisplayMode( IODisplayModeID displayMode, IOIndex depth )
{
    IONDRVFB_START(setDisplayMode,displayMode,depth,0);
    SInt32              err;
    VDSwitchInfoRec     switchInfo;
    VDPageInfo          pageInfo;
    VDTimingInfoRec     timingInfo;
    UInt32              timingID;

    if (0 == powerState)
    {
        IONDRVFB_END(setDisplayMode,kIOReturnNotReady,0,0);
        return (kIOReturnNotReady);
    }

    timingInfo.csTimingMode   = displayMode;
    timingInfo.csTimingFormat = kDeclROMtables;
    err = _doStatus( this, cscGetModeTiming, &timingInfo );
    if ((noErr == err) && (kDeclROMtables == timingInfo.csTimingFormat))
        timingID = timingInfo.csTimingData;
    else
        timingID = timingInvalid;

#if 1
    if (!online && (timingApple_0x0_0hz_Offline != timingID))
    {
        DEBG(thisName, " offline\n");
        IONDRVFB_END(setDisplayMode,kIOReturnSuccess,__LINE__,0);
        return (kIOReturnSuccess);
    }
#endif

    err = validateDisplayMode( displayMode, 0, 0 );
    if (err)
    {
        IONDRVFB_END(setDisplayMode,err,__LINE__,0);
        return (err);
    }

    switchInfo.csData = displayMode;
    switchInfo.csMode = mapDepthIndex(displayMode, depth, false);
    switchInfo.csPage = 0;
    err = _doControl( this, cscSwitchMode, &switchInfo);
    if (err)
        DEBG(thisName, " %d: cscSwitchMode(0x%08x) %d\n",
              online, (int32_t) displayMode, (int) err);

    // duplicate QD InitGDevice
    pageInfo.csMode = switchInfo.csMode;
    pageInfo.csData = 0;
    pageInfo.csPage = 0;
    _doControl( this, cscSetMode, &pageInfo);

#if 0
    if (mirrored)
    {
        VDGammaRecord   gammaRec;
        OSStatus                gammaErr;

        gammaErr = _doStatus( this, cscGetGamma, &gammaRec );
        if (noErr == gammaErr)
            _doControl( this, cscSetGamma, &gammaRec );
        else
            _doControl( this, cscGrayPage, &pageInfo);
    }
    else
        _doControl( this, cscGrayPage, &pageInfo);
#endif

    IONDRVFB_END(setDisplayMode,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setStartupDisplayMode(
    IODisplayModeID displayMode, IOIndex depth )
{
    IONDRVFB_START(setStartupDisplayMode,displayMode,depth,0);
    SInt32              err;
    VDSwitchInfoRec     switchInfo;

    err = validateDisplayMode( displayMode, 0, 0 );
    if (err)
    {
        IONDRVFB_END(setStartupDisplayMode,err,__LINE__,0);
        return (err);
    }

    switchInfo.csData = displayMode;
    switchInfo.csMode = mapDepthIndex(displayMode, depth, false);
    err = _doControl( this, cscSavePreferredConfiguration, &switchInfo);

    IONDRVFB_END(setStartupDisplayMode,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::getStartupDisplayMode(
    IODisplayModeID * displayMode, IOIndex * depth )
{
    IONDRVFB_START(getStartupDisplayMode,0,0,0);
    SInt32              err;
    VDSwitchInfoRec     switchInfo;

    err = _doStatus( this, cscGetPreferredConfiguration, &switchInfo);
    if (err == noErr)
    {
        *displayMode = switchInfo.csData;
        *depth       = mapDepthIndex(switchInfo.csData, (IOIndex) switchInfo.csMode, true);
    }

    IONDRVFB_END(getStartupDisplayMode,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setApertureEnable( IOPixelAperture /* aperture */,
        IOOptionBits /* enable */ )
{
    IONDRVFB_START(setApertureEnable,0,0,0);
    IONDRVFB_END(setApertureEnable,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

IOReturn IONDRVFramebuffer::setCLUTWithEntries(
    IOColorEntry * colors, UInt32 index, UInt32 numEntries,
    IOOptionBits options )
{
    IONDRVFB_START(setCLUTWithEntries,index,numEntries,options);
    IOReturn            err;
    VDSetEntryRecord    setEntryRec;
    VDClutBehavior      clutSetting;
    VDGrayRecord        grayRec;

    if (0 == powerState)
    {
        IONDRVFB_END(setCLUTWithEntries,kIOReturnSuccess,0,0);
        return (kIOReturnSuccess);
    }

    if (options & kSetCLUTWithLuminance)
        grayRec.csMode = 1;             // turn on luminance map
    else
        grayRec.csMode = 0;             // turn off luminance map

    if (grayRec.csMode != lastGrayMode)
    {
        _doControl( this, cscSetGray, &grayRec);
        lastGrayMode = grayRec.csMode;
    }

    if (options & kSetCLUTImmediately)
        clutSetting = kSetClutAtSetEntries;
    else
        clutSetting = kSetClutAtVBL;

    if (clutSetting != lastClutSetting)
    {
        _doControl( this, cscSetClutBehavior, &clutSetting);
        lastClutSetting = clutSetting;
    }

    if (options & kSetCLUTByValue)
        setEntryRec.csStart = -1;
    else
        setEntryRec.csStart = index;

    setEntryRec.csTable = (ColorSpec *) colors;
    setEntryRec.csCount = numEntries - 1;
    err = _doControl( this, cscSetEntries, &setEntryRec);

    IONDRVFB_END(setCLUTWithEntries,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setGammaTable(
                                          UInt32 channelCount, UInt32 dataCount,
                                          UInt32 dataWidth, void * data )
{
    IONDRVFB_START(setGammaTable,channelCount,dataCount,dataWidth);
    IOReturn err = setGammaTable(channelCount, dataCount, dataWidth, data, true);
    IONDRVFB_END(setGammaTable,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setGammaTable(
                                          UInt32 channelCount, UInt32 dataCount,
                                          UInt32 dataWidth, void * data, bool syncToVBL )
{
    IONDRVFB_START(setGammaTable2,channelCount,dataCount,dataWidth);
    IOReturn            err;
    VDClutBehavior      clutSetting;
    VDGammaRecord       gammaRec;
    GammaTbl *          table = (GammaTbl *) data;

    if (0 == powerState)
    {
        IONDRVFB_END(setGammaTable2,kIOReturnSuccess,__LINE__,0);
        return (kIOReturnSuccess);
    }
    if (!data)
    {
        IONDRVFB_END(setGammaTable2,kIOReturnBadArgument,__LINE__,0);
        return (kIOReturnBadArgument);
    }

    if (__private->deferCLUTSet)
    {
        clutSetting = kSetClutAtSetEntries;
        if (clutSetting != lastClutSetting)
        {
            _doControl( this, cscSetClutBehavior, &clutSetting);
            lastClutSetting = clutSetting;
        }
    }

    table->gVersion     = 0;
    table->gType        = static_cast<short>(syncToVBL);
    table->gFormulaSize = 0;
    table->gChanCnt     = channelCount;
    table->gDataCnt     = dataCount;
    table->gDataWidth   = dataWidth;

    gammaRec.csGTable = (Ptr) table;

    // Attempt with syncToVBL case, if that fails fall back to legacy (gType == 0) and retry.
    err = _doControl( this, cscSetGamma, &gammaRec );
    if ((kIOReturnSuccess != err) && (0 != table->gType))
    {
        table->gType        = 0;
        err = _doControl( this, cscSetGamma, &gammaRec );
    }
    if (kIOReturnSuccess != err)
        DEBG(thisName, " cscSetGamma(%d, %d, %d) set: %d\n",
                (uint32_t) channelCount, (uint32_t) dataCount, (uint32_t) dataWidth, err);

    IONDRVFB_END(setGammaTable2,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setMirror( IONDRVFramebuffer * other )
{
    IONDRVFB_START(setMirror,0,0,0);
    IOReturn            err = kIOReturnSuccess;
//    IONDRVFramebuffer * next;
    VDMirrorRec         mirror;

    if (mirrored == (other != 0))
    {
        IONDRVFB_END(setMirror,kIOReturnSuccess,0,0);
        return (kIOReturnSuccess);
    }

    if (!nextDependent)
    {
        IONDRVFB_END(setMirror,kIOReturnUnsupported,0,0);
        return (kIOReturnUnsupported);
    }
//    if (other && (other != nextDependent))
//    {
//        IONDRVFB_END(setMirror,kIOReturnUnsupported,__LINE__,0);
//        return (kIOReturnUnsupported);
//    }

    do
    {
        bzero( &mirror, sizeof( mirror));
        mirror.csMirrorSize = sizeof32(VDMirrorRec);
        mirror.csMirrorFlags = 0
                               | kMirrorCanMirrorMask
                               | kMirrorAreMirroredMask
                               | kMirrorUnclippedMirrorMask
                               | kMirrorClippedMirrorMask
                               | kMirrorHAlignCenterMirrorMask
                               | kMirrorVAlignCenterMirrorMask;
        MAKE_REG_ENTRY( (RegEntryID *)&mirror.csMirrorResultID, 0 );
        if (0 == other)
        {
            MAKE_REG_ENTRY( ((RegEntryID *)&mirror.csMirrorRequestID), 0 );
        }
        else
        {
            MAKE_REG_ENTRY( ((RegEntryID *)&mirror.csMirrorRequestID), other->getProvider() );
        }

        err = _doControl( this, cscSetMirror, &mirror );
        if (err)
            continue;

        mirrored = (other != 0);
//        if ((next = OSDynamicCast(IONDRVFramebuffer, nextDependent)))
//            next->setMirror( (other != 0) ? this : 0 );
    }
    while (false);

    IONDRVFB_END(setMirror,err,0,0);
    return (err);
}


IOReturn IONDRVFramebuffer::setAttribute( IOSelect attribute, uintptr_t _value )
{
    IONDRVFB_START(setAttribute,attribute,_value,0);
    IOReturn            err = kIOReturnSuccess;
    IONDRVFramebuffer * other = 0;
    uintptr_t *         data = (uintptr_t *) _value;
    UInt32              value;

    switch (attribute)
    {
        case kIOPowerStateAttribute:
            err = ndrvSetPowerState( (UInt32)_value );
            break;

        case kIODriverPowerAttribute:
            err = ndrvSetPowerState( (UInt32)_value );
            break;

        case kIOSystemPowerAttribute:
            switch (_value)
            {
              case kIOMessageSystemWillPowerOff:
                if ((kIODVIPowerEnableFlag & __private->displayConnectFlags) && powerState)
                {
                    err = ndrvGetSetFeature( kDVIPowerSwitchFeature, 0, NULL );
                    if (kIOReturnSuccess == err)
                        IOSleep( kDVIPowerSwitchPowerOffDelay );
                }
                /* fall thru */
              case kIOMessageSystemWillRestart:
                if (ndrvState && powerState)
                {
                    IONDRVControlParameters pb;

                    err = doDriverIO( 0, &pb,
                                    kIONDRVCloseCommand, kIONDRVImmediateIOCommandKind );
                    err = doDriverIO( 0, fNub,
                                    kIONDRVFinalizeCommand, kIONDRVImmediateIOCommandKind );
                    ndrvState = 0;

                    DEBG(thisName, " kIOSystemPowerAttribute finalize(%d)\n", err);
                }
            }
            err = kIOReturnSuccess;
            break;

        case kIOFBSpeedAttribute:
            __private->reducedSpeed = (UInt32) _value;
            if (powerState)
                err = ndrvUpdatePowerState();
            break;

        case kIOMirrorAttribute:

            do
            {
                value = data[0] & 1;
                if (value)
                {
                    other = OSDynamicCast( IONDRVFramebuffer, (OSObject *) data[1] );
                    if (!other)
                    {
                        err = kIOReturnBadArgument;
                        continue;
                    }
                }

                err = setMirror( other );
                if (kIOReturnSuccess != err)
                    continue;

                OSNumber * num = OSDynamicCast(OSNumber, getProperty(kIOFBDependentIndexKey));
                mirrorPrimary = mirrored && (!num || (1 == num->unsigned32BitValue()));

//                (void) setDisplayMode( currentDisplayMode, currentDepth );
            }
            while (false);

            break;

        default:
            err = super::setAttribute( attribute, _value );
    }

    IONDRVFB_END(setAttribute,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::getAttribute( IOSelect attribute, uintptr_t * value )
{
    IONDRVFB_START(getAttribute,attribute,0,0);
    IOReturn                    err = kIOReturnSuccess;
    VDSupportsHardwareCursorRec hwCrsrSupport;
    VDMirrorRec                 mirror;
    IONDRVFramebuffer *         other;

    switch (attribute)
    {
        case kIOHardwareCursorAttribute:

            UInt32 flags;

            hwCrsrSupport.csReserved1 = 0;
            hwCrsrSupport.csReserved2 = 0;
            flags = ((kIOReturnSuccess == _doStatus( this, cscSupportsHardwareCursor, &hwCrsrSupport))
                        && true && (hwCrsrSupport.csSupportsHardwareCursor))
                        ? kIOFBHWCursorSupported : 0;
            if (flags)
                flags |= (hwCrsrSupport.csReserved1 & ~kIOFBHWCursorSupported);
            *value = flags;
            break;

        case kIODeferCLUTSetAttribute:

            *value = __private->deferCLUTSet;
            break;

        case kIOMirrorAttribute:

            bzero( &mirror, sizeof( mirror));
            mirror.csMirrorSize = sizeof32(VDMirrorRec);

            other = OSDynamicCast( IONDRVFramebuffer, (OSObject *) value[0] );
            if (other)
            {
                MAKE_REG_ENTRY( ((RegEntryID *)&mirror.csMirrorRequestID), other->getProvider() );
            }
            else
            {
                MAKE_REG_ENTRY( ((RegEntryID *)&mirror.csMirrorRequestID), 0 );
            }
            err = _doStatus( this, cscGetMirror, &mirror );
            value[0] = mirror.csMirrorSupportedFlags;

            if (kIOReturnSuccess == err)
            {
                OSNumber * num = OSDynamicCast(OSNumber, getProperty(kIOFBDependentIndexKey));
                if (!num || (1 == num->unsigned32BitValue()))
                    value[0] |= kIOMirrorIsPrimary;

                if (kMirrorClippedMirrorMask & mirror.csMirrorSupportedFlags)
                    value[0] |= kIOMirrorHWClipped;

                do
                {
                    VDVideoParametersInfoRec    pixelParams;
                    VPBlock                     pixelInfo;
                    IODisplayModeInformation    modeInfo;
                    IOReturn                    er;

                    pixelParams.csDisplayModeID = currentDisplayMode;
                    pixelParams.csDepthMode     = mapDepthIndex(currentDisplayMode, currentDepth, false);
                    pixelParams.csVPBlockPtr    = &pixelInfo;
                    er = _doStatus( this, cscGetVideoParameters, &pixelParams );
                    if (er)
                        continue;
                    er = getResInfoForMode( currentDisplayMode, &modeInfo );
                    if (er)
                        continue;
                    if ((modeInfo.nominalWidth != (UInt32) pixelInfo.vpBounds.right)
                            || (modeInfo.nominalHeight != (UInt32) pixelInfo.vpBounds.bottom))
                        value[0] |= kIOMirrorHWClipped;
                }
                while (false);

                DEBG(thisName, " kIOMirrorAttribute %08lx csMirrorSupportedFlags %08x, csMirrorFeatures %08x\n",
                     value[0], (uint32_t) mirror.csMirrorSupportedFlags, (uint32_t) mirror.csMirrorFeatures);

                err = kIOReturnSuccess;
            }
            break;

        case kIOMirrorDefaultAttribute:
            {
                IORegistryEntry   * entry;
                IORegistryEntry   * root;
                IONDRVFramebuffer * next;
                OSNumber *              num;
                OSData *                data = 0;
                enum { kIOMirrorHint = 0x10000 };

                num = OSDynamicCast(OSNumber, getProperty("graphic-options"));
                if (num)
                    value[0] = num->unsigned32BitValue();
                else
                {
                    value[0] = 0;

                    data = OSDynamicCast(OSData, fNub->getProperty("graphic-options"));
                    if (!data && (root = IORegistryEntry::fromPath("/", gIODTPlane)))
                    {
                        data = OSDynamicCast(OSData, root->getProperty("graphic-options"));
                        root->release();
                    }

                    if (data)
                        value[0] = *((UInt32 *) data->getBytesNoCopy());
                    else if ((entry = IORegistryEntry::fromPath("mac-io/battery", gIODTPlane))
                             || (entry = IORegistryEntry::fromPath("mac-io/via-pmu/battery", gIODTPlane)))
                    {
                        value[0] &= ~kIOMirrorDefault;
                        entry->release();
                    }
                    setProperty("graphic-options", value[0], 32);
                }

                if (online
                        && (kIOMirrorDefault & value[0])
                        && (next = OSDynamicCast(IONDRVFramebuffer, nextDependent))
                        && next->getOnlineState())
                    value[0] |= kIOMirrorHint;
                err = kIOReturnSuccess;
                break;
            }

        case kIOVRAMSaveAttribute:
            DEBG1(thisName, " kIOVRAMSaveAttribute on(%d), mirr(%d), prim(%d)\n",
                 online, mirrored, mirrorPrimary );
            *value = (online && (!mirrored || !mirrorPrimary));
            break;

        default:
            err = super::getAttribute( attribute, value );
    }

    IONDRVFB_END(getAttribute,err,0,0);
    return (err);
}

UInt32 IONDRVFramebuffer::getConnectionCount( void )
{
    IONDRVFB_START(getConnectionCount,0,0,0);
    VDMultiConnectInfoRec       multiConnection;
    UInt32 num = 1;

    multiConnection.csDisplayCountOrNumber = kGetConnectionCount;
    if (noErr == _doStatus(this, cscGetMultiConnect, &multiConnection))
        num = (multiConnection.csDisplayCountOrNumber);

    IONDRVFB_END(getConnectionCount,num,0,0);
    return (num);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IONDRVFramebuffer::createI2C( void )
{
    IONDRVFB_START(createI2C,0,0,0);
    IOReturn                    err;
    VDCommunicationInfoRec      commInfo;
    SInt32                      busID, minBus, maxBus;
    OSArray *                   array;
    OSDictionary *              dict;
    bool                        ok = false;

    array = OSArray::withCapacity(1);
    if (!array)
    {
        IONDRVFB_END(createI2C,kIOReturnNoMemory,0,0);
        return (kIOReturnNoMemory);
    }

    do
    {
        bzero( &commInfo, sizeof( commInfo));
        commInfo.csBusID = kVideoDefaultBus;

        err = _doStatus( this, cscGetCommunicationInfo, &commInfo );
        DEBG(thisName, " cscGetCommunicationInfo(%d): csBusType %x, csMinBus %x, csMaxBus %x\n"
              "csSupportedTypes %x, csSupportedCommFlags %x\n",
              err, (uint32_t) commInfo.csBusType,
              (uint32_t) commInfo.csMinBus, (uint32_t) commInfo.csMaxBus,
              (uint32_t) commInfo.csSupportedTypes, (uint32_t) commInfo.csSupportedCommFlags);

        if (kIOReturnSuccess != err)
            continue;

        minBus = commInfo.csMinBus;
        maxBus = commInfo.csMaxBus;
        if (maxBus < minBus)
            continue;
        for (busID = minBus;
                busID <= maxBus;
                busID++)
        {
            bzero(&commInfo, sizeof(commInfo));
            commInfo.csBusID = busID;
            err = _doStatus(this, cscGetCommunicationInfo, &commInfo);
            if (kIOReturnSuccess != err)
                break;
                
            dict = OSDictionary::withCapacity(4);
            if (!dict)
                break;

            setNumber(dict, kIOI2CInterfaceIDKey,        busID);
            setNumber(dict, kIOI2CBusTypeKey,            commInfo.csBusType);
            setNumber(dict, kIOI2CTransactionTypesKey,   commInfo.csSupportedTypes);
            setNumber(dict, kIOI2CSupportedCommFlagsKey, commInfo.csSupportedCommFlags);
            array->setObject(dict);
            dict->release();
        }

        ok = (busID > maxBus);
    }
    while (false);

    if (ok)
        setProperty(kIOFBI2CInterfaceInfoKey, array);

    array->release();

    IONDRVFB_END(createI2C,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

IOReturn IONDRVFramebuffer::doI2CRequest( UInt32 bus, IOI2CBusTiming * timing, IOI2CRequest * request )
{
    IONDRVFB_START(doI2CRequest,bus,0,0);
    IOReturn            err;
    VDCommunicationRec  comm;

    bzero( &comm, sizeof( comm));

    do
    {
        comm.csBusID            = bus;
        comm.csCommFlags        = request->commFlags;

        comm.csMinReplyDelay    = static_cast<UInt32>(request->minReplyDelay * 1000);    // ms -> us
        if (comm.csMinReplyDelay)
            comm.csCommFlags    |= kVideoReplyMicroSecDelayMask;

        if (kIOI2CUseSubAddressCommFlag & request->commFlags)
            comm.csSendAddress  = (request->sendAddress << 8) | request->sendSubAddress;
        else
            comm.csSendAddress  = request->sendAddress;

        comm.csSendType         = request->sendTransactionType;
        comm.csSendBuffer       = (LogicalAddress) request->sendBuffer;
        comm.csSendSize         = request->sendBytes;

        if (kIOI2CUseSubAddressCommFlag & request->commFlags)
            comm.csReplyAddress = (request->replyAddress << 8) | request->replySubAddress;
        else
            comm.csReplyAddress = request->replyAddress;

        comm.csReplyType        = request->replyTransactionType;
        comm.csReplyBuffer      = (LogicalAddress) request->replyBuffer;
        comm.csReplySize        = request->replyBytes;

        err = _doControl(this, cscDoCommunication, &comm);
    }
    while (false);

    switch (err)
    {
        case kVideoI2CReplyPendingErr:
            err = kIOReturnNoCompletion;
            break;
        case kVideoI2CTransactionErr:
            err = kIOReturnNoDevice;
            break;
        case kVideoI2CBusyErr:
            err = kIOReturnBusy;
            break;
        case kVideoI2CTransactionTypeErr:
            err = kIOReturnUnsupportedMode;
            break;
        case kVideoBufferSizeErr:
            err = kIOReturnOverrun;
            break;
    }

    request->result = err;
    if (request->completion)
        (*request->completion)(request);

    err = kIOReturnSuccess;

    IONDRVFB_END(doI2CRequest,err,0,0);
    return (err);
}

/*
    File:       DDCPowerOnOffUtils.c <CS3>
*/

enum{
    kVCPSendSize                        = 8,
    kVCPReplySize                       = 64,
    kI2CDisplayWriteAddress             = 0x6E,
    kI2CDisplayReadAddress              = 0x6F,
    // Messed up specification says checksum should be calculated with ACCESS.bus value of 50.
    kI2CDisplayReadHostCheckSumAddress  = 0x50,
    // Messed up specification says checksum should be calculated with ACCESS.bus value of 50.
    kI2CDisplayReadHostAddress          = 0x51,

    kI2CVCPGetCode                      = 0x01,
    kI2CVCPGetLength                    = 0x82,
    kI2CVCPGetMessageSize               = 0x05,

    kI2CVCPReplyLength                  = 0x88,
    kI2CNullReplyLength                 = 0x80,
    kI2CNullReplyCheckSum               = 0xBE,

    kI2CVCPSetCode                      = 0x03,
    kI2CVCPSetLength                    = 0x84,
    kI2CVCPReplyCode                    = 0x02,

    kDDCPowerOn                         = 0x01,
    kDDCPowerOff                        = 0x04
};
enum {
    kBasicI2CCommTransactionsMask = ( (1<<kVideoNoTransactionType) | (1<<kVideoSimpleI2CType)
                                      | (1<<kVideoDDCciReplyType) )
};

void IONDRVFramebuffer::displayI2CPower( bool enable )
{
    IONDRVFB_START(displayI2CPower,enable,0,0);
    VDCommunicationRec  i2CRecord;
    VDCommunicationInfoRec i2cInfoRecord;
    Byte                sendBuffer[8];
    Byte                replyBuffer[kVCPReplySize];
    UInt32              supportedCommFlags = 0;
    // Don't do it if we're told it's not supported
    bool                setThisDisplay = true;

    if (enable == __private->i2cPowerState)
    {
        IONDRVFB_END(displayI2CPower,0,__LINE__,0);
        return;
    }

    __private->i2cPowerState = enable;

    //
    // Some displays (like Fiji) do not support the reading
    // of the current power state.  Others (like Mitsubishi
    // Diamond Pro 710) report that they do not support
    // power management calls.
    //
    // I'll avoid sending the power command only in the
    // case that I get a valid reply that does says
    // it does not support the power selector.
    //

    bzero( &i2cInfoRecord, sizeof(i2cInfoRecord) );
    if (noErr != _doStatus(this, cscGetCommunicationInfo, &i2cInfoRecord))
    {
        IONDRVFB_END(displayI2CPower,0,__LINE__,0);
        return;
    }
    if (kBasicI2CCommTransactionsMask != (i2cInfoRecord.csSupportedTypes & kBasicI2CCommTransactionsMask))
    {
        IONDRVFB_END(displayI2CPower,0,__LINE__,0);
        return;
    }

    supportedCommFlags = i2cInfoRecord.csSupportedCommFlags;
    bzero( &i2CRecord, sizeof(i2CRecord) );
    bzero( &sendBuffer, sizeof(sendBuffer) );
    bzero( &replyBuffer, sizeof(replyBuffer) );

    sendBuffer[0]       = kI2CDisplayReadHostAddress;
    sendBuffer[1]       = kI2CVCPGetLength;
    sendBuffer[2]       = kI2CVCPGetCode;               // GetVCP command
    sendBuffer[3]       = 0xD6;
    sendBuffer[4]       = kI2CDisplayWriteAddress ^
                    sendBuffer[0] ^ sendBuffer[1] ^
                    sendBuffer[2] ^ sendBuffer[3];

    i2CRecord.csBusID           = kVideoDefaultBus;
    i2CRecord.csSendType        = kVideoSimpleI2CType;
    i2CRecord.csSendAddress     = kI2CDisplayWriteAddress;
    i2CRecord.csSendBuffer      = &sendBuffer;
    i2CRecord.csSendSize        = 7;
    i2CRecord.csReplyType       = kVideoDDCciReplyType;
    i2CRecord.csReplyAddress    = kI2CDisplayReadAddress;
    i2CRecord.csReplyBuffer     = &replyBuffer;
    i2CRecord.csReplySize       = kVCPReplySize;

    if (supportedCommFlags & kVideoReplyMicroSecDelayMask)
    {
        // We know some displays are slow, this is an important call to get right
        i2CRecord.csCommFlags   |= kVideoReplyMicroSecDelayMask;
        // 50 milliseconds should be enough time for the display to respond.
        i2CRecord.csMinReplyDelay = 50 * 1000;
    }

    if ((noErr == _doControl(this, cscDoCommunication, &i2CRecord))
            && (kI2CDisplayWriteAddress == replyBuffer[0])
            && (kI2CVCPReplyLength == replyBuffer[1])
            && (kI2CVCPReplyCode == replyBuffer[2]))
    {
        Byte checkSum = kI2CDisplayReadHostCheckSumAddress ^    // host address
                        replyBuffer[0] ^        // source address
                        replyBuffer[1] ^        // message length (0x88)
                        replyBuffer[2] ^        // VCP type code
                        replyBuffer[3] ^        // result code
                        replyBuffer[4] ^        // VCP op code
                        replyBuffer[5] ^        // VCP type code
                        replyBuffer[6] ^        // Max value MSB
                        replyBuffer[7] ^        // Max value LSB
                        replyBuffer[8] ^        // Current value MSB
                        replyBuffer[9]; // Current value LSB

        if ((checkSum == replyBuffer[10]) &&            // Did the check sum match AND
                (0 != replyBuffer[3]))          // Are we not supposed to support this feature?
            setThisDisplay = false;                     // Don't do it if we're told it's not supported
    }

    if (setThisDisplay)
    {
        bzero( &i2CRecord, sizeof(i2CRecord) );
        bzero( &sendBuffer, sizeof(sendBuffer) );
        bzero( &replyBuffer, sizeof(replyBuffer) );

        sendBuffer[0]   = kI2CDisplayReadHostAddress;
        sendBuffer[1]   = kI2CVCPSetLength;
        sendBuffer[2]   = kI2CVCPSetCode;                       // SetVCP command
        sendBuffer[3]   = 0xD6;
        sendBuffer[4]   = 0;                                    // MSB
        sendBuffer[5]   = enable ? kDDCPowerOn : kDDCPowerOff;  // LSB
        sendBuffer[6]   = kI2CDisplayWriteAddress ^
                        sendBuffer[0] ^ sendBuffer[1] ^
                        sendBuffer[2] ^ sendBuffer[3] ^
                        sendBuffer[4] ^ sendBuffer[5];

        i2CRecord.csBusID               = kVideoDefaultBus;
        i2CRecord.csSendAddress         = kI2CDisplayWriteAddress;
        i2CRecord.csSendType            = kVideoSimpleI2CType;
        i2CRecord.csSendBuffer          = &sendBuffer;
        i2CRecord.csSendSize            = 7;
        i2CRecord.csReplyType           = kVideoNoTransactionType;
        i2CRecord.csReplyAddress        = 0;
        i2CRecord.csReplyBuffer         = 0;
        i2CRecord.csReplySize           = 0;

        if (supportedCommFlags & kVideoReplyMicroSecDelayMask)
        {
            // We know some displays are slow, this is an important call to get right
            i2CRecord.csCommFlags |= kVideoReplyMicroSecDelayMask;
            // 50 milliseconds should be enough time for the display to respond.
            i2CRecord.csMinReplyDelay   = 50 * 1000;
        }

        _doControl(this, cscDoCommunication, &i2CRecord);
    }

    IONDRVFB_END(displayI2CPower,0,0,0);
}

bool IONDRVFramebuffer::getOnlineState( void )
{
    IONDRVFB_START(getOnlineState,0,0,0);
    IONDRVFB_END(getOnlineState,online,0,0);
    return (online);
}

IOReturn IONDRVFramebuffer::ndrvGetSetFeature( UInt32 feature,
                                               uintptr_t newValue, uintptr_t * currentValue )
{
    IONDRVFB_START(ndrvGetSetFeature,feature,newValue,0);
#define CHAR(c)    ((c) ? (char) (c) : '0')
#define FEAT(f)    CHAR(f>>24), CHAR(f>>16), CHAR(f>>8), CHAR(f>>0)

    IOReturn err;
    VDConfigurationRec configRec;

    bzero( &configRec, sizeof( configRec));
    configRec.csConfigFeature = feature;

    DEBG(thisName, "(0x%08x '%c%c%c%c')\n", (int) feature, FEAT(feature));

    TIMESTART();
    err = _doStatus( this, cscGetFeatureConfiguration, &configRec );
    TIMEEND(thisName, "cscGetFeatureConfiguration: '%c%c%c%c' %qd ms\n", FEAT(feature));

    DEBG1(thisName, " cscGetFeatureConfiguration(%d), %08x %08lx %08lx %08lx\n", err,
         (uint32_t) configRec.csConfigSupport, configRec.csConfigValue, configRec.csReserved1, configRec.csReserved2);

    if ((kIOReturnSuccess != err) || !configRec.csConfigSupport)
        err = kIOReturnUnsupported;

    if (kIOReturnSuccess == err)
    {
        if (currentValue)
        {
            currentValue[0] = configRec.csConfigValue;
            currentValue[1] = configRec.csReserved1;
            currentValue[2] = configRec.csReserved2;
        }
        else if (configRec.csConfigValue != newValue)
        {
            configRec.csConfigFeature = feature;
            configRec.csConfigValue   = newValue;
            TIMESTART();
            err = _doControl( this, cscSetFeatureConfiguration, &configRec );
            TIMEEND(thisName, "cscSetFeatureConfiguration: '%c%c%c%c' = 0x%lx %qd ms\n",
                    FEAT(feature), newValue);

            DEBG1(thisName, " cscSetFeatureConfiguration(%d) %08lx\n", err, configRec.csConfigValue);
        }
        else
            DEBG1(thisName, " skipped cscSetFeatureConfiguration(%d) %08lx\n", err, configRec.csConfigValue);
    }

    IONDRVFB_END(ndrvGetSetFeature,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::setConnectionFlags( void )
{
    IONDRVFB_START(setConnectionFlags,0,0,0);
    if (kIODVIPowerEnableFlag & __private->displayConnectFlags)
        ndrvGetSetFeature( kDVIPowerSwitchFeature, kDVIPowerSwitchActiveMask, NULL );

    shouldDoI2CPower |= (0 != ( kIOI2CPowerEnableFlag & __private->displayConnectFlags));

    if (shouldDoI2CPower)
        displayI2CPower( (powerState == kIONDRVFramebufferPowerStateMax) );

    IONDRVFB_END(setConnectionFlags,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

IOReturn IONDRVFramebuffer::setAttributeForConnection( IOIndex connectIndex,
        IOSelect attribute, uintptr_t value )
{
    IONDRVFB_START(setAttributeForConnection,connectIndex,attribute,value);
    IOReturn            err;
    VDSyncInfoRec       syncInfo;

    // -- temp
    if (ndrvState == 0)
    {
        IONDRVFB_END(setAttributeForConnection,kIOReturnNotOpen,0,0);
        return (kIOReturnNotOpen);
    }
    if (!getControllerWorkLoop()->inGate())
    {
        if ((attribute != kConnectionProbe) && (attribute != 0x696772))
        {
            OSReportWithBacktrace("%s::setAttributeForConnection(0x%x) not gated\n", 
                                    thisName, (int) attribute);
        }
        err = (super::setAttributeForConnectionExt(connectIndex, attribute, value));
        IONDRVFB_END(setAttributeForConnection,err,__LINE__,0);
        return (err);
    }
    // --

    switch (attribute)
    {
        case kConnectionPower:
            err = kIOReturnSuccess;
            break;

        case kConnectionSyncEnable:
            syncInfo.csMode = (UInt8) (value >> 8);
            syncInfo.csFlags = (UInt8) (value & 0xFF);
            _doControl( this, cscSetSync, &syncInfo);
            err = kIOReturnSuccess;
            break;

        case kConnectionFlags:
            __private->displayConnectFlags |= (UInt32) value;
            fNub->setProperty(kIONDRVDisplayConnectFlagsKey,
                &__private->displayConnectFlags, sizeof32(__private->displayConnectFlags));
            err = setConnectionFlags();
            break;

        case kConnectionProbe:
            DEBG1(thisName, " kConnectionProbe\n");
            err = _probeAction(this, (IOOptionBits) value);
            break;

        default:

            err = super::setAttributeForConnection( connectIndex,
                                                    attribute, value );
            if (kIOReturnUnsupported == err)
                err = ndrvGetSetFeature(attribute, value, NULL);
           break;
    }

    IONDRVFB_END(setAttributeForConnection,err,0,0);
    return (err);
}

bool IONDRVFramebuffer::searchOfflineMode( IODisplayModeID * offlineMode )
{
    IONDRVFB_START(searchOfflineMode,0,0,0);
    VDResolutionInfoRec info;
    VDTimingInfoRec     timingInfo;
    bool                ret = false;

    info.csPreviousDisplayModeID = kDisplayModeIDFindFirstResolution;
    while (
        (noErr == _doStatus(this, cscGetNextResolution, &info))
        && ((SInt32) info.csDisplayModeID > 0))
    {
        timingInfo.csTimingMode   = info.csDisplayModeID;
        timingInfo.csTimingFormat = kDeclROMtables;
        ret = _doStatus( this, cscGetModeTiming, &timingInfo);

        if ((noErr == ret)
                && (kDeclROMtables == timingInfo.csTimingFormat)
                && (timingApple_0x0_0hz_Offline == timingInfo.csTimingData)
                && (kDisplayModeSafeFlag & timingInfo.csTimingFlags))
        {
            if (offlineMode)
                *offlineMode = info.csDisplayModeID;
            ret = true;
            break;
        }
        info.csPreviousDisplayModeID = info.csDisplayModeID;
    }

    IONDRVFB_END(searchOfflineMode,ret,0,0);
    return (ret);
}

IOReturn IONDRVFramebuffer::processConnectChange( uintptr_t * value )
{
    IONDRVFB_START(processConnectChange,0,0,0);
    IOReturn  ret;
    uintptr_t connectEnabled;

    DEBG(thisName, "\n");

    ret = getAttributeForConnection( 0, kConnectionCheckEnable, &connectEnabled );

    setDetailedTimings( 0 );
    removeProperty( kIOFBConfigKey );
    __private->displayConnectFlags     = 0;
    __private->i2cPowerState           = 0;
    shouldDoI2CPower                   = 0;
    cachedVDResolution.csDisplayModeID = kDisplayModeIDInvalid;
    __private->depthMapModeID          = kDisplayModeIDInvalid;

    setInfoProperties();
    if (mirrored)
        setMirror( 0 );

    IODisplayModeID offlineMode;
    if (!online
            && searchOfflineMode(&offlineMode)
            && (offlineMode != currentDisplayMode))
    {
        super::resetLimitState();
        super::sendLimitState(__LINE__);

        setDisplayMode( offlineMode, currentDepth );
    }

    __private->ackConnectChange = false;

    ret = kIOReturnSuccess;

    IONDRVFB_END(processConnectChange,ret,0,0);
    return (ret);
}

IOReturn IONDRVFramebuffer::getAttributeForConnection( IOIndex connectIndex,
        IOSelect attribute, uintptr_t * value )
{
    IONDRVFB_START(getAttributeForConnection,connectIndex,attribute,0);
    IOReturn                      ret, thisRet;
    UInt32                        thisCount = 0;
    VDSyncInfoRec                 syncInfo;
    VDConfigurationFeatureListRec featureList;

    switch (attribute)
    {
        case kConnectionSyncFlags:
            // find out current state of sync lines
            syncInfo.csMode = 0x00;
            _doStatus( this, cscGetSync, &syncInfo);
            *value = syncInfo.csMode;
            ret = kIOReturnSuccess;
            break;

        case kConnectionSyncEnable:
            // what are the sync-controlling capabilities of the ndrv?
            syncInfo.csMode = 0xFF;
            _doStatus( this, cscGetSync, &syncInfo);
            *value = (UInt32) syncInfo.csMode;
            ret = kIOReturnSuccess;
            break;

        case kConnectionSupportsHLDDCSense:
        case kConnectionSupportsAppleSense:
            ret = kIOReturnSuccess;
            break;

        case kConnectionFlags:
            VDMultiConnectInfoRec multiConnect;
            if (connectIndex == 0)
                ret = _doStatus( this, cscGetConnection, &multiConnect.csConnectInfo);
            else
            {
                multiConnect.csDisplayCountOrNumber = connectIndex;
                ret = _doStatus( this, cscGetMultiConnect, &multiConnect);
            }
            if (__private->removable)
                multiConnect.csConnectInfo.csConnectFlags &= ~(1<<kBuiltInConnection);

            if (kIOReturnSuccess == ret)
                *value = multiConnect.csConnectInfo.csConnectFlags;
            else
                *value = 0;
            break;

        case kConnectionCheckEnable:

            if (connectIndex == 0)
                ret = _doStatus( this, cscGetConnection, &multiConnect.csConnectInfo);
            else
            {
                multiConnect.csDisplayCountOrNumber = connectIndex;
                ret = _doStatus( this, cscGetMultiConnect, &multiConnect);
            }
            if ((kIOReturnSuccess == ret)
                    && ((1 << kConnectionInactive) & multiConnect.csConnectInfo.csConnectFlags))
            {
                online = false;
                *value = online;
                break;
            }
            online = !searchOfflineMode( 0 );
            /* fall thru */

        case kConnectionEnable:

            *value = online;
            ret = kIOReturnSuccess;
            break;

        case kConnectionPostWake:
            ret = kIOReturnSuccess;
            break;

        case kConnectionChanged:
            if (value)
                ret = processConnectChange(value);
            else if (__private->postWakeProbe)
            {
                DEBG1(thisName, " kConnectionPostWake\n");
                ret = _doControl( this, cscProbeConnection, 0 );
                __private->postWakeProbe = false;
                if (!__private->ackConnectChange)
                    setConnectionFlags();
            } else
                ret = kIOReturnSuccess;
            break;

        case kConnectionInTVMode:

            switch (__private->currentModeTiming)
            {
                case timingAppleNTSC_ST:
                case timingAppleNTSC_FF:
                case timingAppleNTSC_STconv:
                case timingAppleNTSC_FFconv:
                    *value = kConnectionNTSCMode;
                    break;
                case timingApplePAL_ST:
                case timingApplePAL_FF:
                case timingApplePAL_STconv:
                case timingApplePAL_FFconv:
                    *value = kConnectionPALMode;
                    break;

                default:
                    *value = kConnectionNonTVMode;
                    break;
            }
            ret = kIOReturnSuccess;
            break;

        case kConnectionDisplayParameterCount:

            bzero(&featureList, sizeof(featureList));
            TIMESTART();
            thisRet = _doStatus(this, cscGetFeatureList, &featureList);
            TIMEEND(thisName, "cscGetFeatureList: %qd ms\n");
            if (kIOReturnSuccess == thisRet)
                thisCount = featureList.csNumConfigFeatures;

            ret = super::getAttributeForConnection( connectIndex, attribute, value );
            if (kIOReturnSuccess != ret)
            {
                *value = thisCount;
                ret = thisRet;
            }
            else
                *value += thisCount;
            break;

        case kConnectionDisplayParameters:

            bzero(&featureList, sizeof(featureList));
            featureList.csConfigFeatureList = (OSType *) value;
            featureList.csNumConfigFeatures = 0x7fff;
            thisRet = _doStatus(this, cscGetFeatureList, &featureList);
            if (kIOReturnSuccess == thisRet)
            {
                thisCount = featureList.csNumConfigFeatures;
                for (uint32_t idx = thisCount; idx > 0; idx--)
                    value[idx - 1] = ((OSType *)value)[idx - 1];
            }

            ret = super::getAttributeForConnection( connectIndex,
                                                    attribute, value + thisCount );
            if (kIOReturnSuccess != ret)
                ret = thisRet;
            break;

        default:

            ret = super::getAttributeForConnection( connectIndex,
                                                    attribute, value );
            if (kIOReturnUnsupported == ret)
                ret = ndrvGetSetFeature(attribute, 0, value);
            break;
    }

    IONDRVFB_END(getAttributeForConnection,ret,0,0);
    return (ret);
}

IOReturn IONDRVFramebuffer::getAppleSense( IOIndex  connectIndex,
        UInt32 * senseType,
        UInt32 * primary,
        UInt32 * extended,
        UInt32 * displayType )
{
    IONDRVFB_START(getAppleSense,connectIndex,0,0);
    OSStatus            err;
    VDMultiConnectInfoRec       multiConnect;
    UInt32                      sense, extSense;

    if (connectIndex == 0)
        err = _doStatus( this, cscGetConnection, &multiConnect.csConnectInfo);
    else
    {
        multiConnect.csDisplayCountOrNumber = connectIndex;
        err = _doStatus( this, cscGetMultiConnect, &multiConnect);
    }
    if (err)
    {
        IONDRVFB_END(getAppleSense,err,__LINE__,0);
        return (err);
    }

    if ((primary || extended)
            && (0 == ((1<<kReportsTagging) & multiConnect.csConnectInfo.csConnectFlags)))

        err = kIOReturnUnsupported;

    else
    {
        sense           = multiConnect.csConnectInfo.csConnectTaggedType;
        extSense        = multiConnect.csConnectInfo.csConnectTaggedData;
        // bug fixes for really old ATI driver
        if (sense == 0)
        {
            if (extSense == 6)
            {
                sense           = kRSCSix;
                extSense        = kESCSixStandard;
            }
            else if (extSense == 4)
            {
                sense           = kRSCFour;
                extSense        = kESCFourNTSC;
            }
        }
        if (primary)
            *primary = sense;
        if (extended)
            *extended = extSense;
        if (senseType)
            *senseType = (0 != ((1<<kTaggingInfoNonStandard) & multiConnect.csConnectInfo.csConnectFlags));
        if (displayType)
            *displayType = multiConnect.csConnectInfo.csDisplayType;
    }

    IONDRVFB_END(getAppleSense,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::connectFlags( IOIndex /* connectIndex */,
        IODisplayModeID displayMode, IOOptionBits * flags )
{
    IONDRVFB_START(connectFlags,displayMode,0,0);
    VDTimingInfoRec             timingInfo;
    OSStatus                    err;

    timingInfo.csTimingMode = displayMode;
    // in case the driver doesn't do it:
    timingInfo.csTimingFormat = kDeclROMtables;
    err = _doStatus( this, cscGetModeTiming, &timingInfo);

    if (kDetailedTimingFormat == timingInfo.csTimingFormat)
        *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
    else
        *flags = timingInfo.csTimingFlags;

    IONDRVFB_END(connectFlags,err,0,0);
    return (err);
}


bool IONDRVFramebuffer::hasDDCConnect( IOIndex  connectIndex )
{
    IONDRVFB_START(hasDDCConnect,connectIndex,0,0);
    OSStatus              err;
    VDMultiConnectInfoRec multiConnect;
    enum {                kNeedFlags = (1<<kReportsDDCConnection) | (1<<kHasDDCConnection) };

    if (connectIndex == 0)
        err = _doStatus( this, cscGetConnection, &multiConnect.csConnectInfo);
    else
    {
        multiConnect.csDisplayCountOrNumber = connectIndex;
        err = _doStatus( this, cscGetMultiConnect, &multiConnect);
    }
    if (err)
    {
        IONDRVFB_END(hasDDCConnect,err,__LINE__,0);
        return (false);
    }

    bool b = (forceReadEDID
              || (kNeedFlags == (multiConnect.csConnectInfo.csConnectFlags & kNeedFlags)));
    IONDRVFB_END(hasDDCConnect,b,0,0);
    return (b);
}

// I2C first year for Apple displays.
// Apple monitors older than this (and Manta)
// are never called with I2C commands
enum {
    kFirstAppleI2CYear  = 1999,
    kAppleVESAVendorID  = 0x0610
};

struct EDID
{
    UInt8       header[8];
    UInt8       vendorProduct[4];
    UInt8       serialNumber[4];
    UInt8       weekOfManufacture;
    UInt8       yearOfManufacture;
    UInt8       version;
    UInt8       revision;
    UInt8       displayParams[5];
    UInt8       colorCharacteristics[10];
    UInt8       establishedTimings[3];
    UInt16      standardTimings[8];
    UInt8       descriptors[4][18];
    UInt8       extension;
    UInt8       checksum;
};

static bool IsApplePowerBlock(UInt8 * theBlock)
{
    return (theBlock &&
            0x00000000          == *(UInt32 *)&theBlock[0] &&
            0x00                == theBlock[4] &&
            0x06                == theBlock[5] &&
            0x10                == theBlock[6]);
}

IOReturn IONDRVFramebuffer::getDDCBlock( IOIndex /* connectIndex */,
        UInt32 blockNumber,
        IOSelect blockType,
        IOOptionBits options,
        UInt8 * data, IOByteCount * length )

{
    IONDRVFB_START(getDDCBlock,blockNumber,blockType,options);
    OSStatus            err = 0;
    VDDDCBlockRec       ddcRec;
    IOByteCount         actualLength = *length;

    if (forceReadEDID)
    {
        forceReadEDID = 0;
        options |= kDDCForceReadMask;
    }

    ddcRec.ddcBlockNumber       = blockNumber;
    ddcRec.ddcBlockType         = blockType;
    ddcRec.ddcFlags             = options;

    err = _doStatus( this, cscGetDDCBlock, &ddcRec);

    if (err == noErr)
    {
        if (actualLength > kDDCBlockSize)
            actualLength = kDDCBlockSize;
        bcopy( ddcRec.ddcBlockData, data, actualLength);
        *length = actualLength;

        if ((1 == blockNumber) && (kIODDCBlockTypeEDID == blockType)
                && (actualLength >= sizeof(EDID)))
            do
            {
                EDID * edid;
                UInt32 vendor;
                UInt32 product;

                edid = (EDID *) data;
                vendor = (edid->vendorProduct[0] << 8) | edid->vendorProduct[1];
                product = (edid->vendorProduct[3] << 8) | edid->vendorProduct[2];
                if (kAppleVESAVendorID == vendor)
                {
                    if ((0x01F4 == product) || (0x9D02 == product) || (0x9216 == product))
                        continue;
                    if (edid->yearOfManufacture && ((edid->yearOfManufacture + 1990) < kFirstAppleI2CYear))
                        continue;
                }
                shouldDoI2CPower =    (IsApplePowerBlock( &edid->descriptors[1][0])
                                       || IsApplePowerBlock( &edid->descriptors[2][0])
                                       || IsApplePowerBlock( &edid->descriptors[3][0]));

                if (shouldDoI2CPower)
                    __private->displayConnectFlags |= kIODVIPowerEnableFlag;

                err = kIOReturnSuccess;
            }
            while (false);
    }

    if (1 == blockNumber)
        DEBG(thisName, " i2cPower %d\n", shouldDoI2CPower);

    IONDRVFB_END(getDDCBlock,err,0,0);
    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// initForPM
//
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void IONDRVFramebuffer::initForPM( void )
{
    IONDRVFB_START(initForPM,0,0,0);
    IOPMPowerState powerStates[ kIONDRVFramebufferPowerStateCount ] =
    {
        // version,
        // capabilityFlags, outputPowerCharacter, inputPowerRequirement,
        { 1, 0,              0,           0,           0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, 0,                0,           IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
        // staticPower, unbudgetedPower, powerToAttain, timeToAttain, settleUpTime,
        // timeToLower, settleDownTime, powerDomainBudget
    };
    VDPowerStateRec sleepInfo;
    IOReturn err;
    bool dozeOnly;

    dozeOnly = false;
    {
        sleepInfo.powerState = 0;
        sleepInfo.powerFlags = 0;
        sleepInfo.powerReserved1 = 0;
        sleepInfo.powerReserved2 = 0;
        // can this ndrv power off?
        err = _doStatus( this, cscGetPowerState, &sleepInfo);
        dozeOnly = ((kIOReturnSuccess != err)
                    || (0 == (kPowerStateSleepCanPowerOffMask & sleepInfo.powerFlags)));
    }

    if (OSDynamicCast(IOBootNDRV, fNdrv))
        dozeOnly = true;

    if (dozeOnly)
    {
        powerStates[kNDRVFramebufferSleepState].capabilityFlags |= kIOPMPreventSystemSleep;
        powerStates[kNDRVFramebufferDozeState].capabilityFlags  |= kIOPMPreventSystemSleep;
        powerStates[kNDRVFramebufferWakeState].capabilityFlags  |= kIOPMPreventSystemSleep;
    }

    // register ourselves with superclass policy-maker
    registerPowerDriver( this, powerStates, kIONDRVFramebufferPowerStateCount );
    // no sleep until children
    temporaryPowerClampOn();
    // not below doze until system sleep
    changePowerStateTo( kNDRVFramebufferDozeState );

    IONDRVFB_END(initForPM,0,0,0);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// maxCapabilityForDomainState
//
// This simple device needs only power.  If the power domain is supplying
// power, the frame buffer can be on.  If there is no power it can only be off.
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

unsigned long IONDRVFramebuffer::maxCapabilityForDomainState(
    IOPMPowerFlags domainState )
{
    IONDRVFB_START(maxCapabilityForDomainState,domainState,0,0);

    unsigned long   value;
    if (domainState & IOPMPowerOn)
        value = (kIONDRVFramebufferPowerStateMax);
    else
        value = (kNDRVFramebufferSleepState);

    IONDRVFB_END(maxCapabilityForDomainState,value,0,0);
    return (value);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// initialPowerStateForDomainState
//
// The power domain may be changing state.  If power is on in the new
// state, that will not affect our state at all.  If domain power is off,
// we can attain only our lowest state, which is off.
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

unsigned long IONDRVFramebuffer::initialPowerStateForDomainState(
    IOPMPowerFlags domainState )
{
    IONDRVFB_START(initialPowerStateForDomainState,domainState,0,0);

    unsigned long   value;
    if (domainState & IOPMPowerOn)
        value = (kIONDRVFramebufferPowerStateMax);
    else
        value = (kNDRVFramebufferSleepState);

    IONDRVFB_END(initialPowerStateForDomainState,value,0,0);
    return (value);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// powerStateForDomainState
//
// The power domain may be changing state.  If power is on in the new
// state, that will not affect our state at all.  If domain power is off,
// we can attain only our lowest state, which is off.
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

unsigned long IONDRVFramebuffer::powerStateForDomainState(
    IOPMPowerFlags domainState )
{
    IONDRVFB_START(powerStateForDomainState,domainState,0,0);

    unsigned long   value;
    if (domainState & IOPMPowerOn)
        value = (getPowerState());
    else
        value = (kNDRVFramebufferSleepState);

    IONDRVFB_END(powerStateForDomainState,value,0,0);
    return (value);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IONDRVFramebuffer::ndrvSetDisplayPowerState( UInt32 state )
{
    IONDRVFB_START(ndrvSetDisplayPowerState,state,0,0);
    IOReturn            err;
    VDSyncInfoRec       syncInfo;

    state = state ? true : false;

    // what are the sync-controlling capabilities of the ndrv?
    syncInfo.csMode = 0xff;
    err = _doStatus( this, cscGetSync, &syncInfo );
    if (kIOReturnSuccess == err)
    {
        // pick new sync state
        if (state)
            syncInfo.csMode = kDPMSSyncOn;
        else
            syncInfo.csMode = kDPMSSyncOff;
        syncInfo.csFlags = kDPMSSyncMask;
        _doControl( this, cscSetSync, &syncInfo);

        DEBG(thisName, " sync->%02x\n", syncInfo.csMode);
    }

    if (shouldDoI2CPower)
    {
        displayI2CPower( state );
        DEBG(thisName, " i2c->%02x\n", state ? true : false);
    }

    err = kIOReturnSuccess;

    IONDRVFB_END(ndrvSetDisplayPowerState,err,0,0);
    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// ndrvSetPowerState
//
// Called by the superclass to turn the frame buffer on and off.
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOReturn IONDRVFramebuffer::ndrvUpdatePowerState( void )
{
    IONDRVFB_START(ndrvUpdatePowerState,0,0,0);
    IOReturn            err;
    VDPowerStateRec     sleepInfo;
    bool                supportsReducedPower;

    super::handleEvent( kIOFBNotifyWillChangeSpeed, (void *)(uintptr_t) __private->reducedSpeed );

    sleepInfo.powerState = 0;
    sleepInfo.powerFlags = 0;
    sleepInfo.powerReserved1 = 0;
    sleepInfo.powerReserved2 = 0;

    err = _doStatus( this, cscGetPowerState, &sleepInfo);

    supportsReducedPower = (kIOReturnSuccess == err) && (0 != 
                (( kPowerStateSupportsReducedPower1BitMask
                | kPowerStateSupportsReducedPower2BitMask
                | kPowerStateSupportsReducedPower3BitMask) & sleepInfo.powerFlags));

    if (supportsReducedPower)
    {
        sleepInfo.powerFlags = (__private->reducedSpeed << 8);
        sleepInfo.powerReserved1 = kPowerStateSleepWakeNeedsProbeMask;
        sleepInfo.powerReserved2 = 0;
    
        err = _doControl(this, cscSetPowerState, &sleepInfo);
    }
    else
        err = kIOReturnUnsupported;

    super::handleEvent( kIOFBNotifyDidChangeSpeed, (void *)(uintptr_t) __private->reducedSpeed );

    IONDRVFB_END(ndrvUpdatePowerState,err,0,0);
    return (err);
}

IOReturn IONDRVFramebuffer::ndrvSetPowerState( UInt32 newState )
{
    IONDRVFB_START(ndrvSetPowerState,newState,0,0);
    static const UInt32
    // [sleep][fromState][toState]
    states[2][kIONDRVFramebufferPowerStateCount][kIONDRVFramebufferPowerStateCount] =
    {
        {
            { 0, kAVPowerOff, kAVPowerOn },
            { kAVPowerOff, 0, kAVPowerOn },
            { kAVPowerOff, kAVPowerOff, 0 },
        }, {
            { 0, kHardwareWakeToDoze, kHardwareWake },
            { kHardwareSleep, 0, kAVPowerOn },
            { kHardwareSleep, kAVPowerSuspend, 0 }
        }
    };

    IOReturn            err;
    UInt32              sleep = 0;
    UInt32              ndrvPowerState;
    UInt32              oldState;
    IOIndex             postEvent = 0;
    IOAGPDevice *       agpDev;
    IODTPlatformExpert * pe;
    bool                supportsReducedPower;

    if (newState == powerState)
    {
        IONDRVFB_END(ndrvSetPowerState,kIOReturnSuccess,__LINE__,0);
        return (kIOReturnSuccess);
    }

    if (newState > kIONDRVFramebufferPowerStateMax)
        newState = kIONDRVFramebufferPowerStateMax;

    oldState = powerState;

    if (kIONDRVFramebufferPowerStateMax == oldState)
    {
        super::handleEvent( kIOFBNotifyWillPowerOff );
        postEvent = kIOFBNotifyDidPowerOff;
        ndrvSetDisplayPowerState( false );
    }
    else if (kIONDRVFramebufferPowerStateMax == newState)
    {
        super::handleEvent( kIOFBNotifyWillPowerOn );
        postEvent = kIOFBNotifyDidPowerOn;
    }

    if (kNDRVFramebufferSleepState == newState)
    {
        if (kIODVIPowerEnableFlag & __private->displayConnectFlags)
        {
            err = ndrvGetSetFeature( kDVIPowerSwitchFeature, 0, 0 );
            if (kIOReturnSuccess == err)
                IOSleep( kDVIPowerSwitchPowerOffDelay );
        }

        IOMemoryDescriptor * vram;
        if ((vram = getVRAMRange()))
        {
            vram->redirect( kernel_task, true );
            vram->release();
        }
        // tell accelerators to protect HW also
        super::handleEvent( kIOFBNotifyWillSleep, (void *) true );
    }

    if (platformSleep
            && !__private->removable
            && (pe = OSDynamicCast(IODTPlatformExpert, getPlatform()))
            && (pe->getChipSetType() < kChipSetTypeCore99))
    {
        VDSleepWakeInfo sleepInfo;

        ndrvPowerState = newState ? vdWakeState : vdSleepState;

        err = _doStatus( this, cscSleepWake, &sleepInfo);

        powerState = newState;

        if ((kIOReturnSuccess == err) && (sleepWakeSig == sleepInfo.csData)
                && (ndrvPowerState != sleepInfo.csMode))
        {
            sleepInfo.csMode = ndrvPowerState;

            err = _doControl( this, cscSleepWake, &sleepInfo);
        }
    }
    else
    {
        VDPowerStateRec sleepInfo;

        sleepInfo.powerState = 0;
        sleepInfo.powerFlags = 0;
        sleepInfo.powerReserved1 = 0;
        sleepInfo.powerReserved2 = 0;

        err = _doStatus( this, cscGetPowerState, &sleepInfo);

        if (((kIOReturnSuccess == err) && (kPowerStateSleepCanPowerOffMask & sleepInfo.powerFlags))
          || (platformSleep && !__private->removable))
            sleep = 1;

        supportsReducedPower = (kIOReturnSuccess == err) && (0 != 
                    (( kPowerStateSupportsReducedPower1BitMask
                    | kPowerStateSupportsReducedPower2BitMask
                    | kPowerStateSupportsReducedPower3BitMask) & sleepInfo.powerFlags));

        ndrvPowerState = states[sleep][oldState][newState];

        if ((kHardwareWakeToDoze == ndrvPowerState)
                && (0 == (kPowerStateSleepWaketoDozeMask & sleepInfo.powerFlags)))
        {
        	DEBG1(thisName, " no kHardwareWakeToDoze support\n");
            ndrvPowerState = kHardwareWake;
		}
        else if (kAVPowerSuspend == ndrvPowerState)
        {
        }


        DEBG1(thisName, " idx %d powerFlags %08x, state->%02x\n",
             (uint32_t) newState, (uint32_t) sleepInfo.powerFlags, (uint32_t) ndrvPowerState);

        powerState = newState;

        if ((kIOReturnSuccess != err) || (sleepInfo.powerState != ndrvPowerState))
        {
            sleepInfo.powerState = ndrvPowerState;

            if (supportsReducedPower)
                sleepInfo.powerFlags = (__private->reducedSpeed << 8);
            else
                sleepInfo.powerFlags = 0;
            sleepInfo.powerReserved1 = kPowerStateSleepWakeNeedsProbeMask;
            sleepInfo.powerReserved2 = 0;

            err = _doControl( this, cscSetPowerState, &sleepInfo);

            DEBG(thisName, " done powerFlags %08x\n", (uint32_t) sleepInfo.powerFlags);

            if (kNDRVFramebufferSleepState == oldState)
                __private->postWakeProbe = (0 != (kPowerStateSleepWakeNeedsProbeMask & sleepInfo.powerFlags));
            else if (kNDRVFramebufferSleepState == newState)
                __private->postWakeProbe = false;
        }
    }

    agpDev = OSDynamicCast(IOAGPDevice, fDevice);

    if (kNDRVFramebufferSleepState == oldState)
    {
        uintptr_t isOnline, wasOnline = online;
        if (__private->postWakeProbe)
            isOnline = wasOnline;
        else if (kIOReturnSuccess != getAttributeForConnection(0, kConnectionCheckEnable, &isOnline))
            isOnline = true;
        if (isOnline != wasOnline)
        {
            online = static_cast<unsigned int>(isOnline);
            // vramMemory = findVRAM();
            if (isOnline)
                getCurrentConfiguration();
        }
        if (agpDev)
            agpDev->resetAGP();
    }

    if (kNDRVFramebufferSleepState == oldState)
    {
        IOMemoryDescriptor * vram;
        if ((vram = getVRAMRange()))
        {
            vram->redirect( kernel_task, false );
            vram->release();
        }
        // tell accelerators to unprotect HW also
        super::handleEvent( kIOFBNotifyDidWake, (void *) true );
    }

    if (postEvent)
    {
        super::handleEvent( postEvent );
        if (kIOFBNotifyDidPowerOn == postEvent)
        {
            ndrvSetDisplayPowerState( true );
        }
    }

    IONDRVFramebuffer * next;
    next = OSDynamicCast(IONDRVFramebuffer, nextDependent);
    if (true && next
            && ((newState > oldState)))
//                 /*|| ((newState == kNDRVFramebufferDozeState)&& !other->getOnlineState()*/)))
	{
		while (next && (next != this))
		{
		    next->ndrvSetPowerState( newState );
			next = OSDynamicCast(IONDRVFramebuffer, next->nextDependent);
		}
	}

    if ((kNDRVFramebufferSleepState == oldState)
      && !__private->ackConnectChange
      && !__private->postWakeProbe)
        setConnectionFlags();

    IONDRVFB_END(ndrvSetPowerState,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IONDRV
OSDefineMetaClassAndStructors(IOBootNDRV, IONDRV)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool IOBootNDRV::getUInt32Property( IORegistryEntry * regEntry, const char * name,
                                    UInt32 * result )
{
    OSData * data;

    data = OSDynamicCast(OSData, regEntry->getProperty(name));
    if (data)
        *result = *((UInt32 *) data->getBytesNoCopy());

    return (data != 0);
}

IONDRV * IOBootNDRV::fromRegistryEntry( IORegistryEntry * regEntry )
{
    IOBootNDRV *  inst;
    IOBootNDRV *  result = NULL;
    IOService *   device;
    IOByteCount   bytes;

    inst = new IOBootNDRV;
    if (inst && !inst->init())
    {
        inst->release();
        inst = NULL;
    }

    if (inst && (device = OSDynamicCast(IOService, regEntry)))
    do
    {
        PE_Video        bootDisplay;
        UInt32          bpp;

        IOService::getPlatform()->getConsoleInfo( &bootDisplay);

		IOPhysicalAddress64 consolePhysAddress = bootDisplay.v_baseAddr & ~3;
#ifndef __LP64__
		consolePhysAddress |= (((uint64_t) bootDisplay.v_baseAddrHigh) << 32);
#endif

        IOMemoryDescriptor * mem = NULL;
        IOPCIDevice *        pciDevice;
        UInt32               numMaps, i;

        OSNumber * num = OSDynamicCast(OSNumber, device->getProperty(kIOFBDependentIndexKey));
        if (num && (0 != num->unsigned32BitValue()))
            break;
        else if ((pciDevice = OSDynamicCast(IOPCIDevice, device))
                    && pciDevice->getFunctionNumber())
            break;

        bytes = (bootDisplay.v_rowBytes * bootDisplay.v_height);
        numMaps = device->getDeviceMemoryCount();
        for (i = 0; (!mem) && (i < numMaps); i++)
        {
            addr64_t pa;
            mem = device->getDeviceMemoryWithIndex(i);
            if (!mem)
                continue;
            pa = mem->getPhysicalSegment(0, 0, kIOMemoryMapperNone);
            if ((consolePhysAddress < pa) || (consolePhysAddress >= (pa + mem->getLength())))
                mem = NULL;
            else
            {
                mem = IOSubMemoryDescriptor::withSubRange(mem,
                                            consolePhysAddress - pa,
                                            bytes, kIODirectionNone);
            }
        }

        if (mem)
        {
            inst->fVRAMDesc = mem;
            inst->fRowBytes = (UInt32) bootDisplay.v_rowBytes;
            inst->fWidth    = (UInt32) bootDisplay.v_width;
            inst->fHeight   = (UInt32) bootDisplay.v_height;
            bpp = (UInt32) bootDisplay.v_depth;
            if (bpp == 15)
                bpp = 16;
            else if (bpp == 24)
                bpp = 32;
            inst->fBitsPerPixel = bpp;
    
            result = inst;
        }
    }
    while (false);

    if (inst && !result)
        inst->release();

    return (result);
}

void IOBootNDRV::free( void )
{
    super::free();
}

IOReturn IOBootNDRV::getSymbol( const char * symbolName,
                                IOLogicalAddress * address )
{
    return (kIOReturnUnsupported);
}

const char * IOBootNDRV::driverName( void )
{
    return (".Display_boot");
}

IOReturn IOBootNDRV::doDriverIO( UInt32 commandID, void * contents,
                                 UInt32 commandCode, UInt32 commandKind )
{
    IONDRVControlParameters * pb = (IONDRVControlParameters *) contents;
    IOReturn    ret;

    switch (commandCode)
    {
        case kIONDRVInitializeCommand:
        case kIONDRVOpenCommand:
            ret = kIOReturnSuccess;
            break;

        case kIONDRVControlCommand:
            ret = doControl( pb->code, pb->params );
            break;
        case kIONDRVStatusCommand:
            ret = doStatus( pb->code, pb->params );
            break;

        default:
            ret = kIOReturnUnsupported;
            break;
    }

    return (ret);
}

IOReturn IOBootNDRV::doControl( UInt32 code, void * params )
{
    IOReturn            ret;

    switch (code)
    {
        case cscSetEntries:
        case cscSetGamma:
            ret = kIOReturnSuccess;
            break;

        default:
            ret = kIOReturnUnsupported;
            break;
    }

    return (ret);
}

IOReturn IOBootNDRV::doStatus( UInt32 code, void * params )
{
    IOReturn            ret;

    switch (code)
    {
        case cscGetCurMode:
            {
                VDSwitchInfoRec * switchInfo = (VDSwitchInfoRec *) params;

                switchInfo->csData     = kIOBootNDRVDisplayMode;
                switchInfo->csMode     = kDepthMode1;
                switchInfo->csPage     = 1;
                switchInfo->csBaseAddr = (char *)(1 | fVRAMDesc->getPhysicalSegment(0, NULL, kIOMemoryMapperNone));
                ret = kIOReturnSuccess;
            }
            break;

        case cscGetNextResolution:
            {
                VDResolutionInfoRec * resInfo = (VDResolutionInfoRec *) params;

                if ((kDisplayModeIDFindFirstResolution == (SInt32) resInfo->csPreviousDisplayModeID)
                        || (kDisplayModeIDCurrent == (SInt32) resInfo->csPreviousDisplayModeID))
                {
                    resInfo->csDisplayModeID    = kIOBootNDRVDisplayMode;
                    resInfo->csMaxDepthMode             = kDepthMode1;
                    resInfo->csHorizontalPixels = fWidth;
                    resInfo->csVerticalLines    = fHeight;
                    resInfo->csRefreshRate              = 0 << 16;
                    ret = kIOReturnSuccess;
                }
                else if (kIOBootNDRVDisplayMode == resInfo->csPreviousDisplayModeID)
                {
                    resInfo->csDisplayModeID = kDisplayModeIDNoMoreResolutions;
                    ret = kIOReturnSuccess;
                }
                else
                {
                    resInfo->csDisplayModeID = kDisplayModeIDInvalid;
                    ret = kIOReturnBadArgument;
                }
            }
            break;

        case cscGetVideoParameters:
            {
                VDVideoParametersInfoRec * pixelParams = (VDVideoParametersInfoRec *) params;

                if ((kIOBootNDRVDisplayMode != pixelParams->csDisplayModeID)
                        || (kDepthMode1 != pixelParams->csDepthMode))
                {
                    ret = kIOReturnBadArgument;
                    break;
                }
                VPBlock *       pixelInfo = pixelParams->csVPBlockPtr;

                bzero(pixelInfo, sizeof(VPBlock));
                pixelInfo->vpBounds.right  = fWidth;
                pixelInfo->vpBounds.bottom = fHeight;
                pixelInfo->vpRowBytes      = fRowBytes;
                pixelInfo->vpPixelSize     = fBitsPerPixel;
                pixelInfo->vpPixelType     = kIORGBDirectPixels;
                pixelInfo->vpCmpCount      = 3;
                pixelInfo->vpCmpSize       = (fBitsPerPixel <= 16) ? 5 : 8;
                ret = kIOReturnSuccess;
            }
            break;

        case cscGetModeTiming:
            {
                VDTimingInfoRec * timingInfo = (VDTimingInfoRec *) params;

                if (kIOBootNDRVDisplayMode != timingInfo->csTimingMode)
                {
                    ret = kIOReturnBadArgument;
                    break;
                }
                timingInfo->csTimingFormat = kDeclROMtables;
                timingInfo->csTimingFlags  = kDisplayModeValidFlag | kDisplayModeSafeFlag;
                ret = kIOReturnSuccess;
            }
            break;

        default:
            ret = kIOReturnUnsupported;
            break;
    }

    return (ret);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSMetaClassDefineReservedUsed(IONDRVFramebuffer, 0);

OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 1);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 2);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 3);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 4);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 5);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 6);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 7);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 8);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 9);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 10);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 11);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 12);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 13);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 14);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 15);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 16);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 17);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 18);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 19);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 20);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 21);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 22);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 23);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 24);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 25);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 26);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 27);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 28);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 29);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 30);
OSMetaClassDefineReservedUnused(IONDRVFramebuffer, 31);
