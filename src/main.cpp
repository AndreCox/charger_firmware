#define XSTR(x) STR(x)
#define STR(x) #x
#include <STM32FreeRTOS.h>
#include <Arduino.h>
#include <Wire.h>
#include "BQ25756.h"

// ─── Pin Assignments ─────────────────────────────────────────────────────────

namespace Pins
{
  constexpr uint8_t RED_LED = PA0;
  constexpr uint8_t GREEN_LED = PA1;
  constexpr uint8_t BLUE_LED = PA2;
  constexpr uint8_t I2C_SDA = PA6;
  constexpr uint8_t I2C_SCL = PA7;
}

// ─── Constants ───────────────────────────────────────────────────────────────

namespace Config
{
  constexpr uint32_t I2C_CLOCK_HZ = 100000;
  constexpr uint8_t CHARGER_I2C_ADDRESS = 0x6B;

  // Task timing
  constexpr TickType_t LED_PERIOD_MS = 20;
  constexpr TickType_t CHARGER_POLL_PERIOD_MS = 300;

  // I2C task watchdog
  constexpr TickType_t I2C_WATCHDOG_CHECK_MS = 2000;
  constexpr TickType_t I2C_WATCHDOG_TIMEOUT_MS = 3000;

  // LED animation
  constexpr uint32_t STATUS_LED_BLINK_MS = 300;
  constexpr uint32_t BREATHING_CYCLE_MS = 3000;
  constexpr uint32_t INPUT_LOW_FLASH_MS = 1000;

  // Task stack sizes (in words, not bytes — multiply by 4 for bytes)
  constexpr uint16_t LED_TASK_STACK = 256;
  constexpr uint16_t CHARGER_TASK_STACK = 320;
}

// ─── Shared State ────────────────────────────────────────────────────────────
//
// Written by the Charger Monitor task, read by the Status LED task.
// Protected by statusMutex — hold time is trivially short (struct copy).

struct ChargerStatus
{
  BQ25756::ChargeState chargeState = BQ25756::IDLE;
  bool isBatteryPresent = false;
  bool isPowerGood = false;
  uint8_t faultCode = 0;
  bool watchdogTriggered = false;
};

static SemaphoreHandle_t statusMutex = nullptr;
static ChargerStatus sharedStatus;

// ─── I2C Watchdog State ──────────────────────────────────────────────────────
//
// The watchdog timer runs in the FreeRTOS timer daemon context.
// It monitors lastChargerPetTick to detect a stuck charger task.

static TaskHandle_t chargerTaskHandle = nullptr;
static TimerHandle_t i2cWatchdogTimer = nullptr;
static volatile TickType_t lastChargerPetTick = 0;
static volatile TickType_t i2cRecoveryTick = 0;
static volatile bool chargerTaskNeedsReinit = false;

// ─── Hardware ────────────────────────────────────────────────────────────────

TwoWire I2C2_Bus(Pins::I2C_SDA, Pins::I2C_SCL);
BQ25756 charger(I2C2_Bus, Config::CHARGER_I2C_ADDRESS);

// ─── Independent Watchdog (IWDG) ─────────────────────────────────────────────
//
// Hardware watchdog driven by the LSI oscillator (≈32 kHz).
// Completely independent of CPU, interrupts, and FreeRTOS.
// If the charger task can't pet within ~4s, the MCU resets.

/// Pet the hardware watchdog. Must be called regularly.
static inline void iwdgPet()
{
  IWDG->KR = 0xAAAA;
}

/// Start the IWDG with approximately 4-second timeout.
/// Once started, the IWDG cannot be stopped — only a reset clears it.
static void iwdgStart()
{
  IWDG->KR  = 0xCCCC;  // Start IWDG (this automatically enables the LSI clock)
  IWDG->KR  = 0x5555;  // Enable register write access
  IWDG->PR  = 4;       // Prescaler /64 (LSI ≈32kHz → 500Hz)
  IWDG->RLR = 2000;    // Reload: 2000/500 = 4 second timeout
  while (IWDG->SR != 0) {} // Wait for registers to update
  IWDG->KR  = 0xAAAA;  // Initial pet
}

/// Log the cause of the last reset (useful to confirm IWDG recovery).
static void logResetCause()
{
  uint32_t csr = RCC->CSR2;
  if (csr & RCC_CSR2_IWDGRSTF)
    Serial.println("BOOT: Reset by IWDG (I2C lockup auto-recovery)");
  else if (csr & RCC_CSR2_SFTRSTF)
    Serial.println("BOOT: Software reset");
  else if (csr & RCC_CSR2_PINRSTF)
    Serial.println("BOOT: Pin reset (NRST)");
  // Clear reset flags for next boot
  RCC->CSR2 |= RCC_CSR2_RMVF;
}

// ─── LED Helpers ─────────────────────────────────────────────────────────────

static void setLedRGB(uint8_t r, uint8_t g, uint8_t b)
{
  analogWrite(Pins::RED_LED, r);
  analogWrite(Pins::GREEN_LED, g);
  analogWrite(Pins::BLUE_LED, b);
}

/// Drive the RGB LED based on the current charger status.
/// Called every LED_PERIOD_MS from the LED task.
static void updateStatusLed(const ChargerStatus &status)
{
  const uint32_t now = millis();

  // Blink toggle for states that use it
  static uint32_t lastBlinkToggleMs = 0;
  static bool blinkOn = false;
  if ((now - lastBlinkToggleMs) >= Config::STATUS_LED_BLINK_MS)
  {
    lastBlinkToggleMs = now;
    blinkOn = !blinkOn;
  }

  // PRIORITY 1: Fault Condition — LED Blink Code Diagnostic
  // Protocol: Long CYAN pulse (start), followed by N RED blinks (bit position in Reg 0x24)
  // PRIORITY 1: Fault Condition — LED Blink Code Diagnostic
  // [DISABLED per user request]
  /*
  if (status.faultCode != 0)
  {
    // Total cycle: 6 seconds
    const uint32_t cycleMs = 6000;
    const uint32_t t = now % cycleMs;

    // Find the relevant bit (lowest bit set in Register 0x24)
    uint8_t bitPos = 0;
    for (int i = 1; i < 8; i++)
    {
      if (status.faultCode & (1 << i))
      {
        bitPos = i;
        break;
      }
    }

    if (t < 1000)
    {
      // 0-1s: Header (Cyan pulse)
      setLedRGB(0, 255, 255);
    }
    else if (t < 1500)
    {
      // 1-1.5s: Spacer (Black)
      setLedRGB(0, 0, 0);
    }
    else
    {
      // 1.5s - 5.5s: Data (Blinks)
      uint32_t relativeT = t - 1500;
      uint32_t blinkIndex = relativeT / 500; // 500ms per blink (250ms ON, 250ms OFF)

      if (blinkIndex < bitPos)
      {
        bool isOn = (relativeT % 500) < 250;
        if (isOn)
          setLedRGB(255, 0, 0); // RED blink
        else
          setLedRGB(0, 0, 0);
      }
      else
      {
        setLedRGB(0, 0, 0); // Wait for cycle end
      }
    }
    return;
  }
  */

  // PRIORITY 1.5: Watchdog Reset — amber triple-flash (sticky until reboot)
  if (status.watchdogTriggered)
  {
    const uint32_t cycleMs = 2000;
    const uint32_t t = now % cycleMs;

    // Three quick amber flashes followed by a long pause
    bool isOn = (t < 100) || (t >= 200 && t < 300) || (t >= 400 && t < 500);

    if (isOn)
      setLedRGB(255, 100, 0); // Amber
    else
      setLedRGB(0, 0, 0);
    return;
  }

  // PRIORITY 1.75: I2C bus recovery — magenta double-flash (10s after recovery)
  if (i2cRecoveryTick != 0)
  {
    TickType_t recoveryAge = xTaskGetTickCount() - i2cRecoveryTick;
    if (recoveryAge < pdMS_TO_TICKS(10000))
    {
      const uint32_t t = now % 2000;
      bool isOn = (t < 100) || (t >= 200 && t < 300);
      if (isOn)
        setLedRGB(255, 0, 255); // Magenta
      else
        setLedRGB(0, 0, 0);
      return;
    }
  }

  // PRIORITY 2: No input power — soft white breathing
  if (!status.isPowerGood)
  {
    // True breathing using PWM
    uint32_t phase = now % Config::BREATHING_CYCLE_MS;
    uint32_t halfCycle = Config::BREATHING_CYCLE_MS / 2;
    uint8_t brightness = 0;

    if (phase < halfCycle)
    {
      // Fade in
      brightness = (phase * 255) / halfCycle;
    }
    else
    {
      // Fade out
      brightness = 255 - (((phase - halfCycle) * 255) / halfCycle);
    }

    // Apply a simple quadratic curve for more natural perception of brightness
    uint16_t perceived = (brightness * brightness) / 255;
    uint8_t finalBrightness = (uint8_t)perceived;

    setLedRGB(finalBrightness, finalBrightness, finalBrightness);
    return;
  }
  // if (!status.isBatteryPresent)
  // {
  //   setLedRGB(blinkOn ? 255 : 0, 0, 0);
  //   return;
  // }

  // PRIORITY 4: Normal charge states
  switch (status.chargeState)
  {
  case BQ25756::FAST_CHG:
    setLedRGB(0, 255, 0); // Solid green
    break;
  case BQ25756::PRE_CHG:
  case BQ25756::TRICKLE_CHG:
    // Bright yellow (R+G is usually very green-shifted for LEDs, so tweak red higher)
    setLedRGB(255, 128, 0);
    break;
  case BQ25756::IDLE:
  default:
    setLedRGB(0, 0, 255); // Solid blue
    break;
  }
}

// ─── FreeRTOS Tasks ──────────────────────────────────────────────────────────

/// High-priority task: drives the status LED at a consistent 20ms cadence.
/// Reads the shared charger status under the mutex (fast struct copy).
static void statusLedTask(void *pvParameters)
{
  (void)pvParameters;

  TickType_t xLastWake = xTaskGetTickCount();

  for (;;)
  {
    ChargerStatus localStatus;

    if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
      localStatus = sharedStatus;
      xSemaphoreGive(statusMutex);
    }

    updateStatusLed(localStatus);

    vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(Config::LED_PERIOD_MS));
  }
}

// ─── I2C Bus Recovery ────────────────────────────────────────────────────────

// Forward declaration
static void chargerMonitorTask(void *pvParameters);

/// Bit-bang the I2C bus lines to recover from a stuck slave.
/// Generates 9 SCL clock pulses and a STOP condition, with SDA force-release.
static void i2cBusRecover()
{
  // After Wire::end(), pins are in analog mode — reconfigure as GPIO
  pinMode(Pins::I2C_SCL, OUTPUT);
  pinMode(Pins::I2C_SDA, INPUT_PULLUP);

  // Generate up to 9 SCL clock pulses to free a stuck slave
  for (int i = 0; i < 9; i++)
  {
    digitalWrite(Pins::I2C_SCL, LOW);
    delayMicroseconds(10);
    digitalWrite(Pins::I2C_SCL, HIGH);
    delayMicroseconds(10);
    if (digitalRead(Pins::I2C_SDA) == HIGH)
      break;
  }

  // Generate STOP condition: SDA low→high while SCL is high
  pinMode(Pins::I2C_SDA, OUTPUT);
  digitalWrite(Pins::I2C_SDA, LOW);
  delayMicroseconds(10);
  digitalWrite(Pins::I2C_SCL, HIGH);
  delayMicroseconds(10);
  digitalWrite(Pins::I2C_SDA, HIGH); // STOP
  delayMicroseconds(10);

  // If SDA is still stuck low, force it high as last resort
  pinMode(Pins::I2C_SDA, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(Pins::I2C_SDA) == LOW)
  {
    pinMode(Pins::I2C_SDA, OUTPUT);
    digitalWrite(Pins::I2C_SDA, HIGH);
    delayMicroseconds(50);
  }

  // Release pins (Wire::begin will reconfigure for I2C)
  pinMode(Pins::I2C_SCL, INPUT);
  pinMode(Pins::I2C_SDA, INPUT);
}

/// Helper to create the charger monitor task (used at boot and after recovery).
static BaseType_t createChargerTask()
{
  return xTaskCreate(chargerMonitorTask, "CHG", Config::CHARGER_TASK_STACK,
                     nullptr, 1, &chargerTaskHandle);
}

/// FreeRTOS timer callback — supervises the charger task.
/// If the task hasn't petted within the timeout, perform hard I2C reset
/// and recreate the task.
static void i2cWatchdogCallback(TimerHandle_t xTimer)
{
  (void)xTimer;

  TickType_t elapsed = xTaskGetTickCount() - lastChargerPetTick;
  if (elapsed < pdMS_TO_TICKS(Config::I2C_WATCHDOG_TIMEOUT_MS))
    return; // Task is alive

  Serial.println("I2C WATCHDOG: Charger task stuck! Recovering...");

  // 1. Delete the stuck task
  if (chargerTaskHandle != nullptr)
  {
    vTaskDelete(chargerTaskHandle);
    chargerTaskHandle = nullptr;
  }

  // 2. Tear down the I2C peripheral
  I2C2_Bus.end();

  // 3. Bit-bang bus recovery (SCL pulses + SDA manipulation)
  i2cBusRecover();

  // 4. Flag the recreated task to re-init I2C
  chargerTaskNeedsReinit = true;
  i2cRecoveryTick = xTaskGetTickCount();

  // 5. Recreate the charger task
  if (createChargerTask() == pdPASS)
    Serial.println("I2C WATCHDOG: Charger task restarted.");
  else
    Serial.println("I2C WATCHDOG: Failed to recreate task!");
}

// ─── FreeRTOS Tasks ──────────────────────────────────────────────────────────

/// Lower-priority task: polls the BQ25756 over I2C every 300ms.
/// Writes results into the shared status struct under the mutex.
static void chargerMonitorTask(void *pvParameters)
{
  (void)pvParameters;

  // Pet immediately to prevent false watchdog trigger during init
  lastChargerPetTick = xTaskGetTickCount();

  // Re-initialize I2C if recovering from a bus lockup
  if (chargerTaskNeedsReinit)
  {
    Serial.println("CHG: Re-initializing I2C after recovery...");
    while (!charger.begin(Config::I2C_CLOCK_HZ, true))
    {
      vTaskDelay(pdMS_TO_TICKS(500));
      lastChargerPetTick = xTaskGetTickCount();
      iwdgPet(); // Keep hardware watchdog alive during retry
    }
    chargerTaskNeedsReinit = false;
    Serial.println("CHG: I2C re-initialized.");
  }

  // Initialize charger to wake up REGN
  // 1. Disable hardware /CE pin requirement (ignore what's on the pin)
  // 2. Enable charging (enables the buck-boost converter)
  // 3. Enable ADC (required for DRV_OKZ_STAT to read correctly)
  charger.disableCEPin(true);
  charger.disableWatchdog(false);
  charger.enableCharging(true);
  charger.setADCEnabled(true);

  bool watchdogDetected = false;
  TickType_t xLastWake = xTaskGetTickCount();

  for (;;)
  {
    // Pet both watchdogs at the top of each iteration
    lastChargerPetTick = xTaskGetTickCount();
    iwdgPet();
    // Detect watchdog reset: ADC_EN (0x2B bit 7) resets to 0 on timeout.
    // We set it to 1 at init, so if it reads 0 the watchdog must have fired.
    uint8_t adcCtrl = 0;
    if (charger.readRegister8(BQ25756::REG_ADC_CTRL, adcCtrl))
    {
      if (!(adcCtrl & 0x80))
      {
        Serial.println("WATCHDOG RESET detected! Re-applying charger config.");
        charger.disableCEPin(true);
        charger.enableCharging(true);
        charger.setADCEnabled(true);
        watchdogDetected = true;
      }
    }

    bool batteryPresent = false;
    bool powerGood = false;
    BQ25756::ChargeState chargeState = charger.getChargeState(batteryPresent, powerGood);
    uint8_t fault = charger.getFaultStatus();

    // ─── I2C Failure Detection & Recovery ───
    static uint32_t consecutiveI2CFailures = 0;
    if (chargeState == BQ25756::UNKNOWN)
    {
      consecutiveI2CFailures++;
      if (consecutiveI2CFailures >= 10) // 1 second of continuous failures
      {
        Serial.println("I2C Timeout/Failure detected. Restarting I2C bus...");
        I2C2_Bus.end();
        vTaskDelay(pdMS_TO_TICKS(10)); // Brief pause to let lines settle

        charger.begin(Config::I2C_CLOCK_HZ, true);

        // Re-apply critical settings after an I2C hang
        charger.disableCEPin(true);
        charger.disableWatchdog(false);
        charger.enableCharging(true);
        charger.setADCEnabled(true);

        consecutiveI2CFailures = 0;
      }
    }
    else
    {
      if (consecutiveI2CFailures > 0)
      {
        Serial.println("I2C communication recovered.");
        consecutiveI2CFailures = 0;
      }
    }

    // Mask the TS_FAULT (bit 4) per user request to ignore the '4 red flash' error
    fault &= ~(1 << 4);

    if (xSemaphoreTake(statusMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
      sharedStatus.chargeState = chargeState;
      sharedStatus.isBatteryPresent = batteryPresent;
      sharedStatus.isPowerGood = powerGood;
      sharedStatus.faultCode = fault;
      sharedStatus.watchdogTriggered = watchdogDetected;
      xSemaphoreGive(statusMutex);
    }

    if (fault != 0)
    {
      Serial.print("BQ25756 FAULT detected! Register 0x24: 0x");
      if (fault < 0x10)
        Serial.print("0");
      Serial.println(fault, HEX);
    }

    vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(Config::CHARGER_POLL_PERIOD_MS));
    charger.petWatchdog(); // Prevent watchdog timeout (if enabled)
  }
}

// ─── Arduino Entry Points ────────────────────────────────────────────────────

void setup()
{
  // ── Serial init ──
  Serial.begin(115200);
  delay(100);
  logResetCause();
  Serial.println("--- BQ25756 Charger Firmware Initializing ---");

  // Start the hardware watchdog (IWDG) immediately to protect boot
  // We must pet it during wait loops!
  iwdgStart();

  // ── GPIO init ──
  pinMode(Pins::RED_LED, OUTPUT);
  pinMode(Pins::GREEN_LED, OUTPUT);
  pinMode(Pins::BLUE_LED, OUTPUT);
  digitalWrite(Pins::RED_LED, LOW);
  digitalWrite(Pins::GREEN_LED, LOW);
  digitalWrite(Pins::BLUE_LED, LOW);

  // ── Wait for charger IC ──
  while (!charger.begin(Config::I2C_CLOCK_HZ, true))
  {
    iwdgPet();
    digitalWrite(Pins::RED_LED, HIGH);
    delay(500);
    digitalWrite(Pins::RED_LED, LOW);
    delay(500);
  }

  // Boot success indicator: 3 quick green flashes
  for (int i = 0; i < 3; i++)
  {
    iwdgPet();
    digitalWrite(Pins::GREEN_LED, HIGH);
    delay(50);
    digitalWrite(Pins::GREEN_LED, LOW);
    delay(50);
  }

  // ── Read initial charger status (pre-RTOS) ──
  {
    bool isBatteryPresent = false;
    bool isPowerGood = false;
    sharedStatus.chargeState = charger.getChargeState(isBatteryPresent, isPowerGood);
    sharedStatus.isBatteryPresent = isBatteryPresent;
    sharedStatus.isPowerGood = isPowerGood;
    sharedStatus.faultCode = charger.getFaultStatus();
  }

  uint8_t writeData = 0xA0;
  charger.writeRegister8(0x2B, writeData);

  charger.writeRegister8(0x2C, 0x00); // disable VFB ADC Control recommended by datasheet

  charger.writeRegister8(0x1A, 0x00); // disable mppt tracking
  charger.writeRegister8(0x02, 0x3C0); // set higher current limit
  iwdgPet();

  // ── Create RTOS primitives ──
  statusMutex = xSemaphoreCreateMutex();

  // Note: On Cortex-M0+, interrupts are disabled between xTaskCreate()
  // and vTaskStartScheduler(). No interrupt-dependent code here.

  BaseType_t ledCreated = xTaskCreate(statusLedTask, "LED", Config::LED_TASK_STACK, nullptr, 2, nullptr);
  BaseType_t chgCreated = createChargerTask();

  // Create the I2C watchdog timer (auto-reload, checks every 2s)
  i2cWatchdogTimer = xTimerCreate("I2CWD",
                                   pdMS_TO_TICKS(Config::I2C_WATCHDOG_CHECK_MS),
                                   pdTRUE, nullptr,
                                   i2cWatchdogCallback);

  if (statusMutex == nullptr || ledCreated != pdPASS || chgCreated != pdPASS
      || i2cWatchdogTimer == nullptr)
  {
    // RTOS resource allocation failed — fast red/blue alternating blink
    for (;;)
    {
      digitalWrite(Pins::RED_LED, HIGH);
      digitalWrite(Pins::BLUE_LED, LOW);
      delay(100);
      digitalWrite(Pins::RED_LED, LOW);
      digitalWrite(Pins::BLUE_LED, HIGH);
      delay(100);
    }
  }

  // Start the I2C software watchdog (queued, processed after scheduler starts)
  xTimerStart(i2cWatchdogTimer, 0);

  iwdgPet();

  // Start the scheduler — this never returns on success
  vTaskStartScheduler();

  // If we get here, the scheduler failed to start (e.g. idle task allocation failed)
  for (;;)
  {
    digitalWrite(Pins::RED_LED, HIGH);
    delay(50);
    digitalWrite(Pins::RED_LED, LOW);
    delay(50);
  }
}

void loop()
{
  // Never reached — the FreeRTOS scheduler owns execution from here.
}
