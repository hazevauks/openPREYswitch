/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

Critical sections, trigger events and threads. Same semantics as
src/sys/posix/posix_threads.cpp; libnx has no pthread_cancel, so the async
thread leaves through a flag instead.

===========================================================================
*/

#include "../../idlib/precompiled.h"

// idlib and libnx both define BIT(); they compute the same value, so let libnx own it here.
#undef BIT
#include <switch.h>
#include <pthread.h>

#include "switch_local.h"

/*
======================================================
locks
======================================================
*/

// we use an extra lock for the local stuff
const int MAX_LOCAL_CRITICAL_SECTIONS = MAX_CRITICAL_SECTIONS + 1;
static pthread_mutex_t global_lock[ MAX_LOCAL_CRITICAL_SECTIONS ];

void Sys_EnterCriticalSection( int index ) {
	assert( index >= 0 && index < MAX_LOCAL_CRITICAL_SECTIONS );
	pthread_mutex_lock( &global_lock[index] );
}

void Sys_LeaveCriticalSection( int index ) {
	assert( index >= 0 && index < MAX_LOCAL_CRITICAL_SECTIONS );
	pthread_mutex_unlock( &global_lock[index] );
}

/*
======================================================
wait and trigger events

Signals raised while no one is waiting stay raised until a wait happens
(which then does a simple pass-through), matching the win32 version.
======================================================
*/

static pthread_cond_t	event_cond[ MAX_TRIGGER_EVENTS ];
static bool				signaled[ MAX_TRIGGER_EVENTS ];
static bool				waiting[ MAX_TRIGGER_EVENTS ];

void Sys_WaitForEvent( int index ) {
	assert( index >= 0 && index < MAX_TRIGGER_EVENTS );
	Sys_EnterCriticalSection( MAX_LOCAL_CRITICAL_SECTIONS - 1 );
	assert( !waiting[ index ] );	// WaitForEvent from multiple threads? that wouldn't be good
	if ( signaled[ index ] ) {
		// emulate windows behaviour: signal has been raised already. clear and keep going
		signaled[ index ] = false;
	} else {
		waiting[ index ] = true;
		pthread_cond_wait( &event_cond[ index ], &global_lock[ MAX_LOCAL_CRITICAL_SECTIONS - 1 ] );
		waiting[ index ] = false;
	}
	Sys_LeaveCriticalSection( MAX_LOCAL_CRITICAL_SECTIONS - 1 );
}

void Sys_TriggerEvent( int index ) {
	assert( index >= 0 && index < MAX_TRIGGER_EVENTS );
	Sys_EnterCriticalSection( MAX_LOCAL_CRITICAL_SECTIONS - 1 );
	if ( waiting[ index ] ) {
		pthread_cond_signal( &event_cond[ index ] );
	} else {
		// emulate windows behaviour: if no thread is waiting, leave the signal on so next wait keeps going
		signaled[ index ] = true;
	}
	Sys_LeaveCriticalSection( MAX_LOCAL_CRITICAL_SECTIONS - 1 );
}

/*
======================================================
thread create and destroy
======================================================
*/

// not a hard limit, just what we keep track of for debugging
xthreadInfo *g_threads[MAX_THREADS];
int g_thread_count = 0;

typedef void *(*pthread_function_t) (void *);

// Worker threads get more than the libnx default stack; the engine code they
// run was written for desktop stack sizes.
static const size_t WORKER_THREAD_STACK_SIZE = 1024 * 1024;

void Sys_CreateThread( xthread_t function, void *parms, xthreadPriority priority, xthreadInfo& info, const char *name, xthreadInfo **threads, int *thread_count ) {
	Sys_EnterCriticalSection();
	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setstacksize( &attr, WORKER_THREAD_STACK_SIZE );
	pthread_t handle;
	if ( pthread_create( &handle, &attr, ( pthread_function_t )function, parms ) != 0 ) {
		common->Error( "ERROR: pthread_create %s failed\n", name );
	}
	pthread_attr_destroy( &attr );
	info.threadHandle = (intptr_t)handle;
	info.name = name;
	if ( *thread_count < MAX_THREADS ) {
		threads[ ( *thread_count )++ ] = &info;
	} else {
		common->DPrintf( "WARNING: MAX_THREADS reached\n" );
	}
	Sys_LeaveCriticalSection();
}

static void Switch_ForgetThread( xthreadInfo &info ) {
	Sys_EnterCriticalSection();
	for ( int i = 0; i < g_thread_count; i++ ) {
		if ( &info == g_threads[ i ] ) {
			int j;
			for ( j = i + 1; j < g_thread_count; j++ ) {
				g_threads[ j - 1 ] = g_threads[ j ];
			}
			g_threads[ j - 1 ] = NULL;
			g_thread_count--;
			break;
		}
	}
	Sys_LeaveCriticalSection();
}

/*
==================
Sys_DestroyThread

No pthread_cancel on libnx: the only thread the engine destroys is the async
thread, which polls s_asyncThreadExit. Other threads must exit on their own
before this is called.
==================
*/
void Sys_DestroyThread( xthreadInfo& info ) {
	assert( info.threadHandle );
	if ( pthread_join( ( pthread_t )info.threadHandle, NULL ) != 0 ) {
		common->Error( "ERROR: pthread_join %s failed\n", info.name );
	}
	info.threadHandle = 0;
	Switch_ForgetThread( info );
}

const char *Sys_GetThreadName( int *index ) {
	Sys_EnterCriticalSection();
	pthread_t thread = pthread_self();
	for ( int i = 0; i < g_thread_count; i++ ) {
		if ( thread == ( pthread_t )g_threads[ i ]->threadHandle ) {
			if ( index ) {
				*index = i;
			}
			Sys_LeaveCriticalSection();
			return g_threads[ i ]->name;
		}
	}
	if ( index ) {
		*index = -1;
	}
	Sys_LeaveCriticalSection();
	return "main";
}

/*
=========================================================
Async thread

Same 16 ms tick as src/sys/linux/main.cpp Sys_AsyncThread.
=========================================================
*/

static xthreadInfo		asyncThread;
static volatile bool	s_asyncThreadExit = false;

static void Switch_AsyncThread( void ) {
	int now = Sys_Milliseconds();
	int ticked = now >> 4;

	while ( !s_asyncThreadExit ) {
		now = Sys_Milliseconds();
		const int next = ( now & 0xFFFFFFF0 ) + 0x10;
		const int wantSleep = next - now - 1;	// sleep 1ms less than the true target
		if ( wantSleep > 0 ) {
			svcSleepThread( (s64)wantSleep * 1000000LL );
		}

		// compensate if we slept too long
		now = Sys_Milliseconds();
		const int toTicked = now >> 4;

		while ( ticked < toTicked ) {
			common->Async();
			ticked++;
			Sys_TriggerEvent( TRIGGER_EVENT_ONE );
		}
	}
}

void Switch_StartAsyncThread( void ) {
	if ( asyncThread.threadHandle == 0 ) {
		s_asyncThreadExit = false;
		Sys_CreateThread( (xthread_t)Switch_AsyncThread, NULL, THREAD_NORMAL, asyncThread, "Async", g_threads, &g_thread_count );
	} else {
		common->Printf( "Async thread already running\n" );
	}
	common->Printf( "Async thread started\n" );
}

void Switch_StopAsyncThread( void ) {
	if ( asyncThread.threadHandle ) {
		s_asyncThreadExit = true;
		Sys_DestroyThread( asyncThread );
	}
}

void Switch_InitThreads( void ) {
	for ( int i = 0; i < MAX_LOCAL_CRITICAL_SECTIONS; i++ ) {
		pthread_mutex_init( &global_lock[i], NULL );
	}
	for ( int i = 0; i < MAX_TRIGGER_EVENTS; i++ ) {
		pthread_cond_init( &event_cond[ i ], NULL );
		signaled[i] = false;
		waiting[i] = false;
	}
	for ( int i = 0; i < MAX_THREADS; i++ ) {
		g_threads[ i ] = NULL;
	}
	memset( &asyncThread, 0, sizeof( asyncThread ) );
}
