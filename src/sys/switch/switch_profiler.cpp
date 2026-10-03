/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

CPU profiler: per-thread CPU time, a sampling profiler and the GPU load.

Every thread registers here through the pthread_create trampoline
(switch_threads.cpp), library threads included. The kernel counts the CPU
time of each thread, which com_logPerf prints.

com_cpuProfile 1 starts a sampler thread. Every 2 ms it pauses each
registered thread, reads where it is (program counter, link register and the
return addresses up its frame records) and resumes it. Every
com_cpuProfileSeconds, and when the profile is turned off, it appends a
report to basepr/logs/openprey_cpuprofile.txt:

- per thread, its CPU time and the code lines (16 bytes) it was sampled in
  most often while running;
- the most frequent call stacks of all threads.

The engine thread is sampled while it waits as well, so the report covers
its whole time: running, in a system call, or blocked (GPU driver calls into
the system, waits for the GPU, file reads).

Addresses are offsets into the executable. tools/switch/cpu_profile.py names
them from the ELF of the same build, for the engine, the game and the
statically linked GL driver alike, and adds up the time of each function with
and without what it calls.

The method is the one of the CPU profiler in the UnleashedRecomp and
MarathonRecomp Switch ports (os/switch/cpu_profiler_switch.cpp).

===========================================================================
*/

#include "../../idlib/precompiled.h"

// idlib and libnx both define BIT(); they compute the same value, so let libnx own it here.
#undef BIT
#include <switch.h>

#include <fcntl.h>
#include <unistd.h>

#include "switch_local.h"

static idCVar com_cpuProfile( "com_cpuProfile", "0", CVAR_SYSTEM | CVAR_BOOL, "sampling CPU profiler: notes where every thread is each 2 ms and writes reports to logs/openprey_cpuprofile.txt, which tools/switch/cpu_profile.py reads" );
static idCVar com_cpuProfileSeconds( "com_cpuProfileSeconds", "20", CVAR_SYSTEM | CVAR_INTEGER, "seconds each CPU profile report covers", 2, 600 );

#define PROFILE_REPORT_PATH		SWITCH_BASE_PATH "/basepr/logs/openprey_cpuprofile.txt"

/*
============================================================================
THREADS
============================================================================
*/

static const int	MAX_PROFILED_THREADS = 32;

typedef struct {
	Handle	handle;
	bool	active;
	bool	whole;				// sampled while waiting too (the engine thread)
	char	name[32];			// empty for library threads; the report names them by startOffset
	u32		startOffset;		// code offset of the start routine
	u64		stackLow;
	u64		stackHigh;
	u64		sampleTicks;		// CPU ticks at the previous sample
	u64		usageTicks;			// CPU ticks at the previous Switch_ProfilerThreadUsage
	u64		reportTicks;		// CPU ticks when the report window started
	u32		samplesRun;			// samples of the window, by kind
	u32		samplesSvc;
	u32		samplesWait;
} profThread_t;

static profThread_t	s_threads[MAX_PROFILED_THREADS];
static Mutex		s_threadsLock;

// The executable's code mapping. Its start is address 0 of the ELF.
static u64			s_codeStart = 0;
static u64			s_codeEnd = 0;

static void Prof_FindCode( void ) {
	if ( s_codeEnd != 0 ) {
		return;
	}
	MemoryInfo info = {};
	u32 pageInfo;
	if ( R_SUCCEEDED( svcQueryMemory( &info, &pageInfo, (u64)&Prof_FindCode ) ) ) {
		s_codeStart = info.addr;
		s_codeEnd = info.addr + info.size;
	}
}

// 0 for an address outside the code
static u32 Prof_CodeOffset( u64 address ) {
	if ( address < s_codeStart || address >= s_codeEnd ) {
		return 0;
	}
	return (u32)( address - s_codeStart );
}

// false once the kernel has refused to give a thread's CPU time
static bool			s_haveThreadTicks = true;

static u64 Prof_ThreadTicks( Handle handle ) {
	// the id changed in system version 13.0.0
	const u32 type = hosversionAtLeast( 13, 0, 0 ) ? (u32)InfoType_ThreadTickCount : (u32)InfoType_ThreadTickCountDeprecated;
	u64 ticks = 0;
	if ( R_FAILED( svcGetInfo( &ticks, type, handle, (u64)-1 ) ) ) {
		return 0;
	}
	return ticks;
}

/*
================
Switch_ProfilerRegisterThread

Called by a new thread about itself. name may be NULL.
================
*/
void Switch_ProfilerRegisterThread( const char *name, void *start ) {
	Prof_FindCode();

	// the mapping that holds this thread's stack
	MemoryInfo stack = {};
	u32 pageInfo;
	svcQueryMemory( &stack, &pageInfo, (u64)&stack );

	const Handle handle = threadGetCurHandle();
	mutexLock( &s_threadsLock );
	for ( int i = 0; i < MAX_PROFILED_THREADS; i++ ) {
		profThread_t &t = s_threads[i];
		// a slot is free again once its samples have been reported
		if ( t.active || t.samplesRun || t.samplesSvc || t.samplesWait ) {
			continue;
		}
		memset( &t, 0, sizeof( t ) );
		t.handle = handle;
		if ( name ) {
			idStr::Copynz( t.name, name, sizeof( t.name ) );
		}
		t.startOffset = Prof_CodeOffset( (u64)start );
		t.stackLow = stack.addr;
		t.stackHigh = stack.addr + stack.size;
		t.sampleTicks = t.usageTicks = t.reportTicks = Prof_ThreadTicks( handle );
		t.active = true;
		break;
	}
	mutexUnlock( &s_threadsLock );
}

static profThread_t *Prof_FindThread( Handle handle ) {
	for ( int i = 0; i < MAX_PROFILED_THREADS; i++ ) {
		if ( s_threads[i].active && s_threads[i].handle == handle ) {
			return &s_threads[i];
		}
	}
	return NULL;
}

void Switch_ProfilerUnregisterThread( void ) {
	mutexLock( &s_threadsLock );
	profThread_t *t = Prof_FindThread( threadGetCurHandle() );
	if ( t ) {
		t->active = false;
	}
	mutexUnlock( &s_threadsLock );
}

/*
================
Switch_ProfilerTrackWaits

The calling thread is sampled while it waits too. For the engine thread:
where it blocks is part of what a frame costs.
================
*/
void Switch_ProfilerTrackWaits( void ) {
	mutexLock( &s_threadsLock );
	profThread_t *t = Prof_FindThread( threadGetCurHandle() );
	if ( t ) {
		t->whole = true;
	}
	mutexUnlock( &s_threadsLock );
}

/*
================
Switch_ProfilerThreadUsage

CPU time of every thread since the last call, in percent of one core, as
"engine 96%, Async 4%". Threads under 1% are left out.
================
*/
void Switch_ProfilerThreadUsage( char *text, int size ) {
	static u64 lastTick = 0;
	const u64 now = armGetSystemTick();
	const u64 window = now - lastTick;
	lastTick = now;

	text[0] = '\0';
	mutexLock( &s_threadsLock );
	for ( int i = 0; i < MAX_PROFILED_THREADS; i++ ) {
		profThread_t &t = s_threads[i];
		if ( !t.active ) {
			continue;
		}
		const u64 ticks = Prof_ThreadTicks( t.handle );
		const u64 used = ticks - t.usageTicks;
		t.usageTicks = ticks;
		const int percent = window ? (int)( used * 100 / window ) : 0;
		if ( percent < 1 ) {
			continue;
		}
		const char *name = t.name[0] ? t.name : va( "+0x%x", t.startOffset );
		idStr::Append( text, size, va( "%s%s %d%%", text[0] ? ", " : "", name, percent ) );
	}
	mutexUnlock( &s_threadsLock );
}

/*
============================================================================
GPU LOAD
============================================================================
*/

// NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD: the GPU's busy share as its power management
// measures it, in 0.1% units. It is the figure Status Monitor shows.
static const u32	NVGPU_PMU_GET_GPU_LOAD = 0x80044715;

static u32			s_gpuCtrlFd = 0;
static int			s_gpuCtrlState = 0;		// 0 = not tried, 1 = open, -1 = not available

/*
================
Switch_GpuLoad

GPU load in 0.1% units, or -1 when the system does not give it.
================
*/
int Switch_GpuLoad( void ) {
	if ( s_gpuCtrlState == 0 ) {
		s_gpuCtrlState = -1;
		if ( R_SUCCEEDED( nvInitialize() ) ) {
			if ( R_SUCCEEDED( nvOpen( &s_gpuCtrlFd, "/dev/nvhost-ctrl-gpu" ) ) ) {
				s_gpuCtrlState = 1;
			} else {
				nvExit();
			}
		}
	}
	if ( s_gpuCtrlState != 1 ) {
		return -1;
	}
	u32 load = 0;
	if ( R_FAILED( nvIoctl( s_gpuCtrlFd, NVGPU_PMU_GET_GPU_LOAD, &load ) ) ) {
		nvClose( s_gpuCtrlFd );
		nvExit();
		s_gpuCtrlState = -1;
		return -1;
	}
	return (int)load;
}

/*
============================================================================
SAMPLER
============================================================================
*/

static const u64	SAMPLE_INTERVAL_NS = 2000000ULL;	// 500 samples per second and thread
static const int	LINE_TABLE_SIZE = 1 << 16;			// ( thread, 16-byte code line ) -> samples
static const int	LINE_KEY_BITS = 26;					// code offset >> 4: 1 GB of code
static const int	STACK_TABLE_SIZE = 1 << 14;
static const int	STACK_DEPTH = 24;
static const int	REPORT_LINES = 1000;				// code lines listed per thread
static const int	REPORT_STACKS = 4000;				// call stacks listed
static const int	REPORT_TEXT_BYTES = 1536 * 1024;

// what a thread was doing when it was sampled
enum {
	SAMPLE_RUN,			// running its own code
	SAMPLE_SVC,			// in a system call, having used CPU time since the last sample
	SAMPLE_WAIT			// no CPU time since the last sample: blocked
};
static const char	s_kindLetters[] = { 'r', 's', 'w' };

typedef struct {
	u32		key;		// ( thread slot << LINE_KEY_BITS ) | ( code offset >> 4 )
	u32		count;		// 0 = empty
} profLine_t;

typedef struct {
	u32		count;					// 0 = empty
	u32		pc;						// code offset of the first sample that made this entry
	byte	slot;
	byte	kind;
	byte	depth;
	u32		chain[STACK_DEPTH];		// link register, then the return addresses up the frame records
} profStack_t;

// allocated while the sampler runs; only the sampler thread uses them
static profLine_t *		s_lines = NULL;
static profLine_t *		s_sortedLines = NULL;
static profStack_t *	s_stacks = NULL;
static u16 *			s_sortedStacks = NULL;
static char *			s_reportText = NULL;
static int				s_reportLength = 0;
static bool				s_reportFull = false;
static u32				s_linesDropped = 0;
static u32				s_stacksDropped = 0;

static Thread			s_samplerThread;
static bool				s_samplerRunning = false;
static volatile bool	s_samplerStop = false;
static volatile u32		s_frames = 0;				// main loop frames, counted by the engine thread
static volatile u32		s_reportsWritten = 0;
static volatile u32		s_reportWindowMsec = 0;		// last report
static volatile u32		s_reportSamples = 0;
static volatile bool	s_reportFailed = false;
static volatile u64		s_reportIntervalTicks = 0;

// A thread blocked in a system call is reported with its PC at the SVC
// instruction, one on its way out right after it.
static bool Prof_InSystemCall( u64 pc ) {
	if ( pc < s_codeStart + 4 || pc >= s_codeEnd || ( pc & 3 ) != 0 ) {
		return false;
	}
	const u32 *code = (const u32 *)pc;
	return ( code[0] & 0xFFE0001Fu ) == 0xD4000001u || ( code[-1] & 0xFFE0001Fu ) == 0xD4000001u;
}

/*
================
Prof_WalkStack

The link register, then the return addresses of the frame records x29 leads
to (GCC keeps the chain in every function that calls another). The thread is
paused, so its stack can be read.
================
*/
static int Prof_WalkStack( const profThread_t &t, const ThreadContext &ctx, u32 *chain ) {
	int depth = 0;
	chain[depth++] = Prof_CodeOffset( ctx.lr );

	u64 fp = ctx.fp;
	while ( depth < STACK_DEPTH ) {
		if ( ( fp & 15 ) != 0 || fp < t.stackLow || fp + 16 > t.stackHigh ) {
			break;
		}
		const u64 next = ( (const u64 *)fp )[0];
		const u32 ret = Prof_CodeOffset( ( (const u64 *)fp )[1] );
		if ( ret == 0 ) {
			break;
		}
		chain[depth++] = ret;
		if ( next <= fp ) {
			break;
		}
		fp = next;
	}
	return depth;
}

static void Prof_AddLine( int slot, u32 pc ) {
	const u32 key = ( (u32)slot << LINE_KEY_BITS ) | ( pc >> 4 );
	const u32 hash = key * 2654435761u;
	for ( u32 probe = 0; probe < 64; probe++ ) {
		profLine_t &line = s_lines[( hash + probe ) & ( LINE_TABLE_SIZE - 1 )];
		if ( line.count == 0 ) {
			line.key = key;
			line.count = 1;
			return;
		}
		if ( line.key == key ) {
			line.count++;
			return;
		}
	}
	s_linesDropped++;
}

// Running samples of one call path differ in their PC: they share an entry per
// 32 bytes of code. A system call is always at the same instruction.
static ID_INLINE u32 Prof_StackPcKey( int kind, u32 pc ) {
	return ( kind == SAMPLE_RUN ) ? ( pc >> 5 ) : pc;
}

static void Prof_AddStack( int slot, int kind, u32 pc, const u32 *chain, int depth ) {
	const u32 pcKey = Prof_StackPcKey( kind, pc );
	u32 hash = ( ( (u32)slot << 8 ) | (u32)kind ) * 2654435761u ^ pcKey;
	for ( int i = 0; i < depth; i++ ) {
		hash = ( hash ^ chain[i] ) * 16777619u;
	}
	for ( u32 probe = 0; probe < 32; probe++ ) {
		profStack_t &stack = s_stacks[( hash + probe ) & ( STACK_TABLE_SIZE - 1 )];
		if ( stack.count == 0 ) {
			stack.count = 1;
			stack.pc = pc;
			stack.slot = (byte)slot;
			stack.kind = (byte)kind;
			stack.depth = (byte)depth;
			memcpy( stack.chain, chain, depth * sizeof( chain[0] ) );
			return;
		}
		if ( stack.slot == slot && stack.kind == kind && stack.depth == depth && Prof_StackPcKey( kind, stack.pc ) == pcKey
			&& memcmp( stack.chain, chain, depth * sizeof( chain[0] ) ) == 0 ) {
			stack.count++;
			return;
		}
	}
	s_stacksDropped++;
}

static void Prof_SampleThread( int slot ) {
	profThread_t &t = s_threads[slot];

	// Nothing between the pause and the resume may allocate, lock or print:
	// the paused thread may hold any of those locks.
	if ( R_FAILED( svcSetThreadActivity( t.handle, ThreadActivity_Paused ) ) ) {
		return;
	}
	ThreadContext ctx;
	const Result rc = svcGetThreadContext3( &ctx, t.handle );
	u32 chain[STACK_DEPTH];
	int depth = 0;
	bool inSystemCall = false;
	if ( R_SUCCEEDED( rc ) ) {
		depth = Prof_WalkStack( t, ctx, chain );
		inSystemCall = Prof_InSystemCall( ctx.pc.x );
	}
	svcSetThreadActivity( t.handle, ThreadActivity_Runnable );
	if ( R_FAILED( rc ) ) {
		return;
	}

	// The kernel adds up a thread's CPU time when it switches away from it, which
	// the pause just made it do: the count now covers everything up to the pause.
	// Without the count, every sample is taken as one of a thread that ran.
	bool ran = true;
	if ( s_haveThreadTicks ) {
		const u64 ticks = Prof_ThreadTicks( t.handle );
		ran = ( ticks != t.sampleTicks );
		t.sampleTicks = ticks;
	}

	int kind;
	if ( !ran ) {
		t.samplesWait++;
		if ( !t.whole ) {
			return;
		}
		kind = SAMPLE_WAIT;
	} else if ( inSystemCall ) {
		t.samplesSvc++;
		kind = SAMPLE_SVC;
	} else {
		t.samplesRun++;
		kind = SAMPLE_RUN;
	}

	const u32 pc = Prof_CodeOffset( ctx.pc.x );
	if ( kind == SAMPLE_RUN ) {
		Prof_AddLine( slot, pc );
	}
	Prof_AddStack( slot, kind, pc, chain, depth );
}

static void Prof_Append( const char *fmt, ... ) {
	if ( s_reportFull ) {
		return;
	}
	va_list argptr;
	va_start( argptr, fmt );
	const int length = idStr::vsnPrintf( s_reportText + s_reportLength, REPORT_TEXT_BYTES - s_reportLength, fmt, argptr );
	va_end( argptr );
	if ( length < 0 ) {
		// did not fit: the report ends before this text
		s_reportFull = true;
		return;
	}
	s_reportLength += length;
}

// " %x" without the cost of a printf: a report holds tens of thousands of addresses
static void Prof_AppendHex( u32 value ) {
	if ( s_reportFull || s_reportLength > REPORT_TEXT_BYTES - 16 ) {
		s_reportFull = true;
		return;
	}
	char digits[8];
	int numDigits = 0;
	do {
		digits[numDigits++] = "0123456789abcdef"[value & 15];
		value >>= 4;
	} while ( value != 0 );
	s_reportText[s_reportLength++] = ' ';
	while ( numDigits > 0 ) {
		s_reportText[s_reportLength++] = digits[--numDigits];
	}
}

static int Prof_CompareLines( const void *a, const void *b ) {
	const profLine_t *lineA = (const profLine_t *)a;
	const profLine_t *lineB = (const profLine_t *)b;
	const u32 slotA = lineA->key >> LINE_KEY_BITS;
	const u32 slotB = lineB->key >> LINE_KEY_BITS;
	if ( slotA != slotB ) {
		return ( slotA < slotB ) ? -1 : 1;
	}
	if ( lineA->count != lineB->count ) {
		return ( lineA->count > lineB->count ) ? -1 : 1;
	}
	return 0;
}

static int Prof_CompareStacks( const void *a, const void *b ) {
	const u32 countA = s_stacks[*(const u16 *)a].count;
	const u32 countB = s_stacks[*(const u16 *)b].count;
	if ( countA != countB ) {
		return ( countA > countB ) ? -1 : 1;
	}
	return 0;
}

/*
================
Prof_WriteReport

Appends the window's report to the file in one write and clears the tables.
The format is what tools/switch/cpu_profile.py parses: "l" lines are code
lines (offset, samples) of the thread above them, "s" lines are call stacks
(thread slot, kind, samples, PC, link register, return addresses).
================
*/
static void Prof_WriteReport( u64 windowStart, u64 windowEnd, u32 frames ) {
	static profThread_t threads[MAX_PROFILED_THREADS];
	static u32 cpuPermille[MAX_PROFILED_THREADS];
	const u64 windowTicks = windowEnd - windowStart;

	// copy and reset the per-thread counts under the lock; format without it
	mutexLock( &s_threadsLock );
	for ( int i = 0; i < MAX_PROFILED_THREADS; i++ ) {
		profThread_t &t = s_threads[i];
		threads[i] = t;
		cpuPermille[i] = 0;
		if ( t.active ) {
			const u64 ticks = Prof_ThreadTicks( t.handle );
			cpuPermille[i] = windowTicks ? (u32)( ( ticks - t.reportTicks ) * 1000 / windowTicks ) : 0;
			t.reportTicks = ticks;
		}
		t.samplesRun = t.samplesSvc = t.samplesWait = 0;
	}
	mutexUnlock( &s_threadsLock );

	s_reportLength = 0;
	s_reportFull = false;
	Prof_Append( "[cpu profile] report %u: %.1f s, %u frames, %u us between samples, code 0x%llx bytes; %u code line and %u stack samples dropped\n",
		(unsigned)( s_reportsWritten + 1 ), armTicksToNs( windowTicks ) / 1.0e9, (unsigned)frames, (unsigned)( SAMPLE_INTERVAL_NS / 1000 ),
		(unsigned long long)( s_codeEnd - s_codeStart ), (unsigned)s_linesDropped, (unsigned)s_stacksDropped );

	int numLines = 0;
	for ( int i = 0; i < LINE_TABLE_SIZE; i++ ) {
		if ( s_lines[i].count != 0 ) {
			s_sortedLines[numLines++] = s_lines[i];
		}
	}
	qsort( s_sortedLines, numLines, sizeof( s_sortedLines[0] ), Prof_CompareLines );

	u32 engineSamples = 0;
	int line = 0;
	for ( int i = 0; i < MAX_PROFILED_THREADS; i++ ) {
		const profThread_t &t = threads[i];
		const u32 samples = t.samplesRun + t.samplesSvc + t.samplesWait;
		if ( samples == 0 ) {
			continue;
		}
		if ( t.whole ) {
			engineSamples = samples;
		}
		Prof_Append( "thread %d \"%s\" start +0x%x: cpu %u.%u%%; samples %u: %u running, %u in system calls, %u waiting%s\n",
			i, t.name, (unsigned)t.startOffset, (unsigned)( cpuPermille[i] / 10 ), (unsigned)( cpuPermille[i] % 10 ),
			(unsigned)samples, (unsigned)t.samplesRun, (unsigned)t.samplesSvc, (unsigned)t.samplesWait,
			t.whole ? "" : " (not sampled while waiting)" );
		int listed = 0;
		for ( ; line < numLines && ( s_sortedLines[line].key >> LINE_KEY_BITS ) <= (u32)i; line++ ) {
			if ( ( s_sortedLines[line].key >> LINE_KEY_BITS ) == (u32)i && listed < REPORT_LINES ) {
				Prof_Append( " l %x %u\n", (unsigned)( ( s_sortedLines[line].key & ( ( 1u << LINE_KEY_BITS ) - 1 ) ) << 4 ), (unsigned)s_sortedLines[line].count );
				listed++;
			}
		}
	}

	int numStacks = 0;
	u32 stackSamples = 0;
	for ( int i = 0; i < STACK_TABLE_SIZE; i++ ) {
		if ( s_stacks[i].count != 0 ) {
			s_sortedStacks[numStacks++] = (u16)i;
			stackSamples += s_stacks[i].count;
		}
	}
	qsort( s_sortedStacks, numStacks, sizeof( s_sortedStacks[0] ), Prof_CompareStacks );

	const int listedStacks = Min( numStacks, REPORT_STACKS );
	u32 listedSamples = 0;
	for ( int i = 0; i < listedStacks; i++ ) {
		listedSamples += s_stacks[s_sortedStacks[i]].count;
	}
	Prof_Append( "stacks: %d of %d listed, with %u of %u samples\n", listedStacks, numStacks, (unsigned)listedSamples, (unsigned)stackSamples );
	for ( int i = 0; i < listedStacks; i++ ) {
		const profStack_t &stack = s_stacks[s_sortedStacks[i]];
		Prof_Append( " s %d %c %u", (int)stack.slot, s_kindLetters[stack.kind], (unsigned)stack.count );
		Prof_AppendHex( stack.pc );
		for ( int j = 0; j < stack.depth; j++ ) {
			Prof_AppendHex( stack.chain[j] );
		}
		Prof_Append( "\n" );
	}
	Prof_Append( "[end of report]\n" );

	// one write: the SD card is slow per call, not per byte
	bool failed = true;
	const int fd = open( PROFILE_REPORT_PATH, O_WRONLY | O_CREAT | ( s_reportsWritten == 0 ? O_TRUNC : O_APPEND ), 0666 );
	if ( fd >= 0 ) {
		failed = ( write( fd, s_reportText, s_reportLength ) != s_reportLength );
		close( fd );
	}

	memset( s_lines, 0, LINE_TABLE_SIZE * sizeof( s_lines[0] ) );
	memset( s_stacks, 0, STACK_TABLE_SIZE * sizeof( s_stacks[0] ) );
	s_linesDropped = 0;
	s_stacksDropped = 0;

	s_reportFailed = failed;
	s_reportWindowMsec = (u32)( armTicksToNs( windowTicks ) / 1000000ULL );
	s_reportSamples = engineSamples;
	__atomic_add_fetch( &s_reportsWritten, 1, __ATOMIC_SEQ_CST );
}

static void Prof_SamplerThread( void * ) {
	u64 windowStart = armGetSystemTick();
	u32 windowFrames = s_frames;

	while ( !s_samplerStop ) {
		svcSleepThread( SAMPLE_INTERVAL_NS );

		mutexLock( &s_threadsLock );
		for ( int i = 0; i < MAX_PROFILED_THREADS; i++ ) {
			if ( s_threads[i].active ) {
				Prof_SampleThread( i );
			}
		}
		mutexUnlock( &s_threadsLock );

		const u64 now = armGetSystemTick();
		if ( now - windowStart >= s_reportIntervalTicks ) {
			const u32 frames = s_frames;
			Prof_WriteReport( windowStart, now, frames - windowFrames );
			windowFrames = frames;
			windowStart = armGetSystemTick();
		}
	}

	// what was sampled since the last report, unless it is next to nothing
	const u64 now = armGetSystemTick();
	if ( armTicksToNs( now - windowStart ) >= 1000000000ULL ) {
		Prof_WriteReport( windowStart, now, s_frames - windowFrames );
	}
}

static void Prof_FreeTables( void ) {
	free( s_lines );
	free( s_sortedLines );
	free( s_stacks );
	free( s_sortedStacks );
	free( s_reportText );
	s_lines = s_sortedLines = NULL;
	s_stacks = NULL;
	s_sortedStacks = NULL;
	s_reportText = NULL;
}

static bool Prof_Start( void ) {
	// svcSetThreadActivity and svcGetThreadContext3: calling a system call the
	// loader did not grant ends the process
	if ( !envIsSyscallHinted( 0x32 ) || !envIsSyscallHinted( 0x33 ) ) {
		common->Printf( "cpu profile: this loader does not allow pausing threads; not available\n" );
		return false;
	}

	Prof_FindCode();
	s_haveThreadTicks = ( Prof_ThreadTicks( threadGetCurHandle() ) != 0 );
	if ( !s_haveThreadTicks ) {
		common->Printf( "cpu profile: the system does not give the CPU time of threads; waiting and running samples are not told apart\n" );
	}
	s_lines = (profLine_t *)calloc( LINE_TABLE_SIZE, sizeof( s_lines[0] ) );
	s_sortedLines = (profLine_t *)malloc( LINE_TABLE_SIZE * sizeof( s_sortedLines[0] ) );
	s_stacks = (profStack_t *)calloc( STACK_TABLE_SIZE, sizeof( s_stacks[0] ) );
	s_sortedStacks = (u16 *)malloc( STACK_TABLE_SIZE * sizeof( s_sortedStacks[0] ) );
	s_reportText = (char *)malloc( REPORT_TEXT_BYTES );
	if ( !s_lines || !s_sortedLines || !s_stacks || !s_sortedStacks || !s_reportText ) {
		Prof_FreeTables();
		common->Printf( "cpu profile: no memory for the sample tables\n" );
		return false;
	}
	s_linesDropped = s_stacksDropped = 0;

	// start the threads' windows now
	mutexLock( &s_threadsLock );
	for ( int i = 0; i < MAX_PROFILED_THREADS; i++ ) {
		profThread_t &t = s_threads[i];
		if ( t.active ) {
			t.sampleTicks = t.reportTicks = Prof_ThreadTicks( t.handle );
		}
		t.samplesRun = t.samplesSvc = t.samplesWait = 0;
	}
	mutexUnlock( &s_threadsLock );

	// Above the threads it samples (pthreads run at priority 0x3B), so a sample is
	// never late behind them. On the async tick's core: it sleeps between samples.
	s_samplerStop = false;
	s_reportIntervalTicks = armNsToTicks( (u64)com_cpuProfileSeconds.GetInteger() * 1000000000ULL );
	if ( R_FAILED( threadCreate( &s_samplerThread, Prof_SamplerThread, NULL, NULL, 0x10000, 0x2C, 1 ) ) ) {
		Prof_FreeTables();
		common->Printf( "cpu profile: could not create the sampler thread\n" );
		return false;
	}
	if ( R_FAILED( threadStart( &s_samplerThread ) ) ) {
		threadClose( &s_samplerThread );
		Prof_FreeTables();
		common->Printf( "cpu profile: could not start the sampler thread\n" );
		return false;
	}
	s_samplerRunning = true;
	common->Printf( "cpu profile: sampling every %d us, a report every %d s in logs/openprey_cpuprofile.txt\n",
		(int)( SAMPLE_INTERVAL_NS / 1000 ), com_cpuProfileSeconds.GetInteger() );
	return true;
}

static void Prof_PrintReports( void ) {
	static u32 printed = 0;
	const u32 written = s_reportsWritten;
	if ( written == printed ) {
		return;
	}
	printed = written;
	if ( s_reportFailed ) {
		common->Printf( "cpu profile: report %u could not be written to " PROFILE_REPORT_PATH "\n", (unsigned)written );
	} else {
		common->Printf( "cpu profile: report %u written (%.1f s, %u samples of the engine thread)\n",
			(unsigned)written, s_reportWindowMsec / 1000.0f, (unsigned)s_reportSamples );
	}
}

static void Prof_Stop( void ) {
	s_samplerStop = true;
	threadWaitForExit( &s_samplerThread );
	threadClose( &s_samplerThread );
	s_samplerRunning = false;
	Prof_FreeTables();
	Prof_PrintReports();
	common->Printf( "cpu profile: stopped\n" );
}

/*
================
Switch_ProfilerFrame

Engine thread, once per main loop frame: follows com_cpuProfile and logs the
reports the sampler wrote.
================
*/
void Switch_ProfilerFrame( void ) {
	__atomic_add_fetch( &s_frames, 1, __ATOMIC_RELAXED );

	if ( com_cpuProfile.GetBool() != s_samplerRunning ) {
		if ( s_samplerRunning ) {
			Prof_Stop();
		} else if ( !Prof_Start() ) {
			com_cpuProfile.SetBool( false );
		}
	}
	if ( s_samplerRunning ) {
		s_reportIntervalTicks = armNsToTicks( (u64)com_cpuProfileSeconds.GetInteger() * 1000000000ULL );
		Prof_PrintReports();
	}
}

/*
================
Switch_ShutdownProfiler

Every thread has to end before the process leaves.
================
*/
void Switch_ShutdownProfiler( void ) {
	if ( s_samplerRunning ) {
		s_samplerStop = true;
		threadWaitForExit( &s_samplerThread );
		threadClose( &s_samplerThread );
		s_samplerRunning = false;
	}
	if ( s_gpuCtrlState == 1 ) {
		nvClose( s_gpuCtrlFd );
		nvExit();
		s_gpuCtrlState = -1;
	}
}
