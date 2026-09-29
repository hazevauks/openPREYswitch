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
void FS_StopBackgroundDownloadThread( void );

static void Switch_Exit( int ret ) {
	FS_StopBackgroundDownloadThread();
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
============================================================================
CLOCKS

Performance profile (r_switchPerfProfile). Handheld mode runs applications on
PerformanceConfiguration 0x00020003 (CPU 1020 / GPU 307.2 / EMC 1331.2 MHz);
hardware test there: GPU 99% busy at 20-25 fps. Profiles 1-3 pick stronger
official handheld configurations (switchbrew PTM_services). Docked mode
(PerformanceMode Boost) already defaults to GPU 768 MHz and is left alone.

Profile 3 also raises the CPU to 1224 MHz (the docked "CPU boost" rate, well
under the 1785 MHz the system uses on loading screens) through clkrst (pcv
before 8.0.0), the service sys-clk uses. The system puts the CPU back to the
configuration's rate whenever it re-applies a configuration (loading boost,
dock change, sleep), so the rate is checked once a second and after every
loading boost. Everything goes back to the defaults on exit.

Loading boost (Sys_SetLoadingBoost): the system FastLoad boost mode, the one
retail games use on loading screens. It raises the CPU to 1785 MHz and drops
the GPU to its minimum, which a loading screen does not need. Calls nest;
the boost ends when every enable has been matched. Used around common->Init
and idSessionLocal::ExecuteMapChange.
============================================================================
*/

static idCVar r_switchPerfProfile( "r_switchPerfProfile", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
	"clock profile (official handheld configurations): 0 = system default (GPU 307 MHz), 1 = GPU 384 MHz, 2 = GPU 460.8 MHz, 3 = GPU 460.8 MHz + memory 1600 MHz + CPU 1224 MHz", 0, 3 );

typedef struct {
	u32		config;		// handheld PerformanceConfiguration
	u32		cpuHz;		// CPU rate to hold, 0 = the configuration's
} switchPerfProfile_t;

static const u32 SWITCH_HANDHELD_DEFAULT_CONFIG	= 0x00020003;	// Cpu1020MhzGpu307MhzEmc1331Mhz
static const u32 SWITCH_DEFAULT_CPU_HZ			= 1020000000;
static const switchPerfProfile_t s_perfProfiles[] = {
	{ SWITCH_HANDHELD_DEFAULT_CONFIG,	0 },
	{ 0x00020004,						0 },			// Cpu1020MhzGpu384MhzEmc1331Mhz
	{ 0x92220008,						0 },			// Cpu1020MhzGpu460MhzEmc1331Mhz
	{ 0x92220007,						1224000000 },	// Cpu1020MhzGpu460MhzEmc1600Mhz, CPU raised to 1224 MHz
};
static const int NUM_PERF_PROFILES = sizeof( s_perfProfiles ) / sizeof( s_perfProfiles[0] );

static int		s_loadingBoostDepth = 0;
static int		s_appliedPerfProfile = -1;
static bool		s_perfConfigChanged = false;	// handheld configuration differs from the default
static int		s_lastCpuCheckTime = 0;

// CPU clock through clkrst (8.0.0+) or pcv
static bool				s_cpuClockOpen = false;
static bool				s_cpuClockUsesClkrst = false;
static ClkrstSession	s_cpuClockSession;
static bool				s_cpuClockChanged = false;	// the CPU was moved off the configuration's rate

static bool Switch_OpenCpuClock( void ) {
	if ( s_cpuClockOpen ) {
		return true;
	}
	if ( hosversionAtLeast( 8, 0, 0 ) ) {
		if ( R_FAILED( clkrstInitialize() ) ) {
			return false;
		}
		if ( R_FAILED( clkrstOpenSession( &s_cpuClockSession, PcvModuleId_CpuBus, 3 ) ) ) {
			clkrstExit();
			return false;
		}
		s_cpuClockUsesClkrst = true;
	} else {
		if ( R_FAILED( pcvInitialize() ) ) {
			return false;
		}
		s_cpuClockUsesClkrst = false;
	}
	s_cpuClockOpen = true;
	return true;
}

static void Switch_CloseCpuClock( void ) {
	if ( !s_cpuClockOpen ) {
		return;
	}
	if ( s_cpuClockUsesClkrst ) {
		clkrstCloseSession( &s_cpuClockSession );
		clkrstExit();
	} else {
		pcvExit();
	}
	s_cpuClockOpen = false;
}

static u32 Switch_GetCpuHz( void ) {
	u32 hz = 0;
	if ( s_cpuClockUsesClkrst ) {
		clkrstGetClockRate( &s_cpuClockSession, &hz );
	} else {
		pcvGetClockRate( PcvModule_CpuBus, &hz );
	}
	return hz;
}

static Result Switch_SetCpuHz( u32 hz ) {
	return s_cpuClockUsesClkrst ? clkrstSetClockRate( &s_cpuClockSession, hz ) : pcvSetClockRate( PcvModule_CpuBus, hz );
}

/*
================
Switch_EnforceCpuClock

Holds the profile's CPU rate. Not while the loading boost runs (it wants 1785 MHz).
================
*/
static void Switch_EnforceCpuClock( bool log ) {
	if ( s_appliedPerfProfile < 0 || s_loadingBoostDepth > 0 ) {
		return;
	}
	const u32 wanted = s_perfProfiles[s_appliedPerfProfile].cpuHz;
	if ( wanted == 0 ) {
		if ( s_cpuClockChanged && s_cpuClockOpen ) {
			// profile lowered from 3: hand the CPU back to the configuration's rate
			Switch_SetCpuHz( SWITCH_DEFAULT_CPU_HZ );
			s_cpuClockChanged = false;
		}
		return;
	}
	if ( !Switch_OpenCpuClock() ) {
		if ( log ) {
			common->Printf( "CPU clock: clkrst/pcv unavailable, the CPU stays at the configuration's rate\n" );
		}
		return;
	}
	const u32 current = Switch_GetCpuHz();
	if ( current == wanted ) {
		return;
	}
	const Result rc = Switch_SetCpuHz( wanted );
	s_cpuClockChanged = s_cpuClockChanged || R_SUCCEEDED( rc );
	if ( log ) {
		common->Printf( "CPU clock %u -> %u MHz: %s\n", current / 1000000, wanted / 1000000,
			R_SUCCEEDED( rc ) ? "set" : va( "failed (0x%X)", rc ) );
	}
}

void Sys_SetLoadingBoost( bool enable ) {
	if ( enable ) {
		if ( s_loadingBoostDepth++ == 0 ) {
			appletSetCpuBoostMode( ApmCpuBoostMode_FastLoad );
		}
	} else if ( s_loadingBoostDepth > 0 && --s_loadingBoostDepth == 0 ) {
		appletSetCpuBoostMode( ApmCpuBoostMode_Normal );
		Switch_EnforceCpuClock( false );
	}
}

void Switch_ApplyPerformanceProfile( void ) {
	const int profile = idMath::ClampInt( 0, NUM_PERF_PROFILES - 1, r_switchPerfProfile.GetInteger() );
	s_appliedPerfProfile = profile;
	const Result rc = apmSetPerformanceConfiguration( ApmPerformanceMode_Normal, s_perfProfiles[profile].config );
	s_perfConfigChanged = ( profile != 0 ) && R_SUCCEEDED( rc );

	// A new configuration for the current mode is not always applied right away
	// (in-game changes needed a restart). Cycling the CPU boost mode makes the
	// system re-apply the mode's configuration, as it does after loading screens.
	if ( R_SUCCEEDED( rc ) && s_loadingBoostDepth == 0 ) {
		appletSetCpuBoostMode( ApmCpuBoostMode_FastLoad );
		appletSetCpuBoostMode( ApmCpuBoostMode_Normal );
	}

	u32 active = 0;
	apmGetPerformanceConfiguration( ApmPerformanceMode_Normal, &active );
	common->Printf( "Switch clock profile %d (0x%08X): %s, active handheld configuration 0x%08X\n",
		profile, s_perfProfiles[profile].config, R_SUCCEEDED( rc ) ? "set" : va( "failed (0x%X)", rc ), active );

	Switch_EnforceCpuClock( true );
	s_lastCpuCheckTime = Sys_Milliseconds();
}

void Switch_CheckPerformanceProfile( void ) {
	// compare with the value applied instead of relying on the modified flag
	if ( idMath::ClampInt( 0, NUM_PERF_PROFILES - 1, r_switchPerfProfile.GetInteger() ) != s_appliedPerfProfile ) {
		Switch_ApplyPerformanceProfile();
		return;
	}
	const int now = Sys_Milliseconds();
	if ( now - s_lastCpuCheckTime >= 1000 ) {
		s_lastCpuCheckTime = now;
		Switch_EnforceCpuClock( false );
	}
}

void Switch_RestorePerformanceProfile( void ) {
	if ( s_cpuClockChanged && s_cpuClockOpen ) {
		Switch_SetCpuHz( SWITCH_DEFAULT_CPU_HZ );
		s_cpuClockChanged = false;
	}
	Switch_CloseCpuClock();
	if ( s_perfConfigChanged ) {
		apmSetPerformanceConfiguration( ApmPerformanceMode_Normal, SWITCH_HANDHELD_DEFAULT_CONFIG );
		s_perfConfigChanged = false;
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
	Switch_CheckPerformanceProfile();
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

/*
================
Switch_ReportHitch

Logs frames slower than com_logHitches ms with the file system work done in
them, to tell stutters caused by SD card reads during play from GPU/CPU spikes
(e.g. shader compiles). The file system counters run while fs_profileLoads is on.
================
*/
static idCVar com_logHitches( "com_logHitches", "100", CVAR_SYSTEM | CVAR_INTEGER, "log frames slower than this many ms, with the file system work done in them (0 = off)", 0, 10000 );

void FS_GetProfileTotals( int &opens, double &openSec, long long &readBytes, double &readSec );

static void Switch_ReportHitch( int frameMsec, int gameMsec, float swapMsec, const rendererPerf_t &perf,
		int opens0, double openSec0, long long bytes0, double readSec0 ) {
	const int threshold = com_logHitches.GetInteger();
	// map loads run inside one frame and report their own timings
	if ( threshold <= 0 || frameMsec < threshold || frameMsec > 5000 ) {
		return;
	}
	idStr files;
	int opens1;
	double openSec1, readSec1;
	long long bytes1;
	FS_GetProfileTotals( opens1, openSec1, bytes1, readSec1 );
	if ( opens1 < opens0 || bytes1 < bytes0 ) {
		// a map load reset the counters during this frame (fsLoadStats reset)
		files = "(counters reset by a map load)";
	} else {
		files = va( "%d opens (%.0f ms), %.2f MB read (%.0f ms)",
			opens1 - opens0, ( openSec1 - openSec0 ) * 1000.0,
			( bytes1 - bytes0 ) / ( 1024.0 * 1024.0 ), ( readSec1 - readSec0 ) * 1000.0 );
	}
	common->Printf( "hitch: %d ms frame | game %d front %.0f back %.0f swap %.0f | draws %d, buffers %d (%d KB) | files: %s | 3D scale %d%%\n",
		frameMsec, gameMsec, perf.frontEndSec * 1000.0, perf.backEndSec * 1000.0, swapMsec,
		perf.draws, perf.bufferAllocs, perf.bufferAllocBytes / 1024, files.c_str(),
		cvarSystem->GetCVarInteger( "r_renderScaleCurrent" ) );
}

/*
================
Switch_UpdatePerfLog

com_logPerf 1 logs a one-line summary per second: frame rate, average and worst
frame, and where the time went. Game logic, render front end and back end come
from the com_speeds counters; swap is time blocked in eglSwapBuffers (vblank,
r_fpsLock, or the CPU waiting for the GPU). "buffers" counts vertex cache
buffers created per frame (glBufferData on fresh storage, costly on nouveau),
"temp" is the per-frame vertex data and "overflow" the frames whose temp data
did not fit. With r_perfGpuSync 1 "gpu wait" is the GPU work left after the
CPU finished the frame. Comparing these with the Status Monitor CPU/GPU load
shows whether a frame is CPU bound, GPU bound, or serialized.
================
*/
static idCVar com_logPerf( "com_logPerf", "0", CVAR_SYSTEM | CVAR_BOOL, "log a performance summary once per second (frame, game, render front/back end, swap wait, vertex buffers)" );

extern int time_gameFrame;

static void Switch_UpdatePerfLog( int frameMsec, int gameMsec, float swapMsec, const rendererPerf_t &perf ) {
	static int				windowStart = 0;
	static int				frames = 0;
	static int				totalMsec = 0;
	static int				worstMsec = 0;
	static int				gameTotal = 0;
	static float			swapTotal = 0.0f;
	static rendererPerf_t	sum;

	if ( !com_logPerf.GetBool() || frameMsec > 5000 ) {
		windowStart = 0;
		return;
	}

	const int now = Sys_Milliseconds();
	if ( windowStart == 0 ) {
		windowStart = now;
		frames = totalMsec = worstMsec = gameTotal = 0;
		swapTotal = 0.0f;
		memset( &sum, 0, sizeof( sum ) );
	}
	frames++;
	totalMsec += frameMsec;
	worstMsec = Max( worstMsec, frameMsec );
	gameTotal += gameMsec;
	swapTotal += swapMsec;
	sum.frontEndSec += perf.frontEndSec;
	sum.backEndSec += perf.backEndSec;
	sum.gpuTailSec += perf.gpuTailSec;
	sum.draws += perf.draws;
	sum.parmsSkipped += perf.parmsSkipped;
	sum.bufferAllocs += perf.bufferAllocs;
	sum.bufferAllocBytes += perf.bufferAllocBytes;
	sum.tempBytes += perf.tempBytes;
	sum.tempOverflows += perf.tempOverflows;

	if ( now - windowStart >= 1000 && frames > 0 ) {
		const float n = (float)frames;
		idStr gpu;
		if ( sum.gpuTailSec > 0.0 ) {
			gpu = va( " (gpu wait %.1f)", sum.gpuTailSec * 1000.0 / n );
		}
		common->Printf( "perf: %.1f fps | frame %.1f ms (worst %d) | game %.1f | render front %.1f back %.1f%s | swap wait %.1f | draws %d, parms skipped %d | buffers %d (%d KB), temp %d KB, overflow %d | 3D %d%%\n",
			n * 1000.0f / ( now - windowStart ), totalMsec / n, worstMsec,
			gameTotal / n, sum.frontEndSec * 1000.0 / n, sum.backEndSec * 1000.0 / n, gpu.c_str(), swapTotal / n,
			(int)( sum.draws / n ), (int)( sum.parmsSkipped / n ),
			(int)( sum.bufferAllocs / n ), (int)( sum.bufferAllocBytes / n / 1024.0f ), (int)( sum.tempBytes / n / 1024.0f ), sum.tempOverflows,
			cvarSystem->GetCVarInteger( "r_renderScaleCurrent" ) );
		windowStart = now;
		frames = totalMsec = worstMsec = gameTotal = 0;
		swapTotal = 0.0f;
		memset( &sum, 0, sizeof( sum ) );
	}
}

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
	Switch_ApplyPerformanceProfile();

	common->Printf( "%d MB System Memory\n", Sys_GetSystemRam() );
	Switch_StartAsyncThread();

	while ( 1 ) {
		int opens0;
		double openSec0, readSec0;
		long long bytes0;
		FS_GetProfileTotals( opens0, openSec0, bytes0, readSec0 );
		const int gameMsec0 = time_gameFrame;
		const int frameStart = Sys_Milliseconds();

		common->Frame();

		const int frameMsec = Sys_Milliseconds() - frameStart;
		// com_speeds resets time_gameFrame after printing; treat a drop as a reset
		const int gameMsec = ( time_gameFrame >= gameMsec0 ) ? time_gameFrame - gameMsec0 : time_gameFrame;
		// always drain the counters, so a log window never starts inflated
		const float swapMsec = Switch_TakeSwapMsec();
		rendererPerf_t perf;
		R_TakePerfCounters( perf );
		Switch_ReportHitch( frameMsec, gameMsec, swapMsec, perf, opens0, openSec0, bytes0, readSec0 );
		Switch_UpdatePerfLog( frameMsec, gameMsec, swapMsec, perf );

		// r_fpsLock: wait here, between frames, never inside one (see switch_glimp.cpp)
		Switch_PaceFrame();
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
	// the engine (game + render) owns core 0; every other thread defaults to core 2
	Switch_SetNextThreadCore( 0 );
	pthread_t engineThread;
	if ( pthread_create( &engineThread, &attr, Switch_EngineThread, NULL ) != 0 ) {
		Switch_WriteFatalFile( "could not create the engine thread" );
		return EXIT_FAILURE;
	}
	pthread_attr_destroy( &attr );

	// The engine thread ends through Sys_Quit/Sys_Error -> Switch_Exit; the
	// process then leaves normally from here, on the main thread.
	pthread_join( engineThread, NULL );

	// the engine thread is gone: release hardware it left running
	Switch_ShutdownGyro();

	// hand the clocks back to the system default
	Switch_RestorePerformanceProfile();

	if ( s_nxlinkActive ) {
		socketExit();
		s_nxlinkActive = false;
	}
	return s_exitCode;
}
