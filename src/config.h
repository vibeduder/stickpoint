#pragma once

/*
 * StickPoint — tunable constants
 * Adjust these to taste before building.
 */

/* Poll rate ---------------------------------------------------------------- */
#define POLL_INTERVAL_MS        8       /* ~125 Hz polling loop              */

/* Stick deadzone ----------------------------------------------------------- */
/* Raw axis value (0–32767). Values below this are treated as zero.          */
#define STICK_DEADZONE          8000

/* Cursor speed ------------------------------------------------------------- */
/* Max pixels per second when the stick is fully deflected.                  */
#define MOUSE_MAX_SPEED         1200.0f

/*
 * Acceleration exponent applied after deadzone normalisation.
 *   1.0 → linear (constant speed ratio)
 *   1.5 → gentler at low deflection, faster at full deflection (default)
 *   2.0 → very precise at low deflection
 */
#define MOUSE_ACCEL_EXPONENT    1.5f

/* Scroll speed ------------------------------------------------------------- */
/* WHEEL_DELTA ticks per second at full stick/bumper deflection.             */
#define SCROLL_SPEED            3.0f

/* Trigger click threshold -------------------------------------------------- */
/* Trigger value (0–255) above which the trigger counts as a click.          */
#define TRIGGER_THRESHOLD       200

/* Mode-toggle combo -------------------------------------------------------- */
/*
 * To enter/exit mouse mode: hold the Guide button then press A within this
 * window (milliseconds).
 */
#define COMBO_TIMEOUT_MS        500

/* Double-click window ------------------------------------------------------ */
/* Two A presses within this window are treated as a double-click (ms).      */
#define DOUBLE_CLICK_MS         350

/* Guide-hold exit ---------------------------------------------------------- */
/* Hold Guide alone for this long (ms) to exit mouse mode without pressing A. */
#define GUIDE_HOLD_EXIT_MS      1200
