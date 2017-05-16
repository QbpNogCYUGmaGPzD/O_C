// Copyright (c) 2016 Patrick Dowling, 2017 Max Stadler & Tim Churches
//
// Author: Patrick Dowling (pld@gurkenkiste.com)
// Enhancements: Max Stadler and Tim Churches
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Very simple "reference" voltage app (not so simple any more...)

#include "OC_apps.h"
#include "OC_menus.h"
#include "OC_strings.h"
#include "util/util_settings.h"
#include "OC_autotuner.h"
#include "src/drivers/FreqMeasure/OC_FreqMeasure.h"

static constexpr double kAaboveMidCtoC0 = 0.03716272234383494188492;
#define FREQ_MEASURE_TIMEOUT 512
#define ERROR_TIMEOUT (FREQ_MEASURE_TIMEOUT << 0x4)
#define MAX_NUM_PASSES 1500
#define CONVERGE_PASSES 5

const uint8_t NUM_REF_CHANNELS = DAC_CHANNEL_LAST;

enum ReferenceSetting {
  REF_SETTING_OCTAVE,
  REF_SETTING_SEMI,
  REF_SETTING_RANGE,
  REF_SETTING_RATE,
  REF_SETTING_NOTES_OR_BPM,
  REF_SETTING_A_ABOVE_MID_C_INTEGER,
  REF_SETTING_A_ABOVE_MID_C_MANTISSA,
  REF_SETTING_PPQN,
  REF_SETTING_AUTOTUNE,
  REF_SETTING_DUMMY,
  #ifdef BUCHLA_SUPPORT
    REF_SETTING_VOLTAGE_SCALING,
  #endif 
  REF_SETTING_LAST
};

enum ChannelPpqn {
  CHANNEL_PPQN_1,
  CHANNEL_PPQN_2,
  CHANNEL_PPQN_4,
  CHANNEL_PPQN_8,
  CHANNEL_PPQN_16,
  CHANNEL_PPQN_24,
  CHANNEL_PPQN_32,
  CHANNEL_PPQN_48,
  CHANNEL_PPQN_64,
  CHANNEL_PPQN_96,
  CHANNEL_PPQN_LAST
};

enum AUTO_CALIBRATION_STEP {
  DAC_VOLT_0_ARM,
  DAC_VOLT_0_BASELINE,
  DAC_VOLT_3m, 
  DAC_VOLT_2m, 
  DAC_VOLT_1m, 
  DAC_VOLT_0, 
  DAC_VOLT_1, 
  DAC_VOLT_2, 
  DAC_VOLT_3, 
  DAC_VOLT_4, 
  DAC_VOLT_5, 
  DAC_VOLT_6,
  AUTO_CALIBRATION_STEP_LAST
};

class ReferenceChannel : public settings::SettingsBase<ReferenceChannel, REF_SETTING_LAST> {
public:

  static constexpr size_t kHistoryDepth = 10;
  
  void Init(DAC_CHANNEL dac_channel) {
    InitDefaults();

    rate_phase_ = 0;
    mod_offset_ = 0;
    last_pitch_ = 0;
    autotuner_ = false;
    autotuner_step_ = DAC_VOLT_0_ARM;
    dac_channel_ = dac_channel;
    auto_DAC_offset_error_ = 0;
    auto_frequency_ = 0;
    auto_last_frequency_ = 0;
    auto_freq_sum_ = 0;
    auto_freq_count_ = 0;
    auto_ready_ = 0;
    ticks_since_last_freq_ = 0;
    auto_next_step_ = false;
    autotune_completed_ = false;
    F_correction_factor_ = 0xFF;
    correction_direction_ = false;
    correction_cnt_positive_ = 0x0;
    correction_cnt_negative_ = 0x0;
    reset_calibration_data();
    update_enabled_settings();
    history_[0].Init(0x0);
  }

  int get_octave() const {
    return values_[REF_SETTING_OCTAVE];
  }

  int get_channel() const {
    return dac_channel_;
  }

  int32_t get_semitone() const {
    return values_[REF_SETTING_SEMI];
  }

  int get_range() const {
    return values_[REF_SETTING_RANGE];
  }

  uint32_t get_rate() const {
    return values_[REF_SETTING_RATE];
  }

  uint8_t get_notes_or_bpm() const {
    return values_[REF_SETTING_NOTES_OR_BPM];
  }

  double get_a_above_mid_c() const {
    double mantissa_divisor = 100.0;
    return static_cast<double>(values_[REF_SETTING_A_ABOVE_MID_C_INTEGER]) + (static_cast<double>(values_[REF_SETTING_A_ABOVE_MID_C_MANTISSA])/mantissa_divisor) ;
  }

  uint8_t get_a_above_mid_c_mantissa() const {
    return values_[REF_SETTING_A_ABOVE_MID_C_MANTISSA];
  }

  ChannelPpqn get_channel_ppqn() const {
    return static_cast<ChannelPpqn>(values_[REF_SETTING_PPQN]);
  }

  uint8_t autotuner_active() {
    return (autotuner_ && autotuner_step_) ? (dac_channel_ + 0x1) : 0x0;
  }

  bool autotuner_completed() {
    return autotune_completed_;
  }

  void autotuner_reset_completed() {
    autotune_completed_ = false;
  }

  bool autotuner_error() {
    return auto_error_;
  }

  uint8_t get_octave_cnt() {
    return octaves_cnt_ + 0x1;
  }

  uint8_t auto_tune_step() {
    return autotuner_step_;
  }

  void autotuner_arm(uint8_t _status) {
    reset_autotuner();
    autotuner_ = _status ? true : false;
  }
  
  void autotuner_run() {     
    autotuner_step_ = autotuner_ ? DAC_VOLT_0_BASELINE : DAC_VOLT_0_ARM;
    if (autotuner_step_ == DAC_VOLT_0_BASELINE)
    // we start, so reset data to defaults:
      OC::DAC::set_default_channel_calibration_data(dac_channel_);
  }

  void auto_reset_step() {
    auto_num_passes_ = 0x0;
    auto_DAC_offset_error_ = 0x0;
    correction_direction_ = false;
    correction_cnt_positive_ = correction_cnt_negative_ = 0x0;
    F_correction_factor_ = 0xFF;
    auto_ready_ = false;
  }

  void reset_autotuner() {
    ticks_since_last_freq_ = 0x0;
    auto_frequency_ = 0x0;
    auto_last_frequency_ = 0x0;
    auto_error_ = 0x0;
    auto_ready_ = 0x0;
    autotuner_ = 0x0;
    autotuner_step_ = 0x0;
    F_correction_factor_ = 0xFF;
    correction_direction_ = false;
    correction_cnt_positive_ = 0x0;
    correction_cnt_negative_ = 0x0;
    octaves_cnt_ = 0x0;
    auto_num_passes_ = 0x0;
    auto_DAC_offset_error_ = 0x0;
    autotune_completed_ = 0x0;
    reset_calibration_data();
  }

  float get_auto_frequency() {
    return auto_frequency_;
  }

  uint8_t _ready() {
     return auto_ready_;
  }

  void reset_calibration_data() {
    
    for (int i = 0; i <= OCTAVES; i++) {
      auto_calibration_data_[i] = 0;
      auto_target_frequencies_[i] = 0.0f;
    }
  }

  uint8_t data_available() {
    return OC::DAC::calibration_data_used(dac_channel_);
  }

  void use_default() {
    OC::DAC::set_default_channel_calibration_data(dac_channel_);
  }

  void use_auto_calibration() {
    OC::DAC::set_auto_channel_calibration_data(dac_channel_);
  }
  
  bool auto_frequency() {

    bool _f_result = false;

    if (ticks_since_last_freq_ > ERROR_TIMEOUT) {
      auto_error_ = true;
    }
    
    if (FreqMeasure.available()) {
      
      auto_freq_sum_ = auto_freq_sum_ + FreqMeasure.read();
      auto_freq_count_ = auto_freq_count_ + 1;

      // take more time as we're converging toward the target frequency
      uint32_t _wait = (F_correction_factor_ == 0x1) ? (FREQ_MEASURE_TIMEOUT << 2) :  (FREQ_MEASURE_TIMEOUT >> 2);
  
      if (ticks_since_last_freq_ > _wait) {
        
        auto_frequency_ = FreqMeasure.countToFrequency(auto_freq_sum_ / auto_freq_count_);
        history_[0].Push(auto_frequency_);
        auto_freq_sum_ = 0;
        auto_ready_ = true;
        auto_freq_count_ = 0;
        _f_result = true;
        ticks_since_last_freq_ = 0x0;
        OC::ui._Poke();
        for (auto &sh : history_)
          sh.Update();
      }
    }
    return _f_result;
  }

  void measure_frequency_and_calc_error() {

    switch(autotuner_step_) {

      case DAC_VOLT_0_ARM:
      // do nothing
      break;
      case DAC_VOLT_0_BASELINE:
      // 0V baseline / calibration point: in this case, we don't correct.
      {
        bool _update = auto_frequency();
        if (_update && auto_num_passes_ > kHistoryDepth) { 
          
          auto_last_frequency_ = auto_frequency_;
          float history[kHistoryDepth]; 
          float average = 0.0f;
          // average
          history_->Read(history);
          for (uint8_t i = 0; i < kHistoryDepth; i++)
            average += history[i];
          // ... and derive target frequencies
          float target_frequency = ((auto_frequency_ + average) / (float)(kHistoryDepth + 1)); // 0V

          switch(get_voltage_scaling()){
          /* can't use pow (busts the available memory at this point), so we unroll ... */
            case 1: // 1.2V/octave
              auto_target_frequencies_[0]  =  target_frequency * 0.1767766952966368931843f;  // -3V = 2**(-3.0/1.2)
              auto_target_frequencies_[1]  =  target_frequency * 0.3149802624737182976666f;  // -2V = 2**(-2.0/1.2)
              auto_target_frequencies_[2]  =  target_frequency * 0.5612310241546865086093f;  // -1V = 2**(-1.0/1.2)
              auto_target_frequencies_[3]  =  target_frequency * 1.0f;                       // 0V = 2**(0.0/1.2)
              auto_target_frequencies_[4]  =  target_frequency * 1.7817974362806785482150f;  // +1V = 2**(1.0/1.2)
              auto_target_frequencies_[5]  =  target_frequency * 3.1748021039363991668836f;  // +2V = 2**(2.0/1.2)
              auto_target_frequencies_[6]  =  target_frequency * 5.6568542494923805818985f;  // +3V = 2**(3.0/1.2)
              auto_target_frequencies_[7]  =  target_frequency * 10.0793683991589855253324f; // +4V = 2**(4.0/1.2)
              auto_target_frequencies_[8]  =  target_frequency * 17.9593927729499718282113f; // +5V = 2**(5.0/1.2)
              auto_target_frequencies_[9]  =  target_frequency * 32.0f;                      // +6V = 2**(6.0/1.2)
              auto_target_frequencies_[10] =  target_frequency * 57.0175179609817419645879f; // ...
              break;
            case 2: // 2V/octave
              auto_target_frequencies_[0]  =  target_frequency * 0.3535533905932737863687f;  // -3V - 2**(-3.0/2.0)
              auto_target_frequencies_[1]  =  target_frequency * 0.5f;                       // -2V = 2**(-2.0/2.0)
              auto_target_frequencies_[2]  =  target_frequency * 0.7071067811865475727373f;  // -1V = 2**(-1.0/2.0)
              auto_target_frequencies_[3]  =  target_frequency * 1.0f;                       // 0V  = 2**(0.0/2.0)
              auto_target_frequencies_[4]  =  target_frequency * 1.4142135623730951454746f;  // +1V = 2**(1.0/2.0)
              auto_target_frequencies_[5]  =  target_frequency * 2.0f;                       // +2V = 2**(2.0/2.0)
              auto_target_frequencies_[6]  =  target_frequency * 2.8284271247461902909492f;  // +3V = 2**(3.0/2.0)
              auto_target_frequencies_[7]  =  target_frequency * 4.0f;                       // +4V = 2**(4.0/2.0)
              auto_target_frequencies_[8]  =  target_frequency * 5.6568542494923805818985f;  // +5V = 2**(5.0/2.0)
              auto_target_frequencies_[9]  =  target_frequency * 8.0f;                       // +6V = 2**(6.0/2.0)
              auto_target_frequencies_[10] =  target_frequency * 11.3137084989847611637970f; // ...
              break;
            case 0: // 1V/octave
            default:
              auto_target_frequencies_[0]  =  target_frequency * 0.125f;  // -3V
              auto_target_frequencies_[1]  =  target_frequency * 0.25f;   // -2V 
              auto_target_frequencies_[2]  =  target_frequency * 0.5f;    // -1V 
              auto_target_frequencies_[3]  =  target_frequency * 1.0f;    // 0V
              auto_target_frequencies_[4]  =  target_frequency * 2.0f;    // +1V 
              auto_target_frequencies_[5]  =  target_frequency * 4.0f;    // +2V 
              auto_target_frequencies_[6]  =  target_frequency * 8.0f;    // +3V 
              auto_target_frequencies_[7]  =  target_frequency * 16.0f;   // +4V 
              auto_target_frequencies_[8]  =  target_frequency * 32.0f;   // +5V 
              auto_target_frequencies_[9]  =  target_frequency * 64.0f;   // +6V 
              auto_target_frequencies_[10] =  target_frequency * 128.0f;  // ...
              break;
          }
          
          // reset step, and proceed:
          auto_reset_step();
          autotuner_step_++; 
        }
        else if (_update) 
          auto_num_passes_++;
      }
      break;
      case DAC_VOLT_3m:
      case DAC_VOLT_2m:
      case DAC_VOLT_1m: 
      case DAC_VOLT_0:
      case DAC_VOLT_1:
      case DAC_VOLT_2:
      case DAC_VOLT_3:
      case DAC_VOLT_4:
      case DAC_VOLT_5:
      case DAC_VOLT_6:
      { 
        bool _update = auto_frequency();
        
        if (_update && (auto_num_passes_ > MAX_NUM_PASSES)) {  
          /* target frequency reached */
          
          // throw error, if things don't seem to double ...
          if ((autotuner_step_ > DAC_VOLT_2m) && (auto_last_frequency_ * 1.25f > auto_frequency_))
              auto_error_ = true;
          // average:
          float history[kHistoryDepth]; 
          float average = 0.0f;
          history_->Read(history);
          for (uint8_t i = 0; i < kHistoryDepth; i++)
            average += history[i];
          // store last frequency:
           auto_last_frequency_  = ((auto_frequency_ + average) / (float)(kHistoryDepth + 1));
          // and DAC correction value:
          auto_calibration_data_[autotuner_step_ - DAC_VOLT_3m] = auto_DAC_offset_error_;
          // and reset step:
          auto_reset_step();
          autotuner_step_++; 
        }
        // 
        else if (_update) {

          // count passes
          auto_num_passes_++;
          // and correct frequency
          if (auto_target_frequencies_[autotuner_step_ - DAC_VOLT_3m] > auto_frequency_) {
            // update correction factor?
            if (!correction_direction_)
              F_correction_factor_ = (F_correction_factor_ >> 1) | 1u;
            correction_direction_ = true;
            
            auto_DAC_offset_error_ += F_correction_factor_;
            // we're converging -- count passes, so we can stop after x attempts:
            if (F_correction_factor_ == 0x1)
              correction_cnt_positive_++;
          }
          else if (auto_target_frequencies_[autotuner_step_ - DAC_VOLT_3m] < auto_frequency_) {
            // update correction factor?
            if (correction_direction_)
              F_correction_factor_ = (F_correction_factor_ >> 1) | 1u;
            correction_direction_ = false;
            
            auto_DAC_offset_error_ -= F_correction_factor_;
            // we're converging -- count passes, so we can stop after x attempts:
            if (F_correction_factor_ == 0x1)
              correction_cnt_negative_++;
          }

          // approaching target? if so, go to next step.
          if (correction_cnt_positive_ > CONVERGE_PASSES && correction_cnt_negative_ > CONVERGE_PASSES)
            auto_num_passes_ = MAX_NUM_PASSES << 1;
        }
      }
      break;
      case AUTO_CALIBRATION_STEP_LAST:
      // step through the octaves:
      if (ticks_since_last_freq_ > 2000) {
        int32_t new_auto_calibration_point = OC::calibration_data.dac.calibrated_octaves[dac_channel_][octaves_cnt_] + auto_calibration_data_[octaves_cnt_];
        // write to DAC and update data
        OC::DAC::set(dac_channel_, new_auto_calibration_point);
        OC::DAC::update_auto_channel_calibration_data(dac_channel_, octaves_cnt_, new_auto_calibration_point);
        ticks_since_last_freq_ = 0x0;
        octaves_cnt_++;
      }
      // then stop ... 
      if (octaves_cnt_ > OCTAVES) { 
        autotune_completed_ = true;
        // and point to auto data ...
        OC::DAC::set_auto_channel_calibration_data(dac_channel_);
        autotuner_step_++;
      }
      break;
      default:
      autotuner_step_ = DAC_VOLT_0_ARM;
      autotuner_ = 0x0;
      break;
    }
  }
  
  void autotune_updateDAC() {

    switch(autotuner_step_) {

      case DAC_VOLT_0_ARM: 
      {
        F_correction_factor_ = 0x1; // don't go so fast
        auto_frequency();
        OC::DAC::set(dac_channel_, OC::calibration_data.dac.calibrated_octaves[dac_channel_][OC::DAC::kOctaveZero]);
      }
      break;
      case DAC_VOLT_0_BASELINE:
      // set DAC to 0.000V, default calibration:
      OC::DAC::set(dac_channel_, OC::calibration_data.dac.calibrated_octaves[dac_channel_][OC::DAC::kOctaveZero]);
      break;
      case AUTO_CALIBRATION_STEP_LAST:
      // do nothing
      break;
      default: 
      // set DAC to calibration point + error
      {
        int32_t _default_calibration_point = OC::calibration_data.dac.calibrated_octaves[dac_channel_][autotuner_step_ - DAC_VOLT_3m]; // substract first two steps
        OC::DAC::set(dac_channel_, _default_calibration_point + auto_DAC_offset_error_);
      }
      break;
    }
  }

  uint8_t get_voltage_scaling() const {
    #ifdef BUCHLA_SUPPORT
      return values_[REF_SETTING_VOLTAGE_SCALING];
    #else
      return 0x0;
    #endif
  }
 
  void Update() {

    if (autotuner_) {
      autotune_updateDAC();
      ticks_since_last_freq_++;
      return;
    }

    int octave = get_octave();
    int range = get_range();
    if (range) {
      rate_phase_ += OC_CORE_TIMER_RATE;
      if (rate_phase_ >= get_rate() * 1000000UL) {
        rate_phase_ = 0;
        mod_offset_ = 1 - mod_offset_;
      }
      octave += mod_offset_ * range;
    } else {
      rate_phase_ = 0;
      mod_offset_ = 0;
    }

    int32_t semitone = get_semitone();
    OC::DAC::set(dac_channel_, OC::DAC::semitone_to_scaled_voltage_dac(dac_channel_, semitone, octave, get_voltage_scaling()));
    last_pitch_ = (semitone + octave * 12) << 7;       
  }

  int num_enabled_settings() const {
    return num_enabled_settings_;
  }

  ReferenceSetting enabled_setting_at(int index) const {
    return enabled_settings_[index];
  }

  void update_enabled_settings() {
    ReferenceSetting *settings = enabled_settings_;
    *settings++ = REF_SETTING_OCTAVE;
    *settings++ = REF_SETTING_SEMI;
    *settings++ = REF_SETTING_RANGE;
    *settings++ = REF_SETTING_RATE;
    *settings++ = REF_SETTING_AUTOTUNE;
    //*settings++ = REF_SETTING_AUTOTUNE_ERROR;

    if (DAC_CHANNEL_D == dac_channel_) {
      *settings++ = REF_SETTING_NOTES_OR_BPM;
      *settings++ = REF_SETTING_A_ABOVE_MID_C_INTEGER;
      *settings++ = REF_SETTING_A_ABOVE_MID_C_MANTISSA;
      *settings++ = REF_SETTING_PPQN;
    }
    else {
      *settings++ = REF_SETTING_DUMMY;
      *settings++ = REF_SETTING_DUMMY;
    }
    
    #ifdef BUCHLA_SUPPORT
      *settings++ = REF_SETTING_VOLTAGE_SCALING;
    #endif
     num_enabled_settings_ = settings - enabled_settings_;
  }

  void RenderScreensaver(weegfx::coord_t start_x, uint8_t chan) const;

private:
  uint32_t rate_phase_;
  int mod_offset_;
  int32_t last_pitch_;
  bool autotuner_;
  uint8_t autotuner_step_;
  int32_t auto_DAC_offset_error_;
  float auto_frequency_;
  float auto_target_frequencies_[OCTAVES + 1];
  int16_t auto_calibration_data_[OCTAVES + 1];
  float auto_last_frequency_;
  bool auto_next_step_;
  bool auto_error_;
  bool auto_ready_;
  bool autotune_completed_;
  uint32_t auto_freq_sum_;
  uint32_t auto_freq_count_;
  uint32_t ticks_since_last_freq_;
  uint32_t auto_num_passes_;
  uint16_t F_correction_factor_;
  bool correction_direction_;
  int16_t correction_cnt_positive_;
  int16_t correction_cnt_negative_;
  int16_t octaves_cnt_;
  DAC_CHANNEL dac_channel_;

  OC::vfx::ScrollingHistory<float, kHistoryDepth> history_[0x1];

  int num_enabled_settings_;
  ReferenceSetting enabled_settings_[REF_SETTING_LAST];
};

const char* const notes_or_bpm[2] = {
 "notes",  "bpm", 
};

const char* const ppqn_labels[10] = {
 " 1",  " 2", " 4", " 8", "16", "24", "32", "48", "64", "96",  
};

const char* const error[] = {
  "0.050", "0.125", "0.250", "0.500", "1.000", "2.000", "4.000"
};

SETTINGS_DECLARE(ReferenceChannel, REF_SETTING_LAST) {
  { 0, -3, 6, "Octave", nullptr, settings::STORAGE_TYPE_I8 },
  { 0, 0, 11, "Semitone", OC::Strings::note_names_unpadded, settings::STORAGE_TYPE_U8 },
  { 0, -3, 3, "Mod range oct", nullptr, settings::STORAGE_TYPE_U8 },
  { 0, 0, 30, "Mod rate (s)", nullptr, settings::STORAGE_TYPE_U8 },
  { 0, 0, 1, "Notes/BPM :", notes_or_bpm, settings::STORAGE_TYPE_U8 },
  { 440, 400, 480, "A above mid C", nullptr, settings::STORAGE_TYPE_U16 },
  { 0, 0, 99, " > mantissa", nullptr, settings::STORAGE_TYPE_U8 },
  { CHANNEL_PPQN_4, CHANNEL_PPQN_1, CHANNEL_PPQN_LAST - 1, "> ppqn", ppqn_labels, settings::STORAGE_TYPE_U8 },
  { 0, 0, 0, "--> autotune", NULL, settings::STORAGE_TYPE_U8 },
  { 0, 0, 0, "-", NULL, settings::STORAGE_TYPE_U4 }, // dummy
  #ifdef BUCHLA_SUPPORT
  { 0, 0, 2, "V/octave", OC::voltage_scalings, settings::STORAGE_TYPE_U8 }
  #endif
};

class ReferencesApp {
public:
  ReferencesApp() { }
  
  OC::Autotuner<ReferenceChannel> autotuner;

  void Init() {
    int dac_channel = DAC_CHANNEL_A;
    for (auto &channel : channels_)
      channel.Init(static_cast<DAC_CHANNEL>(dac_channel++));

    ui.selected_channel = DAC_CHANNEL_D;
    ui.cursor.Init(0, channels_[DAC_CHANNEL_D].num_enabled_settings() - 1);

    freq_sum_ = 0;
    freq_count_ = 0;
    frequency_ = 0;
    freq_decicents_deviation_ = 0;
    freq_octave_ = 0;
    freq_note_ = 0;
    freq_decicents_residual_ = 0;
    autotuner.Init();
  }

  void ISR() {
      
    for (auto &channel : channels_)
      channel.Update();

    uint8_t _autotuner_active_channel = 0x0;
    for (auto &channel : channels_)
       _autotuner_active_channel += channel.autotuner_active();

    if (_autotuner_active_channel) {
      channels_[_autotuner_active_channel - 0x1].measure_frequency_and_calc_error();
      return;
    }
    else if (FreqMeasure.available()) {
      // average several readings together
      freq_sum_ = freq_sum_ + FreqMeasure.read();
      freq_count_ = freq_count_ + 1;
      
      if (milliseconds_since_last_freq_ > 750) {
        frequency_ = FreqMeasure.countToFrequency(freq_sum_ / freq_count_);
        freq_sum_ = 0;
        freq_count_ = 0;
        milliseconds_since_last_freq_ = 0;
        freq_decicents_deviation_ = round(12000.0 * log2f(frequency_ / get_C0_freq())) + 500;
        freq_octave_ = -2 + ((freq_decicents_deviation_)/ 12000) ;
        freq_note_ = (freq_decicents_deviation_ - ((freq_octave_ + 2) * 12000)) / 1000;
        freq_decicents_residual_ = ((freq_decicents_deviation_ - ((freq_octave_ - 1) * 12000)) % 1000) - 500;
       }
     } else if (milliseconds_since_last_freq_ > 100000) {
      frequency_ = 0.0f;
     }
  }

  ReferenceChannel &selected_channel() {
    return channels_[ui.selected_channel];
  }

  struct {
    int selected_channel;
    menu::ScreenCursor<menu::kScreenLines> cursor;
  } ui;

  ReferenceChannel channels_[DAC_CHANNEL_LAST];

  float get_frequency( ) {
    return(frequency_) ;
  }

  float get_ppqn() {
    float ppqn_ = 4.0 ;
    switch(channels_[DAC_CHANNEL_D].get_channel_ppqn()){
      case CHANNEL_PPQN_1:
        ppqn_ = 1.0;
        break;
      case CHANNEL_PPQN_2:
        ppqn_ = 2.0;
        break;
      case CHANNEL_PPQN_4:
        ppqn_ = 4.0;
        break;
      case CHANNEL_PPQN_8:
        ppqn_ = 8.0;
        break;
      case CHANNEL_PPQN_16:
        ppqn_ = 16.0;
        break;
      case CHANNEL_PPQN_24:
        ppqn_ = 24.0;
        break;
      case CHANNEL_PPQN_32:
        ppqn_ = 32.0;
        break;
      case CHANNEL_PPQN_48:
        ppqn_ = 48.0;
        break;
      case CHANNEL_PPQN_64:
        ppqn_ = 64.0;
        break;
      case CHANNEL_PPQN_96:
        ppqn_ = 96.0;
        break;
      default:
        ppqn_ = 8.0 ;
        break;
    }
    return(ppqn_);
  }

  float get_bpm( ) {
    return((60.0 * frequency_)/get_ppqn()) ;
  }

  bool get_notes_or_bpm( ) {
    return(static_cast<bool>(channels_[DAC_CHANNEL_D].get_notes_or_bpm())) ;
  }

  float get_C0_freq() {
          return(static_cast<float>(channels_[DAC_CHANNEL_D].get_a_above_mid_c() * kAaboveMidCtoC0));
  }

  float get_cents_deviation( ) {
    return(static_cast<float>(freq_decicents_deviation_) / 10.0) ;
  }
  
  float get_cents_residual( ) {
    return(static_cast<float>(freq_decicents_residual_) / 10.0) ;
  }
  
  int8_t get_octave( ) {
    return(freq_octave_) ;
  }
  
  int8_t get_note( ) {
    return(freq_note_) ;
  }

private:
  double freq_sum_;
  uint32_t freq_count_;
  float frequency_ ;
  elapsedMillis milliseconds_since_last_freq_;
  int32_t freq_decicents_deviation_;
  int8_t freq_octave_ ;
  int8_t freq_note_;
  int32_t freq_decicents_residual_;
};

ReferencesApp references_app;

// App stubs
void REFS_init() {
  references_app.Init();
}

size_t REFS_storageSize() {
  return NUM_REF_CHANNELS * ReferenceChannel::storageSize();
}

size_t REFS_save(void *storage) {
  size_t used = 0;
  for (size_t i = 0; i < NUM_REF_CHANNELS; ++i) {
    used += references_app.channels_[i].Save(static_cast<char*>(storage) + used);
  }
  return used;
}

size_t REFS_restore(const void *storage) {
  size_t used = 0;
  for (size_t i = 0; i < NUM_REF_CHANNELS; ++i) {
    used += references_app.channels_[i].Restore(static_cast<const char*>(storage) + used);
    references_app.channels_[i].update_enabled_settings();
  }
  references_app.ui.cursor.AdjustEnd(references_app.channels_[0].num_enabled_settings() - 1);
  return used;
}

void REFS_isr() {
  return references_app.ISR();
}

void REFS_handleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME:
      references_app.ui.cursor.set_editing(false);
      FreqMeasure.begin();
      references_app.autotuner.Close();
      break;
    case OC::APP_EVENT_SUSPEND:
    case OC::APP_EVENT_SCREENSAVER_ON:
    case OC::APP_EVENT_SCREENSAVER_OFF:
      for (size_t i = 0; i < NUM_REF_CHANNELS; ++i)
        references_app.channels_[i].reset_autotuner();
      break;
  }
}

void REFS_loop() {
}

void REFS_menu() {
  menu::QuadTitleBar::Draw();
  for (uint_fast8_t i = 0; i < NUM_REF_CHANNELS; ++i) {
    menu::QuadTitleBar::SetColumn(i);
    graphics.print((char)('A' + i));
  }
  menu::QuadTitleBar::Selected(references_app.ui.selected_channel);

  const auto &channel = references_app.selected_channel();
  menu::SettingsList<menu::kScreenLines, 0, menu::kDefaultValueX> settings_list(references_app.ui.cursor);
  menu::SettingsListItem list_item;

  while (settings_list.available()) {
    const int setting = 
      channel.enabled_setting_at(settings_list.Next(list_item));
    const int value = channel.get_value(setting);
    const settings::value_attr &attr = ReferenceChannel::value_attr(setting);

    switch (setting) {
      case REF_SETTING_AUTOTUNE:
      case REF_SETTING_DUMMY:
         list_item.DrawNoValue<false>(value, attr);
      break;
      default:
        list_item.DrawDefault(value, attr);
      break;
    }
  }
  // autotuner ...
  if (references_app.autotuner.active())
    references_app.autotuner.Draw();
}

void print_voltage(int octave, int fraction) {
  graphics.printf("%01d", octave);
  graphics.movePrintPos(-1, 0); graphics.print('.');
  graphics.movePrintPos(-2, 0); graphics.printf("%03d", fraction);
}

void ReferenceChannel::RenderScreensaver(weegfx::coord_t start_x, uint8_t chan) const {

  // Mostly borrowed from QQ

  weegfx::coord_t x = start_x + 26;
  weegfx::coord_t y = 34 ; // was 60
  // for (int i = 0; i < 5 ; ++i, y -= 4) // was i < 12
    graphics.setPixel(x, y);

  int32_t pitch = last_pitch_ ;
  int32_t unscaled_pitch = last_pitch_ ;

  switch (references_app.channels_[chan].get_voltage_scaling()) {
      case 1: // 1.2V/oct
          pitch = (pitch * 19661) >> 14 ;
          break;
      case 2: // 2V/oct
          pitch = pitch << 1 ;
          break;
      default: // 1V/oct
          break;
    }

  pitch += (OC::DAC::kOctaveZero * 12) << 7;
  unscaled_pitch += (OC::DAC::kOctaveZero * 12) << 7;

  
  CONSTRAIN(pitch, 0, 120 << 7);

  int32_t octave = pitch / (12 << 7);
  int32_t unscaled_octave = unscaled_pitch / (12 << 7);
  pitch -= (octave * 12 << 7);
  unscaled_pitch -= (unscaled_octave * 12 << 7);
  int semitone = pitch >> 7;
  int unscaled_semitone = unscaled_pitch >> 7;

  y = 34 - unscaled_semitone * 2; // was 60, multiplier was 4
  if (unscaled_semitone < 6)
    graphics.setPrintPos(start_x + menu::kIndentDx, y - 7);
  else
    graphics.setPrintPos(start_x + menu::kIndentDx, y);
  graphics.print(OC::Strings::note_names_unpadded[unscaled_semitone]);

  graphics.drawHLine(start_x + 16, y, 8);
  graphics.drawBitmap8(start_x + 28, 34 - unscaled_octave * 2 - 1, OC::kBitmapLoopMarkerW, OC::bitmap_loop_markers_8 + OC::kBitmapLoopMarkerW); // was 60

  octave -= OC::DAC::kOctaveZero;

  // Try and round to 3 digits
  switch (references_app.channels_[chan].get_voltage_scaling()) {
      case 1: // 1.2V/oct
          semitone = ((semitone * 10000 + 40) / 100) % 1000;
          // fudge
          if (octave == -2) semitone -= 100;
          if (octave == -3 && semitone != 0) semitone -= 100;
          break;
      case 2: // 2V/oct
      default: // 1V/oct
          semitone = ((semitone * 10000 + 50) / 120) % 1000;
          break;
    }
  
  // We want [sign]d.ddd = 6 chars in 32px space; with the current font width
  // of 6px that's too tight, so squeeze in the mini minus...
  y = menu::kTextDy;
  graphics.setPrintPos(start_x + menu::kIndentDx, y);
  if (octave >= 0) {
    print_voltage(octave, semitone);
  } else {
    graphics.drawHLine(start_x, y + 3, 2);
    if (semitone)
      print_voltage(-octave - 1, 1000 - semitone);
    else
      print_voltage(-octave, 0);
  }
}

void REFS_screensaver() {
  references_app.channels_[0].RenderScreensaver( 0, 0);
  references_app.channels_[1].RenderScreensaver(32, 1);
  references_app.channels_[2].RenderScreensaver(64, 2);
  references_app.channels_[3].RenderScreensaver(96, 3);
  graphics.setPrintPos(2, 44);
  if (references_app.get_frequency() > 0.0) {
    graphics.printf("TR4 %7.3f Hz", references_app.get_frequency()) ;
    graphics.setPrintPos(2, 56);
    if (references_app.get_notes_or_bpm()) {
      graphics.printf("%7.2f bpm %2.0fppqn", references_app.get_bpm(), references_app.get_ppqn());
    } else if(references_app.get_frequency() >= references_app.get_C0_freq()) {
      graphics.printf("%+i %s %+7.1fc", references_app.get_octave(), OC::Strings::note_names[references_app.get_note()], references_app.get_cents_residual()) ;
    }
  } else {
    graphics.print("TR4 no input") ;
  }
}

void REFS_handleButtonEvent(const UI::Event &event) {

  if (references_app.autotuner.active()) {
    references_app.autotuner.HandleButtonEvent(event);
    return;
  }
  
  if (OC::CONTROL_BUTTON_R == event.control) {

    auto &selected_channel = references_app.selected_channel();
    switch (selected_channel.enabled_setting_at(references_app.ui.cursor.cursor_pos())) {
      case REF_SETTING_AUTOTUNE:
      references_app.autotuner.Open(&selected_channel);
      break;
      case REF_SETTING_DUMMY:
      break;
      default:
      references_app.ui.cursor.toggle_editing();
      break;
    }
  }
}

void REFS_handleEncoderEvent(const UI::Event &event) {

  if (references_app.autotuner.active()) {
    references_app.autotuner.HandleEncoderEvent(event);
    return;
  }
  
  if (OC::CONTROL_ENCODER_L == event.control) {
    int selected = references_app.ui.selected_channel + event.value;
    CONSTRAIN(selected, 0, NUM_REF_CHANNELS - 0x1);
    references_app.ui.selected_channel = selected;
    references_app.ui.cursor.AdjustEnd(references_app.selected_channel().num_enabled_settings() - 1);
  } else if (OC::CONTROL_ENCODER_R == event.control) {
    if (references_app.ui.cursor.editing()) {
        auto &selected_channel = references_app.selected_channel();
        ReferenceSetting setting = selected_channel.enabled_setting_at(references_app.ui.cursor.cursor_pos());
        if (setting == REF_SETTING_DUMMY) 
          references_app.ui.cursor.set_editing(false);
        selected_channel.change_value(setting, event.value);
        selected_channel.update_enabled_settings();
    } else {
      references_app.ui.cursor.Scroll(event.value);
    }
  }
}
