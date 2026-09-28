/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

Controller and touch input through libnx HID.

In game, buttons and sticks use the same K_JOY keys and joystick axes as the
Windows SDL3 backend (src/sys/win32/win_sdl3.cpp), so binds carry over:

	L = JOY1   R = JOY2   B = JOY3 (south)   A = JOY4 (east)
	X = JOY5 (north)      Y = JOY6 (west)    - = JOY8
	D-pad up/down/right/left = JOY9..JOY12   L3 = JOY13   R3 = JOY14
	ZR = JOY15 (right trigger)               ZL = JOY16 (left trigger)
	+ = Escape (opens and closes the menu)

	left stick  -> AXIS_YAW / AXIS_PITCH (move), right stick -> AXIS_SIDE /
	AXIS_FORWARD (look), AXIS_ROLL = 127 marks the dedicated look stick.

The triggers are digital on Switch, so unlike SDL3 they only act as buttons
and do not feed AXIS_UP.

While a GUI is active (menus, PDA, console) the left stick moves the cursor,
A clicks (mouse 1) and B backs out (Escape). Touching the screen places the
cursor and clicks.

===========================================================================
*/

#include "../../idlib/precompiled.h"

// idlib and libnx both define BIT(); they compute the same value, so let libnx own it here.
#undef BIT
#include <switch.h>

#include "switch_local.h"

static idCVar in_joystick( "in_joystick", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "enable joystick/gamepad input" );
static idCVar in_joystickDeadZone( "in_joystickDeadZone", "0.18", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "joystick axis dead zone", 0.0f, 0.95f );
static idCVar in_menuCursorSpeed( "in_menuCursorSpeed", "14", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "menu cursor speed for the left stick, in pixels per frame at full tilt", 1.0f, 60.0f );

static const int SWITCH_SCREEN_WIDTH	= 1280;
static const int SWITCH_SCREEN_HEIGHT	= 720;

// GUIs use a 640x480 virtual screen.
static const float GUI_VIRTUAL_WIDTH	= 640.0f;
static const float GUI_VIRTUAL_HEIGHT	= 480.0f;

typedef struct {
	int		key;
	bool	down;
} switchKeyEvent_t;

typedef struct {
	int		axis;
	int		value;
} switchJoystickAxisEvent_t;

static const int INPUT_QUEUE_SIZE = 256;
static const int INPUT_QUEUE_MASK = INPUT_QUEUE_SIZE - 1;

static switchKeyEvent_t				s_keyboardQueue[INPUT_QUEUE_SIZE];
static int							s_keyboardHead = 0;
static int							s_keyboardTail = 0;
static switchKeyEvent_t				s_polledKeyboard[INPUT_QUEUE_SIZE];
static int							s_polledKeyboardCount = 0;

static int							s_joystickAxisState[MAX_JOYSTICK_AXIS];
static switchJoystickAxisEvent_t	s_polledJoystick[MAX_JOYSTICK_AXIS];
static int							s_polledJoystickCount = 0;

static bool							s_inputInitialized = false;
static PadState						s_pad;
static u64							s_buttonsDown = 0;		// buttons currently reported as pressed
static bool							s_menuMode = false;
static float						s_cursorRemainderX = 0.0f;
static float						s_cursorRemainderY = 0.0f;
static bool							s_touchDown = false;

typedef struct {
	u64		button;
	int		gameKey;	// key while playing
	int		menuKey;	// key while a GUI is active (0 = same as gameKey)
} switchButtonMap_t;

static const switchButtonMap_t s_buttonMap[] = {
	{ HidNpadButton_L,			K_JOY1,		0 },
	{ HidNpadButton_R,			K_JOY2,		0 },
	{ HidNpadButton_B,			K_JOY3,		K_ESCAPE },
	{ HidNpadButton_A,			K_JOY4,		K_MOUSE1 },
	{ HidNpadButton_X,			K_JOY5,		0 },
	{ HidNpadButton_Y,			K_JOY6,		0 },
	{ HidNpadButton_Plus,		K_ESCAPE,	0 },
	{ HidNpadButton_Minus,		K_JOY8,		0 },
	{ HidNpadButton_Up,			K_JOY9,		K_UPARROW },
	{ HidNpadButton_Down,		K_JOY10,	K_DOWNARROW },
	{ HidNpadButton_Right,		K_JOY11,	K_RIGHTARROW },
	{ HidNpadButton_Left,		K_JOY12,	K_LEFTARROW },
	{ HidNpadButton_StickL,		K_JOY13,	0 },
	{ HidNpadButton_StickR,		K_JOY14,	0 },
	{ HidNpadButton_ZR,			K_JOY15,	0 },
	{ HidNpadButton_ZL,			K_JOY16,	0 },
};
static const int NUM_BUTTON_MAPS = sizeof( s_buttonMap ) / sizeof( s_buttonMap[0] );

// key each button was pressed as, so the release matches even if the mode changed
static int s_pressedAs[NUM_BUTTON_MAPS];

/*
================
input queues
================
*/

static void Switch_QueueKeyboardInput( int key, bool down ) {
	const int next = ( s_keyboardHead + 1 ) & INPUT_QUEUE_MASK;
	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	if ( next == s_keyboardTail ) {
		s_keyboardTail = ( s_keyboardTail + 1 ) & INPUT_QUEUE_MASK;
	}
	s_keyboardQueue[s_keyboardHead].key = key;
	s_keyboardQueue[s_keyboardHead].down = down;
	s_keyboardHead = next;
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
}

static void Switch_PostKey( int key, bool down ) {
	if ( key == 0 ) {
		return;
	}
	Switch_QueEvent( SE_KEY, key, down ? 1 : 0, 0, NULL );
	Switch_QueueKeyboardInput( key, down );
}

/*
================
helpers
================
*/

static int Switch_NormalizeStick( s32 value, float deadZone ) {
	float normalized = (float)value / (float)JOYSTICK_MAX;
	normalized = idMath::ClampFloat( -1.0f, 1.0f, normalized );
	const float absValue = idMath::Fabs( normalized );
	if ( absValue <= deadZone ) {
		return 0;
	}
	const float adjusted = ( absValue - deadZone ) / ( 1.0f - deadZone );
	const float signedAdjusted = ( normalized < 0.0f ) ? -adjusted : adjusted;
	return idMath::ClampInt( -127, 127, (int)idMath::Rint( signedAdjusted * 127.0f ) );
}

static void Switch_ClearAxes( void ) {
	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	memset( s_joystickAxisState, 0, sizeof( s_joystickAxisState ) );
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
}

static void Switch_ReleaseAllButtons( void ) {
	for ( int i = 0; i < NUM_BUTTON_MAPS; i++ ) {
		if ( s_pressedAs[i] ) {
			Switch_PostKey( s_pressedAs[i], false );
			s_pressedAs[i] = 0;
		}
	}
	s_buttonsDown = 0;
}

static idUserInterface *Switch_ActiveGUI( void ) {
	return session ? session->GetActiveGUI() : NULL;
}

/*
================
per-frame updates
================
*/

static void Switch_UpdateButtons( u64 held ) {
	const u64 changed = held ^ s_buttonsDown;
	if ( !changed ) {
		return;
	}
	for ( int i = 0; i < NUM_BUTTON_MAPS; i++ ) {
		const switchButtonMap_t &map = s_buttonMap[i];
		if ( !( changed & map.button ) ) {
			continue;
		}
		if ( held & map.button ) {
			const int key = ( s_menuMode && map.menuKey ) ? map.menuKey : map.gameKey;
			s_pressedAs[i] = key;
			Switch_PostKey( key, true );
		} else if ( s_pressedAs[i] ) {
			Switch_PostKey( s_pressedAs[i], false );
			s_pressedAs[i] = 0;
		}
	}
	s_buttonsDown = held;
}

static void Switch_UpdateMenuCursor( const HidAnalogStickState &left ) {
	const float deadZone = idMath::ClampFloat( 0.0f, 0.95f, in_joystickDeadZone.GetFloat() );
	const float x = Switch_NormalizeStick( left.x, deadZone ) / 127.0f;
	const float y = -Switch_NormalizeStick( left.y, deadZone ) / 127.0f;	// HID y is up-positive
	if ( x == 0.0f && y == 0.0f ) {
		s_cursorRemainderX = s_cursorRemainderY = 0.0f;
		return;
	}
	// quadratic response: fine control near the center, fast at full tilt
	const float speed = in_menuCursorSpeed.GetFloat();
	s_cursorRemainderX += x * idMath::Fabs( x ) * speed;
	s_cursorRemainderY += y * idMath::Fabs( y ) * speed;
	const int dx = (int)s_cursorRemainderX;
	const int dy = (int)s_cursorRemainderY;
	s_cursorRemainderX -= dx;
	s_cursorRemainderY -= dy;
	if ( dx || dy ) {
		Switch_QueEvent( SE_MOUSE, dx, dy, 0, NULL );
	}
}

static void Switch_UpdateTouch( idUserInterface *gui ) {
	HidTouchScreenState touch = {};
	const bool touching = hidGetTouchScreenStates( &touch, 1 ) && touch.count > 0;

	if ( touching && gui ) {
		const float x = idMath::ClampFloat( 0.0f, GUI_VIRTUAL_WIDTH, touch.touches[0].x * GUI_VIRTUAL_WIDTH / SWITCH_SCREEN_WIDTH );
		const float y = idMath::ClampFloat( 0.0f, GUI_VIRTUAL_HEIGHT, touch.touches[0].y * GUI_VIRTUAL_HEIGHT / SWITCH_SCREEN_HEIGHT );
		gui->SetCursor( x, y );
	}
	if ( touching != s_touchDown ) {
		// only start a click inside a GUI; always deliver the release
		if ( !touching || gui ) {
			Switch_PostKey( K_MOUSE1, touching );
			s_touchDown = touching;
		}
	}
}

static void Switch_UpdateGameAxes( const HidAnalogStickState &left, const HidAnalogStickState &right ) {
	const float deadZone = idMath::ClampFloat( 0.0f, 0.95f, in_joystickDeadZone.GetFloat() );
	const int moveX = Switch_NormalizeStick( left.x, deadZone );
	const int moveY = Switch_NormalizeStick( left.y, deadZone );		// HID y is already up-positive
	const int lookX = Switch_NormalizeStick( right.x, deadZone );
	const int lookY = -Switch_NormalizeStick( right.y, deadZone );	// match SDL's down-positive look axis

	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	s_joystickAxisState[AXIS_SIDE] = lookX;
	s_joystickAxisState[AXIS_FORWARD] = lookY;
	s_joystickAxisState[AXIS_UP] = 0;
	s_joystickAxisState[AXIS_ROLL] = 127;
	s_joystickAxisState[AXIS_YAW] = moveX;
	s_joystickAxisState[AXIS_PITCH] = moveY;
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
}

/*
================
Switch_PollInput
================
*/
void Switch_PollInput( void ) {
	if ( !s_inputInitialized ) {
		return;
	}

	padUpdate( &s_pad );

	idUserInterface *gui = Switch_ActiveGUI();
	const bool menuMode = ( gui != NULL );
	if ( menuMode != s_menuMode ) {
		// release everything so no key stays stuck across the mode switch
		Switch_ReleaseAllButtons();
		Switch_ClearAxes();
		s_cursorRemainderX = s_cursorRemainderY = 0.0f;
		s_menuMode = menuMode;
	}

	Switch_UpdateButtons( padGetButtons( &s_pad ) );

	const HidAnalogStickState left = padGetStickPos( &s_pad, 0 );
	const HidAnalogStickState right = padGetStickPos( &s_pad, 1 );

	if ( s_menuMode || !in_joystick.GetBool() ) {
		Switch_ClearAxes();
		if ( s_menuMode ) {
			Switch_UpdateMenuCursor( left );
		}
	} else {
		Switch_UpdateGameAxes( left, right );
	}

	Switch_UpdateTouch( gui );
}

/*
================
Sys_InitInput / Sys_ShutdownInput
================
*/
void Sys_InitInput( void ) {
	padConfigureInput( 1, HidNpadStyleSet_NpadStandard );
	padInitializeDefault( &s_pad );
	hidInitializeTouchScreen();
	memset( s_pressedAs, 0, sizeof( s_pressedAs ) );
	memset( s_joystickAxisState, 0, sizeof( s_joystickAxisState ) );
	s_buttonsDown = 0;
	s_menuMode = false;
	s_touchDown = false;
	s_inputInitialized = true;
}

void Sys_ShutdownInput( void ) {
	s_inputInitialized = false;
}

void Sys_InitScanTable( void ) {
}

unsigned char Sys_GetConsoleKey( bool shifted ) {
	return shifted ? '~' : '`';
}

unsigned char Sys_MapCharForKey( int key ) {
	return (unsigned char)key;
}

void Sys_GrabMouseCursor( bool grabIt ) {
}

bool Sys_IsGameWindowFocused( void ) {
	return appletGetFocusState() == AppletFocusState_InFocus;
}

/*
================
keyboard (controller buttons)
================
*/
int Sys_PollKeyboardInputEvents( void ) {
	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	s_polledKeyboardCount = 0;
	while ( s_keyboardTail != s_keyboardHead && s_polledKeyboardCount < INPUT_QUEUE_SIZE ) {
		s_polledKeyboard[s_polledKeyboardCount++] = s_keyboardQueue[s_keyboardTail];
		s_keyboardTail = ( s_keyboardTail + 1 ) & INPUT_QUEUE_MASK;
	}
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
	return s_polledKeyboardCount;
}

int Sys_ReturnKeyboardInputEvent( const int n, int &ch, bool &state ) {
	if ( n < 0 || n >= s_polledKeyboardCount ) {
		ch = 0;
		state = false;
		return 0;
	}
	ch = s_polledKeyboard[n].key;
	state = s_polledKeyboard[n].down;
	return ch;
}

void Sys_EndKeyboardInputEvents( void ) {
}

/*
================
mouse (menu cursor moves go through SE_MOUSE events instead)
================
*/
int Sys_PollMouseInputEvents( void ) {
	return 0;
}

int Sys_ReturnMouseInputEvent( const int n, int &action, int &value ) {
	action = 0;
	value = 0;
	return 0;
}

void Sys_EndMouseInputEvents( void ) {
}

/*
================
joystick axes
================
*/
int Sys_PollJoystickInputEvents( void ) {
	if ( !in_joystick.GetBool() ) {
		s_polledJoystickCount = 0;
		return 0;
	}
	Sys_EnterCriticalSection( CRITICAL_SECTION_ONE );
	s_polledJoystickCount = 0;
	for ( int axis = 0; axis < MAX_JOYSTICK_AXIS; ++axis ) {
		s_polledJoystick[s_polledJoystickCount].axis = axis;
		s_polledJoystick[s_polledJoystickCount].value = s_joystickAxisState[axis];
		s_polledJoystickCount++;
	}
	Sys_LeaveCriticalSection( CRITICAL_SECTION_ONE );
	return s_polledJoystickCount;
}

int Sys_ReturnJoystickInputEvent( const int n, int &axis, int &value ) {
	if ( n < 0 || n >= s_polledJoystickCount ) {
		axis = 0;
		value = 0;
		return 0;
	}
	axis = s_polledJoystick[n].axis;
	value = s_polledJoystick[n].value;
	return 1;
}

void Sys_EndJoystickInputEvents( void ) {
}
