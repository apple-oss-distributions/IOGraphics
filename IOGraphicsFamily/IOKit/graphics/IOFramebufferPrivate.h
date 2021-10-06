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
    void setupCursor( IOPixelInformation * info );
    void stopCursor( void );
    IOReturn doSetup( bool full );
    IOReturn createSharedCursor( int shmemVersion,
					int maxWidth, int maxHeight );
    IOReturn setBoundingRect( Bounds * bounds );
    IOReturn setUserRanges( void );
    IOReturn getConnectFlagsForDisplayMode(
                    IODisplayModeID mode, IOIndex connection, UInt32 * flags );
    IOReturn extGetDisplayModeCount( IOItemCount * count );
    IOReturn extGetDisplayModes( IODisplayModeID * allModes,
					IOByteCount * size );
    IOReturn extSetDisplayMode( IODisplayModeID displayMode,
                                    IOIndex depth );
    IOReturn extGetInformationForDisplayMode(
		IODisplayModeID mode, void * info, IOByteCount length );

    IOReturn extGetVRAMMapOffset( IOPixelAperture aperture,
				 IOByteCount * offset );
    IOReturn extSetBounds( Bounds * bounds );

    IOReturn extSetNewCursor( void * cursor, IOIndex frame,
					IOOptionBits options );
    IOReturn extSetCursorVisible( bool visible );
    IOReturn extSetCursorPosition( SInt32 x, SInt32 y );
    IOReturn extSetColorConvertTable( UInt32 select,
                                        UInt8 * data, IOByteCount length );
    IOReturn extSetCLUTWithEntries( UInt32 index, IOOptionBits options,
                            IOColorEntry * colors, IOByteCount inputCount );
    IOReturn extSetAttribute(
            IOSelect attribute, UInt32 value, IOFramebuffer * other );

    IOReturn extGetAttribute(
            IOSelect attribute, UInt32 * value, IOFramebuffer * other );

    IOReturn makeModeList( void );
    IOReturn getDefaultMode( IOIndex connection, IODisplayModeID * mode,
			IOIndex * depth);
    IOReturn extValidateDetailedTiming(
                void * description, void * outDescription,
                IOByteCount inSize, IOByteCount * outSize );
    IOReturn beginSystemSleep( void * ackRef );

    static inline void StdFBDisplayCursor( IOFramebuffer * inst );
    static inline void StdFBRemoveCursor( IOFramebuffer * inst );
    static inline void RemoveCursor( IOFramebuffer * inst );
    static inline void DisplayCursor( IOFramebuffer * inst );
    static inline void SysHideCursor( IOFramebuffer * inst );
    static inline void SysShowCursor( IOFramebuffer * inst );
    static inline void CheckShield( IOFramebuffer * inst );

    static void StdFBDisplayCursor8P(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned char *vramPtr,
                                    unsigned int cursStart,
                                    unsigned int vramRow,
                                    unsigned int cursRow,
                                    int width,
                                    int height );

    static void StdFBDisplayCursor8G(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned char *vramPtr,
                                    unsigned int cursStart,
                                    unsigned int vramRow,
                                    unsigned int cursRow,
                                    int width,
                                    int height );

    static void StdFBDisplayCursor555(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned short *vramPtr,
                                    unsigned int cursStart,
                                    unsigned int vramRow,
                                    unsigned int cursRow,
                                    int width,
                                    int height );

    static void StdFBDisplayCursor444(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned short *vramPtr,
                                    unsigned int cursStart,
                                    unsigned int vramRow,
                                    unsigned int cursRow,
                                    int width,
                                    int height );

    static void StdFBDisplayCursor32Axxx(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned int *vramPtr,
                                    unsigned int cursStart,
                                    unsigned int vramRow,
                                    unsigned int cursRow,
                                    int width,
                                    int height );

    static void StdFBDisplayCursor32xxxA(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned int *vramPtr,
                                    unsigned int cursStart,
                                    unsigned int vramRow,
                                    unsigned int cursRow,
                                    int width,
                                    int height );

    static void StdFBRemoveCursor8(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned char *vramPtr,
                                    unsigned int vramRow,
                                    int width,
                                    int height );
    static void StdFBRemoveCursor16(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned short *vramPtr,
                                    unsigned int vramRow,
                                    int width,
                                    int height );

    static void StdFBRemoveCursor32(
                                    IOFramebuffer * inst,
                                    StdFBShmem_t *shmem,
                                    volatile unsigned int *vramPtr,
                                    unsigned int vramRow,
                                    int width,
                                    int height );

    static void deferredMoveCursor(IOFramebuffer * inst);
    static void deferredInterrupt( OSObject * owner,
                                    IOInterruptEventSource * evtSrc, int intCount );
    static void handleVBL(IOFramebuffer * inst, void * ref);
    static void connectChangeInterrupt( IOFramebuffer * inst, void * ref );
    void checkConnectionChange( bool message = true );
    void postConnectionChange( void );
    void setNextDependent( IOFramebuffer * dependent );
    IOFramebuffer * getNextDependent( void );
    void setCaptured( bool captured );
    IOReturn notifyServer( UInt8 state );
    void notifyServerAll( UInt8 state );
    bool getIsUsable( void );
    static void sleepWork( void * arg );
    static void clamshellWork( thread_call_param_t p0, thread_call_param_t p1 );
    IOOptionBits checkPowerWork( void );
    static IOReturn systemPowerChange( void * target, void * refCon,
                                    UInt32 messageType, IOService * service,
                                    void * messageArgument, vm_size_t argSize );

    IOReturn extRegisterNotificationPort(
                    mach_port_t 	port,
                    UInt32		type,
                    UInt32		refCon );
    IOReturn extAcknowledgeNotification( void );
    IOReturn extSetProperties( OSDictionary * dict );
public:
    static void clamshellEnable( SInt32 delta );
    static IOOptionBits clamshellState( void );
protected:
