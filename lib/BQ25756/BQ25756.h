#pragma once

#include <Arduino.h>
#include <Wire.h>

class BQ25756
{
public:
    static constexpr uint8_t DEFAULT_I2C_ADDRESS = 0x6B;
    static constexpr uint8_t WIRE_ERROR_UNKNOWN = 0xFF;

    explicit BQ25756(TwoWire &wire, uint8_t i2cAddress = DEFAULT_I2C_ADDRESS);

    bool begin(uint32_t busClockHz, bool enableInternalPullups = false);
    bool isPresent();
    uint8_t lastWireError() const;

    // 8-bit Control Registers
    static constexpr uint8_t REG_CHG_CTRL = 0x17;
    static constexpr uint8_t REG_ADC_CTRL = 0x2B;
    static constexpr uint8_t REG_TIM_CTRL = 0x15;

    // 8-bit Status Registers
    static constexpr uint8_t REG_STAT_1 = 0x21;
    static constexpr uint8_t REG_STAT_2 = 0x22;
    static constexpr uint8_t REG_FAULT_STAT = 0x24;

    bool readRegister8(uint8_t reg, uint8_t &value);
    bool writeRegister8(uint8_t reg, uint8_t value);

    bool readRegister16(uint8_t reg, uint16_t &value, bool littleEndian = true);
    bool writeRegister16(uint8_t reg, uint16_t value, bool littleEndian = true);

    uint8_t address() const;

    bool petWatchdog();
    bool disableWatchdog(bool disable);
    bool enableCharging(bool enable);
    bool disableCEPin(bool disable); // 1 = Ignore hardware /CE pin
    bool setADCEnabled(bool enable);
    uint8_t getFaultStatus();

    enum ChargeState
    {
        IDLE = 0,
        TRICKLE_CHG = 1,
        PRE_CHG = 2,
        FAST_CHG = 3,
        TAPER_CHG = 4,
        TERMINATION_DONE = 5,
        RESERVED = 6,
        VBUS_REVERSE = 7,
        UNKNOWN = 0xFF
    };

    ChargeState getChargeState(bool &isBatteryPresent, bool &isPowerGood);

private:
    bool probeOnce();
    bool writeRegisterPointer(uint8_t reg);
    void enableWeakInternalPullupsOnI2CPins();

    TwoWire &_wire;
    uint8_t _address;
    uint32_t _busClockHz = 100000;
    uint8_t _lastWireError = WIRE_ERROR_UNKNOWN;
};
