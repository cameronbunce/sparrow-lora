// Copyright 2021 Blues Inc.  All rights reserved.
// Use of this source code is governed by licenses granted by the
// copyright holder including that found in the LICENSE file.

#include "sensor.h"
#include "main.h"

// States for the local state machine
#define STATE_MOTION_CHECK          0

// Special request IDs
#define REQUESTID_TEMPLATE          1

// The filename of the test database.  Note that * is replaced by the
// gateway with the sensor's ID, while the # is a special character
// reserved by the notecard and notehub for a Sensor ID that is
// appended to the device ID within Events.
#define SENSORDATA_NOTEFILE         "*#motion.qo"

// TRUE if we've successfully registered the template
static bool templateRegistered = false;

// Number of motion events since last note
static uint32_t motionEvents = 0;

// Forwards
static void addNote(void);
static bool registerNotefileTemplate(void);
static void resetInterrupt(void);

// Sensor One-Time Init
bool pirInit(int sensorID)
{

    // Initialize GPIOs as per data sheet 2.6 and 2.7
    GPIO_InitTypeDef init = {0};
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    init.Pin = PIR_SERIAL_IN_Pin;
    HAL_GPIO_Init(PIR_SERIAL_IN_Port, &init);
    HAL_GPIO_WritePin(PIR_SERIAL_IN_Port, PIR_SERIAL_IN_Pin, GPIO_PIN_RESET);
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLDOWN;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    init.Pin = PIR_DIRECT_LINK_Pin;
    HAL_GPIO_Init(PIR_DIRECT_LINK_Port, &init);
    HAL_NVIC_SetPriority(PIR_DIRECT_LINK_EXTI_IRQn, PIR_DIRECT_LINK_IT_PRIORITY, 0x00);
    HAL_NVIC_EnableIRQ(PIR_DIRECT_LINK_EXTI_IRQn);

    // Prepare to configure the module
    uint32_t configurationRegister = 0;

    // Threshold [24:17] 8 bits (Detection threshold on BPF value)
    // The pyroelectric signal must exceed that threshold after band-pass filtering in order to be recognized by
    // the pulse counter. The threshold applies to positive as well as negative pulses by the pyroelectric element.
    // The threshold must be configured to a value which meets the application's requirements.
    // Lower threshold means longer detection range, higher threshold means shorter detection range. You want the
    // threshold to be set as high as possible to avoid false triggers, but you want it set low enough so you get
    // the detection range you need to achieve.
    uint32_t threshold = 24;
    configurationRegister |= ((threshold & 0xff) << 17);

    // Blind Time [16:13] 4 bits (0.5 s + [Reg Val] * 0.5 s)
    // The purpose of blind time is to avoid immediate re-triggering after a motion event was detected and
    // an interrupt was signalized. The blind time starts counting after pulling the "DIRECT LINK" line from
    // high to low by the host system. The time can be selected between 0.5 s and 8 s in steps of 0.5 s.
    // This parameter is only critical if you want to detect multiple motion events while always staying in
    // the wake up mode. This is typically not the way the sensor is used. The typical sensor used case
    // is: pyro is in wake up mode, detects a motion event, generates an interrupt and the application
    // takes an action. In that case the blind time is irrelevant.
    uint32_t blindTime = 2;
    configurationRegister |= ((blindTime & 0x0f) << 13);

    // Pulse Counter [12:11] 2 bits (1 + [Reg Val])
    // The amount of pulses above the threshold is counted in a specified window time. It triggers
    // the alarm event (DIRECT LINK is pushed by the ASIC from low to high) in wake up operation mode. It can
    // be configured from 1 up to 4 pulses. The amount of pulses is application specific.
    // This is the number of times the threshold must be exceeded to constitute a motion event and for
    // the pyro to generate an interrupt. A low pulse count is more sensitive to small amplitude motion
    // but is more prone to have false triggers due to thermal events.
    uint32_t pulseCounter = 2;
    configurationRegister |= ((pulseCounter & 0x03) << 11);

    // Window Time [10:9] 2 bits (2 s + [Reg Val] * 2 s)
    // The pulse counter is evaluated for pulses above the threshold within a given moving window
    // time. The window time can be set from 2 s up to 8 s in intervals of 2 s. The best setting depends on
    // the application specific motion pattern.
    // This is the window of time in which the threshold must be exceeded the number of times as defined
    // in the pulse counter register, to constitute a motion event for the pyro to generate an interrupt.
    // This also helps filter out motion events from thermal events since both types of events do not
    // have the same temporal signature.
    uint32_t windowTime = 3;
    configurationRegister |= ((windowTime & 0x03) << 9);

    // Operation Modes [8:7] 2 bits (0: Forced Readout 1: Interrupt Readout 2: Wake Up 3: Reserved)
    // In "Forced" and "Interrupt Readout" mode the "DIRECT LINK" interface is used to read raw data and
    // configuration settings. The source is defined by the filter source setting. Please refer to
    // section 2.7 for communication details. In wake up operation mode, the internal alarm event unit is
    // used to generate a low to high transition on the "DIRECT LINK" line once the criteria for motion was
    // met. The host system must pull this line from high to low in order to reset the alarm unit.
    uint32_t operationModes = 2;        // Wake Up mode
    configurationRegister |= ((operationModes & 0x03) << 7);

    // Signal Source [6:5] 2 bits (0: PIR (BPF) 1: PIR (LPF) 2: Reserved 3: Temperature Sensor)
    // The signal of the pyroelectric sensor can be observed after low-pass filtering (LPF). The data on the
    // "DIRECT LINK" line will be an unsigned integer in the range of 0 counts to 16,383 counts.
    // After band pass filtering (BPF) the data will be a signed integer in the range of -8192 counts to +8191 counts.
    // If the source is set to the internal temperature sensor, an unsigned integer in the range of 0 counts to
    // 16,383 counts will be provided which is proportional to the internal temperature of the sensor. This can
    // be used to ignore false triggers due to difficult conditions such as sudden temperature changes above 1 K min^-1.
    // For motion detection this register should always be set to 0 (Band pass filtered Pyro output).
    uint32_t signalSource = 0;
    configurationRegister |= ((signalSource & 0x03) << 5);

    // Reserved1 [4:3] 2 bits (Must be set to the value 2)
    uint32_t reserved1 = 2;
    configurationRegister |= ((reserved1 & 0x03) << 3);

    // HPF Cut-Off [2] 1 bit (0: 0.4 Hz 1: 0.2 Hz)
    // The optimal value depends on the motion pattern and is application specific. Generally
    // speaking, the lower cut-off value is used for long distance motion detection.
    // This setting is to be determined experimentally based on the detection range you want to achieve,
    // lens design and speed of motion you want to detect. However a good starting point is to set
    // that register at 0 (0.4Hz).
    uint32_t hpfCutoff = 0;             // Long-distance
    configurationRegister |= ((hpfCutoff & 0x01) << 2);

    // Reserved2 [1] 1 bit (Must be set to the value 0)
    uint32_t reserved2 = 0;
    configurationRegister |= ((reserved2 & 0x01) << 1);

    // Pulse Detection Mode [0] 1 bit (Count with (0) or without (1) BPF sign change)
    // If the mode is set to 0, pulses above the threshold are only counted when the sign of
    // the signal changed after BPF. If set to 1, no zero crossing is required.
    // This register is to decide if you want the threshold to be exceeded with or without sign change
    // to be counted as a motion event. With sign change makes it more robust against false triggers
    // but makes it more difficult to detect small amplitude motion at long distances.
    uint32_t pulseDetectionMode = 0;
    configurationRegister |= ((pulseDetectionMode & 0x01) << 0);

    // Send the register according to 2.6 timing
    HAL_DelayUs(750);       // tSLT must be at least 580uS to prepare for accepting config
    for (int i=24; i>=0; --i) {
        HAL_GPIO_WritePin(PIR_SERIAL_IN_Port, PIR_SERIAL_IN_Pin, GPIO_PIN_RESET);
        HAL_DelayUs(5);     // tSL can be very short
        HAL_GPIO_WritePin(PIR_SERIAL_IN_Port, PIR_SERIAL_IN_Pin, GPIO_PIN_SET);
        HAL_DelayUs(1);     // between tSL and tSHD
        HAL_GPIO_WritePin(PIR_SERIAL_IN_Port, PIR_SERIAL_IN_Pin,
                          (configurationRegister & (1<<i)) != 0 ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_DelayUs(100);   // tSHD must be at least 72uS
    }
    HAL_GPIO_WritePin(PIR_SERIAL_IN_Port, PIR_SERIAL_IN_Pin, GPIO_PIN_RESET);
    HAL_DelayUs(750);       // tSLT must be at least 580uS for latching

    // Reset the interrupt
    resetInterrupt();

    // Success
    return true;

}

// Reset the interrupt according to datasheet 2.7 "Wake Up Mode"
void resetInterrupt()
{
    GPIO_InitTypeDef init = {0};
    init.Mode = GPIO_MODE_OUTPUT_PP;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    init.Pin = PIR_DIRECT_LINK_Pin;
    HAL_GPIO_Init(PIR_DIRECT_LINK_Port, &init);
    HAL_GPIO_WritePin(PIR_DIRECT_LINK_Port, PIR_DIRECT_LINK_Pin, GPIO_PIN_RESET);
    HAL_DelayUs(250);                   // Must be held low for at least 35uS
    // Note that the datasheet suggests that this should be NOPULL, but I have
    // tested PULLDOWN and the PIR's active state is strong enough that it works.
    // This is important so that if the PIR is not mounted on the board we
    // don't have an open input that is generating random interrupts with noise.
    init.Mode = GPIO_MODE_IT_RISING;
    init.Pull = GPIO_PULLDOWN;
    init.Speed = GPIO_SPEED_FREQ_HIGH;
    init.Pin = PIR_DIRECT_LINK_Pin;
    HAL_GPIO_Init(PIR_DIRECT_LINK_Port, &init);
    HAL_NVIC_SetPriority(PIR_DIRECT_LINK_EXTI_IRQn, PIR_DIRECT_LINK_IT_PRIORITY, 0x00);
    HAL_NVIC_EnableIRQ(PIR_DIRECT_LINK_EXTI_IRQn);
}

// Poller
void pirPoll(int sensorID, int state)
{

    // Switch based upon state
    switch (state) {

    case STATE_ACTIVATED:
        if (!templateRegistered) {
            registerNotefileTemplate();
            schedSetCompletionState(sensorID, STATE_ACTIVATED, STATE_MOTION_CHECK);
            traceLn("pir: template registration request");
            break;
        }

    // fallthrough to do a motion check

    case STATE_MOTION_CHECK:
        if (motionEvents == 0) {
            schedSetState(sensorID, STATE_DEACTIVATED, "pir: completed");
            break;
        }
        traceValueLn("pir: ", motionEvents, " motion events sensed");
        addNote();
        schedSetCompletionState(sensorID, STATE_MOTION_CHECK, STATE_MOTION_CHECK);
        traceLn("pir: note queued");
        break;

    }

}

// Register the notefile template for our data
static bool registerNotefileTemplate()
{

    // Create the request
    J *req = NoteNewRequest("note.template");
    if (req == NULL) {
        return false;
    }

    // Create the body
    J *body = JCreateObject();
    if (body == NULL) {
        JDelete(req);
        return false;
    }

    // Add an ID to the request, which will be echo'ed
    // back in the response by the notecard itself.  This
    // helps us to identify the asynchronous response
    // without needing to have an additional state.
    JAddNumberToObject(req, "id", REQUESTID_TEMPLATE);

    // Fill-in request parameters.  Note that in order to minimize
    // the size of the over-the-air JSON we're using a special format
    // for the "file" parameter implemented by the gateway, in which
    // a "file" parameter beginning with * will have that character
    // substituted with the textified sensor address.
    JAddStringToObject(req, "file", SENSORDATA_NOTEFILE);

    // Fill-in the body template
    JAddNumberToObject(body, "count", TINT32);

    // Attach the body to the request, and send it to the gateway
    JAddItemToObject(req, "body", body);
    noteSendToGatewayAsync(req, true);
    return true;

}

// Gateway Response handler
void pirResponse(int sensorID, J *rsp)
{

    // If this is a response timeout, indicate as such
    if (rsp == NULL) {
        traceLn("pir: response timeout");
        return;
    }

    // See if there's an error
    char *err = JGetString(rsp, "err");
    if (err[0] != '\0') {
        trace("sensor error response: ");
        trace(err);
        traceNL();
        return;
    }

    // Flash the LED if this is a response to this specific ping request
    switch (JGetInt(rsp, "id")) {

    case REQUESTID_TEMPLATE:
        templateRegistered = true;
        traceLn("pir: SUCCESSFUL template registration");
        break;
    }

}

// Send the sensor data
static void addNote()
{

    // Create the request
    J *req = NoteNewRequest("note.add");
    if (req == NULL) {
        return;
    }

    // Create the body
    J *body = JCreateObject();
    if (body == NULL) {
        JDelete(req);
        return;
    }

    // Set the target notefile
    JAddStringToObject(req, "file", SENSORDATA_NOTEFILE);

    // Fill-in the body
    uint32_t count = motionEvents;
    motionEvents = 0;
    JAddNumberToObject(body, "count", count);

    // Attach the body to the request, and send it to the gateway
    JAddItemToObject(req, "body", body);
    noteSendToGatewayAsync(req, false);

}

// Interrupt handler
void pirISR(int sensorID, uint16_t pins)
{

    // Set the state to 'motion' and immediately schedule
    if ((pins & PIR_DIRECT_LINK_Pin) != 0) {
        motionEvents++;
        resetInterrupt();
        if (schedGetState(sensorID) == STATE_DEACTIVATED) {
            schedActivateNowFromISR(sensorID, true, STATE_MOTION_CHECK);
        }
        return;
    }

}