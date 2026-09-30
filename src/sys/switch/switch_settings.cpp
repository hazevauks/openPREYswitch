/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

Settings menu: the common options (frame counter, frame rate lock, shadows,
gyro, look speed...) without typing console commands.

- It is an overlay the engine draws itself (idSessionLocal::Draw calls
  Switch_DrawSettingsMenu after the console), so no retail GUI file is
  changed or shipped.
- The - button opens and closes it (switch_input.cpp); inside, the D-pad and
  A change settings, B closes, Y opens the console.
- Opened during play, it also opens Prey's pause menu, so the game waits
  behind it, and closing it returns to the game. It does not go through
  Escape, which would skip a playing cutscene.
- Changes apply at once; the cvars are archived, so they are saved with the
  config.

Settings defaults (Switch_ApplySettingsDefaults) are versioned like the
control scheme in switch_input.cpp: an entry applies once to configs saved
before it existed, so the player's later choices stick.

===========================================================================
*/

#include "../../idlib/precompiled.h"

#include "switch_local.h"

/*
================
settings list
================
*/
typedef enum {
	ITEM_TOGGLE,		// bool cvar
	ITEM_CHOICE,		// integer cvar, one of the listed values
	ITEM_RANGE,			// float cvar stepped between min and max
	ITEM_CONSOLE,		// closes the menu and opens the console
	ITEM_CLOSE
} settingsItemType_t;

typedef struct {
	const char *			label;
	const char *			help;			// one line under the list
	settingsItemType_t		type;
	const char *			cvar;
	const char *			cvar2;			// kept equal to cvar (gyro X/Y, yaw/pitch speed)
	int						numChoices;
	const int *				choiceValues;
	const char * const *	choiceLabels;
	float					min, max, step;
	const char *			format;			// ITEM_RANGE value
} settingsItem_t;

static const int			s_fpsLockValues[] = { 30, 0 };
static const char * const	s_fpsLockLabels[] = { "30 fps", "Off" };
static const int			s_clockValues[] = { 0, 1, 2, 3 };
static const char * const	s_clockLabels[] = { "System default", "GPU 384 MHz", "GPU 460 MHz", "GPU 460 + RAM 1600" };
static const int			s_gyroValues[] = { 0, 1, 2 };
static const char * const	s_gyroLabels[] = { "Off", "Always", "While aiming (ZL)" };

#define CHOICES( name )		sizeof( name##Values ) / sizeof( name##Values[0] ), name##Values, name##Labels

static const settingsItem_t s_items[] = {
	{ "Show FPS",			"Frame counter in the corner of the screen.",		ITEM_TOGGLE,	"com_showFPS" },
	{ "Frame rate lock",	"Holds a steady 30 fps. Off: as fast as it can.",	ITEM_CHOICE,	"r_fpsLock", NULL, CHOICES( s_fpsLock ) },
	{ "Shadows",			"Stencil shadows. Off is much faster on Switch.",	ITEM_TOGGLE,	"r_shadows" },
	{ "Dynamic resolution",	"Lowers the 3D resolution in heavy scenes.",		ITEM_TOGGLE,	"r_dynamicResolution" },
	{ "GPU clock profile",	"Handheld GPU and memory clocks.",				ITEM_CHOICE,	"r_switchPerfProfile", NULL, CHOICES( s_clock ) },
	{ "Gyro aiming",		"Aim by moving the controller.",					ITEM_CHOICE,	"in_gyro", NULL, CHOICES( s_gyro ) },
	{ "Gyro sensitivity",	"Camera degrees per degree the controller turns.",	ITEM_RANGE,		"in_gyroSensitivityX", "in_gyroSensitivityY", 0, NULL, NULL, 0.25f, 6.0f, 0.25f, "%.2f" },
	{ "Look speed",			"Right stick turn speed, degrees per second.",		ITEM_RANGE,		"in_yawspeed", "in_pitchspeed", 0, NULL, NULL, 60.0f, 400.0f, 20.0f, "%.0f" },
	{ "Invert look",		"Right stick up looks down.",						ITEM_TOGGLE,	"in_joystickInvertLook" },
	{ "Subtitles",			"Dialogue subtitles.",								ITEM_TOGGLE,	"g_subtitles" },
	{ "Console",			"Developer console (commands and cvars).",			ITEM_CONSOLE },
	{ "Close",				"",													ITEM_CLOSE },
};
static const int NUM_ITEMS = sizeof( s_items ) / sizeof( s_items[0] );

/*
================
settings defaults

version: the settings version that introduced the default. Configs saved with
an older com_switchSettings get it once.
================
*/
typedef struct {
	int				version;
	const char *	cvar;
	const char *	value;
} settingsDefault_t;

static const settingsDefault_t s_settingsDefaults[] = {
	{ 1,	"r_shadows",	"0" },		// shadows cost ~40% of the frame in busy maps (switch-port.md)
};
static const int SWITCH_SETTINGS_VERSION = 1;

static idCVar com_switchSettings( "com_switchSettings", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NOCHEAT, "settings defaults version the config was last updated to (internal)" );

void Switch_ApplySettingsDefaults( void ) {
	const int saved = com_switchSettings.GetInteger();
	if ( saved >= SWITCH_SETTINGS_VERSION ) {
		return;
	}
	for ( size_t i = 0; i < sizeof( s_settingsDefaults ) / sizeof( s_settingsDefaults[0] ); i++ ) {
		if ( s_settingsDefaults[i].version > saved ) {
			cvarSystem->SetCVarString( s_settingsDefaults[i].cvar, s_settingsDefaults[i].value );
		}
	}
	com_switchSettings.SetInteger( SWITCH_SETTINGS_VERSION );
}

/*
================
menu state
================
*/
static bool					s_menuOpen = false;
static int					s_selected = 0;
static idUserInterface *	s_pauseMenu = NULL;		// pause menu this menu opened, closed with it

// items whose cvar is missing are skipped (g_subtitles lives in the game module)
static bool Switch_ItemAvailable( const settingsItem_t &item ) {
	return item.cvar == NULL || cvarSystem->Find( item.cvar ) != NULL;
}

bool Switch_SettingsMenuActive( void ) {
	return s_menuOpen;
}

void Switch_OpenSettingsMenu( void ) {
	if ( s_menuOpen || Switch_IsLoading() ) {
		return;
	}
	s_menuOpen = true;
	if ( !Switch_ItemAvailable( s_items[s_selected] ) ) {
		s_selected = 0;
	}

	// pause single player behind Prey's own pause menu
	s_pauseMenu = NULL;
	if ( session != NULL && !session->IsGUIActive() && !session->IsMultiplayer() ) {
		session->StartMenu();
		s_pauseMenu = session->GetActiveGUI();
	}
}

void Switch_CloseSettingsMenu( bool resumeGame ) {
	if ( !s_menuOpen ) {
		return;
	}
	s_menuOpen = false;

	// back to the game if the pause menu is still the one this menu opened;
	// Escape closes it the way + does
	if ( resumeGame && s_pauseMenu != NULL && session != NULL && session->GetActiveGUI() == s_pauseMenu ) {
		Switch_QueEvent( SE_KEY, K_ESCAPE, 1, 0, NULL );
		Switch_QueEvent( SE_KEY, K_ESCAPE, 0, 0, NULL );
	}
	s_pauseMenu = NULL;
}

/*
================
value access
================
*/
static int Switch_ChoiceIndex( const settingsItem_t &item ) {
	const int value = cvarSystem->GetCVarInteger( item.cvar );
	for ( int i = 0; i < item.numChoices; i++ ) {
		if ( item.choiceValues[i] == value ) {
			return i;
		}
	}
	return -1;
}

static void Switch_ChangeItem( const settingsItem_t &item, int direction ) {
	switch ( item.type ) {
		case ITEM_TOGGLE:
			cvarSystem->SetCVarBool( item.cvar, !cvarSystem->GetCVarBool( item.cvar ) );
			break;
		case ITEM_CHOICE: {
			int index = Switch_ChoiceIndex( item );
			if ( index < 0 ) {
				index = 0;		// a value set from the console that is not listed
			} else {
				index = ( index + direction + item.numChoices ) % item.numChoices;
			}
			cvarSystem->SetCVarInteger( item.cvar, item.choiceValues[index] );
			break;
		}
		case ITEM_RANGE: {
			float value = cvarSystem->GetCVarFloat( item.cvar );
			// snap to the step grid, then move one step
			value = item.min + idMath::Rint( ( value - item.min ) / item.step ) * item.step + direction * item.step;
			value = idMath::ClampFloat( item.min, item.max, value );
			cvarSystem->SetCVarFloat( item.cvar, value );
			if ( item.cvar2 ) {
				cvarSystem->SetCVarFloat( item.cvar2, value );
			}
			break;
		}
		default:
			break;
	}
}

static const char *Switch_ItemValue( const settingsItem_t &item ) {
	switch ( item.type ) {
		case ITEM_TOGGLE:
			return cvarSystem->GetCVarBool( item.cvar ) ? "On" : "Off";
		case ITEM_CHOICE: {
			const int index = Switch_ChoiceIndex( item );
			return index >= 0 ? item.choiceLabels[index] : va( "%d", cvarSystem->GetCVarInteger( item.cvar ) );
		}
		case ITEM_RANGE:
			return va( item.format, cvarSystem->GetCVarFloat( item.cvar ) );
		default:
			return "";
	}
}

/*
================
Switch_SettingsMenuButton

A button pressed while the menu is open. The menu takes every button.
================
*/
static void Switch_MoveSelection( int direction ) {
	for ( int i = 0; i < NUM_ITEMS; i++ ) {
		s_selected = ( s_selected + direction + NUM_ITEMS ) % NUM_ITEMS;
		if ( Switch_ItemAvailable( s_items[s_selected] ) ) {
			return;
		}
	}
}

static void Switch_SettingsOpenConsole( void ) {
	Switch_CloseSettingsMenu( false );		// the game stays paused under the console
	const int consoleKey = Sys_GetConsoleKey( false );
	Switch_QueEvent( SE_KEY, consoleKey, 1, 0, NULL );
	Switch_QueEvent( SE_KEY, consoleKey, 0, 0, NULL );
}

void Switch_SettingsMenuButton( settingsButton_t button ) {
	const settingsItem_t &item = s_items[s_selected];
	switch ( button ) {
		case SETTINGS_UP:
			Switch_MoveSelection( -1 );
			break;
		case SETTINGS_DOWN:
			Switch_MoveSelection( 1 );
			break;
		case SETTINGS_LEFT:
			Switch_ChangeItem( item, -1 );
			break;
		case SETTINGS_RIGHT:
			Switch_ChangeItem( item, 1 );
			break;
		case SETTINGS_ACCEPT:
			if ( item.type == ITEM_CONSOLE ) {
				Switch_SettingsOpenConsole();
			} else if ( item.type == ITEM_CLOSE ) {
				Switch_CloseSettingsMenu( true );
			} else {
				Switch_ChangeItem( item, 1 );
			}
			break;
		case SETTINGS_BACK:
			Switch_CloseSettingsMenu( true );
			break;
		case SETTINGS_CONSOLE:
			Switch_SettingsOpenConsole();
			break;
	}
}

/*
================
Switch_DrawSettingsMenu

Drawn in the 640x480 virtual screen with the console font (8x16 characters).
================
*/
static const float	MENU_X = 100.0f;
static const float	MENU_Y = 64.0f;
static const float	MENU_W = 440.0f;
static const float	MENU_TITLE_H = 26.0f;
static const float	MENU_ROW_H = 20.0f;
static const int	CHAR_W = 8;

void Switch_DrawSettingsMenu( void ) {
	if ( !s_menuOpen ) {
		return;
	}
	const idMaterial *white = declManager->FindMaterial( "_white" );
	const idMaterial *font = declManager->FindMaterial( "textures/bigchars" );
	const idVec4 textColor( 0.85f, 0.85f, 0.85f, 1.0f );
	const idVec4 valueColor( 0.55f, 0.80f, 1.0f, 1.0f );
	const idVec4 helpColor( 0.65f, 0.65f, 0.65f, 1.0f );

	int rows = 0;
	for ( int i = 0; i < NUM_ITEMS; i++ ) {
		if ( Switch_ItemAvailable( s_items[i] ) ) {
			rows++;
		}
	}
	const float listY = MENU_Y + MENU_TITLE_H + 8.0f;
	const float helpY = listY + rows * MENU_ROW_H + 10.0f;
	const float hintY = helpY + 26.0f;
	const float menuH = hintY + 24.0f - MENU_Y;

	// panel and title bar
	renderSystem->SetColor4( 0.0f, 0.0f, 0.0f, 0.85f );
	renderSystem->DrawStretchPic( MENU_X, MENU_Y, MENU_W, menuH, 0, 0, 1, 1, white );
	renderSystem->SetColor4( 0.12f, 0.30f, 0.55f, 0.95f );
	renderSystem->DrawStretchPic( MENU_X, MENU_Y, MENU_W, MENU_TITLE_H, 0, 0, 1, 1, white );
	renderSystem->DrawSmallStringExt( (int)MENU_X + 12, (int)MENU_Y + 5, "OpenPrey settings", colorWhite, true, font );

	float y = listY;
	for ( int i = 0; i < NUM_ITEMS; i++ ) {
		const settingsItem_t &item = s_items[i];
		if ( !Switch_ItemAvailable( item ) ) {
			continue;
		}
		if ( i == s_selected ) {
			renderSystem->SetColor4( 0.25f, 0.50f, 0.85f, 0.55f );
			renderSystem->DrawStretchPic( MENU_X + 4, y, MENU_W - 8, MENU_ROW_H, 0, 0, 1, 1, white );
		}
		renderSystem->DrawSmallStringExt( (int)MENU_X + 16, (int)y + 2, item.label, i == s_selected ? colorWhite : textColor, true, font );
		const char *value = Switch_ItemValue( item );
		if ( value[0] ) {
			const int valueX = (int)( MENU_X + MENU_W ) - 16 - idStr::Length( value ) * CHAR_W;
			renderSystem->DrawSmallStringExt( valueX, (int)y + 2, value, valueColor, true, font );
		}
		y += MENU_ROW_H;
	}

	renderSystem->DrawSmallStringExt( (int)MENU_X + 16, (int)helpY, s_items[s_selected].help, helpColor, true, font );
	renderSystem->DrawSmallStringExt( (int)MENU_X + 16, (int)hintY, "D-pad/A: change   B: close   Y: console", helpColor, true, font );
	renderSystem->SetColor( colorWhite );
}
