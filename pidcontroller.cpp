#include <cmath>

#include "pidcontroller.h"
#include "logger.h"

#define PID_CONTROLLER_TRANSMIT_TUNING_AID
#ifdef PID_CONTROLLER_TRANSMIT_TUNING_AID
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#endif

/**
 * Constructor for the PID Controller.
 *
 * @param kp            Proportional gain
 * @param ki            Integral gain
 * @param kd            Derivative gain
 * @param maxOutput     Max allowed output value (absolute)
 */
PidController::PidController(double kp, double ki, double kd, double maxOutput)
    : proportionalGain(kp),
      integralGain(ki),
      derivativeGain(kd),
      maxOutput(maxOutput) {

    // Calculate max integral so the I-term alone can never exceed the max output
    if (integralGain > 0)
        maxIntegral = maxOutput / integralGain;
    else
        maxIntegral = maxOutput;
}

/**
 * Calculates the new output value.
 *
 * @param currentValue     The current value
 * @param dt               The time elapsed since the last update (seconds)
 * @return The output value
 */
double PidController::Update(double currentValue, double dt) {
    if (dt <= 0.0)
        return 0.0;

    // Error > 0 means we are below target
    double error = targetValue - currentValue;

    pTerm = proportionalGain * error;

    if (!firstRun) { // the dt value is not yet valid on the first run
        integralSum += error * dt;

        // Anti-Windup: Clamp the integrator
        integralSum = std::min(integralSum, maxIntegral);
        integralSum = std::max(integralSum, -maxIntegral);

        iTerm = integralGain * integralSum;
        dTerm = derivativeGain * ((error - previousError) / dt);
    }

    double output = pTerm + iTerm + dTerm;

    previousError = error;
    firstRun = false;

    output = std::min(output, maxOutput);
    output = std::max(output, -maxOutput);

    if (std::abs(output) >= maxOutput) {
        LOGWARNING("pidcontroller: max output value exceeded. Resetting.");
        Reset();
    }

    SendTuningAidData(pTerm, iTerm, dTerm, currentValue, output, targetValue);

    return output;
}

/**
 * Resets the internal state (integral sum and error history)
 */
void PidController::Reset() {
    firstRun = true;
    pTerm = 0;
    iTerm = 0;
    dTerm = 0;
    integralSum = 0.0;
    previousError = 0.0;
}

/**
 * Live streams the internal PID controller state via UDP for visualization/tuning.
 *
 * This function is intended for debugging and tuning purposes only.
 * It sends a JSON payload containing P, I, D terms, input, output, and target values.
 *
 * To enable this feature:
 * 1. Define PID_CONTROLLER_TRANSMIT_TUNING_AID at the top of this file.
 * 2. Update the IP address in the implementation to match your receiving
 *    machine running PlotJuggler with UDP server.
 */
void PidController::SendTuningAidData(double pTerm, double iTerm, double dTerm, double input, double output, double targetValue) {
#ifdef PID_CONTROLLER_TRANSMIT_TUNING_AID
    static int sock = -1;
    static struct sockaddr_in dest_addr;

    // One-time setup check
    if (sock < 0) {
        sock = socket(AF_INET, SOCK_DGRAM, 0);

        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(9870); // PlotJuggler port
        inet_pton(AF_INET, "192.168.2.22", &dest_addr.sin_addr); // replace with your PC's IP
    }

    // Actual Payload
    char payload[512];
    int len = snprintf(payload, sizeof(payload),
        "{\"bufferFillLevelMs\":%g,\"targetBufferFillLevelMs\":%g,\"pTerm\":%g,\"iTerm\":%g,\"dTerm\":%g,\"outputPpm\":%g}",
        input, targetValue, pTerm, iTerm, dTerm, output);

    // Send (Non-blocking usually, very fast)
    sendto(sock, payload, len, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
#endif
}