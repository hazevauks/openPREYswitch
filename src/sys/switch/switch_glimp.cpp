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

static const int SWITCH_SCREEN_WIDTH	= 1280;
static const int SWITCH_SCREEN_HEIGHT	= 720;

static EGLDisplay	s_display = EGL_NO_DISPLAY;
static EGLContext	s_context = EGL_NO_CONTEXT;
static EGLSurface	s_surface = EGL_NO_SURFACE;

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

	eglSwapInterval( s_display, r_swapInterval.GetInteger() );

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
}

void GLimp_SwapBuffers( void ) {
	if ( r_swapInterval.IsModified() ) {
		r_swapInterval.ClearModified();
		eglSwapInterval( s_display, r_swapInterval.GetInteger() );
	}
	eglSwapBuffers( s_display, s_surface );
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
