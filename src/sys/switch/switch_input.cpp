/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

Controller and touch input through libnx HID.

In game, buttons and sticks use the same K_JOY keys and joystick axes as the
Windows SDL3 backend (src/sys/win32/win_sdl3.cpp), so binds carry over:

	L = JOY1   R = JOY2   B = JOY3 (south)   A = JOY4 (east)
	X = JOY5 (north)      Y = JOY6 (west)
	D-pad up/down/right/left = JOY9..JOY12   L3 = JOY13   R3 = JOY14
	ZR = JOY15 (right trigger)               ZL = JOY16 (left trigger)
	+ = Escape (opens and closes the menu)   - = console key

Default binds for those keys are in s_defaultBinds: ZR fire, ZL alt fire, B jump,
Y reload, X next weapon, A spirit walk, R grenade, L lighter, L3 sprint, R3 crouch
(both toggles), D-pad up zoom, down center view, right/left next/previous weapon.

	left stick  -> AXIS_YAW / AXIS_PITCH (move), right stick -> AXIS_SIDE /
	AXIS_FORWARD (look), AXIS_ROLL = 127 marks the dedicated look stick.

The triggers are digital on Switch, so unlike SDL3 they only act as buttons
and do not feed AXIS_UP.

While a GUI is active (menus) the left stick moves the cursor, A clicks
(mouse 1) and B backs out (Escape). Touching the screen places the cursor and
clicks.

While the console is down, A opens the system keyboard to type a command,
the D-pad up/down walks the command history, L/R scroll, and B or - close it.

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
static float						s_cursorRemainderX = 0.0f;
static float						s_cursorRemainderY = 0.0f;
static bool							s_touchDown = false;

typedef enum {
	INPUT_MODE_GAME,
	INPUT_MODE_MENU,		// a GUI is active
	INPUT_MODE_CONSOLE		// the console is down
} switchInputMode_t;

static switchInputMode_t	s_inputMode = INPUT_MODE_GAME;

// pseudo keys, handled here instead of being posted to the engine
static const int KEY_NONE				= 0;
static const int KEY_CONSOLE_TOGGLE		= -1;	// posts the console key
static const int KEY_SOFTWARE_KEYBOARD	= -2;	// opens the system keyboard for a console command

typedef struct {
	u64		button;
	int		gameKey;	// key while playing
	int		menuKey;	// key while a GUI is active (KEY_NONE = same as gameKey)
	int		consoleKey;	// key while the console is down (KEY_NONE = ignored)
} switchButtonMap_t;

static const switchButtonMap_t s_buttonMap[] = {
	{ HidNpadButton_L,			K_JOY1,				KEY_NONE,		K_PGUP },
	{ HidNpadButton_R,			K_JOY2,				KEY_NONE,		K_PGDN },
	{ HidNpadButton_B,			K_JOY3,				K_ESCAPE,		KEY_CONSOLE_TOGGLE },
	{ HidNpadButton_A,			K_JOY4,				K_MOUSE1,		KEY_SOFTWARE_KEYBOARD },
	{ HidNpadButton_X,			K_JOY5,				KEY_NONE,		KEY_NONE },
	{ HidNpadButton_Y,			K_JOY6,				KEY_NONE,		KEY_NONE },
	{ HidNpadButton_Plus,		K_ESCAPE,			KEY_NONE,		KEY_NONE },
	{ HidNpadButton_Minus,		KEY_CONSOLE_TOGGLE,	KEY_NONE,		KEY_CONSOLE_TOGGLE },
	{ HidNpadButton_Up,			K_JOY9,				K_UPARROW,		K_UPARROW },
	{ HidNpadButton_Down,		K_JOY10,			K_DOWNARROW,	K_DOWNARROW },
	{ HidNpadButton_Right,		K_JOY11,			K_RIGHTARROW,	KEY_NONE },
	{ HidNpadButton_Left,		K_JOY12,			K_LEFTARROW,	KEY_NONE },
	{ HidNpadButton_StickL,		K_JOY13,			KEY_NONE,		KEY_NONE },
	{ HidNpadButton_StickR,		K_JOY14,			KEY_NONE,		KEY_NONE },
	{ HidNpadButton_ZR,			K_JOY15,			KEY_NONE,		KEY_NONE },
	{ HidNpadButton_ZL,			K_JOY16,			KEY_NONE,		KEY_NONE },
};

/*
Default controller scheme, modeled on current console shooters (fire on the
right trigger, jump on the bottom face button, click the sticks to sprint and
crouch). Switch_ApplyDefaultBinds applies it to keys the player has not bound,
so anything rebound from the menu or console is kept.

Schemes are versioned: when in_switchControlScheme is older than the current
one, keys still holding the previous scheme's default move to the new one.
*/
typedef struct {
	int				key;
	const char *	command;		// current scheme
	const char *	previous;		// scheme 1 default for this key (NULL = none)
} switchDefaultBind_t;

static const int SWITCH_CONTROL_SCHEME = 2;

static const switchDefaultBind_t s_defaultBinds[] = {
	{ K_JOY15,	"_attack",		"_attack" },		// ZR: fire
	{ K_JOY16,	"_attackalt",	"_attackalt" },		// ZL: alternate fire
	{ K_JOY3,	"_moveUp",		"_moveUp" },		// B: jump
	{ K_JOY6,	"_impulse13",	"_impulse54" },		// Y: reload
	{ K_JOY5,	"_impulse14",	"_impulse16" },		// X: next weapon
	{ K_JOY4,	"_impulse54",	"_impulse13" },		// A: spirit walk
	{ K_JOY2,	"_impulse25",	"_impulse14" },		// R: throw grenade
	{ K_JOY1,	"_impulse16",	"_impulse15" },		// L: lighter
	{ K_JOY13,	"_speed",		"_speed" },			// L3: sprint (toggle, in_toggleRun)
	{ K_JOY14,	"_moveDown",	"_zoom" },			// R3: crouch (toggle, in_toggleCrouch)
	{ K_JOY9,	"_zoom",		"_impulse25" },		// D-pad up: zoom (toggle, in_toggleZoom)
	{ K_JOY10,	"_impulse18",	"_moveDown" },		// D-pad down: center view
	{ K_JOY11,	"_impulse14",	"_impulse14" },		// D-pad right: next weapon
	{ K_JOY12,	"_impulse15",	"_impulse15" },		// D-pad left: previous weapon
};

// Clicking a stick is awkward to hold, so sprint, crouch and zoom toggle.
static const char *s_schemeCvars[][2] = {
	{ "in_toggleRun",		"1" },
	{ "in_toggleCrouch",	"1" },
	{ "in_toggleZoom",		"1" },
};

static idCVar in_switchControlScheme( "in_switchControlScheme", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NOCHEAT, "controller scheme version the binds were last set up for (internal)" );
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

/*
================
Switch_ConsoleKeyboard

Asks for a console command with the system keyboard applet and types it
into the console, followed by Enter.
================
*/
static void Switch_ConsoleKeyboard( void ) {
	SwkbdConfig kbd;
	char text[256] = {};
	if ( R_FAILED( swkbdCreate( &kbd, 0 ) ) ) {
		return;
	}
	swkbdConfigMakePresetDefault( &kbd );
	swkbdConfigSetHeaderText( &kbd, "OpenPrey console" );
	swkbdConfigSetGuideText( &kbd, "Command, e.g. com_showFPS 1" );
	const Result rc = swkbdShow( &kbd, text, sizeof( text ) );
	swkbdClose( &kbd );
	if ( R_FAILED( rc ) || !text[0] ) {
		return;
	}
	for ( const char *c = text; *c; c++ ) {
		const unsigned char ch = (unsigned char)*c;
		if ( ch >= 32 && ch < 127 ) {	// the console input line is ASCII
			Switch_QueEvent( SE_CHAR, ch, 0, 0, NULL );
		}
	}
	Switch_PostKey( K_ENTER, true );
	Switch_PostKey( K_ENTER, false );
}

static int Switch_KeyForMode( const switchButtonMap_t &map ) {
	switch ( s_inputMode ) {
		case INPUT_MODE_CONSOLE:
			return map.consoleKey;
		case INPUT_MODE_MENU:
			return map.menuKey != KEY_NONE ? map.menuKey : map.gameKey;
		default:
			return map.gameKey;
	}
}

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
			int key = Switch_KeyForMode( map );
			if ( key == KEY_SOFTWARE_KEYBOARD ) {
				Switch_ConsoleKeyboard();
				key = KEY_NONE;
			} else if ( key == KEY_CONSOLE_TOGGLE ) {
				key = Sys_GetConsoleKey( false );
			}
			s_pressedAs[i] = key;
			Switch_PostKey( key, true );
		} else if ( s_pressedAs[i] ) {
			Switch_PostKey( s_pressedAs[i], false );
			s_pressedAs[i] = 0;
		}
	}
	s_buttonsDown = held;
}

/*
================
Switch_ApplyDefaultBinds

Called once after common->Init, when the saved config has been executed.
================
*/
void Switch_ApplyDefaultBinds( void ) {
	const bool upgrading = in_switchControlScheme.GetInteger() < SWITCH_CONTROL_SCHEME;

	for ( size_t i = 0; i < sizeof( s_defaultBinds ) / sizeof( s_defaultBinds[0] ); i++ ) {
		const switchDefaultBind_t &bind = s_defaultBinds[i];
		const char *current = idKeyInput::GetBinding( bind.key );
		const bool unbound = !current || !current[0];
		// on a scheme upgrade, also replace binds the player never changed
		const bool stillPreviousDefault = upgrading && !unbound && bind.previous && idStr::Icmp( current, bind.previous ) == 0;
		if ( unbound || stillPreviousDefault ) {
			idKeyInput::SetBinding( bind.key, bind.command );
		}
	}

	if ( upgrading ) {
		for ( size_t i = 0; i < sizeof( s_schemeCvars ) / sizeof( s_schemeCvars[0] ); i++ ) {
			cvarSystem->SetCVarString( s_schemeCvars[i][0], s_schemeCvars[i][1] );
		}
		in_switchControlScheme.SetInteger( SWITCH_CONTROL_SCHEME );
	}
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
	switchInputMode_t mode = INPUT_MODE_GAME;
	if ( console && console->Active() ) {
		mode = INPUT_MODE_CONSOLE;
	} else if ( gui != NULL ) {
		mode = INPUT_MODE_MENU;
	}
	if ( mode != s_inputMode ) {
		// release everything so no key stays stuck across the mode switch
		Switch_ReleaseAllButtons();
		Switch_ClearAxes();
		s_cursorRemainderX = s_cursorRemainderY = 0.0f;
		s_inputMode = mode;
		// keep a button held across the switch from firing again in the new mode
		s_buttonsDown = padGetButtons( &s_pad );
	}

	Switch_UpdateButtons( padGetButtons( &s_pad ) );

	const HidAnalogStickState left = padGetStickPos( &s_pad, 0 );
	const HidAnalogStickState right = padGetStickPos( &s_pad, 1 );

	if ( s_inputMode != INPUT_MODE_GAME || !in_joystick.GetBool() ) {
		Switch_ClearAxes();
		if ( s_inputMode == INPUT_MODE_MENU ) {
			Switch_UpdateMenuCursor( left );
		}
	} else {
		Switch_UpdateGameAxes( left, right );
	}

	Switch_UpdateTouch( s_inputMode == INPUT_MODE_MENU ? gui : NULL );
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
	s_inputMode = INPUT_MODE_GAME;
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
