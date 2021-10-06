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


#define IOFRAMEBUFFER_PRIVATE

#include <libkern/OSAtomic.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOHibernatePrivate.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/assert.h>

#include <IOKit/graphics/IODisplay.h>
#include <IOKit/graphics/IOGraphicsPrivate.h>

#include "IOGraphicsKTrace.h"
#include "GMetric.hpp"

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

const OSSymbol * gIODisplayParametersKey;
const OSSymbol * gIODisplayGUIDKey;

const OSSymbol * gIODisplayValueKey;
const OSSymbol * gIODisplayMinValueKey;
const OSSymbol * gIODisplayMaxValueKey;

const OSSymbol * gIODisplayBrightnessProbeKey;
const OSSymbol * gIODisplayLinearBrightnessProbeKey;
const OSSymbol * gIODisplayContrastKey;
const OSSymbol * gIODisplayBrightnessKey;
const OSSymbol * gIODisplayLinearBrightnessKey;
const OSSymbol * gIODisplayUsableLinearBrightnessKey;
const OSSymbol * gIODisplayBrightnessFadeKey;
const OSSymbol * gIODisplayHorizontalPositionKey;
const OSSymbol * gIODisplayHorizontalSizeKey;
const OSSymbol * gIODisplayVerticalPositionKey;
const OSSymbol * gIODisplayVerticalSizeKey;
const OSSymbol * gIODisplayTrapezoidKey;
const OSSymbol * gIODisplayPincushionKey;
const OSSymbol * gIODisplayParallelogramKey;
const OSSymbol * gIODisplayRotationKey;
const OSSymbol * gIODisplayOverscanKey;
const OSSymbol * gIODisplayVideoBestKey;
const OSSymbol * gIODisplaySelectedColorModeKey;

const OSSymbol * gIODisplayRedGammaScaleKey;
const OSSymbol * gIODisplayGreenGammaScaleKey;
const OSSymbol * gIODisplayBlueGammaScaleKey;
const OSSymbol * gIODisplayGammaScaleKey;

const OSSymbol * gIODisplayParametersTheatreModeKey;
const OSSymbol * gIODisplayParametersTheatreModeWindowKey;

const OSSymbol * gIODisplayMCCSVersionKey;
const OSSymbol * gIODisplayTechnologyTypeKey;
const OSSymbol * gIODisplayUsageTimeKey;
const OSSymbol * gIODisplayFirmwareLevelKey;

const OSSymbol * gIODisplaySpeakerVolumeKey;
const OSSymbol * gIODisplaySpeakerSelectKey;
const OSSymbol * gIODisplayMicrophoneVolumeKey;
const OSSymbol * gIODisplayAmbientLightSensorKey;
const OSSymbol * gIODisplayAudioMuteAndScreenBlankKey;
const OSSymbol * gIODisplayAudioTrebleKey;
const OSSymbol * gIODisplayAudioBassKey;
const OSSymbol * gIODisplayAudioBalanceLRKey;
const OSSymbol * gIODisplayAudioProcessorModeKey;
const OSSymbol * gIODisplayPowerModeKey;
const OSSymbol * gIODisplayManufacturerSpecificKey;

const OSSymbol * gIODisplayPowerStateKey;
const OSSymbol * gIODisplayControllerIDKey;
const OSSymbol * gIODisplayCapabilityStringKey;

const OSSymbol * gIODisplayParametersCommitKey;
const OSSymbol * gIODisplayParametersDefaultKey;
const OSSymbol * gIODisplayParametersFlushKey;

const OSSymbol * gIODisplayFadeTime1Key;
const OSSymbol * gIODisplayFadeTime2Key;
const OSSymbol * gIODisplayFadeTime3Key;
const OSSymbol * gIODisplayFadeStyleKey;

static const OSSymbol * gIODisplayFastBootEDIDKey;
static IODTPlatformExpert * gIODisplayFastBootPlatform;
static OSData *  gIODisplayZeroData;

enum {
    kIODisplayMaxUsableState  = kIODisplayMaxPowerState - 1
};

enum 
{
    kIODisplayBlankValue   = 0x100,
    kIODisplayUnblankValue = 0x200,
};

#define RECORD_METRIC(func) \
    GMETRICFUNC(func, DBG_FUNC_NONE, \
            kGMETRICS_DOMAIN_IODISPLAY | kGMETRICS_DOMAIN_POWER)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOService

OSDefineMetaClassAndAbstractStructorsWithInit( IODisplay, IOService, IODisplay::initialize() )

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

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
    UInt8       detailedTimings[72];
    UInt8       extension;
    UInt8       checksum;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void IODisplay::initialize( void )
{
    IOD_START(initialize,0,0,0);
    gIODisplayParametersKey     = OSSymbol::withCStringNoCopy(
                                  kIODisplayParametersKey );
    gIODisplayGUIDKey           = OSSymbol::withCStringNoCopy(
                                        kIODisplayGUIDKey );
    gIODisplayValueKey          = OSSymbol::withCStringNoCopy(
                                        kIODisplayValueKey );
    gIODisplayMinValueKey       = OSSymbol::withCStringNoCopy(
                                        kIODisplayMinValueKey );
    gIODisplayMaxValueKey       = OSSymbol::withCStringNoCopy(
                                        kIODisplayMaxValueKey );
    gIODisplayContrastKey       = OSSymbol::withCStringNoCopy(
                                        kIODisplayContrastKey );
    gIODisplayBrightnessKey     = OSSymbol::withCStringNoCopy(
                                        kIODisplayBrightnessKey );
    gIODisplayLinearBrightnessKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayLinearBrightnessKey );
    gIODisplayUsableLinearBrightnessKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayUsableLinearBrightnessKey );
    gIODisplayBrightnessFadeKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayBrightnessFadeKey );
    gIODisplayHorizontalPositionKey = OSSymbol::withCStringNoCopy(
                                          kIODisplayHorizontalPositionKey );
    gIODisplayHorizontalSizeKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayHorizontalSizeKey );
    gIODisplayVerticalPositionKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayVerticalPositionKey );
    gIODisplayVerticalSizeKey   = OSSymbol::withCStringNoCopy(
                                        kIODisplayVerticalSizeKey );
    gIODisplayTrapezoidKey      = OSSymbol::withCStringNoCopy(
                                        kIODisplayTrapezoidKey );
    gIODisplayPincushionKey     = OSSymbol::withCStringNoCopy(
                                        kIODisplayPincushionKey );
    gIODisplayParallelogramKey  = OSSymbol::withCStringNoCopy(
                                        kIODisplayParallelogramKey );
    gIODisplayRotationKey       = OSSymbol::withCStringNoCopy(
                                        kIODisplayRotationKey );

    gIODisplayOverscanKey       = OSSymbol::withCStringNoCopy(
                                        kIODisplayOverscanKey );
    gIODisplayVideoBestKey      = OSSymbol::withCStringNoCopy(
                                        kIODisplayVideoBestKey );
    gIODisplaySelectedColorModeKey = OSSymbol::withCStringNoCopy(
                                        kIODisplaySelectedColorModeKey );
    gIODisplayRedGammaScaleKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayRedGammaScaleKey );
    gIODisplayGreenGammaScaleKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayGreenGammaScaleKey );
    gIODisplayBlueGammaScaleKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayBlueGammaScaleKey );
    gIODisplayGammaScaleKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayGammaScaleKey );

    gIODisplayParametersCommitKey = OSSymbol::withCStringNoCopy(
                                        kIODisplayParametersCommitKey );
    gIODisplayParametersDefaultKey = OSSymbol::withCStringNoCopy(
                                         kIODisplayParametersDefaultKey );
    gIODisplayParametersFlushKey = OSSymbol::withCStringNoCopy(
                                         kIODisplayParametersFlushKey );

    gIODisplayParametersTheatreModeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayTheatreModeKey);
    gIODisplayParametersTheatreModeWindowKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayTheatreModeWindowKey);

    gIODisplayMCCSVersionKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayMCCSVersionKey);
    gIODisplayTechnologyTypeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayTechnologyTypeKey);
    gIODisplayUsageTimeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayUsageTimeKey);
    gIODisplayFirmwareLevelKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayFirmwareLevelKey);
    gIODisplaySpeakerVolumeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplaySpeakerVolumeKey);
    gIODisplaySpeakerSelectKey = OSSymbol::withCStringNoCopy(
                                            kIODisplaySpeakerSelectKey);
    gIODisplayMicrophoneVolumeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayMicrophoneVolumeKey);
    gIODisplayAmbientLightSensorKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayAmbientLightSensorKey);
    gIODisplayAudioMuteAndScreenBlankKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayAudioMuteAndScreenBlankKey);
    gIODisplayAudioTrebleKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayAudioTrebleKey);
    gIODisplayAudioBassKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayAudioBassKey);
    gIODisplayAudioBalanceLRKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayAudioBalanceLRKey);
    gIODisplayAudioProcessorModeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayAudioProcessorModeKey);
    gIODisplayPowerModeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayPowerModeKey);
    gIODisplayManufacturerSpecificKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayManufacturerSpecificKey);

    gIODisplayPowerStateKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayPowerStateKey);

	gIODisplayControllerIDKey = OSSymbol::withCStringNoCopy(
											kIODisplayControllerIDKey);
	gIODisplayCapabilityStringKey = OSSymbol::withCStringNoCopy(
											kIODisplayCapabilityStringKey);
    gIODisplayBrightnessProbeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayBrightnessProbeKey);
    gIODisplayLinearBrightnessProbeKey = OSSymbol::withCStringNoCopy(
                                            kIODisplayLinearBrightnessProbeKey);

	gIODisplayFadeTime1Key = OSSymbol::withCStringNoCopy("fade-time1");
	gIODisplayFadeTime2Key = OSSymbol::withCStringNoCopy("fade-time2");
	gIODisplayFadeTime3Key = OSSymbol::withCStringNoCopy("fade-time3");
	gIODisplayFadeStyleKey = OSSymbol::withCStringNoCopy("fade-style");

    IORegistryEntry * entry;
    if ((entry = getServiceRoot())
     && (0 != entry->getProperty("has-safe-sleep")))
    {
        gIODisplayFastBootPlatform = OSDynamicCast(IODTPlatformExpert, IOService::getPlatform());
        gIODisplayFastBootEDIDKey  = OSSymbol::withCStringNoCopy( kIODisplayFastBootEDIDKey );
        gIODisplayZeroData         = OSData::withCapacity(0);
    }
    IOD_END(initialize,0,0,0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

IOService * IODisplay::probe(   IOService *     provider,
                              SInt32 *  score )
{
    IOD_START(probe,0,0,0);
    fConnection = OSDynamicCast(IODisplayConnect, provider);

    IOD_END(probe,0,0,0);
    return (this);
}

IODisplayConnect * IODisplay::getConnection( void )
{
    IOD_START(getConnection,0,0,0);
    IOD_END(getConnection,0,0,0);
    return (fConnection);
}


IOReturn IODisplay::getGammaTableByIndex(
    UInt32 * /* channelCount */, UInt32 * /* dataCount */,
    UInt32 * /* dataWidth */, void ** /* data */ )
{
    IOD_START(getGammaTableByIndex,0,0,0);
    IOD_END(getGammaTableByIndex,kIOReturnUnsupported,0,0);
    return (kIOReturnUnsupported);
}

void IODisplayUpdateNVRAM( IOService * entry, OSData * property );
void IODisplayUpdateNVRAM( IOService * entry, OSData * property )
{
    if (true && gIODisplayFastBootPlatform)
    {
        while (entry && !entry->inPlane(gIODTPlane))
        {
            entry = entry->getProvider();
        }
        if (entry)
        {
            gIODisplayFastBootPlatform->writeNVRAMProperty(entry, gIODisplayFastBootEDIDKey, 
                                                            property);
        }
    }
}

void IODisplay::searchParameterHandlers(IORegistryEntry * entry)
{
    IOD_START(searchParameterHandlers,0,0,0);
    IORegistryIterator *        iter;
    IODisplayParameterHandler * parameterHandler;

    iter = IORegistryIterator::iterateOver(entry, gIOServicePlane,
                                           kIORegistryIterateRecursively);
    if (!iter)
    {
        IOD_END(searchParameterHandlers,-1,0,0);
        return;
    }
	do
	{
		iter->reset();
		while( (entry = iter->getNextObject()))
		{
			if (!(parameterHandler = OSDynamicCast(IODisplayParameterHandler, entry)))
				continue;
			addParameterHandler(parameterHandler);
		}
	} 
	while (!iter->isValid());
	iter->release();
    IOD_END(searchParameterHandlers,0,0,0);
}

bool IODisplay::start( IOService * provider )
{
    IOD_START(start,0,0,0);
    IOFramebuffer *     framebuffer;
    uintptr_t           connectFlags;
    OSData *            edidData;
    EDID *              edid;
    uint32_t            vendor = 0;
    uint32_t            product = 0;
    uint32_t            serial = 0;

    if (!super::start(provider))
    {
        IOD_END(start,false,0,0);
        return (false);
    }

    if (!fConnection)
    {
        // as yet unmatched display device (ADB)
        IOD_END(start,true,__LINE__,0);
        return (true);
    }

    framebuffer = fConnection->getFramebuffer();
    assert( framebuffer );

    fWSAADeferState = kIOWSAA_DeferEnd;

    FB_START(getAttributeForConnection,kConnectionFlags,__LINE__,0);
    fConnection->getAttributeForConnection(kConnectionFlags, &connectFlags);
    FB_END(getAttributeForConnection,0,__LINE__,connectFlags);
    uint32_t flagsData = (uint32_t) connectFlags;
    setProperty(kIODisplayConnectFlagsKey, &flagsData, sizeof(flagsData));

    edidData = OSDynamicCast( OSData, getProperty( kIODisplayEDIDKey ));
    if (!edidData)
    {
        readFramebufferEDID();
        edidData = OSDynamicCast( OSData, getProperty( kIODisplayEDIDKey ));
    }

    if (edidData)
    {
        do
        {
            edid = (EDID *) edidData->getBytesNoCopy();
            DEBG(framebuffer->thisName, " EDID v%d.%d\n", edid->version, edid->revision );

            if (edid->version != 1)
                continue;
            // vendor
            vendor = (edid->vendorProduct[0] << 8) | edid->vendorProduct[1];

#if 0
			if (true && (0x10ac == vendor))
			{
				vendor = 0;
				edidData = 0;
				edid = 0;
				removeProperty(kIODisplayEDIDKey);
				break;
			}
#endif

            // product
            product = (edid->vendorProduct[3] << 8) | edid->vendorProduct[2];

			serial = (edid->serialNumber[3] << 24)
				   | (edid->serialNumber[2] << 16)
				   | (edid->serialNumber[1] << 8)
				   | (edid->serialNumber[0]);
			if (serial == 0x01010101) serial = 0;

            DEBG(framebuffer->thisName, " vendor/product/serial 0x%02x/0x%02x/0x%x\n", 
											vendor, product, serial );
        }
        while (false);
    }

    IODisplayUpdateNVRAM(this, edidData);

    do
    {
        UInt32  sense, extSense;
        UInt32  senseType, displayType;

        FB_START(getAttributeForConnection,kConnectionSupportsAppleSense,__LINE__,0);
        IOReturn err = fConnection->getAttributeForConnection( kConnectionSupportsAppleSense, NULL);
        FB_END(getAttributeForConnection,err,__LINE__,0);
        if (kIOReturnSuccess != err)
            continue;

        FB_START(getAppleSense,0,__LINE__,0);
        IOReturn error = framebuffer->getAppleSense(fConnection->getConnection(),
                                                  &senseType, &sense, &extSense, &displayType);
        FB_END(getAppleSense,error,__LINE__,0);
        if (kIOReturnSuccess != error)
            continue;

        setProperty( kAppleDisplayTypeKey, displayType, 32);
        setProperty( kAppleSenseKey, ((sense & 0xff) << 8) | (extSense & 0xff), 32);

        if (0 == vendor)
        {
            vendor = kDisplayVendorIDUnknown;
            if (0 == senseType)
                product = ((sense & 0xff) << 8) | (extSense & 0xff);
            else
                product = (displayType & 0xff) << 16;
        }
    }
    while (false);

    if (0 == vendor)
    {
        vendor = kDisplayVendorIDUnknown;
        product = kDisplayProductIDGeneric;
    }

    if (0 == getProperty(kDisplayVendorID))
        setProperty( kDisplayVendorID, vendor, 32);
    if (0 == getProperty(kDisplayProductID))
        setProperty( kDisplayProductID, product, 32);
    if (0 == getProperty(kDisplaySerialNumber))
        setProperty( kDisplaySerialNumber, serial, 32);

    enum
    {
        kMaxKeyLen = 1024,
        kMaxKeyVendorProduct = 20 /* "-12345678-12345678" */
    };
    int pathLen = kMaxKeyLen - kMaxKeyVendorProduct;
    char * prefsKey = IONew(char, kMaxKeyLen);

    if (prefsKey)
    {
        bool ok = false;
        OSObject * obj;
        OSData * data;
        if ((obj = copyProperty("AAPL,display-alias", gIOServicePlane)))
        {
            ok = (data = OSDynamicCast(OSData, obj));
            if (ok)
                pathLen = snprintf(prefsKey, kMaxKeyLen, "Alias:%d/%s",
                                ((uint32_t *) data->getBytesNoCopy())[0], getName());
            obj->release();
        }
        if (!ok)
            ok = getPath(prefsKey, &pathLen, gIOServicePlane);
        if (ok)
        {
            snprintf(prefsKey + pathLen, kMaxKeyLen - pathLen, "-%x-%x", (int) vendor, (int) product);
            const OSSymbol * sym = OSSymbol::withCString(prefsKey);
            if (sym)
            {
                setProperty(kIODisplayPrefKeyKey, (OSObject *) sym);
                sym->release();
            }
        }
        IODelete(prefsKey, char, kMaxKeyLen);
    }

    OSNumber * num;
    if ((num = OSDynamicCast(OSNumber, framebuffer->getProperty(kIOFBTransformKey))))
    {
        if ((kIOScaleSwapAxes | kIOFBSwapAxes) & num->unsigned32BitValue())
            setName("AppleDisplay-Portrait");
    }

    // display parameter hooks

    IODisplayParameterHandler * parameterHandler;

	searchParameterHandlers(framebuffer);

    if (OSDynamicCast(IOBacklightDisplay, this))
    {
        OSDictionary * matching = nameMatching("backlight");
        OSIterator *   iter = NULL;
        IOService  *   look;
        if (matching)
        {
            iter = getMatchingServices(matching);
            matching->release();
        }
        if (iter)
        {
            look = OSDynamicCast(IOService, iter->getNextObject());
            if (look) searchParameterHandlers(look);
            iter->release();
        }
    }

    if ((parameterHandler = OSDynamicCast(IODisplayParameterHandler,
                                            framebuffer->getProperty(gIODisplayParametersKey))))
    {
        addParameterHandler(parameterHandler);
    }

    doUpdate();

    // initialize power management of the display

    fDisplayPMVars = IONew(IODisplayPMVars, 1);
    assert( fDisplayPMVars );
    bzero(fDisplayPMVars, sizeof(IODisplayPMVars));

    fDisplayPMVars->maxState = kIODisplayMaxPowerState;
    fDisplayPMVars->currentState = kIODisplayMaxPowerState;

    initPowerManagement( provider );

	uint32_t options = 0;
	if (NULL != OSDynamicCast(IOBacklightDisplay, this))
		options |= kIODisplayOptionBacklight;
	if (fDisplayPMVars->minDimState)
		options |= kIODisplayOptionDimDisable;

    framebuffer->displayOnline(this, +1, options);

    fNotifier = framebuffer->addFramebufferNotificationWithOptions( &IODisplay::_framebufferEvent, this, NULL,
                                                                   kIOFBNotifyGroupID_IODisplay, 0,
                                                                   kIOFBNotifyEvent_SleepWake |
                                                                   kIOFBNotifyEvent_ClamshellChange |
                                                                   kIOFBNotifyEvent_Probed |
                                                                   kIOFBNotifyEvent_DisplayDimsChange |
                                                                   kIOFBNotifyEvent_WSAADefer |
                                                                   kIOFBNotifyEvent_DisplayModeChange );
    registerService();

    IOD_END(start,true,__LINE__,0);
    return (true);
}

bool IODisplay::addParameterHandler( IODisplayParameterHandler * parameterHandler )
{
    IOD_START(addParameterHandler,0,0,0);
    OSArray * array;
    array = OSDynamicCast(OSArray, fParameterHandler);

    if (array && ((unsigned int) -1 != array->getNextIndexOfObject(parameterHandler, 0)))
    {
        IOD_END(addParameterHandler,true,__LINE__,0);
        return (true);
    }

    if (!parameterHandler->setDisplay(this))
    {
        IOD_END(addParameterHandler,false,__LINE__,0);
        return (false);
    }

    if (!array)
    {
        array = OSArray::withCapacity(2);
        if (!array)
        {
            IOD_END(addParameterHandler,false,__LINE__,0);
            return (false);
        }
       fParameterHandler = (IODisplayParameterHandler *) array;
    }

    array->setObject(parameterHandler);

    IOD_END(addParameterHandler,true,__LINE__,0);
    return (true);
}

bool IODisplay::removeParameterHandler( IODisplayParameterHandler * parameterHandler )
{
    IOD_START(removeParameterHandler,0,0,0);
    OSArray * array;

    if (parameterHandler == fParameterHandler)
    {
        fParameterHandler->release();
        fParameterHandler = 0;
        IOD_END(removeParameterHandler,true,__LINE__,0);
        return (true);
    }

    array = OSDynamicCast(OSArray, fParameterHandler);
    if (array)
    {
        unsigned int idx = array->getNextIndexOfObject(parameterHandler, 0);
        if (idx != (unsigned int)-1)
        {
            array->removeObject(idx);
            IOD_END(removeParameterHandler,true,__LINE__,0);
            return (true);
        }
    }
    IOD_END(removeParameterHandler,false,0,0);
    return (false);
}

void IODisplay::stop( IOService * provider )
{
    IOD_START(stop,0,0,0);
    if (fConnection)
    {
        fConnection->getFramebuffer()->displayOnline(this, -1, 0);
        fConnection = 0;
    }

    IODisplayUpdateNVRAM(this, 0);

    if ( initialized )
        PMstop();
    if (fNotifier)
    {
        fNotifier->remove();
        fNotifier = 0;
    }

	removeProperty(gIODisplayParametersKey);
    IOD_END(stop,0,0,0);
}

void IODisplay::free()
{
    IOD_START(free,0,0,0);
    OSSafeReleaseNULL(fParameterHandler);
    if (fDisplayPMVars) {
        IODelete(fDisplayPMVars, IODisplayPMVars, 1);
        fDisplayPMVars = 0;
    }
    super::free();
    IOD_END(free,0,0,0);
}

IOReturn IODisplay::readFramebufferEDID( void )
{
    IOD_START(readFramebufferEDID,0,0,0);
    IOReturn            err;
    IOFramebuffer *     framebuffer;
    OSData *            data;
    IOByteCount         length;
    EDID                readEDID;
    UInt8               edidBlock[128];
    UInt32              index;
    UInt32              numExts;

    assert( fConnection );
    framebuffer = fConnection->getFramebuffer();
    assert( framebuffer );

    do
    {
        FB_START(getAttributeForConnection,kConnectionSupportsHLDDCSense,__LINE__,0);
        err = fConnection->getAttributeForConnection(
                  kConnectionSupportsHLDDCSense, NULL );
        FB_END(getAttributeForConnection,err,__LINE__,0);
        if (err)
            continue;

        FB_START(hasDDCConnect,0,__LINE__,0);
        bool hasDDC = framebuffer->hasDDCConnect(fConnection->getConnection());
        FB_END(hasDDCConnect,hasDDC,__LINE__,0);

        if (!hasDDC)
        {
            err = kIOReturnUnsupported;
            continue;
        }
        length = sizeof( EDID);
        FB_START(getDDCBlock,0,__LINE__,0);
        err = framebuffer->getDDCBlock( fConnection->getConnection(),
                                        1, kIODDCBlockTypeEDID, 0, (UInt8 *) &readEDID, &length );
        FB_END(getDDCBlock,err,__LINE__,0);
        if (err || (length != sizeof(EDID)))
            continue;


        data = OSData::withBytes( &readEDID, sizeof( EDID ));
        if (!data)
            continue;

        numExts = readEDID.extension;
        for (index = 2; index < (2 + numExts); index++)
        {
            length = sizeof(EDID);
            FB_START(getDDCBlock,0,__LINE__,0);
            err = framebuffer->getDDCBlock( fConnection->getConnection(),
                                            index, kIODDCBlockTypeEDID, 0, edidBlock, &length );
            FB_END(getDDCBlock,err,__LINE__,0);
            if (err || (length != sizeof(EDID)))
                break;
            if (0 == bcmp(edidBlock, &readEDID, sizeof(EDID)))
                break;
            if (!data->appendBytes(edidBlock, sizeof(EDID)))
                break;
        }

        setProperty( kIODisplayEDIDKey, data );
        data->release();
    }
    while (false);

    IOD_END(readFramebufferEDID,err,0,0);
    return (err);
}

IOReturn IODisplay::getConnectFlagsForDisplayMode(
    IODisplayModeID mode, UInt32 * flags )
{
    IOD_START(getConnectFlagsForDisplayMode,mode,0,0);
    IOReturn            err;
    IOFramebuffer *     framebuffer;

    assert( fConnection );
    framebuffer = fConnection->getFramebuffer();
    assert( framebuffer );

    FB_START(connectFlags,mode,__LINE__,0);
    err = framebuffer->connectFlags(
              fConnection->getConnection(),
              mode, flags );
    FB_END(connectFlags,err,__LINE__,*flags);

    if (kIOReturnUnsupported == err)
    {
        *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
        err = kIOReturnSuccess;
    }

    IOD_END(getConnectFlagsForDisplayMode,err,0,0);
    return (err);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDictionary * IODisplay::getIntegerRange( OSDictionary * params,
        const OSSymbol * sym, SInt32 * value, SInt32 * min, SInt32 * max )
{
    IOD_START(getIntegerRange,0,0,0);
    OSNumber *          num;

    params = OSDynamicCast( OSDictionary, params->getObject( sym ));

    if (params)
    {
        do
        {
            if (value)
            {
                num = OSDynamicCast( OSNumber, params->getObject(gIODisplayValueKey));
                if (!num)
                    continue;
                *value = num->unsigned32BitValue();
            }
            if (min)
            {
                num = OSDynamicCast( OSNumber, params->getObject(gIODisplayMinValueKey));
                if (!num)
                    continue;
                *min = num->unsigned32BitValue();
            }
            if (max)
            {
                num = OSDynamicCast( OSNumber, params->getObject(gIODisplayMaxValueKey));
                if (!num)
                    continue;
                *max = num->unsigned32BitValue();
            }
            IOD_END(getIntegerRange,0,0,0);
            return (params);
        }
        while (false);
    }

    IOD_END(getIntegerRange,0,__LINE__,0);
    return (NULL);
}

bool IODisplay::setForKey( OSDictionary * params, const OSSymbol * sym,
                           SInt32 value, SInt32 min, SInt32 max )
{
    IOD_START(setForKey,value,min,max);
    SInt32 adjValue;
    bool ok;

    // invert rotation
    if (sym == gIODisplayRotationKey)
        adjValue = max - value + min;
    else
        adjValue = value;

    if ((ok = doIntegerSet(params, sym, adjValue)))
        updateNumber(params, gIODisplayValueKey, value);

    IOD_END(setForKey,ok,0,0);
    return (ok);
}

IOReturn IODisplay::setProperties( OSObject * properties )
{
    IOD_START(setProperties,0,0,0);
    IOReturn                    err = kIOReturnSuccess;
    OSDictionary *              dict;
    OSDictionary *              dict2;
    OSSymbol *                  sym;
    IODisplayParameterHandler * parameterHandler;
    OSArray *                   array;
    OSDictionary *              displayParams;
    OSDictionary *              params;
    OSNumber *                  valueNum;
    OSObject *                  valueObj;
    OSDictionary *              valueDict;
    OSIterator *                iter;
    SInt32                      min, max, value;
    bool                        doCommit = false;
    bool                        allOK = true;
    bool                        ok;

    IOFramebuffer *             framebuffer = NULL;

    if (isInactive())
    {
        IOD_END(setProperties,kIOReturnNotReady,__LINE__,0);
        return (kIOReturnNotReady);
    }

    if (fConnection)
        framebuffer = fConnection->getFramebuffer();
    if (!framebuffer)
    {
        IOD_END(setProperties,kIOReturnNotReady,__LINE__,0);
        return (kIOReturnNotReady);
    }

    framebuffer->fbLock();

    parameterHandler = OSDynamicCast(IODisplayParameterHandler, fParameterHandler);
    if (parameterHandler)
    {
        err = parameterHandler->setProperties( properties );
        if (kIOReturnUnsupported == err)
            err = kIOReturnSuccess;
    }
    else if ((array = OSDynamicCast(OSArray, fParameterHandler)))
    {
        for (unsigned int i = 0;
            (parameterHandler = OSDynamicCast(IODisplayParameterHandler, array->getObject(i)));
            i++)
        {
            err = parameterHandler->setProperties( properties );
            if (kIOReturnUnsupported == err)
                err = kIOReturnSuccess;
        }
    }

    dict = OSDynamicCast(OSDictionary, properties);
    OSObject *paramProp = copyProperty(gIODisplayParametersKey);
    if (!dict || !(displayParams = OSDynamicCast(OSDictionary, paramProp)))
    {
        framebuffer->fbUnlock();
        OSSafeReleaseNULL(paramProp);
        IOD_END(setProperties,kIOReturnUnsupported,0,0);
        return (kIOReturnUnsupported);
    }

    dict2 = OSDynamicCast(OSDictionary, dict->getObject(gIODisplayParametersKey));
    if (dict2)
        dict = dict2;

    if ((properties != displayParams) && dict->getObject(gIODisplayParametersDefaultKey))
    {
        params = OSDynamicCast( OSDictionary,
                                displayParams->getObject(gIODisplayParametersDefaultKey));
        doIntegerSet( params, gIODisplayParametersDefaultKey, 0 );
        doUpdate();
        setProperties( displayParams );
    }

    iter = OSCollectionIterator::withCollection( dict );
    if (iter)
    {
        OSSymbol * doLast = 0;

        for (; ; allOK &= ok)
        {
            sym = (OSSymbol *) iter->getNextObject();
            if (!sym)
            {
                if (doLast)
                {
                    sym = doLast;
                    doLast = 0;
                }
                else
                    break;
            }
            else if (sym == gIODisplayVideoBestKey)
            {
                doLast = sym;
                ok = true;
                continue;
            }

            if (sym == gIODisplayParametersCommitKey)
            {
                if (properties != displayParams)
                    doCommit = true;
                ok = true;
                continue;
            }
            if (sym == gIODisplayParametersDefaultKey)
            {
                ok = true;
                continue;
            }

            OSData * valueData = OSDynamicCast( OSData, dict->getObject(sym) );
            if (valueData)
            {
                ok = doDataSet( sym, valueData );
                continue;
            }

            ok = false;
            if (0 == (params = getIntegerRange(displayParams, sym, 0, &min, &max)))
                continue;

            valueObj = dict->getObject(sym);
            if (!valueObj)
                continue;
            if ((valueDict = OSDynamicCast(OSDictionary, valueObj)))
                valueObj = valueDict->getObject( gIODisplayValueKey );
            valueNum = OSDynamicCast( OSNumber, valueObj );
            if (!valueNum)
                continue;
            value = valueNum->unsigned32BitValue();

            if (value < min) value = min;
            if (value > max) value = max;

			if (kIOGDbgForceBrightness & gIOGDebugFlags)
			{
				if (sym == gIODisplayLinearBrightnessKey) continue;
				if (sym == gIODisplayBrightnessKey)
				{
					value = (((max - min) * 3) / 4 + min);
					updateNumber(params, gIODisplayValueKey, value);
				}
			}

            ok = setForKey( params, sym, value, min, max );
        }
        iter->release();
    }

    if (doCommit)
        doIntegerSet( OSDynamicCast( OSDictionary, displayParams->getObject(gIODisplayParametersCommitKey)),
                      gIODisplayParametersCommitKey, 0 );

    if (kIOWSAA_DeferStart != fWSAADeferState)
        doIntegerSet( OSDynamicCast( OSDictionary, displayParams->getObject(gIODisplayParametersFlushKey)),
                     gIODisplayParametersFlushKey, 0 );

    framebuffer->fbUnlock();

    displayParams->release();

    IOD_END(setProperties,kIOReturnUnsupported,allOK,err);
    return (allOK ? err : kIOReturnError);
}

bool IODisplay::updateNumber( OSDictionary * params, const OSSymbol * key,
                              SInt32 value )
{
    IOD_START(updateNumber,value,0,0);
    OSNumber * num;

    if ((OSCollection::kImmutable & params->setOptions(0, 0)) 
		&& (num = (OSNumber *) params->getObject(key)))
    {
        num->setValue(value);
	}
    else
    {
        num = OSNumber::withNumber( value, 32 );
        if (num)
        {
            params->setObject( key, num );
            num->release();
        }
    }
    IOD_END(updateNumber,num != 0,0,0);
    return (num != 0);
}

bool IODisplay::addParameter( OSDictionary * params, const OSSymbol * paramName,
                              SInt32 min, SInt32 max )
{
    IOD_START(addParameter,min,max,0);
    OSDictionary *      paramDict;
    bool                ok = true;

    paramDict = (OSDictionary *) params->getObject(paramName);
    if (!paramDict)
    {
        paramDict = OSDictionary::withCapacity(3);
        if (!paramDict)
        {
            IOD_END(addParameter,false,__LINE__,0);
            return (false);
        }
        params->setObject(paramName, paramDict);
        paramDict->release();
    }

    paramDict->setCapacityIncrement(1);

    updateNumber(paramDict, gIODisplayMinValueKey, min);
    updateNumber(paramDict, gIODisplayMaxValueKey, max);
    if (!paramDict->getObject(gIODisplayValueKey))
        updateNumber(paramDict, gIODisplayValueKey, min);

    IOD_END(addParameter,ok,__LINE__,0);
    return (ok);
}

bool IODisplay::setParameter( OSDictionary * params, const OSSymbol * paramName,
                              SInt32 value )
{
    IOD_START(setParameter,value,0,0);
    OSDictionary *      paramDict;
    bool                ok = true;

    paramDict = (OSDictionary *) params->getObject(paramName);
    if (!paramDict)
    {
        IOD_END(setParameter,false,__LINE__,0);
        return (false);
    }

    // invert rotation
    if (paramName == gIODisplayRotationKey)
    {
        SInt32 min, max;
        getIntegerRange( params, paramName, NULL, &min, &max );
        value = max - value + min;
    }

    updateNumber( paramDict, gIODisplayValueKey, value );

    IOD_END(setParameter,ok,__LINE__,0);
    return (ok);
}

IOReturn IODisplay::_framebufferEvent( OSObject * osobj, void * ref,
                                       IOFramebuffer * framebuffer, IOIndex event, void * info )
{
    IOD_START(_framebufferEvent,event,0,0);
    IOReturn    err;

    IODisplay * iod = (IODisplay *) osobj;
    err = iod->framebufferEvent(framebuffer , event, info);

    IOD_END(_framebufferEvent,err,0,0);
    return (err);
}

IOReturn IODisplay::framebufferEvent( IOFramebuffer * framebuffer,
                                      IOIndex event, void * info )
{
    IOD_START(framebufferEvent,event,0,0);
    IOReturn       err;
    OSDictionary * displayParams;
    OSObject     * paramProp;

    switch (event)
    {
        case kIOFBNotifyWSAAWillEnterDefer:
            fWSAADeferState = kIOWSAA_DeferStart;
            err = kIOReturnSuccess;
            break;
        case kIOFBNotifyWSAADidExitDefer:
            fWSAADeferState = kIOWSAA_DeferEnd;
            err = kIOReturnSuccess;
            break;
        case kIOFBNotifyDisplayModeDidChange:

            paramProp = copyProperty(gIODisplayParametersKey);
            displayParams = OSDynamicCast(OSDictionary, paramProp);
            if (doUpdate() && displayParams && (kIOWSAA_DeferStart != fWSAADeferState))
                setProperties(displayParams);
            OSSafeReleaseNULL(paramProp);
            /* fall thru */

        default:
            err = kIOReturnSuccess;
            break;
    }

    IOD_END(framebufferEvent,err,0,0);
    return (err);
}

UInt32 gIODisplayFadeTime1;
UInt32 gIODisplayFadeTime2;
UInt32 gIODisplayFadeTime3;
UInt32 gIODisplayFadeStyle;

bool IODisplay::doIntegerSet( OSDictionary * params,
                              const OSSymbol * paramName, UInt32 value )
{
    IOD_START(doIntegerSet,value,0,0);
    IODisplayParameterHandler * parameterHandler;
    OSArray *                   array;
    bool                        ok = false;

    if (gIODisplayFadeTime1Key == paramName)
    {
    	gIODisplayFadeTime1 = value;
        IOD_END(doIntegerSet,true,__LINE__,0);
        return (true);
    }
    if (gIODisplayFadeTime2Key == paramName)
    {
    	gIODisplayFadeTime2 = value;
        IOD_END(doIntegerSet,true,__LINE__,0);
        return (true);
    }
    if (gIODisplayFadeTime3Key == paramName)
    {
    	gIODisplayFadeTime3 = value;
        IOD_END(doIntegerSet,true,__LINE__,0);
        return (true);
    }
    if (gIODisplayFadeStyleKey == paramName)
    {
    	gIODisplayFadeStyle = value;
        IOD_END(doIntegerSet,true,__LINE__,0);
        return (true);
    }

    parameterHandler = OSDynamicCast(IODisplayParameterHandler, fParameterHandler);

    if (parameterHandler)
        ok = parameterHandler->doIntegerSet(params, paramName, value);

    else if ((array = OSDynamicCast(OSArray, fParameterHandler)))
    {
        for (unsigned int i = 0;
            !ok && (parameterHandler = OSDynamicCast(IODisplayParameterHandler, array->getObject(i)));
            i++)
        {
            ok = parameterHandler->doIntegerSet(params, paramName, value);
        }
    }

    IOD_END(doIntegerSet,ok,__LINE__,0);
    return (ok);
}

bool IODisplay::doDataSet( const OSSymbol * paramName, OSData * value )
{
    IOD_START(doDataSet,0,0,0);
    IODisplayParameterHandler * parameterHandler;
    OSArray *                   array;
    bool                        ok = false;

    parameterHandler = OSDynamicCast(IODisplayParameterHandler, fParameterHandler);

    if (parameterHandler)
        ok = parameterHandler->doDataSet(paramName, value);

    else if ((array = OSDynamicCast(OSArray, fParameterHandler)))
    {
        for (unsigned int i = 0;
            !ok && (parameterHandler = OSDynamicCast(IODisplayParameterHandler, array->getObject(i)));
            i++)
        {
            ok = parameterHandler->doDataSet(paramName, value);
        }
    }

    IOD_END(doDataSet,ok,0,0);
    return (ok);
}

bool IODisplay::doUpdate( void )
{
    IOD_START(doUpdate,0,0,0);
    IODisplayParameterHandler * parameterHandler;
    OSArray *                   array;
    bool                        ok = true;

    parameterHandler = OSDynamicCast(IODisplayParameterHandler, fParameterHandler);

    if (parameterHandler)
        ok = parameterHandler->doUpdate();

    else if ((array = OSDynamicCast(OSArray, fParameterHandler)))
    {
        for (unsigned int i = 0;
            (parameterHandler = OSDynamicCast(IODisplayParameterHandler, array->getObject(i)));
            i++)
        {
            ok &= parameterHandler->doUpdate();
        }
    }

    IOD_END(doUpdate,ok,0,0);
    return (ok);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
    This is the power-controlling driver for a display. It also acts as an
    agent of the policy-maker for display power which is the DisplayWrangler.
    The Display Wrangler calls here to lower power by one state when it senses
    no user activity.  It also calls here to make the display usable after it
    has been idled down, and it also calls here to make the display barely
    usable if it senses a power emergency (e.g. low battery).
    
    This driver assumes a video display, and it calls the framebuffer driver
    to control the sync signals.  Non-video display drivers (e.g. flat panels)
    subclass IODisplay and override this and other appropriate methods.
 */

void IODisplay::initPowerManagement( IOService * provider )
{
    IOD_START(initPowerManagement,0,0,0);
    static const IOPMPowerState defaultPowerStates[kIODisplayNumPowerStates] = {
        // version,
        // capabilityFlags, outputPowerCharacter, inputPowerRequirement,
        { 1, 0,                                     0, 0,           0,0,0,0,0,0,0,0 },
        { 1, 0,                                     0, 0,           0,0,0,0,0,0,0,0 },
        { 1, IOPMDeviceUsable,                      0, kIOPMPowerOn, 0,0,0,0,0,0,0,0 },
        { 1, IOPMDeviceUsable | IOPMMaxPerformance, 0, kIOPMPowerOn, 0,0,0,0,0,0,0,0 }
        // staticPower, unbudgetedPower, powerToAttain, timeToAttain, settleUpTime,
        // timeToLower, settleDownTime, powerDomainBudget
    };
    IOPMPowerState ourPowerStates[kIODisplayNumPowerStates];

    SInt32         value, min, max;

    bcopy(defaultPowerStates, ourPowerStates, sizeof(ourPowerStates));

    OSObject *paramProp = copyProperty(gIODisplayParametersKey);
    OSDictionary *displayParams = OSDynamicCast(OSDictionary, paramProp);
    if (displayParams
     && getIntegerRange(displayParams, gIODisplayAudioMuteAndScreenBlankKey,
                        &value, &min, &max)
     && ((max >= kIODisplayBlankValue) || (kIOGDbgK59Mode & gIOGDebugFlags))) {
        fDisplayPMVars->minDimState = 1;
    }
    OSSafeReleaseNULL(paramProp);
    fDisplayPMVars->currentState = kIODisplayMaxPowerState;

    // initialize superclass variables
    PMinit();
    // attach into the power management hierarchy
    provider->joinPMtree(this);

    // register ourselves with policy-maker (us)
    registerPowerDriver(this, ourPowerStates, kIODisplayNumPowerStates);

    IOD_END(initPowerManagement,0,0,0);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// setDisplayPowerState
//
// Called by the display wrangler when it decides there hasn't been user
// activity for a while.  We drop one power level.  This can be called by the
// display wrangler before we have been completely initialized, or:
// The DisplayWrangler has sensed user activity after we have idled the
// display and wants us to make it usable again.  We are running on its
// workloop thread.  This can be called before we are completely
// initialized.

void IODisplay::setDisplayPowerState(unsigned long state)
{
    // On FBController workloop
    IOD_START(setDisplayPowerState,state,0,0);
    if (initialized)
    {
        if (state)
        {
            state--;
            if (state < fDisplayPMVars->minDimState)
                state = fDisplayPMVars->minDimState;
        }
        fDisplayPMVars->displayIdle = (state != fDisplayPMVars->maxState);

        RECORD_METRIC(DBG_IOG_CHANGE_POWER_STATE_PRIV);
        IOG_KTRACE(DBG_IOG_CHANGE_POWER_STATE_PRIV, DBG_FUNC_NONE,
                   0, DBG_IOG_SOURCE_IODISPLAY,
                   0, state,
                   0, 0,
                   0, 0);

        changePowerStateToPriv(state);
    }
    IOD_END(setDisplayPowerState,0,0,0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// obsolete
void IODisplay::dropOneLevel(void)
{
    IOD_START(dropOneLevel,0,0,0);
    IOD_END(dropOneLevel,0,0,0);
}
void IODisplay::makeDisplayUsable(void)
{
    IOD_START(makeDisplayUsable,0,0,0);
    IOD_END(makeDisplayUsable,0,0,0);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// setPowerState
//
// Called by the superclass to change the display power state.

IOReturn IODisplay::setPowerState( unsigned long powerState, IOService * whatDevice )
{
    // Single threaded by IOServicePM design
    RECORD_METRIC(DBG_IOG_SET_POWER_STATE);
    IOG_KTRACE(DBG_IOG_SET_POWER_STATE, DBG_FUNC_NONE,
               0, powerState,
               0, DBG_IOG_SOURCE_IODISPLAY,
               0, 0,
               0, 0);
    IOD_START(setPowerState,powerState,0,0);

    if (fDisplayPMVars->minDimState)
    {
        OSObject *paramProp = copyProperty(gIODisplayParametersKey);
        OSDictionary * displayParams = OSDynamicCast(OSDictionary, paramProp);
        if (displayParams)
        {
            doIntegerSet(displayParams, gIODisplayAudioMuteAndScreenBlankKey, 
                                (powerState > fDisplayPMVars->minDimState) 
                                    ? kIODisplayUnblankValue : kIODisplayBlankValue);
            if (kIOGDbgK59Mode & gIOGDebugFlags)
            {
                doIntegerSet(displayParams, gIODisplayBrightnessKey, 
                                (powerState > fDisplayPMVars->minDimState) 
                                    ? 255 : 0);
            }
        }
        OSSafeReleaseNULL(paramProp);
    }

    IOD_END(setPowerState,kIOReturnSuccess,0,0);
    return (kIOReturnSuccess);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// maxCapabilityForDomainState
//
// This simple device needs only power.  If the power domain is supplying
// power, the display can go to its highest state.  If there is no power
// it can only be in its lowest state, which is off.

unsigned long IODisplay::maxCapabilityForDomainState( IOPMPowerFlags domainState )
{
    IOD_START(maxCapabilityForDomainState,domainState,0,0);
    unsigned long   ret = 0;
    if (domainState & IOPMPowerOn)
        ret = (kIODisplayMaxPowerState);
    IOD_END(maxCapabilityForDomainState,ret,0,0);
    return (ret);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// initialPowerStateForDomainState
//
// The power domain may be changing state.  If power is on in the new
// state, that will not affect our state at all.  In that case return
// what our current state is.  If domain power is off, we can attain
// only our lowest state, which is off.

unsigned long IODisplay::initialPowerStateForDomainState( IOPMPowerFlags domainState )
{
    IOD_START(initialPowerStateForDomainState,domainState,0,0);
    unsigned long   ret = 0;
    if (domainState & IOPMPowerOn)
        // domain has power
        ret = (kIODisplayMaxPowerState);
    else
        // domain is down, so display is off
        ret = (kIODisplayMaxPowerState);

    IOD_END(initialPowerStateForDomainState,ret,0,0);
    return (ret);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// powerStateForDomainState
//
// The power domain may be changing state.  If power is on in the new
// state, that will not affect our state at all.  In that case ask the ndrv
// what our current state is.  If domain power is off, we can attain
// only our lowest state, which is off.

unsigned long IODisplay::powerStateForDomainState( IOPMPowerFlags domainState )
{
    IOD_START(powerStateForDomainState,domainState,0,0);
    unsigned long   ret;
    if (domainState & IOPMPowerOn)
        // domain has power
        ret = (getPowerState());
    else
        // domain is down, so display is off
        ret = (0);

    IOD_END(powerStateForDomainState,ret,0,0);
    return (ret);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class AppleDisplay : public IODisplay
{
    OSDeclareDefaultStructors(AppleDisplay)
};

#undef super
#define super IODisplay

OSDefineMetaClassAndStructors(AppleDisplay, IODisplay)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#undef super
#define super IOService

OSDefineMetaClass( IODisplayParameterHandler, IOService )
OSDefineAbstractStructors( IODisplayParameterHandler, IOService )

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSMetaClassDefineReservedUnused(IODisplay, 0);
OSMetaClassDefineReservedUnused(IODisplay, 1);
OSMetaClassDefineReservedUnused(IODisplay, 2);
OSMetaClassDefineReservedUnused(IODisplay, 3);
OSMetaClassDefineReservedUnused(IODisplay, 4);
OSMetaClassDefineReservedUnused(IODisplay, 5);
OSMetaClassDefineReservedUnused(IODisplay, 6);
OSMetaClassDefineReservedUnused(IODisplay, 7);
OSMetaClassDefineReservedUnused(IODisplay, 8);
OSMetaClassDefineReservedUnused(IODisplay, 9);
OSMetaClassDefineReservedUnused(IODisplay, 10);
OSMetaClassDefineReservedUnused(IODisplay, 11);
OSMetaClassDefineReservedUnused(IODisplay, 12);
OSMetaClassDefineReservedUnused(IODisplay, 13);
OSMetaClassDefineReservedUnused(IODisplay, 14);
OSMetaClassDefineReservedUnused(IODisplay, 15);
OSMetaClassDefineReservedUnused(IODisplay, 16);
OSMetaClassDefineReservedUnused(IODisplay, 17);
OSMetaClassDefineReservedUnused(IODisplay, 18);
OSMetaClassDefineReservedUnused(IODisplay, 19);

OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 0);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 1);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 2);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 3);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 4);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 5);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 6);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 7);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 8);
OSMetaClassDefineReservedUnused(IODisplayParameterHandler, 9);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
