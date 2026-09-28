/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

Process entry, time, files, events and misc system services. Mirrors the
POSIX implementation (src/sys/posix) where libnx/newlib allow it; POSIX
signals, mmap, dlopen and terminals do not exist on Horizon.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../sys_local.h"

// struct in_addr for __nxlink_host (switch.h -> nxlink.h). idlib defines an empty
// _LITTLE_ENDIAN marker, while newlib's <machine/endian.h> compares it numerically.
#pragma push_macro( "_LITTLE_ENDIAN" )
#undef _LITTLE_ENDIAN
#include <netinet/in.h>
#pragma pop_macro( "_LITTLE_ENDIAN" )

// idlib and libnx both define BIT(); they compute the same value, so let libnx own it here.
#undef BIT
#include <switch.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>

#include "switch_local.h"

#define MAX_OSPATH			256

// newlib hides fileno() in strict C++ mode
extern "C" int fileno( FILE *stream );

// The engine runs on its own thread so it gets a desktop-sized stack; the
// Windows build links with /STACK:16MB and some recursive code (PVS flood,
// AAS, script compiler) is tuned for that.
static const size_t	ENGINE_THREAD_STACK_SIZE = 16 * 1024 * 1024;

static int			s_argc;
static char **		s_argv;
static bool			s_nxlinkActive = false;

/*
============================================================================
EXIT / ERROR
============================================================================
*/

static void Switch_WriteFatalFile( const char *msg ) {
	FILE *f = fopen( SWITCH_BASE_PATH "/openprey_error.txt", "wb" );
	if ( f ) {
		fputs( msg, f );
		fputc( '\n', f );
		fclose( f );
	}
}

static pthread_t		s_mainThread;
static volatile int	s_exitCode = EXIT_SUCCESS;

/*
================
Switch_Exit

Quit and error paths run on the engine thread, but the process must leave from
the main thread. libnx hands control back to the homebrew loader on whichever
thread calls exit(), so exiting from the engine thread left hbmenu running on
the engine thread's heap-allocated stack while the real main thread stayed
blocked in pthread_join. The system then crashed when the process closed
("Closing software"). The engine thread now just ends, and main() returns.
================
*/
static void Switch_Exit( int ret ) {
	Switch_StopAsyncThread();
	s_exitCode = ret;
	if ( pthread_equal( pthread_self(), s_mainThread ) ) {
		exit( ret );
	}
	pthread_exit( NULL );
}

void Sys_Quit( void ) {
	Switch_Exit( EXIT_SUCCESS );
}

void Sys_Error( const char *error, ... ) {
	char text[4096];
	va_list argptr;

	va_start( argptr, error );
	idStr::vsnPrintf( text, sizeof( text ), error, argptr );
	va_end( argptr );

	Sys_Printf( "Sys_Error: %s\n", text );
	Switch_WriteFatalFile( text );
	Switch_Exit( EXIT_FAILURE );
}

/*
================
Sys_SetFatalError

common->FatalError calls this before shutting every subsystem down, and only
calls Sys_Error afterwards. Write the message now: a partially initialized
engine can crash during that shutdown and the message would be lost.
================
*/
void Sys_SetFatalError( const char *error ) {
	Switch_WriteFatalFile( error );
}

/*
============================================================================
CRASH HANDLER

libnx calls __libnx_exception_handler for CPU exceptions (bad memory
access, undefined instruction, ...) when an exception stack is provided.
It writes SWITCH_BASE_PATH/openprey_crash.txt with the faulting PC, LR and
a frame-pointer backtrace. Addresses are also given as offsets into the
executable, so they can be resolved against OpenPrey-client_arm64.elf:

	aarch64-none-elf-addr2line -f -C -e OpenPrey-client_arm64.elf <offset>...
============================================================================
*/

extern "C" {
	u32		__nx_exception_ignoredebug = 1;
	alignas( 16 ) u8 __nx_exception_stack[0x8000];
	u64		__nx_exception_stack_size = sizeof( __nx_exception_stack );
	void	__libnx_exception_handler( ThreadExceptionDump *ctx );
}

static char s_crashText[8192];

static bool Switch_IsReadable( u64 addr ) {
	MemoryInfo info;
	u32 pageInfo;
	if ( R_FAILED( svcQueryMemory( &info, &pageInfo, addr ) ) ) {
		return false;
	}
	return ( info.perm & Perm_R ) != 0 && info.type != MemType_Unmapped;
}

void __libnx_exception_handler( ThreadExceptionDump *ctx ) {
	// the executable's code region, whose start is the ELF's address 0
	MemoryInfo codeInfo = {};
	u32 pageInfo;
	svcQueryMemory( &codeInfo, &pageInfo, (u64)&__libnx_exception_handler );
	const u64 codeStart = codeInfo.addr;
	const u64 codeEnd = codeInfo.addr + codeInfo.size;

	size_t len = 0;
	auto append = [&]( const char *fmt, ... ) {
		if ( len >= sizeof( s_crashText ) ) {
			return;
		}
		va_list ap;
		va_start( ap, fmt );
		const int n = idStr::vsnPrintf( s_crashText + len, (int)( sizeof( s_crashText ) - len ), fmt, ap );
		va_end( ap );
		if ( n > 0 ) {
			len += (size_t)n;
		}
	};
	auto appendAddress = [&]( const char *label, u64 addr ) {
		if ( addr >= codeStart && addr < codeEnd ) {
			append( "%-4s 0x%016llx  (elf offset 0x%llx)\n", label, (unsigned long long)addr, (unsigned long long)( addr - codeStart ) );
		} else {
			append( "%-4s 0x%016llx\n", label, (unsigned long long)addr );
		}
	};

	append( "OpenPrey crash\n" );
	append( "exception 0x%x  far 0x%016llx  esr 0x%x\n", ctx->error_desc, (unsigned long long)ctx->far.x, ctx->esr );
	append( "code 0x%016llx..0x%016llx\n", (unsigned long long)codeStart, (unsigned long long)codeEnd );
	appendAddress( "pc", ctx->pc.x );
	appendAddress( "lr", ctx->lr.x );
	append( "sp   0x%016llx\nfp   0x%016llx\n", (unsigned long long)ctx->sp.x, (unsigned long long)ctx->fp.x );

	append( "backtrace:\n" );
	u64 fp = ctx->fp.x;
	for ( int depth = 0; depth < 64 && fp && ( fp & 7 ) == 0; depth++ ) {
		if ( !Switch_IsReadable( fp ) || !Switch_IsReadable( fp + 8 ) ) {
			break;
		}
		const u64 nextFp = ( (const u64 *)fp )[0];
		const u64 ret = ( (const u64 *)fp )[1];
		char label[8];
		idStr::snPrintf( label, sizeof( label ), "#%d", depth );
		appendAddress( label, ret );
		if ( nextFp <= fp ) {
			break;
		}
		fp = nextFp;
	}

	append( "registers:\n" );
	for ( int i = 0; i < 29; i++ ) {
		append( "x%-2d 0x%016llx%s", i, (unsigned long long)ctx->cpu_gprs[i].x, ( i % 3 == 2 ) ? "\n" : "  " );
	}
	append( "\n" );

	FILE *f = fopen( SWITCH_BASE_PATH "/openprey_crash.txt", "wb" );
	if ( f ) {
		fwrite( s_crashText, 1, len < sizeof( s_crashText ) ? len : sizeof( s_crashText ) - 1, f );
		fclose( f );
	}

	svcExitProcess();
}

/*
============================================================================
OUTPUT

stdout goes to nxlink when the app was started with `nxlink -s`; otherwise
it is discarded. Use logFile (enabled by default, see main) for field logs.
============================================================================
*/

void Sys_Printf( const char *msg, ... ) {
	va_list argptr;
	va_start( argptr, msg );
	vprintf( msg, argptr );
	va_end( argptr );
}

void Sys_DebugPrintf( const char *fmt, ... ) {
	va_list argptr;
	va_start( argptr, fmt );
	vprintf( fmt, argptr );
	va_end( argptr );
}

void Sys_DebugVPrintf( const char *fmt, va_list arg ) {
	vprintf( fmt, arg );
}

void Sys_ShowConsole( int visLevel, bool quitOnClose ) {
}

/*
============================================================================
TIME
============================================================================
*/

static u64 s_timeBaseTicks = 0;

int Sys_Milliseconds( void ) {
	const u64 now = armGetSystemTick();
	if ( !s_timeBaseTicks ) {
		s_timeBaseTicks = now;
	}
	return (int)( armTicksToNs( now - s_timeBaseTicks ) / 1000000ULL );
}

double Sys_GetClockTicks( void ) {
	return (double)armGetSystemTick();
}

double Sys_ClockTicksPerSecond( void ) {
	return (double)armGetSystemTickFreq();
}

void Sys_Sleep( int msec ) {
	if ( msec <= 0 ) {
		svcSleepThread( 0 );	// yield
		return;
	}
	svcSleepThread( (s64)msec * 1000000LL );
}

/*
============================================================================
CPU / FPU / MEMORY
============================================================================
*/

cpuid_t Sys_GetProcessorId( void ) {
	return CPUID_GENERIC;
}

const char *Sys_GetProcessorString( void ) {
	return "ARM Cortex-A57 (Nintendo Switch)";
}

bool Sys_FPU_StackIsEmpty( void ) {
	return true;
}

void Sys_FPU_ClearStack( void ) {
}

const char *Sys_FPU_GetState( void ) {
	return "";
}

void Sys_FPU_EnableExceptions( int exceptions ) {
}

void Sys_FPU_SetPrecision( int precision ) {
}

void Sys_FPU_SetRounding( int rounding ) {
}

void Sys_FPU_SetFTZ( bool enable ) {
}

void Sys_FPU_SetDAZ( bool enable ) {
}

int Sys_GetSystemRam( void ) {
	u64 total = 0;
	if ( R_FAILED( svcGetInfo( &total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0 ) ) ) {
		return 512;
	}
	return (int)( total / ( 1024 * 1024 ) );
}

int Sys_GetVideoRam( void ) {
	// Unified memory: the GPU draws from the same pool as the CPU.
	return Sys_GetSystemRam();
}

void Sys_GetCurrentMemoryStatus( sysMemoryStats_t &stats ) {
	memset( &stats, 0, sizeof( stats ) );
	u64 total = 0, used = 0;
	svcGetInfo( &total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0 );
	svcGetInfo( &used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0 );
	stats.totalPhysical = (int)( total / 1024 );
	stats.availPhysical = (int)( ( total - used ) / 1024 );
	stats.memoryLoad = total ? (int)( ( used * 100 ) / total ) : 0;
}

void Sys_GetExeLaunchMemoryStatus( sysMemoryStats_t &stats ) {
	Sys_GetCurrentMemoryStatus( stats );
}

bool Sys_LockMemory( void *ptr, int bytes ) {
	return true;
}

bool Sys_UnlockMemory( void *ptr, int bytes ) {
	return true;
}

void Sys_SetPhysicalWorkMemory( int minBytes, int maxBytes ) {
}

void Sys_FlushCacheMemory( void *base, int bytes ) {
}

void Sys_ShutdownSymbols( void ) {
}

/*
================
Sys_SetLoadingBoost

Raises the CPU clock while loading with the system FastLoad boost mode (the
one retail games use on loading screens; it also drops the GPU to its
minimum, which a loading screen does not need). Calls nest; the boost ends
when every enable has been matched. Called around common->Init and
idSessionLocal::ExecuteMapChange.
================
*/
static int s_loadingBoostDepth = 0;

void Sys_SetLoadingBoost( bool enable ) {
	if ( enable ) {
		if ( s_loadingBoostDepth++ == 0 ) {
			appletSetCpuBoostMode( ApmCpuBoostMode_FastLoad );
		}
	} else if ( s_loadingBoostDepth > 0 && --s_loadingBoostDepth == 0 ) {
		appletSetCpuBoostMode( ApmCpuBoostMode_Normal );
	}
}

/*
============================================================================
FILES AND PATHS
============================================================================
*/

const char *Sys_DefaultBasePath( void ) {
	return SWITCH_BASE_PATH;
}

const char *Sys_DefaultSavePath( void ) {
	return SWITCH_BASE_PATH;
}

const char *Sys_DefaultCDPath( void ) {
	return SWITCH_BASE_PATH;
}

const char *Sys_EXEPath( void ) {
	static char exePath[MAX_OSPATH];
	if ( s_argc > 0 && s_argv && s_argv[0] ) {
		idStr::Copynz( exePath, s_argv[0], sizeof( exePath ) );
	} else {
		idStr::Copynz( exePath, SWITCH_BASE_PATH "/OpenPrey.nro", sizeof( exePath ) );
	}
	return exePath;
}

void Sys_Mkdir( const char *path ) {
	mkdir( path, 0777 );
}

ID_TIME_T Sys_FileTimeStamp( FILE *fp ) {
	struct stat st;
	if ( fstat( fileno( fp ), &st ) != 0 ) {
		return 0;
	}
	return st.st_mtime;
}

int Sys_GetDriveFreeSpace( const char *path ) {
	struct statvfs st;
	if ( statvfs( path, &st ) != 0 ) {
		return 1000 * 1024;
	}
	return (int)( ( (u64)st.f_bavail * st.f_frsize ) / ( 1024 * 1024 ) );
}

int Sys_ListFiles( const char *directory, const char *extension, idStrList &list ) {
	struct dirent *d;
	DIR *fdir;
	bool dironly = false;
	char search[MAX_OSPATH];
	struct stat st;
	bool debug;

	list.Clear();

	debug = cvarSystem->GetCVarBool( "fs_debug" );

	if ( !extension ) {
		extension = "";
	}

	// passing a slash as extension will find directories
	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		dironly = true;
	}

	if ( ( fdir = opendir( directory ) ) == NULL ) {
		if ( debug ) {
			common->Printf( "Sys_ListFiles: opendir %s failed\n", directory );
		}
		return -1;
	}

	while ( ( d = readdir( fdir ) ) != NULL ) {
		idStr::snPrintf( search, sizeof( search ), "%s/%s", directory, d->d_name );
		if ( stat( search, &st ) == -1 ) {
			continue;
		}
		if ( !dironly ) {
			idStr look( search );
			idStr ext;
			look.ExtractFileExtension( ext );
			if ( extension[0] != '\0' && ext.Icmp( &extension[1] ) != 0 ) {
				continue;
			}
		}
		if ( ( dironly && !S_ISDIR( st.st_mode ) ) || ( !dironly && S_ISDIR( st.st_mode ) ) ) {
			continue;
		}
		list.Append( d->d_name );
	}

	closedir( fdir );

	if ( debug ) {
		common->Printf( "Sys_ListFiles: %d entries in %s\n", list.Num(), directory );
	}

	return list.Num();
}

/*
============================================================================
GAME MODULE

The game is linked into the executable as a single object that only exports
GetGameAPI (basepy/meson.build), so it keeps the private globals a DLL has.
Sys_DLL_Load hands out a fake handle for it.
============================================================================
*/

static const intptr_t SWITCH_GAME_MODULE_HANDLE = 1;

intptr_t Sys_DLL_Load( const char *dllName ) {
	idStr name = dllName;
	name.StripPath();
	if ( idStr::Icmpn( name.c_str(), "game", 4 ) == 0 ) {
		return SWITCH_GAME_MODULE_HANDLE;
	}
	Sys_Printf( "Sys_DLL_Load: no dynamic modules on Switch ('%s')\n", dllName );
	return 0;
}

void *Sys_DLL_GetProcAddress( intptr_t dllHandle, const char *procName ) {
	if ( dllHandle == SWITCH_GAME_MODULE_HANDLE && idStr::Cmp( procName, "GetGameAPI" ) == 0 ) {
		return (void *)GetGameAPI;
	}
	return NULL;
}

void Sys_DLL_Unload( intptr_t dllHandle ) {
}

/*
============================================================================
EVENT LOOP
============================================================================
*/

#define	MAX_QUED_EVENTS		256
#define	MASK_QUED_EVENTS	( MAX_QUED_EVENTS - 1 )

static sysEvent_t	eventQue[MAX_QUED_EVENTS];
static int			eventHead, eventTail;

/*
================
Switch_QueEvent

ptr should either be null, or point to a block of data that can be freed later
================
*/
void Switch_QueEvent( sysEventType_t type, int value, int value2, int ptrLength, void *ptr ) {
	sysEvent_t *ev = &eventQue[eventHead & MASK_QUED_EVENTS];
	if ( eventHead - eventTail >= MAX_QUED_EVENTS ) {
		common->Printf( "Switch_QueEvent: overflow\n" );
		// we are discarding an event, but don't leak memory
		if ( ev->evPtr ) {
			Mem_Free( ev->evPtr );
			ev->evPtr = NULL;
		}
		eventTail++;
	}

	eventHead++;

	ev->evType = type;
	ev->evValue = value;
	ev->evValue2 = value2;
	ev->evPtrLength = ptrLength;
	ev->evPtr = ptr;
}

sysEvent_t Sys_GetEvent( void ) {
	static sysEvent_t ev;

	if ( eventHead > eventTail ) {
		eventTail++;
		return eventQue[( eventTail - 1 ) & MASK_QUED_EVENTS];
	}

	memset( &ev, 0, sizeof( ev ) );
	return ev;
}

void Sys_ClearEvents( void ) {
	eventHead = eventTail = 0;
}

/*
================
Sys_GenerateEvents

Called during frame loops and pacifier updates. Also services the applet
message loop: returning to HOME and closing the app from there must quit.
================
*/
void Sys_GenerateEvents( void ) {
	static bool quitQueued = false;
	if ( !appletMainLoop() ) {
		if ( !quitQueued ) {
			quitQueued = true;
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
		}
		return;
	}
	Switch_PollInput();
}

/*
============================================================================
MISC
============================================================================
*/

void Sys_Init( void ) {
}

void Sys_Shutdown( void ) {
}

bool Sys_AlreadyRunning( void ) {
	return false;
}

char *Sys_GetClipboardData( void ) {
	return NULL;
}

void Sys_SetClipboardData( const char *string ) {
}

void Sys_DoPreferences( void ) {
}

void idSysLocal::OpenURL( const char *url, bool quit ) {
	common->Printf( "OpenURL is not supported on Switch: %s\n", url );
	if ( quit ) {
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
	}
}

void idSysLocal::StartProcess( const char *exePath, bool quit ) {
	common->Printf( "StartProcess is not supported on Switch: %s\n", exePath );
	if ( quit ) {
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
	}
}

/*
============================================================================
ENTRY
============================================================================
*/

// Prepended to the launch arguments; anything passed by the launcher comes
// after them and wins. logFile keeps a log on the SD card for field testing
// (<fs_savepath>/<fs_game>/logs/openprey.log, as in the desktop debug loop).
static const char *s_defaultArgs[] = {
	"+set", "logFile", "2",
	"+set", "logFileName", "logs/openprey.log",
};

static void *Switch_EngineThread( void * ) {
	idList<const char *> args;
	for ( size_t i = 0; i < sizeof( s_defaultArgs ) / sizeof( s_defaultArgs[0] ); i++ ) {
		args.Append( s_defaultArgs[i] );
	}
	for ( int i = 1; i < s_argc; i++ ) {
		args.Append( s_argv[i] );
	}

	Sys_SetLoadingBoost( true );
	common->Init( args.Num(), args.Ptr(), NULL );
	Sys_SetLoadingBoost( false );
	Switch_ApplyDefaultBinds();

	common->Printf( "%d MB System Memory\n", Sys_GetSystemRam() );
	Switch_StartAsyncThread();

	while ( 1 ) {
		common->Frame();
	}
	return NULL;
}

int main( int argc, char **argv ) {
	s_argc = argc;
	s_argv = argv;
	s_mainThread = pthread_self();

	// stdout over the network when launched with `nxlink -s`
	if ( __nxlink_host.s_addr != 0 && R_SUCCEEDED( socketInitializeDefault() ) ) {
		s_nxlinkActive = nxlinkStdio() >= 0;
		if ( !s_nxlinkActive ) {
			socketExit();
		}
	}

	mkdir( SWITCH_BASE_PATH, 0777 );
	chdir( SWITCH_BASE_PATH );

	// set the time base
	Sys_Milliseconds();
	Switch_InitThreads();

	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, ENGINE_THREAD_STACK_SIZE );
	pthread_t engineThread;
	if ( pthread_create( &engineThread, &attr, Switch_EngineThread, NULL ) != 0 ) {
		Switch_WriteFatalFile( "could not create the engine thread" );
		return EXIT_FAILURE;
	}
	pthread_attr_destroy( &attr );

	// The engine thread ends through Sys_Quit/Sys_Error -> Switch_Exit; the
	// process then leaves normally from here, on the main thread.
	pthread_join( engineThread, NULL );

	if ( s_nxlinkActive ) {
		socketExit();
		s_nxlinkActive = false;
	}
	return s_exitCode;
}
