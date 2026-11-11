// =============================================================================
// Shared SX1262 driver with board-specific pins and bounded I/O recovery.
// =============================================================================

#include "SX1262.h"
#include "RadioFrequency.h"
#include "RadioBandwidth.h"
#include "SX1262Timing.h"
#include "RadioTimingPolicy.h"
#include <cmath>
#include "config/BoardConfig.h"
#include "hal/SharedSPIBus.h"

SX1262* SX1262::_instance = nullptr;

namespace {
constexpr uint8_t IMAGE_CAL_UNSUPPORTED = 0xFF;

uint8_t imageCalParams(uint32_t frequency, uint8_t out[2]) {
    const auto* band = loRaFrequencyBand(frequency);
    if (!band) return IMAGE_CAL_UNSUPPORTED;
    out[0] = band->imageCal[0];
    out[1] = band->imageCal[1];
    return static_cast<uint8_t>(band - LORA_FREQUENCY_BANDS);
}

const char* packetTypeName(uint8_t packetType) {
    switch (packetType) {
        case 0x00: return "GFSK";
        case 0x01: return "LoRa";
        case 0x02: return "LR-FHSS";
        default: return "unknown";
    }
}
}

SX1262::SX1262(SPIClass* spi, int ss, int sclk, int mosi, int miso,
               int reset, int irq, int busy, int rxen,
               bool tcxo, bool dio2_as_rf_switch)
    : _spiSettings(SPI_FREQUENCY, MSBFIRST, SPI_MODE0),
      _spiModem(spi), _ss(ss), _sclk(sclk), _mosi(mosi), _miso(miso),
      _reset(reset), _irq(irq), _busy(busy), _rxen(rxen),
      _frequency(0), _sf(0x07), _bw(0x04), _cr(0x01),
      _ldro(false), _preambleLength(LORA_PREAMBLE_SYMBOLS_MIN),
      _packetIndex(0), _implicitHeaderMode(0),
      _payloadLength(255), _crcMode(1),
      _fifo_tx_addr_ptr(0), _fifo_rx_addr_ptr(0),
      _preinitDone(false), _radioOnline(false),
      _tcxo(tcxo), _dio2_as_rf_switch(dio2_as_rf_switch),
      _onReceive(nullptr), _preambleDetectedAt(0),
      _loraPreambleTimeMs(0), _loraHeaderTimeMs(0), _loraSymbolTimeMs(0)
{
    _txp = 14;
    _instance = this;
    memset(_packet, 0, sizeof(_packet));
}

bool SX1262::preInit() {
    pinMode(_ss, OUTPUT);
    digitalWrite(_ss, HIGH);

    // SPI bus is initialized by main.cpp — do NOT call _spiModem->begin() here

    const uint32_t start = millis();
    uint8_t syncmsb = 0, synclsb = 0;
    int probes = 0;
    while (uint32_t(millis() - start) < 2000) {
        syncmsb = readRegister(REG_SYNC_WORD_MSB_6X);
        synclsb = readRegister(REG_SYNC_WORD_LSB_6X);
        uint16_t sw = (uint16_t)(syncmsb << 8 | synclsb);
        probes++;
        Serial.printf("[SX1262] preInit probe %d: syncword=0x%04X\n", probes, sw);
        if (sw == 0x1424 || sw == 0x4434) {
            break;
        }
        if (_ioFailed) return false;
        delay(100);
    }

    uint16_t sw = (uint16_t)(syncmsb << 8 | synclsb);
    if (sw != 0x1424 && sw != 0x4434) {
        Serial.printf("[SX1262] preInit FAILED: syncword=0x%04X after %d probes\n", sw, probes);
        return false;
    }

    Serial.printf("[SX1262] preInit OK: syncword=0x%04X\n", sw);
    _preinitDone = true;
    return true;
}

uint8_t IRAM_ATTR SX1262::readRegister(uint16_t address) {
    return singleTransfer(OP_READ_REGISTER_6X, address, 0x00);
}

void SX1262::writeRegister(uint16_t address, uint8_t value) {
    singleTransfer(OP_WRITE_REGISTER_6X, address, value);
}

uint8_t IRAM_ATTR SX1262::singleTransfer(uint8_t opcode, uint16_t address, uint8_t value) {
    if (!waitOnBusy()) return 0;
    SharedSPILock lock;
    if (!lock.locked()) { failIo(); return 0; }
    uint8_t response;
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(opcode);
    _spiModem->transfer((address & 0xFF00) >> 8);
    _spiModem->transfer(address & 0x00FF);
    if (opcode == OP_READ_REGISTER_6X) {
        _spiModem->transfer(0x00);
    }
    response = _spiModem->transfer(value);
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
    return response;
}

void SX1262::executeOpcode(uint8_t opcode, uint8_t* buffer, uint8_t size) {
    if (!waitOnBusy()) return;
    SharedSPILock lock;
    if (!lock.locked()) { failIo(); return; }
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(opcode);
    for (int i = 0; i < size; i++) {
        _spiModem->transfer(buffer[i]);
    }
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
}

void SX1262::executeOpcodeRead(uint8_t opcode, uint8_t* buffer, uint8_t size) {
    memset(buffer, 0, size);
    if (!waitOnBusy()) return;
    SharedSPILock lock;
    if (!lock.locked()) { failIo(); return; }
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(opcode);
    _spiModem->transfer(0x00);
    for (int i = 0; i < size; i++) {
        buffer[i] = _spiModem->transfer(0x00);
    }
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
}

void SX1262::writeBuffer(const uint8_t* buffer, size_t size) {
    if (!waitOnBusy()) return;
    SharedSPILock lock;
    if (!lock.locked()) { failIo(); return; }
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(OP_FIFO_WRITE_6X);
    _spiModem->transfer(_fifo_tx_addr_ptr);
    for (size_t i = 0; i < size; i++) {
        _spiModem->transfer(buffer[i]);
        _fifo_tx_addr_ptr++;
    }
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
}

void SX1262::readBuffer(uint8_t* buffer, size_t size) {
    memset(buffer, 0, size);
    if (!waitOnBusy()) return;
    SharedSPILock lock;
    if (!lock.locked()) { failIo(); return; }
    _spiModem->beginTransaction(_spiSettings);
    digitalWrite(_ss, LOW);
    _spiModem->transfer(OP_FIFO_READ_6X);
    _spiModem->transfer(_fifo_rx_addr_ptr);
    _spiModem->transfer(0x00);
    for (size_t i = 0; i < size; i++) {
        buffer[i] = _spiModem->transfer(0x00);
    }
    digitalWrite(_ss, HIGH);
    _spiModem->endTransaction();
}

void SX1262::failIo() {
    // A partially applied tuple/FIFO cannot be used safely. Only an explicit
    // begin/reset can restore all configuration after a failed command.
    _ioFailed = true;
    _radioOnline = false;
    _txFailed = true;
    packetAvailable = false;
}

bool SX1262::waitOnBusy(unsigned long capMs) {
    if (_ioFailed) return false;
    if (_dcOn && !_sleeping && _busy != -1 && digitalRead(_busy) == HIGH) {
        // RX duty cycle: BUSY is high while the chip sleeps between windows, which can
        // last longer than capMs. A command finishing clears BUSY within microseconds;
        // still high after 3 ms means asleep, so wake it (it lands in STDBY_RC) and
        // resume the cycle later (rxHeal / next receive()).
        const uint32_t t0 = micros();
        while (digitalRead(_busy) == HIGH && uint32_t(micros() - t0) < 3000) {}
        if (digitalRead(_busy) == HIGH) { _sleeping = true; _dcNeedRearm = true; }
    }
    if (_sleeping) {
        // SX1261/2 datasheet 8.2.2: NSS wakes the chip; BUSY remains high
        // throughout sleep. Do not clock a command until wake-up completes.
        SharedSPILock lock;
        if (!lock.locked()) { failIo(); return false; }
        digitalWrite(_ss, LOW);
        delayMicroseconds(1000);
        digitalWrite(_ss, HIGH);
        _sleeping = false;
    }
    if (_busy == -1) return true;
    const uint32_t started = millis();
    while (digitalRead(_busy) == HIGH) {
        if (uint32_t(millis() - started) >= capMs) {
            ++_busyTimeouts;
            failIo();
            Serial.printf("[SX1262] BUSY wait timeout after %lums (count=%lu)\n",
                          capMs, (unsigned long)_busyTimeouts);
            return false;
        }
        if (_yieldCb) _yieldCb();
        yield();
    }
    return true;
}

void SX1262::reset() {
    _ioFailed = _sleeping = _txActive = _txFailed = false;
    _rxArmed = _dcOn = _dcNeedRearm = _dcTcxoFail = _tcxoShort = false;
    _radioOnline = _preinitDone = packetAvailable = false;
    _imageCalBand = IMAGE_CAL_UNSUPPORTED;
    if (_reset != -1) {
        pinMode(_reset, OUTPUT);
        digitalWrite(_reset, LOW);
        delay(10);
        digitalWrite(_reset, HIGH);
        delay(100);  // Allow TCXO + shared SPI to stabilize
    }
}

bool SX1262::startXoscRobust() {
    // Retain the bounded oscillator-start retries for integrated TCXO boards.
    // A stuck BUSY cannot accept ClearDeviceErrors/SetStandby. Reset into RC,
    // then restore TCXO configuration before trying XOSC again.
    for (unsigned attempt = 0; attempt < 6; ++attempt) {
        if (attempt) {
            if (_reset == -1) return false;
            reset();
            if (!preInit()) return false;
            enableTCXO();
            delay(10);
            standby();
        }
        if (waitOnBusy(handheld::sx1262_timing::tcxoTimeoutMs())) {
            clearDeviceErrors();
            return !_ioFailed;
        }
    }
    return false;
}

void SX1262::calibrate() {
    // Calibrate must be issued from STDBY_RC per datasheet.
    // TCXO is already configured via DIO3 with sufficient timeout,
    // so the 32MHz reference is available for PLL calibration.
    uint8_t mode_byte = MODE_STDBY_RC_6X;
    executeOpcode(OP_STANDBY_6X, &mode_byte, 1);
    uint8_t cal = MASK_CALIBRATE_ALL;
    executeOpcode(OP_CALIBRATE_6X, &cal, 1);
    const uint32_t calibrationStarted = millis();
    delay(5);
    // Calibration restarts the TCXO from RC standby. Its configured 640 ms
    // startup already exceeds 500 ms, before calibration itself completes.
    const bool calibrationReady = waitOnBusy(handheld::sx1262_timing::calibrationTimeoutMs(_tcxo));
    Serial.printf("[SX1262] Calibration ready=%d after %lums\n", calibrationReady,
                  (unsigned long)(millis() - calibrationStarted));
}

bool SX1262::calibrate_image(uint32_t frequency) {
    uint8_t image_freq[2] = {0};
    uint8_t band = imageCalParams(frequency, image_freq);

    if (band == IMAGE_CAL_UNSUPPORTED) {
        Serial.printf("[SX1262] No image calibration table for %lu Hz\n",
                      (unsigned long)frequency);
        return false;
    }

    if (_imageCalBand == band) return true;

    // Use the donor's proven standby() (STDBY_XOSC on TCXO boards) — forcing
    // STDBY_RC here (the P4 rsDeck#54 attempt) does NOT cleanly work on the
    // integrated SX1262 ("Bug A", root-caused to a marginal TCXO XOSC start;
    // startXoscRobust() owns the fix — history: docs/radio-calibration-fix.md).
    // Keep the generous BUSY cap + no-cache-on-timeout observability.
    standby();
    executeOpcode(OP_CALIBRATE_IMAGE_6X, image_freq, 2);
    // Fresh-band image cal outlives the default 100ms BUSY cap
    bool calDone = waitOnBusy(500);

    if (!calDone) {
        Serial.printf("[SX1262] Image calibration TIMEOUT for %lu Hz — not cached, retries on next tune\n",
                      (unsigned long)frequency);
        return false;
    }

    _imageCalBand = band;
    Serial.printf("[SX1262] Image calibrated for %lu Hz (0x%02X 0x%02X)\n",
                  (unsigned long)frequency, image_freq[0], image_freq[1]);
    return true;
}

void SX1262::enableTCXO() {
    if (_tcxo) {
        // Timeout: how long SX1262 waits for TCXO to stabilize when entering
        // STDBY_XOSC/TX/RX. Units = 15.625µs. 0x00A000 = 640ms (matches RadioLib).
        // If too short, chip stays in STDBY_RC and calibration uses RC oscillator.
        constexpr uint32_t ticks = handheld::sx1262_timing::tcxoStartupTicks;
        uint8_t buf[4] = {LORA_TCXO_VOLTAGE, uint8_t(ticks >> 16), uint8_t(ticks >> 8), uint8_t(ticks)};
        executeOpcode(OP_DIO3_TCXO_CTRL_6X, buf, 4);
        waitOnBusy(handheld::sx1262_timing::tcxoTimeoutMs());
    }
}

void SX1262::enableDio2RfSwitch() {
    if (_dio2_as_rf_switch) {
        uint8_t byte = 0x01;
        executeOpcode(OP_DIO2_RF_CTRL_6X, &byte, 1);
    }
}

bool SX1262::loraMode() {
    uint8_t mode = MODE_LONG_RANGE_MODE_6X;
    for (int attempt = 0; attempt < 5; attempt++) {
        executeOpcode(OP_PACKET_TYPE_6X, &mode, 1);
        delay(2);

        uint8_t packetType = getPacketType();
        if (packetType == MODE_LONG_RANGE_MODE_6X) {
            if (attempt > 0) {
                Serial.printf("[SX1262] LoRa packet type set after %d attempts\n", attempt + 1);
            }
            return true;
        }

        Serial.printf("[SX1262] SetPacketType LoRa attempt %d/5 read back 0x%02X (%s)\n",
                      attempt + 1, packetType, packetTypeName(packetType));
    }

    Serial.println("[SX1262] Failed to enter LoRa packet mode");
    return false;
}

bool SX1262::ensureLoRaMode(const char* context) {
    uint8_t packetType = getPacketType();
    if (packetType == MODE_LONG_RANGE_MODE_6X) return true;

    Serial.printf("[SX1262] %s found packet_type=0x%02X (%s), reasserting LoRa\n",
                  context ? context : "config", packetType, packetTypeName(packetType));
    standby();
    return loraMode();
}

void SX1262::rxAntEnable() {
    if (_rxen != -1) {
        digitalWrite(_rxen, HIGH);
    }
}

bool SX1262::begin(uint32_t frequency) {
    _frequency = frequency;
    reset();
    _imageCalBand = IMAGE_CAL_UNSUPPORTED;
    if (_busy != -1) { pinMode(_busy, INPUT); }
    if (!_preinitDone) {
        if (!preInit()) {
            return false;
        }
    }
    if (_rxen != -1) { pinMode(_rxen, OUTPUT); }

    // Match RadioLib's proven SX1262 init sequence:
    // 1. Configure TCXO via DIO3 (with generous timeout)
    // 2. Enter STDBY_XOSC to actually start the TCXO
    // 3. Set regulator mode
    // 4. Calibrate from STDBY_RC (TCXO stays powered via DIO3)
    enableTCXO();
    delay(10);

    // Force STDBY_XOSC to start the TCXO oscillator. On the integrated T-Deck/
    // T-Pager SX1262 this is marginal: the crystal sometimes fails to start on
    // the first attempt (BUSY stuck high + XOSC_START_ERR 0x20), which then
    // wedges calibration and the whole radio. Start it robustly: wait for BUSY
    // to clear, and on failure reset and reinitialize before retrying.
    standby();
    if (!startXoscRobust()) return false;

    // Set regulator mode. The T-Deck Plus integrated radio works in DC-DC
    // mode; cap-style modules may need board-specific LDO-only mode.
    uint8_t regMode = LORA_USE_DCDC_REGULATOR ? 0x01 : 0x00;  // 0x00=LDO, 0x01=DC-DC
    Serial.printf("[SX1262] Regulator mode: %s\n",
                  LORA_USE_DCDC_REGULATOR ? "DC-DC" : "LDO");
    executeOpcode(OP_REGULATOR_MODE_6X, &regMode, 1);

    // Calibrate from STDBY_RC with TCXO already running
    calibrate();
    if (_ioFailed || !calibrate_image(_frequency)) return false;

    // Set LoRa packet type and return to STDBY_XOSC
    if (!loraMode()) {
        return false;
    }
    standby();

    // Post-calibration diagnostic
    uint16_t postCalErr = getDeviceErrors();
    uint8_t iqReg = readRegister(REG_IQ_POLARITY_6X);
    Serial.printf("[SX1262] Post-cal DevErrors: 0x%04X%s IQ_REG=0x%02X\n",
        postCalErr, (postCalErr & 0x40) ? " *** PLL FAIL ***" : " OK", iqReg);
    clearDeviceErrors();

    setSyncWord(SYNC_WORD_6X);

    enableDio2RfSwitch();

    rxAntEnable();
    setFrequency(_frequency);
    setTxPower(_txp);
    enableCrc();
    writeRegister(REG_LNA_6X, 0x96);

    uint8_t basebuf[2] = {0};
    executeOpcode(OP_BUFFER_BASE_ADDR_6X, basebuf, 2);
    setModulationParams(_sf, _bw, _cr, _ldro);
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);

    uint8_t irqBuf[8];
    irqBuf[0] = 0xFF; irqBuf[1] = 0xFF;
    irqBuf[2] = 0x00; irqBuf[3] = IRQ_RX_DONE_MASK_6X;
    irqBuf[4] = 0x00; irqBuf[5] = 0x00;
    irqBuf[6] = 0x00; irqBuf[7] = 0x00;
    executeOpcode(OP_SET_IRQ_FLAGS_6X, irqBuf, 8);
    enableDio2RfSwitch();

    // Keep TCXO running between TX/RX transitions (don't fall back to RC oscillator)
    uint8_t fallback = MODE_FALLBACK_STDBY_XOSC_6X;
    executeOpcode(OP_RX_TX_FALLBACK_MODE_6X, &fallback, 1);

    clearDeviceErrors();
    _radioOnline = !_ioFailed;
    return _radioOnline;
}

void SX1262::end() {
    onReceive(nullptr);
    sleep();
    // The board owns this bus; ending it would also stop display/SD access.
    _radioOnline = false;
    _preinitDone = false;
}

int SX1262::beginPacket(int implicitHeader) {
    if (_ioFailed || _txActive) return 0;
    _txFailed = false;
    standby();
    if (!ensureLoRaMode("beginPacket")) return 0;

    if (implicitHeader) { implicitHeaderMode(); } else { explicitHeaderMode(); }
    _payloadLength = 0;
    _fifo_tx_addr_ptr = 0;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
    return !_ioFailed;
}

int SX1262::endPacket(bool async) {
    if (_ioFailed || _txActive || !ensureLoRaMode("endPacket")) return 0;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
    enableDio2RfSwitch();
    uint8_t clear[2] = {0xFF, 0xFF};
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, clear, 2);
    const uint32_t airtime = handheld::sx1262_timing::airtimeMs(_payloadLength, {
        _sf, _bw, getCodingRate4(), static_cast<uint16_t>(_preambleLength),
        _implicitHeaderMode != 0, _crcMode != 0, _ldro});
    if (!airtime) { _txFailed = true; return 0; }
    _txBudgetMs = handheld::radio_timing::transmitTimeoutMs(airtime);
    uint8_t timeout[3] = {0};
    _txStartMs = millis();
    executeOpcode(OP_TX_6X, timeout, 3);
    // BUSY falls after the TX transition/PA ramp. Admission must not report
    // a physical start when that transition failed. XOSC is already running.
    if (!waitOnBusy()) return 0;
    _txFailed = false;
    _txActive = true;
    txStarted++;
    txAirMs += airtime;
    if (async) return 1;
    while (isTxBusy()) {
        yield();
        if (_yieldCb) _yieldCb();
    }
    return !_txFailed;
}

bool SX1262::isTxBusy() {
    if (!_txActive) return false;
    uint8_t irq[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, irq, 2);
    const bool done = (irq[1] & IRQ_TX_DONE_MASK_6X) != 0;
    if (!_ioFailed && !done && uint32_t(millis() - _txStartMs) < _txBudgetMs)
        return true;

    _txActive = false;
    _txFailed = _ioFailed || !done;
    if (done) txDone++; else txTimeout++;
    // A timeout is not TX_DONE. Abort the radio operation before the caller
    // restores RX, and retain the failure until the next beginPacket.
    if (_txFailed) standby();
    uint8_t clear[2] = {0x00, IRQ_TX_DONE_MASK_6X};
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, clear, 2);
    return false;
}

size_t SX1262::write(uint8_t byte) { return write(&byte, 1); }

size_t SX1262::write(const uint8_t* buffer, size_t size) {
    if ((_payloadLength + size) > MAX_PACKET_SIZE) {
        Serial.printf("[SX1262] WARNING: write() truncating %d->%d bytes (payload=%d max=%d)\n",
                      (int)size, (int)(MAX_PACKET_SIZE - _payloadLength),
                      (int)_payloadLength, (int)MAX_PACKET_SIZE);
        size = MAX_PACKET_SIZE - _payloadLength;
    }
    writeBuffer(buffer, size);
    if (_ioFailed) return 0;
    _payloadLength += size;
    return size;
}

void SX1262::receive(int size) {
    bool dc = size <= 0 && _dcWanted && !_dcTcxoFail && dcCompute();
    if (_tcxo && dc != _tcxoShort) {
        setTcxoDelay(dc);                        // leaves the chip in STDBY_RC
        if (dc && !_tcxoShort) dc = false;       // TCXO too slow: stay continuous
        if (dc) dc = dcCompute();                // listen window includes the chosen delay
    }
    standby();
    if (!ensureLoRaMode("receive")) return;

    uint8_t clear[2] = {0xFF, 0xFF};
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, clear, 2);

    if (size > 0) {
        implicitHeaderMode();
        _payloadLength = size;
        setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
    } else {
        explicitHeaderMode();
    }

    // Set up DIO1 interrupt for RX done (enables packetAvailable flag)
    if (!_onReceive) {
        pinMode(_irq, INPUT);
        // Duty cycle: header errors on DIO1 too, so a broken frame re-arms at once.
        const uint8_t dio1 = IRQ_RX_DONE_MASK_6X | (dc ? IRQ_HEADER_ERR_MASK_6X : 0);
        uint8_t irqBuf[8] = {0xFF, 0xFF, 0x00, dio1, 0x00, 0x00, 0x00, 0x00};
        executeOpcode(OP_SET_IRQ_FLAGS_6X, irqBuf, 8);
        enableDio2RfSwitch();
        attachInterrupt(digitalPinToInterrupt(_irq), onDio0Rise, RISING);
    }

    packetAvailable = false;

    if (_rxen != -1) { rxAntEnable(); }
    if (dc) {
        startRxDc();
        _dcOn = !_ioFailed;
    } else {
        uint8_t mode[3] = {0xFF, 0xFF, 0xFF};
        executeOpcode(OP_RX_6X, mode, 3);
    }
    _rxArmed = !_ioFailed;
}

// ---- RX duty cycle ------------------------------------------------------------------
// Window sizing follows RadioLib's startReceiveDutyCycleAuto: with a sender preamble of
// P symbols and m symbols needed to detect it, sleep (P - 2m) symbols and listen long
// enough that any preamble overlaps >= m symbols of a window; on detection the chip
// extends the window itself and receives the whole frame.
static constexpr uint32_t kDcTcxoStepsUs[] = {5000, 10000, 20000, 40000};

bool SX1262::dcCompute() {
    const uint32_t bw = getSignalBandwidth();
    const uint32_t pre = _preambleLength > 0 ? (uint32_t)_preambleLength : 0;
    if (!bw || _sf < 5 || _sf > 12 || pre <= 2u * _dcMinSym) return false;
    const uint32_t symUs = (uint32_t)(((uint64_t)1000000 << _sf) / bw);
    const uint32_t sleepUs = symUs * (pre - 2u * _dcMinSym);
    const uint32_t tcxoUs = _tcxo ? _dcTcxoUs : 0;
    if (sleepUs < tcxoUs + 1016) return false;            // too short to be worth it
    uint32_t wakeUs = (symUs * (pre + 1) - (sleepUs - 1000)) / 2;
    if (wakeUs < symUs * (_dcMinSym + 1u)) wakeUs = symUs * (_dcMinSym + 1u);
    _dcRxUs = wakeUs + tcxoUs;                            // TCXO start-up eats into it
    _dcSleepUs = sleepUs;
    return true;
}

void SX1262::startRxDc() {
    const uint32_t rx = (uint32_t)((uint64_t)_dcRxUs * 64 / 1000);
    const uint32_t sl = (uint32_t)((uint64_t)_dcSleepUs * 64 / 1000);
    uint8_t buf[6] = {uint8_t(rx >> 16), uint8_t(rx >> 8), uint8_t(rx),
                      uint8_t(sl >> 16), uint8_t(sl >> 8), uint8_t(sl)};
    executeOpcode(OP_RX_DUTY_CYCLE_6X, buf, 6);
    _dcNeedRearm = false;
    ++rxDcArms;
}

static void tcxoCmd(uint8_t* buf, uint32_t us) {
    const uint32_t ticks = (uint32_t)((uint64_t)us * 64 / 1000);   // 15.625 us units
    buf[0] = LORA_TCXO_VOLTAGE;
    buf[1] = uint8_t(ticks >> 16);
    buf[2] = uint8_t(ticks >> 8);
    buf[3] = uint8_t(ticks);
}

// Cold start: warm sleep (TCXO unpowered), then XOSC standby with the programmed delay.
bool SX1262::tcxoColdStartOk() {
    uint8_t warm = 0x04;
    executeOpcode(OP_SLEEP_6X, &warm, 1);
    _sleeping = true;
    delay(5);
    // Wake into STDBY_RC first (TCXO still unpowered); waking itself can flag errors.
    uint8_t rc = MODE_STDBY_RC_6X;
    executeOpcode(OP_STANDBY_6X, &rc, 1);
    waitOnBusy(100);
    clearDeviceErrors();
    standby();                                            // XOSC: TCXO on, programmed delay
    uint8_t err[2] = {0};
    executeOpcodeRead(OP_GET_DEVICE_ERRORS_6X, err, 2);
    _dcLastErr = (uint16_t)(err[0] << 8 | err[1]);
    clearDeviceErrors();
    return !_ioFailed && !(err[1] & 0x20);               // XOSC_START_ERR
}

uint16_t SX1262::tcxoProbe(uint32_t us) {
    if (!_tcxo || _txActive || _ioFailed) return 0xFFFF;
    uint8_t rc = MODE_STDBY_RC_6X, buf[4];
    tcxoCmd(buf, us);
    executeOpcode(OP_STANDBY_6X, &rc, 1);
    _rxArmed = _dcOn = false;
    waitOnBusy(100);
    executeOpcode(OP_DIO3_TCXO_CTRL_6X, buf, 4);
    waitOnBusy(100);
    clearDeviceErrors();
    tcxoColdStartOk();
    const uint16_t err = _dcLastErr;
    _tcxoShort = true;                                    // force a reprogram on the next receive()
    if (!_dcWanted) setTcxoDelay(false);
    return err;
}

void SX1262::setTcxoDelay(bool shortDelay) {
    uint8_t rc = MODE_STDBY_RC_6X;
    uint8_t buf[4];
    executeOpcode(OP_STANDBY_6X, &rc, 1);
    _rxArmed = _dcOn = false;
    waitOnBusy(100);
    if (!shortDelay) {
        buf[0] = LORA_TCXO_VOLTAGE;
        constexpr uint32_t ticks = handheld::sx1262_timing::tcxoStartupTicks;
        buf[1] = uint8_t(ticks >> 16); buf[2] = uint8_t(ticks >> 8); buf[3] = uint8_t(ticks);
        executeOpcode(OP_DIO3_TCXO_CTRL_6X, buf, 4);
        waitOnBusy(100);
        clearDeviceErrors();
        _tcxoShort = false;
        return;
    }
    for (uint32_t us : kDcTcxoStepsUs) {
        tcxoCmd(buf, us);
        executeOpcode(OP_STANDBY_6X, &rc, 1);
        waitOnBusy(100);
        executeOpcode(OP_DIO3_TCXO_CTRL_6X, buf, 4);
        waitOnBusy(100);
        clearDeviceErrors();
        if (_ioFailed) return;
        // Two cold starts in a row must pass, the delay is then used for every window.
        if (tcxoColdStartOk() && tcxoColdStartOk()) {
            _dcTcxoUs = us;
            _tcxoShort = true;
            Serial.printf("[SX1262] RX duty cycle: TCXO start-up %lu ms\n", (unsigned long)(us / 1000));
            executeOpcode(OP_STANDBY_6X, &rc, 1);
            waitOnBusy(100);
            return;
        }
    }
    Serial.printf("[SX1262] TCXO not ready within 40 ms (errors 0x%04X) - RX duty cycle off%c", _dcLastErr, 10);
    _dcTcxoFail = true;
    setTcxoDelay(false);
}

void SX1262::setRxDutyCycle(bool on, uint8_t minSymbols) {
    if (minSymbols < 2) minSymbols = 2;
    if (minSymbols > 16) minSymbols = 16;
    if (on == _dcWanted && minSymbols == _dcMinSym) return;
    _dcWanted = on;
    _dcMinSym = minSymbols;
    // Listening now: switch at once (a pending packet switches when it is parsed).
    if (_rxArmed && !_txActive && !_ioFailed && !packetAvailable) receive();
}

void SX1262::rxHeal() {
    if (!_dcNeedRearm || !_dcOn || !_rxArmed || _txActive || _ioFailed) return;
    startRxDc();
    ++rxDcHeals;
}

int SX1262::parsePacket(int size) {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);
    if ((buf[1] & IRQ_RX_DONE_MASK_6X) == 0) { return 0; }

    uint8_t mask[2] = {0x00, IRQ_RX_DONE_MASK_6X};
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, mask, 2);

    // Read buffer info
    uint8_t rxinfo[2] = {0};
    executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, rxinfo, 2);
    int pktLen = rxinfo[0];
    _fifo_rx_addr_ptr = rxinfo[1];

    // Corrupted FIFO status (bus glitch / desync) — reset RX instead of
    // handing garbage upstream
    if (pktLen < 1 || pktLen > MAX_PACKET_SIZE) {
        Serial.printf("[SX1262] Invalid FIFO length: %d — resetting RX\n", pktLen);
        receive();
        return 0;
    }

    // Read RSSI/SNR before clearing IRQ
    uint8_t pktStat[3] = {0};
    executeOpcodeRead(OP_PACKET_STATUS_6X, pktStat, 3);
    float rssi = -float(pktStat[0]) / 2.0;
    float snr  = float((int8_t)pktStat[1]) * 0.25;
    _lastPktRssi = -int(pktStat[0]) / 2;
    _lastPktSnr = snr;

    bool crcOk = getPacketValidity();
    if (crcOk) rxOk++; else rxCrcFail++;

    // Always read FIFO data for diagnostics
    _packetIndex = 0;
    readBuffer(_packet, pktLen);

    if (_ioFailed) return 0;
    if (!crcOk) {
        Serial.printf("[SX1262] RX CRC FAIL: %d bytes RSSI=%.0f SNR=%.1f\n",
                      pktLen, rssi, snr);
        // Full hex dump for diagnosis
        for (int i = 0; i < pktLen; i++) {
            Serial.printf("%02X ", _packet[i]);
            if ((i & 0x1F) == 0x1F) Serial.println();
        }
        Serial.println();
        receive();
        return 0;
    }

    // Duty cycle: after RxDone the chip falls back to standby, while continuous RX keeps
    // listening on its own. The frame is in _packet[] and its status cached, so re-arm.
    if (_dcWanted || _dcOn) receive();
    return pktLen;
}

int IRAM_ATTR SX1262::available() {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, buf, 2);
    return _ioFailed || buf[0] <= _packetIndex ? 0 : buf[0] - _packetIndex;
}

int IRAM_ATTR SX1262::read() {
    if (!available()) { return -1; }
    if (_packetIndex == 0) {
        uint8_t rxbuf[2] = {0};
        executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, rxbuf, 2);
        int size = rxbuf[0];
        _fifo_rx_addr_ptr = rxbuf[1];
        readBuffer(_packet, size);
        if (_ioFailed) return -1;
    }
    uint8_t byte = _packet[_packetIndex];
    _packetIndex++;
    return byte;
}

int SX1262::peek() {
    if (!available()) { return -1; }
    if (_packetIndex == 0) {
        uint8_t rxbuf[2] = {0};
        executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, rxbuf, 2);
        int size = rxbuf[0];
        _fifo_rx_addr_ptr = rxbuf[1];
        readBuffer(_packet, size);
        if (_ioFailed) return -1;
    }
    return _packet[_packetIndex];
}

void SX1262::readBytes(uint8_t* buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        int b = read();
        if (b < 0) break;
        buffer[i] = (uint8_t)b;
    }
}

int IRAM_ATTR SX1262::currentRssi() {
    uint8_t byte = 0;
    executeOpcodeRead(OP_CURRENT_RSSI_6X, &byte, 1);
    rxHeal();
    return -(int(byte)) / 2;
}

int SX1262::packetRssi() {
    if (_dcWanted || _dcOn) return _lastPktRssi;
    uint8_t buf[3] = {0};
    executeOpcodeRead(OP_PACKET_STATUS_6X, buf, 3);
    return -buf[0] / 2;
}

float SX1262::packetSnr() {
    if (_dcWanted || _dcOn) return _lastPktSnr;
    uint8_t buf[3] = {0};
    executeOpcodeRead(OP_PACKET_STATUS_6X, buf, 3);
    return float((int8_t)buf[1]) * 0.25;
}

uint16_t SX1262::getDeviceErrors() {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_DEVICE_ERRORS_6X, buf, 2);
    rxHeal();
    return (uint16_t)(buf[0] << 8 | buf[1]);
}

void SX1262::clearDeviceErrors() {
    uint8_t buf[2] = {0x00, 0x00};
    executeOpcode(OP_CLEAR_DEVICE_ERRORS_6X, buf, 2);
}

uint8_t SX1262::getStatus() {
    uint8_t buf[1] = {0};
    executeOpcodeRead(OP_STATUS_6X, buf, 1);
    rxHeal();
    return buf[0];
}

uint8_t SX1262::getPacketType() {
    uint8_t buf[1] = {0xFF};
    executeOpcodeRead(OP_GET_PACKET_TYPE_6X, buf, 1);
    return buf[0];
}

uint16_t SX1262::getIrqFlags() {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);
    rxHeal();
    return (uint16_t)(buf[0] << 8 | buf[1]);
}

bool IRAM_ATTR SX1262::getPacketValidity() {
    uint8_t buf[2] = {0};
    executeOpcodeRead(OP_GET_IRQ_STATUS_6X, buf, 2);
    executeOpcode(OP_CLEAR_IRQ_STATUS_6X, buf, 2);
    return (buf[1] & IRQ_PAYLOAD_CRC_ERROR_MASK_6X) == 0;
}

void SX1262::setFrequency(uint32_t frequency) {
    if (!calibrate_image(frequency)) {
        Serial.printf("[SX1262] setFrequency %lu Hz: image calibration incomplete\n",
                      (unsigned long)frequency);
    }
    standby();
    _frequency = frequency;
    uint32_t freq = (uint32_t)((double)frequency / (double)FREQ_STEP_6X);
    uint8_t buf[4];
    buf[0] = ((freq >> 24) & 0xFF);
    buf[1] = ((freq >> 16) & 0xFF);
    buf[2] = ((freq >> 8) & 0xFF);
    buf[3] = (freq & 0xFF);
    executeOpcode(OP_RF_FREQ_6X, buf, 4);
}

uint32_t SX1262::getFrequency() { return _frequency; }

void SX1262::setTxPower(int level) {
    writeRegister(REG_TX_CLAMP_CONFIG_6X, readRegister(REG_TX_CLAMP_CONFIG_6X) | (0x0F << 1));
    uint8_t pa_buf[4];
    pa_buf[0] = 0x04; pa_buf[1] = 0x07; pa_buf[2] = 0x00; pa_buf[3] = 0x01;
    executeOpcode(OP_PA_CONFIG_6X, pa_buf, 4);
    if (level > 22) level = 22;
    else if (level < -9) level = -9;
    _txp = level;
    writeRegister(REG_OCP_6X, LORA_OCP_TUNED);
    uint8_t tx_buf[2];
    tx_buf[0] = level;
    tx_buf[1] = 0x02;  // PA ramp: 40us
    executeOpcode(OP_TX_PARAMS_6X, tx_buf, 2);
}

int8_t SX1262::getTxPower() { return _txp; }

void SX1262::setSpreadingFactor(int sf) {
    if (sf < 5) sf = 5;
    else if (sf > 12) sf = 12;
    _sf = sf;
    handleLowDataRate();
    setModulationParams(sf, _bw, _cr, _ldro);
}

uint8_t SX1262::getSpreadingFactor() { return _sf; }

uint32_t SX1262::getSignalBandwidth() {
    return RadioBandwidth::fromCode(_bw);
}

void SX1262::setSignalBandwidth(uint32_t sbw) {
    _bw = RadioBandwidth::code(sbw);
    handleLowDataRate();
    setModulationParams(_sf, _bw, _cr, _ldro);
}

void SX1262::setCodingRate4(int denominator) {
    if (denominator < 5) denominator = 5;
    else if (denominator > 8) denominator = 8;
    _cr = denominator - 4;
    setModulationParams(_sf, _bw, _cr, _ldro);
}

uint8_t SX1262::getCodingRate4() { return _cr + 4; }

void SX1262::setPreambleLength(long length) {
    _preambleLength = length;
    setPacketParams(length, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::setInvertIQ(bool invert) {
    _invertIq = invert;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::enableCrc() {
    _crcMode = 1;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::disableCrc() {
    _crcMode = 0;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::setModulationParams(uint8_t sf, uint8_t bw, uint8_t cr, int ldro) {
    // SetModulationParams is only valid in STDBY mode (SX1262 DS Table 11-2).
    // Calling from RX/TX mode is silently rejected by the hardware.
    standby();
    if (!ensureLoRaMode("setModulationParams")) return;

    // Match the RNode SX1262 driver: write the complete LoRa parameter block,
    // including reserved trailing bytes, so modem state is not left stale.
    uint8_t buf[8] = {sf, bw, cr, (uint8_t)ldro, 0x00, 0x00, 0x00, 0x00};
    executeOpcode(OP_MODULATION_PARAMS_6X, buf, 8);
}

void SX1262::setPacketParams(uint32_t preamble, uint8_t headermode, uint8_t length, uint8_t crc) {
    if (!ensureLoRaMode("setPacketParams")) return;

    // Match the RNode SX1262 driver: write the complete packet parameter block.
    uint8_t buf[9];
    buf[0] = (uint8_t)((preamble & 0xFF00) >> 8);
    buf[1] = (uint8_t)(preamble & 0x00FF);
    buf[2] = headermode;
    buf[3] = length;
    buf[4] = crc;
    buf[5] = _invertIq ? 0x01 : 0x00;
    buf[6] = 0x00;
    buf[7] = 0x00;
    buf[8] = 0x00;
    executeOpcode(OP_PACKET_PARAMS_6X, buf, 9);

    // SX1262 errata 15.1: IQ polarity register must be corrected after SetPacketParams.
    // For standard IQ (no inversion), bit 2 of register 0x0736 must be SET.
    // For inverted IQ, bit 2 must be CLEARED. (RadioLib: SX126x.cpp)
    uint8_t iqReg = readRegister(REG_IQ_POLARITY_6X);
    if (_invertIq) {
        iqReg &= ~0x04;
    } else {
        iqReg |= 0x04;
    }
    writeRegister(REG_IQ_POLARITY_6X, iqReg);
}

void SX1262::setSyncWord(uint16_t sw) {
    writeRegister(REG_SYNC_WORD_MSB_6X, 0x14);
    writeRegister(REG_SYNC_WORD_LSB_6X, 0x24);
}

void SX1262::explicitHeaderMode() {
    _implicitHeaderMode = 0;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::implicitHeaderMode() {
    _implicitHeaderMode = 1;
    setPacketParams(_preambleLength, _implicitHeaderMode, _payloadLength, _crcMode);
}

void SX1262::handleLowDataRate() {
    _ldro = handheld::sx1262_timing::lowDataRateOptimize(_sf, _bw);
}

void SX1262::standby() {
    uint8_t byte = _tcxo ? MODE_STDBY_XOSC_6X : MODE_STDBY_RC_6X;
    executeOpcode(OP_STANDBY_6X, &byte, 1);
    _rxArmed = _dcOn = false;              // standby ends RX and any duty cycle
    waitOnBusy(_tcxo ? handheld::sx1262_timing::tcxoTimeoutMs() : 100);
}

void SX1262::sleep() {
    standby();
    // Warm sleep retains the programmed modem tuple for a later wake-up.
    uint8_t byte = 0x04;
    executeOpcode(OP_SLEEP_6X, &byte, 1);
    if (!_ioFailed) { _sleeping = true; delayMicroseconds(500); }
}

float SX1262::getAirtime(uint16_t written) {
    if (!_radioOnline) return 0;
    const uint32_t milliseconds = handheld::sx1262_timing::airtimeMs(written, {
        _sf, _bw, getCodingRate4(), static_cast<uint16_t>(_preambleLength),
        _implicitHeaderMode != 0, _crcMode != 0, _ldro});
    // Keep the existing float API for accounting/diagnostic callers. Supported
    // configurations are exactly representable; also round conservatively for
    // callers that program the full 16-bit preamble register directly.
    const float result = static_cast<float>(milliseconds);
    return static_cast<double>(result) < milliseconds ? std::nextafter(result, INFINITY) : result;
}

uint32_t SX1262::getBitrate() {
    return handheld::sx1262_timing::bitrate(_sf, _bw, getCodingRate4());
}

void IRAM_ATTR SX1262::onDio0Rise() {
    if (_instance) {
        _instance->packetAvailable = true;
        // Don't call handleDio0Rise() from ISR — it does SPI which deadlocks
        // on the shared bus when the display holds the SPI mutex (causes
        // "Interrupt wdt timeout on CPU1" crash). LoRaInterface::loop() polls
        // packetAvailable and reads the radio buffer from main loop context.
    }
}

void SX1262::handleDio0Rise() {
    _packetIndex = 0;
    uint8_t rxbuf[2] = {0};
    executeOpcodeRead(OP_RX_BUFFER_STATUS_6X, rxbuf, 2);
    int packetLength = rxbuf[0];
    if (_onReceive) { _onReceive(packetLength); }
}

void SX1262::onReceive(void(*callback)(int)) {
    _onReceive = callback;
    if (callback) {
        pinMode(_irq, INPUT);
        uint8_t buf[8] = {0xFF, 0xFF, 0x00, IRQ_RX_DONE_MASK_6X, 0x00, 0x00, 0x00, 0x00};
        executeOpcode(OP_SET_IRQ_FLAGS_6X, buf, 8);
        enableDio2RfSwitch();
        attachInterrupt(digitalPinToInterrupt(_irq), onDio0Rise, RISING);
    } else {
        detachInterrupt(digitalPinToInterrupt(_irq));
    }
}

uint8_t SX1262::random() {
    const uint8_t v = readRegister(REG_RANDOM_GEN_6X);
    rxHeal();
    return v;
}
