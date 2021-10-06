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
/*
 * Copyright (c) 1997-1998 Apple Computer, Inc.
 *
 *
 * HISTORY
 *
 * sdouglas  22 Oct 97 - first checked in.
 * sdouglas  18 May 98 - make loadable.
 * sdouglas  23 Jul 98 - start IOKit
 * sdouglas  08 Dec 98 - start cpp
 */

#include <libkern/OSAtomic.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/assert.h>

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

const OSSymbol * gIODisplayParametersKey;
const OSSymbol * gIODisplayGUIDKey;

const OSSymbol * gIODisplayValueKey;
const OSSymbol * gIODisplayMinValueKey;
const OSSymbol * gIODisplayMaxValueKey;

const OSSymbol * gIODisplayContrastKey;
const OSSymbol * gIODisplayBrightnessKey;
const OSSymbol * gIODisplayHorizontalPositionKey;
const OSSymbol * gIODisplayHorizontalSizeKey;
const OSSymbol * gIODisplayVerticalPositionKey;
const OSSymbol * gIODisplayVerticalSizeKey;
const OSSymbol * gIODisplayTrapezoidKey;
const OSSymbol * gIODisplayPincushionKey;
const OSSymbol * gIODisplayParallelogramKey;
const OSSymbol * gIODisplayRotationKey;

const OSSymbol * gIODisplayParametersTheatreModeKey;
const OSSymbol * gIODisplayParametersTheatreModeWindowKey;

const OSSymbol * gIODisplayParametersCommitKey;
const OSSymbol * gIODisplayParametersDefaultKey;

enum {
    kIODisplayMaxUsableState  = kIODisplayMaxPowerState
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOService

OSDefineMetaClassAndAbstractStructorsWithInit( IODisplay, IOService, IODisplay::initialize() )

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct EDID {
    UInt8	header[8];
    UInt8	vendorProduct[4];
    UInt8	serialNumber[4];
    UInt8	weekOfManufacture;
    UInt8	yearOfManufacture;
    UInt8	version;
    UInt8	revision;
    UInt8	displayParams[5];
    UInt8	colorCharacteristics[10];
    UInt8	establishedTimings[3];
    UInt16	standardTimings[8];
    UInt8	detailedTimings[72];
    UInt8	extension;
    UInt8	checksum;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void IODisplay::initialize( void )
{
    gIODisplayParametersKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayParametersKey );
    gIODisplayGUIDKey		= OSSymbol::withCStringNoCopy(
                                kIODisplayGUIDKey );
    gIODisplayValueKey		= OSSymbol::withCStringNoCopy(
                                kIODisplayValueKey );
    gIODisplayMinValueKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayMinValueKey );
    gIODisplayMaxValueKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayMaxValueKey );
    gIODisplayContrastKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayContrastKey );
    gIODisplayBrightnessKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayBrightnessKey );
    gIODisplayHorizontalPositionKey = OSSymbol::withCStringNoCopy(
                                kIODisplayHorizontalPositionKey );
    gIODisplayHorizontalSizeKey = OSSymbol::withCStringNoCopy(
                                kIODisplayHorizontalSizeKey );
    gIODisplayVerticalPositionKey = OSSymbol::withCStringNoCopy(
                                kIODisplayVerticalPositionKey );
    gIODisplayVerticalSizeKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayVerticalSizeKey );
    gIODisplayTrapezoidKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayTrapezoidKey );
    gIODisplayPincushionKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayPincushionKey );
    gIODisplayParallelogramKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayParallelogramKey );
    gIODisplayRotationKey	= OSSymbol::withCStringNoCopy(
                                kIODisplayRotationKey );

    gIODisplayParametersCommitKey = OSSymbol::withCStringNoCopy(
                                kIODisplayParametersCommitKey );
    gIODisplayParametersDefaultKey = OSSymbol::withCStringNoCopy(
                                kIODisplayParametersDefaultKey );

    gIODisplayParametersTheatreModeKey = OSSymbol::withCStringNoCopy(
                                kIODisplayTheatreModeKey);
    gIODisplayParametersTheatreModeWindowKey = OSSymbol::withCStringNoCopy(
                                kIODisplayTheatreModeWindowKey);
}

IOService * IODisplay::probe(	IOService * 	provider,
				SInt32 *	score )
{
    fConnection = OSDynamicCast(IODisplayConnect, provider);

    return( this );
}

IODisplayConnect * IODisplay::getConnection( void )
{
    return( fConnection );
}


IOReturn IODisplay::getGammaTableByIndex(
	UInt32 * /* channelCount */, UInt32 * /* dataCount */,
    	UInt32 * /* dataWidth */, void ** /* data */ )
{
    return( kIOReturnUnsupported);
}

bool IODisplay::start( IOService * provider )
{
    IOFramebuffer *	framebuffer;
    UInt32		connectFlags;
    OSData *		edidData;
    EDID *		edid;
    UInt32		index;
    UInt32		vendor = 0;
    UInt32		product = 0;

    if( !super::start(provider))
        return( false );

    if( !fConnection)
        // as yet unmatched display device (ADB)
        return( true );

    framebuffer = fConnection->getFramebuffer();
    assert( framebuffer );

    fConnection->getAttributeForConnection( kConnectionFlags, &connectFlags);
    setProperty( kIODisplayConnectFlagsKey, &connectFlags, sizeof( connectFlags ));

    edidData = OSDynamicCast( OSData, getProperty( kIODisplayEDIDKey ));
    if( !edidData) {
        readFramebufferEDID();
        edidData = OSDynamicCast( OSData, getProperty( kIODisplayEDIDKey ));
    }

    if( edidData) do {

        edid = (EDID *) edidData->getBytesNoCopy();
        IOLog("%s EDID Version %d.%d\n", framebuffer->getName(),
            edid->version, edid->revision );

        if( edid->version != 1)
            continue;
        // vendor
        vendor = (edid->vendorProduct[0] << 8) | edid->vendorProduct[1];
        // product
        product = (edid->vendorProduct[3] << 8) | edid->vendorProduct[2];
#if 1
        IOLog("Vendor/product 0x%02lx/0x%02lx, ", vendor, product );
        IOLog("Est: ");
        for( index = 0; index < 3; index++)
            IOLog(" 0x%02x,", edid->establishedTimings[ index ] );
        IOLog("\nStd: " );
        for( index = 0; index < 8; index++)
            IOLog(" 0x%04x,", edid->standardTimings[ index ] );
        IOLog("\n");
#endif
    } while( false );

    do {
        UInt32	sense, extSense;
        UInt32	senseType, displayType;

        if( kIOReturnSuccess != fConnection->getAttributeForConnection(
                                kConnectionSupportsAppleSense, NULL ))
            continue;

        if( kIOReturnSuccess != framebuffer->getAppleSense(
                            fConnection->getConnection(),
                            &senseType, &sense, &extSense, &displayType ))
            continue;

        setProperty( kAppleDisplayTypeKey, displayType, 32);
        setProperty( kAppleSenseKey, ((sense & 0xff) << 8) | (extSense & 0xff), 32);

        if( 0 == vendor) {
            vendor = kDisplayVendorIDUnknown;
            if( 0 == senseType)
                product = ((sense & 0xff) << 8) | (extSense & 0xff);
            else
                product = (displayType & 0xff) << 16;
        }

    } while( false );

    if( 0 == vendor) {
        vendor = kDisplayVendorIDUnknown;
        product = kDisplayProductIDGeneric;
    }

    if( 0 == getProperty( kDisplayVendorID))
        setProperty( kDisplayVendorID, vendor, 32);
    if( 0 == getProperty( kDisplayProductID))
        setProperty( kDisplayProductID, product, 32);

    // display parameter hooks

    fNotifier = framebuffer->addFramebufferNotification(
                    &IODisplay::_framebufferEvent, this, NULL );

    IOService * look = this;
    while( look && !fParameterHandler) {
        fParameterHandler = OSDynamicCast( IODisplayParameterHandler,
                            look->getClientWithCategory(gIODisplayParametersKey));
        if( fParameterHandler && fParameterHandler->setDisplay( this ))
            break;
        fParameterHandler = 0;

        if( OSDynamicCast( IOPlatformDevice, look))
            look = OSDynamicCast( IOService, look->getParentEntry( gIODTPlane ));
        else
            look = look->getProvider();
    }

    fDisplayParams = OSDynamicCast( OSDictionary,
                            getProperty( gIODisplayParametersKey) );
    doUpdate();

    // initialize power management of the display

    fDisplayPMVars = IONew( DisplayPMVars, 1);
    assert( fDisplayPMVars );
    bzero( fDisplayPMVars, sizeof(DisplayPMVars));

    fDisplayPMVars->maxState = kIODisplayMaxPowerState;

    initPowerManagement( provider );
    registerService();

    return( true);
}

void IODisplay::stop( IOService * provider )
{
    if( pm_vars)
        PMstop();
    if( fNotifier) {
        fNotifier->remove();
        fNotifier = 0;
    }
}

IOReturn IODisplay::readFramebufferEDID( void )
{
    IOReturn		err;
    IOFramebuffer *	framebuffer;
    OSData *		data;
    IOByteCount		length;
    EDID 		readEDID;
    UInt32		index;
    UInt32		numExts;

    assert( fConnection );
    framebuffer = fConnection->getFramebuffer();
    assert( framebuffer );

    do {

        err = fConnection->getAttributeForConnection(
                                kConnectionSupportsHLDDCSense, NULL );
        if( err)
            continue;

        if( !framebuffer->hasDDCConnect( fConnection->getConnection())) {
            err = kIOReturnUnsupported;
            continue;
        }
        length = sizeof( EDID);
        err = framebuffer->getDDCBlock( fConnection->getConnection(),
                1, kIODDCBlockTypeEDID, 0, (UInt8 *) &readEDID, &length );
        if( err || (length != sizeof( EDID)))
            continue;


        data = OSData::withBytes( &readEDID, sizeof( EDID ));
        if( !data)
            continue;

        numExts = readEDID.extension;
        for( index = 2; index < (2 + numExts); index++) {
            length = sizeof( EDID);
            err = framebuffer->getDDCBlock( fConnection->getConnection(),
                    index, kIODDCBlockTypeEDID, 0, (UInt8 *) &readEDID, &length );
            if( err || (length != sizeof( EDID)))
                break;
            if( !data->appendBytes( &readEDID, sizeof( EDID ) ))
                break;
        }

        setProperty( kIODisplayEDIDKey, data );
        data->release();

    } while( false );

    return( err );
}

IOReturn IODisplay::getConnectFlagsForDisplayMode(
		IODisplayModeID mode, UInt32 * flags )
{
    IOReturn		err;
    IOFramebuffer *	framebuffer;

    assert( fConnection );
    framebuffer = fConnection->getFramebuffer();
    assert( framebuffer );

    err = framebuffer->connectFlags(
                            fConnection->getConnection(),
                            mode, flags );

    if( kIOReturnUnsupported == err) {
        *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
        err = kIOReturnSuccess;
    }

    return( err );
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

OSDictionary * IODisplay::getIntegerRange( OSDictionary * params,
                        const OSSymbol * sym, SInt32 * value, SInt32 * min, SInt32 * max )
{
    OSNumber *		num;

    params = OSDynamicCast( OSDictionary, params->getObject( sym ));

    if( params) do {

        if( value) {
            num = OSDynamicCast( OSNumber, params->getObject(gIODisplayValueKey));
            if( !num)
                continue;
            *value = num->unsigned32BitValue();
        }
        if( min) {
            num = OSDynamicCast( OSNumber, params->getObject(gIODisplayMinValueKey));
            if( !num)
                continue;
            *min = num->unsigned32BitValue();
        }
        if( max) {
            num = OSDynamicCast( OSNumber, params->getObject(gIODisplayMaxValueKey));
            if( !num)
                continue;
            *max = num->unsigned32BitValue();
        }
        return( params );

    } while( false );

    return( false );
}

bool IODisplay::setForKey( OSDictionary * params, const OSSymbol * sym,
                                    SInt32 value, SInt32 min, SInt32 max )
{
    OSNumber * num;
    SInt32 adjValue;
    bool ok;

    // invert rotation
    if( sym == gIODisplayRotationKey)
        adjValue = max - value + min;
    else
        adjValue = value;

    if( (ok = doIntegerSet( params, sym, adjValue ))) {

        num = OSNumber::withNumber( value, 32 );
        params->setObject( gIODisplayValueKey, num );
        num->release();
    }

    return( ok );
}

IOReturn IODisplay::setProperties( OSObject * properties )
{
    IOReturn		err;
    OSDictionary *	dict;
    OSDictionary *	dict2;
    OSSymbol *		sym;
    OSDictionary *	params;
    OSNumber *		valueNum;
    OSObject *		valueObj;
    OSDictionary *	valueDict;
    OSIterator *	iter;
    SInt32		min, max, value;
    bool		doCommit = false;
    bool		allOK = true;
    bool		ok;

    if( fParameterHandler) {
        err = fParameterHandler->setProperties( properties );
        if( kIOReturnUnsupported == err)
            err = kIOReturnSuccess;
    } else
        err = kIOReturnSuccess;

    if( !fDisplayParams)
        return( kIOReturnUnsupported );

    dict = OSDynamicCast( OSDictionary, properties);
    if( !dict)
        return( kIOReturnUnsupported );

    dict2 = OSDynamicCast( OSDictionary, dict->getObject(gIODisplayParametersKey));
    if( dict2)
        dict = dict2;

    if( !(dict = OSDynamicCast(OSDictionary, properties)))
        return( kIOReturnUnsupported );

    if( dict->getObject(gIODisplayParametersDefaultKey)) {
        doIntegerSet( 0, gIODisplayParametersDefaultKey, 0 );
        doUpdate();
        setProperties( fDisplayParams );
    }

    iter = OSCollectionIterator::withCollection( dict );
    if( iter) {
        for( ; (sym = (OSSymbol *) iter->getNextObject());
             allOK &= ok) {

            if( sym == gIODisplayParametersCommitKey) {
                ok = true;
                doCommit = true;
                continue;
            }
            if( sym == gIODisplayParametersDefaultKey) {
                ok = true;
                continue;
            }

            OSData * valueData = OSDynamicCast( OSData, dict->getObject(sym) );
            if( valueData) {
                ok = doDataSet( sym, valueData );
                continue;
            }
    
            ok = false;
            if( 0 == (params = getIntegerRange( fDisplayParams, sym, 0, &min, &max)))
                continue;

            valueObj = dict->getObject(sym);
            if( !valueObj)
                continue;
            if( (valueDict = OSDynamicCast( OSDictionary, valueObj)))
                valueObj = valueDict->getObject( gIODisplayValueKey );
            valueNum = OSDynamicCast( OSNumber, valueObj );
            if( !valueNum)
                continue;
            value = valueNum->unsigned32BitValue();

            if( value < min)
                value = min;
            if( value > max)
                value = max;

            ok = setForKey( params, sym, value, min, max );
        }
        iter->release();
    }

    if( doCommit)
        doIntegerSet( 0, gIODisplayParametersCommitKey, 0 );

    return( allOK ? err : kIOReturnError );
}

bool IODisplay::addParameter( OSDictionary * params, const OSSymbol * paramName,
                              SInt32 min, SInt32 max )
{
    OSDictionary *	paramDict;
    OSNumber *		num;
    bool 		ok = true;

    paramDict = (OSDictionary *) params->getObject(paramName);
    if( !paramDict)
        return( false );

    paramDict->setCapacityIncrement(1);
    num = OSNumber::withNumber( min, 32 );
    paramDict->setObject( gIODisplayMinValueKey, num);
    num->release();
    num = OSNumber::withNumber( max, 32 );
    paramDict->setObject( gIODisplayMaxValueKey, num);
    num->release();

    return( ok );
}

bool IODisplay::setParameter( OSDictionary * params, const OSSymbol * paramName,
                                        SInt32 value )
{
    OSDictionary *	paramDict;
    OSNumber *		num;
    bool 		ok = true;

    paramDict = (OSDictionary *) params->getObject(paramName);
    if( !paramDict)
        return( false );

    // invert rotation
    if( paramName == gIODisplayRotationKey) {
        SInt32 min, max;
        getIntegerRange( params, paramName, NULL, &min, &max );
        value = max - value + min;
    }

    num = OSNumber::withNumber( value, 32 );
    paramDict->setObject( gIODisplayValueKey, num);
    num->release();

    return( ok );
}

IOReturn IODisplay::_framebufferEvent( OSObject * _self, void * ref,
                    IOFramebuffer * framebuffer, IOIndex event, void * info )
{
    IODisplay *	self = (IODisplay *) _self;

    return( self->framebufferEvent( framebuffer , event, info ));
}

IOReturn IODisplay::framebufferEvent( IOFramebuffer * framebuffer,
                                        IOIndex event, void * info )
{
    IOReturn	err;

    switch( event) {

        case kIOFBNotifyDisplayModeDidChange:
            if( doUpdate() && fDisplayParams)
                setProperties( fDisplayParams );
        default:
            err = kIOReturnSuccess;
            break;
    }

    return( err);
}

bool IODisplay::doIntegerSet( OSDictionary * params,
                                const OSSymbol * paramName, UInt32 value )
{
    if( fParameterHandler)
        return( fParameterHandler->doIntegerSet( params, paramName, value ));
    else
        return( false );
}

bool IODisplay::doDataSet( const OSSymbol * paramName, OSData * value )
{
    if( fParameterHandler)
        return( fParameterHandler->doDataSet( paramName, value ));
    else
        return( false );
}

bool IODisplay::doUpdate( void )
{
    if( fParameterHandler)
        return( fParameterHandler->doUpdate());
    else
        return( false );
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

static IOPMPowerState ourPowerStates[kIODisplayNumPowerStates] = {
  // version,
  // capabilityFlags, outputPowerCharacter, inputPowerRequirement,
  { 1, 0,                                     0, 0,           0,0,0,0,0,0,0,0 },
  { 1, 0,                                     0, IOPMPowerOn, 0,0,0,0,0,0,0,0 },
  { 1, 0,                                     0, IOPMPowerOn, 0,0,0,0,0,0,0,0 },
  { 1, IOPMDeviceUsable | IOPMMaxPerformance, 0, IOPMPowerOn, 0,0,0,0,0,0,0,0 }
  // staticPower, unbudgetedPower, powerToAttain, timeToAttain, settleUpTime, 
  // timeToLower, settleDownTime, powerDomainBudget
};


void IODisplay::initPowerManagement( IOService * provider )
{
    fDisplayPMVars->currentState = kIODisplayMaxPowerState;

    // initialize superclass variables
    PMinit();
    // attach into the power management hierarchy
    provider->joinPMtree(this);	

    // register ourselves with policy-maker (us)
    registerPowerDriver(this, ourPowerStates, kIODisplayNumPowerStates);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// setAggressiveness
//
// We are informed by our power domain parent of a new level of "power management
// aggressiveness".  Our only interest is if it implies a power management
// emergency, in which case we keep the display brightness low.

IOReturn IODisplay::setAggressiveness( unsigned long type, unsigned long newLevel )
{
    unsigned long i;

    if( type == kPMGeneralAggressiveness  ) {
        if( newLevel >= kIOPowerEmergencyLevel ) {
            // emergency level
            // find lowest usable state
            for( i = 0; i < pm_vars->theNumberOfPowerStates; i++ ) {
                if( pm_vars->thePowerStates[i].capabilityFlags & IOPMDeviceUsable ) {
                    break;
                }
            }
            fDisplayPMVars->maxState = i;
            if( pm_vars->myCurrentState > i ) {
                // if we are currently above that, drop to emergency level
                changePowerStateToPriv(i);
            }
        }
        else {
            // not emergency level
            if( pm_vars->aggressiveness >= kIOPowerEmergencyLevel ) {
                // but it was emergency level
                fDisplayPMVars->maxState = pm_vars->theNumberOfPowerStates - 1;
                if( !fDisplayPMVars->displayIdle ) {
                    // return to normal usable level
                    changePowerStateToPriv(fDisplayPMVars->maxState);
                }
            }
        }
    }
    super::setAggressiveness(type, newLevel);
    return( IOPMNoErr );
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// dropOneLevel
//
// Called by the display wrangler when it decides there hasn't been user
// activity for a while.  We drop one power level.  This can be called by the
// display wrangler before we have been completely initialized.

void IODisplay::dropOneLevel( void )
{
    if( initialized) {
        fDisplayPMVars->displayIdle = true;
        if( pm_vars != NULL ) {
            if( pm_vars->myCurrentState > 0 )
                // drop a level
                changePowerStateToPriv(pm_vars->myCurrentState - 1);
            else
                // this may rescind previous request for domain power
                changePowerStateToPriv(0);
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// makeDisplayUsable
//
// The DisplayWrangler has sensed user activity after we have idled the
// display and wants us to make it usable again.  We are running on its
// workloop thread.  This can be called before we are completely
// initialized.

void IODisplay::makeDisplayUsable( void )
{
    if( initialized) {
        fDisplayPMVars->displayIdle = false;
        if( pm_vars)
            changePowerStateToPriv(fDisplayPMVars->maxState);
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// setPowerState
//
// Called by the superclass to change the display power state.

IOReturn IODisplay::setPowerState( unsigned long powerState, IOService * whatDevice )
{
    if( initialized && (powerState < kIODisplayNumPowerStates)
     && (powerState != fDisplayPMVars->currentState)) {

        fDisplayPMVars->currentState = powerState;

        powerState |= (powerState >= kIODisplayMaxUsableState) ? kFBDisplayUsablePowerState : 0;
        if( fConnection)
            fConnection->setAttributeForConnection( kConnectionPower, powerState );
    }
    return( IOPMAckImplied );
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// maxCapabilityForDomainState
//
// This simple device needs only power.  If the power domain is supplying
// power, the display can go to its highest state.  If there is no power
// it can only be in its lowest state, which is off.

unsigned long IODisplay::maxCapabilityForDomainState( IOPMPowerFlags domainState )
{
   if( domainState & IOPMPowerOn )
        return( kIODisplayMaxPowerState );
   else
        return( 0 );
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
   if( domainState & IOPMPowerOn )
        // domain has power
        return( kIODisplayMaxPowerState );
   else
        // domain is down, so display is off
        return( kIODisplayMaxPowerState );
        return( 0 );
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
   if( domainState & IOPMPowerOn )
        // domain has power
        return( pm_vars->myCurrentState );
   else
       // domain is down, so display is off
       return( 0 );
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
