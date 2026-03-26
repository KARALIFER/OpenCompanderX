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

// Forward declaration to keep Arduino auto-prototype generation safe for
// functions that take PersistSettings before the struct definition appears.
struct PersistSettings;

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
static constexpr float kSidechainHpHz = 90.0f;
static constexpr float kSidechainShelfHz = 2800.0f;
static constexpr float kSidechainShelfDb = 16.0f;
static constexpr float kDeemphHz = 1850.0f;
static constexpr float kDeemphDb = -6.0f;
static constexpr float kSoftClipDrive = 1.08f;
static constexpr float kDcBlockHz = 12.0f;
static constexpr float kHeadroomDb = 1.0f;
static constexpr float kW1200OutputTrimDb = -4.0f;
static constexpr float kW1200Strength = 0.710f;
static constexpr float kW1200ReferenceDb = -15.0f;
static constexpr float kW1200HeadroomDb = 3.0f;
static constexpr float kEncInputTrimDb = 0.0f;
static constexpr float kEncOutputTrimDb = -1.0f;
static constexpr float kEncStrength = 0.45f;
static constexpr float kEncReferenceDb = -18.0f;
static constexpr float kEncMaxBoostDb = 24.0f;
static constexpr float kEncMaxCutDb = 9.0f;
static constexpr float kEncAttackMs = 3.5f;
static constexpr float kEncReleaseMs = 140.0f;
static constexpr float kEncSidechainHpHz = 90.0f;
static constexpr float kEncSidechainShelfHz = 2800.0f;
static constexpr float kEncSidechainShelfDb = 14.0f;
static constexpr float kEncTiltHz = 1850.0f;
static constexpr float kEncTiltDb = 5.0f;
static constexpr float kEncSoftClipDrive = 1.04f;
static constexpr float kEncDcBlockHz = 12.0f;
static constexpr float kEncHeadroomDb = 1.0f;
// Calibration tone defaults are deck/workflow calibration parameters, not decoder model reference.
static constexpr float kToneHz = 400.0f;
static constexpr float kToneDb = -9.8f;
}

enum PresetId : uint8_t { PRESET_UNIVERSAL = 0, PRESET_W1200 = 1, PRESET_AUTO_CAL = 2 };
enum AutoCalState : uint8_t { AUTO_IDLE = 0, AUTO_WAIT_FOR_TONE = 1, AUTO_MEASURE = 2, AUTO_COMPUTE = 3, AUTO_LOCKED = 4, AUTO_FAILED = 5 };


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

class AudioEffectOCXType2CodecStereo : public AudioStream {
public:
  enum Mode : uint8_t { MODE_DECODE = 0, MODE_ENCODE = 1 };
  AudioEffectOCXType2CodecStereo() : AudioStream(2, inputQueueArray) { recalcAll(); resetState(); }
  virtual void update(void);
  struct DiagSnapshot {
    float inputPeakL = 0.0f;
    float inputPeakR = 0.0f;
    float outputPeakL = 0.0f;
    float outputPeakR = 0.0f;
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

  void setBypass(bool v)             { bypass = v; }
  bool getBypass() const             { return bypass; }
  void setMode(Mode m)               { mode = m; }
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
  void setSidechainShelfHz(float hz) { sidechainShelfHz = clampf(hz, 500.0f, 12000.0f); recalcSidechainFilters(); }
  void setSidechainShelfDb(float db) { sidechainShelfDb = clampf(db, 0.0f, 24.0f); recalcSidechainFilters(); }
  void setDeemphHz(float hz)         { deemphHz = clampf(hz, 300.0f, 12000.0f); recalcDeemphFilter(); }
  void setDeemphDb(float db)         { deemphDb = clampf(db, -24.0f, 0.0f); recalcDeemphFilter(); }
  void setSoftClipDrive(float v)     { softClipDrive = clampf(v, 1.0f, 2.0f); }
  void setDcBlockHz(float hz)        { dcBlockHz = clampf(hz, 1.0f, 40.0f); recalcDcBlockers(); }
  void setHeadroomDb(float db)       { headroomDb = clampf(db, 0.0f, 6.0f); headroomGain = dbToLin(-headroomDb); }

  float getInputTrimDb() const       { return inputTrimDb; }
  float getOutputTrimDb() const      { return outputTrimDb; }
  float getStrength() const          { return strength; }
  float getReferenceDb() const       { return referenceDb; }
  float getAttackMs() const          { return attackMs; }
  float getReleaseMs() const         { return releaseMs; }
  float getSidechainHpHz() const     { return sidechainHpHz; }
  float getSidechainShelfHz() const  { return sidechainShelfHz; }
  float getSidechainShelfDb() const  { return sidechainShelfDb; }
  float getDeemphHz() const          { return deemphHz; }
  float getDeemphDb() const          { return deemphDb; }
  float getSoftClipDrive() const     { return softClipDrive; }
  float getDcBlockHz() const         { return dcBlockHz; }
  float getHeadroomDb() const        { return headroomDb; }

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
      scShelf[ch].reset();
      encScHP[ch].reset();
      encScShelf[ch].reset();
      deemph[ch].reset();
      encTilt[ch].reset();
      dcBlock[ch].reset();
      encDcBlock[ch].reset();
    }
    linkedEnv2 = 1.0e-9f;
    encLinkedEnv2 = 1.0e-9f;
    noInterrupts();
    diagLastGainDb = 0.0f;
    diagLastEnvDb = -120.0f;
    interrupts();
  }

private:
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

  float sidechainHpHz = 90.0f;
  float sidechainShelfHz = 2800.0f;
  float sidechainShelfDb = 16.0f;
  Biquad scHP[2];
  Biquad scShelf[2];
  float encSidechainHpHz = OCXProfile::kEncSidechainHpHz;
  float encSidechainShelfHz = OCXProfile::kEncSidechainShelfHz;
  float encSidechainShelfDb = OCXProfile::kEncSidechainShelfDb;
  Biquad encScHP[2];
  Biquad encScShelf[2];

  float attackMs = 3.5f;
  float releaseMs = 140.0f;
  float attackCoeff = 0.0f;
  float releaseCoeff = 0.0f;
  float linkedEnv2 = 1.0e-9f;
  float encAttackMs = OCXProfile::kEncAttackMs;
  float encReleaseMs = OCXProfile::kEncReleaseMs;
  float encAttackCoeff = 0.0f;
  float encReleaseCoeff = 0.0f;
  float encLinkedEnv2 = 1.0e-9f;

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
  uint32_t lastReportedInputClipCount = 0;
  uint32_t lastReportedOutputClipCount = 0;

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
  }
  void recalcEncoderDetector() {
    encAttackCoeff = expf(-1.0f / (OCXProfile::kFs * encAttackMs * 0.001f));
    encReleaseCoeff = expf(-1.0f / (OCXProfile::kFs * encReleaseMs * 0.001f));
  }

  void recalcSidechainFilters() {
    for (int ch = 0; ch < 2; ++ch) {
      designHighpass(scHP[ch], OCXProfile::kFs, sidechainHpHz, 0.7071f);
      designHighShelf(scShelf[ch], OCXProfile::kFs, sidechainShelfHz, sidechainShelfDb, 0.8f);
    }
  }
  void recalcEncoderSidechainFilters() {
    for (int ch = 0; ch < 2; ++ch) {
      designHighpass(encScHP[ch], OCXProfile::kFs, encSidechainHpHz, 0.7071f);
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

  inline float finalizeOutput(float y, float outGain, float roomGain, float clipDrive) {
    y = clampf(softClip(y * outGain * roomGain, clipDrive), -1.0f, 1.0f);
    if (fabsf(y) > 0.98f) {
      outputClipFlag = true;
      ++outputClipCount;
    }
    return sanitizef(y);
  }

  inline void processDecode(float xL, float xR, float &outL, float &outR) {
    const float scL = scShelf[0].process(scHP[0].process(xL));
    const float scR = scShelf[1].process(scHP[1].process(xR));
    const float lowProxy = 0.5f * (fabsf(xL) + fabsf(xR));
    const float highProxy = 0.5f * (fabsf(scL) + fabsf(scR));
    diagLowProxySum += lowProxy;
    diagHighProxySum += highProxy;
    const float linkedP = fmaxf(scL * scL, scR * scR);
    const float coeff = (linkedP > linkedEnv2) ? attackCoeff : releaseCoeff;
    linkedEnv2 = sanitizef(coeff * linkedEnv2 + (1.0f - coeff) * linkedP);
    const float env = sqrtf(linkedEnv2 + 1.0e-12f);
    const float rawGainDb = (linToDb(env) - referenceDb) * strength;
    float gainDb = clampf(rawGainDb, -maxCutDb, maxBoostDb);
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
    outL = finalizeOutput(deemph[0].process(xL * gainLin), outputGain, headroomGain, softClipDrive);
    outR = finalizeOutput(deemph[1].process(xR * gainLin), outputGain, headroomGain, softClipDrive);
  }

  inline void processEncode(float xL, float xR, float &outL, float &outR) {
    const float scL = encScShelf[0].process(encScHP[0].process(xL));
    const float scR = encScShelf[1].process(encScHP[1].process(xR));
    const float linkedP = fmaxf(scL * scL, scR * scR);
    const float coeff = (linkedP > encLinkedEnv2) ? encAttackCoeff : encReleaseCoeff;
    encLinkedEnv2 = sanitizef(coeff * encLinkedEnv2 + (1.0f - coeff) * linkedP);
    const float env = sqrtf(encLinkedEnv2 + 1.0e-12f);
    const float rawGainDb = -((linToDb(env) - encReferenceDb) * encStrength);
    const float gainDb = clampf(rawGainDb, -encMaxCutDb, encMaxBoostDb);
    diagLastGainDb = gainDb;
    diagLastEnvDb = linToDb(env);
    const float gainLin = dbToLin(gainDb);
    outL = finalizeOutput(encTilt[0].process(xL * gainLin), encOutputGain, encHeadroomGain, encSoftClipDrive);
    outR = finalizeOutput(encTilt[1].process(xR * gainLin), encOutputGain, encHeadroomGain, encSoftClipDrive);
  }

  inline void processStereo(float inL, float inR, float &outL, float &outR) {
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
      outL = finalizeOutput(xL, outputGain, headroomGain, softClipDrive);
      outR = finalizeOutput(xR, outputGain, headroomGain, softClipDrive);
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

    if (mode == MODE_ENCODE) processEncode(xL, xR, outL, outR);
    else processDecode(xL, xR, outL, outR);
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
unsigned long autoStateEnterMs = 0;
unsigned long lastAutoMeasureMs = 0;
PresetId currentPreset = PRESET_UNIVERSAL;
AutoCalState autoCalState = AUTO_IDLE;
bool autoCalValid = false;
float autoCalReferenceDb = OCXProfile::kReferenceDb;
float autoCalOutputTrimDb = OCXProfile::kOutputTrimDb;
float autoCalHeadroomDb = OCXProfile::kHeadroomDb;
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
static constexpr unsigned long kAutoFreshWindowMs = 450UL;
static constexpr unsigned long kAutoWarmupMs = 1200UL;

struct PersistSettings {
  uint32_t magic;
  uint16_t version;
  uint8_t mode;
  uint8_t presetId;
  uint8_t autoCalValid;
  uint8_t reserved;
  float autoCalReferenceDb;
  float autoCalOutputTrimDb;
  float autoCalHeadroomDb;
  uint32_t checksum;
};
static constexpr uint32_t kSettingsMagic = 0x4F435831u;
static constexpr uint16_t kSettingsVersion = 2;
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
    case PRESET_W1200: return "W1200";
    case PRESET_AUTO_CAL: return "AUTO_CAL";
    case PRESET_UNIVERSAL:
    default: return "UNIVERSAL";
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
  ocx.setOutputTrimDb(OCXProfile::kOutputTrimDb);
  ocx.setStrength(OCXProfile::kStrength);
  ocx.setReferenceDb(OCXProfile::kReferenceDb);
  ocx.setMaxBoostDb(OCXProfile::kMaxBoostDb);
  ocx.setMaxCutDb(OCXProfile::kMaxCutDb);
  ocx.setAttackMs(OCXProfile::kAttackMs);
  ocx.setReleaseMs(OCXProfile::kReleaseMs);
  ocx.setSidechainHpHz(OCXProfile::kSidechainHpHz);
  ocx.setSidechainShelfHz(OCXProfile::kSidechainShelfHz);
  ocx.setSidechainShelfDb(OCXProfile::kSidechainShelfDb);
  ocx.setDeemphHz(OCXProfile::kDeemphHz);
  ocx.setDeemphDb(OCXProfile::kDeemphDb);
  ocx.setSoftClipDrive(OCXProfile::kSoftClipDrive);
  ocx.setDcBlockHz(OCXProfile::kDcBlockHz);
  ocx.setHeadroomDb(OCXProfile::kHeadroomDb);
  if ((PresetId)id == PRESET_W1200) {
    ocx.setOutputTrimDb(OCXProfile::kW1200OutputTrimDb);
    ocx.setStrength(OCXProfile::kW1200Strength);
    ocx.setReferenceDb(OCXProfile::kW1200ReferenceDb);
    ocx.setHeadroomDb(OCXProfile::kW1200HeadroomDb);
  } else if ((PresetId)id == PRESET_AUTO_CAL && autoCalValid) {
    ocx.setReferenceDb(autoCalReferenceDb);
    ocx.setOutputTrimDb(autoCalOutputTrimDb);
    ocx.setHeadroomDb(autoCalHeadroomDb);
  }
  currentPreset = (PresetId)id;
}

void persistSettings() {
  PersistSettings s{};
  s.magic = kSettingsMagic;
  s.version = kSettingsVersion;
  s.mode = static_cast<uint8_t>(ocx.getMode());
  s.presetId = static_cast<uint8_t>(currentPreset);
  s.autoCalValid = autoCalValid ? 1 : 0;
  s.autoCalReferenceDb = autoCalReferenceDb;
  s.autoCalOutputTrimDb = autoCalOutputTrimDb;
  s.autoCalHeadroomDb = autoCalHeadroomDb;
  s.checksum = settingsChecksum(reinterpret_cast<const uint8_t *>(&s), sizeof(PersistSettings) - sizeof(uint32_t));
  EEPROM.put(kSettingsAddr, s);
}

void loadSettingsOrFactory() {
  PersistSettings s{};
  EEPROM.get(kSettingsAddr, s);
  const bool ok = s.magic == kSettingsMagic &&
                  s.version == kSettingsVersion &&
                  s.checksum == settingsChecksum(reinterpret_cast<const uint8_t *>(&s), sizeof(PersistSettings) - sizeof(uint32_t)) &&
                  s.mode <= 1 &&
                  s.presetId <= PRESET_AUTO_CAL;
  if (ok) {
    ocx.setMode(static_cast<AudioEffectOCXType2CodecStereo::Mode>(s.mode));
    currentPreset = static_cast<PresetId>(s.presetId);
    autoCalValid = s.autoCalValid != 0;
    autoCalReferenceDb = clampf(s.autoCalReferenceDb, -30.0f, -10.0f);
    autoCalOutputTrimDb = clampf(s.autoCalOutputTrimDb, -6.0f, 0.0f);
    autoCalHeadroomDb = clampf(s.autoCalHeadroomDb, 0.5f, 4.0f);
    applyDecoderPreset(currentPreset);
  } else {
    ocx.setMode(AudioEffectOCXType2CodecStereo::MODE_DECODE);
    currentPreset = PRESET_UNIVERSAL;
    autoCalValid = false;
    applyDecoderPreset(PRESET_UNIVERSAL);
  }
}

void factoryResetSettings() {
  PersistSettings s{};
  s.magic = kSettingsMagic;
  s.version = kSettingsVersion;
  s.mode = static_cast<uint8_t>(AudioEffectOCXType2CodecStereo::MODE_DECODE);
  s.presetId = static_cast<uint8_t>(PRESET_UNIVERSAL);
  s.autoCalValid = 0;
  s.autoCalReferenceDb = OCXProfile::kReferenceDb;
  s.autoCalOutputTrimDb = OCXProfile::kOutputTrimDb;
  s.autoCalHeadroomDb = OCXProfile::kHeadroomDb;
  s.checksum = settingsChecksum(reinterpret_cast<const uint8_t *>(&s), sizeof(PersistSettings) - sizeof(uint32_t));
  EEPROM.put(kSettingsAddr, s);
  ocx.setMode(AudioEffectOCXType2CodecStereo::MODE_DECODE);
  autoCalValid = false;
  autoCalState = AUTO_IDLE;
  currentPreset = PRESET_UNIVERSAL;
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
}

void computeAutoCalResult() {
  const float peakAvg = autoPeakAccum / fmaxf((float)autoBlocksValid, 1.0f);
  const float rmsAvg = autoRmsAccum / fmaxf((float)autoBlocksValid, 1.0f);
  const float toneAvg = autoToneAccum / fmaxf((float)autoBlocksValid, 1.0f);
  const float lrMismatch = fabsf(autoPeakL - autoPeakR) / fmaxf(fmaxf(autoPeakL, autoPeakR), 1.0e-6f);
  const float stabilityPenalty = fabsf((peakAvg - autoLastPeak)) + fabsf(toneAvg - autoLastToneMetric);
  const float predClipUniversal = fmaxf(0.0f, peakAvg * dbToLin(OCXProfile::kOutputTrimDb) * dbToLin(-OCXProfile::kHeadroomDb) - 0.98f);
  const float predClipW1200 = fmaxf(0.0f, peakAvg * dbToLin(OCXProfile::kW1200OutputTrimDb) * dbToLin(-OCXProfile::kW1200HeadroomDb) - 0.98f);
  const float nearLimitUniversal = fmaxf(0.0f, peakAvg - 0.86f);
  const float nearLimitW1200 = fmaxf(0.0f, peakAvg - 0.82f);
  const float scoreUniversal =
    predClipUniversal * 8.0f +
    nearLimitUniversal * 3.5f +
    fmaxf(0.0f, peakAvg - 0.62f) * 4.0f +
    fmaxf(0.0f, rmsAvg - 0.32f) * 3.0f +
    fabsf(rmsAvg - 0.30f) * 1.8f +
    fabsf(peakAvg - 0.52f) * 1.5f +
    lrMismatch * 1.5f +
    stabilityPenalty * 1.1f;
  const float scoreW1200 =
    predClipW1200 * 8.0f +
    nearLimitW1200 * 3.5f +
    fmaxf(0.0f, 0.18f - rmsAvg) * 1.5f +
    fabsf(rmsAvg - 0.24f) * 1.8f +
    fabsf(peakAvg - 0.42f) * 1.5f +
    lrMismatch * 1.5f +
    stabilityPenalty * 1.1f;
  const PresetId base = (scoreW1200 + 0.08f < scoreUniversal) ? PRESET_W1200 : PRESET_UNIVERSAL;
  const float baseReference = (base == PRESET_W1200) ? OCXProfile::kW1200ReferenceDb : OCXProfile::kReferenceDb;
  const float baseOutputTrim = (base == PRESET_W1200) ? OCXProfile::kW1200OutputTrimDb : OCXProfile::kOutputTrimDb;
  const float baseHeadroom = (base == PRESET_W1200) ? OCXProfile::kW1200HeadroomDb : OCXProfile::kHeadroomDb;
  autoCalReferenceDb = clampf(baseReference + clampf((0.28f - rmsAvg) * 8.0f, -2.0f, 2.0f), baseReference - 2.0f, baseReference + 2.0f);
  autoCalOutputTrimDb = clampf(baseOutputTrim + clampf((0.46f - peakAvg) * 6.0f, -1.5f, 1.5f), baseOutputTrim - 1.5f, baseOutputTrim + 1.5f);
  autoCalHeadroomDb = clampf(baseHeadroom + clampf((peakAvg - 0.58f) * 4.0f, -1.0f, 1.0f), baseHeadroom - 1.0f, baseHeadroom + 1.0f);
  autoCalScore = 100.0f - 100.0f * (fminf(scoreUniversal, scoreW1200) + fmaxf(0.0f, 0.70f - toneAvg));
  autoCalValid = true;
  currentPreset = PRESET_AUTO_CAL;
  applyDecoderPreset(PRESET_AUTO_CAL);
  autoCalState = AUTO_LOCKED;
  autoStateEnterMs = millis();
  autoRejectReason = "none";
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
    if (now - lastAutoMeasureMs >= 150UL) {
      lastAutoMeasureMs = now;
      ++autoBlocksSeen;
      if (gateOk) {
        ++autoBlocksValid;
        ++autoToneBlocks;
        ++autoCurrentSegmentValidBlocks;
        ++autoCurrentSegmentBlocks;
        autoSilenceBlocks = 0;
        autoPeakAccum += 0.5f * (autoPeakL + autoPeakR);
        autoRmsAccum += 0.5f * (autoRmsProxyL + autoRmsProxyR);
        autoToneAccum += fmaxf(autoToneMetricL, autoToneMetricR);
      } else {
        ++autoSilenceBlocks;
        if (autoCurrentSegmentBlocks > 0 && autoSilenceBlocks >= 4) {
          if (autoCurrentSegmentValidBlocks >= 120) ++autoToneSegments;
          autoCurrentSegmentBlocks = 0;
          autoCurrentSegmentValidBlocks = 0;
        }
      }
    }
    const bool enoughSegments = (autoToneSegments >= 2) || (autoToneSegments >= 1 && autoCurrentSegmentValidBlocks >= 150);
    const bool enoughDuration = (now - autoStateEnterMs) >= 70000UL;
    if (enoughSegments && autoToneBlocks >= 320 && autoBlocksSeen >= 360 && enoughDuration) {
      autoCalState = AUTO_COMPUTE;
      autoRejectReason = "none";
    } else if (now - autoStateEnterMs > 130000UL) {
      autoCalState = AUTO_FAILED;
      setAutoRejectReason("reject_unstable");
    }
  }

  if (autoCalState == AUTO_COMPUTE) computeAutoCalResult();
}

void printHelp() {
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  h  : help"));
  Serial.println(F("  p  : print status"));
  Serial.println(F("  m  : print compact telemetry status"));
  Serial.println(F("  n  : print signal diagnostics snapshot (input/output/gain activity)"));
  Serial.println(F("  N  : reset signal diagnostics counters"));
  Serial.println(F("  x  : clear clip flags"));
  Serial.println(F("  v  : print NEW clip counts since last v/m/p call"));
  Serial.println(F("  X  : clear clip flags + runtime counters + signal diagnostics + usage maxima"));
  Serial.println(F("  B  : reset DSP state"));
  Serial.println(F("  b  : toggle bypass"));
  Serial.println(F("  M  : print codec mode"));
  Serial.println(F("  j  : print preset"));
  Serial.println(F("  u  : select preset UNIVERSAL"));
  Serial.println(F("  2  : select preset W1200"));
  Serial.println(F("  l  : start AUTO_CAL (1-kHz measurement cassette only)"));
  Serial.println(F("  J  : print AUTO_CAL state"));
  Serial.println(F("  K  : print AUTO_CAL raw telemetry/reject reasons"));
  Serial.println(F("  L  : print locked AUTO_CAL values"));
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
  Serial.println(F("  Detector: stereo-linked peak detector (shared gain on L/R)."));
  Serial.println(F("  Snapshot includes clamp-hit/near-limit stats for maxCut/maxBoost interpretation."));
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
  Serial.print(F(" auto=")); Serial.print(autoCalStateLabel(autoCalState));
  Serial.print(F(" autoValid=")); Serial.print(autoCalValid ? F("YES") : F("NO"));
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
  Serial.print(F("AUTO_CAL state: ")); Serial.println(autoCalStateLabel(autoCalState));
  Serial.print(F("AUTO_CAL values valid: ")); Serial.println(autoCalValid ? F("YES") : F("NO"));
  Serial.print(F("Bypass: ")); Serial.println(ocx.getBypass() ? F("ON") : F("OFF"));
  Serial.println(F("Bypass mode keeps output protection (headroom + soft clip), not a hard relay bypass."));
  Serial.print(F("Input trim: ")); Serial.print(ocx.getInputTrimDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Output trim: ")); Serial.print(ocx.getOutputTrimDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Strength: ")); Serial.println(ocx.getStrength(), 3);
  Serial.print(F("Reference: ")); Serial.print(ocx.getReferenceDb(), 2); Serial.println(F(" dB"));
  Serial.println(F("Detector: stereo-linked peak (max of L/R sidechain power)."));
  Serial.print(F("Attack: ")); Serial.print(ocx.getAttackMs(), 2); Serial.println(F(" ms"));
  Serial.print(F("Release: ")); Serial.print(ocx.getReleaseMs(), 2); Serial.println(F(" ms"));
  Serial.print(F("Sidechain HP: ")); Serial.print(ocx.getSidechainHpHz(), 1); Serial.println(F(" Hz"));
  Serial.print(F("Sidechain shelf: ")); Serial.print(ocx.getSidechainShelfDb(), 1); Serial.print(F(" dB @ ")); Serial.print(ocx.getSidechainShelfHz(), 0); Serial.println(F(" Hz"));
  Serial.print(F("De-emphasis shelf: ")); Serial.print(ocx.getDeemphDb(), 2); Serial.print(F(" dB @ ")); Serial.print(ocx.getDeemphHz(), 0); Serial.println(F(" Hz"));
  Serial.print(F("Headroom: ")); Serial.print(ocx.getHeadroomDb(), 2); Serial.println(F(" dB"));
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
  Serial.print(F("Input clip NEW since last report: ")); Serial.println(delta.inputNew);
  Serial.print(F("Output clip NEW since last report: ")); Serial.println(delta.outputNew);
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
  Serial.print(F(" score=")); Serial.println(autoCalScore, 2);
}

void printAutoCalLockedValues() {
  Serial.print(F("[AUTO_CAL_LOCKED] reference_db=")); Serial.print(autoCalReferenceDb, 2);
  Serial.print(F(" output_trim_db=")); Serial.print(autoCalOutputTrimDb, 2);
  Serial.print(F(" headroom_db=")); Serial.println(autoCalHeadroomDb, 2);
}

void handleSerial() {
  while (Serial.available()) {
    const char c = Serial.read();
    switch (c) {
      case 'h': printHelp(); break;
      case 'p': printStatus(); break;
      case 'm': printCompactTelemetryLine(); break;
      case 'n': printSignalDiagnosticsSnapshot(); break;
      case 'N': ocx.resetSignalDiagnostics(); break;
      case 'x': ocx.clearClipFlags(); break;
      case 'v': {
        const ClipDelta delta = ocx.consumeClipDelta();
        Serial.print(F("[CLIPΔ] inNew=")); Serial.print(delta.inputNew);
        Serial.print(F(" outNew=")); Serial.println(delta.outputNew);
      } break;
      case 'X': ocx.clearClipFlags(); ocx.clearRuntimeCounters(); ocx.resetSignalDiagnostics(); AudioProcessorUsageMaxReset(); AudioMemoryUsageMaxReset(); break;
      case 'B': ocx.resetState(); break;
      case 'b': ocx.setBypass(!ocx.getBypass()); break;
      case 'M': Serial.println(ocx.getMode() == AudioEffectOCXType2CodecStereo::MODE_ENCODE ? F("ENCODE") : F("DECODE")); break;
      case 'j': Serial.println(presetLabel(currentPreset)); break;
      case 'u': currentPreset = PRESET_UNIVERSAL; applyDecoderPreset(currentPreset); autoCalState = AUTO_IDLE; break;
      case '2': currentPreset = PRESET_W1200; applyDecoderPreset(currentPreset); autoCalState = AUTO_IDLE; break;
      case 'l': beginAutoCal(); break;
      case 'J': printAutoCalStatus(); break;
      case 'K': printAutoCalRawTelemetry(); break;
      case 'L': printAutoCalLockedValues(); break;
      case '>': ocx.setMode(AudioEffectOCXType2CodecStereo::MODE_DECODE); break;
      case '<': ocx.setMode(AudioEffectOCXType2CodecStereo::MODE_ENCODE); break;
      case 'P': persistSettings(); break;
      case '!': factoryResetSettings(); break;
      case '0': applyFactoryPreset(); break;
      case 'i': ocx.setInputTrimDb(ocx.getInputTrimDb() - 0.5f); break;
      case 'I': ocx.setInputTrimDb(ocx.getInputTrimDb() + 0.5f); break;
      case 'o': ocx.setOutputTrimDb(ocx.getOutputTrimDb() - 0.5f); break;
      case 'O': ocx.setOutputTrimDb(ocx.getOutputTrimDb() + 0.5f); break;
      case 's': ocx.setStrength(ocx.getStrength() - 0.05f); break;
      case 'S': ocx.setStrength(ocx.getStrength() + 0.05f); break;
      case 'f': ocx.setReferenceDb(ocx.getReferenceDb() - 1.0f); break;
      case 'F': ocx.setReferenceDb(ocx.getReferenceDb() + 1.0f); break;
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
      case 'g': ocx.setHeadroomDb(ocx.getHeadroomDb() - 0.5f); break;
      case 'G': ocx.setHeadroomDb(ocx.getHeadroomDb() + 0.5f); break;
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
  Serial.println(F("OCX Type 2 decoder firmware ready (preset system: UNIVERSAL/W1200/AUTO_CAL)."));
  Serial.print(F("Runtime sample-rate (AUDIO_SAMPLE_RATE_EXACT): "));
  Serial.println((double)AUDIO_SAMPLE_RATE_EXACT, 4);
  Serial.println(F("Profile/sample-method baseline remains 44100 Hz nominal for simulator/harness comparability."));
  Serial.println(F("Offline build validated; final analog validation still requires physical Teensy 4.1 + SGTL5000 hardware."));
  printStatus();
  printHelp();
}

void loop() {
  handleSerial();
  updateAutoCal();
  const unsigned long now = millis();
  if (now - lastStatusMs > 2000) {
    lastStatusMs = now;
    if (ocx.hasInputClip()) Serial.println(F("[WARN] Input clipped at least once. Lower source level or reduce input trim."));
    if (ocx.hasOutputClip()) Serial.println(F("[WARN] Output clipped at least once. Lower output trim or increase headroom."));
    if (ocx.getAllocFailCount() > 0) Serial.println(F("[WARN] Audio block allocation failure observed."));
  }
}
