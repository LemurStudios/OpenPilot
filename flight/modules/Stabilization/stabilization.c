/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup StabilizationModule Stabilization Module
 * @brief Stabilization PID loops in an airframe type independent manner
 * @note This object updates the @ref ActuatorDesired "Actuator Desired" based on the
 * PID loops on the @ref AttitudeDesired "Attitude Desired" and @ref AttitudeState "Attitude State"
 * @{
 *
 * @file       stabilization.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Attitude stabilization module.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <openpilot.h>
#include <pios_struct_helper.h>
#include "stabilization.h"
#include "stabilizationsettings.h"
#include "actuatordesired.h"
#include "ratedesired.h"
#include "relaytuning.h"
#include "relaytuningsettings.h"
#include "stabilizationdesired.h"
#include "attitudestate.h"
#include "airspeedstate.h"
#include "gyrostate.h"
#include "flightstatus.h"
#include "manualcontrol.h" // Just to get a macro
#include "taskinfo.h"

// Math libraries
#include "CoordinateConversions.h"
#include "pid.h"
#include "sin_lookup.h"

// Includes for various stabilization algorithms
#include "relay_tuning.h"
#include "virtualflybar.h"

// Includes for various stabilization algorithms
#include "relay_tuning.h"

// Private constants
#define MAX_QUEUE_SIZE      1

#if defined(PIOS_STABILIZATION_STACK_SIZE)
#define STACK_SIZE_BYTES    PIOS_STABILIZATION_STACK_SIZE
#else
#define STACK_SIZE_BYTES    724
#endif

#define TASK_PRIORITY       (tskIDLE_PRIORITY + 4)
#define FAILSAFE_TIMEOUT_MS 30

// The PID_RATE_ROLL set is used by Rate mode and the rate portion of Attitude mode
// The PID_RATE set is used by the attitude portion of Attitude mode
// The PID_RATEA_ROLL set is used by Rattitude mode because it needs to maintain
// - two independant rate PIDs because it does rate and attitude simultaneously
enum { PID_RATE_ROLL, PID_RATE_PITCH, PID_RATE_YAW, PID_ROLL, PID_PITCH, PID_YAW, PID_RATEA_ROLL, PID_RATEA_PITCH, PID_RATEA_YAW, PID_MAX };

// Private variables
static xTaskHandle taskHandle;
static StabilizationSettingsData settings;
static xQueueHandle queue;
float gyro_alpha = 0;
float axis_lock_accum[3] = { 0, 0, 0 };
uint8_t max_axis_lock     = 0;
uint8_t max_axislock_rate = 0;
float weak_leveling_kp    = 0;
uint8_t weak_leveling_max = 0;
bool lowThrottleZeroIntegral;
bool lowThrottleZeroAxis[MAX_AXES];
float vbar_decay = 0.991f;
struct pid pids[PID_MAX];
static uint8_t rattitude_anti_windup;

// Private functions
static void stabilizationTask(void *parameters);
static float bound(float val, float range);
static void ZeroPids(void);
static void SettingsUpdatedCb(UAVObjEvent *ev);
static float stab_log2f(float x);
static float stab_powf(float x, uint8_t p);

/**
 * Module initialization
 */
int32_t StabilizationStart()
{
    // Initialize variables
    // Create object queue
    queue = xQueueCreate(MAX_QUEUE_SIZE, sizeof(UAVObjEvent));

    // Listen for updates.
    // AttitudeStateConnectQueue(queue);
    GyroStateConnectQueue(queue);

    StabilizationSettingsConnectCallback(SettingsUpdatedCb);
    SettingsUpdatedCb(StabilizationSettingsHandle());

    // Start main task
    xTaskCreate(stabilizationTask, (signed char *)"Stabilization", STACK_SIZE_BYTES / 4, NULL, TASK_PRIORITY, &taskHandle);
    PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_STABILIZATION, taskHandle);
#ifdef PIOS_INCLUDE_WDG
    PIOS_WDG_RegisterFlag(PIOS_WDG_STABILIZATION);
#endif
    return 0;
}

/**
 * Module initialization
 */
int32_t StabilizationInitialize()
{
    // Initialize variables
    StabilizationSettingsInitialize();
    ActuatorDesiredInitialize();
#ifdef DIAG_RATEDESIRED
    RateDesiredInitialize();
#endif
#ifdef REVOLUTION
    AirspeedStateInitialize();
#endif
    // Code required for relay tuning
    sin_lookup_initalize();
    RelayTuningSettingsInitialize();
    RelayTuningInitialize();

    return 0;
}

MODULE_INITCALL(StabilizationInitialize, StabilizationStart);

/**
 * Module task
 */
static void stabilizationTask(__attribute__((unused)) void *parameters)
{
    UAVObjEvent ev;

    uint32_t timeval = PIOS_DELAY_GetRaw();

    ActuatorDesiredData actuatorDesired;
    StabilizationDesiredData stabDesired;
    RateDesiredData rateDesired;
    AttitudeStateData attitudeState;
    GyroStateData gyroStateData;
    FlightStatusData flightStatus;

#ifdef REVOLUTION
    AirspeedStateData airspeedState;
#endif

    SettingsUpdatedCb((UAVObjEvent *)NULL);

    // Main task loop
    ZeroPids();
    while (1) {
        float dT;

#ifdef PIOS_INCLUDE_WDG
        PIOS_WDG_UpdateFlag(PIOS_WDG_STABILIZATION);
#endif

        // Wait until the Gyro object is updated, if a timeout then go to failsafe
        if (xQueueReceive(queue, &ev, FAILSAFE_TIMEOUT_MS / portTICK_RATE_MS) != pdTRUE) {
            AlarmsSet(SYSTEMALARMS_ALARM_STABILIZATION, SYSTEMALARMS_ALARM_WARNING);
            continue;
        }

        dT = PIOS_DELAY_DiffuS(timeval) * 1.0e-6f;
        timeval = PIOS_DELAY_GetRaw();

        FlightStatusGet(&flightStatus);
        StabilizationDesiredGet(&stabDesired);
        AttitudeStateGet(&attitudeState);
        GyroStateGet(&gyroStateData);
#ifdef DIAG_RATEDESIRED
        RateDesiredGet(&rateDesired);
#endif
#ifdef REVOLUTION
        float speedScaleFactor;
        // Scale PID coefficients based on current airspeed estimation - needed for fixed wing planes
        AirspeedStateGet(&airspeedState);
        if (settings.ScaleToAirspeed < 0.1f || airspeedState.CalibratedAirspeed < 0.1f) {
            // feature has been turned off
            speedScaleFactor = 1.0f;
        } else {
            // scale the factor to be 1.0 at the specified airspeed (for example 10m/s) but scaled by 1/speed^2
            speedScaleFactor = (settings.ScaleToAirspeed * settings.ScaleToAirspeed) / (airspeedState.CalibratedAirspeed * airspeedState.CalibratedAirspeed);
            if (speedScaleFactor < settings.ScaleToAirspeedLimits.Min) {
                speedScaleFactor = settings.ScaleToAirspeedLimits.Min;
            }
            if (speedScaleFactor > settings.ScaleToAirspeedLimits.Max) {
                speedScaleFactor = settings.ScaleToAirspeedLimits.Max;
            }
        }
#else
        const float speedScaleFactor = 1.0f;
#endif

#if defined(PIOS_QUATERNION_STABILIZATION)
        // Quaternion calculation of error in each axis.  Uses more memory.
        float rpy_desired[3];
        float q_desired[4];
        float q_error[4];
        float local_error[3];

        // Essentially zero errors for anything in rate or none
        if (stabDesired.StabilizationMode.Roll == STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE) {
            rpy_desired[0] = stabDesired.Roll;
        } else {
            rpy_desired[0] = attitudeState.Roll;
        }

        if (stabDesired.StabilizationMode.Pitch == STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE) {
            rpy_desired[1] = stabDesired.Pitch;
        } else {
            rpy_desired[1] = attitudeState.Pitch;
        }

        if (stabDesired.StabilizationMode.Yaw == STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE) {
            rpy_desired[2] = stabDesired.Yaw;
        } else {
            rpy_desired[2] = attitudeState.Yaw;
        }

        RPY2Quaternion(rpy_desired, q_desired);
        quat_inverse(q_desired);
        quat_mult(q_desired, &attitudeState.q1, q_error);
        quat_inverse(q_error);
        Quaternion2RPY(q_error, local_error);

#else /* if defined(PIOS_QUATERNION_STABILIZATION) */
        // Simpler algorithm for CC, less memory
        float local_error[3] = { stabDesired.Roll  - attitudeState.Roll,
                                 stabDesired.Pitch - attitudeState.Pitch,
                                 stabDesired.Yaw   - attitudeState.Yaw };
        // find shortest way
        float modulo = fmodf(local_error[2] + 180.0f, 360.0f);
        if (modulo < 0) {
            local_error[2] = modulo + 180.0f;
        } else {
            local_error[2] = modulo - 180.0f;
        }
#endif /* if defined(PIOS_QUATERNION_STABILIZATION) */

        float gyro_filtered[3];
        gyro_filtered[0] = gyro_filtered[0] * gyro_alpha + gyroStateData.x * (1 - gyro_alpha);
        gyro_filtered[1] = gyro_filtered[1] * gyro_alpha + gyroStateData.y * (1 - gyro_alpha);
        gyro_filtered[2] = gyro_filtered[2] * gyro_alpha + gyroStateData.z * (1 - gyro_alpha);

        float *stabDesiredAxis     = &stabDesired.Roll;
        float *actuatorDesiredAxis = &actuatorDesired.Roll;
        float *rateDesiredAxis     = &rateDesired.Roll;

        ActuatorDesiredGet(&actuatorDesired);

        // A flag to track which stabilization mode each axis is in
        static uint8_t previous_mode[MAX_AXES] = { 255, 255, 255 };
        bool error = false;

        // Run the selected stabilization algorithm on each axis:
        for (uint8_t i = 0; i < MAX_AXES; i++) {
            // Check whether this axis mode needs to be reinitialized
            bool reinit = (cast_struct_to_array(stabDesired.StabilizationMode, stabDesired.StabilizationMode.Roll)[i] != previous_mode[i]);
            previous_mode[i] = cast_struct_to_array(stabDesired.StabilizationMode, stabDesired.StabilizationMode.Roll)[i];

            // Apply the selected control law
            switch (cast_struct_to_array(stabDesired.StabilizationMode, stabDesired.StabilizationMode.Roll)[i]) {
            case STABILIZATIONDESIRED_STABILIZATIONMODE_RATE:
                if (reinit) {
                    pids[PID_RATE_ROLL + i].iAccumulator = 0;
                }

                // Store to rate desired variable for storing to UAVO
                rateDesiredAxis[i] =
                    bound(stabDesiredAxis[i], cast_struct_to_array(settings.ManualRate, settings.ManualRate.Roll)[i]);

                // Compute the inner loop
                actuatorDesiredAxis[i] = pid_apply_setpoint(&pids[PID_RATE_ROLL + i], speedScaleFactor, rateDesiredAxis[i], gyro_filtered[i], dT);
                actuatorDesiredAxis[i] = bound(actuatorDesiredAxis[i], 1.0f);

                break;

            case STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE:
                if (reinit) {
                    pids[PID_ROLL + i].iAccumulator = 0;
                    pids[PID_RATE_ROLL + i].iAccumulator = 0;
                }

                // Compute the outer loop
                rateDesiredAxis[i] = pid_apply(&pids[PID_ROLL + i], local_error[i], dT);
                rateDesiredAxis[i] = bound(rateDesiredAxis[i],
                                           cast_struct_to_array(settings.MaximumRate, settings.MaximumRate.Roll)[i]);

                // Compute the inner loop
                actuatorDesiredAxis[i] = pid_apply_setpoint(&pids[PID_RATE_ROLL + i], speedScaleFactor, rateDesiredAxis[i], gyro_filtered[i], dT);
                actuatorDesiredAxis[i] = bound(actuatorDesiredAxis[i], 1.0f);

                break;

            case STABILIZATIONDESIRED_STABILIZATIONMODE_RATTITUDE:
            // A parameterization from Attitude mode at center stick
            // - to Rate mode at full stick
            // This is done by parameterizing to use the rotation rate that Attitude mode
            // - would use at center stick to using the rotation rate that Rate mode
            // - would use at full stick in a weighted average sort of way.
            {
                if (reinit) {
                    pids[PID_ROLL       + i].iAccumulator = 0;
                    pids[PID_RATE_ROLL  + i].iAccumulator = 0;
                    pids[PID_RATEA_ROLL + i].iAccumulator = 0;
                }

                // Compute what Rate mode would give for this stick angle's rate
                // Save Rate's rate in a temp for later merging with Attitude's rate
                float rateDesiredAxisRate;
                rateDesiredAxisRate = bound(stabDesiredAxis[i], 1.0f)
                    * cast_struct_to_array(settings.ManualRate, settings.ManualRate.Roll)[i];

                // Compute what Attitude mode would give for this stick angle's rate

                // stabDesired for this mode is [-1.0f,+1.0f]
                // - multiply by Attitude mode max angle to get desired angle
                // - subtract off the actual angle to get the angle error
                // This is what local_error[] holds for Attitude mode
                float attitude_error = stabDesiredAxis[i]
                   * cast_struct_to_array(settings.RollMax, settings.RollMax)[i]
                   - cast_struct_to_array(attitudeState.Roll, attitudeState.Roll)[i];

                // Compute the outer loop just like Attitude mode does
                float rateDesiredAxisAttitude;
                rateDesiredAxisAttitude = pid_apply(&pids[PID_ROLL + i], attitude_error, dT);
                rateDesiredAxisAttitude = bound(rateDesiredAxisAttitude,
                                                cast_struct_to_array(settings.MaximumRate,
                                                                     settings.MaximumRate.Roll)[i]);

                // Compute the weighted average rate desired
                // Using max() rather than sqrt() for cpu speed;
                // - this makes the stick region into a square;
                // - this is a feature!
                // - hold a roll angle and add just pitch without the stick sensitivity changing
                // magnitude = sqrt(stabDesired.Roll*stabDesired.Roll + stabDesired.Pitch*stabDesired.Pitch);
                float magnitude;
                magnitude = fmaxf(fabsf(stabDesired.Roll), fabsf(stabDesired.Pitch));
                rateDesiredAxis[i] = (1.0f-magnitude) * rateDesiredAxisAttitude
                                   +       magnitude  * rateDesiredAxisRate;

                // Compute the inner loop for both Rate mode and Attitude mode
                // actuatorDesiredAxis[i] is the weighted average of the two PIDs from the two rates
                actuatorDesiredAxis[i] =
                    (1.0f-magnitude) * pid_apply_setpoint(&pids[PID_RATEA_ROLL + i], speedScaleFactor, rateDesiredAxis[i], gyro_filtered[i], dT)
                    +     magnitude  * pid_apply_setpoint(&pids[PID_RATE_ROLL  + i], speedScaleFactor, rateDesiredAxis[i], gyro_filtered[i], dT);
                actuatorDesiredAxis[i] = bound(actuatorDesiredAxis[i], 1.0f);

                // settings.RattitudeAntiWindup controls the iAccumulator zeroing
                // - so both iAccs don't wind up too far;
                // - nor do both iAccs get held too close to zero at mid stick

                // I suspect that there would be windup without it
                // - since rate and attitude fight each other here
                // - rate trying to pull it over the top and attitude trying to pull it back down

                // Wind-up increases linearly with cycles for a fixed error.
                // We must never increase the iAcc or we risk oscillation.

                // Use the powf() function to make two anti-windup curves
                // - one for zeroing rate close to center stick
                // - the other for zeroing attitude close to max stick

                // the bigger the dT      the more anti windup needed
                // the bigger the PID[].i the more anti windup needed
                // more anti windup is achieved with a lower powf() power
                // a doubling of e.g. PID[].i should cause the runtime anti windup factor
                // to get twice as far away from 1.0 (towards zero)
                // e.g. from .90 to .80

                // magic numbers
                // these generate the inverted parabola like curves that go through [0,1] and [1,0]
                // the higher the power, the closer the curve is to a straight line
                // from [0,1] to [1,1] to [1,0] and the less anti windup is applied

                // the UAVO RattitudeAntiWindup varies from 0 to 31
                // 0 turns off anti windup
                // 1 gives very little anti-windup because the power given to powf() is 31
                // 31 gives a lot of anti-windup because the power given to powf() is 1
                // the 32.1 is what does this
                // the 7.966 and 17.668 cancel the default PID value and dT given to log2f()
                // if these are non-default, tweaking is thus done so the user doesn't have to readjust
                // the default value of 10 for UAVO RattitudeAntiWindup gives a power of 22
		// these calculations are for magnitude = 0.5, so 22 corresponds to the number of bits
		// used in the mantissa of the float
		// i.e. 1.0-(0.5^22) almost underflows

                // This may only be useful for aircraft with large Ki values and limits
                if (dT > 0.0f && rattitude_anti_windup > 0.0f) {
                    float factor;

                    // At magnitudes close to one, the Attitude accumulators gets zeroed
                    if (pids[PID_ROLL+i].i > 0.0f) {
                        factor = 1.0f - stab_powf(magnitude, ((uint8_t) (32.1f - 7.966f - stab_log2f(dT * pids[PID_ROLL+i].i))) - rattitude_anti_windup);
                        pids[PID_ROLL+i].iAccumulator *= factor;
                    }
                    if (pids[PID_RATEA_ROLL+i].i > 0.0f) {
                        factor = 1.0f - stab_powf(magnitude, ((uint8_t) (32.1f - 17.668f - stab_log2f(dT * pids[PID_RATEA_ROLL+i].i))) - rattitude_anti_windup);
                        pids[PID_RATEA_ROLL+i].iAccumulator *= factor;
                    }

                    // At magnitudes close to zero, the Rate accumulator gets zeroed
                    if (pids[PID_RATE_ROLL+i].i > 0.0f) {
                        factor = 1.0f - stab_powf(1.0f-magnitude, ((uint8_t) (32.1f - 17.668f - stab_log2f(dT * pids[PID_RATE_ROLL+i].i))) - rattitude_anti_windup);
                        pids[PID_RATE_ROLL+i].iAccumulator *= factor;
                    }
                }

                break;
            }

            case STABILIZATIONDESIRED_STABILIZATIONMODE_VIRTUALBAR:

                // Store for debugging output
                rateDesiredAxis[i] = stabDesiredAxis[i];

                // Run a virtual flybar stabilization algorithm on this axis
                stabilization_virtual_flybar(gyro_filtered[i], rateDesiredAxis[i], &actuatorDesiredAxis[i], dT, reinit, i, &settings);

                break;

            case STABILIZATIONDESIRED_STABILIZATIONMODE_WEAKLEVELING:
            // FIXME: local_error[] is rate - attitude for Weak Leveling
            // The only ramifications are:
            // Weak Leveling Kp is off by a factor of 3 to 12 and may need a different default in GCS
            // Changing Rate mode max rate currently requires a change to Kp
            // That would be changed to Attitude mode max angle affecting Kp
            // Also does not take dT into account
            {
                if (reinit) {
                    pids[PID_RATE_ROLL + i].iAccumulator = 0;
                }

                float weak_leveling = local_error[i] * weak_leveling_kp;
                weak_leveling = bound(weak_leveling, weak_leveling_max);

                // Compute desired rate as input biased towards leveling
                rateDesiredAxis[i]     = stabDesiredAxis[i] + weak_leveling;
                actuatorDesiredAxis[i] = pid_apply_setpoint(&pids[PID_RATE_ROLL + i], speedScaleFactor, rateDesiredAxis[i], gyro_filtered[i], dT);
                actuatorDesiredAxis[i] = bound(actuatorDesiredAxis[i], 1.0f);

                break;
            }

            case STABILIZATIONDESIRED_STABILIZATIONMODE_AXISLOCK:
                if (reinit) {
                    pids[PID_RATE_ROLL + i].iAccumulator = 0;
                }

                if (fabsf(stabDesiredAxis[i]) > max_axislock_rate) {
                    // While getting strong commands act like rate mode
                    rateDesiredAxis[i] = stabDesiredAxis[i];
                    axis_lock_accum[i] = 0;
                } else {
                    // For weaker commands or no command simply attitude lock (almost) on no gyro change
                    axis_lock_accum[i] += (stabDesiredAxis[i] - gyro_filtered[i]) * dT;
                    axis_lock_accum[i]  = bound(axis_lock_accum[i], max_axis_lock);
                    rateDesiredAxis[i]  = pid_apply(&pids[PID_ROLL + i], axis_lock_accum[i], dT);
                }

                rateDesiredAxis[i]     = bound(rateDesiredAxis[i],
                                               cast_struct_to_array(settings.ManualRate, settings.ManualRate.Roll)[i]);

                actuatorDesiredAxis[i] = pid_apply_setpoint(&pids[PID_RATE_ROLL + i], speedScaleFactor, rateDesiredAxis[i], gyro_filtered[i], dT);
                actuatorDesiredAxis[i] = bound(actuatorDesiredAxis[i], 1.0f);

                break;

            case STABILIZATIONDESIRED_STABILIZATIONMODE_RELAYRATE:
                // Store to rate desired variable for storing to UAVO
                rateDesiredAxis[i] = bound(stabDesiredAxis[i],
                                           cast_struct_to_array(settings.ManualRate, settings.ManualRate.Roll)[i]);

                // Run the relay controller which also estimates the oscillation parameters
                stabilization_relay_rate(rateDesiredAxis[i] - gyro_filtered[i], &actuatorDesiredAxis[i], i, reinit);
                actuatorDesiredAxis[i] = bound(actuatorDesiredAxis[i], 1.0f);

                break;

            case STABILIZATIONDESIRED_STABILIZATIONMODE_RELAYATTITUDE:
                if (reinit) {
                    pids[PID_ROLL + i].iAccumulator = 0;
                }

                // Compute the outer loop like attitude mode
                rateDesiredAxis[i] = pid_apply(&pids[PID_ROLL + i], local_error[i], dT);
                rateDesiredAxis[i] = bound(rateDesiredAxis[i],
                                           cast_struct_to_array(settings.MaximumRate, settings.MaximumRate.Roll)[i]);

                // Run the relay controller which also estimates the oscillation parameters
                stabilization_relay_rate(rateDesiredAxis[i] - gyro_filtered[i], &actuatorDesiredAxis[i], i, reinit);
                actuatorDesiredAxis[i] = bound(actuatorDesiredAxis[i], 1.0f);

                break;

            case STABILIZATIONDESIRED_STABILIZATIONMODE_NONE:
                actuatorDesiredAxis[i] = bound(stabDesiredAxis[i], 1.0f);
                break;
            default:
                error = true;
                break;
            }
        }

        if (settings.VbarPiroComp == STABILIZATIONSETTINGS_VBARPIROCOMP_TRUE) {
            stabilization_virtual_flybar_pirocomp(gyro_filtered[2], dT);
        }

#ifdef DIAG_RATEDESIRED
        RateDesiredSet(&rateDesired);
#endif

        // Save dT
        actuatorDesired.UpdateTime = dT * 1000;
        actuatorDesired.Throttle   = stabDesired.Throttle;

        // Suppress desired output while disarmed or throttle low, for configured axis
        if (flightStatus.Armed != FLIGHTSTATUS_ARMED_ARMED || stabDesired.Throttle < 0) {
            if (lowThrottleZeroAxis[ROLL]) {
                actuatorDesired.Roll = 0.0f;
            }

            if (lowThrottleZeroAxis[PITCH]) {
                actuatorDesired.Pitch = 0.0f;
            }

            if (lowThrottleZeroAxis[YAW]) {
                actuatorDesired.Yaw = 0.0f;
            }
        }

        if (PARSE_FLIGHT_MODE(flightStatus.FlightMode) != FLIGHTMODE_MANUAL) {
            ActuatorDesiredSet(&actuatorDesired);
        } else {
            // Force all axes to reinitialize when engaged
            for (uint8_t i = 0; i < MAX_AXES; i++) {
                previous_mode[i] = 255;
            }
        }

        if (flightStatus.Armed != FLIGHTSTATUS_ARMED_ARMED ||
            (lowThrottleZeroIntegral && stabDesired.Throttle < 0)) {
            // Force all axes to reinitialize when engaged
            for (uint8_t i = 0; i < MAX_AXES; i++) {
                previous_mode[i] = 255;
            }
        }

        // Clear or set alarms.  Done like this to prevent toggline each cycle
        // and hammering system alarms
        if (error) {
            AlarmsSet(SYSTEMALARMS_ALARM_STABILIZATION, SYSTEMALARMS_ALARM_ERROR);
        } else {
            AlarmsClear(SYSTEMALARMS_ALARM_STABILIZATION);
        }
    }
}


/**
 * Clear the accumulators and derivatives for all the axes
 */
static void ZeroPids(void)
{
    for (uint32_t i = 0; i < PID_MAX; i++) {
        pid_zero(&pids[i]);
    }


    for (uint8_t i = 0; i < 3; i++) {
        axis_lock_accum[i] = 0.0f;
    }
}


/**
 * Bound input value between limits
 */
static float bound(float val, float range)
{
    if (val < -range) {
        val = -range;
    } else if (val > range) {
        val = range;
    }
    return val;
}


// x small (0.0 < x < .01) so interpolation of fractional part is reasonable
static float stab_log2f(float x)
{
  union
  {
    volatile float f;
    volatile uint32_t i;
    volatile unsigned char c[4];
  } __attribute__((packed)) u1, u2;

  u2.f = u1.f = x;
  u1.i <<= 1;
  u2.i &= 0xff800000;

  // get and unbias the exponent, add in a linear interpolation of the fractional part
  return (float) (u1.c[3] - 127) + (x / u2.f) - 1.0f;
}


// 0<=x<=1, 0<=p<=31
static float stab_powf(float x, uint8_t p)
{
  float retval = 1.0f;

  while (p)
  {
    if (p&1)
    {
      retval *= x;
    }
    x *= x;
    p >>= 1;
  }

  return retval;
}


static void SettingsUpdatedCb(__attribute__((unused)) UAVObjEvent *ev)
{
    StabilizationSettingsGet(&settings);

    // Set the roll rate PID constants
    pid_configure(&pids[PID_RATE_ROLL],
                  settings.RollRatePID.Kp,
                  settings.RollRatePID.Ki,
                  settings.RollRatePID.Kd,
                  settings.RollRatePID.ILimit);

    // Set the pitch rate PID constants
    pid_configure(&pids[PID_RATE_PITCH],
                  settings.PitchRatePID.Kp,
                  settings.PitchRatePID.Ki,
                  settings.PitchRatePID.Kd,
                  settings.PitchRatePID.ILimit);

    // Set the yaw rate PID constants
    pid_configure(&pids[PID_RATE_YAW],
                  settings.YawRatePID.Kp,
                  settings.YawRatePID.Ki,
                  settings.YawRatePID.Kd,
                  settings.YawRatePID.ILimit);

    // Set the roll attitude PI constants
    pid_configure(&pids[PID_ROLL],
                  settings.RollPI.Kp,
                  settings.RollPI.Ki,
                  0,
                  settings.RollPI.ILimit);

    // Set the pitch attitude PI constants
    pid_configure(&pids[PID_PITCH],
                  settings.PitchPI.Kp,
                  settings.PitchPI.Ki,
                  0,
                  settings.PitchPI.ILimit);

    // Set the yaw attitude PI constants
    pid_configure(&pids[PID_YAW],
                  settings.YawPI.Kp,
                  settings.YawPI.Ki,
                  0,
                  settings.YawPI.ILimit);

    // Set the Rattitude roll rate PID constants
    pid_configure(&pids[PID_RATEA_ROLL],
                  settings.RollRatePID.Kp,
                  settings.RollRatePID.Ki,
                  settings.RollRatePID.Kd,
                  settings.RollRatePID.ILimit);

    // Set the Rattitude pitch rate PID constants
    pid_configure(&pids[PID_RATEA_PITCH],
                  settings.PitchRatePID.Kp,
                  settings.PitchRatePID.Ki,
                  settings.PitchRatePID.Kd,
                  settings.PitchRatePID.ILimit);

    // Set the Rattitude yaw rate PID constants
    pid_configure(&pids[PID_RATEA_YAW],
                  settings.YawRatePID.Kp,
                  settings.YawRatePID.Ki,
                  settings.YawRatePID.Kd,
                  settings.YawRatePID.ILimit);

    // Set up the derivative term
    pid_configure_derivative(settings.DerivativeCutoff, settings.DerivativeGamma);

    // Maximum deviation to accumulate for axis lock
    max_axis_lock     = settings.MaxAxisLock;
    max_axislock_rate = settings.MaxAxisLockRate;

    // Settings for weak leveling
    weak_leveling_kp  = settings.WeakLevelingKp;
    weak_leveling_max = settings.MaxWeakLevelingRate;

    // Whether to zero the PID integrals while throttle is low
    lowThrottleZeroIntegral    = settings.LowThrottleZeroIntegral == STABILIZATIONSETTINGS_LOWTHROTTLEZEROINTEGRAL_TRUE;

    // Whether to suppress (zero) the StabilizationDesired output for each axis while disarmed or throttle is low
    lowThrottleZeroAxis[ROLL]  = settings.LowThrottleZeroAxis.Roll == STABILIZATIONSETTINGS_LOWTHROTTLEZEROAXIS_TRUE;
    lowThrottleZeroAxis[PITCH] = settings.LowThrottleZeroAxis.Pitch == STABILIZATIONSETTINGS_LOWTHROTTLEZEROAXIS_TRUE;
    lowThrottleZeroAxis[YAW]   = settings.LowThrottleZeroAxis.Yaw == STABILIZATIONSETTINGS_LOWTHROTTLEZEROAXIS_TRUE;

    // The dT has some jitter iteration to iteration that we don't want to
    // make thie result unpredictable.  Still, it's nicer to specify the constant
    // based on a time (in ms) rather than a fixed multiplier.  The error between
    // update rates on OP (~300 Hz) and CC (~475 Hz) is negligible for this
    // calculation
    const float fakeDt = 0.0025f;
    if (settings.GyroTau < 0.0001f) {
        gyro_alpha = 0; // not trusting this to resolve to 0
    } else {
        gyro_alpha = expf(-fakeDt / settings.GyroTau);
    }

    // Compute time constant for vbar decay term based on a tau
    vbar_decay = expf(-fakeDt / settings.VbarTau);

    // Rattitude flight mode anti-windup factor
    rattitude_anti_windup = settings.RattitudeAntiWindup;
}


/**
 * @}
 * @}
 */
