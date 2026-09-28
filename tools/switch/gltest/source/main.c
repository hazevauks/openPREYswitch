/*
===========================================================================
OpenPrey Switch port - OpenGL capability probe (phase 1 risk test)

Answers one question before the real port starts: does the Switch Mesa
(nouveau) driver give us what the OpenPrey renderer needs?

  - an OpenGL *compatibility* profile context with an 8-bit stencil buffer
  - immediate mode / fixed function (glBegin, glMatrixMode, ...)
  - ARB assembly programs (the real .vfp files in basepy/glprogs)
  - legacy GLSL (ftransform/gl_TexCoord) for the post-processing shaders
  - S3TC (DXT) compressed textures and the extensions the renderer checks

Results are shown on screen and written to sdmc:/openprey_gltest.txt.
===========================================================================
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#define REPORT_PATH "sdmc:/openprey_gltest.txt"
#define SCREEN_W 1280
#define SCREEN_H 720

/*
===========================================================================
Report
===========================================================================
*/

static char	g_report[32768];
static size_t	g_reportLen;
static int	g_pass, g_warn, g_fail;

static void Rep( const char *fmt, ... ) {
	va_list ap;
	va_start( ap, fmt );
	int n = vsnprintf( g_report + g_reportLen, sizeof( g_report ) - g_reportLen, fmt, ap );
	va_end( ap );
	if ( n > 0 ) {
		g_reportLen += (size_t)n;
		if ( g_reportLen >= sizeof( g_report ) ) {
			g_reportLen = sizeof( g_report ) - 1;
		}
	}
}

typedef enum { CHECK_REQUIRED, CHECK_OPTIONAL } checkLevel_t;

static void Check( checkLevel_t level, int ok, const char *what, const char *detail ) {
	const char *tag;
	if ( ok ) {
		tag = "PASS";
		g_pass++;
	} else if ( level == CHECK_REQUIRED ) {
		tag = "FAIL";
		g_fail++;
	} else {
		tag = "WARN";
		g_warn++;
	}
	Rep( "[%s] %s%s%s\n", tag, what, ( detail && detail[0] ) ? " - " : "", detail ? detail : "" );
}

/*
===========================================================================
GL function loading

The devkitPro glad package is core-profile only, and there is no libGL:
every entry point (including GL 1.0 ones) comes from eglGetProcAddress.
The real port will load its qgl* pointers the same way.
===========================================================================
*/

#define GL_FUNCS( X ) \
	X( PFNGLGETERRORPROC_,            glGetError ) \
	X( PFNGLGETSTRINGPROC_,           glGetString ) \
	X( PFNGLGETINTEGERVPROC_,         glGetIntegerv ) \
	X( PFNGLVIEWPORTPROC_,            glViewport ) \
	X( PFNGLCLEARCOLORPROC_,          glClearColor ) \
	X( PFNGLCLEARPROC_,               glClear ) \
	X( PFNGLENABLEPROC_,              glEnable ) \
	X( PFNGLDISABLEPROC_,             glDisable ) \
	X( PFNGLFINISHPROC_,              glFinish ) \
	X( PFNGLREADPIXELSPROC_,          glReadPixels ) \
	X( PFNGLMATRIXMODEPROC_,          glMatrixMode ) \
	X( PFNGLLOADIDENTITYPROC_,        glLoadIdentity ) \
	X( PFNGLBEGINPROC_,               glBegin ) \
	X( PFNGLENDPROC_,                 glEnd ) \
	X( PFNGLCOLOR3FPROC_,             glColor3f ) \
	X( PFNGLVERTEX2FPROC_,            glVertex2f ) \
	X( PFNGLGENTEXTURESPROC_,         glGenTextures ) \
	X( PFNGLBINDTEXTUREPROC_,         glBindTexture ) \
	X( PFNGLDELETETEXTURESPROC_,      glDeleteTextures ) \
	X( PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D ) \
	X( PFNGLGENPROGRAMSARBPROC,       glGenProgramsARB ) \
	X( PFNGLBINDPROGRAMARBPROC,       glBindProgramARB ) \
	X( PFNGLPROGRAMSTRINGARBPROC,     glProgramStringARB ) \
	X( PFNGLDELETEPROGRAMSARBPROC,    glDeleteProgramsARB ) \
	X( PFNGLCREATESHADERPROC,         glCreateShader ) \
	X( PFNGLSHADERSOURCEPROC,         glShaderSource ) \
	X( PFNGLCOMPILESHADERPROC,        glCompileShader ) \
	X( PFNGLGETSHADERIVPROC,          glGetShaderiv ) \
	X( PFNGLGETSHADERINFOLOGPROC,     glGetShaderInfoLog ) \
	X( PFNGLDELETESHADERPROC,         glDeleteShader ) \
	X( PFNGLCREATEPROGRAMPROC,        glCreateProgram ) \
	X( PFNGLATTACHSHADERPROC,         glAttachShader ) \
	X( PFNGLLINKPROGRAMPROC,          glLinkProgram ) \
	X( PFNGLGETPROGRAMIVPROC,         glGetProgramiv ) \
	X( PFNGLGETPROGRAMINFOLOGPROC,    glGetProgramInfoLog ) \
	X( PFNGLDELETEPROGRAMPROC,        glDeleteProgram ) \
	X( PFNGLGENBUFFERSPROC,           glGenBuffers ) \
	X( PFNGLBINDBUFFERPROC,           glBindBuffer ) \
	X( PFNGLBUFFERDATAPROC,           glBufferData ) \
	X( PFNGLDELETEBUFFERSPROC,        glDeleteBuffers )

/* GL 1.0/1.1 entry points have no PFN typedefs in glext.h; declare them here. */
typedef GLenum         ( APIENTRY *PFNGLGETERRORPROC_ )( void );
typedef const GLubyte *( APIENTRY *PFNGLGETSTRINGPROC_ )( GLenum );
typedef void           ( APIENTRY *PFNGLGETINTEGERVPROC_ )( GLenum, GLint * );
typedef void           ( APIENTRY *PFNGLVIEWPORTPROC_ )( GLint, GLint, GLsizei, GLsizei );
typedef void           ( APIENTRY *PFNGLCLEARCOLORPROC_ )( GLfloat, GLfloat, GLfloat, GLfloat );
typedef void           ( APIENTRY *PFNGLCLEARPROC_ )( GLbitfield );
typedef void           ( APIENTRY *PFNGLENABLEPROC_ )( GLenum );
typedef void           ( APIENTRY *PFNGLDISABLEPROC_ )( GLenum );
typedef void           ( APIENTRY *PFNGLFINISHPROC_ )( void );
typedef void           ( APIENTRY *PFNGLREADPIXELSPROC_ )( GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void * );
typedef void           ( APIENTRY *PFNGLMATRIXMODEPROC_ )( GLenum );
typedef void           ( APIENTRY *PFNGLLOADIDENTITYPROC_ )( void );
typedef void           ( APIENTRY *PFNGLBEGINPROC_ )( GLenum );
typedef void           ( APIENTRY *PFNGLENDPROC_ )( void );
typedef void           ( APIENTRY *PFNGLCOLOR3FPROC_ )( GLfloat, GLfloat, GLfloat );
typedef void           ( APIENTRY *PFNGLVERTEX2FPROC_ )( GLfloat, GLfloat );
typedef void           ( APIENTRY *PFNGLGENTEXTURESPROC_ )( GLsizei, GLuint * );
typedef void           ( APIENTRY *PFNGLBINDTEXTUREPROC_ )( GLenum, GLuint );
typedef void           ( APIENTRY *PFNGLDELETETEXTURESPROC_ )( GLsizei, const GLuint * );

#define DECLARE_FUNC( type, name ) static type q##name;
GL_FUNCS( DECLARE_FUNC )

static int LoadGLFunctions( void ) {
	int missing = 0;
#define LOAD_FUNC( type, name ) \
	q##name = (type)eglGetProcAddress( #name ); \
	if ( !q##name ) { Rep( "       missing entry point: %s\n", #name ); missing++; }
	GL_FUNCS( LOAD_FUNC )
#undef LOAD_FUNC
	return missing;
}

/*
===========================================================================
EGL
===========================================================================
*/

static EGLDisplay	s_display = EGL_NO_DISPLAY;
static EGLContext	s_context = EGL_NO_CONTEXT;
static EGLSurface	s_surface = EGL_NO_SURFACE;

static const char *InitEGL( void ) {
	s_display = eglGetDisplay( EGL_DEFAULT_DISPLAY );
	if ( s_display == EGL_NO_DISPLAY ) {
		return NULL;
	}
	if ( !eglInitialize( s_display, NULL, NULL ) ) {
		return NULL;
	}
	if ( !eglBindAPI( EGL_OPENGL_API ) ) {
		return NULL;
	}

	/* The renderer's shadow volumes need a stencil buffer. */
	static const EGLint configAttribs[] = {
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
		return NULL;
	}

	s_surface = eglCreateWindowSurface( s_display, config, nwindowGetDefault(), NULL );
	if ( s_surface == EGL_NO_SURFACE ) {
		return NULL;
	}

	/* Prefer an explicit compatibility profile, then fall back to a plain legacy context. */
	static const EGLint compatAttribs[] = {
		EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR,
		EGL_NONE
	};
	static const EGLint legacyAttribs[] = { EGL_NONE };

	const char *kind = "compatibility profile";
	s_context = eglCreateContext( s_display, config, EGL_NO_CONTEXT, compatAttribs );
	if ( s_context == EGL_NO_CONTEXT ) {
		kind = "default (legacy) context";
		s_context = eglCreateContext( s_display, config, EGL_NO_CONTEXT, legacyAttribs );
	}
	if ( s_context == EGL_NO_CONTEXT ) {
		return NULL;
	}

	eglMakeCurrent( s_display, s_surface, s_surface, s_context );
	return kind;
}

static void DeinitEGL( void ) {
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
===========================================================================
Helpers
===========================================================================
*/

static char *LoadFile( const char *path ) {
	FILE *f = fopen( path, "rb" );
	if ( !f ) {
		return NULL;
	}
	fseek( f, 0, SEEK_END );
	long len = ftell( f );
	fseek( f, 0, SEEK_SET );
	char *buf = malloc( (size_t)len + 1 );
	if ( buf ) {
		size_t got = fread( buf, 1, (size_t)len, f );
		buf[got] = '\0';
	}
	fclose( f );
	return buf;
}

static int HasExtension( const char *extensions, const char *name ) {
	size_t len = strlen( name );
	const char *p = extensions;
	while ( p && ( p = strstr( p, name ) ) != NULL ) {
		int startOk = ( p == extensions ) || ( p[-1] == ' ' );
		int endOk = ( p[len] == ' ' ) || ( p[len] == '\0' );
		if ( startOk && endOk ) {
			return 1;
		}
		p += len;
	}
	return 0;
}

static void DrawFullscreenTriangle( void ) {
	qglBegin( GL_TRIANGLES );
	qglVertex2f( -1.0f, -1.0f );
	qglVertex2f( 3.0f, -1.0f );
	qglVertex2f( -1.0f, 3.0f );
	qglEnd();
}

static void ReadCenterPixel( GLubyte out[4] ) {
	memset( out, 0, 4 );
	qglFinish();
	qglReadPixels( SCREEN_W / 2, SCREEN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out );
}

/*
===========================================================================
Tests
===========================================================================
*/

static void TestExtensions( void ) {
	static const struct { const char *name; checkLevel_t level; } exts[] = {
		/* needed by the ARB2 interaction path the renderer uses */
		{ "GL_ARB_multitexture",             CHECK_REQUIRED },
		{ "GL_ARB_texture_env_combine",      CHECK_REQUIRED },
		{ "GL_ARB_texture_cube_map",         CHECK_REQUIRED },
		{ "GL_ARB_vertex_program",           CHECK_REQUIRED },
		{ "GL_ARB_fragment_program",         CHECK_REQUIRED },
		{ "GL_ARB_vertex_buffer_object",     CHECK_REQUIRED },
		{ "GL_EXT_texture_compression_s3tc", CHECK_REQUIRED },
		{ "GL_ARB_texture_compression",      CHECK_REQUIRED },
		/* GLSL post-processing (bloom, SSAO, CRT, blur) */
		{ "GL_ARB_shader_objects",           CHECK_OPTIONAL },
		{ "GL_ARB_vertex_shader",            CHECK_OPTIONAL },
		{ "GL_ARB_fragment_shader",          CHECK_OPTIONAL },
		{ "GL_ARB_shading_language_100",     CHECK_OPTIONAL },
		/* quality / performance paths with fallbacks */
		{ "GL_EXT_stencil_wrap",             CHECK_OPTIONAL },
		{ "GL_EXT_stencil_two_side",         CHECK_OPTIONAL },
		{ "GL_ATI_separate_stencil",         CHECK_OPTIONAL },
		{ "GL_EXT_depth_bounds_test",        CHECK_OPTIONAL },
		{ "GL_EXT_texture_filter_anisotropic", CHECK_OPTIONAL },
		{ "GL_ARB_texture_non_power_of_two", CHECK_OPTIONAL },
		{ "GL_ARB_texture_env_add",          CHECK_OPTIONAL },
		{ "GL_ARB_texture_env_dot3",         CHECK_OPTIONAL },
		{ "GL_EXT_texture3D",                CHECK_OPTIONAL },
		{ "GL_EXT_texture_lod",              CHECK_OPTIONAL },
	};

	const char *extensions = (const char *)qglGetString( GL_EXTENSIONS );
	Check( CHECK_REQUIRED, extensions != NULL, "glGetString(GL_EXTENSIONS)",
		extensions ? "" : "not available (core-only context?)" );
	if ( !extensions ) {
		return;
	}
	for ( size_t i = 0; i < sizeof( exts ) / sizeof( exts[0] ); i++ ) {
		Check( exts[i].level, HasExtension( extensions, exts[i].name ), exts[i].name, "" );
	}
}

static void TestImmediateMode( void ) {
	char detail[128];
	GLubyte px[4];

	qglViewport( 0, 0, SCREEN_W, SCREEN_H );
	qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity();
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();
	qglColor3f( 1.0f, 0.0f, 0.0f );
	DrawFullscreenTriangle();
	ReadCenterPixel( px );

	GLenum err = qglGetError();
	snprintf( detail, sizeof( detail ), "pixel=%u,%u,%u glError=0x%04X", px[0], px[1], px[2], err );
	Check( CHECK_REQUIRED, px[0] > 200 && px[1] < 50 && px[2] < 50 && err == GL_NO_ERROR,
		"immediate mode draw (glBegin/glColor/glVertex)", detail );
}

/* Mirrors R_LoadARBProgram: program text runs from "!!ARBvp"/"!!ARBfp" to the first "END". */
static int CompileARBProgram( GLenum target, const char *fileText, const char *label ) {
	char detail[256];
	const char *marker = ( target == GL_VERTEX_PROGRAM_ARB ) ? "!!ARBvp" : "!!ARBfp";
	const char *start = strstr( fileText, marker );
	const char *end = start ? strstr( start, "END" ) : NULL;
	if ( !start || !end ) {
		Check( CHECK_REQUIRED, 0, label, "program markers not found in file" );
		return 0;
	}
	size_t len = (size_t)( end - start ) + 3;

	GLuint prog = 0;
	qglGenProgramsARB( 1, &prog );
	qglBindProgramARB( target, prog );
	qglGetError();
	qglProgramStringARB( target, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)len, start );

	GLint errPos = -1;
	qglGetIntegerv( GL_PROGRAM_ERROR_POSITION_ARB, &errPos );
	GLenum err = qglGetError();
	int ok = ( errPos == -1 && err == GL_NO_ERROR );
	if ( ok ) {
		detail[0] = '\0';
	} else {
		const char *msg = (const char *)qglGetString( GL_PROGRAM_ERROR_STRING_ARB );
		snprintf( detail, sizeof( detail ), "error at char %d: %.180s", errPos, msg ? msg : "(no message)" );
	}
	Check( CHECK_REQUIRED, ok, label, detail );
	qglDeleteProgramsARB( 1, &prog );
	return ok;
}

static void TestARBPrograms( void ) {
	static const char *files[] = {
		"romfs:/glprogs/interaction.vfp",
		"romfs:/glprogs/openq4_smaa_edge.vfp",
		"romfs:/glprogs/openq4_smaa_blend.vfp",
	};
	char label[128];

	for ( size_t i = 0; i < sizeof( files ) / sizeof( files[0] ); i++ ) {
		const char *name = strrchr( files[i], '/' ) + 1;
		char *text = LoadFile( files[i] );
		if ( !text ) {
			snprintf( label, sizeof( label ), "load %s", name );
			Check( CHECK_REQUIRED, 0, label, "file missing from RomFS" );
			continue;
		}
		snprintf( label, sizeof( label ), "ARB vertex program   %s", name );
		CompileARBProgram( GL_VERTEX_PROGRAM_ARB, text, label );
		snprintf( label, sizeof( label ), "ARB fragment program %s", name );
		CompileARBProgram( GL_FRAGMENT_PROGRAM_ARB, text, label );
		free( text );
	}

	/* Draw with a trivial fragment program to prove ARB programs actually execute. */
	static const char solidBlue[] = "!!ARBfp1.0\nMOV result.color, {0.0, 0.0, 1.0, 1.0};\nEND";
	char detail[128];
	GLubyte px[4];
	GLuint prog = 0;
	qglGenProgramsARB( 1, &prog );
	qglBindProgramARB( GL_FRAGMENT_PROGRAM_ARB, prog );
	qglProgramStringARB( GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)strlen( solidBlue ), solidBlue );
	qglEnable( GL_FRAGMENT_PROGRAM_ARB );
	qglColor3f( 1.0f, 0.0f, 0.0f );
	DrawFullscreenTriangle();
	ReadCenterPixel( px );
	qglDisable( GL_FRAGMENT_PROGRAM_ARB );
	qglDeleteProgramsARB( 1, &prog );
	snprintf( detail, sizeof( detail ), "pixel=%u,%u,%u", px[0], px[1], px[2] );
	Check( CHECK_REQUIRED, px[0] < 50 && px[2] > 200, "draw with ARB fragment program", detail );
}

static GLuint CompileShader( GLenum type, const char *path, char *log, size_t logSize ) {
	char *src = LoadFile( path );
	if ( !src ) {
		snprintf( log, logSize, "%s missing from RomFS", path );
		return 0;
	}
	GLuint sh = qglCreateShader( type );
	const char *srcPtr = src;
	qglShaderSource( sh, 1, &srcPtr, NULL );
	qglCompileShader( sh );
	free( src );

	GLint ok = 0;
	qglGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
	if ( !ok ) {
		GLsizei n = 0;
		qglGetShaderInfoLog( sh, (GLsizei)logSize, &n, log );
		qglDeleteShader( sh );
		return 0;
	}
	return sh;
}

static void TestGLSLPair( const char *vsPath, const char *fsPath, const char *label ) {
	char log[512] = "";
	char detail[256];
	GLuint vs = CompileShader( GL_VERTEX_SHADER, vsPath, log, sizeof( log ) );
	GLuint fs = vs ? CompileShader( GL_FRAGMENT_SHADER, fsPath, log, sizeof( log ) ) : 0;
	GLint linked = 0;
	if ( vs && fs ) {
		GLuint prog = qglCreateProgram();
		qglAttachShader( prog, vs );
		qglAttachShader( prog, fs );
		qglLinkProgram( prog );
		qglGetProgramiv( prog, GL_LINK_STATUS, &linked );
		if ( !linked ) {
			GLsizei n = 0;
			qglGetProgramInfoLog( prog, sizeof( log ), &n, log );
		}
		qglDeleteProgram( prog );
	}
	if ( vs ) {
		qglDeleteShader( vs );
	}
	if ( fs ) {
		qglDeleteShader( fs );
	}
	/* keep the one-line report readable */
	for ( char *c = log; *c; c++ ) {
		if ( *c == '\n' || *c == '\r' ) {
			*c = ' ';
		}
	}
	snprintf( detail, sizeof( detail ), "%.200s", linked ? "" : log );
	Check( CHECK_OPTIONAL, linked, label, detail );
}

static void TestTexturesAndBuffers( void ) {
	char detail[64];

	/* One 4x4 DXT1 block: the retail .dds textures are S3TC compressed. */
	static const GLubyte dxt1Block[8] = { 0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00 };
	GLuint tex = 0;
	qglGenTextures( 1, &tex );
	qglBindTexture( GL_TEXTURE_2D, tex );
	qglGetError();
	qglCompressedTexImage2D( GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 4, 4, 0, sizeof( dxt1Block ), dxt1Block );
	GLenum err = qglGetError();
	qglDeleteTextures( 1, &tex );
	snprintf( detail, sizeof( detail ), "glError=0x%04X", err );
	Check( CHECK_REQUIRED, err == GL_NO_ERROR, "upload DXT1 compressed texture", detail );

	GLuint vbo = 0;
	static const float verts[6] = { 0 };
	qglGenBuffers( 1, &vbo );
	qglBindBuffer( GL_ARRAY_BUFFER, vbo );
	qglBufferData( GL_ARRAY_BUFFER, sizeof( verts ), verts, GL_STATIC_DRAW );
	err = qglGetError();
	qglBindBuffer( GL_ARRAY_BUFFER, 0 );
	qglDeleteBuffers( 1, &vbo );
	snprintf( detail, sizeof( detail ), "glError=0x%04X", err );
	Check( CHECK_REQUIRED, err == GL_NO_ERROR, "vertex buffer object", detail );
}

/* Returns 1 when a GL context with loaded entry points is left current for the result screen. */
static int RunTests( void ) {
	const char *contextKind = InitEGL();
	Check( CHECK_REQUIRED, contextKind != NULL, "create EGL OpenGL context with 8-bit stencil",
		contextKind ? contextKind : "eglCreateContext failed" );
	if ( !contextKind ) {
		return 0;
	}

	int missing = LoadGLFunctions();
	Check( CHECK_REQUIRED, missing == 0, "load GL entry points via eglGetProcAddress", "" );
	if ( missing ) {
		return 0;
	}

	GLint profileMask = 0, stencilBits = 0, maxTexSize = 0;
	qglGetIntegerv( GL_CONTEXT_PROFILE_MASK, &profileMask );
	qglGetIntegerv( GL_STENCIL_BITS, &stencilBits );
	qglGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxTexSize );
	qglGetError(); /* GL_CONTEXT_PROFILE_MASK is invalid on pre-3.2 contexts */

	Rep( "       GL_VENDOR   : %s\n", qglGetString( GL_VENDOR ) );
	Rep( "       GL_RENDERER : %s\n", qglGetString( GL_RENDERER ) );
	Rep( "       GL_VERSION  : %s\n", qglGetString( GL_VERSION ) );
	Rep( "       GLSL        : %s\n", qglGetString( GL_SHADING_LANGUAGE_VERSION ) );
	Rep( "       profile mask: 0x%X  max texture: %d\n", profileMask, maxTexSize );

	char detail[64];
	snprintf( detail, sizeof( detail ), "%d bits", stencilBits );
	Check( CHECK_REQUIRED, stencilBits >= 8, "stencil buffer", detail );
	Check( CHECK_REQUIRED, ( profileMask & GL_CONTEXT_CORE_PROFILE_BIT ) == 0,
		"not a core-only profile", ( profileMask & GL_CONTEXT_CORE_PROFILE_BIT ) ? "core profile" : "" );

	TestExtensions();
	TestImmediateMode();
	TestARBPrograms();
	TestGLSLPair( "romfs:/glprogs/blur.vs", "romfs:/glprogs/blur.fs", "GLSL blur.vs + blur.fs" );
	TestGLSLPair( "romfs:/glprogs/openprey_bloom.vs", "romfs:/glprogs/openprey_bloom.fs", "GLSL openprey_bloom" );
	TestTexturesAndBuffers();

	return 1;
}

/*
===========================================================================
Entry
===========================================================================
*/

int main( int argc, char **argv ) {
	(void)argc;
	(void)argv;

	Rep( "OpenPrey Switch GL probe\n" );
	Rep( "========================\n" );

	Result rc = romfsInit();
	Check( CHECK_REQUIRED, R_SUCCEEDED( rc ), "mount RomFS", "" );

	int glReady = RunTests();

	Rep( "------------------------\n" );
	Rep( "PASS %d  WARN %d  FAIL %d\n", g_pass, g_warn, g_fail );
	Rep( "%s\n", g_fail == 0 ? "RESULT: renderer requirements met" : "RESULT: renderer requirements NOT met" );

	FILE *f = fopen( REPORT_PATH, "wb" );
	int saved = 0;
	if ( f ) {
		saved = fwrite( g_report, 1, g_reportLen, f ) == g_reportLen;
		fclose( f );
	}

	if ( R_SUCCEEDED( rc ) ) {
		romfsExit();
	}

	padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
	PadState pad;
	padInitializeDefault( &pad );

	if ( glReady ) {
		/*
		Result screen drawn with GL: green = no FAIL, red = FAIL (details in the
		report file). Switching the same window over to the libnx text console
		after tearing EGL down crashed on hardware, so GL keeps the screen until
		exit, the same way the official devkitPro GL examples do.
		*/
		const int ok = ( g_fail == 0 ) && saved;
		while ( appletMainLoop() ) {
			padUpdate( &pad );
			if ( padGetButtonsDown( &pad ) & HidNpadButton_Plus ) {
				break;
			}
			qglClearColor( ok ? 0.0f : 0.8f, ok ? 0.6f : 0.0f, 0.0f, 1.0f );
			qglClear( GL_COLOR_BUFFER_BIT );
			eglSwapBuffers( s_display, s_surface );
		}
		DeinitEGL();
		return 0;
	}

	/* No usable GL: EGL never drew to the window, so the text console is safe here. */
	DeinitEGL();
	consoleInit( NULL );
	printf( "%s", g_report );
	printf( "\nReport %s %s\n", saved ? "saved to" : "could NOT be saved to", REPORT_PATH );
	printf( "Press + to exit.\n" );
	while ( appletMainLoop() ) {
		padUpdate( &pad );
		if ( padGetButtonsDown( &pad ) & HidNpadButton_Plus ) {
			break;
		}
		consoleUpdate( NULL );
	}
	consoleExit( NULL );
	return 0;
}
