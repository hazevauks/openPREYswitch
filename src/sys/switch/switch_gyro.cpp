/*
===========================================================================

OpenPrey - Nintendo Switch platform layer (devkitPro libnx)

Gyro aiming, in the style of console shooters with motion controls: turning
the console (or controller) turns the camera by the same angle times a
sensitivity, on top of the right stick.

The rotation is fed through the engine's mouse path (M_DELTAX/M_DELTAY), which
applies exact angles: yaw -= m_yaw * sensitivity * dx and pitch +=
m_pitch * sensitivity * dy. Degrees are converted to mouse counts with the
live values of those cvars, keeping the fractional remainder.

"Player space" gyro: yaw is the rotation around the world's vertical axis,
found from gravity (the accelerometer at rest reads +1 G pointing up), so aiming
behaves the same with the console upright, tilted, or a Pro Controller lying
flat. Pitch is the rotation around the controller's right axis.

Six-axis units (libnx HidSixAxisSensorState): angular velocity in rotations per
second (1.0 = 360 deg/s), acceleration in G. Device axes are assumed to be
x = right, y = up, z = toward the player (yaw sign confirmed on hardware);
in_gyroInvertX / in_gyroInvertY flip
the result if a controller reports them otherwise, and in_gyroDebug 1 prints
raw values once per second to check.

cvars:
	in_gyro					0 = off (default), 1 = always, 2 = only while ZL (aim) is held
	in_gyroSensitivityX		camera degrees per degree of controller yaw
	in_gyroSensitivityY		camera degrees per degree of controller pitch
	in_gyroDeadZone			ignore rotation slower than this, in degrees per second
	in_gyroInvertX/Y		flip an axis
	in_gyroDebug			print raw sensor values once per second

===========================================================================
*/

#include "../../idlib/precompiled.h"

// idlib and libnx both define BIT(); they compute the same value, so let libnx own it here.
#undef BIT
#include <switch.h>

#include "switch_local.h"

static idCVar in_gyro( "in_gyro", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_INTEGER, "gyro aiming: 0 = off, 1 = always, 2 = only while ZL (aim) is held", 0, 2 );
static idCVar in_gyroSensitivityX( "in_gyroSensitivityX", "2.0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "gyro yaw: camera degrees per degree the controller turns", 0.0f, 20.0f );
static idCVar in_gyroSensitivityY( "in_gyroSensitivityY", "2.0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "gyro pitch: camera degrees per degree the controller tilts", 0.0f, 20.0f );
static idCVar in_gyroDeadZone( "in_gyroDeadZone", "1.0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT, "ignore gyro rotation slower than this, in degrees per second (hand tremor, sensor drift)", 0.0f, 20.0f );
static idCVar in_gyroInvertX( "in_gyroInvertX", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "invert gyro yaw" );
static idCVar in_gyroInvertY( "in_gyroInvertY", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL, "invert gyro pitch" );
static idCVar in_gyroDebug( "in_gyroDebug", "0", CVAR_SYSTEM | CVAR_BOOL, "print raw gyro/accelerometer values once per second" );

static const int	GYRO_MAX_SAMPLES = 17;		// size of the HID six-axis ring buffer

typedef enum {
	GYRO_HANDHELD,
	GYRO_FULLKEY,			// Pro Controller
	GYRO_JOYDUAL_RIGHT,		// two Joy-Cons in the grip / held apart: aim with the right one
	GYRO_NUM_SOURCES
} gyroSource_t;

static HidSixAxisSensorHandle	s_handles[GYRO_NUM_SOURCES];
static bool						s_haveHandle[GYRO_NUM_SOURCES];
static bool						s_started[GYRO_NUM_SOURCES];
static bool						s_initialized = false;
static int						s_sensorsWanted = -1;		// last in_gyro on/off state applied (-1 = none)

static int		s_activeSource = -1;
static u64		s_lastSampling = 0;
static int		s_lastPollTime = 0;
static idVec3	s_up( 0.0f, 1.0f, 0.0f );		// low-passed "up" from the accelerometer, device space
static float	s_remainderX = 0.0f;			// fractional mouse counts carried over
static float	s_remainderY = 0.0f;
static int		s_lastDebugTime = 0;

/*
================
Switch_InitGyro
================
*/
void Switch_InitGyro( void ) {
	memset( s_started, 0, sizeof( s_started ) );

	HidSixAxisSensorHandle joyDual[2];
	const bool haveHandheld = R_SUCCEEDED( hidGetSixAxisSensorHandles( &s_handles[GYRO_HANDHELD], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld ) );
	const bool haveFullKey = R_SUCCEEDED( hidGetSixAxisSensorHandles( &s_handles[GYRO_FULLKEY], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey ) );
	const bool haveJoyDual = R_SUCCEEDED( hidGetSixAxisSensorHandles( joyDual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual ) );
	if ( haveJoyDual ) {
		s_handles[GYRO_JOYDUAL_RIGHT] = joyDual[1];
	}

	s_haveHandle[GYRO_HANDHELD] = haveHandheld;
	s_haveHandle[GYRO_FULLKEY] = haveFullKey;
	s_haveHandle[GYRO_JOYDUAL_RIGHT] = haveJoyDual;

	// the sensors start on demand (Switch_SetSensorsRunning), only while in_gyro is on
	s_sensorsWanted = -1;
	s_activeSource = -1;
	s_initialized = true;
}

static void Switch_SetSensorsRunning( bool run ) {
	for ( int i = 0; i < GYRO_NUM_SOURCES; i++ ) {
		if ( run && !s_started[i] && s_haveHandle[i] ) {
			s_started[i] = R_SUCCEEDED( hidStartSixAxisSensor( s_handles[i] ) );
		} else if ( !run && s_started[i] ) {
			hidStopSixAxisSensor( s_handles[i] );
			s_started[i] = false;
		}
	}
	if ( !run ) {
		s_activeSource = -1;
	}
}

static void Switch_ResetGyroTracking( int source ) {
	s_activeSource = source;
	s_lastSampling = 0;
	s_lastPollTime = Sys_Milliseconds();
	s_remainderX = s_remainderY = 0.0f;
}

/*
================
Switch_UpdateGyro
================
*/
void Switch_UpdateGyro( bool gameplay, bool aimHeld, bool handheld, unsigned int npadStyleSet ) {
	if ( !s_initialized ) {
		return;
	}

	// keep the sensors off entirely while gyro aiming is disabled (switch only on changes:
	// each start/stop is a service call, and a missing controller would fail every frame)
	const int wanted = ( in_gyro.GetInteger() != 0 ) ? 1 : 0;
	if ( wanted != s_sensorsWanted ) {
		s_sensorsWanted = wanted;
		Switch_SetSensorsRunning( wanted != 0 );
	}
	if ( !wanted ) {
		return;
	}

	int source = -1;
	if ( handheld ) {
		source = GYRO_HANDHELD;
	} else if ( npadStyleSet & HidNpadStyleTag_NpadFullKey ) {
		source = GYRO_FULLKEY;
	} else if ( npadStyleSet & HidNpadStyleTag_NpadJoyDual ) {
		source = GYRO_JOYDUAL_RIGHT;
	}
	if ( source < 0 || !s_started[source] ) {
		s_activeSource = -1;
		return;
	}
	if ( source != s_activeSource ) {
		Switch_ResetGyroTracking( source );
	}

	HidSixAxisSensorState states[GYRO_MAX_SAMPLES];
	const size_t count = hidGetSixAxisSensorStates( s_handles[source], states, GYRO_MAX_SAMPLES );

	const int now = Sys_Milliseconds();
	const float elapsedSec = idMath::ClampFloat( 0.0f, 0.1f, ( now - s_lastPollTime ) * 0.001f );	// cap hitches
	s_lastPollTime = now;

	// average the samples that are new since the last poll (states[0] is the newest)
	idVec3 omega( 0.0f, 0.0f, 0.0f );
	int fresh = 0;
	u64 newest = s_lastSampling;
	for ( size_t i = 0; i < count; i++ ) {
		const HidSixAxisSensorState &s = states[i];
		if ( s.sampling_number <= s_lastSampling ) {
			continue;
		}
		newest = Max( newest, s.sampling_number );
		omega += idVec3( s.angular_velocity.x, s.angular_velocity.y, s.angular_velocity.z );

		// track "up" with a slow low-pass so aiming motion does not bend it
		const idVec3 accel( s.acceleration.x, s.acceleration.y, s.acceleration.z );
		const float g = accel.Length();
		if ( g > 0.5f && g < 1.5f ) {
			s_up = s_up * 0.98f + ( accel / g ) * 0.02f;
			s_up.Normalize();
		}
		fresh++;
	}
	const bool firstPoll = ( s_lastSampling == 0 );
	s_lastSampling = newest;

	if ( in_gyroDebug.GetBool() && count > 0 && now - s_lastDebugTime >= 1000 ) {
		s_lastDebugTime = now;
		common->Printf( "gyro[%d] w=( %.3f %.3f %.3f ) rev/s  a=( %.2f %.2f %.2f ) G  up=( %.2f %.2f %.2f )\n", source,
			states[0].angular_velocity.x, states[0].angular_velocity.y, states[0].angular_velocity.z,
			states[0].acceleration.x, states[0].acceleration.y, states[0].acceleration.z,
			s_up.x, s_up.y, s_up.z );
	}

	const int mode = in_gyro.GetInteger();
	const bool active = gameplay && ( mode == 1 || ( mode == 2 && aimHeld ) );
	if ( !active || fresh == 0 || firstPoll ) {
		s_remainderX = s_remainderY = 0.0f;
		return;
	}

	omega /= (float)fresh;
	const idVec3 omegaDeg = omega * 360.0f;		// rotations/s -> degrees/s

	// player space: yaw around world up; pitch around the controller's right axis
	// + = turn left. The HID six-axis frame is left-handed for this projection:
	// hardware test showed yaw reversed with ( omega . up ), so it is negated here.
	float yawRate = -( omegaDeg * s_up );
	float pitchRate = omegaDeg.x;			// + = top edge toward the player = aim up

	const float deadZone = in_gyroDeadZone.GetFloat();
	if ( idMath::Fabs( yawRate ) < deadZone ) {
		yawRate = 0.0f;
	}
	if ( idMath::Fabs( pitchRate ) < deadZone ) {
		pitchRate = 0.0f;
	}
	if ( yawRate == 0.0f && pitchRate == 0.0f ) {
		return;
	}

	float yawDeg = yawRate * elapsedSec * in_gyroSensitivityX.GetFloat();		// + = turn left
	float pitchUpDeg = pitchRate * elapsedSec * in_gyroSensitivityY.GetFloat();	// + = aim up
	if ( in_gyroInvertX.GetBool() ) {
		yawDeg = -yawDeg;
	}
	if ( in_gyroInvertY.GetBool() ) {
		pitchUpDeg = -pitchUpDeg;
	}

	// degrees -> mouse counts: yaw -= m_yaw * sensitivity * dx, pitch += m_pitch * sensitivity * dy
	const float mouseSensitivity = cvarSystem->GetCVarFloat( "sensitivity" );
	const float yawPerCount = cvarSystem->GetCVarFloat( "m_yaw" ) * mouseSensitivity;
	const float pitchPerCount = cvarSystem->GetCVarFloat( "m_pitch" ) * mouseSensitivity;
	if ( idMath::Fabs( yawPerCount ) < 1e-6f || idMath::Fabs( pitchPerCount ) < 1e-6f ) {
		return;
	}

	s_remainderX += -yawDeg / yawPerCount;			// turning left = negative mouse x
	s_remainderY += -pitchUpDeg / pitchPerCount;	// aiming up = negative pitch = negative mouse y

	const int dx = (int)s_remainderX;
	const int dy = (int)s_remainderY;
	s_remainderX -= dx;
	s_remainderY -= dy;
	if ( dx || dy ) {
		Switch_QueueMouseDelta( dx, dy );
	}
}

/*
================
Switch_ShutdownGyro

Stops every six-axis sensor started by Switch_InitGyro. Leaving them running
when the process ends crashed the system on "Closing software".
================
*/
void Switch_ShutdownGyro( void ) {
	if ( !s_initialized ) {
		return;
	}
	for ( int i = 0; i < GYRO_NUM_SOURCES; i++ ) {
		if ( s_started[i] ) {
			hidStopSixAxisSensor( s_handles[i] );
			s_started[i] = false;
		}
	}
	s_sensorsWanted = -1;
	s_activeSource = -1;
	s_initialized = false;
}
