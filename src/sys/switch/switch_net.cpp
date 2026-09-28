/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

Networking. The first bring-up targets single player, which only uses
loopback addresses: UDP ports never open, so multiplayer is unavailable.
Address helpers follow src/sys/posix/posix_net.cpp.

===========================================================================
*/

#include "../../idlib/precompiled.h"

#include "switch_local.h"

/*
=============
Sys_StringToNetAdr

Understands "localhost" and dotted IPv4 addresses with an optional :port.
No DNS on Switch yet.
=============
*/
bool Sys_StringToNetAdr( const char *s, netadr_t *a, bool doDNSResolve ) {
	memset( a, 0, sizeof( *a ) );

	idStr host = s;
	int port = 0;
	const int colon = host.Find( ':' );
	if ( colon >= 0 ) {
		port = atoi( host.c_str() + colon + 1 );
		host.CapLength( colon );
	}

	if ( host.Icmp( "localhost" ) == 0 ) {
		a->type = NA_LOOPBACK;
		a->port = (unsigned short)port;
		return true;
	}

	int ip[4];
	char trailing;
	if ( sscanf( host.c_str(), "%d.%d.%d.%d%c", &ip[0], &ip[1], &ip[2], &ip[3], &trailing ) != 4 ) {
		return false;
	}
	for ( int i = 0; i < 4; i++ ) {
		if ( ip[i] < 0 || ip[i] > 255 ) {
			return false;
		}
		a->ip[i] = (unsigned char)ip[i];
	}
	a->type = NA_IP;
	a->port = (unsigned short)port;
	return true;
}

const char *Sys_NetAdrToString( const netadr_t a ) {
	static char s[64];

	if ( a.type == NA_LOOPBACK ) {
		if ( a.port ) {
			idStr::snPrintf( s, sizeof( s ), "localhost:%i", a.port );
		} else {
			idStr::snPrintf( s, sizeof( s ), "localhost" );
		}
	} else if ( a.type == NA_IP ) {
		idStr::snPrintf( s, sizeof( s ), "%i.%i.%i.%i:%i", a.ip[0], a.ip[1], a.ip[2], a.ip[3], a.port );
	} else {
		idStr::snPrintf( s, sizeof( s ), "bad address" );
	}
	return s;
}

bool Sys_IsLANAddress( const netadr_t adr ) {
	return adr.type == NA_LOOPBACK;
}

bool Sys_CompareNetAdrBase( const netadr_t a, const netadr_t b ) {
	if ( a.type != b.type ) {
		return false;
	}
	if ( a.type == NA_LOOPBACK ) {
		return true;
	}
	if ( a.type == NA_IP ) {
		return a.ip[0] == b.ip[0] && a.ip[1] == b.ip[1] && a.ip[2] == b.ip[2] && a.ip[3] == b.ip[3];
	}
	common->Printf( "Sys_CompareNetAdrBase: bad address type\n" );
	return false;
}

void Sys_InitNetworking( void ) {
	common->Printf( "Networking: multiplayer is not available on Switch yet\n" );
}

void Sys_ShutdownNetworking( void ) {
}

/*
==================
idPort

Never binds, so the async network layer treats the port as unavailable.
==================
*/
idPort::idPort() {
	netSocket = 0;
	memset( &bound_to, 0, sizeof( bound_to ) );
	packetsRead = bytesRead = packetsWritten = bytesWritten = 0;
}

idPort::~idPort() {
	Close();
}

bool idPort::InitForPort( int portNumber ) {
	return false;
}

void idPort::Close() {
	netSocket = 0;
	memset( &bound_to, 0, sizeof( bound_to ) );
}

bool idPort::GetPacket( netadr_t &from, void *data, int &size, int maxSize ) {
	return false;
}

bool idPort::GetPacketBlocking( netadr_t &from, void *data, int &size, int maxSize, int timeout ) {
	return false;
}

void idPort::SendPacket( const netadr_t to, const void *data, int size ) {
}
