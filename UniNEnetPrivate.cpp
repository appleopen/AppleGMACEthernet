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
 * Copyright (c) 1998-1999 Apple Computer
 *
 * Implementation for hardware dependent (relatively) code 
 * for the Sun GEM Ethernet controller. 
 *
 * HISTORY
 *
 * 10-Sept-97        
 *  Created.
 *
 */
#include "UniNEnet.h"
#include "UniNEnetMII.h"
#include <libkern/OSByteOrder.h>


	extern void			*kernel_pmap;
	extern globals		g;


/*
 * Private functions
 */
bool UniNEnet::allocateMemory()
{
    UInt32              rxRingSize, txRingSize;
    UInt32              i, n;
    UInt8               *virtAddr;
    UInt32              physBase;
    UInt32              physAddr;

 
    /* 
     * Calculate total space for DMA channel commands
     */
    txRingSize = (TX_RING_LENGTH * sizeof(enet_txdma_cmd_t) + 2048 - 1) & ~(2048-1);
    rxRingSize = (RX_RING_LENGTH * sizeof(enet_dma_cmd_t)   + 2048 - 1) & ~(2048-1);
	ELG( IOThreadSelf(), txRingSize << 16 | rxRingSize, 'Allo', "allocateMemory" );

    dmaCommandsSize = round_page( txRingSize + rxRingSize ); 
    /*
     * Allocate required memory
     */
    if ( !dmaCommands )
    {
      dmaCommands = (UInt8 *)IOMallocContiguous( dmaCommandsSize, PAGE_SIZE, 0 );

      if ( dmaCommands == 0  )
      {
          IOLog( "Ethernet(UniN): Cant allocate channel dma commands\n\r" );
          return false;
      }
    }

    /*
     * If we needed more than one page, then make sure we received contiguous memory.
     */
    n = (dmaCommandsSize - PAGE_SIZE) / PAGE_SIZE;
    physBase = pmap_extract(kernel_pmap, (vm_address_t) dmaCommands);

    virtAddr = (UInt8 *) dmaCommands;
    for( i=0; i < n; i++, virtAddr += PAGE_SIZE )
    {
        physAddr =  pmap_extract(kernel_pmap, (vm_address_t) virtAddr);      
        if (physAddr != (physBase + i * PAGE_SIZE) )
        {
            IOLog( "Ethernet(UniN): Cant allocate contiguous memory for dma commands\n\r" );
            return false;
        }
    }           

		/*  Setup the receive ring pointer	*/
    rxDMACommands = (enet_dma_cmd_t*)dmaCommands;

		/* Setup the transmit ring pointer	*/
    txDMACommands = (enet_txdma_cmd_t*)(dmaCommands + rxRingSize);
    
	bzero( rxMbuf, sizeof( rxMbuf ) );		// clear out all the rxMbuf pointers
	bzero( txMbuf, sizeof( txMbuf ) );		// clear out all the txMbuf pointers

    return true;
}/* end allocateMemory */


	/*-------------------------------------------------------------------------
	 *
	 * Setup the Transmit Ring - called by monitorLinkStatus()
	 * -----------------------
	 * Each transmit ring entry consists of two words to transmit data from buffer
	 * segments (possibly) spanning a page boundary. This is followed by two DMA commands 
	 * which read transmit frame status and interrupt status from the UniN chip. The last
	 * DMA command in each transmit ring entry generates a host interrupt.
	 * The last entry in the ring is followed by a DMA branch to the first
	 * entry.
	 *-------------------------------------------------------------------------*/

bool UniNEnet::initTxRing()
{
	UInt32		     i;


	ELG( this, txDMACommands, 'ITxR', "initTxRing" );

    /*
     * Clear the transmit DMA command memory
     */  
    bzero( (void *)txDMACommands, sizeof(enet_txdma_cmd_t) * TX_RING_LENGTH);
    txCommandHead = 0;
    txCommandTail = 0;
    
    txDMACommandsPhys = pmap_extract(kernel_pmap, (vm_address_t) txDMACommands);

    if ( txDMACommandsPhys == 0 )
    {
        IOLog( "Ethernet(UniN): Bad dma command buf - %08x\n\r",
               (int)txDMACommands );
    }
 
	for ( i = 0; i < TX_RING_LENGTH; i++ )
	{  
		if ( txMbuf[ i ] )
		{
			ELG( i, txMbuf[ i ], 'txpf', "UniNEnet::initTxRing - free the packet" );
			freePacket( txMbuf[ i ] );
			txMbuf[ i ] = 0;
        }
	}

    txCommandsAvail = TX_RING_LENGTH - 1; 

    txIntCnt  = 0;
    txWDCount = 0;

    return true;
}/* end initTxRing */


/*-------------------------------------------------------------------------
 *
 * Setup the Receive ring
 * ----------------------
 * Each receive ring entry consists of two DMA commands to receive data
 * into a network buffer (possibly) spanning a page boundary. The second
 * DMA command in each entry generates a host interrupt.
 * The last entry in the ring is followed by a DMA branch to the first
 * entry. 
 *
 *-------------------------------------------------------------------------*/

bool UniNEnet::initRxRing()
{
    UInt32   i;
    bool     status;
    
	ELG( this, rxDMACommands, 'IRxR', "initRxRing" );

		/* Clear the receive DMA command memory	*/
	bzero( (void*)rxDMACommands, sizeof( enet_dma_cmd_t ) * RX_RING_LENGTH );

    rxDMACommandsPhys = pmap_extract(kernel_pmap, (vm_address_t) rxDMACommands);
    if ( rxDMACommandsPhys == 0 )
    {
        IOLog( "Ethernet(UniN): Bad dma command buf - %08x\n\r",
               (int) rxDMACommands );
        return false;
    }

		/* Allocate a receive buffer for each entry in the Receive ring	*/
	for ( i = 0; i < RX_RING_LENGTH; i++ ) 
    {
        if (rxMbuf[i] == NULL)    
        {
            rxMbuf[i] = allocatePacket(NETWORK_BUFSIZE);
            if (rxMbuf[i] == NULL)    
            {
                IOLog("Ethernet(UniN): NULL packet in initRxRing\n");
                return false;
            }
		///	ELG( i, rxMbuf[i], '=RxP', "UniNEnet::initRxRing" );
        }

        /*
        * Set the DMA commands for the ring entry to transfer data to the Mbuf.
        */
        status = updateDescriptorFromMbuf(rxMbuf[i], &rxDMACommands[i], true);
        if (status == false)
        {
            IOLog("Ethernet(UniN): updateDescriptorFromMbuf error in "
                  "initRxRing\n");
            return false;
        }
    }

    /*
     * Set the receive queue head to point to the first entry in the ring.
     * Set the receive queue tail to point to a DMA Stop command after the
     * last ring entry
     */    
    i-=4;
    rxCommandHead = 0;
    rxCommandTail = i;

    return true;
}/* end initRxRing */


	/*-------------------------------------------------------------------------
	 * 
	 *
	 *
	 *-------------------------------------------------------------------------*/

void UniNEnet::flushRings( bool flushTx, bool flushRx )
{
	UInt32		i;


	ELG( TX_RING_LENGTH, RX_RING_LENGTH, 'FluR', "flushRings" );

	if ( flushRx )
	{
			// Free all mbufs from the receive ring:
	
		for ( i = 0; i < RX_RING_LENGTH; i++ )
		{
			if ( rxMbuf[ i ] )
			{
				ELG( i, rxMbuf[ i ], 'flRx', "UniNEnet::flushRings" );
				freePacket( rxMbuf[ i ] );
				rxMbuf[i] = 0;
			}
		}
	}

	if ( flushTx )
	{
			// Free all mbufs from the transmit ring.

		for ( i = 0; i < TX_RING_LENGTH; i++ )
		{
			if ( txMbuf[ i ] )
			{
				ELG( i, txMbuf[ i ], 'flTx', "UniNEnet::flushRings" );
				freePacket( txMbuf[ i ] );
				txMbuf[ i ] = 0;
			}
		}
	}
	return;
}/* end flushRings */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

void UniNEnet::startChip()
{
    UInt32  gemReg;
  

	ELG( this, 0, 'SChp', "startChip" );

	gemReg	= READ_REGISTER( TxConfiguration );				// Tx DMA enable
	gemReg |= kTxConfiguration_Tx_DMA_Enable;
	WRITE_REGISTER( TxConfiguration, gemReg );

	gemReg	= READ_REGISTER( RxConfiguration );				// Rx DMA enable
	gemReg |= kRxConfiguration_Rx_DMA_Enable;
	WRITE_REGISTER( RxConfiguration, gemReg	 );

	gemReg	= READ_REGISTER( TxMACConfiguration );			// Tx MAC enable
	gemReg |= kTxMACConfiguration_TxMac_Enable;
	WRITE_REGISTER( TxMACConfiguration, gemReg  );

	rxMacConfigReg	= kRxMACConfiguration_Rx_Mac_Enable		// Rx MAC enable
					| kRxMACConfiguration_Strip_FCS;
	WRITE_REGISTER( RxMACConfiguration, rxMacConfigReg  );

	gemReg = ~(kStatus_TX_INT_ME | kStatus_RX_DONE);	/// add kStatus_MIF_Interrupt for WOL
	WRITE_REGISTER( InterruptMask, gemReg );

	return;
}/* end startChip */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

void UniNEnet::stopChip()
{
    UInt32	gemReg;


	ELG( READ_REGISTER( TxConfiguration ), READ_REGISTER( RxConfiguration ), 'HChp', "stopChip" );

	gemReg	= READ_REGISTER( TxConfiguration );				// clear Tx DMA enable
	gemReg &= ~kTxConfiguration_Tx_DMA_Enable;
	WRITE_REGISTER( TxConfiguration, gemReg );

	gemReg	= READ_REGISTER( RxConfiguration );				// clear Rx DMA enable
	gemReg &= ~kRxConfiguration_Rx_DMA_Enable;
	WRITE_REGISTER( RxConfiguration, gemReg	 );

	IOSleep( 1 );	// Give time for DMAs to finish what they're doing.

	gemReg	= READ_REGISTER( TxMACConfiguration );			// clr Tx MAC enable
	gemReg &= ~kTxMACConfiguration_TxMac_Enable;
	WRITE_REGISTER( TxMACConfiguration, gemReg  );

    rxMacConfigReg  = READ_REGISTER( RxMACConfiguration );	// clr Rx MAC enable
    rxMacConfigReg &= ~kRxMACConfiguration_Rx_Mac_Enable;    
	WRITE_REGISTER( RxMACConfiguration, rxMacConfigReg  );

	IOSleep( 4 );		/// Wait 3.2 ms (Rx) or poll for enables to clear.

	return;
}/* end stopChip */


bool UniNEnet::getPhyType()
{
	UInt16		*pPhyType;
	UInt16		phyWord;


	ELG( this, phyId, 'gPhT', "getPhyType" );

	miiResetPHY( phyId );

	pPhyType = (UInt16 *)&phyType;
	miiReadWord( pPhyType,   MII_ID0, phyId );
	miiReadWord( pPhyType+1, MII_ID1, phyId );

	ELG( phyId, phyType, '=FyT', "UniNEnet::getPhyType" );

	if ( (phyType & MII_BCM5400_MASK) == MII_BCM5400_ID )
	{
		phyBCMType = 5400;
		ELG( this, phyId, '5400', "UniNEnet::getPhyType" );

		miiReadWord( &phyWord, MII_BCM5400_AUXCONTROL, phyId );
		phyWord |= MII_BCM5400_AUXCONTROL_PWR10BASET;
		miiWriteWord( phyWord, MII_BCM5400_AUXCONTROL, phyId );
	  
		miiReadWord( &phyWord, MII_BCM5400_1000BASETCONTROL, phyId );
		phyWord |= MII_BCM5400_1000BASETCONTROL_FULLDUPLEXCAP;
		miiWriteWord( phyWord, MII_BCM5400_1000BASETCONTROL, phyId );

		IODelay(100);   
					
		miiResetPHY( 0x1F );

		miiReadWord( &phyWord, MII_BCM5201_MULTIPHY, 0x1F );
		phyWord |= MII_BCM5201_MULTIPHY_SERIALMODE;
		miiWriteWord( phyWord, MII_BCM5201_MULTIPHY, 0x1F );

		miiReadWord( &phyWord, MII_BCM5400_AUXCONTROL, phyId );
		phyWord &= ~MII_BCM5400_AUXCONTROL_PWR10BASET;
		miiWriteWord( phyWord, MII_BCM5400_AUXCONTROL, phyId );
	}              
	else if ( (phyType & MII_BCM5400_MASK) == MII_BCM5401_ID )
	{
		phyBCMType = 5401;
		ELG( this, phyId, '5401', "UniNEnet::getPhyType" );

			// "0x1" in the low nibble which is Rev. B0 Silicon
			// "0x3" in the low nibble which is Rev. B2 Silicon	
		miiReadWord( &phyWord, MII_ID1, phyId );

			// check if this is the "B0" revision of the 5401 PHY...this will
			// help with the gigabit link establishment.
		phyWord &= 0x000F ;

		if ( (phyWord == 0x0001 ) || (phyWord == 0x0003) )
		{
			miiWriteWord( 0x0C20, 0x018, phyId );
			miiWriteWord( 0x0012, 0x017, phyId );
			miiWriteWord( 0x1804, 0x015, phyId );
			miiWriteWord( 0x0013, 0x017, phyId );
			miiWriteWord( 0x1204, 0x015, phyId );
			miiWriteWord( 0x8006, 0x017, phyId );
			miiWriteWord( 0x0132, 0x015, phyId );
			miiWriteWord( 0x8006, 0x017, phyId );
			miiWriteWord( 0x0232, 0x015, phyId );
			miiWriteWord( 0x201F, 0x017, phyId );
			miiWriteWord( 0x0A20, 0x015, phyId );
		}

		miiReadWord( &phyWord, MII_BCM5400_1000BASETCONTROL, phyId );
		phyWord |= MII_BCM5400_1000BASETCONTROL_FULLDUPLEXCAP;
		miiWriteWord( phyWord, MII_BCM5400_1000BASETCONTROL, phyId );

		IODelay( 10 );

		miiResetPHY( 0x1F );

		miiReadWord( &phyWord, MII_BCM5201_MULTIPHY, 0x1F );
		phyWord |= MII_BCM5201_MULTIPHY_SERIALMODE;
		miiWriteWord( phyWord, MII_BCM5201_MULTIPHY, 0x1F );
	}/* end else IF 5401 */
	else if ( (phyType & MII_BCM5400_MASK) == MII_BCM5411_ID )	// 5411:
	{
		phyBCMType = 5411;
		ELG( this, phyId, '5411', "UniNEnet::getPhyType - Broadcom 5411" );

		miiWriteWord( 0x8C23, 0x01C, phyId );	// setting some undocumented voltage
		miiWriteWord( 0x8CA3, 0x01C, phyId );
		miiWriteWord( 0x8C23, 0x01C, phyId );

		miiWriteWord( 0x8000, 0x000, phyId );	// reset PHY

		miiWriteWord( 0x1340, 0x000, phyId );	// advertise gigabit, full-duplex

		miiReadWord( &phyWord, MII_BCM5400_1000BASETCONTROL, phyId );
		phyWord |= MII_BCM5400_1000BASETCONTROL_FULLDUPLEXCAP;
		miiWriteWord( phyWord, MII_BCM5400_1000BASETCONTROL, phyId );

		IODelay( 10 );

		miiResetPHY( 0x1F );
	}/* end IF 5411 */
	else if ( (phyType & MII_BCM5201_MASK) == MII_BCM5201_ID )
	{
		phyBCMType = 5201;
		ELG( this, phyId, '5201', "UniNEnet::getPhyType" );
	}
	else
	{
		phyBCMType = 0;
		ELG( this, phyType, 'phy?', "UniNEnet::getPhyType" );
	}
	// IOLog("DEBUG:UniNEnet: phy type = %d\n", phyBCMType);

    return true;
}/* end getPhyType */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

bool UniNEnet::initChip()
{
    UInt32          	i, temp;
    mach_timespec_t		timeStamp;
    UInt32          	rxFifoSize;
    UInt32          	rxOff;
    UInt32          	rxOn;
    u_int16_t       	*p16;


	ELG( 0, phyId, 'ChpI', "initChip" );

    if ( fBuiltin == false )
    {
		WRITE_REGISTER( DatapathMode,		kDatapathMode_ExtSERDESMode );
		WRITE_REGISTER( SerialinkControl,	kSerialinkControl_DisableLoopback
										  | kSerialinkControl_EnableSyncDet );
		WRITE_REGISTER( Advertisement,		kAdvertisement_Full_Duplex
										  | kAdvertisement_PAUSE );
		WRITE_REGISTER( PCSMIIControl,		kPCSMIIControl_Auto_Negotiation_Enable
										  |	kPCSMIIControl_Restart_Auto_Negotiation );
		WRITE_REGISTER( PCSConfiguration,	kPCSConfiguration_Enable );
		fXIFConfiguration =					kXIFConfiguration_Tx_MII_OE
										  | kXIFConfiguration_GMIIMODE
										  | kXIFConfiguration_FDPLXLED;
    }
    else
    {
		WRITE_REGISTER( DatapathMode, kDatapathMode_GMIIMode );
		fXIFConfiguration = kXIFConfiguration_Tx_MII_OE;
   }
	WRITE_REGISTER( XIFConfiguration, fXIFConfiguration );

	WRITE_REGISTER( SendPauseCommand,		kSendPauseCommand_default );
	WRITE_REGISTER( MACControlConfiguration,kMACControlConfiguration_Receive_Pause_Enable );
	WRITE_REGISTER( InterruptMask,			kInterruptMask_None );
	WRITE_REGISTER( TxMACMask,				kTxMACMask_default );
	WRITE_REGISTER( RxMACMask,				kRxMACMask_default );
	WRITE_REGISTER( MACControlMask,			kMACControlMask_default );
	WRITE_REGISTER( Configuration,			kConfiguration_TX_DMA_Limit
										  | kConfiguration_RX_DMA_Limit
										  | kConfiguration_Infinite_Burst );

	WRITE_REGISTER( InterPacketGap0,	kInterPacketGap0_default );
	WRITE_REGISTER( InterPacketGap1,	kInterPacketGap1_default );
	WRITE_REGISTER( InterPacketGap2,	kInterPacketGap2_default );
	WRITE_REGISTER( SlotTime,			kSlotTime_default );
	WRITE_REGISTER( MinFrameSize,		kMinFrameSize_default );
	WRITE_REGISTER( MaxFrameSize,		kMaxFrameSize_default );
	WRITE_REGISTER( PASize,				kPASize_default );
	WRITE_REGISTER( JamSize,			kJamSize_default );
	WRITE_REGISTER( AttemptLimit,		kAttemptLimit_default );
	WRITE_REGISTER( MACControlType,		kMACControlType_default );

    p16 = (u_int16_t *) myAddress.bytes;
    for ( i=0; i < sizeof(IOEthernetAddress) / 2; i++ )
		WRITE_REGISTER( MACAddress[ i ], p16[ 2 - i ] );

    for ( i=0; i < 3; i ++ )
    {
		WRITE_REGISTER( MACAddress[ i + 3 ],	0 );
		WRITE_REGISTER( AddressFilter[ i  ],	0 );
    }

	WRITE_REGISTER( MACAddress[ 6 ], kMACAddress_default_6 );
	WRITE_REGISTER( MACAddress[ 7 ], kMACAddress_default_7 );
	WRITE_REGISTER( MACAddress[ 8 ], kMACAddress_default_8 );

	WRITE_REGISTER( AddressFilter2_1Mask,	0 );
	WRITE_REGISTER( AddressFilter0Mask,		0 );

    for ( i=0; i < 16; i++ )
		WRITE_REGISTER( HashTable[ i ], 0 );

	WRITE_REGISTER( NormalCollisionCounter,					0 );
	WRITE_REGISTER( FirstAttemptSuccessfulCollisionCounter,	0 );
	WRITE_REGISTER( ExcessiveCollisionCounter,				0 );
	WRITE_REGISTER( LateCollisionCounter,					0 );
	WRITE_REGISTER( DeferTimer,								0 );
	WRITE_REGISTER( PeakAttempts,							0 );
	WRITE_REGISTER( ReceiveFrameCounter,					0 );
	WRITE_REGISTER( LengthErrorCounter,						0 );
	WRITE_REGISTER( AlignmentErrorCounter,					0 );
	WRITE_REGISTER( FCSErrorCounter,						0 );
	WRITE_REGISTER( RxCodeViolationErrorCounter,			0 );

    IOGetTime(&timeStamp); 
	WRITE_REGISTER( RandomNumberSeed, timeStamp.tv_nsec & 0xFFFF );

	WRITE_REGISTER( TxDescriptorBaseLow, txDMACommandsPhys );
	WRITE_REGISTER( TxDescriptorBaseHigh, 0 );

	temp	= kTxConfiguration_TxFIFO_Threshold
			| TX_RING_LENGTH_FACTOR << kTxConfiguration_Tx_Desc_Ring_Size_Shift;
	WRITE_REGISTER( TxConfiguration, temp );

	WRITE_REGISTER( TxMACConfiguration, 0 );
	IOSleep( 4 );		/// Wait or poll for enable to clear.

    setDuplexMode( (phyId == 0xff) ? true : false );
   
	WRITE_REGISTER( RxDescriptorBaseLow,	rxDMACommandsPhys );
	WRITE_REGISTER( RxDescriptorBaseHigh,	0 );

	WRITE_REGISTER( RxKick, RX_RING_LENGTH - 4 );

	temp	= kRxConfiguration_RX_DMA_Threshold
	///		| kRxConfiguration_Batch_Disable	may cause 4x primary interrupts
			| RX_RING_LENGTH_FACTOR << kRxConfiguration_Rx_Desc_Ring_Size_Shift
			| kRxConfiguration_Checksum_Start_Offset;
	WRITE_REGISTER( RxConfiguration, temp );

	rxMacConfigReg = 0;
	WRITE_REGISTER( RxMACConfiguration,	rxMacConfigReg );
	IOSleep( 4 ); 		// it takes time to clear the enable bit

	rxFifoSize	= READ_REGISTER( RxFIFOSize );	// 64-byte (kPauseThresholds_Factor) chunks

    rxOff  = rxFifoSize - ((kGEMMacMaxFrameSize_Aligned + 8) * 2 / kPauseThresholds_Factor);
    rxOn   = rxFifoSize - ((kGEMMacMaxFrameSize_Aligned + 8) * 3 / kPauseThresholds_Factor);

	WRITE_REGISTER( PauseThresholds,
					  (rxOff << kPauseThresholds_OFF_Threshold_Shift)
					| (rxOn	 << kPauseThresholds_ON_Threshold_Shift) );

	temp = READ_REGISTER( BIFConfiguration );
	if ( temp & kBIFConfiguration_M66EN )
		 temp = kRxBlanking_default_66;
	else temp = kRxBlanking_default_33;
	WRITE_REGISTER( RxBlanking, temp );

	if ( fBuiltin )
		WRITE_REGISTER( WOLWakeupCSR, 0 );	// Disable Wake on LAN

	ELG( READ_REGISTER( TxFIFOSize ), READ_REGISTER( RxFIFOSize ), 'FFsz', "initChip - done" );

    return true;
}/* end initChip */


void UniNEnet::setDuplexMode( bool duplexMode )
{
	UInt32		txMacConfig;


	isFullDuplex	= duplexMode;
	txMacConfig		= READ_REGISTER( TxMACConfiguration );

	ELG( txMacConfig, duplexMode, 'DupM', "setDuplexMode" );

	WRITE_REGISTER( TxMACConfiguration, txMacConfig & ~kTxMACConfiguration_TxMac_Enable );
    while ( READ_REGISTER( TxMACConfiguration ) & kTxMACConfiguration_TxMac_Enable )
      ;	/// Set time limit of a couple of milliseconds.


    if ( isFullDuplex )
    {
		txMacConfig |= (kTxMACConfiguration_Ignore_Collisions | kTxMACConfiguration_Ignore_Carrier_Sense);
		fXIFConfiguration &= ~kXIFConfiguration_Disable_Echo;
    }
    else
    {
		txMacConfig &= ~(kTxMACConfiguration_Ignore_Collisions | kTxMACConfiguration_Ignore_Carrier_Sense);
		fXIFConfiguration |= kXIFConfiguration_Disable_Echo;
    }

	WRITE_REGISTER( TxMACConfiguration,	txMacConfig );
	WRITE_REGISTER( XIFConfiguration,	fXIFConfiguration );
	return;
}/* end setDuplexMode */


void UniNEnet::restartReceiver()
{

    // Perform a software reset to the logic in the RX MAC.
    // The MAC config register should be re-programmed following
    // the reset. Everything else *should* be unaffected.

	WRITE_REGISTER( RxMACSoftwareResetCommand, kRxMACSoftwareResetCommand_Reset );

    // Poll until the reset bit is cleared by the hardware.

    for ( int i = 0; i < 5000; i++ )
    {
		if ( ( READ_REGISTER( RxMACSoftwareResetCommand )
				& kRxMACSoftwareResetCommand_Reset ) == 0 )
        {
            break;	// 'i' is always 0 or 1
        }
        IODelay(1);
    }

    // Update the MAC Config register. Watch out for the programming
    // restrictions documented in the GEM specification!!!
    //
    // Disable MAC before setting any other bits in the MAC config
    // register.

	WRITE_REGISTER( RxMACConfiguration, 0 );

    for ( int i = 0; i < 5000; i++ )
    {
		if ( ( READ_REGISTER( RxMACConfiguration )
				& kRxMACConfiguration_Rx_Mac_Enable ) == 0 )
        {
            break;	// 'i' is always 0
        }
        IODelay(1);
    }

    // Update MAC config register.

	WRITE_REGISTER( RxMACConfiguration, rxMacConfigReg );
	return;
}/* end restartReceiver */


bool UniNEnet::transmitPacket( struct mbuf *packet )
{
	GEMTxDescriptor		*dp;			// descriptor pointer
	UInt32              i,j,k;
	struct mbuf         *m;
	UInt32              dataPhys;

    
    for ( m = packet, i=1; m->m_next; m=m->m_next, i++ )
      ;
      
	ELG( i, packet, '  Tx', "UniNEnet::transmitPacket" );
    
	if ( i > txCommandsAvail )  
		return false;

	j = txCommandTail;
    
	OSAddAtomic( -i, (SInt32*)&txCommandsAvail );

    m = packet;

    do
    {        
		k = j;		// k will be the index to the last element on loop exit
        
        dataPhys = (UInt32)mcl_to_paddr( mtod( m, char* ) );
		if ( dataPhys == 0 )
			 dataPhys = pmap_extract( kernel_pmap, mtod( m, vm_offset_t ) );

		dp = &txDMACommands[ j ].desc_seg[ 0 ];
		OSWriteLittleInt32( &dp->bufferAddrLo,	0, dataPhys );
		OSWriteLittleInt32( &dp->flags0,		0, m->m_len );
		dp->flags1 = 0;
		txIntCnt++;
		j = (j + 1) & TX_RING_WRAP_MASK;
    } while ( (m = m->m_next) != 0 );

	txMbuf[ k ] = packet;	// save the packet mbuf address in the last segment's index

	txDMACommands[ k ].desc_seg[ 0 ].flags0             |= OSSwapHostToLittleConstInt32( kGEMTxDescFlags0_EndOfFrame );
	txDMACommands[ txCommandTail ].desc_seg[ 0 ].flags0 |= OSSwapHostToLittleConstInt32( kGEMTxDescFlags0_StartOfFrame );
    if ( txIntCnt >= TX_DESC_PER_INT )
    {
		txDMACommands[ txCommandTail ].desc_seg[ 0 ].flags1 |= OSSwapHostToLittleConstInt32( kGEMTxDescFlags1_Int );
        txIntCnt = txIntCnt % TX_DESC_PER_INT;
    }

#ifdef HDW_CHECKSUM
	UInt32				demandMask;
	UInt16				param0, param1;

	getChecksumDemand(	packet,
						kChecksumFamilyTCPIP,
						&demandMask,			// this chip supports only TCP checksumming
						&param0,				// start offset
						&param1 );				// stuff offset
	if ( demandMask & kChecksumTCPSum16 )
	{
		txDMACommands[ txCommandTail ].desc_seg[ 0 ].flags0
			|= OSSwapHostToLittleConstInt32(	kGEMTxDescFlags0_ChecksumEnable
											|	param0 << kGEMTxDescFlags0_ChecksumStart_Shift
											|	param1 << kGEMTxDescFlags0_ChecksumStuff_Shift );
	}
#endif // HDW_CHECKSUM

    txCommandTail = j;
          
	WRITE_REGISTER( TxKick, j );
     
    return true;          
}/* end transmitPacket */


/*-------------------------------------------------------------------------
 * _receivePacket
 * --------------
 * This routine runs the receiver in polled-mode (yuk!) for the kernel debugger.
 * Don't mess with the interrupt source here that can deadlock in the debugger
 *
 * The _receivePackets allocate MBufs and pass them up the stack. The kernel
 * debugger interface passes a buffer into us. To reconsile the two interfaces,
 * we allow the receive routine to continue to allocate its own buffers and
 * transfer any received data to the passed-in buffer. This is handled by 
 * _receivePacket calling _packetToDebugger.
 *-------------------------------------------------------------------------*/

void UniNEnet::receivePacket( void *   pkt,
                              UInt32 * pkt_len,
                              UInt32   timeout )
{
    mach_timespec_t	startTime;
    mach_timespec_t	currentTime;
    UInt32          elapsedTimeMS;

///	ELG( 0, ready, 'kdRx', "receivePacket - kernel debugger routine" );

    *pkt_len = 0;

    if (ready == false)
    {
        return;
    }

#if USE_ELG
	UInt32		elgFlag = g.evLogFlag;	// if kernel debugging, turn off ELG
	g.evLogFlag = 0;
#endif // USE_ELG

    debuggerPkt     = pkt;
    debuggerPktSize = 0;

    IOGetTime(&startTime);
    do
    {
        receivePackets( true );
        IOGetTime( &currentTime );
        elapsedTimeMS = (currentTime.tv_nsec - startTime.tv_nsec) / (1000*1000);
    } 
    while ( (debuggerPktSize == 0) && (elapsedTimeMS < timeout) );

    *pkt_len = debuggerPktSize;
#if USE_ELG
	g.evLogFlag = elgFlag;
#endif // USE_ELG

    return;
}/* end receivePacket */


	/*-------------------------------------------------------------------------
	 * packetToDebugger
	 * -----------------
	 * This is called by _receivePackets when we are polling for kernel debugger
	 * packets. It copies the MBuf contents to the buffer passed by the debugger.
	 * It also sets the var debuggerPktSize which will break the polling loop.
	 *-------------------------------------------------------------------------*/

void UniNEnet::packetToDebugger( struct mbuf *packet, u_int size )
{
	ELG( packet, size, 'ToDb', "packetToDebugger" );

    debuggerPktSize = size;
    bcopy( mtod(packet, char*), debuggerPkt, size );
	return;
}/* end packetToDebugger */


	/*-------------------------------------------------------------------------
	 * sendPacket
	 * -----------
	 *
	 * This routine runs the transmitter in polled-mode (yuk!) for the kernel debugger.
	 * Don't mess with the interrupt source here that can deadlock in the debugger
	 *
	 *-------------------------------------------------------------------------*/

void UniNEnet::sendPacket( void *pkt, UInt32 pkt_len )
{
    mach_timespec_t	startTime;
    mach_timespec_t	currentTime;
    UInt32		elapsedTimeMS;

///	ELG( pkt, pkt_len, 'Send', "sendPacket" );

    if (!ready || !pkt || (pkt_len > ETHERMAXPACKET))
    {
        return;
    }

#if USE_ELG
	UInt32		elgFlag = g.evLogFlag;	// if kernel debugging, turn off ELG
	g.evLogFlag = 0;
#endif // USE_ELG

    /*
     * Wait for the transmit ring to empty
     */
    IOGetTime(&startTime); 
    do
    {   
      debugTransmitInterruptOccurred();
      IOGetTime(&currentTime);
      elapsedTimeMS = (currentTime.tv_nsec - startTime.tv_nsec) / (1000*1000);
	}
	while ( (txCommandHead != txCommandTail) && (elapsedTimeMS < TX_KDB_TIMEOUT) ); 
    
    if ( txCommandHead != txCommandTail )
    {
      IOLog( "Ethernet(UniN): Polled tranmit timeout - 1\n\r");
      return;
    }

    /*
     * Allocate a MBuf and copy the debugger transmit data into it.
     *
     * jliu - no allocation, just recycle the same buffer dedicated to
     * KDB transmit.
     */
    txDebuggerPkt->m_next = 0;
    txDebuggerPkt->m_data = (caddr_t) pkt;
    txDebuggerPkt->m_pkthdr.len = txDebuggerPkt->m_len = pkt_len;

    /*
     * Send the debugger packet. txDebuggerPkt must not be freed by
     * the transmit routine.
     */
    transmitPacket(txDebuggerPkt);

    /*
     * Poll waiting for the transmit ring to empty again
     */
    do 
    {
        debugTransmitInterruptOccurred();
        IOGetTime(&currentTime);
        elapsedTimeMS = (currentTime.tv_nsec - startTime.tv_nsec) / (1000*1000);
    }
    while ( (txCommandHead != txCommandTail) &&
            (elapsedTimeMS < TX_KDB_TIMEOUT) ); 

    if ( txCommandHead != txCommandTail )
    {
        IOLog( "Ethernet(UniN): Polled tranmit timeout - 2\n\r");
    }
#if USE_ELG
	g.evLogFlag = elgFlag;
#endif // USE_ELG

	return;
}/* end sendPacket */


/*-------------------------------------------------------------------------
 * _sendDummyPacket
 * ----------------
 * The UniN receiver seems to be locked until we send our first packet.
 *
 *-------------------------------------------------------------------------*/
void UniNEnet::sendDummyPacket()
{
    union
    {
        UInt8                 bytes[64];
        IOEthernetAddress     enet_addr[2];
    } dummyPacket;

	ELG( &dummyPacket, sizeof( dummyPacket ), 'SenD', "sendDummyPacket" );

    bzero( &dummyPacket, sizeof(dummyPacket) );


    dummyPacket.enet_addr[0] = myAddress;   
    dummyPacket.enet_addr[1] = myAddress;

    sendPacket( (void*)dummyPacket.bytes, (unsigned int)sizeof( dummyPacket ) );
	return;
}/* end sendDummyPacket */



/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

bool UniNEnet::receiveInterruptOccurred()
{
    return receivePackets( false );
}/* end receiveInterruptOccurred */


	/*-------------------------------------------------------------------------
	 *
	 *
	 *
	 *-------------------------------------------------------------------------*/

bool UniNEnet::receivePackets( bool fDebugger )
{
	struct mbuf	*packet;
	UInt32		i, last;
	int			receivedFrameSize = 0;
	UInt16		dmaFlags;
	UInt16		checksum;
	UInt32		rxPktStatus = 0;
	bool		passPacketUp;
	bool		reusePkt;
	bool		status;
	bool		useNetif = !fDebugger && netifEnabled;
	bool		packetsQueued = false;
	bool		replaced;

   
    last      = (UInt32)-1;  
    i         = rxCommandHead;

	ELG( rxCommandTail, rxCommandHead, 'Rx I', "receivePackets" );

    while ( 1 )
    {
        passPacketUp = false;
        reusePkt     = false;

		dmaFlags = OSReadLittleInt16( &rxDMACommands[ i ].desc_seg[ 0 ].frameDataSize,     0 );
		checksum = OSReadLittleInt16( &rxDMACommands[ i ].desc_seg[ 0 ].tcpPseudoChecksum, 0 );

        /* 
         * If the current entry has not been written, then stop at this entry
         */
        if ( dmaFlags & kGEMRxDescFrameSize_Own )
        {
            break;
        }


        receivedFrameSize	= dmaFlags & kGEMRxDescFrameSize_Mask;
		rxPktStatus			= OSReadLittleInt32( &rxDMACommands[ i ].desc_seg[ 0 ].flags, 0 );


        /*
         * Reject packets that are runts or that have other mutations.
         */
        if ( receivedFrameSize < (ETHERMINPACKET - ETHERCRC) || 
                     receivedFrameSize > (ETHERMAXPACKET + ETHERCRC) ||
                         rxPktStatus & kGEMRxDescFlags_BadCRC )
		{
            reusePkt = true;
			NETWORK_STAT_ADD( inputErrors );
			if ( receivedFrameSize < (ETHERMINPACKET - ETHERCRC) )
				 ETHERNET_STAT_ADD( dot3RxExtraEntry.frameTooShorts );
			else ETHERNET_STAT_ADD( dot3StatsEntry.frameTooLongs );
			ELG( rxPktStatus, receivedFrameSize, '-Rx-', "receivePackets - mutant or bad CRC." );
        }
        else if ( useNetif == false )
        {
            /*
             * Always reuse packets in debugger mode. We also refuse to
             * pass anything up the stack unless the driver is open. The
             * hardware is enabled before the stack has opened us, to
             * allow earlier debug interface registration. But we must
             * not pass any packets up.
             */
            reusePkt = true;
            if (fDebugger)
            {
                packetToDebugger(rxMbuf[i], receivedFrameSize);
            }
        }
        
 
        /*
         * Before we pass this packet up the networking stack. Make sure we
         * can get a replacement. Otherwise, hold on to the current packet and
         * increment the input error count.
         * Thanks Justin!
         */

        packet = 0;

        if ( reusePkt == false )
        {
            packet = replaceOrCopyPacket(&rxMbuf[i], receivedFrameSize, &replaced);

            reusePkt = true;

            if (packet && replaced)
            {
                status = updateDescriptorFromMbuf(rxMbuf[i], &rxDMACommands[i], true);

                if (status)
                {
                    reusePkt = false;
                }
                else
                {
					ELG( packet, rxMbuf[i], 'upd-', "UniNEnet::receivePackets" );
                    // Assume descriptor has not been corrupted.
                    freePacket(rxMbuf[i]);  // release new packet.
                    rxMbuf[i] = packet;     // get the old packet back.
                    packet = 0;             // pass up nothing.
                    IOLog("Ethernet(UniN): updateDescriptorFromMbuf error\n");
                }
            }
            
			if ( packet == 0 )
				NETWORK_STAT_ADD( inputErrors );
        }/* end IF reusePkt is false */

			/* Install the new MBuf for the one we're about	*/
			/* to pass to the network stack					*/

        if ( reusePkt == true )
        {
			rxDMACommands[i].desc_seg[0].flags         = 0;
			rxDMACommands[i].desc_seg[0].frameDataSize = OSSwapHostToLittleConstInt16( NETWORK_BUFSIZE | kGEMRxDescFrameSize_Own );
        }

        last = i;	/* Keep track of the last receive descriptor processed	*/
		i = (i + 1) & RX_RING_WRAP_MASK;

		if ( (i & 3) == 0 )		// only kick modulo 4
			WRITE_REGISTER( RxKick, (i - 4) & RX_RING_WRAP_MASK );

        if ( fDebugger )
            break;

			/* Transfer received packet to the network stack:	*/

        if ( packet )
        {
            KERNEL_DEBUG(	DBG_UniN_RXCOMPLETE | DBG_FUNC_NONE, 
                			(int)packet, (int)receivedFrameSize, 0, 0, 0 );
#ifdef HDW_CHECKSUM
			if ( (receivedFrameSize > 64) && (checksum != 0) )
				setChecksumResult(	packet,
									kChecksumFamilyTCPIP,
									kChecksumTCPSum16,
									0,							// validMask
									checksum,					// param0: actual cksum
									kRxHwCksumStartOffset );	// param1: 1st byte cksum'd 

#endif // HDW_CHECKSUM
			networkInterface->inputPacket( packet, receivedFrameSize, true );
			NETWORK_STAT_ADD( inputPackets );
			packetsQueued = true;
        }
	}/* end WHILE */

    if ( last != (UInt32)-1 )
    {
        rxCommandTail = last;
        rxCommandHead = i;
    }

    return packetsQueued;
}/* end receivePackets */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

bool UniNEnet::transmitInterruptOccurred()
{
	UInt32			i;
	bool			serviced	= false;


	i = READ_REGISTER( TxCompletion );
	ELG( txCommandHead, i, ' Tx+', "transmitInterruptOccurred" );

	while ( i != txCommandHead )	// i and txCommandHead race each other
	{
		do		// This DO reduces READ_REGISTER calls which access the PCI bus
		{		/* Free the MBuf we just transmitted	*/

			KERNEL_DEBUG(	DBG_UniN_TXCOMPLETE | DBG_FUNC_NONE,
							txMbuf[ txCommandHead ], 0, 0, 0, 0 );

			OSIncrementAtomic( (SInt32*)&txCommandsAvail );	// ???put outside loop?

			if ( txMbuf[ txCommandHead ] )
			{
			//	ELG( txCommandHead, txMbuf[ txCommandHead ], 'TxPF', "UniNEnet::receivePackets - free the packet" );
				freePacket( txMbuf[ txCommandHead ] );
				txMbuf[ txCommandHead ] = 0;
				NETWORK_STAT_ADD( outputPackets );
			}

			txCommandHead = (txCommandHead + 1) & TX_RING_WRAP_MASK;

		} while ( i != txCommandHead );		// loop til txCommandHead catches i

		serviced = true;
		i = READ_REGISTER( TxCompletion );	// see if i advanced during last batch
	}/* end WHILE */

	return serviced;
}/* end transmitInterruptOccurred */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

void UniNEnet::debugTransmitInterruptOccurred()
{
	UInt32		i;


		// Set the debugTxPoll flag to indicate the debugger was active
		// and some cleanup may be needed when the driver returns to
		// normal operation.
	debugTxPoll = true;

	i = READ_REGISTER( TxCompletion );

	while ( i != txCommandHead )
	{
			/* Free the mbuf we just transmitted.
			 *
			 * If it is the debugger packet, just remove it from the ring.
			 * and reuse the same packet for the next sendPacket() request.
			 */
			 
			/* While in debugger mode, do not touch the mbuf pool.
			 * Queue any used mbufs to a local queue. This queue
			 * will get flushed after we exit from debugger mode.
			 *
			 * During continuous debugger transmission and
			 * interrupt polling, we expect only the txDebuggerPkt
			 * to show up on the transmit mbuf ring.
			 */

		OSIncrementAtomic( (SInt32*)&txCommandsAvail );

		KERNEL_DEBUG(	DBG_UniN_TXCOMPLETE | DBG_FUNC_NONE,
						(int)txMbuf[ txCommandHead ],
						(int)txMbuf[ txCommandHead ]->m_pkthdr.len, 0, 0, 0 );

		if ( txMbuf[ txCommandHead ] )
		{
			if ( txMbuf[ txCommandHead ] != txDebuggerPkt )
				debugQueue->enqueue( txMbuf[ txCommandHead ] );
			txMbuf[ txCommandHead ] = 0;
		}

		txCommandHead = (txCommandHead + 1) & TX_RING_WRAP_MASK;
	}/* end WHILE */

    return;
}/* end debugTransmitInterruptOccurred */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

void UniNEnet::debugTransmitCleanup()
{
    // Debugger was active, clear all packets in the debugQueue, and
    // issue a start(), just in case the debugger became active while the
    // ring was full and the output queue stopped. Since the debugger
    // does not restart the output queue, to avoid calling
    // semaphore_signal() which may reenable interrupts, we need to
    // make sure the output queue is not stalled after the debugger has
    // flushed the ring.
    
    debugQueue->flush();

    transmitQueue->start();
	return;
}/* end debugTransmitCleanup */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

bool UniNEnet::updateDescriptorFromMbuf(struct mbuf * m,  enet_dma_cmd_t *desc, bool isReceive)
{
    struct IOPhysicalSegment    	segVector[1];
    UInt32			segments;

    segments = mbufCursor->getPhysicalSegmentsWithCoalesce(m, segVector);
    
    if ( segments == 0 || segments > 1 )
    {
        IOLog("Ethernet(UniN): updateDescriptorFromMbuf error, %d segments\n", (int)segments);
        return false;
    }    
    
    if ( isReceive )
    {
        enet_dma_cmd_t      *rxCmd = (enet_dma_cmd_t *)desc;

		OSWriteLittleInt32( &rxCmd->desc_seg[0].bufferAddrLo,  0, segVector[0].location );
		OSWriteLittleInt16( &rxCmd->desc_seg[0].frameDataSize, 0, segVector[0].length | kGEMRxDescFrameSize_Own );
		rxCmd->desc_seg[0].flags = 0;
    }
    else
    {
        enet_txdma_cmd_t    *txCmd = (enet_txdma_cmd_t *)desc;

		OSWriteLittleInt32( &txCmd->desc_seg[0].bufferAddrLo, 0, segVector[0].location );
		OSWriteLittleInt32( &txCmd->desc_seg[0].flags0, 0, segVector[0].length
													|	kGEMTxDescFlags0_StartOfFrame
													|	kGEMTxDescFlags0_EndOfFrame );

		txCmd->desc_seg[0].flags1 = 0;
		txIntCnt += 1;
		if ( (txIntCnt % TX_DESC_PER_INT) == 0 )	/// Divide???
			txCmd->desc_seg[0].flags1 = OSSwapHostToLittleConstInt32( kGEMTxDescFlags1_Int );
    }                                          

    return true;
}/* end updateDescriptorFromMbuf */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

void UniNEnet::monitorLinkStatus( bool firstPoll )
{
	UInt32				gemReg;
    UInt16          	phyStatus;
    UInt16          	linkStatus;
    UInt16          	linkMode;
    UInt16          	lpAbility;
    UInt16          	phyStatusChange;
    bool            	fullDuplex = false;
    UInt32          	linkSpeed = 0;
    IOMediumType    	mediumType = kIOMediumEthernetNone;
    IONetworkMedium	*medium;


	ELG( phyId, firstPoll, ' MLS', "monitorLinkStatus" );

    if ( firstPoll )
    {
        phyStatusPrev  = 0;
        linkStatusPrev = kLinkStatusUnknown;
    }

    if ( phyId == 0xff )
    {
		phyStatus = READ_REGISTER( PCSMIIStatus )				& 0x0000FFFF;
		lpAbility = READ_REGISTER( PCSMIILinkPartnerAbility )	& 0x0000FFFF;
    }
    else 
    {
        if ( miiReadWord( &phyStatus, MII_STATUS, phyId) != true )
        {
            return;
        }
        miiReadWord( &lpAbility, MII_STATUS, phyId);
    }
	ELG( lpAbility, phyStatus, ' mls', "monitorLinkStatus - LinkPartnerAbility and Status" );

    phyStatusChange = (phyStatusPrev ^ phyStatus) &
                      ( MII_STATUS_LINK_STATUS |
                        MII_STATUS_NEGOTIATION_COMPLETE );

    if ( phyStatusChange || firstPoll )
    {
        if ( firstPoll && fBuiltin )
        {
            // For the initial link status poll, wait a bit, then
            // re-read the status register to clear any latched bits.
            // Why wait? Well, the debugger can kick in shortly after
            // this function returns, and we want the duplex setting
            // on the MAC to match the PHY.

            miiWaitForAutoNegotiation( phyId );
            miiReadWord(&phyStatus, MII_STATUS, phyId);
            miiReadWord(&phyStatus, MII_STATUS, phyId);
        }

		gemReg = READ_REGISTER( MACControlConfiguration );
		if ( lpAbility & MII_LPAR_PAUSE )
			 gemReg |=  kMACControlConfiguration_Send_Pause_Enable;
		else gemReg &= ~kMACControlConfiguration_Send_Pause_Enable;
		WRITE_REGISTER( MACControlConfiguration, gemReg );

        if ( (phyStatus & MII_STATUS_LINK_STATUS) &&
             ( firstPoll || (phyStatus & MII_STATUS_NEGOTIATION_COMPLETE) ) )
        {
            if ( phyId == 0xff )
            {
                linkSpeed  = 1000;
                fullDuplex = true;
                mediumType = kIOMediumEthernet1000BaseSX;
            }
            else if ( (phyType & MII_LXT971_MASK) == MII_LXT971_ID )
            {
                miiReadWord( &linkStatus, MII_LXT971_STATUS_2, phyId );
                linkSpeed  = (linkStatus & MII_LXT971_STATUS_2_SPEED)  ?
                              100 : 10;
                fullDuplex = (linkStatus & MII_LXT971_STATUS_2_DUPLEX) ?
                              true : false;
                mediumType = (linkSpeed == 10) ? kIOMediumEthernet10BaseT : 
                                                  kIOMediumEthernet100BaseTX;
            }
            else if ( (phyType & MII_BCM5201_MASK) == MII_BCM5201_ID )
            {
                miiReadWord( &linkStatus, MII_BCM5201_AUXSTATUS, phyId );
                linkSpeed  = (linkStatus & MII_BCM5201_AUXSTATUS_SPEED)  ?
                             100 : 10;
                fullDuplex = (linkStatus & MII_BCM5201_AUXSTATUS_DUPLEX) ?
                              true : false;
                mediumType = (linkSpeed == 10) ? kIOMediumEthernet10BaseT : 
                                                 kIOMediumEthernet100BaseTX;
            }
            else if ( ((phyType & MII_BCM5400_MASK) == MII_BCM5400_ID)
				  ||  ((phyType & MII_BCM5400_MASK) == MII_BCM5401_ID)		/// mlj temporary quick fix
				  ||  ((phyType & MII_BCM5400_MASK) == MII_BCM5411_ID) )	/// mlj temporary quick fix
            {
                miiReadWord( &linkStatus, MII_BCM5400_AUXSTATUS, phyId );

                linkMode = (linkStatus & MII_BCM5400_AUXSTATUS_LINKMODE_MASK) /
                           MII_BCM5400_AUXSTATUS_LINKMODE_BIT;

				if ( linkMode < 6 )
					 fXIFConfiguration &= ~kXIFConfiguration_GMIIMODE;
                else fXIFConfiguration |=  kXIFConfiguration_GMIIMODE;
				WRITE_REGISTER( XIFConfiguration, fXIFConfiguration );

                if ( linkMode == 0 )
                {
                    linkSpeed = 0;
                }
                else if ( linkMode < 3 )
                {
                    linkSpeed   =  10;
                    fullDuplex  =  ( linkMode < 2 ) ? false : true; 
                    mediumType  =  kIOMediumEthernet10BaseT;                   
                }
                else if ( linkMode < 6 )
                {
                    linkSpeed   =  100;
                    fullDuplex  =  ( linkMode < 5 ) ? false : true;
                    mediumType  =  kIOMediumEthernet100BaseTX;  
                }
                else
                {
                    linkSpeed   = 1000;
                    fullDuplex  = true;
                    mediumType  =  kIOMediumEthernet1000BaseTX;
                }                    
            }

		///	if ( fullDuplex != isFullDuplex )
                setDuplexMode( fullDuplex );    

            if ( ready == true )
                startChip();

            if ( linkSpeed != 0 )
            {
                mediumType |= (fullDuplex == true)  ?
                              kIOMediumOptionFullDuplex :
                              kIOMediumOptionHalfDuplex;
            }

            medium = IONetworkMedium::getMediumWithType( mediumDict,
                                                         mediumType );

            setLinkStatus( kIONetworkLinkActive | kIONetworkLinkValid,
                           medium,
                           linkSpeed * 1000000 );

			IOLog( "UniNEnet::monitorLinkStatus - Link is up at %ld Mbps - %s Duplex\n\r",
						linkSpeed, fullDuplex ? "Full" : "Half" );

            linkStatusPrev = kLinkStatusUp;
        }
        else
        {
            if ( (linkStatusPrev == kLinkStatusUp)     ||
                 (linkStatusPrev == kLinkStatusUnknown) )
            {
                stopChip();

                medium = IONetworkMedium::getMediumWithType( mediumDict,
                                                             mediumType );

                setLinkStatus( kIONetworkLinkValid,
                               medium,
                               0 );
       
                if ( linkStatusPrev != kLinkStatusUnknown )
                {
                   IOLog( "Ethernet(UniN): Link is down.\n\r" );
                }

                txIntCnt = 0;

                if ( txCommandHead != txCommandTail )
                {
                    initTxRing();

					txCommandHead = READ_REGISTER( TxCompletion );
                    txCommandTail = txCommandHead;
                }
            }

            linkStatusPrev = kLinkStatusDown;
        }

        phyStatusPrev = phyStatus;
    }
	return;
}/* end monitorLinkStatus */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

IOReturn UniNEnet::getHardwareAddress(IOEthernetAddress *ea)
{
    UInt32      i;
    OSData	*macEntry;
    UInt8       *macAddress;
    UInt32      len;

    macEntry    = OSDynamicCast( OSData, nub->getProperty( "local-mac-address" ) );
    if ( macEntry == 0 )
    {
        return kIOReturnError;
    }

    macAddress  = (UInt8 *)macEntry->getBytesNoCopy();
    if ( macAddress == 0 )
    {
        return kIOReturnError;
    }

    len = macEntry->getLength();
    if ( len != 6 )
    {
        return kIOReturnError;
    }
   
    for (i = 0; i < sizeof(*ea); i++)   
    {
        ea->bytes[i] = macAddress[i];
    }
    return kIOReturnSuccess;
}/* end getHardwareAddress */


/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

#define ENET_CRCPOLY 0x04c11db7

static UInt32 crc416(UInt32 current, UInt16 nxtval )
{
    register UInt32 counter;
    register int highCRCBitSet, lowDataBitSet;

    /* Swap bytes */
    nxtval = ((nxtval & 0x00FF) << 8) | (nxtval >> 8);

    /* Compute bit-by-bit */
    for (counter = 0; counter != 16; ++counter)
    {   /* is high CRC bit set? */
      if ((current & 0x80000000) == 0)  
        highCRCBitSet = 0;
      else
        highCRCBitSet = 1;
        
      current = current << 1;
    
      if ((nxtval & 0x0001) == 0)
        lowDataBitSet = 0;
      else
    lowDataBitSet = 1;

      nxtval = nxtval >> 1;
    
      /* do the XOR */
      if (highCRCBitSet ^ lowDataBitSet)
        current = current ^ ENET_CRCPOLY;
    }
    return current;
}

/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

static UInt32 mace_crc(UInt16 *address)
{   
    register UInt32 newcrc;

    newcrc = crc416(0xffffffff, *address);  /* address bits 47 - 32 */
    newcrc = crc416(newcrc, address[1]);    /* address bits 31 - 16 */
    newcrc = crc416(newcrc, address[2]);    /* address bits 15 - 0  */

    return(newcrc);
}

/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

/*
 * Add requested mcast addr to UniN's hash table filter.  
 *  
 */
void UniNEnet::addToHashTableMask(UInt8 *addr)
{   
    UInt32   i,j;
    UInt32   crcBitIndex;
    UInt16   mask;

    j = mace_crc((UInt16 *)addr) & 0xFF; /* Big-endian alert! */
   
    for ( crcBitIndex = i = 0; i < 8; i++ )
    {
        crcBitIndex >>= 1;
        crcBitIndex  |= (j & 0x80);
        j           <<= 1;
    }

    crcBitIndex ^= 0xFF;
            
    if (hashTableUseCount[crcBitIndex]++)   
      return;           /* This bit is already set */
    mask = crcBitIndex % 16;
    mask = 1 << mask;
    hashTableMask[crcBitIndex/16] |= mask;
}

/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

void UniNEnet::resetHashTableMask()
{
    bzero(hashTableUseCount, sizeof(hashTableUseCount));
    bzero(hashTableMask, sizeof(hashTableMask));
}

/*-------------------------------------------------------------------------
 *
 *
 *
 *-------------------------------------------------------------------------*/

/*
 * Sync the adapter with the software copy of the multicast mask
 *  (logical address filter).
 */
void UniNEnet::updateHashTableMask()
{
    UInt32      i;

    rxMacConfigReg = READ_REGISTER( RxMACConfiguration );
	ELG( this, rxMacConfigReg, 'updH', "updateHashTableMask" );

	WRITE_REGISTER( RxMACConfiguration,
					rxMacConfigReg & ~(kRxMACConfiguration_Rx_Mac_Enable
								   |   kRxMACConfiguration_Hash_Filter_Enable) );
		/// Fix this to have a time limit around 3.2 ms
	while ( READ_REGISTER( RxMACConfiguration )	& (kRxMACConfiguration_Rx_Mac_Enable
												|  kRxMACConfiguration_Hash_Filter_Enable) )
      ;

    for ( i= 0; i < 16; i++ )
		WRITE_REGISTER( HashTable[ i ], hashTableMask[ 15 - i ] );

    rxMacConfigReg |= kRxMACConfiguration_Hash_Filter_Enable;
	WRITE_REGISTER( RxMACConfiguration, rxMacConfigReg );
}
