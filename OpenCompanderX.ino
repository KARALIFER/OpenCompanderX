/*
  OCX Type 2 cassette decoder - firmware for Teensy 4.1 + Teensy Audio Adaptor Rev D/D2

  Practical scope:
  - Decoder-only playback path for analog line input to analog line/headphone output
  - One conservative universal playback profile for TEAC W-1200 line-out and portable headphone sources
  - Real-time-safe sample loop: no dynamic allocation in update(), no Serial I/O in update(), no heap DSP objects
*/

#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <EEPROM.h>
#include <math.h>
#include <string.h>

// Forward declarations to keep Arduino auto-prototype generation safe for
// functions that mention structs before their full definitions appear.
struct PersistSettings;
struct CalProfile;
void factoryResetSettings();
void setAutoRejectReason(const char* reason);

namespace OCXProfile {
static constexpr float kFs = AUDIO_SAMPLE_RATE_EXACT;
static constexpr int kAudioMemoryBlocks = 64;

static constexpr int kLineInLevel = 0;
static constexpr int kLineOutLevel = 29;
static constexpr float kHeadphoneVolume = 0.45f;
static constexpr float kInputTrimDb = -3.0f;
static constexpr float kOutputTrimDb = -1.0f;
static constexpr float kStrength = 0.76f;
static constexpr float kReferenceDb = -18.0f;
static constexpr float kMaxBoostDb = 9.0f;
static constexpr float kMaxCutDb = 24.0f;
static constexpr float kAttackMs = 3.5f;
static constexpr float kReleaseMs = 140.0f;
static constexpr float kSidechainHpHz = 30.0f;
static constexpr float kSidechainLpHz = 10000.0f;
static constexpr float kSidechainShelfHz = 2800.0f;
static constexpr float kSidechainShelfDb = 16.0f;
static constexpr float kDetectorRmsMs = 8.0f;
static constexpr bool  kAutoTrimEnabled = true;
static constexpr float kAutoTrimMaxDb = 1.5f;
static constexpr float kDropoutHoldMs = 0.0f;
static constexpr float kDropoutHfDropDb = 10.0f;
static constexpr float kDropoutLevelDropDb = 9.0f;
static constexpr bool  kSaturationSoftfail = false;
static constexpr float kSaturationThreshold = 0.90f;
static constexpr float kSaturationKneeDb = 4.0f;
static constexpr bool  kRestorationAutoTrimEnabled = true;
static constexpr float kRestorationAutoTrimMaxDb = 2.0f;
static constexpr float kRestorationDropoutHoldMs = 12.0f;
static constexpr float kRestorationDropoutHfDropDb = 8.0f;
static constexpr float kRestorationDropoutLevelDropDb = 7.0f;
static constexpr bool  kRestorationSaturationSoftfail = true;
static constexpr float kRestorationSaturationThreshold = 0.86f;
static constexpr float kRestorationSaturationKneeDb = 6.0f;
static constexpr float kDeemphHz = 1850.0f;
static constexpr float kDeemphDb = -6.0f;
static constexpr float kSoftClipDrive = 1.08f;
static constexpr float kDcBlockHz = 12.0f;
static constexpr float kHeadroomDb = 1.0f;
static constexpr float kEncInputTrimDb = 0.0f;
static constexpr float kEncOutputTrimDb = -1.0f;
static constexpr float kEncStrength = 0.45f;
static constexpr float kEncReferenceDb = -18.0f;
static constexpr float kEncMaxBoostDb = 24.0f;
static constexpr float kEncMaxCutDb = 9.0f;
static constexpr float kEncAttackMs = 3.5f;
static constexpr float kEncReleaseMs = 140.0f;
static constexpr float kEncSidechainHpHz = 90.0f;
static constexpr float kEncSidechainLpHz = 10000.0f;
static constexpr float kEncSidechainShelfHz = 2800.0f;
static constexpr float kEncSidechainShelfDb = 14.0f;
static constexpr float kEncDetectorRmsMs = 8.0f;
static constexpr float kEncTiltHz = 1850.0f;
static constexpr float kEncTiltDb = 5.0f;
static constexpr float kEncSoftClipDrive = 1.04f;
static constexpr float kEncDcBlockHz = 12.0f;
static constexpr float kEncHeadroomDb = 1.0f;
// Calibration tone defaults are deck/workflow calibration parameters, not decoder model reference.
static constexpr float kToneHz = 400.0f;
static constexpr float kToneDb = -9.8f;
}

enum PresetId : uint8_t { PRESET_UNIVERSAL = 0, PRESET_AUTO_CAL = 1 };
enum AutoCalState : uint8_t { AUTO_IDLE = 0, AUTO_WAIT_FOR_TONE = 1, AUTO_MEASURE = 2, AUTO_COMPUTE = 3, AUTO_LOCKED = 4, AUTO_FAILED = 5 };
// Keep GuardState visible before Arduino auto-generated prototypes so
// guardMarkChanged(...) signatures remain valid in Arduino-CLI builds.
enum GuardState : uint8_t { GUARD_IDLE = 0, GUARD_BRAKE_A = 1, GUARD_PROTECT_B = 2, GUARD_RELAX_C = 3, GUARD_SETTLED = 4 };
enum DeckType : uint8_t { DECK_SINGLE_LW = 0, DECK_DUAL_LW = 1 };
enum TransportId : uint8_t { TRANSPORT_LW1 = 0, TRANSPORT_LW2 = 1 };
enum ProfileSelect : uint8_t { PROFILE_SINGLE = 0, PROFILE_LW1 = 1, PROFILE_LW2 = 2, PROFILE_COMMON = 3 };
enum DecoderOperatingMode : uint8_t { DECODER_STRICT_COMPATIBLE = 0, DECODER_RESTORATION = 1, DECODER_CONTROLLED_RECORD = 2 };
enum PlaybackSafetyMode : uint8_t { SAFETY_STRICT_REFERENCE = 0, SAFETY_STRICT_SAFE = 1, SAFETY_PLAYBACK_ADAPTIVE = 2 };


static inline float clampf(float x, float lo, float hi) {
  return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline float sanitizef(float x) {
  return isfinite(x) ? x : 0.0f;
}

static inline float dbToLin(float db) {
  return powf(10.0f, db / 20.0f);
}

static inline float linToDb(float x) {
  return 20.0f * log10f(fmaxf(x, 1.0e-12f));
}

static inline float softClip(float x, float drive) {
  return tanhf(drive * x) / tanhf(drive);
}

struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
  float a1 = 0.0f, a2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f;

  inline float process(float x) {
    x = sanitizef(x);
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    z1 = sanitizef(z1);
    z2 = sanitizef(z2);
    return sanitizef(y);
  }

  inline void reset() {
    z1 = 0.0f;
    z2 = 0.0f;
  }
};

struct ClipDelta {
  uint32_t inputNew = 0;
  uint32_t outputNew = 0;
};

struct OnePoleHP {
  float alpha = 0.0f;
  float prevX = 0.0f;
  float prevY = 0.0f;

  inline void design(float fs, float hz) {
    hz = clampf(hz, 0.1f, 200.0f);
    const float rc = 1.0f / (2.0f * 3.14159265359f * hz);
    const float dt = 1.0f / fs;
    alpha = rc / (rc + dt);
    reset();
  }

  inline float process(float x) {
    float y = alpha * (prevY + x - prevX);
    prevX = x;
    prevY = y;
    return sanitizef(y);
  }

  inline void reset() {
    prevX = 0.0f;
    prevY = 0.0f;
  }
};

static void designHighpass(Biquad &f, float fs, float hz, float q = 0.7071f) {
  hz = clampf(hz, 10.0f, fs * 0.45f);
  const float w0 = 2.0f * 3.14159265359f * hz / fs;
  const float cosw0 = cosf(w0);
  const float sinw0 = sinf(w0);
  const float alpha = sinw0 / (2.0f * q);

  const float b0 =  (1.0f + cosw0) * 0.5f;
  const float b1 = -(1.0f + cosw0);
  const float b2 =  (1.0f + cosw0) * 0.5f;
  const float a0 =   1.0f + alpha;
  const float a1 =  -2.0f * cosw0;
  const float a2 =   1.0f - alpha;

  f.b0 = b0 / a0;
  f.b1 = b1 / a0;
  f.b2 = b2 / a0;
  f.a1 = a1 / a0;
  f.a2 = a2 / a0;
  f.reset();
}

static void designHighShelf(Biquad &f, float fs, float hz, float gainDb, float slope = 0.8f) {
  hz = clampf(hz, 100.0f, fs * 0.45f);
  const float A = powf(10.0f, gainDb / 40.0f);
  const float w0 = 2.0f * 3.14159265359f * hz / fs;
  const float cosw0 = cosf(w0);
  const float sinw0 = sinf(w0);
  const float alpha = sinw0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f);
  const float beta = 2.0f * sqrtf(A) * alpha;

  const float b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + beta);
  const float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
  const float b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - beta);
  const float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + beta;
  const float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
  const float a2 = (A + 1.0f) - (A - 1.0f) * cosw0 - beta;

  f.b0 = b0 / a0;
  f.b1 = b1 / a0;
  f.b2 = b2 / a0;
  f.a1 = a1 / a0;
  f.a2 = a2 / a0;
  f.reset();
}

static void designLowpass(Biquad &f, float fs, float hz, float q = 0.7071f) {
  hz = clampf(hz, 100.0f, fs * 0.45f);
  const float w0 = 2.0f * 3.14159265359f * hz / fs;
  const float cosw0 = cosf(w0);
  const float sinw0 = sinf(w0);
  const float alpha = sinw0 / (2.0f * q);
  const float b0 = (1.0f - cosw0) * 0.5f;
  const float b1 = 1.0f - cosw0;
  const float b2 = (1.0f - cosw0) * 0.5f;
  const float a0 = 1.0f + alpha;
  const float a1 = -2.0f * cosw0;
  const float a2 = 1.0f - alpha;
  f.b0 = b0 / a0;
  f.b1 = b1 / a0;
  f.b2 = b2 / a0;
  f.a1 = a1 / a0;
  f.a2 = a2 / a0;
  f.reset();
}

class AudioEffectOCXType2CodecStereo : public AudioStream {
public:
  enum Mode : uint8_t { MODE_DECODE = 0, MODE_ENCODE = 1 };
  AudioEffectOCXType2CodecStereo() : AudioStream(2, inputQueueArray) { recalcAll(); resetState(); armOutputSoftStart(); }
  virtual void update(void);
  struct DiagSnapshot {
    float inputPeakL = 0.0f;
    float inputPeakR = 0.0f;
    float outputPeakL = 0.0f;
    float outputPeakR = 0.0f;
    float pkInRaw = 0.0f;
    float pkAfterInputTrim = 0.0f;
    float pkAfterDecodeGain = 0.0f;
    float pkAfterDeEmphasis = 0.0f;
    float pkPreGuard = 0.0f;
    float pkPostGuard = 0.0f;
    float pkPreSoftClip = 0.0f;
    float pkPostSoftClip = 0.0f;
    float pkPostOutputTrim = 0.0f;
    float inputSumSqL = 0.0f;
    float inputSumSqR = 0.0f;
    float outputSumSqL = 0.0f;
    float outputSumSqR = 0.0f;
    float inputMeanL = 0.0f;
    float inputMeanR = 0.0f;
    float outputMeanL = 0.0f;
    float outputMeanR = 0.0f;
    uint32_t sampleCount = 0;
    float lastGainDb = 0.0f;
    float minGainDb = 0.0f;
    float maxGainDb = 0.0f;
    float avgGainDb = 0.0f;
    float lastEnvDb = -120.0f;
    uint32_t gainSampleCount = 0;
    uint32_t decoderActiveCount = 0;
    uint32_t bypassSampleCount = 0;
    uint32_t decodedSampleCount = 0;
    uint32_t clampCutHitCount = 0;
    uint32_t clampBoostHitCount = 0;
    uint32_t nearCutCount = 0;
    uint32_t nearBoostCount = 0;
    float inputAbsDiffMean = 0.0f;
    float outputAbsDiffMean = 0.0f;
    float inputCrossMean = 0.0f;
    float outputCrossMean = 0.0f;
    float lowProxyMean = 0.0f;
    float highProxyMean = 0.0f;
  };

  void setBypass(bool v)             { if (bypass != v) { bypass = v; armOutputSoftStart(); } }
  bool getBypass() const             { return bypass; }
  void setMode(Mode m)               { if (mode != m) { mode = m; armOutputSoftStart(); } }
  Mode getMode() const               { return mode; }
  void setInputTrimDb(float db)      { inputTrimDb = clampf(db, -18.0f, 18.0f); inputGain = dbToLin(inputTrimDb); }
  void setOutputTrimDb(float db)     { outputTrimDb = clampf(db, -18.0f, 18.0f); outputGain = dbToLin(outputTrimDb); }
  void setStrength(float v)          { strength = clampf(v, 0.0f, 1.25f); }
  void setReferenceDb(float db)      { referenceDb = clampf(db, -40.0f, 0.0f); }
  void setMaxBoostDb(float db)       { maxBoostDb = clampf(db, 0.0f, 24.0f); }
  void setMaxCutDb(float db)         { maxCutDb = clampf(db, 0.0f, 36.0f); }
  void setAttackMs(float ms)         { attackMs = clampf(ms, 0.2f, 100.0f); recalcDetector(); }
  void setReleaseMs(float ms)        { releaseMs = clampf(ms, 5.0f, 1000.0f); recalcDetector(); }
  void setSidechainHpHz(float hz)    { sidechainHpHz = clampf(hz, 20.0f, 1000.0f); recalcSidechainFilters(); }
  void setSidechainLpHz(float hz)    { sidechainLpHz = clampf(hz, 3000.0f, 16000.0f); recalcSidechainFilters(); }
  void setSidechainShelfHz(float hz) { sidechainShelfHz = clampf(hz, 500.0f, 12000.0f); recalcSidechainFilters(); }
  void setSidechainShelfDb(float db) { sidechainShelfDb = clampf(db, 0.0f, 24.0f); recalcSidechainFilters(); }
  void setDetectorRmsMs(float ms)    { detectorRmsMs = clampf(ms, 0.5f, 80.0f); recalcDetector(); }
  void setDeemphHz(float hz)         { deemphHz = clampf(hz, 300.0f, 12000.0f); recalcDeemphFilter(); }
  void setDeemphDb(float db)         { deemphDb = clampf(db, -24.0f, 0.0f); recalcDeemphFilter(); }
  void setSoftClipDrive(float v)     { softClipDrive = clampf(v, 1.0f, 2.0f); }
  void setDcBlockHz(float hz)        { dcBlockHz = clampf(hz, 1.0f, 40.0f); recalcDcBlockers(); }
  void setHeadroomDb(float db)       { headroomDb = clampf(db, 0.0f, 6.0f); headroomGain = dbToLin(-headroomDb); }
  void setAutoTrimEnabled(bool v)       { autoTrimEnabled = v; }
  void setAutoTrimMaxDb(float db)       { autoTrimMaxDb = clampf(fabsf(db), 0.0f, 6.0f); }
  void setDropoutHoldMs(float ms)       { dropoutHoldMs = clampf(ms, 0.0f, 40.0f); recalcDetector(); }
  void setDropoutHfDropDb(float db)     { dropoutHfDropDb = clampf(db, 1.0f, 24.0f); }
  void setDropoutLevelDropDb(float db)  { dropoutLevelDropDb = clampf(db, 1.0f, 24.0f); }
  void setSaturationSoftfail(bool v)    { saturationSoftfail = v; }
  void setSaturationThreshold(float v)  { saturationThreshold = clampf(v, 0.70f, 0.99f); }
  void setSaturationKneeDb(float db)    { saturationKneeDb = clampf(db, 0.0f, 12.0f); }
  void setDecoderOperatingMode(DecoderOperatingMode m) { if (decoderMode != m) armOutputSoftStart(); decoderMode = m; recalcDetector(); }
  void setOutputSoftStartEnabled(bool v) { outputSoftStartEnabled = v; if (!v) outputSoftStartSamplesRemaining = 0; }
  void armOutputSoftStart() { outputSoftStartSamplesRemaining = kOutputSoftStartSamples; }

  float getInputTrimDb() const       { return inputTrimDb; }
  float getOutputTrimDb() const      { return outputTrimDb; }
  float getStrength() const          { return strength; }
  float getReferenceDb() const       { return referenceDb; }
  float getAttackMs() const          { return attackMs; }
  float getReleaseMs() const         { return releaseMs; }
  float getSidechainHpHz() const     { return sidechainHpHz; }
  float getSidechainLpHz() const     { return sidechainLpHz; }
  float getSidechainShelfHz() const  { return sidechainShelfHz; }
  float getSidechainShelfDb() const  { return sidechainShelfDb; }
  float getDetectorRmsMs() const     { return detectorRmsMs; }
  float getDeemphHz() const          { return deemphHz; }
  float getDeemphDb() const          { return deemphDb; }
  float getSoftClipDrive() const     { return softClipDrive; }
  float getDcBlockHz() const         { return dcBlockHz; }
  float getHeadroomDb() const        { return headroomDb; }
  bool getAutoTrimEnabled() const    { return autoTrimEnabled; }
  float getAutoTrimMaxDb() const     { return autoTrimMaxDb; }
  float getDropoutHoldMs() const     { return dropoutHoldMs; }
  float getDropoutHfDropDb() const   { return dropoutHfDropDb; }
  float getDropoutLevelDropDb() const { return dropoutLevelDropDb; }
  bool getSaturationSoftfail() const { return saturationSoftfail; }
  float getSaturationThreshold() const { return saturationThreshold; }
  float getSaturationKneeDb() const  { return saturationKneeDb; }
  DecoderOperatingMode getDecoderOperatingMode() const { return decoderMode; }

  bool hasInputClip() const          { return inputClipFlag; }
  bool hasOutputClip() const         { return outputClipFlag; }
  uint32_t getAllocFailCount() const { return allocFailCount; }
  uint32_t getInputClipCount() const { return inputClipCount; }
  uint32_t getOutputClipCount() const { return outputClipCount; }
  ClipDelta consumeClipDelta();
  void clearRuntimeCounters()        {
    allocFailCount = 0;
    inputClipCount = 0;
    outputClipCount = 0;
    lastReportedInputClipCount = 0;
    lastReportedOutputClipCount = 0;
  }
  void clearClipFlags()              { inputClipFlag = false; outputClipFlag = false; }
  float getLastGainDb() const        { return diagLastGainDb; }
  float getLastEnvDb() const         { return diagLastEnvDb; }
  void resetSignalDiagnostics();
  DiagSnapshot getSignalDiagnosticsSnapshot() const;
  void resetState() {
    clearClipFlags();
    for (int ch = 0; ch < 2; ++ch) {
      scHP[ch].reset();
      scLP[ch].reset();
      scShelf[ch].reset();
      encScHP[ch].reset();
      encScLP[ch].reset();
      encScShelf[ch].reset();
      deemph[ch].reset();
      encTilt[ch].reset();
      dcBlock[ch].reset();
      encDcBlock[ch].reset();
    }
    linkedEnv2 = 1.0e-9f;
    linkedRms2 = 1.0e-9f;
    encLinkedEnv2 = 1.0e-9f;
    encLinkedRms2 = 1.0e-9f;
    autoTrimDb = 0.0f;
    dropoutHoldSamples = 0;
    prevScRms = 1.0e-6f;
    prevInRms = 1.0e-6f;
    prevGainDb = 0.0f;
    outputSoftStartSamplesRemaining = kOutputSoftStartSamples;
    noInterrupts();
    diagLastGainDb = 0.0f;
    diagLastEnvDb = -120.0f;
    interrupts();
  }

private:
  static constexpr float kOutputSoftStartMs = 80.0f;
  static constexpr uint16_t kOutputSoftStartSamples = (uint16_t)(OCXProfile::kFs * (kOutputSoftStartMs * 0.001f));
  audio_block_t *inputQueueArray[2];
  Mode mode = MODE_DECODE;
  bool  bypass = false;
  bool  inputClipFlag = false;
  bool  outputClipFlag = false;
  uint32_t allocFailCount = 0;
  uint32_t inputClipCount = 0;
  uint32_t outputClipCount = 0;

  float inputTrimDb = 0.0f;
  float outputTrimDb = 0.0f;
  float inputGain = 1.0f;
  float outputGain = 1.0f;
  float headroomDb = 1.0f;
  float headroomGain = dbToLin(-1.0f);

  float strength = 0.76f;
  float referenceDb = -18.0f;
  float maxBoostDb = 9.0f;
  float maxCutDb = 24.0f;

  float sidechainHpHz = OCXProfile::kSidechainHpHz;
  float sidechainLpHz = OCXProfile::kSidechainLpHz;
  float sidechainShelfHz = 2800.0f;
  float sidechainShelfDb = 16.0f;
  Biquad scHP[2];
  Biquad scLP[2];
  Biquad scShelf[2];
  float encSidechainHpHz = OCXProfile::kEncSidechainHpHz;
  float encSidechainLpHz = OCXProfile::kEncSidechainLpHz;
  float encSidechainShelfHz = OCXProfile::kEncSidechainShelfHz;
  float encSidechainShelfDb = OCXProfile::kEncSidechainShelfDb;
  Biquad encScHP[2];
  Biquad encScLP[2];
  Biquad encScShelf[2];

  float attackMs = 3.5f;
  float releaseMs = 140.0f;
  float detectorRmsMs = OCXProfile::kDetectorRmsMs;
  float attackCoeff = 0.0f;
  float releaseCoeff = 0.0f;
  float rmsCoeff = 0.0f;
  float linkedEnv2 = 1.0e-9f;
  float linkedRms2 = 1.0e-9f;
  float encAttackMs = OCXProfile::kEncAttackMs;
  float encReleaseMs = OCXProfile::kEncReleaseMs;
  float encDetectorRmsMs = OCXProfile::kEncDetectorRmsMs;
  float encAttackCoeff = 0.0f;
  float encReleaseCoeff = 0.0f;
  float encRmsCoeff = 0.0f;
  float encLinkedEnv2 = 1.0e-9f;
  float encLinkedRms2 = 1.0e-9f;
  DecoderOperatingMode decoderMode = DECODER_STRICT_COMPATIBLE;
  float autoTrimDb = 0.0f;
  bool autoTrimEnabled = OCXProfile::kAutoTrimEnabled;
  float autoTrimMaxDb = OCXProfile::kAutoTrimMaxDb;
  float dropoutHoldMs = OCXProfile::kDropoutHoldMs;
  float dropoutHfDropDb = OCXProfile::kDropoutHfDropDb;
  float dropoutLevelDropDb = OCXProfile::kDropoutLevelDropDb;
  bool saturationSoftfail = OCXProfile::kSaturationSoftfail;
  float saturationThreshold = OCXProfile::kSaturationThreshold;
  float saturationKneeDb = OCXProfile::kSaturationKneeDb;
  bool outputSoftStartEnabled = true;
  float autoTrimCoeff = 0.0f;
  uint16_t dropoutHoldSamples = 0;
  uint16_t dropoutHoldCounter = 0;
  float prevScRms = 1.0e-6f;
  float prevInRms = 1.0e-6f;
  float prevGainDb = 0.0f;

  float deemphHz = 1850.0f;
  float deemphDb = -6.0f;
  Biquad deemph[2];

  float softClipDrive = 1.08f;
  float dcBlockHz = 12.0f;
  OnePoleHP dcBlock[2];
  float encDcBlockHz = OCXProfile::kEncDcBlockHz;
  OnePoleHP encDcBlock[2];
  float encInputGain = 1.0f;
  float encOutputGain = 1.0f;
  float encHeadroomGain = 1.0f;
  float encStrength = OCXProfile::kEncStrength;
  float encReferenceDb = OCXProfile::kEncReferenceDb;
  float encMaxBoostDb = OCXProfile::kEncMaxBoostDb;
  float encMaxCutDb = OCXProfile::kEncMaxCutDb;
  float encSoftClipDrive = OCXProfile::kEncSoftClipDrive;
  Biquad encTilt[2];

  float diagInputPeakL = 0.0f;
  float diagInputPeakR = 0.0f;
  float diagOutputPeakL = 0.0f;
  float diagOutputPeakR = 0.0f;
  float diagInputSumSqL = 0.0f;
  float diagInputSumSqR = 0.0f;
  float diagOutputSumSqL = 0.0f;
  float diagOutputSumSqR = 0.0f;
  float diagInputSumL = 0.0f;
  float diagInputSumR = 0.0f;
  float diagOutputSumL = 0.0f;
  float diagOutputSumR = 0.0f;
  uint32_t diagSampleCount = 0;
  float diagLastGainDb = 0.0f;
  float diagMinGainDb = 0.0f;
  float diagMaxGainDb = 0.0f;
  float diagGainDbSum = 0.0f;
  uint32_t diagGainCount = 0;
  float diagLastEnvDb = -120.0f;
  uint32_t diagDecoderActiveCount = 0;
  uint32_t diagBypassSampleCount = 0;
  uint32_t diagDecodedSampleCount = 0;
  uint32_t diagClampCutHitCount = 0;
  uint32_t diagClampBoostHitCount = 0;
  uint32_t diagNearCutCount = 0;
  uint32_t diagNearBoostCount = 0;
  float diagInputDiffAbsSum = 0.0f;
  float diagOutputDiffAbsSum = 0.0f;
  float diagInputCrossSum = 0.0f;
  float diagOutputCrossSum = 0.0f;
  float diagLowProxySum = 0.0f;
  float diagHighProxySum = 0.0f;
  float diagPkInRaw = 0.0f;
  float diagPkAfterInputTrim = 0.0f;
  float diagPkAfterDecodeGain = 0.0f;
  float diagPkAfterDeEmphasis = 0.0f;
  float diagPkPreGuard = 0.0f;
  float diagPkPostGuard = 0.0f;
  float diagPkPreSoftClip = 0.0f;
  float diagPkPostSoftClip = 0.0f;
  float diagPkPostOutputTrim = 0.0f;
  uint32_t lastReportedInputClipCount = 0;
  uint32_t lastReportedOutputClipCount = 0;
  uint16_t outputSoftStartSamplesRemaining = 0;

  void recalcAll() {
    setInputTrimDb(inputTrimDb);
    setOutputTrimDb(outputTrimDb);
    setHeadroomDb(headroomDb);
    recalcSidechainFilters();
    recalcEncoderSidechainFilters();
    recalcDetector();
    recalcEncoderDetector();
    recalcDeemphFilter();
    recalcDcBlockers();
    recalcEncoderDcBlockers();
    encInputGain = dbToLin(OCXProfile::kEncInputTrimDb);
    encOutputGain = dbToLin(OCXProfile::kEncOutputTrimDb);
    encHeadroomGain = dbToLin(-OCXProfile::kEncHeadroomDb);
    for (int ch = 0; ch < 2; ++ch) designHighShelf(encTilt[ch], OCXProfile::kFs, OCXProfile::kEncTiltHz, OCXProfile::kEncTiltDb, 0.8f);
  }

  void recalcDetector() {
    attackCoeff  = expf(-1.0f / (OCXProfile::kFs * attackMs  * 0.001f));
    releaseCoeff = expf(-1.0f / (OCXProfile::kFs * releaseMs * 0.001f));
    rmsCoeff = expf(-1.0f / (OCXProfile::kFs * detectorRmsMs * 0.001f));
    autoTrimCoeff = expf(-1.0f / (OCXProfile::kFs * 0.8f));
    dropoutHoldSamples = (uint16_t)clampf(dropoutHoldMs * OCXProfile::kFs * 0.001f, 0.0f, 4095.0f);
  }
  void recalcEncoderDetector() {
    encAttackCoeff = expf(-1.0f / (OCXProfile::kFs * encAttackMs * 0.001f));
    encReleaseCoeff = expf(-1.0f / (OCXProfile::kFs * encReleaseMs * 0.001f));
    encRmsCoeff = expf(-1.0f / (OCXProfile::kFs * encDetectorRmsMs * 0.001f));
  }

  void recalcSidechainFilters() {
    for (int ch = 0; ch < 2; ++ch) {
      designHighpass(scHP[ch], OCXProfile::kFs, sidechainHpHz, 0.7071f);
      designLowpass(scLP[ch], OCXProfile::kFs, sidechainLpHz, 0.7071f);
      designHighShelf(scShelf[ch], OCXProfile::kFs, sidechainShelfHz, sidechainShelfDb, 0.8f);
    }
  }
  void recalcEncoderSidechainFilters() {
    for (int ch = 0; ch < 2; ++ch) {
      designHighpass(encScHP[ch], OCXProfile::kFs, encSidechainHpHz, 0.7071f);
      designLowpass(encScLP[ch], OCXProfile::kFs, encSidechainLpHz, 0.7071f);
      designHighShelf(encScShelf[ch], OCXProfile::kFs, encSidechainShelfHz, encSidechainShelfDb, 0.8f);
    }
  }

  void recalcDeemphFilter() {
    for (int ch = 0; ch < 2; ++ch) designHighShelf(deemph[ch], OCXProfile::kFs, deemphHz, deemphDb, 0.8f);
  }

  void recalcDcBlockers() {
    for (int ch = 0; ch < 2; ++ch) dcBlock[ch].design(OCXProfile::kFs, dcBlockHz);
  }
  void recalcEncoderDcBlockers() {
    for (int ch = 0; ch < 2; ++ch) encDcBlock[ch].design(OCXProfile::kFs, encDcBlockHz);
  }

  inline void updatePeak(float &dst, float v) {
    if (v > dst) dst = v;
  }

  inline float finalizeOutput(float y, float outGain, float roomGain, float clipDrive, float softStartGain) {
    const float preGuard = y;
    const float postGuard = preGuard * outGain * roomGain;
    const float preSoftClip = postGuard * softStartGain;
    const float postSoftClip = softClip(preSoftClip, clipDrive);
    updatePeak(diagPkPreGuard, fabsf(preGuard));
    updatePeak(diagPkPostGuard, fabsf(postGuard));
    updatePeak(diagPkPreSoftClip, fabsf(preSoftClip));
    updatePeak(diagPkPostSoftClip, fabsf(postSoftClip));
    y = clampf(postSoftClip, -1.0f, 1.0f);
    updatePeak(diagPkPostOutputTrim, fabsf(y));
    if (fabsf(y) > 0.98f) {
      outputClipFlag = true;
      ++outputClipCount;
    }
    return sanitizef(y);
  }

  inline void processDecode(float xL, float xR, float &outL, float &outR, float softStartGain) {
    const float scL = scShelf[0].process(scLP[0].process(scHP[0].process(xL)));
    const float scR = scShelf[1].process(scLP[1].process(scHP[1].process(xR)));
    const float lowProxy = 0.5f * (fabsf(xL) + fabsf(xR));
    const float highProxy = 0.5f * (fabsf(scL) + fabsf(scR));
    diagLowProxySum += lowProxy;
    diagHighProxySum += highProxy;
    const float linkedP = fmaxf(scL * scL, scR * scR);
    linkedRms2 = sanitizef(rmsCoeff * linkedRms2 + (1.0f - rmsCoeff) * linkedP);
    const float coeff = (linkedRms2 > linkedEnv2) ? attackCoeff : releaseCoeff;
    linkedEnv2 = sanitizef(coeff * linkedEnv2 + (1.0f - coeff) * linkedRms2);
    const float env = sqrtf(linkedEnv2 + 1.0e-12f);
    if (autoTrimEnabled && decoderMode != DECODER_CONTROLLED_RECORD) {
      const float envDb = linToDb(env);
      if (envDb < -36.0f) {
        const float target = clampf((referenceDb - envDb) * 0.12f, -autoTrimMaxDb, autoTrimMaxDb);
        autoTrimDb = sanitizef(autoTrimCoeff * autoTrimDb + (1.0f - autoTrimCoeff) * target);
      }
    }
    const float rawGainDb = (linToDb(env) - referenceDb + autoTrimDb) * strength;
    float gainDb = clampf(rawGainDb, -maxCutDb, maxBoostDb);
    if (decoderMode == DECODER_RESTORATION) {
      const float scRms = sqrtf(0.5f * (scL * scL + scR * scR) + 1.0e-12f);
      const float inRms = sqrtf(0.5f * (xL * xL + xR * xR) + 1.0e-12f);
      const float hfDropDb = linToDb(prevScRms / fmaxf(scRms, 1.0e-12f));
      const float levelDropDb = linToDb(prevInRms / fmaxf(inRms, 1.0e-12f));
      if (hfDropDb > dropoutHfDropDb && levelDropDb > dropoutLevelDropDb) dropoutHoldCounter = dropoutHoldSamples;
      if (dropoutHoldCounter > 0) {
        --dropoutHoldCounter;
        gainDb = 0.8f * prevGainDb + 0.2f * gainDb;
      }
      const float peak = fmaxf(fabsf(xL), fabsf(xR));
      if (saturationSoftfail && peak > saturationThreshold && gainDb > 0.0f) {
        const float over = clampf((peak - saturationThreshold) / fmaxf(1.0e-4f, 1.0f - saturationThreshold), 0.0f, 1.0f);
        gainDb -= over * saturationKneeDb;
      }
      prevScRms = scRms;
      prevInRms = inRms;
    } else {
      prevScRms = sqrtf(0.5f * (scL * scL + scR * scR) + 1.0e-12f);
      prevInRms = sqrtf(0.5f * (xL * xL + xR * xR) + 1.0e-12f);
      dropoutHoldCounter = 0;
    }
    prevGainDb = gainDb;
    if (rawGainDb <= -maxCutDb) ++diagClampCutHitCount;
    if (rawGainDb >=  maxBoostDb) ++diagClampBoostHitCount;
    if (gainDb <= (-maxCutDb + 1.0f)) ++diagNearCutCount;
    if (gainDb >= (maxBoostDb - 1.0f)) ++diagNearBoostCount;
    diagLastGainDb = gainDb;
    if (diagGainCount == 0) { diagMinGainDb = gainDb; diagMaxGainDb = gainDb; } else { if (gainDb < diagMinGainDb) diagMinGainDb = gainDb; if (gainDb > diagMaxGainDb) diagMaxGainDb = gainDb; }
    diagGainDbSum += gainDb;
    ++diagGainCount;
    if (fabsf(gainDb) >= 1.0f) ++diagDecoderActiveCount;
    ++diagDecodedSampleCount;
    diagLastEnvDb = linToDb(env);
    const float gainLin = dbToLin(gainDb);
    const float decL = xL * gainLin;
    const float decR = xR * gainLin;
    updatePeak(diagPkAfterDecodeGain, fmaxf(fabsf(decL), fabsf(decR)));
    const float deemL = deemph[0].process(decL);
    const float deemR = deemph[1].process(decR);
    updatePeak(diagPkAfterDeEmphasis, fmaxf(fabsf(deemL), fabsf(deemR)));
    outL = finalizeOutput(deemL, outputGain, headroomGain, softClipDrive, softStartGain);
    outR = finalizeOutput(deemR, outputGain, headroomGain, softClipDrive, softStartGain);
  }

  inline void processEncode(float xL, float xR, float &outL, float &outR, float softStartGain) {
    const float scL = encScShelf[0].process(encScLP[0].process(encScHP[0].process(xL)));
    const float scR = encScShelf[1].process(encScLP[1].process(encScHP[1].process(xR)));
    const float linkedP = fmaxf(scL * scL, scR * scR);
    encLinkedRms2 = sanitizef(encRmsCoeff * encLinkedRms2 + (1.0f - encRmsCoeff) * linkedP);
    const float coeff = (encLinkedRms2 > encLinkedEnv2) ? encAttackCoeff : encReleaseCoeff;
    encLinkedEnv2 = sanitizef(coeff * encLinkedEnv2 + (1.0f - coeff) * encLinkedRms2);
    const float env = sqrtf(encLinkedEnv2 + 1.0e-12f);
    const float rawGainDb = -((linToDb(env) - encReferenceDb) * encStrength);
    const float gainDb = clampf(rawGainDb, -encMaxCutDb, encMaxBoostDb);
    diagLastGainDb = gainDb;
    diagLastEnvDb = linToDb(env);
    const float gainLin = dbToLin(gainDb);
    const float encL = xL * gainLin;
    const float encR = xR * gainLin;
    updatePeak(diagPkAfterDecodeGain, fmaxf(fabsf(encL), fabsf(encR)));
    const float tiltL = encTilt[0].process(encL);
    const float tiltR = encTilt[1].process(encR);
    updatePeak(diagPkAfterDeEmphasis, fmaxf(fabsf(tiltL), fabsf(tiltR)));
    outL = finalizeOutput(tiltL, encOutputGain, encHeadroomGain, encSoftClipDrive, softStartGain);
    outR = finalizeOutput(tiltR, encOutputGain, encHeadroomGain, encSoftClipDrive, softStartGain);
  }

  inline void processStereo(float inL, float inR, float &outL, float &outR) {
    updatePeak(diagPkInRaw, fmaxf(fabsf(inL), fabsf(inR)));
    float softStartGain = 1.0f;
    if (outputSoftStartEnabled && outputSoftStartSamplesRemaining > 0) {
      const float t = 1.0f - ((float)outputSoftStartSamplesRemaining / (float)kOutputSoftStartSamples);
      softStartGain = clampf(t * t, 0.0f, 1.0f);
      --outputSoftStartSamplesRemaining;
    }
    const float absInL = fabsf(inL);
    const float absInR = fabsf(inR);
    if (absInL > diagInputPeakL) diagInputPeakL = absInL;
    if (absInR > diagInputPeakR) diagInputPeakR = absInR;
    diagInputSumSqL += inL * inL;
    diagInputSumSqR += inR * inR;
    diagInputSumL += inL;
    diagInputSumR += inR;
    diagInputDiffAbsSum += fabsf(inL - inR);
    diagInputCrossSum += inL * inR;
    ++diagSampleCount;

    const float modeInputGain = (mode == MODE_ENCODE) ? encInputGain : inputGain;
    float xL = (mode == MODE_ENCODE ? encDcBlock[0].process(sanitizef(inL) * modeInputGain) : dcBlock[0].process(sanitizef(inL) * modeInputGain));
    float xR = (mode == MODE_ENCODE ? encDcBlock[1].process(sanitizef(inR) * modeInputGain) : dcBlock[1].process(sanitizef(inR) * modeInputGain));
    updatePeak(diagPkAfterInputTrim, fmaxf(fabsf(xL), fabsf(xR)));
    if (fabsf(xL) > 0.98f) {
      inputClipFlag = true;
      ++inputClipCount;
    }
    if (fabsf(xR) > 0.98f) {
      inputClipFlag = true;
      ++inputClipCount;
    }

    if (bypass) {
      // Bypass keeps output protection (headroom + soft clip), so it is not a hard transparent relay.
      outL = finalizeOutput(xL, outputGain, headroomGain, softClipDrive, softStartGain);
      outR = finalizeOutput(xR, outputGain, headroomGain, softClipDrive, softStartGain);
      ++diagBypassSampleCount;
      const float absOutL = fabsf(outL);
      const float absOutR = fabsf(outR);
      if (absOutL > diagOutputPeakL) diagOutputPeakL = absOutL;
      if (absOutR > diagOutputPeakR) diagOutputPeakR = absOutR;
      diagOutputSumSqL += outL * outL;
      diagOutputSumSqR += outR * outR;
      diagOutputSumL += outL;
      diagOutputSumR += outR;
      diagOutputDiffAbsSum += fabsf(outL - outR);
      diagOutputCrossSum += outL * outR;
      return;
    }

    if (mode == MODE_ENCODE) processEncode(xL, xR, outL, outR, softStartGain);
    else processDecode(xL, xR, outL, outR, softStartGain);
    const float absOutL = fabsf(outL);
    const float absOutR = fabsf(outR);
    if (absOutL > diagOutputPeakL) diagOutputPeakL = absOutL;
    if (absOutR > diagOutputPeakR) diagOutputPeakR = absOutR;
    diagOutputSumSqL += outL * outL;
    diagOutputSumSqR += outR * outR;
    diagOutputSumL += outL;
    diagOutputSumR += outR;
    diagOutputDiffAbsSum += fabsf(outL - outR);
    diagOutputCrossSum += outL * outR;
  }
};

void AudioEffectOCXType2CodecStereo::resetSignalDiagnostics() {
  noInterrupts();
  diagInputPeakL = 0.0f;
  diagInputPeakR = 0.0f;
  diagOutputPeakL = 0.0f;
  diagOutputPeakR = 0.0f;
  diagInputSumSqL = 0.0f;
  diagInputSumSqR = 0.0f;
  diagOutputSumSqL = 0.0f;
  diagOutputSumSqR = 0.0f;
  diagInputSumL = 0.0f;
  diagInputSumR = 0.0f;
  diagOutputSumL = 0.0f;
  diagOutputSumR = 0.0f;
  diagSampleCount = 0;
  diagLastGainDb = 0.0f;
  diagMinGainDb = 0.0f;
  diagMaxGainDb = 0.0f;
  diagGainDbSum = 0.0f;
  diagGainCount = 0;
  diagLastEnvDb = -120.0f;
  diagDecoderActiveCount = 0;
  diagBypassSampleCount = 0;
  diagDecodedSampleCount = 0;
  diagClampCutHitCount = 0;
  diagClampBoostHitCount = 0;
  diagNearCutCount = 0;
  diagNearBoostCount = 0;
  diagInputDiffAbsSum = 0.0f;
  diagOutputDiffAbsSum = 0.0f;
  diagInputCrossSum = 0.0f;
  diagOutputCrossSum = 0.0f;
  diagLowProxySum = 0.0f;
  diagHighProxySum = 0.0f;
  diagPkInRaw = 0.0f;
  diagPkAfterInputTrim = 0.0f;
  diagPkAfterDecodeGain = 0.0f;
  diagPkAfterDeEmphasis = 0.0f;
  diagPkPreGuard = 0.0f;
  diagPkPostGuard = 0.0f;
  diagPkPreSoftClip = 0.0f;
  diagPkPostSoftClip = 0.0f;
  diagPkPostOutputTrim = 0.0f;
  interrupts();
}

ClipDelta AudioEffectOCXType2CodecStereo::consumeClipDelta() {
  ClipDelta delta;
  noInterrupts();
  delta.inputNew = inputClipCount - lastReportedInputClipCount;
  delta.outputNew = outputClipCount - lastReportedOutputClipCount;
  lastReportedInputClipCount = inputClipCount;
  lastReportedOutputClipCount = outputClipCount;
  interrupts();
  return delta;
}

AudioEffectOCXType2CodecStereo::DiagSnapshot AudioEffectOCXType2CodecStereo::getSignalDiagnosticsSnapshot() const {
  DiagSnapshot snap;
  noInterrupts();
  snap.inputPeakL = diagInputPeakL;
  snap.inputPeakR = diagInputPeakR;
  snap.outputPeakL = diagOutputPeakL;
  snap.outputPeakR = diagOutputPeakR;
  snap.pkInRaw = diagPkInRaw;
  snap.pkAfterInputTrim = diagPkAfterInputTrim;
  snap.pkAfterDecodeGain = diagPkAfterDecodeGain;
  snap.pkAfterDeEmphasis = diagPkAfterDeEmphasis;
  snap.pkPreGuard = diagPkPreGuard;
  snap.pkPostGuard = diagPkPostGuard;
  snap.pkPreSoftClip = diagPkPreSoftClip;
  snap.pkPostSoftClip = diagPkPostSoftClip;
  snap.pkPostOutputTrim = diagPkPostOutputTrim;
  snap.inputSumSqL = diagInputSumSqL;
  snap.inputSumSqR = diagInputSumSqR;
  snap.outputSumSqL = diagOutputSumSqL;
  snap.outputSumSqR = diagOutputSumSqR;
  snap.inputMeanL = (diagSampleCount > 0) ? (diagInputSumL / (float)diagSampleCount) : 0.0f;
  snap.inputMeanR = (diagSampleCount > 0) ? (diagInputSumR / (float)diagSampleCount) : 0.0f;
  snap.outputMeanL = (diagSampleCount > 0) ? (diagOutputSumL / (float)diagSampleCount) : 0.0f;
  snap.outputMeanR = (diagSampleCount > 0) ? (diagOutputSumR / (float)diagSampleCount) : 0.0f;
  snap.sampleCount = diagSampleCount;
  snap.lastGainDb = diagLastGainDb;
  snap.minGainDb = diagMinGainDb;
  snap.maxGainDb = diagMaxGainDb;
  snap.avgGainDb = (diagGainCount > 0) ? (diagGainDbSum / (float)diagGainCount) : 0.0f;
  snap.lastEnvDb = diagLastEnvDb;
  snap.gainSampleCount = diagGainCount;
  snap.decoderActiveCount = diagDecoderActiveCount;
  snap.bypassSampleCount = diagBypassSampleCount;
  snap.decodedSampleCount = diagDecodedSampleCount;
  snap.clampCutHitCount = diagClampCutHitCount;
  snap.clampBoostHitCount = diagClampBoostHitCount;
  snap.nearCutCount = diagNearCutCount;
  snap.nearBoostCount = diagNearBoostCount;
  snap.inputAbsDiffMean = (diagSampleCount > 0) ? (diagInputDiffAbsSum / (float)diagSampleCount) : 0.0f;
  snap.outputAbsDiffMean = (diagSampleCount > 0) ? (diagOutputDiffAbsSum / (float)diagSampleCount) : 0.0f;
  snap.inputCrossMean = (diagSampleCount > 0) ? (diagInputCrossSum / (float)diagSampleCount) : 0.0f;
  snap.outputCrossMean = (diagSampleCount > 0) ? (diagOutputCrossSum / (float)diagSampleCount) : 0.0f;
  snap.lowProxyMean = (diagDecodedSampleCount > 0) ? (diagLowProxySum / (float)diagDecodedSampleCount) : 0.0f;
  snap.highProxyMean = (diagDecodedSampleCount > 0) ? (diagHighProxySum / (float)diagDecodedSampleCount) : 0.0f;
  interrupts();
  return snap;
}

void AudioEffectOCXType2CodecStereo::update(void) {
  audio_block_t *inL = receiveReadOnly(0);
  audio_block_t *inR = receiveReadOnly(1);
  if (!inL || !inR) {
    if (inL) release(inL);
    if (inR) release(inR);
    return;
  }

  audio_block_t *outL = allocate();
  audio_block_t *outR = allocate();
  if (!outL || !outR) {
    ++allocFailCount;
    if (outL) release(outL);
    if (outR) release(outR);
    release(inL);
    release(inR);
    return;
  }

  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
    const float xl = (float)inL->data[i] / 32768.0f;
    const float xr = (float)inR->data[i] / 32768.0f;
    float yl = 0.0f;
    float yr = 0.0f;
    processStereo(xl, xr, yl, yr);

    int32_t sl = (int32_t)(yl * 32767.0f);
    int32_t sr = (int32_t)(yr * 32767.0f);
    sl = (sl > 32767) ? 32767 : ((sl < -32768) ? -32768 : sl);
    sr = (sr > 32767) ? 32767 : ((sr < -32768) ? -32768 : sr);
    outL->data[i] = (int16_t)sl;
    outR->data[i] = (int16_t)sr;
  }

  transmit(outL, 0);
  transmit(outR, 1);
  release(outL);
  release(outR);
  release(inL);
  release(inR);
}

AudioInputI2S                    i2sIn;
AudioEffectOCXType2CodecStereo  ocx;
AudioSynthWaveformSine           calTone;
AudioAnalyzeToneDetect           toneDetectLLo;
AudioAnalyzeToneDetect           toneDetectLCenter;
AudioAnalyzeToneDetect           toneDetectLHi;
AudioAnalyzeToneDetect           toneDetectRLo;
AudioAnalyzeToneDetect           toneDetectRCenter;
AudioAnalyzeToneDetect           toneDetectRHi;
AudioAnalyzePeak                 peakL;
AudioAnalyzePeak                 peakR;
AudioMixer4                      mixL;
AudioMixer4                      mixR;
AudioOutputI2S                   i2sOut;
AudioControlSGTL5000             codec;

AudioConnection patchCord1(i2sIn, 0, ocx, 0);
AudioConnection patchCord2(i2sIn, 1, ocx, 1);
AudioConnection patchCord3(ocx, 0, mixL, 0);
AudioConnection patchCord4(ocx, 1, mixR, 0);
AudioConnection patchCord5(calTone, 0, mixL, 1);
AudioConnection patchCord6(calTone, 0, mixR, 1);
AudioConnection patchCord7(mixL, 0, i2sOut, 0);
AudioConnection patchCord8(mixR, 0, i2sOut, 1);
AudioConnection patchCord9(i2sIn, 0, toneDetectLLo, 0);
AudioConnection patchCord10(i2sIn, 0, toneDetectLCenter, 0);
AudioConnection patchCord11(i2sIn, 0, toneDetectLHi, 0);
AudioConnection patchCord12(i2sIn, 1, toneDetectRLo, 0);
AudioConnection patchCord13(i2sIn, 1, toneDetectRCenter, 0);
AudioConnection patchCord14(i2sIn, 1, toneDetectRHi, 0);
AudioConnection patchCord15(i2sIn, 0, peakL, 0);
AudioConnection patchCord16(i2sIn, 1, peakR, 0);

bool  calToneEnabled = false;
float calToneDb = OCXProfile::kToneDb;
float calToneHz = OCXProfile::kToneHz;
enum ToneChannelMode : uint8_t { TONE_BOTH = 0, TONE_LEFT = 1, TONE_RIGHT = 2 };
ToneChannelMode toneChannelMode = TONE_BOTH;
unsigned long lastStatusMs = 0;
uint32_t lastWarnedInputClipCount = 0;
uint32_t lastWarnedOutputClipCount = 0;
uint32_t lastWarnedAllocFailCount = 0;
unsigned long autoStateEnterMs = 0;
unsigned long lastAutoMeasureMs = 0;
PresetId currentPreset = PRESET_UNIVERSAL;
DecoderOperatingMode decoderOperatingMode = DECODER_STRICT_COMPATIBLE;
PlaybackSafetyMode playbackSafetyMode = SAFETY_PLAYBACK_ADAPTIVE;
AutoCalState autoCalState = AUTO_IDLE;
bool autoCalValid = false;
float autoCalReferenceDb = OCXProfile::kReferenceDb;
float autoCalOutputTrimDb = OCXProfile::kOutputTrimDb;
float autoCalHeadroomDb = OCXProfile::kHeadroomDb;
float staticReferenceDb = OCXProfile::kReferenceDb;
float staticOutputTrimDb = OCXProfile::kOutputTrimDb;
float staticHeadroomDb = OCXProfile::kHeadroomDb;
bool guardEnabled = true;
float guardTrimOffsetDb = 0.0f;             // 0 .. -3 dB
float guardHeadroomOffsetDb = 0.0f;         // 0 .. +2.5 dB
float guardBoostCapReductionDb = 0.0f;      // 0 .. -3 dB
float guardTrimOffsetPersistedDb = 0.0f;
float guardHeadroomOffsetPersistedDb = 0.0f;
float guardBoostCapReductionPersistedDb = 0.0f;
bool guardDirty = false;
unsigned long guardLastStateChangeMs = 0;
unsigned long guardLastPersistMs = 0;
unsigned long guardLastTickMs = 0;
unsigned long guardLastBrakeMs = 0;
unsigned long guardStateAccumMs[5] = {0, 0, 0, 0, 0};
uint8_t guardWindowPos = 0;
uint8_t guardWindowCount = 0;
uint16_t guardWindowClip1s = 0;
uint16_t guardWindowNearLimit10s = 0;
uint16_t guardWindowBoostClamp10s = 0;
GuardState guardState = GUARD_IDLE;
const char* guardReason = "boot";
const char* guardTriggerSource = "none";
const char* guardRelaxBlockedBy = "none";
float guardTriggerValue = 0.0f;
float guardTriggerThreshold = 0.0f;
unsigned long guardLastTriggerMs = 0;
bool guardCanRelax = false;
bool diagModeEnabled = false;
unsigned long diagLastPrintMs = 0;
static constexpr unsigned long kDiagIntervalMs = 3000UL;
static constexpr float kGuardMinTrimOffsetDb = -3.0f;
static constexpr float kGuardMaxTrimOffsetDb = 0.0f;
static constexpr float kGuardMinHeadroomOffsetDb = 0.0f;
static constexpr float kGuardMaxHeadroomOffsetDb = 2.5f;
static constexpr float kGuardMinBoostReductionDb = -3.0f;
static constexpr float kGuardMaxBoostReductionDb = 0.0f;
static constexpr uint8_t kGuardWindowSeconds = 60;
uint16_t guardClipBins[kGuardWindowSeconds] = {0};
uint16_t guardNearBins[kGuardWindowSeconds] = {0};
uint16_t guardBoostBins[kGuardWindowSeconds] = {0};
float autoCalScore = 0.0f;
uint16_t autoBlocksSeen = 0;
uint16_t autoBlocksValid = 0;
float autoPeakAccum = 0.0f;
float autoRmsAccum = 0.0f;
float autoToneAccum = 0.0f;
float autoLastPeak = 0.0f;
float autoToneMetricL = 0.0f;
float autoToneMetricR = 0.0f;
float autoPeakL = 0.0f;
float autoPeakR = 0.0f;
float autoRmsProxyL = 0.0f;
float autoRmsProxyR = 0.0f;
float autoLastPeakL = 0.0f;
float autoLastPeakR = 0.0f;
float autoLastToneMetric = 0.0f;
uint16_t autoToneBlocks = 0;
uint8_t autoToneSegments = 0;
uint16_t autoCurrentSegmentBlocks = 0;
uint16_t autoCurrentSegmentValidBlocks = 0;
uint16_t autoSilenceBlocks = 0;
uint16_t autoConsecutiveToneReady = 0;
unsigned long autoLastPeriodicMs = 0;
bool autoFreshToneL = false;
bool autoFreshToneR = false;
bool autoFreshPeakL = false;
bool autoFreshPeakR = false;
bool autoFreshWindowToneL = false;
bool autoFreshWindowToneR = false;
bool autoFreshWindowPeakL = false;
bool autoFreshWindowPeakR = false;
bool autoFreshWindowOk = false;
bool autoLrCheckActive = false;
float autoLatchedToneL = 0.0f;
float autoLatchedToneR = 0.0f;
float autoLatchedPeakL = 0.0f;
float autoLatchedPeakR = 0.0f;
uint16_t autoToneAgeMsL = 65535;
uint16_t autoToneAgeMsR = 65535;
uint16_t autoPeakAgeMsL = 65535;
uint16_t autoPeakAgeMsR = 65535;
unsigned long autoLastToneFreshMsL = 0;
unsigned long autoLastToneFreshMsR = 0;
unsigned long autoLastPeakFreshMsL = 0;
unsigned long autoLastPeakFreshMsR = 0;
bool autoLevelOk = false;
bool autoTonalOk = false;
bool autoStabilityOk = false;
bool autoLrOk = false;
const char* autoGateBlockReason = "none";
const char* autoRejectReason = "none";
bool autoLockSummaryPending = false;
static constexpr unsigned long kAutoFreshWindowMs = 450UL;
static constexpr unsigned long kAutoWarmupMs = 1200UL;
static constexpr uint8_t kExpectedSegments = 3;
static constexpr uint32_t kSegmentTargetMs = 60000UL;
static constexpr uint32_t kPauseTargetMs = 3000UL;
static constexpr uint16_t kBlockPeriodMs = 150;
static constexpr uint16_t kMinValidBlocksPerSegment = 240;
static constexpr uint16_t kWarmupSkipBlocks = 6;
static constexpr uint16_t kPauseDetectBlocks = 12;

struct CalProfile {
  float referenceDb = OCXProfile::kReferenceDb;
  float outputTrimDb = OCXProfile::kOutputTrimDb;
  float headroomDb = OCXProfile::kHeadroomDb;
  float guardTrimStartDb = 0.0f;
  float guardHeadroomStartDb = 0.0f;
};

struct MeasurementMeta {
  uint8_t segmentsDetected = 0;
  uint8_t segmentsValid = 0;
  uint16_t segmentDurationSec[kExpectedSegments] = {0, 0, 0};
  float segmentPeak[kExpectedSegments] = {0.0f, 0.0f, 0.0f};
  float segmentTone[kExpectedSegments] = {0.0f, 0.0f, 0.0f};
  float segmentSpread[kExpectedSegments] = {0.0f, 0.0f, 0.0f};
  float confidence = 0.0f;
  uint32_t timestampMs = 0;
  uint32_t persistVersion = 0;
};

struct ProfileStore {
  CalProfile singleProfile{};
  CalProfile lw1Profile{};
  CalProfile lw2Profile{};
  CalProfile commonProfile{};
  MeasurementMeta singleMeta{};
  MeasurementMeta lw1Meta{};
  MeasurementMeta lw2Meta{};
  MeasurementMeta commonMeta{};
  uint8_t singleValid = 0;
  uint8_t lw1Valid = 0;
  uint8_t lw2Valid = 0;
  uint8_t commonValid = 0;
};

struct SegmentAccumulator {
  uint16_t blocksSeen = 0;
  uint16_t validBlocks = 0;
  float peakSum = 0.0f;
  float peakSqSum = 0.0f;
  float toneSum = 0.0f;
  const char* rejectReason = "none";
};

DeckType deckType = DECK_SINGLE_LW;
TransportId activeTransport = TRANSPORT_LW1;
TransportId wizardExpectedTransport = TRANSPORT_LW1;
ProfileSelect selectedProfile = PROFILE_SINGLE;
ProfileStore profileStore{};
SegmentAccumulator segAccum{};
uint8_t wizardPass = 0;
uint8_t wizardDetectedSegments = 0;
uint8_t wizardValidSegments = 0;
uint8_t wizardCurrentSegment = 0;
bool wizardInTone = false;
uint16_t wizardPauseBlocks = 0;
uint16_t autoSegDurationBlocks[kExpectedSegments] = {0, 0, 0};
float autoSegPeakAvg[kExpectedSegments] = {0.0f, 0.0f, 0.0f};
float autoSegToneAvg[kExpectedSegments] = {0.0f, 0.0f, 0.0f};
float autoSegPeakSpread[kExpectedSegments] = {0.0f, 0.0f, 0.0f};

struct PersistSettings {
  uint32_t magic;
  uint16_t version;
  uint8_t mode;
  uint8_t presetId;
  uint8_t deckType;
  uint8_t activeTransport;
  uint8_t selectedProfile;
  uint8_t decoderOperatingMode;
  uint8_t playbackSafetyMode;
  uint8_t autoCalValid;
  uint8_t guardEnabled;
  float autoCalReferenceDb;
  float autoCalOutputTrimDb;
  float autoCalHeadroomDb;
  float guardTrimOffsetDb;
  float guardHeadroomOffsetDb;
  float guardBoostCapReductionDb;
  uint32_t guardLastPersistMs;
  ProfileStore profileStore;
  uint32_t checksum;
};
static constexpr uint32_t kSettingsMagic = 0x4F435831u;
static constexpr uint16_t kSettingsVersion = 6;
static constexpr int kSettingsAddr = 0;

uint32_t settingsChecksum(const uint8_t *p, size_t n) {
  uint32_t x = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    x ^= p[i];
    x *= 16777619u;
  }
  return x;
}

const char* presetLabel(uint8_t id) {
  switch ((PresetId)id) {
    case PRESET_AUTO_CAL: return "AUTO_CAL";
    case PRESET_UNIVERSAL:
    default: return "UNIVERSAL";
  }
}

const char* decoderModeLabel(uint8_t m) {
  switch ((DecoderOperatingMode)m) {
    case DECODER_RESTORATION: return "RESTORATION";
    case DECODER_CONTROLLED_RECORD: return "CONTROLLED_RECORD";
    case DECODER_STRICT_COMPATIBLE:
    default: return "STRICT_COMPATIBLE";
  }
}

const char* safetyModeLabel(uint8_t m) {
  switch ((PlaybackSafetyMode)m) {
    case SAFETY_STRICT_REFERENCE: return "STRICT_REFERENCE";
    case SAFETY_PLAYBACK_ADAPTIVE: return "PLAYBACK_ADAPTIVE";
    case SAFETY_STRICT_SAFE:
    default: return "STRICT_SAFE";
  }
}

const char* guardStateLabel(uint8_t s) {
  switch ((GuardState)s) {
    case GUARD_BRAKE_A: return "BRAKE_A";
    case GUARD_PROTECT_B: return "PROTECT_B";
    case GUARD_RELAX_C: return "RELAX_C";
    case GUARD_SETTLED: return "SETTLED";
    case GUARD_IDLE:
    default: return "IDLE";
  }
}

const char* deckTypeLabel(uint8_t t) {
  return (t == DECK_DUAL_LW) ? "DUAL_LW" : "SINGLE_LW";
}

const char* transportLabel(uint8_t t) {
  return (t == TRANSPORT_LW2) ? "LW2" : "LW1";
}

const char* profileSelectLabel(uint8_t p) {
  switch ((ProfileSelect)p) {
    case PROFILE_LW1: return "LW1_PROFILE";
    case PROFILE_LW2: return "LW2_PROFILE";
    case PROFILE_COMMON: return "COMMON_PROFILE";
    case PROFILE_SINGLE:
    default: return "SINGLE_PROFILE";
  }
}

CalProfile* mutableProfileForSelection(ProfileSelect p) {
  if (p == PROFILE_COMMON) return &profileStore.commonProfile;
  if (deckType == DECK_SINGLE_LW || p == PROFILE_SINGLE) return &profileStore.singleProfile;
  return (p == PROFILE_LW2) ? &profileStore.lw2Profile : &profileStore.lw1Profile;
}

bool profileSelectionValid(ProfileSelect p) {
  if (p == PROFILE_COMMON) return profileStore.commonValid != 0;
  if (deckType == DECK_SINGLE_LW || p == PROFILE_SINGLE) return profileStore.singleValid != 0;
  return (p == PROFILE_LW2) ? (profileStore.lw2Valid != 0) : (profileStore.lw1Valid != 0);
}

void applyDecoderOperatingModeTuning() {
  if (decoderOperatingMode == DECODER_RESTORATION) {
    ocx.setAutoTrimEnabled(OCXProfile::kRestorationAutoTrimEnabled);
    ocx.setAutoTrimMaxDb(OCXProfile::kRestorationAutoTrimMaxDb);
    ocx.setDropoutHoldMs(OCXProfile::kRestorationDropoutHoldMs);
    ocx.setDropoutHfDropDb(OCXProfile::kRestorationDropoutHfDropDb);
    ocx.setDropoutLevelDropDb(OCXProfile::kRestorationDropoutLevelDropDb);
    ocx.setSaturationSoftfail(OCXProfile::kRestorationSaturationSoftfail);
    ocx.setSaturationThreshold(OCXProfile::kRestorationSaturationThreshold);
    ocx.setSaturationKneeDb(OCXProfile::kRestorationSaturationKneeDb);
  } else {
    ocx.setAutoTrimEnabled(OCXProfile::kAutoTrimEnabled);
    ocx.setAutoTrimMaxDb(OCXProfile::kAutoTrimMaxDb);
    ocx.setDropoutHoldMs(OCXProfile::kDropoutHoldMs);
    ocx.setDropoutHfDropDb(OCXProfile::kDropoutHfDropDb);
    ocx.setDropoutLevelDropDb(OCXProfile::kDropoutLevelDropDb);
    ocx.setSaturationSoftfail(OCXProfile::kSaturationSoftfail);
    ocx.setSaturationThreshold(OCXProfile::kSaturationThreshold);
    ocx.setSaturationKneeDb(OCXProfile::kSaturationKneeDb);
  }
}

void applyEffectiveDecoderSettings() {
  const bool referenceSafety = playbackSafetyMode == SAFETY_STRICT_REFERENCE;
  const bool adaptiveSafety = playbackSafetyMode == SAFETY_PLAYBACK_ADAPTIVE;
  const float guardTrim = referenceSafety ? 0.0f : guardTrimOffsetDb;
  const float guardHead = referenceSafety ? 0.0f : guardHeadroomOffsetDb;
  const float guardBoost = referenceSafety ? 0.0f : guardBoostCapReductionDb;
  const float effectiveTrim = clampf(staticOutputTrimDb + guardTrim, -18.0f, 18.0f);
  const float effectiveHeadroom = clampf(staticHeadroomDb + guardHead + (adaptiveSafety ? 0.5f : 0.0f), 0.0f, 6.0f);
  const float effectiveMaxBoost = clampf(OCXProfile::kMaxBoostDb + guardBoost - (adaptiveSafety ? 0.5f : 0.0f), 0.0f, 24.0f);
  ocx.setReferenceDb(staticReferenceDb);
  ocx.setOutputTrimDb(effectiveTrim);
  ocx.setHeadroomDb(effectiveHeadroom);
  ocx.setMaxBoostDb(effectiveMaxBoost);
  ocx.setDecoderOperatingMode(decoderOperatingMode);
  ocx.setOutputSoftStartEnabled(!referenceSafety);
  if (referenceSafety) {
    ocx.setAutoTrimEnabled(false);
    ocx.setAutoTrimMaxDb(0.0f);
    ocx.setSaturationSoftfail(false);
    ocx.setSaturationKneeDb(0.0f);
    ocx.setSoftClipDrive(1.0f);
    ocx.setDropoutHoldMs(0.0f);
  } else {
    applyDecoderOperatingModeTuning();
  }
}

const char* autoCalStateLabel(uint8_t s) {
  switch ((AutoCalState)s) {
    case AUTO_WAIT_FOR_TONE: return "AUTO_WAIT_FOR_TONE";
    case AUTO_MEASURE: return "AUTO_MEASURE";
    case AUTO_COMPUTE: return "AUTO_COMPUTE";
    case AUTO_LOCKED: return "AUTO_LOCKED";
    case AUTO_FAILED: return "AUTO_FAILED";
    case AUTO_IDLE:
    default: return "AUTO_IDLE";
  }
}

void applyDecoderPreset(uint8_t id) {
  ocx.setInputTrimDb(OCXProfile::kInputTrimDb);
  ocx.setStrength(OCXProfile::kStrength);
  ocx.setMaxCutDb(OCXProfile::kMaxCutDb);
  ocx.setAttackMs(OCXProfile::kAttackMs);
  ocx.setReleaseMs(OCXProfile::kReleaseMs);
  ocx.setSidechainHpHz(OCXProfile::kSidechainHpHz);
  ocx.setSidechainLpHz(OCXProfile::kSidechainLpHz);
  ocx.setSidechainShelfHz(OCXProfile::kSidechainShelfHz);
  ocx.setSidechainShelfDb(OCXProfile::kSidechainShelfDb);
  ocx.setDetectorRmsMs(OCXProfile::kDetectorRmsMs);
  ocx.setDeemphHz(OCXProfile::kDeemphHz);
  ocx.setDeemphDb(OCXProfile::kDeemphDb);
  ocx.setSoftClipDrive(OCXProfile::kSoftClipDrive);
  ocx.setDcBlockHz(OCXProfile::kDcBlockHz);
  staticReferenceDb = OCXProfile::kReferenceDb;
  staticOutputTrimDb = OCXProfile::kOutputTrimDb;
  staticHeadroomDb = OCXProfile::kHeadroomDb;
  if ((PresetId)id == PRESET_AUTO_CAL && autoCalValid) {
    if (profileSelectionValid(selectedProfile)) {
      CalProfile* p = mutableProfileForSelection(selectedProfile);
      staticReferenceDb = p->referenceDb;
      staticOutputTrimDb = p->outputTrimDb;
      staticHeadroomDb = p->headroomDb;
      guardTrimOffsetDb = clampf(p->guardTrimStartDb, kGuardMinTrimOffsetDb, kGuardMaxTrimOffsetDb);
      guardHeadroomOffsetDb = clampf(p->guardHeadroomStartDb, kGuardMinHeadroomOffsetDb, kGuardMaxHeadroomOffsetDb);
    } else {
      staticReferenceDb = autoCalReferenceDb;
      staticOutputTrimDb = autoCalOutputTrimDb;
      staticHeadroomDb = autoCalHeadroomDb;
    }
  }
  applyEffectiveDecoderSettings();
  currentPreset = (PresetId)id;
}

void persistSettings() {
  PersistSettings s{};
  s.magic = kSettingsMagic;
  s.version = kSettingsVersion;
  s.mode = static_cast<uint8_t>(ocx.getMode());
  s.presetId = static_cast<uint8_t>(currentPreset);
  s.deckType = static_cast<uint8_t>(deckType);
  s.activeTransport = static_cast<uint8_t>(activeTransport);
  s.selectedProfile = static_cast<uint8_t>(selectedProfile);
  s.decoderOperatingMode = static_cast<uint8_t>(decoderOperatingMode);
  s.playbackSafetyMode = static_cast<uint8_t>(playbackSafetyMode);
  s.autoCalValid = autoCalValid ? 1 : 0;
  s.guardEnabled = guardEnabled ? 1 : 0;
  s.autoCalReferenceDb = autoCalReferenceDb;
  s.autoCalOutputTrimDb = autoCalOutputTrimDb;
  s.autoCalHeadroomDb = autoCalHeadroomDb;
  s.guardTrimOffsetDb = guardTrimOffsetDb;
  s.guardHeadroomOffsetDb = guardHeadroomOffsetDb;
  s.guardBoostCapReductionDb = guardBoostCapReductionDb;
  s.guardLastPersistMs = millis();
  s.profileStore = profileStore;
  s.checksum = settingsChecksum(reinterpret_cast<const uint8_t *>(&s), sizeof(PersistSettings) - sizeof(uint32_t));
  EEPROM.put(kSettingsAddr, s);
  guardTrimOffsetPersistedDb = guardTrimOffsetDb;
  guardHeadroomOffsetPersistedDb = guardHeadroomOffsetDb;
  guardBoostCapReductionPersistedDb = guardBoostCapReductionDb;
  guardDirty = false;
  guardLastPersistMs = s.guardLastPersistMs;
}

void loadSettingsOrFactory() {
  PersistSettings s{};
  EEPROM.get(kSettingsAddr, s);
  if (s.magic == kSettingsMagic && s.version == 3) {
    factoryResetSettings();
    return;
  }
  const bool ok = s.magic == kSettingsMagic &&
                  s.version == kSettingsVersion &&
                  s.checksum == settingsChecksum(reinterpret_cast<const uint8_t *>(&s), sizeof(PersistSettings) - sizeof(uint32_t)) &&
                  s.mode <= 1 &&
                  s.presetId <= PRESET_AUTO_CAL &&
                  s.deckType <= DECK_DUAL_LW &&
                  s.activeTransport <= TRANSPORT_LW2 &&
                  s.selectedProfile <= PROFILE_COMMON &&
                  s.decoderOperatingMode <= DECODER_CONTROLLED_RECORD &&
                  s.playbackSafetyMode <= SAFETY_PLAYBACK_ADAPTIVE;
  if (ok) {
    ocx.setMode(static_cast<AudioEffectOCXType2CodecStereo::Mode>(s.mode));
    currentPreset = static_cast<PresetId>(s.presetId);
    deckType = static_cast<DeckType>(s.deckType);
    activeTransport = static_cast<TransportId>(s.activeTransport);
    selectedProfile = static_cast<ProfileSelect>(s.selectedProfile);
    decoderOperatingMode = static_cast<DecoderOperatingMode>(s.decoderOperatingMode);
    playbackSafetyMode = static_cast<PlaybackSafetyMode>(s.playbackSafetyMode);
    autoCalValid = s.autoCalValid != 0;
    autoCalReferenceDb = clampf(s.autoCalReferenceDb, -30.0f, -10.0f);
    autoCalOutputTrimDb = clampf(s.autoCalOutputTrimDb, -6.0f, 0.0f);
    autoCalHeadroomDb = clampf(s.autoCalHeadroomDb, 0.5f, 4.0f);
    guardEnabled = s.guardEnabled != 0;
    guardTrimOffsetDb = clampf(s.guardTrimOffsetDb, kGuardMinTrimOffsetDb, kGuardMaxTrimOffsetDb);
    guardHeadroomOffsetDb = clampf(s.guardHeadroomOffsetDb, kGuardMinHeadroomOffsetDb, kGuardMaxHeadroomOffsetDb);
    guardBoostCapReductionDb = clampf(s.guardBoostCapReductionDb, kGuardMinBoostReductionDb, kGuardMaxBoostReductionDb);
    guardTrimOffsetPersistedDb = guardTrimOffsetDb;
    guardHeadroomOffsetPersistedDb = guardHeadroomOffsetDb;
    guardBoostCapReductionPersistedDb = guardBoostCapReductionDb;
    guardLastPersistMs = s.guardLastPersistMs;
    profileStore = s.profileStore;
    guardDirty = false;
    applyDecoderPreset(currentPreset);
  } else {
    ocx.setMode(AudioEffectOCXType2CodecStereo::MODE_DECODE);
    currentPreset = PRESET_UNIVERSAL;
    deckType = DECK_SINGLE_LW;
    activeTransport = TRANSPORT_LW1;
    selectedProfile = PROFILE_SINGLE;
    decoderOperatingMode = DECODER_STRICT_COMPATIBLE;
    playbackSafetyMode = SAFETY_PLAYBACK_ADAPTIVE;
    profileStore = ProfileStore{};
    autoCalValid = false;
    guardEnabled = true;
    guardTrimOffsetDb = 0.0f;
    guardHeadroomOffsetDb = 0.0f;
    guardBoostCapReductionDb = 0.0f;
    guardTrimOffsetPersistedDb = 0.0f;
    guardHeadroomOffsetPersistedDb = 0.0f;
    guardBoostCapReductionPersistedDb = 0.0f;
    guardDirty = false;
    guardLastPersistMs = 0;
    applyDecoderPreset(PRESET_UNIVERSAL);
  }
}

void factoryResetSettings() {
  PersistSettings s{};
  s.magic = kSettingsMagic;
  s.version = kSettingsVersion;
  s.mode = static_cast<uint8_t>(AudioEffectOCXType2CodecStereo::MODE_DECODE);
  s.presetId = static_cast<uint8_t>(PRESET_UNIVERSAL);
  s.deckType = static_cast<uint8_t>(DECK_SINGLE_LW);
  s.activeTransport = static_cast<uint8_t>(TRANSPORT_LW1);
  s.selectedProfile = static_cast<uint8_t>(PROFILE_SINGLE);
  s.decoderOperatingMode = static_cast<uint8_t>(DECODER_STRICT_COMPATIBLE);
  s.playbackSafetyMode = static_cast<uint8_t>(SAFETY_PLAYBACK_ADAPTIVE);
  s.autoCalValid = 0;
  s.guardEnabled = 1;
  s.autoCalReferenceDb = OCXProfile::kReferenceDb;
  s.autoCalOutputTrimDb = OCXProfile::kOutputTrimDb;
  s.autoCalHeadroomDb = OCXProfile::kHeadroomDb;
  s.guardTrimOffsetDb = 0.0f;
  s.guardHeadroomOffsetDb = 0.0f;
  s.guardBoostCapReductionDb = 0.0f;
  s.guardLastPersistMs = 0;
  s.profileStore = ProfileStore{};
  s.checksum = settingsChecksum(reinterpret_cast<const uint8_t *>(&s), sizeof(PersistSettings) - sizeof(uint32_t));
  EEPROM.put(kSettingsAddr, s);
  ocx.setMode(AudioEffectOCXType2CodecStereo::MODE_DECODE);
  autoCalValid = false;
  autoCalState = AUTO_IDLE;
  currentPreset = PRESET_UNIVERSAL;
  deckType = DECK_SINGLE_LW;
  activeTransport = TRANSPORT_LW1;
  selectedProfile = PROFILE_SINGLE;
  decoderOperatingMode = DECODER_STRICT_COMPATIBLE;
  playbackSafetyMode = SAFETY_PLAYBACK_ADAPTIVE;
  profileStore = ProfileStore{};
  guardEnabled = true;
  guardTrimOffsetDb = 0.0f;
  guardHeadroomOffsetDb = 0.0f;
  guardBoostCapReductionDb = 0.0f;
  guardTrimOffsetPersistedDb = 0.0f;
  guardHeadroomOffsetPersistedDb = 0.0f;
  guardBoostCapReductionPersistedDb = 0.0f;
  guardDirty = false;
  guardLastPersistMs = 0;
  applyDecoderPreset(PRESET_UNIVERSAL);
}

// Keep this helper Arduino-.ino preprocessor safe: use uint8_t in signature to avoid enum prototype ordering issues.
const char* toneChannelModeLabel(uint8_t mode) {
  switch ((ToneChannelMode)mode) {
    case TONE_LEFT: return "LEFT";
    case TONE_RIGHT: return "RIGHT";
    case TONE_BOTH:
    default: return "BOTH";
  }
}

void updateTone() {
  calToneDb = clampf(calToneDb, -60.0f, -1.0f);
  calTone.frequency(calToneHz);
  calTone.amplitude(calToneEnabled ? clampf(dbToLin(calToneDb), 0.0f, 0.89f) : 0.0f);
  const float toneGainL = (calToneEnabled && toneChannelMode != TONE_RIGHT) ? 1.0f : 0.0f;
  const float toneGainR = (calToneEnabled && toneChannelMode != TONE_LEFT) ? 1.0f : 0.0f;
  mixL.gain(1, toneGainL);
  mixR.gain(1, toneGainR);
}

void applyFactoryPreset() {
  ocx.setBypass(false);
  applyDecoderPreset(PRESET_UNIVERSAL);
  ocx.resetState();
  ocx.resetSignalDiagnostics();
}

void beginAutoCal() {
  wizardPass = 0;
  if (deckType == DECK_SINGLE_LW) wizardExpectedTransport = TRANSPORT_LW1;
  Serial.print(F("[WIZARD] deck_type=")); Serial.print(deckTypeLabel(deckType));
  Serial.print(F(" expected_transport=")); Serial.println(transportLabel(wizardExpectedTransport));
  if (deckType == DECK_DUAL_LW) {
    Serial.print(F("[WIZARD] Dual-LW Messung fuer "));
    Serial.print(transportLabel(wizardExpectedTransport));
    Serial.println(F(": bitte dieses Laufwerk am Deck waehlen und 1-kHz Messton starten (3x60s, ~3s Pause)."));
  } else {
    Serial.println(F("[WIZARD] Step 1/1: Single-LW Messung starten (1-kHz 3x60s, ~3s Pause)."));
  }
  autoCalState = AUTO_WAIT_FOR_TONE;
  autoStateEnterMs = millis();
  lastAutoMeasureMs = 0;
  autoLastPeriodicMs = 0;
  autoBlocksSeen = 0;
  autoBlocksValid = 0;
  autoToneBlocks = 0;
  autoToneSegments = 0;
  autoCurrentSegmentBlocks = 0;
  autoCurrentSegmentValidBlocks = 0;
  autoSilenceBlocks = 0;
  autoConsecutiveToneReady = 0;
  autoPeakAccum = 0.0f;
  autoRmsAccum = 0.0f;
  autoToneAccum = 0.0f;
  autoLastPeak = 0.0f;
  autoLastPeakL = 0.0f;
  autoLastPeakR = 0.0f;
  autoLastToneMetric = 0.0f;
  autoToneMetricL = 0.0f;
  autoToneMetricR = 0.0f;
  autoPeakL = 0.0f;
  autoPeakR = 0.0f;
  autoRmsProxyL = 0.0f;
  autoRmsProxyR = 0.0f;
  autoFreshToneL = false;
  autoFreshToneR = false;
  autoFreshPeakL = false;
  autoFreshPeakR = false;
  autoFreshWindowToneL = false;
  autoFreshWindowToneR = false;
  autoFreshWindowPeakL = false;
  autoFreshWindowPeakR = false;
  autoFreshWindowOk = false;
  autoLrCheckActive = false;
  autoLatchedToneL = 0.0f;
  autoLatchedToneR = 0.0f;
  autoLatchedPeakL = 0.0f;
  autoLatchedPeakR = 0.0f;
  autoToneAgeMsL = 65535;
  autoToneAgeMsR = 65535;
  autoPeakAgeMsL = 65535;
  autoPeakAgeMsR = 65535;
  autoLastToneFreshMsL = 0;
  autoLastToneFreshMsR = 0;
  autoLastPeakFreshMsL = 0;
  autoLastPeakFreshMsR = 0;
  autoLevelOk = false;
  autoTonalOk = false;
  autoStabilityOk = false;
  autoLrOk = false;
  autoGateBlockReason = "none";
  autoRejectReason = "none";
  autoCalScore = 0.0f;
  autoLockSummaryPending = false;
  wizardDetectedSegments = 0;
  wizardValidSegments = 0;
  wizardCurrentSegment = 0;
  wizardInTone = false;
  wizardPauseBlocks = 0;
  segAccum = SegmentAccumulator{};
  for (uint8_t i = 0; i < kExpectedSegments; ++i) {
    autoSegDurationBlocks[i] = 0;
    autoSegPeakAvg[i] = 0.0f;
    autoSegToneAvg[i] = 0.0f;
    autoSegPeakSpread[i] = 0.0f;
  }
}

void computeAutoCalResult() {
  const uint8_t validSegs = wizardValidSegments;
  if (validSegs < 2) {
    autoCalState = AUTO_FAILED;
    setAutoRejectReason(validSegs == 1 ? "reject_only_one_segment" : "reject_not_enough_segments");
    return;
  }
  const float peakAvg = autoPeakAccum / fmaxf((float)autoBlocksValid, 1.0f);
  const float rmsAvg = autoRmsAccum / fmaxf((float)autoBlocksValid, 1.0f);
  const float toneAvg = autoToneAccum / fmaxf((float)autoBlocksValid, 1.0f);
  const float lrMismatch = fabsf(autoPeakL - autoPeakR) / fmaxf(fmaxf(autoPeakL, autoPeakR), 1.0e-6f);
  const float stabilityPenalty = fabsf((peakAvg - autoLastPeak)) + fabsf(toneAvg - autoLastToneMetric);
  const float predClipUniversal = fmaxf(0.0f, peakAvg * dbToLin(OCXProfile::kOutputTrimDb) * dbToLin(-OCXProfile::kHeadroomDb) - 0.98f);
  const float nearLimitUniversal = fmaxf(0.0f, peakAvg - 0.86f);
  const float scoreUniversal =
    predClipUniversal * 8.0f +
    nearLimitUniversal * 3.5f +
    fmaxf(0.0f, peakAvg - 0.62f) * 4.0f +
    fmaxf(0.0f, rmsAvg - 0.32f) * 3.0f +
    fabsf(rmsAvg - 0.30f) * 1.8f +
    fabsf(peakAvg - 0.52f) * 1.5f +
    lrMismatch * 1.5f +
    stabilityPenalty * 1.1f;
  const float baseReference = OCXProfile::kReferenceDb;
  const float baseOutputTrim = OCXProfile::kOutputTrimDb;
  const float baseHeadroom = OCXProfile::kHeadroomDb;
  autoCalReferenceDb = clampf(baseReference + clampf((0.28f - rmsAvg) * 8.0f, -2.0f, 2.0f), baseReference - 2.0f, baseReference + 2.0f);
  autoCalOutputTrimDb = clampf(baseOutputTrim + clampf((0.43f - peakAvg) * 2.3f, -0.8f, 0.25f), -2.8f, 0.0f);
  autoCalHeadroomDb = clampf(baseHeadroom + clampf((peakAvg - 0.52f) * 1.8f, -0.15f, 1.0f), 1.0f, 3.8f);
  autoCalScore = 100.0f - 100.0f * (scoreUniversal + fmaxf(0.0f, 0.70f - toneAvg));
  if (validSegs == 2) autoCalScore -= 7.0f;
  autoCalValid = true;

  CalProfile p{};
  p.referenceDb = autoCalReferenceDb;
  p.outputTrimDb = autoCalOutputTrimDb;
  p.headroomDb = autoCalHeadroomDb;
  p.guardTrimStartDb = clampf((peakAvg - 0.55f) * -0.8f, -0.5f, 0.0f);
  p.guardHeadroomStartDb = clampf((peakAvg - 0.55f) * 0.8f, 0.0f, 0.6f);
  MeasurementMeta meta{};
  meta.segmentsDetected = wizardDetectedSegments;
  meta.segmentsValid = wizardValidSegments;
  meta.confidence = clampf((float)validSegs / 3.0f - fmaxf(0.0f, 0.20f - toneAvg), 0.0f, 1.0f);
  meta.timestampMs = millis();
  meta.persistVersion = kSettingsVersion;
  for (uint8_t i = 0; i < kExpectedSegments; ++i) {
    meta.segmentDurationSec[i] = (uint16_t)((uint32_t)autoSegDurationBlocks[i] * kBlockPeriodMs / 1000UL);
    meta.segmentPeak[i] = autoSegPeakAvg[i];
    meta.segmentTone[i] = autoSegToneAvg[i];
    meta.segmentSpread[i] = autoSegPeakSpread[i];
  }

  if (deckType == DECK_SINGLE_LW) {
    profileStore.singleProfile = p;
    profileStore.singleMeta = meta;
    profileStore.singleValid = 1;
    selectedProfile = PROFILE_SINGLE;
  } else {
    if (wizardExpectedTransport == TRANSPORT_LW1) {
      profileStore.lw1Profile = p;
      profileStore.lw1Meta = meta;
      profileStore.lw1Valid = 1;
      selectedProfile = PROFILE_LW1;
    } else {
      profileStore.lw2Profile = p;
      profileStore.lw2Meta = meta;
      profileStore.lw2Valid = 1;
      selectedProfile = PROFILE_LW2;
    }
    if (profileStore.lw1Valid && profileStore.lw2Valid) {
      profileStore.commonProfile.referenceDb = 0.5f * (profileStore.lw1Profile.referenceDb + profileStore.lw2Profile.referenceDb);
      profileStore.commonProfile.outputTrimDb = 0.5f * (profileStore.lw1Profile.outputTrimDb + profileStore.lw2Profile.outputTrimDb);
      profileStore.commonProfile.headroomDb = fmaxf(profileStore.lw1Profile.headroomDb, profileStore.lw2Profile.headroomDb);
      profileStore.commonProfile.guardTrimStartDb = fminf(profileStore.lw1Profile.guardTrimStartDb, profileStore.lw2Profile.guardTrimStartDb);
      profileStore.commonProfile.guardHeadroomStartDb = fmaxf(profileStore.lw1Profile.guardHeadroomStartDb, profileStore.lw2Profile.guardHeadroomStartDb);
      profileStore.commonMeta.confidence = fminf(profileStore.lw1Meta.confidence, profileStore.lw2Meta.confidence) * 0.9f;
      profileStore.commonValid = 1;
      selectedProfile = PROFILE_COMMON;
    }
  }

  currentPreset = PRESET_AUTO_CAL;
  applyDecoderPreset(PRESET_AUTO_CAL);
  persistSettings();
  autoCalState = AUTO_LOCKED;
  autoStateEnterMs = millis();
  autoRejectReason = "none";
  autoLockSummaryPending = true;
}

float combinedToneMetricFromBins(float lo, float mid, float hi) {
  return clampf(fmaxf(mid, 0.65f * fmaxf(lo, hi) + 0.35f * mid), 0.0f, 1.5f);
}

void captureAutoCalInputs(unsigned long now) {
  float loL = autoToneMetricL;
  float midL = autoToneMetricL;
  float hiL = autoToneMetricL;
  float loR = autoToneMetricR;
  float midR = autoToneMetricR;
  float hiR = autoToneMetricR;

  autoFreshToneL = false;
  autoFreshToneR = false;
  autoFreshPeakL = false;
  autoFreshPeakR = false;

  if (toneDetectLLo.available()) { loL = toneDetectLLo.read(); autoFreshToneL = true; }
  if (toneDetectLCenter.available()) { midL = toneDetectLCenter.read(); autoFreshToneL = true; }
  if (toneDetectLHi.available()) { hiL = toneDetectLHi.read(); autoFreshToneL = true; }
  if (toneDetectRLo.available()) { loR = toneDetectRLo.read(); autoFreshToneR = true; }
  if (toneDetectRCenter.available()) { midR = toneDetectRCenter.read(); autoFreshToneR = true; }
  if (toneDetectRHi.available()) { hiR = toneDetectRHi.read(); autoFreshToneR = true; }
  if (peakL.available()) { autoPeakL = peakL.read(); autoFreshPeakL = true; }
  if (peakR.available()) { autoPeakR = peakR.read(); autoFreshPeakR = true; }

  autoToneMetricL = combinedToneMetricFromBins(loL, midL, hiL);
  autoToneMetricR = combinedToneMetricFromBins(loR, midR, hiR);
  if (autoFreshToneL) autoLastToneFreshMsL = now;
  if (autoFreshToneR) autoLastToneFreshMsR = now;
  if (autoFreshPeakL) autoLastPeakFreshMsL = now;
  if (autoFreshPeakR) autoLastPeakFreshMsR = now;
  autoToneAgeMsL = (autoLastToneFreshMsL > 0) ? (uint16_t)fminf((float)(now - autoLastToneFreshMsL), 65535.0f) : 65535;
  autoToneAgeMsR = (autoLastToneFreshMsR > 0) ? (uint16_t)fminf((float)(now - autoLastToneFreshMsR), 65535.0f) : 65535;
  autoPeakAgeMsL = (autoLastPeakFreshMsL > 0) ? (uint16_t)fminf((float)(now - autoLastPeakFreshMsL), 65535.0f) : 65535;
  autoPeakAgeMsR = (autoLastPeakFreshMsR > 0) ? (uint16_t)fminf((float)(now - autoLastPeakFreshMsR), 65535.0f) : 65535;
  autoFreshWindowToneL = autoToneAgeMsL <= kAutoFreshWindowMs;
  autoFreshWindowToneR = autoToneAgeMsR <= kAutoFreshWindowMs;
  autoFreshWindowPeakL = autoPeakAgeMsL <= kAutoFreshWindowMs;
  autoFreshWindowPeakR = autoPeakAgeMsR <= kAutoFreshWindowMs;
  autoFreshWindowOk = autoFreshWindowToneL && autoFreshWindowToneR && autoFreshWindowPeakL && autoFreshWindowPeakR;
  autoLatchedToneL = autoToneMetricL;
  autoLatchedToneR = autoToneMetricR;
  autoLatchedPeakL = autoPeakL;
  autoLatchedPeakR = autoPeakR;
  autoRmsProxyL = autoPeakL * 0.707f;
  autoRmsProxyR = autoPeakR * 0.707f;
}

void updateAutoCalGateFlags() {
  const float toneMetric = fmaxf(autoToneMetricL, autoToneMetricR);
  const float peakMono = fmaxf(autoPeakL, autoPeakR);
  const float lrPeakMismatch = fabsf(autoPeakL - autoPeakR) / fmaxf(peakMono, 1.0e-6f);
  const float lrToneMismatch = fabsf(autoToneMetricL - autoToneMetricR) / fmaxf(toneMetric, 1.0e-6f);
  autoLevelOk = peakMono > 0.030f && peakMono < 0.95f;
  autoTonalOk = toneMetric > 0.46f;
  autoStabilityOk = fabsf(autoPeakL - autoLastPeakL) < 0.12f && fabsf(autoPeakR - autoLastPeakR) < 0.12f;
  autoLrCheckActive = peakMono >= 0.06f;
  autoLrOk = (!autoLrCheckActive) || (lrPeakMismatch < 0.55f && lrToneMismatch < 0.70f);
  autoLastPeakL = autoPeakL;
  autoLastPeakR = autoPeakR;
  autoLastPeak = peakMono;
  autoLastToneMetric = toneMetric;
}

void setAutoRejectReason(const char* reason) {
  autoRejectReason = reason;
}

void printAutoCalRawTelemetry() {
  Serial.print(F("[AUTO_RAW] state=")); Serial.print(autoCalStateLabel(autoCalState));
  Serial.print(F(" preset=")); Serial.print(presetLabel(currentPreset));
  Serial.print(F(" autoCalValid=")); Serial.print(autoCalValid ? F("YES") : F("NO"));
  Serial.print(F(" toneL=")); Serial.print(autoToneMetricL, 3);
  Serial.print(F(" toneR=")); Serial.print(autoToneMetricR, 3);
  Serial.print(F(" peakL=")); Serial.print(autoPeakL, 4);
  Serial.print(F(" peakR=")); Serial.print(autoPeakR, 4);
  Serial.print(F(" rmsL=")); Serial.print(autoRmsProxyL, 4);
  Serial.print(F(" rmsR=")); Serial.print(autoRmsProxyR, 4);
  Serial.print(F(" levelOk=")); Serial.print(autoLevelOk ? F("YES") : F("NO"));
  Serial.print(F(" tonalOk=")); Serial.print(autoTonalOk ? F("YES") : F("NO"));
  Serial.print(F(" stabilityOk=")); Serial.print(autoStabilityOk ? F("YES") : F("NO"));
  Serial.print(F(" lrOk=")); Serial.print(autoLrOk ? F("YES") : F("NO"));
  Serial.print(F(" freshToneL=")); Serial.print(autoFreshToneL ? F("YES") : F("NO"));
  Serial.print(F(" freshToneR=")); Serial.print(autoFreshToneR ? F("YES") : F("NO"));
  Serial.print(F(" freshPeakL=")); Serial.print(autoFreshPeakL ? F("YES") : F("NO"));
  Serial.print(F(" freshPeakR=")); Serial.print(autoFreshPeakR ? F("YES") : F("NO"));
  Serial.print(F(" freshWinToneL=")); Serial.print(autoFreshWindowToneL ? F("YES") : F("NO"));
  Serial.print(F(" freshWinToneR=")); Serial.print(autoFreshWindowToneR ? F("YES") : F("NO"));
  Serial.print(F(" freshWinPeakL=")); Serial.print(autoFreshWindowPeakL ? F("YES") : F("NO"));
  Serial.print(F(" freshWinPeakR=")); Serial.print(autoFreshWindowPeakR ? F("YES") : F("NO"));
  Serial.print(F(" toneAgeL_ms=")); Serial.print(autoToneAgeMsL);
  Serial.print(F(" toneAgeR_ms=")); Serial.print(autoToneAgeMsR);
  Serial.print(F(" peakAgeL_ms=")); Serial.print(autoPeakAgeMsL);
  Serial.print(F(" peakAgeR_ms=")); Serial.print(autoPeakAgeMsR);
  Serial.print(F(" latchedToneL=")); Serial.print(autoLatchedToneL, 3);
  Serial.print(F(" latchedToneR=")); Serial.print(autoLatchedToneR, 3);
  Serial.print(F(" latchedPeakL=")); Serial.print(autoLatchedPeakL, 4);
  Serial.print(F(" latchedPeakR=")); Serial.print(autoLatchedPeakR, 4);
  Serial.print(F(" lrCheck=")); Serial.print(autoLrCheckActive ? F("ACTIVE") : F("LOW_LEVEL_BYPASS"));
  Serial.print(F(" blocksSeen=")); Serial.print(autoBlocksSeen);
  Serial.print(F(" blocksValid=")); Serial.print(autoBlocksValid);
  Serial.print(F(" toneBlocks=")); Serial.print(autoToneBlocks);
  Serial.print(F(" toneSeg=")); Serial.print(autoToneSegments);
  Serial.print(F(" segDetected=")); Serial.print(wizardDetectedSegments);
  Serial.print(F(" segValid=")); Serial.print(wizardValidSegments);
  Serial.print(F(" expectedTransport=")); Serial.print(transportLabel(wizardExpectedTransport));
  Serial.print(F(" goodWin=")); Serial.print(autoConsecutiveToneReady);
  Serial.print(F(" gateBlock=")); Serial.print(autoGateBlockReason);
  Serial.print(F(" outClipCount=")); Serial.print(ocx.getOutputClipCount());
  Serial.print(F(" stateMs=")); Serial.print(millis() - autoStateEnterMs);
  Serial.print(F(" reject=")); Serial.println(autoRejectReason);
}

void maybePrintAutoCalPeriodic(unsigned long now) {
  if (autoCalState != AUTO_WAIT_FOR_TONE && autoCalState != AUTO_MEASURE) return;
  if (now - autoLastPeriodicMs < 1500UL) return;
  autoLastPeriodicMs = now;
  printAutoCalRawTelemetry();
}

void updateAutoCal() {
  if (autoCalState == AUTO_IDLE || autoCalState == AUTO_LOCKED || autoCalState == AUTO_FAILED) return;
  const unsigned long now = millis();
  captureAutoCalInputs(now);
  updateAutoCalGateFlags();
  maybePrintAutoCalPeriodic(now);
  const bool warmupDone = (now - autoStateEnterMs) >= kAutoWarmupMs;
  const bool freshOk = autoFreshWindowOk;
  const char* gateBlock = "none";
  if (!warmupDone) gateBlock = "reject_wait_warmup";
  else if (!freshOk) gateBlock = "reject_no_fresh_data";
  else if (!autoLevelOk && autoLastPeak <= 0.030f) gateBlock = "reject_peak_too_low";
  else if (!autoLevelOk && autoLastPeak >= 0.95f) gateBlock = "reject_peak_too_high";
  else if (!autoTonalOk) gateBlock = "reject_tone_too_weak";
  else if (!autoStabilityOk) gateBlock = "reject_unstable";
  else if (!autoLrOk) gateBlock = "reject_lr_mismatch";
  const bool gateOk = strcmp(gateBlock, "none") == 0;
  autoGateBlockReason = gateBlock;
  setAutoRejectReason(gateBlock);

  if (autoCalState == AUTO_WAIT_FOR_TONE) {
    if (gateOk) {
      ++autoConsecutiveToneReady;
    } else {
      autoConsecutiveToneReady = 0;
    }
    if (autoConsecutiveToneReady >= 5) {
      autoCalState = AUTO_MEASURE;
      autoStateEnterMs = now;
      lastAutoMeasureMs = now;
      autoRejectReason = "none";
      autoGateBlockReason = "none";
    } else if (now - autoStateEnterMs > 75000UL) {
      autoCalState = AUTO_FAILED;
      setAutoRejectReason("reject_tone_too_weak");
    }
    return;
  }

  if (autoCalState == AUTO_MEASURE) {
    if (now - lastAutoMeasureMs >= kBlockPeriodMs) {
      lastAutoMeasureMs = now;
      ++autoBlocksSeen;
      if (gateOk) {
        ++autoBlocksValid;
        ++autoToneBlocks;
        ++autoCurrentSegmentValidBlocks;
        ++autoCurrentSegmentBlocks;
        ++segAccum.blocksSeen;
        ++segAccum.validBlocks;
        autoSilenceBlocks = 0;
        wizardPauseBlocks = 0;
        wizardInTone = true;
        const float segPeak = 0.5f * (autoPeakL + autoPeakR);
        autoPeakAccum += segPeak;
        autoRmsAccum += 0.5f * (autoRmsProxyL + autoRmsProxyR);
        const float segTone = fmaxf(autoToneMetricL, autoToneMetricR);
        autoToneAccum += segTone;
        segAccum.peakSum += segPeak;
        segAccum.peakSqSum += segPeak * segPeak;
        segAccum.toneSum += segTone;
      } else {
        ++autoSilenceBlocks;
        ++wizardPauseBlocks;
        if (autoCurrentSegmentBlocks > 0 && autoSilenceBlocks >= kPauseDetectBlocks) {
          const uint8_t segIdx = wizardDetectedSegments;
          ++wizardDetectedSegments;
          if (segIdx < kExpectedSegments) {
            autoSegDurationBlocks[segIdx] = autoCurrentSegmentBlocks;
            if (segAccum.validBlocks > 0) {
              const float invN = 1.0f / (float)segAccum.validBlocks;
              const float peakMean = segAccum.peakSum * invN;
              const float peakVar = fmaxf(0.0f, segAccum.peakSqSum * invN - peakMean * peakMean);
              autoSegPeakAvg[segIdx] = peakMean;
              autoSegToneAvg[segIdx] = segAccum.toneSum * invN;
              autoSegPeakSpread[segIdx] = sqrtf(peakVar);
            }
          }
          if (autoCurrentSegmentValidBlocks >= kMinValidBlocksPerSegment) {
            ++autoToneSegments;
            ++wizardValidSegments;
          }
          autoCurrentSegmentBlocks = 0;
          autoCurrentSegmentValidBlocks = 0;
          wizardInTone = false;
          segAccum = SegmentAccumulator{};
        }
      }
    }
    const uint16_t targetBlocks = (uint16_t)(kSegmentTargetMs / kBlockPeriodMs);
    if (wizardInTone && wizardDetectedSegments < kExpectedSegments && autoCurrentSegmentBlocks >= targetBlocks) {
      const uint8_t segIdx = wizardDetectedSegments;
      ++wizardDetectedSegments;
      if (segIdx < kExpectedSegments) {
        autoSegDurationBlocks[segIdx] = autoCurrentSegmentBlocks;
        if (segAccum.validBlocks > 0) {
          const float invN = 1.0f / (float)segAccum.validBlocks;
          const float peakMean = segAccum.peakSum * invN;
          const float peakVar = fmaxf(0.0f, segAccum.peakSqSum * invN - peakMean * peakMean);
          autoSegPeakAvg[segIdx] = peakMean;
          autoSegToneAvg[segIdx] = segAccum.toneSum * invN;
          autoSegPeakSpread[segIdx] = sqrtf(peakVar);
        }
      }
      if (autoCurrentSegmentValidBlocks >= kMinValidBlocksPerSegment) {
        ++autoToneSegments;
        ++wizardValidSegments;
      }
      autoCurrentSegmentBlocks = 0;
      autoCurrentSegmentValidBlocks = 0;
      wizardInTone = false;
      segAccum = SegmentAccumulator{};
    }
    const bool enoughSegments = (wizardValidSegments >= 3) || (wizardDetectedSegments >= 3 && wizardValidSegments >= 2);
    const bool enoughDuration = (now - autoStateEnterMs) >= (kSegmentTargetMs * 3UL + kPauseTargetMs * 2UL);
    if (enoughSegments && autoToneBlocks >= 320 && autoBlocksSeen >= 360 && enoughDuration) {
      autoCalState = AUTO_COMPUTE;
      autoRejectReason = "none";
    } else if (now - autoStateEnterMs > 230000UL) {
      autoCalState = AUTO_FAILED;
      setAutoRejectReason("reject_unstable");
    }
  }

  if (autoCalState == AUTO_COMPUTE) computeAutoCalResult();
}

void guardMarkChanged(unsigned long now, uint8_t s, const char* reason) {
  guardState = static_cast<GuardState>(s);
  guardReason = reason;
  guardLastStateChangeMs = now;
  guardDirty = true;
  applyEffectiveDecoderSettings();
}

void maybePersistGuard(unsigned long now) {
  if (!guardDirty) return;
  if (now - guardLastStateChangeMs < 90000UL) return;
  if (now - guardLastPersistMs < 180000UL) return;
  const float dTrim = fabsf(guardTrimOffsetDb - guardTrimOffsetPersistedDb);
  const float dHead = fabsf(guardHeadroomOffsetDb - guardHeadroomOffsetPersistedDb);
  const float dBoost = fabsf(guardBoostCapReductionDb - guardBoostCapReductionPersistedDb);
  if (fmaxf(dTrim, fmaxf(dHead, dBoost)) < 0.5f) return;
  persistSettings();
}

void updatePlaybackGuard() {
  const unsigned long now = millis();
  if (guardLastTickMs == 0) guardLastTickMs = now;
  if (now - guardLastTickMs < 1000UL) {
    maybePersistGuard(now);
    return;
  }
  const unsigned long dtMs = now - guardLastTickMs;
  guardStateAccumMs[(uint8_t)guardState] += dtMs;
  guardLastTickMs = now;
  const AudioEffectOCXType2CodecStereo::DiagSnapshot d = ocx.getSignalDiagnosticsSnapshot();
  static uint32_t prevNearBoost = 0;
  static uint32_t prevClampBoost = 0;
  static uint32_t prevOutClip = 0;
  const uint32_t outClipNow = ocx.getOutputClipCount();
  const uint16_t outClipNew = (uint16_t)(outClipNow - prevOutClip);
  prevOutClip = outClipNow;
  const uint16_t nearBoostNew = (uint16_t)(d.nearBoostCount - prevNearBoost);
  const uint16_t clampBoostNew = (uint16_t)(d.clampBoostHitCount - prevClampBoost);
  prevNearBoost = d.nearBoostCount;
  prevClampBoost = d.clampBoostHitCount;
  const float outPeakMono = fmaxf(d.outputPeakL, d.outputPeakR);

  guardClipBins[guardWindowPos] = outClipNew;
  guardNearBins[guardWindowPos] = nearBoostNew;
  guardBoostBins[guardWindowPos] = clampBoostNew;
  guardWindowPos = (guardWindowPos + 1) % kGuardWindowSeconds;
  if (guardWindowCount < kGuardWindowSeconds) ++guardWindowCount;

  guardWindowClip1s = outClipNew;
  guardWindowNearLimit10s = 0;
  guardWindowBoostClamp10s = 0;
  for (uint8_t i = 0; i < 10 && i < guardWindowCount; ++i) {
    const int idx = (int)guardWindowPos - 1 - i;
    const int pos = (idx < 0) ? idx + kGuardWindowSeconds : idx;
    guardWindowNearLimit10s += guardNearBins[pos];
    guardWindowBoostClamp10s += guardBoostBins[pos];
  }

  const bool referenceSafety = playbackSafetyMode == SAFETY_STRICT_REFERENCE;
  if (!guardEnabled || referenceSafety) {
    if (guardTrimOffsetDb != 0.0f || guardHeadroomOffsetDb != 0.0f || guardBoostCapReductionDb != 0.0f) {
      guardTrimOffsetDb = 0.0f;
      guardHeadroomOffsetDb = 0.0f;
      guardBoostCapReductionDb = 0.0f;
      guardMarkChanged(now, GUARD_IDLE, referenceSafety ? "reference_mode" : "disabled");
    }
    guardTriggerSource = referenceSafety ? "reference_mode_guard_off" : "guard_disabled";
    guardTriggerValue = 0.0f;
    guardTriggerThreshold = 0.0f;
    guardRelaxBlockedBy = "guard_off";
    guardCanRelax = true;
    maybePersistGuard(now);
    return;
  }

  const bool triggerA = outClipNew > 0 || outPeakMono >= 0.985f || guardWindowNearLimit10s >= 250;
  const bool triggerB = guardWindowNearLimit10s >= 700 || guardWindowBoostClamp10s >= 300;
  const bool stableForRelax = (now - guardLastBrakeMs) >= 45000UL && guardWindowClip1s == 0 && guardWindowNearLimit10s <= 70 && guardWindowBoostClamp10s <= 50;
  guardCanRelax = stableForRelax;
  guardRelaxBlockedBy = stableForRelax ? "none" : ((guardWindowClip1s > 0) ? "recent_clip" : ((guardWindowNearLimit10s > 70) ? "near_limit_window" : ((guardWindowBoostClamp10s > 50) ? "boost_clamp_window" : "min_hold_time")));

  if (triggerA) {
    guardTriggerSource = (outClipNew > 0) ? "recent_clip_history" : ((outPeakMono >= 0.985f) ? "post_output_peak" : "margin_shortfall");
    guardTriggerValue = (outClipNew > 0) ? (float)outClipNew : ((outPeakMono >= 0.985f) ? outPeakMono : (float)guardWindowNearLimit10s);
    guardTriggerThreshold = (outClipNew > 0) ? 1.0f : ((outPeakMono >= 0.985f) ? 0.985f : 250.0f);
    guardLastTriggerMs = now;
    guardTrimOffsetDb = clampf(guardTrimOffsetDb - 0.5f, kGuardMinTrimOffsetDb, kGuardMaxTrimOffsetDb);
    if (outClipNew > 0 || outPeakMono >= 0.995f) {
      guardHeadroomOffsetDb = clampf(guardHeadroomOffsetDb + 0.5f, kGuardMinHeadroomOffsetDb, kGuardMaxHeadroomOffsetDb);
    }
    guardLastBrakeMs = now;
    guardMarkChanged(now, GUARD_BRAKE_A, outClipNew > 0 ? "out_clip" : "near_limit");
  } else if (triggerB) {
    guardTriggerSource = (guardWindowBoostClamp10s >= 300) ? "boost_risk" : "near_limit_window";
    guardTriggerValue = (guardWindowBoostClamp10s >= 300) ? (float)guardWindowBoostClamp10s : (float)guardWindowNearLimit10s;
    guardTriggerThreshold = (guardWindowBoostClamp10s >= 300) ? 300.0f : 700.0f;
    guardLastTriggerMs = now;
    guardTrimOffsetDb = clampf(guardTrimOffsetDb - 0.5f, kGuardMinTrimOffsetDb, kGuardMaxTrimOffsetDb);
    guardHeadroomOffsetDb = clampf(guardHeadroomOffsetDb + 0.5f, kGuardMinHeadroomOffsetDb, kGuardMaxHeadroomOffsetDb);
    guardBoostCapReductionDb = clampf(guardBoostCapReductionDb - 0.5f, kGuardMinBoostReductionDb, kGuardMaxBoostReductionDb);
    guardMarkChanged(now, GUARD_PROTECT_B, "boost_near_limit");
  } else if (stableForRelax && (guardTrimOffsetDb < 0.0f || guardHeadroomOffsetDb > 0.0f || guardBoostCapReductionDb < 0.0f)) {
    guardTriggerSource = "stable_relax";
    guardTriggerValue = (float)(now - guardLastBrakeMs);
    guardTriggerThreshold = 45000.0f;
    guardTrimOffsetDb = clampf(guardTrimOffsetDb + 0.15f, kGuardMinTrimOffsetDb, kGuardMaxTrimOffsetDb);
    guardHeadroomOffsetDb = clampf(guardHeadroomOffsetDb - 0.15f, kGuardMinHeadroomOffsetDb, kGuardMaxHeadroomOffsetDb);
    guardBoostCapReductionDb = clampf(guardBoostCapReductionDb + 0.15f, kGuardMinBoostReductionDb, kGuardMaxBoostReductionDb);
    guardMarkChanged(now, GUARD_RELAX_C, "stable_relax");
  } else {
    if (guardTrimOffsetDb == 0.0f && guardHeadroomOffsetDb == 0.0f && guardBoostCapReductionDb == 0.0f) {
      guardState = GUARD_IDLE;
      guardReason = "neutral";
      guardTriggerSource = "none";
      guardTriggerValue = 0.0f;
      guardTriggerThreshold = 0.0f;
    } else {
      guardState = GUARD_SETTLED;
      guardReason = "holding";
    }
  }
  maybePersistGuard(now);
}

void printHelp() {
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  h  : help"));
  Serial.println(F("  p  : print status"));
  Serial.println(F("  m  : print compact telemetry status"));
  Serial.println(F("  T  : toggle periodic DIAG mode (3 s compact runtime diagnostics)"));
  Serial.println(F("  n  : print signal diagnostics snapshot (input/output/gain activity)"));
  Serial.println(F("  N  : reset signal diagnostics counters"));
  Serial.println(F("  x  : clear clip flags"));
  Serial.println(F("  v  : print new clip counts since last v/m/p call"));
  Serial.println(F("  X  : clear clip flags + runtime counters + signal diagnostics + usage maxima"));
  Serial.println(F("  B  : reset DSP state"));
  Serial.println(F("  b  : toggle bypass"));
  Serial.println(F("  M  : print codec mode"));
  Serial.println(F("  j  : print preset"));
  Serial.println(F("  6  : cycle safety mode STRICT_REFERENCE -> STRICT_SAFE -> PLAYBACK_ADAPTIVE"));
  Serial.println(F("  7  : print control diagnostics (guard trigger + peak/margin chain)"));
  Serial.println(F("  u  : select preset UNIVERSAL"));
  Serial.println(F("  U  : cycle decoder operating mode STRICT -> RESTORATION -> CONTROLLED_RECORD"));
  Serial.println(F("  l  : start AUTO_CAL (1-kHz measurement cassette only)"));
  Serial.println(F("  1/2: set deck type SINGLE_LW / DUAL_LW"));
  Serial.println(F("  [ ]: set active transport LW1 / LW2"));
  Serial.println(F("  {  : select dedicated profile (single or active transport)"));
  Serial.println(F("  }  : select common profile (dual-deck fallback)"));
  Serial.println(F("  |  : print stored profile summary"));
  Serial.println(F("  J  : print AUTO_CAL state"));
  Serial.println(F("  K  : print AUTO_CAL raw telemetry/reject reasons"));
  Serial.println(F("  L  : print locked AUTO_CAL values"));
  Serial.println(F("  H  : toggle PLAYBACK_GUARD_DYNAMIC"));
  Serial.println(F("  @  : hard reset guard state/window/history"));
  Serial.println(F("  >  : set mode decode"));
  Serial.println(F("  <  : set mode encode"));
  Serial.println(F("  P  : persist mode/preset/AUTO_CAL settings"));
  Serial.println(F("  !  : factory reset persisted settings"));
  Serial.println(F("  0  : reload factory preset"));
  Serial.println(F("  i/I: input trim -/+ 0.5 dB"));
  Serial.println(F("  o/O: output trim -/+ 0.5 dB"));
  Serial.println(F("  s/S: strength -/+ 0.05"));
  Serial.println(F("  f/F: reference dB -/+ 1 dB"));
  Serial.println(F("  a/A: attack -/+ 0.5 ms"));
  Serial.println(F("  r/R: release -/+ 5 ms"));
  Serial.println(F("  c/C: sidechain HP -/+ 10 Hz"));
  Serial.println(F("  q/Q: sidechain shelf gain -/+ 1 dB"));
  Serial.println(F("  w/W: sidechain shelf freq -/+ 100 Hz"));
  Serial.println(F("  e/E: de-emphasis gain -/+ 1 dB"));
  Serial.println(F("  d/D: de-emphasis freq -/+ 50 Hz"));
  Serial.println(F("  g/G: headroom -/+ 0.5 dB"));
  Serial.println(F("  y/Y: DC block -/+ 1 Hz"));
  Serial.println(F("  t  : toggle 400 Hz calibration tone"));
  Serial.println(F("  z/Z: tone level -/+ 1 dB"));
  Serial.println(F("  k  : cycle tone channel mode BOTH -> LEFT -> RIGHT"));
  Serial.println(F("  Detector: stereo-linked true-RMS sidechain (HP + LP + HF weighting)."));
  Serial.println(F("  Snapshot includes clamp-hit/near-limit stats for maxCut/maxBoost interpretation."));
  Serial.println(F("  NOTE: AUTO_CAL uses a dbx Type II encoded 1 kHz tape reference as static base."));
  Serial.println(F("  NOTE: 0/VU belongs to tape recording reference; playback level depends on deck/player output."));
  Serial.println(F("  NOTE: 400 Hz tone is post-decoder workflow/output calibration, not the AUTO_CAL tape tone."));
  Serial.println(F("  NOTE: calibration tone is mixed post-decoder into the output path."));
  Serial.println(F("  NOTE: tone channel mode is an output routing test (not a decoder-input test)."));
  Serial.println();
}

void printTelemetry() {
  const float cpuNow = AudioProcessorUsage();
  const float cpuMax = AudioProcessorUsageMax();
  const uint32_t memNow = AudioMemoryUsage();
  const uint32_t memMax = AudioMemoryUsageMax();
  const uint32_t memCfg = OCXProfile::kAudioMemoryBlocks;
  const bool cpuTight = cpuMax > 80.0f;
  const bool memTight = memMax + 4 >= memCfg;
  Serial.println();
  Serial.println(F("---- OCX TELEMETRY ----"));
  Serial.print(F("CPU usage now/max (%): ")); Serial.print(cpuNow, 2); Serial.print(F(" / ")); Serial.println(cpuMax, 2);
  Serial.print(F("AudioMemory now/max blocks: ")); Serial.print(memNow); Serial.print(F(" / ")); Serial.print(memMax); Serial.print(F(" of ")); Serial.println(memCfg);
  Serial.print(F("Alloc fail count: ")); Serial.println(ocx.getAllocFailCount());
  Serial.print(F("Input clip count: ")); Serial.println(ocx.getInputClipCount());
  Serial.print(F("Output clip count: ")); Serial.println(ocx.getOutputClipCount());
  Serial.print(F("CPU reserve: ")); Serial.println(cpuTight ? F("TIGHT") : F("OK"));
  Serial.print(F("AudioMemory reserve: ")); Serial.println(memTight ? F("TIGHT") : F("OK"));
  Serial.println(F("Interpretation: CPU and AudioMemory should remain OK in multi-minute runs; allocFail must stay 0."));
  Serial.println();
}

void printCompactTelemetryLine() {
  const float cpuNow = AudioProcessorUsage();
  const float cpuMax = AudioProcessorUsageMax();
  const uint32_t memNow = AudioMemoryUsage();
  const uint32_t memMax = AudioMemoryUsageMax();
  const uint32_t memCfg = OCXProfile::kAudioMemoryBlocks;
  const bool cpuTight = cpuMax > 80.0f;
  const bool memTight = memMax + 4 >= memCfg;
  Serial.print(F("[TLM] cpuNow=")); Serial.print(cpuNow, 1);
  Serial.print(F("% cpuMax=")); Serial.print(cpuMax, 1);
  Serial.print(F("% memNow=")); Serial.print(memNow);
  Serial.print(F(" memMax=")); Serial.print(memMax);
  Serial.print(F("/")); Serial.print(memCfg);
  Serial.print(F(" cpuRes=")); Serial.print(cpuTight ? F("TIGHT") : F("OK"));
  Serial.print(F(" memRes=")); Serial.print(memTight ? F("TIGHT") : F("OK"));
  Serial.print(F(" allocFail=")); Serial.print(ocx.getAllocFailCount());
  Serial.print(F(" inClip=")); Serial.print(ocx.getInputClipCount());
  Serial.print(F(" outClip=")); Serial.print(ocx.getOutputClipCount());
  const ClipDelta clipDelta = ocx.consumeClipDelta();
  Serial.print(F(" inClipNew=")); Serial.print(clipDelta.inputNew);
  Serial.print(F(" outClipNew=")); Serial.print(clipDelta.outputNew);
  Serial.print(F(" bypass=")); Serial.print(ocx.getBypass() ? F("ON") : F("OFF"));
  Serial.print(F(" preset=")); Serial.print(presetLabel(currentPreset));
  Serial.print(F(" decMode=")); Serial.print(decoderModeLabel(decoderOperatingMode));
  Serial.print(F(" safeMode=")); Serial.print(safetyModeLabel(playbackSafetyMode));
  Serial.print(F(" auto=")); Serial.print(autoCalStateLabel(autoCalState));
  Serial.print(F(" autoValid=")); Serial.print(autoCalValid ? F("YES") : F("NO"));
  Serial.print(F(" deck=")); Serial.print(deckTypeLabel(deckType));
  Serial.print(F(" tr=")); Serial.print(transportLabel(activeTransport));
  Serial.print(F(" prof=")); Serial.print(profileSelectLabel(selectedProfile));
  Serial.print(F(" guardEnabled=")); Serial.print(guardEnabled ? F("YES") : F("NO"));
  Serial.print(F(" guardState=")); Serial.print(guardStateLabel(guardState));
  Serial.print(F(" guardReason=")); Serial.print(guardReason);
  Serial.print(F(" guardTrigSrc=")); Serial.print(guardTriggerSource);
  Serial.print(F(" guardTrigVal=")); Serial.print(guardTriggerValue, 2);
  Serial.print(F(" guardTrigThr=")); Serial.print(guardTriggerThreshold, 2);
  Serial.print(F(" guardTrimOffsetDb=")); Serial.print(guardTrimOffsetDb, 2);
  Serial.print(F(" guardHeadroomOffsetDb=")); Serial.print(guardHeadroomOffsetDb, 2);
  Serial.print(F(" guardBoostCapReductionDb=")); Serial.print(guardBoostCapReductionDb, 2);
  Serial.print(F(" guardWindowClip1s=")); Serial.print(guardWindowClip1s);
  Serial.print(F(" guardWindowNearLimit10s=")); Serial.print(guardWindowNearLimit10s);
  Serial.print(F(" guardWindowBoostClamp10s=")); Serial.print(guardWindowBoostClamp10s);
  Serial.print(F(" nearLimitPct10s=")); Serial.print(clampf((float)guardWindowNearLimit10s / 1280.0f * 100.0f, 0.0f, 100.0f), 1);
  Serial.print(F(" clampBoostPct10s=")); Serial.print(clampf((float)guardWindowBoostClamp10s / 1280.0f * 100.0f, 0.0f, 100.0f), 1);
  Serial.print(F(" protectStateMs=")); Serial.print(guardStateAccumMs[GUARD_BRAKE_A] + guardStateAccumMs[GUARD_PROTECT_B]);
  Serial.print(F(" guardStableMs=")); Serial.print(millis() - guardLastStateChangeMs);
  Serial.print(F(" guardDirty=")); Serial.print(guardDirty ? F("YES") : F("NO"));
  Serial.print(F(" guardLastPersistMs=")); Serial.print(guardLastPersistMs);
  Serial.print(F(" gDb=")); Serial.print(ocx.getLastGainDb(), 2);
  Serial.print(F(" envDb=")); Serial.print(ocx.getLastEnvDb(), 1);
  Serial.print(F(" tone=")); Serial.print(calToneEnabled ? F("ON") : F("OFF"));
  Serial.print(F("/")); Serial.println(toneChannelModeLabel(toneChannelMode));
}

void printSignalDiagnosticsSnapshot() {
  const AudioEffectOCXType2CodecStereo::DiagSnapshot d = ocx.getSignalDiagnosticsSnapshot();
  Serial.println();
  Serial.println(F("---- OCX SIGNAL DIAGNOSTICS ----"));
  Serial.print(F("Snapshot bypass now: ")); Serial.println(ocx.getBypass() ? F("ON") : F("OFF"));
  if (d.sampleCount == 0) {
    Serial.println(F("No samples captured yet. Play signal first, then run snapshot again."));
    Serial.println();
    return;
  }

  const float n = (float)d.sampleCount;
  const float inRmsL = sqrtf(d.inputSumSqL / n);
  const float inRmsR = sqrtf(d.inputSumSqR / n);
  const float outRmsL = sqrtf(d.outputSumSqL / n);
  const float outRmsR = sqrtf(d.outputSumSqR / n);
  const float inRmsMono = sqrtf(0.5f * (inRmsL * inRmsL + inRmsR * inRmsR));
  const float outRmsMono = sqrtf(0.5f * (outRmsL * outRmsL + outRmsR * outRmsR));
  const float inPeakMono = fmaxf(d.inputPeakL, d.inputPeakR);
  const float outPeakMono = fmaxf(d.outputPeakL, d.outputPeakR);
  const float deltaRmsDb = linToDb(outRmsMono) - linToDb(inRmsMono);
  const float deltaPeakDb = linToDb(outPeakMono) - linToDb(inPeakMono);
  const float inBalanceDb = linToDb(inRmsL) - linToDb(inRmsR);
  const float outBalanceDb = linToDb(outRmsL) - linToDb(outRmsR);
  const float inPeakBalanceDb = linToDb(d.inputPeakL) - linToDb(d.inputPeakR);
  const float outPeakBalanceDb = linToDb(d.outputPeakL) - linToDb(d.outputPeakR);
  const float decodeActivityPct = (d.gainSampleCount > 0) ? (100.0f * (float)d.decoderActiveCount / (float)d.gainSampleCount) : 0.0f;
  const float bypassPct = 100.0f * (float)d.bypassSampleCount / n;
  const float decodedPct = 100.0f * (float)d.decodedSampleCount / n;
  const float cutClampPct = (d.gainSampleCount > 0) ? (100.0f * (float)d.clampCutHitCount / (float)d.gainSampleCount) : 0.0f;
  const float boostClampPct = (d.gainSampleCount > 0) ? (100.0f * (float)d.clampBoostHitCount / (float)d.gainSampleCount) : 0.0f;
  const float nearCutPct = (d.gainSampleCount > 0) ? (100.0f * (float)d.nearCutCount / (float)d.gainSampleCount) : 0.0f;
  const float nearBoostPct = (d.gainSampleCount > 0) ? (100.0f * (float)d.nearBoostCount / (float)d.gainSampleCount) : 0.0f;
  const float highLowRatioDb = linToDb(d.highProxyMean) - linToDb(d.lowProxyMean);
  const float inCorrNorm = d.inputCrossMean / fmaxf(inRmsL * inRmsR, 1.0e-9f);
  const float outCorrNorm = d.outputCrossMean / fmaxf(outRmsL * outRmsR, 1.0e-9f);
  const char* activityLabel = (decodeActivityPct < 15.0f) ? "LOW" : ((decodeActivityPct > 60.0f) ? "HIGH" : "MODERATE");
  const char* lrAlert = (fabsf(outBalanceDb) >= 3.0f || d.outputAbsDiffMean >= 0.25f) ? "CHECK-LR" : "OK";
  const char* corrAlert = (outCorrNorm <= -0.35f) ? "OUT-OF-PHASE?" : ((fabsf(outCorrNorm) < 0.15f) ? "WEAK-CORR" : "OK");

  Serial.print(F("Samples: ")); Serial.println(d.sampleCount);
  Serial.print(F("Path share during snapshot: bypass=")); Serial.print(bypassPct, 1);
  Serial.print(F("% decoded=")); Serial.print(decodedPct, 1);
  Serial.println(F("%"));
  Serial.print(F("Tone state: ")); Serial.print(calToneEnabled ? F("ON") : F("OFF"));
  Serial.print(F(" @ ")); Serial.print(calToneHz, 1); Serial.print(F(" Hz "));
  Serial.print(calToneDb, 1); Serial.print(F(" dBFS mode=")); Serial.println(toneChannelModeLabel(toneChannelMode));

  Serial.print(F("Input peak L/R: ")); Serial.print(d.inputPeakL, 4); Serial.print(F(" / ")); Serial.print(d.inputPeakR, 4);
  Serial.print(F("  (mono ")); Serial.print(inPeakMono, 4); Serial.println(F(")"));
  Serial.print(F("Input RMS  L/R: ")); Serial.print(inRmsL, 5); Serial.print(F(" / ")); Serial.print(inRmsR, 5);
  Serial.print(F("  (mono ")); Serial.print(inRmsMono, 5); Serial.print(F(", ")); Serial.print(linToDb(inRmsMono), 1); Serial.println(F(" dBFS)"));
  Serial.print(F("Input mean L/R: ")); Serial.print(d.inputMeanL, 6); Serial.print(F(" / ")); Serial.println(d.inputMeanR, 6);

  Serial.print(F("Output peak L/R: ")); Serial.print(d.outputPeakL, 4); Serial.print(F(" / ")); Serial.print(d.outputPeakR, 4);
  Serial.print(F("  (mono ")); Serial.print(outPeakMono, 4); Serial.println(F(")"));
  Serial.print(F("Output RMS  L/R: ")); Serial.print(outRmsL, 5); Serial.print(F(" / ")); Serial.print(outRmsR, 5);
  Serial.print(F("  (mono ")); Serial.print(outRmsMono, 5); Serial.print(F(", ")); Serial.print(linToDb(outRmsMono), 1); Serial.println(F(" dBFS)"));
  Serial.print(F("Output mean L/R: ")); Serial.print(d.outputMeanL, 6); Serial.print(F(" / ")); Serial.println(d.outputMeanR, 6);

  Serial.print(F("Delta RMS (out-in): ")); Serial.print(deltaRmsDb, 2); Serial.println(F(" dB"));
  Serial.print(F("Delta Peak(out-in): ")); Serial.print(deltaPeakDb, 2); Serial.println(F(" dB"));
  Serial.print(F("L/R balance in/out: ")); Serial.print(inBalanceDb, 2); Serial.print(F(" dB / ")); Serial.print(outBalanceDb, 2); Serial.println(F(" dB"));
  Serial.print(F("L/R peak balance in/out: ")); Serial.print(inPeakBalanceDb, 2); Serial.print(F(" dB / "));
  Serial.print(outPeakBalanceDb, 2); Serial.println(F(" dB"));
  Serial.print(F("L-R abs mean in/out: ")); Serial.print(d.inputAbsDiffMean, 5); Serial.print(F(" / "));
  Serial.println(d.outputAbsDiffMean, 5);
  Serial.print(F("L/R correlation in/out: ")); Serial.print(inCorrNorm, 3); Serial.print(F(" / "));
  Serial.print(outCorrNorm, 3); Serial.print(F("  -> ")); Serial.println(corrAlert);
  Serial.print(F("Sidechain spectral proxy (high-vs-low): ")); Serial.print(highLowRatioDb, 2);
  Serial.println(F(" dB (decoded path only)"));
  Serial.print(F("Detector env (last): ")); Serial.print(d.lastEnvDb, 2); Serial.println(F(" dBFS"));
  Serial.print(F("Gain dB last/min/max/avg: ")); Serial.print(d.lastGainDb, 2); Serial.print(F(" / "));
  Serial.print(d.minGainDb, 2); Serial.print(F(" / ")); Serial.print(d.maxGainDb, 2);
  Serial.print(F(" / ")); Serial.println(d.avgGainDb, 2);
  Serial.print(F("Gain clamp hits cut/boost: ")); Serial.print(d.clampCutHitCount); Serial.print(F(" / "));
  Serial.print(d.clampBoostHitCount); Serial.print(F(" (")); Serial.print(cutClampPct, 1); Serial.print(F("% / "));
  Serial.print(boostClampPct, 1); Serial.println(F("%)"));
  Serial.print(F("Gain near-limit <=1 dB cut/boost: ")); Serial.print(nearCutPct, 1); Serial.print(F("% / "));
  Serial.print(nearBoostPct, 1); Serial.println(F("%"));
  Serial.print(F("Decode activity (|gain|>=1 dB): ")); Serial.print(decodeActivityPct, 1);
  Serial.print(F("% of decoded samples (")); Serial.print(activityLabel); Serial.println(F(")"));
  if (cutClampPct > 2.0f || boostClampPct > 2.0f) {
    Serial.println(F("Clamp interpretation: frequent clamp contact -> check level/strength/reference fit."));
  } else if (nearCutPct > 20.0f || nearBoostPct > 20.0f) {
    Serial.println(F("Clamp interpretation: often near limit -> watch for mistracking risk on hard passages."));
  } else {
    Serial.println(F("Clamp interpretation: clamp usage currently low."));
  }
  Serial.print(F("Cassette quick hints: LR=")); Serial.print(lrAlert);
  Serial.print(F(" corr=")); Serial.println(corrAlert);
  Serial.println();
}

void printStatus() {
  Serial.println();
  Serial.println(F("==== OCX TYPE 2 STATUS ===="));
  Serial.print(F("Mode: ")); Serial.println(ocx.getMode() == AudioEffectOCXType2CodecStereo::MODE_ENCODE ? F("ENCODE") : F("DECODE"));
  Serial.print(F("Preset: ")); Serial.println(presetLabel(currentPreset));
  Serial.print(F("Decoder operating mode: ")); Serial.println(decoderModeLabel(decoderOperatingMode));
  Serial.print(F("Safety mode: ")); Serial.println(safetyModeLabel(playbackSafetyMode));
  Serial.print(F("Deck type: ")); Serial.println(deckTypeLabel(deckType));
  Serial.print(F("Active transport: ")); Serial.println(transportLabel(activeTransport));
  Serial.print(F("Selected profile: ")); Serial.println(profileSelectLabel(selectedProfile));
  Serial.print(F("Profile validity single/lw1/lw2/common: "));
  Serial.print(profileStore.singleValid ? F("Y") : F("N")); Serial.print(F("/"));
  Serial.print(profileStore.lw1Valid ? F("Y") : F("N")); Serial.print(F("/"));
  Serial.print(profileStore.lw2Valid ? F("Y") : F("N")); Serial.print(F("/"));
  Serial.println(profileStore.commonValid ? F("Y") : F("N"));
  Serial.print(F("AUTO_CAL state: ")); Serial.println(autoCalStateLabel(autoCalState));
  Serial.print(F("AUTO_CAL values valid: ")); Serial.println(autoCalValid ? F("YES") : F("NO"));
  Serial.print(F("Wizard expected transport: ")); Serial.println(transportLabel(wizardExpectedTransport));
  Serial.print(F("Wizard segments detected/valid: ")); Serial.print(wizardDetectedSegments); Serial.print(F("/")); Serial.println(wizardValidSegments);
  Serial.print(F("Wizard reject reason: ")); Serial.println(autoRejectReason);
  Serial.print(F("PLAYBACK_GUARD_DYNAMIC: ")); Serial.println(guardEnabled ? F("ON") : F("OFF"));
  Serial.print(F("Periodic DIAG mode: ")); Serial.println(diagModeEnabled ? F("ON (3 s)") : F("OFF"));
  Serial.print(F("Guard state/reason: ")); Serial.print(guardStateLabel(guardState)); Serial.print(F(" / ")); Serial.println(guardReason);
  Serial.print(F("Guard trigger src/value/thr: ")); Serial.print(guardTriggerSource); Serial.print(F(" / "));
  Serial.print(guardTriggerValue, 2); Serial.print(F(" / ")); Serial.println(guardTriggerThreshold, 2);
  Serial.print(F("Guard can relax / blocked by: ")); Serial.print(guardCanRelax ? F("YES") : F("NO")); Serial.print(F(" / ")); Serial.println(guardRelaxBlockedBy);
  Serial.print(F("Bypass: ")); Serial.println(ocx.getBypass() ? F("ON") : F("OFF"));
  Serial.println(F("Bypass mode keeps output protection (headroom + soft clip), not a hard relay bypass."));
  Serial.print(F("Input trim: ")); Serial.print(ocx.getInputTrimDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Output trim (effective): ")); Serial.print(ocx.getOutputTrimDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Output trim (static base): ")); Serial.print(staticOutputTrimDb, 2); Serial.println(F(" dB"));
  Serial.print(F("Guard trim offset: ")); Serial.print(guardTrimOffsetDb, 2); Serial.println(F(" dB"));
  Serial.print(F("Strength: ")); Serial.println(ocx.getStrength(), 3);
  Serial.print(F("Reference (static): ")); Serial.print(ocx.getReferenceDb(), 2); Serial.println(F(" dB"));
  Serial.println(F("Detector: stereo-linked true RMS (max-link power -> RMS smoothing -> attack/release envelope)."));
  Serial.print(F("Attack: ")); Serial.print(ocx.getAttackMs(), 2); Serial.println(F(" ms"));
  Serial.print(F("Release: ")); Serial.print(ocx.getReleaseMs(), 2); Serial.println(F(" ms"));
  Serial.print(F("Detector RMS window: ")); Serial.print(ocx.getDetectorRmsMs(), 2); Serial.println(F(" ms"));
  Serial.print(F("Auto-trim enabled/max: ")); Serial.print(ocx.getAutoTrimEnabled() ? F("YES") : F("NO")); Serial.print(F(" / ")); Serial.print(ocx.getAutoTrimMaxDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Dropout hold/hf/level: ")); Serial.print(ocx.getDropoutHoldMs(), 2); Serial.print(F(" ms / "));
  Serial.print(ocx.getDropoutHfDropDb(), 2); Serial.print(F(" dB / "));
  Serial.print(ocx.getDropoutLevelDropDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Saturation soft-fail thr/knee: ")); Serial.print(ocx.getSaturationSoftfail() ? F("ON") : F("OFF")); Serial.print(F(" / "));
  Serial.print(ocx.getSaturationThreshold(), 3); Serial.print(F(" / ")); Serial.print(ocx.getSaturationKneeDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Sidechain HP: ")); Serial.print(ocx.getSidechainHpHz(), 1); Serial.println(F(" Hz"));
  Serial.print(F("Sidechain LP: ")); Serial.print(ocx.getSidechainLpHz(), 1); Serial.println(F(" Hz"));
  Serial.print(F("Sidechain shelf: ")); Serial.print(ocx.getSidechainShelfDb(), 1); Serial.print(F(" dB @ ")); Serial.print(ocx.getSidechainShelfHz(), 0); Serial.println(F(" Hz"));
  Serial.print(F("De-emphasis shelf: ")); Serial.print(ocx.getDeemphDb(), 2); Serial.print(F(" dB @ ")); Serial.print(ocx.getDeemphHz(), 0); Serial.println(F(" Hz"));
  Serial.print(F("Headroom (effective): ")); Serial.print(ocx.getHeadroomDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Headroom (static base): ")); Serial.print(staticHeadroomDb, 2); Serial.println(F(" dB"));
  Serial.print(F("Guard headroom offset: ")); Serial.print(guardHeadroomOffsetDb, 2); Serial.println(F(" dB"));
  Serial.print(F("DC block: ")); Serial.print(ocx.getDcBlockHz(), 1); Serial.println(F(" Hz"));
  Serial.print(F("Soft clip drive: ")); Serial.println(ocx.getSoftClipDrive(), 2);
  Serial.print(F("Tone: ")); Serial.print(calToneEnabled ? F("ON") : F("OFF"));
  Serial.print(F("  ")); Serial.print(calToneHz, 1); Serial.print(F(" Hz @ ")); Serial.print(calToneDb, 1); Serial.println(F(" dBFS"));
  Serial.print(F("Tone channel mode: ")); Serial.println(toneChannelModeLabel(toneChannelMode));
  Serial.println(F("Tone routing: post-decoder output injection (for deck/workflow level calibration)."));
  Serial.print(F("Input clip seen: ")); Serial.println(ocx.hasInputClip() ? F("YES") : F("NO"));
  Serial.print(F("Output clip seen: ")); Serial.println(ocx.hasOutputClip() ? F("YES") : F("NO"));
  Serial.print(F("Input clip count: ")); Serial.println(ocx.getInputClipCount());
  Serial.print(F("Output clip count: ")); Serial.println(ocx.getOutputClipCount());
  const ClipDelta delta = ocx.consumeClipDelta();
  Serial.print(F("Input clip new since last report: ")); Serial.println(delta.inputNew);
  Serial.print(F("Output clip new since last report: ")); Serial.println(delta.outputNew);
  Serial.print(F("Allocate fail count: ")); Serial.println(ocx.getAllocFailCount());
  printTelemetry();
}

void printAutoCalStatus() {
  Serial.print(F("[AUTO_CAL] state=")); Serial.print(autoCalStateLabel(autoCalState));
  Serial.print(F(" preset=")); Serial.print(presetLabel(currentPreset));
  Serial.print(F(" valid=")); Serial.print(autoCalValid ? F("YES") : F("NO"));
  Serial.print(F(" reject=")); Serial.print(autoRejectReason);
  Serial.print(F(" blocks=")); Serial.print(autoBlocksValid); Serial.print(F("/")); Serial.print(autoBlocksSeen);
  Serial.print(F(" toneBlocks=")); Serial.print(autoToneBlocks);
  Serial.print(F(" toneSeg=")); Serial.print(autoToneSegments);
  Serial.print(F(" segDetected=")); Serial.print(wizardDetectedSegments);
  Serial.print(F(" segValid=")); Serial.print(wizardValidSegments);
  Serial.print(F(" expected=")); Serial.print(transportLabel(wizardExpectedTransport));
  Serial.print(F(" score=")); Serial.println(autoCalScore, 2);
}

void printAutoCalLockedValues() {
  Serial.print(F("[AUTO_CAL_LOCKED] reference_db=")); Serial.print(autoCalReferenceDb, 2);
  Serial.print(F(" output_trim_db=")); Serial.print(autoCalOutputTrimDb, 2);
  Serial.print(F(" headroom_db=")); Serial.print(autoCalHeadroomDb, 2);
  Serial.print(F(" guardEnabled=")); Serial.print(guardEnabled ? F("YES") : F("NO"));
  Serial.print(F(" guardTrimOffsetDb=")); Serial.print(guardTrimOffsetDb, 2);
  Serial.print(F(" guardHeadroomOffsetDb=")); Serial.print(guardHeadroomOffsetDb, 2);
  Serial.print(F(" guardBoostCapReductionDb=")); Serial.println(guardBoostCapReductionDb, 2);
}

void printStoredProfiles() {
  Serial.print(F("[PROFILES] deck_type=")); Serial.print(deckTypeLabel(deckType));
  Serial.print(F(" active_transport=")); Serial.print(transportLabel(activeTransport));
  Serial.print(F(" selected=")); Serial.println(profileSelectLabel(selectedProfile));
  Serial.print(F("[PROFILES] single_valid=")); Serial.print(profileStore.singleValid ? F("YES") : F("NO"));
  Serial.print(F(" lw1_valid=")); Serial.print(profileStore.lw1Valid ? F("YES") : F("NO"));
  Serial.print(F(" lw2_valid=")); Serial.print(profileStore.lw2Valid ? F("YES") : F("NO"));
  Serial.print(F(" common_valid=")); Serial.println(profileStore.commonValid ? F("YES") : F("NO"));
  Serial.print(F("[PROFILES_SINGLE] ref=")); Serial.print(profileStore.singleProfile.referenceDb, 2);
  Serial.print(F(" trim=")); Serial.print(profileStore.singleProfile.outputTrimDb, 2);
  Serial.print(F(" head=")); Serial.println(profileStore.singleProfile.headroomDb, 2);
  Serial.print(F("[PROFILES_LW1] ref=")); Serial.print(profileStore.lw1Profile.referenceDb, 2);
  Serial.print(F(" trim=")); Serial.print(profileStore.lw1Profile.outputTrimDb, 2);
  Serial.print(F(" head=")); Serial.println(profileStore.lw1Profile.headroomDb, 2);
  Serial.print(F("[PROFILES_LW2] ref=")); Serial.print(profileStore.lw2Profile.referenceDb, 2);
  Serial.print(F(" trim=")); Serial.print(profileStore.lw2Profile.outputTrimDb, 2);
  Serial.print(F(" head=")); Serial.println(profileStore.lw2Profile.headroomDb, 2);
  Serial.print(F("[PROFILES_COMMON] ref=")); Serial.print(profileStore.commonProfile.referenceDb, 2);
  Serial.print(F(" trim=")); Serial.print(profileStore.commonProfile.outputTrimDb, 2);
  Serial.print(F(" head=")); Serial.println(profileStore.commonProfile.headroomDb, 2);
}

void printPeriodicDiagLine() {
  const AudioEffectOCXType2CodecStereo::DiagSnapshot d = ocx.getSignalDiagnosticsSnapshot();
  if (d.sampleCount == 0) {
    Serial.println(F("[DIAG] waiting_for_signal samples=0"));
    return;
  }
  const float n = (float)d.sampleCount;
  const float inRmsL = sqrtf(d.inputSumSqL / n);
  const float inRmsR = sqrtf(d.inputSumSqR / n);
  const float outRmsL = sqrtf(d.outputSumSqL / n);
  const float outRmsR = sqrtf(d.outputSumSqR / n);
  const float inRmsMono = sqrtf(0.5f * (inRmsL * inRmsL + inRmsR * inRmsR));
  const float outRmsMono = sqrtf(0.5f * (outRmsL * outRmsL + outRmsR * outRmsR));
  const float inPeakMono = fmaxf(d.inputPeakL, d.inputPeakR);
  const float outPeakMono = fmaxf(d.outputPeakL, d.outputPeakR);
  const float outBalanceDb = linToDb(outRmsL) - linToDb(outRmsR);
  const float outCorrNorm = d.outputCrossMean / fmaxf(outRmsL * outRmsR, 1.0e-9f);
  const float decodeActivityPct = (d.gainSampleCount > 0) ? (100.0f * (float)d.decoderActiveCount / (float)d.gainSampleCount) : 0.0f;
  const float nearBoostPct = (d.gainSampleCount > 0) ? (100.0f * (float)d.nearBoostCount / (float)d.gainSampleCount) : 0.0f;
  const float boostClampPct = (d.gainSampleCount > 0) ? (100.0f * (float)d.clampBoostHitCount / (float)d.gainSampleCount) : 0.0f;
  static float prevAvgGainDb = 0.0f;
  static float prevOutRmsDb = -120.0f;
  static float prevOutBalDb = 0.0f;
  static bool hasPrev = false;
  const float outRmsDb = linToDb(outRmsMono);
  const float avgGainDb = d.avgGainDb;
  const float driftGainDb = hasPrev ? (avgGainDb - prevAvgGainDb) : 0.0f;
  const float driftOutRmsDb = hasPrev ? (outRmsDb - prevOutRmsDb) : 0.0f;
  const float driftBalanceDb = hasPrev ? (outBalanceDb - prevOutBalDb) : 0.0f;
  hasPrev = true;
  prevAvgGainDb = avgGainDb;
  prevOutRmsDb = outRmsDb;
  prevOutBalDb = outBalanceDb;

  Serial.print(F("[DIAG] samples=")); Serial.print(d.sampleCount);
  Serial.print(F(" inPk=")); Serial.print(inPeakMono, 3);
  Serial.print(F(" outPk=")); Serial.print(outPeakMono, 3);
  Serial.print(F(" inRmsDb=")); Serial.print(linToDb(inRmsMono), 1);
  Serial.print(F(" outRmsDb=")); Serial.print(outRmsDb, 1);
  Serial.print(F(" gDb(last/min/max/avg)=")); Serial.print(d.lastGainDb, 2); Serial.print(F("/"));
  Serial.print(d.minGainDb, 2); Serial.print(F("/")); Serial.print(d.maxGainDb, 2); Serial.print(F("/")); Serial.print(avgGainDb, 2);
  Serial.print(F(" nearBoost%=")); Serial.print(nearBoostPct, 1);
  Serial.print(F(" clampBoost%=")); Serial.print(boostClampPct, 1);
  Serial.print(F(" clipOutNew=")); Serial.print(guardWindowClip1s);
  Serial.print(F(" clipOutTotal=")); Serial.print(ocx.getOutputClipCount());
  Serial.print(F(" guard=")); Serial.print(guardStateLabel(guardState)); Serial.print(F("/")); Serial.print(guardReason);
  Serial.print(F(" gTrim=")); Serial.print(guardTrimOffsetDb, 2);
  Serial.print(F(" gHead=")); Serial.print(guardHeadroomOffsetDb, 2);
  Serial.print(F(" gBoost=")); Serial.print(guardBoostCapReductionDb, 2);
  Serial.print(F(" baseRef=")); Serial.print(staticReferenceDb, 2);
  Serial.print(F(" baseTrim=")); Serial.print(staticOutputTrimDb, 2);
  Serial.print(F(" baseHead=")); Serial.print(staticHeadroomDb, 2);
  Serial.print(F(" effTrim=")); Serial.print(ocx.getOutputTrimDb(), 2);
  Serial.print(F(" effHead=")); Serial.print(ocx.getHeadroomDb(), 2);
  Serial.print(F(" lrBalDb=")); Serial.print(outBalanceDb, 2);
  Serial.print(F(" corr=")); Serial.print(outCorrNorm, 2);
  Serial.print(F(" act%=")); Serial.print(decodeActivityPct, 1);
  Serial.print(F(" drift(g/rms/bal)=")); Serial.print(driftGainDb, 2); Serial.print(F("/")); Serial.print(driftOutRmsDb, 2); Serial.print(F("/")); Serial.print(driftBalanceDb, 2);
  Serial.print(F(" stableMs=")); Serial.println(millis() - guardLastStateChangeMs);

  const float marginPreSoftClipDb = linToDb(1.0f / fmaxf(d.pkPreSoftClip, 1.0e-6f));
  const float marginOutDb = linToDb(1.0f / fmaxf(d.pkPostOutputTrim, 1.0e-6f));
  Serial.print(F("[DIAG2] pkInRaw=")); Serial.print(d.pkInRaw, 3);
  Serial.print(F(" pkAfterInputTrim=")); Serial.print(d.pkAfterInputTrim, 3);
  Serial.print(F(" pkAfterDecodeGain=")); Serial.print(d.pkAfterDecodeGain, 3);
  Serial.print(F(" pkAfterDeEmp=")); Serial.print(d.pkAfterDeEmphasis, 3);
  Serial.print(F(" pkPreGuard=")); Serial.print(d.pkPreGuard, 3);
  Serial.print(F(" pkPostGuard=")); Serial.print(d.pkPostGuard, 3);
  Serial.print(F(" pkPreClip=")); Serial.print(d.pkPreSoftClip, 3);
  Serial.print(F(" pkPostClip=")); Serial.print(d.pkPostSoftClip, 3);
  Serial.print(F(" pkOut=")); Serial.print(d.pkPostOutputTrim, 3);
  Serial.print(F(" marginPreClipDb=")); Serial.print(marginPreSoftClipDb, 2);
  Serial.print(F(" marginOutDb=")); Serial.print(marginOutDb, 2);
  Serial.print(F(" gCmd=")); Serial.print(d.lastGainDb, 2);
  Serial.print(F(" gApplied=")); Serial.print(d.avgGainDb, 2);
  Serial.print(F(" gGuard=")); Serial.print(guardTrimOffsetDb - guardHeadroomOffsetDb, 2);
  Serial.print(F(" trigSrc=")); Serial.print(guardTriggerSource);
  Serial.print(F(" trigVal=")); Serial.print(guardTriggerValue, 2);
  Serial.print(F(" trigThr=")); Serial.println(guardTriggerThreshold, 2);
}

void printControlDiagnostics() {
  Serial.println(F("---- OCX CONTROL DIAGNOSTICS ----"));
  Serial.print(F("Safety mode: ")); Serial.println(safetyModeLabel(playbackSafetyMode));
  Serial.print(F("Decoder mode: ")); Serial.println(decoderModeLabel(decoderOperatingMode));
  Serial.print(F("Guard enabled: ")); Serial.println((guardEnabled && playbackSafetyMode != SAFETY_STRICT_REFERENCE) ? F("YES") : F("NO"));
  Serial.print(F("Guard state/reason: ")); Serial.print(guardStateLabel(guardState)); Serial.print(F(" / ")); Serial.println(guardReason);
  Serial.print(F("Guard trigger source/value/threshold: ")); Serial.print(guardTriggerSource); Serial.print(F(" / "));
  Serial.print(guardTriggerValue, 3); Serial.print(F(" / ")); Serial.println(guardTriggerThreshold, 3);
  Serial.print(F("Guard can relax / blocked by: ")); Serial.print(guardCanRelax ? F("YES") : F("NO")); Serial.print(F(" / ")); Serial.println(guardRelaxBlockedBy);
  const unsigned long ago = (guardLastTriggerMs == 0) ? 0UL : (millis() - guardLastTriggerMs);
  Serial.print(F("Guard last trigger ms ago: ")); Serial.println(ago);
  Serial.print(F("Guard offsets trim/head/boost: ")); Serial.print(guardTrimOffsetDb, 2); Serial.print(F(" / "));
  Serial.print(guardHeadroomOffsetDb, 2); Serial.print(F(" / ")); Serial.println(guardBoostCapReductionDb, 2);
  printPeriodicDiagLine();
}

void resetWarningLatches() {
  lastWarnedInputClipCount = ocx.getInputClipCount();
  lastWarnedOutputClipCount = ocx.getOutputClipCount();
  lastWarnedAllocFailCount = ocx.getAllocFailCount();
}

void maybePrintAutoLockSummary() {
  if (!autoLockSummaryPending || autoCalState != AUTO_LOCKED) return;
  printAutoCalStatus();
  printAutoCalLockedValues();
  if (deckType == DECK_DUAL_LW && wizardExpectedTransport == TRANSPORT_LW1) {
    Serial.println(F("[WIZARD] Messung fuer LW1 abgeschlossen. Fuer LW2 Deck umschalten (']') und 'l' erneut starten."));
  } else if (deckType == DECK_DUAL_LW && wizardExpectedTransport == TRANSPORT_LW2) {
    if (selectedProfile == PROFILE_COMMON && profileStore.commonValid) {
      Serial.println(F("[WIZARD] Messung fuer LW2 abgeschlossen. common_profile ist jetzt Default und gespeichert."));
    } else {
      Serial.println(F("[WIZARD] Messung fuer LW2 abgeschlossen. common_profile nicht verfuegbar; dediziertes Profil bleibt aktiv."));
    }
  } else {
    Serial.println(F("[WIZARD] Single-LW Messung abgeschlossen."));
  }
  autoLockSummaryPending = false;
}

void maybePrintDiagPeriodic(unsigned long now) {
  if (!diagModeEnabled) return;
  if (now - diagLastPrintMs < kDiagIntervalMs) return;
  diagLastPrintMs = now;
  printPeriodicDiagLine();
}

void handleSerial() {
  while (Serial.available()) {
    const char c = Serial.read();
    switch (c) {
      case 'h': printHelp(); break;
      case 'p': printStatus(); break;
      case 'm': printCompactTelemetryLine(); break;
      case 'T':
        diagModeEnabled = !diagModeEnabled;
        diagLastPrintMs = 0;
        Serial.print(F("[DIAG] periodic mode "));
        Serial.println(diagModeEnabled ? F("ON (3 s)") : F("OFF"));
        break;
      case 'n': printSignalDiagnosticsSnapshot(); break;
      case 'N': ocx.resetSignalDiagnostics(); break;
      case 'x': ocx.clearClipFlags(); resetWarningLatches(); break;
      case 'v': {
        const ClipDelta delta = ocx.consumeClipDelta();
        Serial.print(F("[CLIPΔ] inNew=")); Serial.print(delta.inputNew);
        Serial.print(F(" outNew=")); Serial.println(delta.outputNew);
      } break;
      case 'X': ocx.clearClipFlags(); ocx.clearRuntimeCounters(); ocx.resetSignalDiagnostics(); AudioProcessorUsageMaxReset(); AudioMemoryUsageMaxReset(); resetWarningLatches(); break;
      case 'B': ocx.resetState(); break;
      case 'b': ocx.setBypass(!ocx.getBypass()); break;
      case 'M': Serial.println(ocx.getMode() == AudioEffectOCXType2CodecStereo::MODE_ENCODE ? F("ENCODE") : F("DECODE")); break;
      case 'j': Serial.println(presetLabel(currentPreset)); break;
      case '6':
        playbackSafetyMode = static_cast<PlaybackSafetyMode>((static_cast<uint8_t>(playbackSafetyMode) + 1U) % 3U);
        applyEffectiveDecoderSettings();
        Serial.print(F("[CFG] safety_mode=")); Serial.println(safetyModeLabel(playbackSafetyMode));
        break;
      case '7': printControlDiagnostics(); break;
      case 'u': currentPreset = PRESET_UNIVERSAL; applyDecoderPreset(currentPreset); autoCalState = AUTO_IDLE; break;
      case 'U':
        decoderOperatingMode = static_cast<DecoderOperatingMode>((static_cast<uint8_t>(decoderOperatingMode) + 1U) % 3U);
        applyEffectiveDecoderSettings();
        Serial.print(F("[CFG] decoder_mode=")); Serial.println(decoderModeLabel(decoderOperatingMode));
        break;
      case '1':
        deckType = DECK_SINGLE_LW;
        activeTransport = TRANSPORT_LW1;
        selectedProfile = PROFILE_SINGLE;
        Serial.println(F("[CFG] deck_type=SINGLE_LW"));
        break;
      case '2':
        deckType = DECK_DUAL_LW;
        selectedProfile = (activeTransport == TRANSPORT_LW2) ? PROFILE_LW2 : PROFILE_LW1;
        Serial.println(F("[CFG] deck_type=DUAL_LW"));
        break;
      case '[':
        activeTransport = TRANSPORT_LW1;
        if (deckType == DECK_DUAL_LW && selectedProfile != PROFILE_COMMON) selectedProfile = PROFILE_LW1;
        Serial.println(F("[CFG] active_transport=LW1"));
        break;
      case ']':
        activeTransport = TRANSPORT_LW2;
        if (deckType == DECK_DUAL_LW && selectedProfile != PROFILE_COMMON) selectedProfile = PROFILE_LW2;
        Serial.println(F("[CFG] active_transport=LW2"));
        break;
      case '{':
        selectedProfile = (deckType == DECK_SINGLE_LW) ? PROFILE_SINGLE : ((activeTransport == TRANSPORT_LW2) ? PROFILE_LW2 : PROFILE_LW1);
        if ((currentPreset == PRESET_AUTO_CAL) && profileSelectionValid(selectedProfile)) applyDecoderPreset(PRESET_AUTO_CAL);
        Serial.print(F("[CFG] selected_profile=")); Serial.println(profileSelectLabel(selectedProfile));
        break;
      case '}':
        if (deckType == DECK_DUAL_LW && profileStore.commonValid) {
          selectedProfile = PROFILE_COMMON;
          if (currentPreset == PRESET_AUTO_CAL) applyDecoderPreset(PRESET_AUTO_CAL);
          Serial.println(F("[CFG] selected_profile=COMMON_PROFILE"));
        } else {
          Serial.println(F("[CFG] common_profile not valid"));
        }
        break;
      case '|': printStoredProfiles(); break;
      case 'l':
        if (deckType == DECK_DUAL_LW) {
          wizardExpectedTransport = (activeTransport == TRANSPORT_LW2) ? TRANSPORT_LW2 : TRANSPORT_LW1;
        } else {
          wizardExpectedTransport = TRANSPORT_LW1;
        }
        beginAutoCal();
        break;
      case 'J': printAutoCalStatus(); break;
      case 'K': printAutoCalRawTelemetry(); break;
      case 'L': printAutoCalLockedValues(); break;
      case 'H':
        guardEnabled = !guardEnabled;
        if (!guardEnabled) {
          guardTrimOffsetDb = 0.0f;
          guardHeadroomOffsetDb = 0.0f;
          guardBoostCapReductionDb = 0.0f;
        }
        applyEffectiveDecoderSettings();
        break;
      case '@':
        guardTrimOffsetDb = 0.0f;
        guardHeadroomOffsetDb = 0.0f;
        guardBoostCapReductionDb = 0.0f;
        guardWindowPos = 0;
        guardWindowCount = 0;
        guardWindowClip1s = 0;
        guardWindowNearLimit10s = 0;
        guardWindowBoostClamp10s = 0;
        memset(guardClipBins, 0, sizeof(guardClipBins));
        memset(guardNearBins, 0, sizeof(guardNearBins));
        memset(guardBoostBins, 0, sizeof(guardBoostBins));
        guardTriggerSource = "manual_reset";
        guardTriggerValue = 0.0f;
        guardTriggerThreshold = 0.0f;
        guardRelaxBlockedBy = "none";
        guardCanRelax = true;
        guardState = GUARD_IDLE;
        guardReason = "manual_reset";
        applyEffectiveDecoderSettings();
        Serial.println(F("[CFG] guard history hard reset"));
        break;
      case '>': ocx.setMode(AudioEffectOCXType2CodecStereo::MODE_DECODE); break;
      case '<': ocx.setMode(AudioEffectOCXType2CodecStereo::MODE_ENCODE); break;
      case 'P': persistSettings(); break;
      case '!': factoryResetSettings(); break;
      case '0': applyFactoryPreset(); break;
      case 'i': ocx.setInputTrimDb(ocx.getInputTrimDb() - 0.5f); break;
      case 'I': ocx.setInputTrimDb(ocx.getInputTrimDb() + 0.5f); break;
      case 'o': staticOutputTrimDb = clampf(staticOutputTrimDb - 0.5f, -18.0f, 18.0f); applyEffectiveDecoderSettings(); break;
      case 'O': staticOutputTrimDb = clampf(staticOutputTrimDb + 0.5f, -18.0f, 18.0f); applyEffectiveDecoderSettings(); break;
      case 's': ocx.setStrength(ocx.getStrength() - 0.05f); break;
      case 'S': ocx.setStrength(ocx.getStrength() + 0.05f); break;
      case 'f': staticReferenceDb = clampf(staticReferenceDb - 1.0f, -40.0f, 0.0f); applyEffectiveDecoderSettings(); break;
      case 'F': staticReferenceDb = clampf(staticReferenceDb + 1.0f, -40.0f, 0.0f); applyEffectiveDecoderSettings(); break;
      case 'a': ocx.setAttackMs(ocx.getAttackMs() - 0.5f); break;
      case 'A': ocx.setAttackMs(ocx.getAttackMs() + 0.5f); break;
      case 'r': ocx.setReleaseMs(ocx.getReleaseMs() - 5.0f); break;
      case 'R': ocx.setReleaseMs(ocx.getReleaseMs() + 5.0f); break;
      case 'c': ocx.setSidechainHpHz(ocx.getSidechainHpHz() - 10.0f); break;
      case 'C': ocx.setSidechainHpHz(ocx.getSidechainHpHz() + 10.0f); break;
      case 'q': ocx.setSidechainShelfDb(ocx.getSidechainShelfDb() - 1.0f); break;
      case 'Q': ocx.setSidechainShelfDb(ocx.getSidechainShelfDb() + 1.0f); break;
      case 'w': ocx.setSidechainShelfHz(ocx.getSidechainShelfHz() - 100.0f); break;
      case 'W': ocx.setSidechainShelfHz(ocx.getSidechainShelfHz() + 100.0f); break;
      case 'e': ocx.setDeemphDb(ocx.getDeemphDb() - 1.0f); break;
      case 'E': ocx.setDeemphDb(ocx.getDeemphDb() + 1.0f); break;
      case 'd': ocx.setDeemphHz(ocx.getDeemphHz() - 50.0f); break;
      case 'D': ocx.setDeemphHz(ocx.getDeemphHz() + 50.0f); break;
      case 'g': staticHeadroomDb = clampf(staticHeadroomDb - 0.5f, 0.0f, 6.0f); applyEffectiveDecoderSettings(); break;
      case 'G': staticHeadroomDb = clampf(staticHeadroomDb + 0.5f, 0.0f, 6.0f); applyEffectiveDecoderSettings(); break;
      case 'y': ocx.setDcBlockHz(ocx.getDcBlockHz() - 1.0f); break;
      case 'Y': ocx.setDcBlockHz(ocx.getDcBlockHz() + 1.0f); break;
      case 't': calToneEnabled = !calToneEnabled; updateTone(); break;
      case 'z': calToneDb -= 1.0f; updateTone(); break;
      case 'Z': calToneDb += 1.0f; updateTone(); break;
      case 'k':
        toneChannelMode = (toneChannelMode == TONE_RIGHT) ? TONE_BOTH : (ToneChannelMode)((uint8_t)toneChannelMode + 1);
        updateTone();
        break;
      default: break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  AudioMemory(OCXProfile::kAudioMemoryBlocks);
  mixL.gain(0, 1.0f);
  mixR.gain(0, 1.0f);
  mixL.gain(1, 0.0f);
  mixR.gain(1, 0.0f);
  calTone.amplitude(0.0f);
  toneDetectLLo.frequency(960.0f, 20);
  toneDetectLCenter.frequency(1000.0f, 20);
  toneDetectLHi.frequency(1040.0f, 20);
  toneDetectRLo.frequency(960.0f, 20);
  toneDetectRCenter.frequency(1000.0f, 20);
  toneDetectRHi.frequency(1040.0f, 20);

  codec.enable();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(OCXProfile::kLineInLevel);
  codec.lineOutLevel(OCXProfile::kLineOutLevel);
  codec.headphoneSelect(AUDIO_HEADPHONE_DAC);
  codec.volume(OCXProfile::kHeadphoneVolume);

  applyFactoryPreset();
  loadSettingsOrFactory();
  updateTone();

  Serial.println();
  Serial.println(F("OCX Type 2 decoder firmware ready (preset system: UNIVERSAL/AUTO_CAL + PLAYBACK_GUARD_DYNAMIC)."));
  Serial.print(F("Runtime sample-rate (AUDIO_SAMPLE_RATE_EXACT): "));
  Serial.println((double)AUDIO_SAMPLE_RATE_EXACT, 4);
  Serial.println(F("Profile/sample-method baseline remains 44100 Hz nominal for simulator/harness comparability."));
  Serial.println(F("Offline build validated; final analog validation still requires physical Teensy 4.1 + SGTL5000 hardware."));
  printStatus();
  printHelp();
  resetWarningLatches();
}

void loop() {
  handleSerial();
  updateAutoCal();
  maybePrintAutoLockSummary();
  updatePlaybackGuard();
  maybePrintDiagPeriodic(millis());
  const unsigned long now = millis();
  if (now - lastStatusMs > 2000) {
    lastStatusMs = now;
    const uint32_t inputClipCount = ocx.getInputClipCount();
    const uint32_t outputClipCount = ocx.getOutputClipCount();
    const uint32_t allocFailCount = ocx.getAllocFailCount();
    if (inputClipCount > lastWarnedInputClipCount) {
      Serial.print(F("[WARN] Input clipping increased (new events="));
      Serial.print(inputClipCount - lastWarnedInputClipCount);
      Serial.println(F("). Lower source level or reduce input trim."));
      lastWarnedInputClipCount = inputClipCount;
    }
    if (outputClipCount > lastWarnedOutputClipCount) {
      Serial.print(F("[WARN] Output clipping increased (new events="));
      Serial.print(outputClipCount - lastWarnedOutputClipCount);
      Serial.println(F("). Lower output trim or increase headroom."));
      lastWarnedOutputClipCount = outputClipCount;
    }
    if (allocFailCount > lastWarnedAllocFailCount) {
      Serial.print(F("[WARN] Audio block allocation failures increased (new events="));
      Serial.print(allocFailCount - lastWarnedAllocFailCount);
      Serial.println(F(")."));
      lastWarnedAllocFailCount = allocFailCount;
    }
  }
}
