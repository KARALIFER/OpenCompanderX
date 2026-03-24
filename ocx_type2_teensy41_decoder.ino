/*
  OCX Type 2 cassette decoder - firmware for Teensy 4.1 + Teensy Audio Adaptor Rev D/D2

  Practical scope:
  - Decoder-only playback path for analog line input to analog line/headphone output
  - One conservative universal playback profile for TEAC W-1200 line-out and portable headphone sources
  - Real-time-safe sample loop: no dynamic allocation in update(), no Serial I/O in update(), no heap DSP objects
  - Honest limit: compile/build can be validated offline, but full SGTL5000 analog behavior still requires physical hardware
*/

#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SerialFlash.h>
#include <math.h>

namespace OCXProfile {
static constexpr float kFs = AUDIO_SAMPLE_RATE_EXACT;
static constexpr int kAudioMemoryBlocks = 64;
static constexpr int kLineInLevel = 5;
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
static constexpr float kToneHz = 1000.0f;
static constexpr float kToneDb = -18.0f;
}

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

class AudioEffectOCXType2DecodeStereo : public AudioStream {
public:
  AudioEffectOCXType2DecodeStereo() : AudioStream(2, inputQueueArray) { recalcAll(); resetState(); }
  virtual void update(void);

  void setBypass(bool v)             { bypass = v; }
  bool getBypass() const             { return bypass; }
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
  void clearClipFlags()              { inputClipFlag = false; outputClipFlag = false; }
  void resetState() {
    clearClipFlags();
    for (int ch = 0; ch < 2; ++ch) {
      scHP[ch].reset();
      scShelf[ch].reset();
      deemph[ch].reset();
      dcBlock[ch].reset();
      env2[ch] = 1.0e-9f;
    }
  }

private:
  audio_block_t *inputQueueArray[2];
  bool  bypass = false;
  bool  inputClipFlag = false;
  bool  outputClipFlag = false;

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

  float attackMs = 3.5f;
  float releaseMs = 140.0f;
  float attackCoeff = 0.0f;
  float releaseCoeff = 0.0f;
  float env2[2] = {1.0e-9f, 1.0e-9f};

  float deemphHz = 1850.0f;
  float deemphDb = -6.0f;
  Biquad deemph[2];

  float softClipDrive = 1.08f;
  float dcBlockHz = 12.0f;
  OnePoleHP dcBlock[2];

  void recalcAll() {
    setInputTrimDb(inputTrimDb);
    setOutputTrimDb(outputTrimDb);
    setHeadroomDb(headroomDb);
    recalcSidechainFilters();
    recalcDetector();
    recalcDeemphFilter();
    recalcDcBlockers();
  }

  void recalcDetector() {
    attackCoeff  = expf(-1.0f / (OCXProfile::kFs * attackMs  * 0.001f));
    releaseCoeff = expf(-1.0f / (OCXProfile::kFs * releaseMs * 0.001f));
  }

  void recalcSidechainFilters() {
    for (int ch = 0; ch < 2; ++ch) {
      designHighpass(scHP[ch], OCXProfile::kFs, sidechainHpHz, 0.7071f);
      designHighShelf(scShelf[ch], OCXProfile::kFs, sidechainShelfHz, sidechainShelfDb, 0.8f);
    }
  }

  void recalcDeemphFilter() {
    for (int ch = 0; ch < 2; ++ch) designHighShelf(deemph[ch], OCXProfile::kFs, deemphHz, deemphDb, 0.8f);
  }

  void recalcDcBlockers() {
    for (int ch = 0; ch < 2; ++ch) dcBlock[ch].design(OCXProfile::kFs, dcBlockHz);
  }

  inline float processOne(int ch, float x) {
    x = dcBlock[ch].process(sanitizef(x) * inputGain);
    if (fabsf(x) > 0.98f) inputClipFlag = true;

    if (bypass) {
      float y = clampf(softClip(x * outputGain * headroomGain, softClipDrive), -1.0f, 1.0f);
      if (fabsf(y) > 0.98f) outputClipFlag = true;
      return y;
    }

    float sc = scHP[ch].process(x);
    sc = scShelf[ch].process(sc);
    const float p = sc * sc;
    const float coeff = (p > env2[ch]) ? attackCoeff : releaseCoeff;
    env2[ch] = sanitizef(coeff * env2[ch] + (1.0f - coeff) * p);
    const float env = sqrtf(env2[ch] + 1.0e-12f);
    float gainDb = (linToDb(env) - referenceDb) * strength;
    gainDb = clampf(gainDb, -maxCutDb, maxBoostDb);

    float y = x * dbToLin(gainDb);
    y = deemph[ch].process(y);
    y *= outputGain * headroomGain;
    y = clampf(softClip(y, softClipDrive), -1.0f, 1.0f);
    if (fabsf(y) > 0.98f) outputClipFlag = true;
    return sanitizef(y);
  }
};

void AudioEffectOCXType2DecodeStereo::update(void) {
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
    if (outL) release(outL);
    if (outR) release(outR);
    release(inL);
    release(inR);
    return;
  }

  for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
    const float xl = (float)inL->data[i] / 32768.0f;
    const float xr = (float)inR->data[i] / 32768.0f;
    const float yl = processOne(0, xl);
    const float yr = processOne(1, xr);

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
AudioEffectOCXType2DecodeStereo  ocx;
AudioSynthWaveformSine           calTone;
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

bool  calToneEnabled = false;
float calToneDb = OCXProfile::kToneDb;
float calToneHz = OCXProfile::kToneHz;
unsigned long lastStatusMs = 0;

void updateTone() {
  calToneDb = clampf(calToneDb, -60.0f, -1.0f);
  calTone.frequency(calToneHz);
  calTone.amplitude(calToneEnabled ? clampf(dbToLin(calToneDb), 0.0f, 0.89f) : 0.0f);
  mixL.gain(1, calToneEnabled ? 1.0f : 0.0f);
  mixR.gain(1, calToneEnabled ? 1.0f : 0.0f);
}

void applyFactoryPreset() {
  ocx.setBypass(false);
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
  ocx.resetState();
}

void printHelp() {
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  h  : help"));
  Serial.println(F("  p  : print status"));
  Serial.println(F("  x  : clear clip flags"));
  Serial.println(F("  B  : reset DSP state"));
  Serial.println(F("  b  : toggle bypass"));
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
  Serial.println(F("  t  : toggle 1 kHz calibration tone"));
  Serial.println(F("  z/Z: tone level -/+ 1 dB"));
  Serial.println();
}

void printStatus() {
  Serial.println();
  Serial.println(F("==== OCX TYPE 2 STATUS ===="));
  Serial.print(F("Bypass: ")); Serial.println(ocx.getBypass() ? F("ON") : F("OFF"));
  Serial.print(F("Input trim: ")); Serial.print(ocx.getInputTrimDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Output trim: ")); Serial.print(ocx.getOutputTrimDb(), 2); Serial.println(F(" dB"));
  Serial.print(F("Strength: ")); Serial.println(ocx.getStrength(), 3);
  Serial.print(F("Reference: ")); Serial.print(ocx.getReferenceDb(), 2); Serial.println(F(" dB"));
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
  Serial.print(F("Input clip seen: ")); Serial.println(ocx.hasInputClip() ? F("YES") : F("NO"));
  Serial.print(F("Output clip seen: ")); Serial.println(ocx.hasOutputClip() ? F("YES") : F("NO"));
  Serial.println();
}

void handleSerial() {
  while (Serial.available()) {
    const char c = Serial.read();
    switch (c) {
      case 'h': printHelp(); break;
      case 'p': printStatus(); break;
      case 'x': ocx.clearClipFlags(); break;
      case 'B': ocx.resetState(); break;
      case 'b': ocx.setBypass(!ocx.getBypass()); break;
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

  codec.enable();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(OCXProfile::kLineInLevel);
  codec.lineOutLevel(OCXProfile::kLineOutLevel);
  codec.headphoneSelect(AUDIO_HEADPHONE_DAC);
  codec.volume(OCXProfile::kHeadphoneVolume);

  applyFactoryPreset();
  updateTone();

  Serial.println();
  Serial.println(F("OCX Type 2 decoder firmware ready (single universal playback profile)."));
  Serial.println(F("Offline build validated; final analog validation still requires physical Teensy 4.1 + SGTL5000 hardware."));
  printStatus();
  printHelp();
}

void loop() {
  handleSerial();
  const unsigned long now = millis();
  if (now - lastStatusMs > 2000) {
    lastStatusMs = now;
    if (ocx.hasInputClip()) Serial.println(F("[WARN] Input clipped at least once. Lower source level or reduce input trim."));
    if (ocx.hasOutputClip()) Serial.println(F("[WARN] Output clipped at least once. Lower output trim or increase headroom."));
  }
}
