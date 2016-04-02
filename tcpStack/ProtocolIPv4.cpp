//----------------------------------------------------------------------------
// Copyright( c ) 2015, Robert Kimball
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//----------------------------------------------------------------------------

#include <stdio.h>

#include "ProtocolMACEthernet.h"
#include "ProtocolARP.h"
#include "ProtocolIPv4.h"
#include "ProtocolICMP.h"
#include "ProtocolTCP.h"
#include "ProtocolUDP.h"
#include "Address.h"
#include "FCS.h"
#include "DataBuffer.h"
#include "NetworkInterface.h"
#include "Utility.h"

uint16_t ProtocolIPv4::PacketID;

static void* TxBuffer[ TX_BUFFER_COUNT ];
osQueue ProtocolIPv4::UnresolvedQueue( "IP", TX_BUFFER_COUNT, TxBuffer );
ProtocolIPv4::AddressInfo ProtocolIPv4::Address;

// Version - 4 bits
// Header Length - 4 bits
// Type of Service - 8 bits
// TotalLength - 16 bits
// Identification - 16 bits
// Flags - 3 bits
// FragmentOffset - 13 bits
// TTL - 8 bits
// Protocol - 8 bits
// HeaderChecksum - 16 bits

//============================================================================
//
//============================================================================

void ProtocolIPv4::Initialize()
{
   Address.DataValid = false;
   ProtocolTCP::Initialize();
   ProtocolICMP::Initialize();
}

//============================================================================
//
//============================================================================

bool ProtocolIPv4::IsLocal( const uint8_t* addr )
{
   bool rc;
   uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF};
   if( Address.DataValid )
   {
      rc = Address::Compare( addr, broadcast, ProtocolIPv4::AddressSize ) ||
            Address::Compare( addr, ProtocolIPv4::GetUnicastAddress(), ProtocolIPv4::AddressSize ) ||
            Address::Compare( addr, ProtocolIPv4::GetBroadcastAddress(), ProtocolIPv4::AddressSize )
         ;
   }
   else
   {
      rc = Address::Compare( addr, broadcast, ProtocolIPv4::AddressSize );
   }
   return rc;
}

//============================================================================
//
//============================================================================

void ProtocolIPv4::ProcessRx( DataBuffer* buffer, const uint8_t* hardwareAddress )
{
   uint8_t headerLength;
   uint8_t protocol;
   uint8_t* sourceIP;
   uint8_t* targetIP;
   uint8_t* packet = buffer->Packet;
   uint16_t length = buffer->Length;
   uint16_t dataLength;

   headerLength = (packet[ 0 ] & 0x0F) * 4;
   dataLength = Unpack16( packet, 2 );
   protocol = packet[ 9 ];
   sourceIP = &packet[ 12 ];
   targetIP = &packet[ 16 ];

   if( IsLocal( targetIP ) )
   {
      buffer->Packet += headerLength;
      dataLength -= headerLength;
      buffer->Length = dataLength;

      switch( protocol )
      {
      case 0x01:  // ICMP
         ProtocolICMP::ProcessRx( buffer, sourceIP );
         break;
      case 0x02:  // IGMP
         break;
      case 0x06:  // TCP
         ProtocolTCP::ProcessRx( buffer, sourceIP, targetIP );
         break;
      case 0x11:  // UDP
         ProtocolUDP::ProcessRx( buffer, sourceIP, targetIP );
         break;
      default:
         printf( "Unsupported IP Protocol 0x%02X\n", protocol );
         break;
      }
   }
}

//============================================================================
//
//============================================================================

DataBuffer* ProtocolIPv4::GetTxBuffer()
{
   DataBuffer*   buffer;

   buffer = ProtocolMACEthernet::GetTxBuffer();
   if( buffer != 0 )
   {
      buffer->Packet += IP_HEADER_SIZE;
      buffer->Remainder -= IP_HEADER_SIZE;
   }
   
   return buffer;
}

//============================================================================
//
//============================================================================

void ProtocolIPv4::Transmit( DataBuffer* buffer, uint8_t protocol, const uint8_t* targetIP, const uint8_t* sourceIP )
{
   uint16_t checksum;
   uint8_t* targetMAC;
   uint8_t* packet;
   
   buffer->Packet -= IP_HEADER_SIZE;
   buffer->Length += IP_HEADER_SIZE;
   packet = buffer->Packet;

   packet[ 0 ] = 0x45;     // Version and HeaderSize
   packet[ 1 ] = 0;        // ToS
   Pack16( packet, 2, buffer->Length );

   PacketID++;
   Pack16( packet, 4, PacketID );
   packet[ 6 ] = 0;        // Flags & FragmentOffset
   packet[ 7 ] = 0;        // rest of FragmentOffset

   packet[  8 ] = 32;      // TTL
   packet[  9 ] = protocol;
   Pack16( packet, 10, 0 ); // checksum placeholder
   PackBytes( packet, 12, sourceIP, 4 );
   PackBytes( packet, 16, targetIP, 4 );

   checksum = FCS::Checksum( packet, 20 );
   Pack16( packet, 10, checksum );

   targetMAC = ProtocolARP::Protocol2Hardware( targetIP );
   if( targetMAC != 0 )
   {
      ProtocolMACEthernet::Transmit( buffer, targetMAC, 0x0800 );
   }
   else
   {
      // Could not find MAC address, ARP for it
      UnresolvedQueue.Put( buffer );
   }
}

//============================================================================
//
//============================================================================

void ProtocolIPv4::Retransmit( DataBuffer* buffer )
{
   ProtocolMACEthernet::Retransmit( buffer );
}

//============================================================================
//
//============================================================================

void ProtocolIPv4::Retry()
{
   int count;
   DataBuffer* buffer;
   uint8_t* targetMAC;

   count = UnresolvedQueue.GetCount();
   for( int i=0; i<count; i++ )
   {
      buffer = (DataBuffer*)UnresolvedQueue.Get();

      targetMAC = ProtocolARP::Protocol2Hardware( &buffer->Packet[ 16 ] );
      if( targetMAC != 0 )
      {
         ProtocolMACEthernet::Transmit( buffer, targetMAC, 0x0800 );
      }
      else
      {
         printf( "Still could not find MAC for IP\n" );
         UnresolvedQueue.Put( buffer );
      }
   }
}

//============================================================================
//
//============================================================================

void ProtocolIPv4::FreeTxBuffer( DataBuffer* buffer )
{
   ProtocolMACEthernet::FreeTxBuffer( buffer );
}

//============================================================================
//
//============================================================================

void ProtocolIPv4::FreeRxBuffer( DataBuffer* buffer )
{
   ProtocolMACEthernet::FreeRxBuffer( buffer );
}

//============================================================================
//
//============================================================================

void ProtocolIPv4::Show( osPrintfInterface* out )
{
   out->Printf( "Network Configuration\n" );
   out->Printf( "Ethernet Unicast MAC Address: %s\n", macaddrtoa( ProtocolMACEthernet::GetUnicastAddress() ) );
   out->Printf( "Ethernet Broadcast MAC Address: %s\n", macaddrtoa( ProtocolMACEthernet::GetBroadcastAddress() ) );

   out->Printf( "IPv4 Configuration\n" );
   out->Printf( "   Address:            %s\n", ipv4toa( Address.Address ) );
   out->Printf( "   Subnet Mask:        %s\n", ipv4toa( Address.SubnetMask ) );
   out->Printf( "   Gateway:            %s\n", ipv4toa( Address.Gateway ) );
   out->Printf( "   Domain Name Server: %s\n", ipv4toa( Address.DomainNameServer ) );
   out->Printf( "   Broadcast Address:  %s\n", ipv4toa( Address.BroadcastAddress ) );
   out->Printf( "   Address Lease Time: %d seconds\n", Address.IpAddressLeaseTime );
   out->Printf( "   RenewTime:          %d seconds\n", Address.RenewTime );
   out->Printf( "   RebindTime:         %d seconds\n", Address.RebindTime );
}

//============================================================================
//
//============================================================================

uint8_t* ProtocolIPv4::GetUnicastAddress()
{
   return Address.Address;
}

//============================================================================
//
//============================================================================

uint8_t* ProtocolIPv4::GetBroadcastAddress()
{
   return Address.BroadcastAddress;
}

//============================================================================
//
//============================================================================

uint8_t* ProtocolIPv4::GetGatewayAddress()
{
   return Address.Gateway;
}

//============================================================================
//
//============================================================================

uint8_t* ProtocolIPv4::GetSubnetMask()
{
   return Address.SubnetMask;
}

//============================================================================
//
//============================================================================

void ProtocolIPv4::SetAddressInfo( const AddressInfo& info )
{
   Address = info;
}