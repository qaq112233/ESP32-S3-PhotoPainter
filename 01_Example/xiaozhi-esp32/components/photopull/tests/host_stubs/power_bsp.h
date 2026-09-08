#pragma once
struct PmicRegisterConfig {
    const char* isCharging;
    const char* chargeStatus;
    const char* batteryVoltage;
    const char* batteryPercent;
};
inline PmicRegisterConfig Custom_PmicGetBatteryInfo() { return {"Charging", "Status OK", "4.0 V", "80%"}; }
