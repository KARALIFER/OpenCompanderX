/*
  OCX Type 2 cassette decoder - firmware for Teensy 4.1 + Teensy Audio Adaptor Rev D/D2

  Target hardware:
  - Teensy 4.1 (USB Serial)
  - PJRC Teensy Audio Adaptor Board Rev D or D2 (SGTL5000)
  - Stereo LINE IN wired to the shield's line input header
  - Stereo LINE OUT wired to the shield's line output header
  - Optional direct monitoring with the shield headphone jack

  Goals of this build:
  - Stable real-time stereo processing on the final target hardware
  - Sensible default parameters so you can start listening immediately
  - Minimal run-time tuning via USB serial, not endless reflashing
  - Built-in 1 kHz calibration tone for basic level checks

  Important honesty note:
  - This is a practical Type 2 companded-cassette decoder firmware and a solid hardware target implementation.
  - It is NOT claimed to be a bit-exact clone of any vintage hardware unit.
  - Final best settings still depend on real source material and line levels.

  Arduino IDE settings:
  - Board: Teensy 4.1
  - USB Type: Serial
  - CPU Speed: 600 MHz
  - Optimize: Faster or Fastest
*/

#include <Arduino.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SerialFlash.h>
#include <math.h>

static inline float clampf(float x, float lo, float hi) {
  return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static inline float dbToLin(float db) {
  return powf(10.0f, db / 20.0f);
}

static inline float linToDb(float x) {
  return 20.0f * log10f(fmaxf(x, 1.0e-12f));
}

static inline float softClip(float x) {
  const float drive = 1.10f;
  return tanhf(drive * x) / tanhf(drive);
}

struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
  float a1 = 0.0f, a2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f;

  inline float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  inline void reset() {
    z1 = 0.0f;
    z2 = 0.0f;
  }
};

static void designHighpass(Biquad &f, float fs, float hz, float q = 0.7071f) {
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
  AudioEffectOCXType2DecodeStereo() : AudioStream(2, inputQueueArray) {
    recalcAll();
  }

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

  bool hasInputClip() const          { return inputClipFlag; }
  bool hasOutputClip() const         { return outputClipFlag; }
  void clearClipFlags()              { inputClipFlag = false; outputClipFlag = false; }

private:
  audio_block_t *inputQueueArray[2];
  static constexpr float fs = 44100.0f;

  bool  bypass = false;
  bool  inputClipFlag = false;
  bool  outputClipFlag = false;

  float inputTrimDb = 0.0f;
  float outputTrimDb = 0.0f;
  float inputGain = 1.0f;
  float outputGain = 1.0f;

  float strength = 0.78f;
  float referenceDb = -18.0f;
  float maxBoostDb = 10.0f;
  float maxCutDb = 24.0f;

  float sidechainHpHz = 65.0f;
  float sidechainShelfHz = 2500.0f;
  float sidechainShelfDb = 20.0f;
  Biquad scHP[2];
  Biquad scShelf[2];

  float attackMs = 4.0f;
  float releaseMs = 120.0f;
  float attackCoeff = 0.0f;
  float releaseCoeff = 0.0f;
  float env2[2] = {1.0e-9f, 1.0e-9f};

  float deemphHz = 750.0f;
  float deemphDb = -12.0f;
  Biquad deemph[2];

  void recalcAll() {
    setInputTrimDb(inputTrimDb);
    setOutputTrimDb(outputTrimDb);
    recalcSidechainFilters();
    recalcDetector();
    recalcDeemphFilter();
  }

  void recalcDetector() {
    attackCoeff  = expf(-1.0f / (fs * attackMs  * 0.001f));
    releaseCoeff = expf(-1.0f / (fs * releaseMs * 0.001f));
  }

  void recalcSidechainFilters() {
    for (int ch = 0; ch < 2; ++ch) {
      designHighpass(scHP[ch], fs, sidechainHpHz, 0.7071f);
      designHighShelf(scShelf[ch], fs, sidechainShelfHz, sidechainShelfDb, 0.8f);
    }
  }

  void recalcDeemphFilter() {
    for (int ch = 0; ch < 2; ++ch) {
      designHighShelf(deemph[ch], fs, deemphHz, deemphDb, 0.8f);
    }
  }

  inline float processOne(int ch, float x) {
    x *= inputGain;
    if (fabsf(x) > 0.98f) inputClipFlag = true;

    if (bypass) {
      float y = clampf(softClip(x * outputGain), -1.0f, 1.0f);
      if (fabsf(y) > 0.98f) outputClipFlag = true;
      return y;
    }

    float sc = scHP[ch].process(x);
    sc = scShelf[ch].process(sc);

    float p = sc * sc;
    float coeff = (p > env2[ch]) ? attackCoeff : releaseCoeff;
    env2[ch] = coeff * env2[ch] + (1.0f - coeff) * p;
    float env = sqrtf(env2[ch] + 1.0e-12f);

    float levelDb = linToDb(env);
    float gainDb = (levelDb - referenceDb) * strength;
    gainDb = clampf(gainDb, -maxCutDb, maxBoostDb);
    float gainLin = dbToLin(gainDb);

    float y = x * gainLin;
    y = deemph[ch].process(y);

    y *= outputGain;
    y = clampf(softClip(y), -1.0f, 1.0f);
    if (fabsf(y) > 0.98f) outputClipFlag = true;
    return y;
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
    float xl = (float)inL->data[i] / 32768.0f;
    float xr = (float)inR->data[i] / 32768.0f;

    float yl = processOne(0, xl);
    float yr = processOne(1, xr);

    int32_t sl = (int32_t)(yl * 32767.0f);
    int32_t sr = (int32_t)(yr * 32767.0f);

    if (sl > 32767) sl = 32767;
    if (sl < -32768) sl = -32768;
    if (sr > 32767) sr = 32767;
    if (sr < -32768) sr = -32768;

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
float calToneDb = -18.0f;
float calToneHz = 1000.0f;
unsigned long lastStatusMs = 0;

void updateTone() {
  calToneDb = clampf(calToneDb, -60.0f, -1.0f);
  if (calToneEnabled) {
    calTone.frequency(calToneHz);
    calTone.amplitude(clampf(dbToLin(calToneDb), 0.0f, 0.89f));
    mixL.gain(1, 1.0f);
    mixR.gain(1, 1.0f);
  } else {
    calTone.amplitude(0.0f);
    mixL.gain(1, 0.0f);
    mixR.gain(1, 0.0f);
  }
}

void applyFactoryPreset() {
  ocx.setBypass(false);
  ocx.setInputTrimDb(0.0f);
  ocx.setOutputTrimDb(0.0f);
  ocx.setStrength(0.78f);
  ocx.setReferenceDb(-18.0f);
  ocx.setMaxBoostDb(10.0f);
  ocx.setMaxCutDb(24.0f);
  ocx.setAttackMs(4.0f);
  ocx.setReleaseMs(120.0f);
  ocx.setSidechainHpHz(65.0f);
  ocx.setSidechainShelfHz(2500.0f);
  ocx.setSidechainShelfDb(20.0f);
  ocx.setDeemphHz(750.0f);
  ocx.setDeemphDb(-12.0f);
}

void printHelp() {
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  h  : help"));
  Serial.println(F("  p  : print status"));
  Serial.println(F("  x  : clear clip flags"));
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
  Serial.print(F("Tone: ")); Serial.print(calToneEnabled ? F("ON") : F("OFF"));
  Serial.print(F("  ")); Serial.print(calToneHz, 1); Serial.print(F(" Hz @ ")); Serial.print(calToneDb, 1); Serial.println(F(" dBFS"));
  Serial.print(F("Input clip seen: ")); Serial.println(ocx.hasInputClip() ? F("YES") : F("NO"));
  Serial.print(F("Output clip seen: ")); Serial.println(ocx.hasOutputClip() ? F("YES") : F("NO"));
  Serial.println();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'h': printHelp(); break;
      case 'p': printStatus(); break;
      case 'x': ocx.clearClipFlags(); break;
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

  AudioMemory(48);

  mixL.gain(0, 1.0f);
  mixR.gain(0, 1.0f);
  mixL.gain(1, 0.0f);
  mixR.gain(1, 0.0f);
  calTone.amplitude(0.0f);

  codec.enable();
  codec.inputSelect(AUDIO_INPUT_LINEIN);
  codec.lineInLevel(5);
  codec.lineOutLevel(29);
  codec.headphoneSelect(AUDIO_HEADPHONE_DAC);
  codec.volume(0.45f);

  applyFactoryPreset();
  updateTone();

  Serial.println();
  Serial.println(F("OCX Type 2 decoder firmware ready (single universal playback profile)."));
  printStatus();
  printHelp();
}

void loop() {
  handleSerial();

  unsigned long now = millis();
  if (now - lastStatusMs > 2000) {
    lastStatusMs = now;
    if (ocx.hasInputClip()) {
      Serial.println(F("[WARN] Input clipped at least once. Lower source level or reduce input trim."));
    }
    if (ocx.hasOutputClip()) {
      Serial.println(F("[WARN] Output clipped at least once. Lower output trim or use bypass for comparison."));
    }
  }
}
