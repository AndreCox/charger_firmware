#include "BQ25756.h"

BQ25756::BQ25756(TwoWire &wire, uint8_t i2cAddress) : _wire(wire), _address(i2cAddress)
{
}

void BQ25756::enableWeakInternalPullupsOnI2CPins()
{
    // Keep PA6/PA7 in alternate function while enabling weak pull-ups.
    GPIOA->PUPDR &= ~((0x3UL << (6U * 2U)) | (0x3UL << (7U * 2U)));
    GPIOA->PUPDR |= (0x1UL << (6U * 2U)) | (0x1UL << (7U * 2U));
}

bool BQ25756::begin(uint32_t busClockHz, bool enableInternalPullups)
{
    _busClockHz = busClockHz;
    _wire.begin();
    _wire.setClock(_busClockHz);
    if (enableInternalPullups)
    {
        enableWeakInternalPullupsOnI2CPins();
    }
    return isPresent();
}

bool BQ25756::isPresent()
{
    if (probeOnce())
    {
        return true;
    }

    // Retry once after reinitializing I2C for noisy/stuck bus states.
    _wire.begin();
    _wire.setClock(_busClockHz);
    delayMicroseconds(100);

    return probeOnce();
}

uint8_t BQ25756::lastWireError() const
{
    return _lastWireError;
}

bool BQ25756::readRegister8(uint8_t reg, uint8_t &value)
{
    if (!writeRegisterPointer(reg))
    {
        return false;
    }

    if (_wire.requestFrom(static_cast<int>(_address), 1) != 1)
    {
        return false;
    }

    value = _wire.read();
    return true;
}

bool BQ25756::writeRegister8(uint8_t reg, uint8_t value)
{
    _wire.beginTransmission(_address);
    _wire.write(reg);
    _wire.write(value);
    return (_wire.endTransmission() == 0);
}

bool BQ25756::readRegister16(uint8_t reg, uint16_t &value, bool littleEndian)
{
    if (!writeRegisterPointer(reg))
    {
        return false;
    }

    if (_wire.requestFrom(static_cast<int>(_address), 2) != 2)
    {
        return false;
    }

    const uint8_t b0 = _wire.read();
    const uint8_t b1 = _wire.read();

    if (littleEndian)
    {
        value = static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8U);
    }
    else
    {
        value = static_cast<uint16_t>(b1) | (static_cast<uint16_t>(b0) << 8U);
    }

    return true;
}

bool BQ25756::writeRegister16(uint8_t reg, uint16_t value, bool littleEndian)
{
    const uint8_t low = static_cast<uint8_t>(value & 0xFFU);
    const uint8_t high = static_cast<uint8_t>((value >> 8U) & 0xFFU);

    _wire.beginTransmission(_address);
    _wire.write(reg);

    if (littleEndian)
    {
        _wire.write(low);
        _wire.write(high);
    }
    else
    {
        _wire.write(high);
        _wire.write(low);
    }

    return (_wire.endTransmission() == 0);
}

uint8_t BQ25756::address() const
{
    return _address;
}

bool BQ25756::probeOnce()
{
    _wire.beginTransmission(_address);
    _lastWireError = _wire.endTransmission();
    return (_lastWireError == 0);
}

bool BQ25756::writeRegisterPointer(uint8_t reg)
{
    _wire.beginTransmission(_address);
    _wire.write(reg);
    return (_wire.endTransmission(false) == 0);
}

bool BQ25756::petWatchdog()
{
    uint8_t val;
    if (!readRegister8(REG_CHG_CTRL, val))
        return false;
    val |= (1 << 5); // WD_RST
    return writeRegister8(REG_CHG_CTRL, val);
}

bool BQ25756::enableCharging(bool enable)
{
    uint8_t val;
    if (!readRegister8(REG_CHG_CTRL, val))
        return false;
    if (enable)
        val |= (1 << 0);
    else
        val &= ~(1 << 0);
    return writeRegister8(REG_CHG_CTRL, val);
}

bool BQ25756::disableCEPin(bool disable)
{
    uint8_t val;
    if (!readRegister8(REG_CHG_CTRL, val))
        return false;
    if (disable)
        val |= (1 << 4);
    else
        val &= ~(1 << 4);
    return writeRegister8(REG_CHG_CTRL, val);
}

bool BQ25756::setADCEnabled(bool enable)
{
    uint8_t val;
    if (!readRegister8(REG_ADC_CTRL, val))
        return false;
    if (enable)
        val |= (1 << 7);
    else
        val &= ~(1 << 7);
    return writeRegister8(REG_ADC_CTRL, val);
}

uint8_t BQ25756::getFaultStatus()
{
    uint8_t val = 0;
    readRegister8(REG_FAULT_STAT, val);
    return val;
}

BQ25756::ChargeState BQ25756::getChargeState(bool &isBatteryPresent, bool &isPowerGood)
{
    uint8_t stat1 = 0;
    uint8_t stat2 = 0;

    if (!readRegister8(REG_STAT_1, stat1) || !readRegister8(REG_STAT_2, stat2))
    {
        return UNKNOWN;
    }

    // REG_STAT_2 (0x22): Bit 7 is PG_STAT
    isPowerGood = (stat2 >> 7) & 0x01;

    // REG_STAT_2 (0x22): Bits 6:4 are TS_STAT.
    // 111b (TS_OPEN) usually indicates battery disconnected.
    uint8_t tsStat = (stat2 >> 4) & 0x07;
    isBatteryPresent = (tsStat != 0x07);

    // REG_STAT_1 (0x21): Bits 2:0 are CHARGE_STAT
    uint8_t stateBits = stat1 & 0x07;

    return static_cast<ChargeState>(stateBits);
}

bool BQ25756::disableWatchdog(bool disable)
{
    uint8_t val;
    if (!readRegister8(REG_TIM_CTRL, val))
        return false;
    if (disable)
    {
        // WD[5:4] = 00 (disable watchdog)
        val &= ~(0x3U << 4);
    }
    else
    {
        // WD[5:4] = 01 (enable watchdog)
        val = (val & ~(0x3U << 4)) | (0x1U << 4);
    }

    return writeRegister8(REG_TIM_CTRL, val);
}