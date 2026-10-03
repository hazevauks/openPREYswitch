/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

OpenGL through Mesa (nouveau) EGL on the default native window. The context
setup matches tools/switch/gltest, which was verified on hardware: a GL 4.3
compatibility profile with an 8-bit stencil buffer.

The screen is always 1280x720 (the native handheld size; docked output is
scaled by the system). r_mode/r_fullscreen are ignored.

===========================================================================
*/

#include "../../idlib/precompiled.h"
#include "../../renderer/tr_local.h"

// idlib and libnx both define BIT(); they compute the same value, so let libnx own it here.
#undef BIT
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "switch_local.h"

// newlib hides setenv() in strict C++ mode
extern "C" int setenv( const char *name, const char *value, int overwrite );

static const int SWITCH_SCREEN_WIDTH	= 1280;
static const int SWITCH_SCREEN_HEIGHT	= 720;

static EGLDisplay	s_display = EGL_NO_DISPLAY;
static EGLContext	s_context = EGL_NO_CONTEXT;
static EGLSurface	s_surface = EGL_NO_SURFACE;

static EGLint	s_maxSwapInterval = 1;

/*
GLthread (Mesa 26 port only): Mesa records GL calls on the calling thread and
runs the driver on its own thread, which the pthread_create wrapper places on
core 2 (switch_threads.cpp). The engine thread then pays only for recording.
The port enables it by default. The first Mesa 26 test ran before thread
placement existed, so that thread shared core 0 with the engine. Applies at
the next vid_restart.
*/
static idCVar r_switchGLThread( "r_switchGLThread", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Mesa GLthread (Mesa 26 build only): run the GL driver on its own core; applies on vid_restart" );

/*
GL driver and driver options (Mesa 26 port only; devkitPro Mesa ignores them).
Both are read when the display is created.

r_switchGLDriver 1 runs OpenGL through Zink on the NVK Vulkan driver instead
of nouveau's own GL driver (NVC0). It needs the unified Mesa SDK, which has
both.

r_switchMesaEnv hands environment variables to the driver, as
"NAME=value;NAME=value", to compare its options without a rebuild.

The choice is saved with the config, so a driver that cannot start would leave
no way back. While a driver other than NVC0 runs, a marker file exists; a
normal GL shutdown removes it. Finding it at startup means the last session
with that driver crashed, hung or was closed from HOME, and NVC0 comes back.
*/
static idCVar r_switchGLDriver( "r_switchGLDriver", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "GL driver (Mesa 26 build only): 0 = NVC0 (nouveau), 1 = Zink on the NVK Vulkan driver; applies at the next start", 0, 1 );
static idCVar r_switchMesaEnv( "r_switchMesaEnv", "", CVAR_RENDERER | CVAR_ARCHIVE, "environment variables for the Mesa driver (Mesa 26 build only), as NAME=value;NAME=value; applies at the next start" );

#define SWITCH_GL_DRIVER_MARKER		SWITCH_BASE_PATH "/basepr/gl_driver_trial.txt"

static int		s_mesaMajor = 0;		// from GL_VERSION

bool Switch_GLDriverChoice( void ) {
	return s_mesaMajor >= 26;
}

static void GLimp_SelectDriver( void ) {
	bool zink = ( r_switchGLDriver.GetInteger() == 1 );
	if ( zink ) {
		FILE *marker = fopen( SWITCH_GL_DRIVER_MARKER, "rb" );
		if ( marker ) {
			// stays until GL shuts down normally, so the fallback holds even if this session fails too
			fclose( marker );
			common->Printf( "GL driver: the last start with Zink did not end normally; back to NVC0\n" );
			r_switchGLDriver.SetInteger( 0 );
			zink = false;
		} else {
			marker = fopen( SWITCH_GL_DRIVER_MARKER, "wb" );
			if ( marker ) {
				fputs( "OpenPrey started with r_switchGLDriver 1 (Zink) and has not shut GL down yet.\n", marker );
				fclose( marker );
			}
		}
	}
	setenv( "MESA_SWITCH_GL_DRIVER", zink ? "zink" : "nvc0", 1 );

	// NAME=value;NAME=value
	idStr env = r_switchMesaEnv.GetString();
	while ( env.Length() > 0 ) {
		idStr entry = env;
		const int end = env.Find( ';' );
		if ( end >= 0 ) {
			entry = env.Left( end );
			env = env.Right( env.Length() - end - 1 );
		} else {
			env.Clear();
		}
		const int equals = entry.Find( '=' );
		if ( equals > 0 ) {
			const idStr name = entry.Left( equals );
			const idStr value = entry.Right( entry.Length() - equals - 1 );
			setenv( name.c_str(), value.c_str(), 1 );
			common->Printf( "Mesa environment: %s=%s\n", name.c_str(), value.c_str() );
		}
	}
}

static void GLimp_ApplySwapInterval( void ) {
	const int interval = idMath::ClampInt( 0, Max( 1, (int)s_maxSwapInterval ), r_swapInterval.GetInteger() );
	eglSwapInterval( s_display, interval );
	common->Printf( "swap interval %d\n", interval );
}

static void GLimp_DestroyEGL( void ) {
	if ( s_display != EGL_NO_DISPLAY ) {
		eglMakeCurrent( s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
		if ( s_context != EGL_NO_CONTEXT ) {
			eglDestroyContext( s_display, s_context );
		}
		if ( s_surface != EGL_NO_SURFACE ) {
			eglDestroySurface( s_display, s_surface );
		}
		eglTerminate( s_display );
	}
	s_display = EGL_NO_DISPLAY;
	s_context = EGL_NO_CONTEXT;
	s_surface = EGL_NO_SURFACE;
}

/*
===================
GLimp_Init
===================
*/
bool GLimp_Init( glimpParms_t parms ) {
	common->Printf( "Initializing OpenGL subsystem (Switch EGL)\n" );

	// read by the Mesa 26 port (-Dswitch_mesa_sdk) at context creation; devkitPro Mesa ignores it
	setenv( "MESA_SWITCH_GLTHREAD", r_switchGLThread.GetBool() ? "1" : "0", 1 );
	GLimp_SelectDriver();

	s_display = eglGetDisplay( EGL_DEFAULT_DISPLAY );
	if ( s_display == EGL_NO_DISPLAY || !eglInitialize( s_display, NULL, NULL ) ) {
		common->Printf( "GLimp_Init: eglInitialize failed (0x%x)\n", eglGetError() );
		GLimp_DestroyEGL();
		return false;
	}
	if ( !eglBindAPI( EGL_OPENGL_API ) ) {
		common->Printf( "GLimp_Init: eglBindAPI failed (0x%x)\n", eglGetError() );
		GLimp_DestroyEGL();
		return false;
	}

	// shadow volumes need the stencil buffer
	const EGLint configAttribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint numConfigs = 0;
	if ( !eglChooseConfig( s_display, configAttribs, &config, 1, &numConfigs ) || numConfigs == 0 ) {
		common->Printf( "GLimp_Init: no RGBA8/D24/S8 config (0x%x)\n", eglGetError() );
		GLimp_DestroyEGL();
		return false;
	}

	NWindow *win = nwindowGetDefault();
	nwindowSetDimensions( win, SWITCH_SCREEN_WIDTH, SWITCH_SCREEN_HEIGHT );
	s_surface = eglCreateWindowSurface( s_display, config, win, NULL );
	if ( s_surface == EGL_NO_SURFACE ) {
		common->Printf( "GLimp_Init: eglCreateWindowSurface failed (0x%x)\n", eglGetError() );
		GLimp_DestroyEGL();
		return false;
	}

	const EGLint contextAttribs[] = {
		EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
		EGL_NONE
	};
	s_context = eglCreateContext( s_display, config, EGL_NO_CONTEXT, contextAttribs );
	if ( s_context == EGL_NO_CONTEXT ) {
		common->Printf( "GLimp_Init: eglCreateContext failed (0x%x)\n", eglGetError() );
		GLimp_DestroyEGL();
		return false;
	}
	if ( !eglMakeCurrent( s_display, s_surface, s_surface, s_context ) ) {
		common->Printf( "GLimp_Init: eglMakeCurrent failed (0x%x)\n", eglGetError() );
		GLimp_DestroyEGL();
		return false;
	}

	const char *firstMissing = NULL;
	const int missing = SwitchGL_LoadCoreEntryPoints( &firstMissing );
	if ( missing ) {
		common->Printf( "GLimp_Init: %d GL 1.1 entry points missing (first: %s)\n", missing, firstMissing ? firstMissing : "?" );
		GLimp_DestroyEGL();
		return false;
	}
	common->Printf( "GL: %s | %s | %s\n", (const char *)glGetString( GL_VENDOR ),
		(const char *)glGetString( GL_RENDERER ), (const char *)glGetString( GL_VERSION ) );

	// "4.3 (Compatibility Profile) Mesa 20.1.0"
	const char *mesa = strstr( (const char *)glGetString( GL_VERSION ), "Mesa " );
	s_mesaMajor = mesa ? atoi( mesa + 5 ) : 0;

	if ( !eglGetConfigAttrib( s_display, config, EGL_MAX_SWAP_INTERVAL, &s_maxSwapInterval ) ) {
		s_maxSwapInterval = 1;
	}
	GLimp_ApplySwapInterval();

	glConfig.vidWidth = SWITCH_SCREEN_WIDTH;
	glConfig.vidHeight = SWITCH_SCREEN_HEIGHT;
	glConfig.isFullscreen = true;
	glConfig.displayFrequency = 60;
	glConfig.colorBits = 32;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;

	if ( parms.width != SWITCH_SCREEN_WIDTH || parms.height != SWITCH_SCREEN_HEIGHT ) {
		common->Printf( "GLimp_Init: requested %dx%d, using the Switch screen size %dx%d\n",
			parms.width, parms.height, SWITCH_SCREEN_WIDTH, SWITCH_SCREEN_HEIGHT );
	}
	return true;
}

bool GLimp_SetScreenParms( glimpParms_t parms ) {
	// fixed screen size on Switch
	return true;
}

void GLimp_Shutdown( void ) {
	common->Printf( "Shutting down OpenGL subsystem\n" );
	GLimp_DestroyEGL();
	// the driver ran and ended normally (GLimp_SelectDriver)
	remove( SWITCH_GL_DRIVER_MARKER );
}

/*
===================
Frame timing and the frame rate lock

r_fpsLock 30 holds frames 33.3 ms apart, so the frame rate stays steady instead
of swinging between 20 and 45 fps, and the spare time is left idle (cooler,
longer battery). Switch_PaceFrame sleeps between main loop frames, never inside
a frame. Two earlier designs failed on hardware:
- swap interval 2 (EGL reports a huge maximum) froze the loading screen;
- sleeping before each swap made map loads crawl: the loading screen is
  redrawn after nearly every file read (idSessionLocal::PacifierUpdate), so
  each read waited up to 33 ms and a load took minutes at ~15% CPU.
===================
*/
static idCVar r_fpsLock( "r_fpsLock", "30", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "lock the frame rate: 30 = hold 30 fps, 0 = off (20 to 60 set the rate, 1 to 19 mean 30)", 0, 60 );

static double	s_swapMsecAccum = 0.0;		// eglSwapBuffers time since Switch_TakeSwapMsec
static double	s_frameWaitMsec = 0.0;		// last frame: swap + pacing
static u64		s_nextFrameTick = 0;		// when the next paced frame may start

// time spent inside eglSwapBuffers, for com_logPerf (switch_main.cpp): a long
// swap means the CPU is waiting for the GPU to finish the frame
float Switch_TakeSwapMsec( void ) {
	const float msec = (float)s_swapMsecAccum;
	s_swapMsecAccum = 0.0;
	return msec;
}

// time the last frame waited (swap and r_fpsLock); dynamic resolution
// subtracts it to see how long the frame actually worked
float GLimp_LastFrameWaitMsec( void ) {
	return (float)s_frameWaitMsec;
}

void Switch_PaceFrame( void ) {
	static int loggedFps = -1;
	const int lock = r_fpsLock.GetInteger();
	const int fps = ( lock <= 0 ) ? 0 : ( lock >= 20 ? lock : 30 );
	if ( fps != loggedFps ) {
		loggedFps = fps;
		if ( fps ) {
			common->Printf( "frame rate locked at %d fps\n", fps );
		} else {
			common->Printf( "frame rate unlocked\n" );
		}
	}
	if ( fps == 0 ) {
		s_nextFrameTick = 0;
		return;
	}
	const u64 period = armNsToTicks( 1000000000ULL / (u64)fps );
	const u64 now = armGetSystemTick();
	if ( s_nextFrameTick != 0 && now < s_nextFrameTick ) {
		const u64 waitTicks = s_nextFrameTick - now;
		svcSleepThread( (s64)armTicksToNs( waitTicks ) );
		s_frameWaitMsec += armTicksToNs( waitTicks ) / 1000000.0;
		s_nextFrameTick += period;
	} else if ( s_nextFrameTick != 0 && now < s_nextFrameTick + period ) {
		// a little late: keep the cadence
		s_nextFrameTick += period;
	} else {
		// first frame, or a long one (a hitch, a map load): restart the cadence
		s_nextFrameTick = now + period;
	}
}

void GLimp_SwapBuffers( void ) {
	if ( r_swapInterval.IsModified() ) {
		r_swapInterval.ClearModified();
		GLimp_ApplySwapInterval();
	}
	const u64 start = armGetSystemTick();
	eglSwapBuffers( s_display, s_surface );
	s_frameWaitMsec = armTicksToNs( armGetSystemTick() - start ) / 1000000.0;
	s_swapMsecAccum += s_frameWaitMsec;
}

void GLimp_SetGamma( unsigned short red[256], unsigned short green[256], unsigned short blue[256] ) {
	// no hardware gamma ramps on Switch
}

void GLimp_ActivateContext( void ) {
	eglMakeCurrent( s_display, s_surface, s_surface, s_context );
}

void GLimp_DeactivateContext( void ) {
	eglMakeCurrent( s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
}

void GLimp_EnableLogging( bool enable ) {
}
