/*
 * Copyright (c) 2020 Deomid "rojer" Ryabkov
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "shelly_switch.hpp"

#include "mgos.hpp"

#include "shelly_hap_accessory.hpp"
#include "shelly_hap_chars.hpp"

#define SHELLY_HAP_IID_BASE_SWITCH 0x100
#define SHELLY_HAP_IID_STEP_SWITCH 4

namespace shelly {

ShellySwitch::ShellySwitch(int id, Input *in, Output *out, PowerMeter *out_pm,
                           Output *led_out, struct mgos_config_sw *cfg)
    : Component(id),
      in_(in),
      out_(out),
      led_out_(led_out),
      out_pm_(out_pm),
      cfg_(cfg),
      auto_off_timer_(std::bind(&ShellySwitch::AutoOffTimerCB, this)) {
}

ShellySwitch::~ShellySwitch() {
  if (in_ != nullptr) {
    in_->RemoveHandler(handler_id_);
  }
  SaveState();
}

Component::Type ShellySwitch::type() const {
  return Type::kSwitch;
}

StatusOr<std::string> ShellySwitch::GetInfo() const {
  int in_st = -1;
  if (in_ != nullptr) in_st = in_->GetState();
  const_cast<ShellySwitch *>(this)->SaveState();
  return mgos::SPrintf("st:%d in_st:%d inm:%d ininv:%d", out_->GetState(),
                       in_st, cfg_->in_mode, cfg_->in_inverted);
}

StatusOr<std::string> ShellySwitch::GetInfoJSON() const {
  std::string res = mgos::JSONPrintStringf(
      "{id: %d, type: %d, name: %Q, svc_type: %d, in_mode: %d, "
      "in_inverted: %B, initial: %d, state: %B, auto_off: %B, "
      "auto_off_delay: %.3f",
      id(), type(), (cfg_->name ? cfg_->name : ""), cfg_->svc_type,
      cfg_->in_mode, cfg_->in_inverted, cfg_->initial_state, out_->GetState(),
      cfg_->auto_off, cfg_->auto_off_delay);
  if (out_pm_ != nullptr) {
    auto power = out_pm_->GetPowerW();
    if (power.ok()) {
      mgos::JSONAppendStringf(&res, ", apower: %.3f", power.ValueOrDie());
    }
    auto energy = out_pm_->GetEnergyWH();
    if (energy.ok()) {
      mgos::JSONAppendStringf(&res, ", aenergy: %.3f", energy.ValueOrDie());
    }
  }
  res.append("}");
  return res;
}

Status ShellySwitch::SetConfig(const std::string &config_json,
                               bool *restart_required) {
  struct mgos_config_sw cfg = *cfg_;
  int8_t in_inverted = -1;
  cfg.name = nullptr;
  cfg.in_mode = -2;
  json_scanf(config_json.c_str(), config_json.size(),
             "{name: %Q, svc_type: %d, in_mode: %d, in_inverted: %B, "
             "initial_state: %d, "
             "auto_off: %B, auto_off_delay: %lf}",
             &cfg.name, &cfg.svc_type, &cfg.in_mode, &in_inverted,
             &cfg.initial_state, &cfg.auto_off, &cfg.auto_off_delay);
  mgos::ScopedCPtr name_owner((void *) cfg.name);
  // Validation.
  if (cfg.name != nullptr && strlen(cfg.name) > 64) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s",
                        "name (too long, max 64)");
  }
  if (cfg.svc_type < -1 || cfg.svc_type > 2) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "svc_type");
  }
  if (cfg.in_mode != -2 &&
      (cfg.in_mode < 0 || cfg.in_mode >= (int) InMode::kMax)) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "in_mode");
  }
  if (cfg.initial_state < 0 || cfg.initial_state >= (int) InitialState::kMax ||
      (cfg_->in_mode == -1 &&
       cfg.initial_state == (int) InitialState::kInput)) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "initial_state");
  }
  cfg.auto_off = (cfg.auto_off != 0);
  if (cfg.initial_state < 0 || cfg.initial_state > (int) InitialState::kMax) {
    return mgos::Errorf(STATUS_INVALID_ARGUMENT, "invalid %s", "initial_state");
  }
  // Now copy over.
  *restart_required = false;
  if (cfg_->name != nullptr && strcmp(cfg_->name, cfg.name) != 0) {
    mgos_conf_set_str(&cfg_->name, cfg.name);
    *restart_required = true;
  }
  if (cfg_->svc_type != cfg.svc_type) {
    cfg_->svc_type = cfg.svc_type;
    *restart_required = true;
  }
  if (cfg.in_mode != -2 && cfg_->in_mode != cfg.in_mode) {
    if (cfg_->in_mode == (int) InMode::kDetached ||
        cfg.in_mode == (int) InMode::kDetached) {
      *restart_required = true;
    }
    cfg_->in_mode = cfg.in_mode;
  }
  if (in_inverted != -1 && cfg_->in_inverted != in_inverted) {
    cfg_->in_inverted = in_inverted;
    *restart_required = true;
  }
  cfg_->initial_state = cfg.initial_state;
  cfg_->auto_off = cfg.auto_off;
  cfg_->auto_off_delay = cfg.auto_off_delay;
  return Status::OK();
}

Status ShellySwitch::Init() {
  if (!cfg_->enable) {
    LOG(LL_INFO, ("'%s' is disabled", cfg_->name));
    return Status::OK();
  }
  if (in_ != nullptr) {
    handler_id_ = in_->AddHandler(
        std::bind(&ShellySwitch::InputEventHandler, this, _1, _2));
    in_->SetInvert(cfg_->in_inverted);
  }
  switch (static_cast<InitialState>(cfg_->initial_state)) {
    case InitialState::kOff:
      SetState(false, "init");
      break;
    case InitialState::kOn:
      SetState(true, "init");
      break;
    case InitialState::kLast:
      SetState(cfg_->state, "init");
      break;
    case InitialState::kInput:
      if (in_ != nullptr &&
          cfg_->in_mode == static_cast<int>(InMode::kToggle)) {
        SetState(in_->GetState(), "init");
      }
      break;
    case InitialState::kMax:
      break;
  }
  LOG(LL_INFO, ("Exporting '%s': type %d, state: %d", cfg_->name,
                cfg_->svc_type, out_->GetState()));
  return Status::OK();
}

bool ShellySwitch::GetState() const {
  return out_->GetState();
}

void ShellySwitch::SetState(bool new_state, const char *source) {
  bool cur_state = out_->GetState();
  out_->SetState(new_state, source);
  if (led_out_ != nullptr) {
    led_out_->SetState(new_state, source);
  }
  if (cfg_->state != new_state) {
    cfg_->state = new_state;
    dirty_ = true;
  }

  if (new_state && cfg_->auto_off) {
    auto_off_timer_.Reset(cfg_->auto_off_delay * 1000, 0);
  } else {
    auto_off_timer_.Clear();
  }

  if (new_state == cur_state) return;

  for (auto *c : state_notify_chars_) {
    c->RaiseEvent();
  }
}

void ShellySwitch::AutoOffTimerCB() {
  // Don't set state if auto off has been disabled during timer run
  if (!cfg_->auto_off) return;
  if (static_cast<InMode>(cfg_->in_mode) == InMode::kActivation &&
      in_ != nullptr && in_->GetState() && GetState()) {
    // Input is active, re-arm.
    LOG(LL_INFO, ("Input is active, re-arming auto off timer"));
    auto_off_timer_.Reset(cfg_->auto_off_delay * 1000, 0);
    return;
  }
  SetState(false, "auto_off");
}

void ShellySwitch::SaveState() {
  if (!dirty_) return;
  mgos_sys_config_save(&mgos_sys_config, false /* try_once */, NULL /* msg */);
  dirty_ = false;
}

void ShellySwitch::InputEventHandler(Input::Event ev, bool state) {
  InMode in_mode = static_cast<InMode>(cfg_->in_mode);
  if (in_mode == InMode::kDetached) {
    // Nothing to do
    return;
  }
  switch (ev) {
    case Input::Event::kChange: {
      switch (static_cast<InMode>(cfg_->in_mode)) {
        case InMode::kMomentary:
          if (state) {  // Only on 0 -> 1 transitions.
            SetState(!out_->GetState(), "ext_mom");
          }
          break;
        case InMode::kToggle:
          SetState(state, "switch");
          break;
        case InMode::kEdge:
          SetState(!out_->GetState(), "ext_edge");
          break;
        case InMode::kActivation:
          if (state) {
            SetState(true, "ext_act");
          } else if (GetState() && cfg_->auto_off) {
            // On 1 -> 0 transitions do not turn on output
            // but re-arm auto off timer if running.
            auto_off_timer_.Reset(cfg_->auto_off_delay * 1000, 0);
          }
          break;
        case InMode::kAbsent:
        case InMode::kDetached:
        case InMode::kMax:
          break;
      }
      break;
    }
    case Input::Event::kLong:
      // Disable auto-off if it was active.
      if (in_mode == InMode::kMomentary) {
        auto_off_timer_.Clear();
      }
      break;
    case Input::Event::kSingle:
    case Input::Event::kDouble:
    case Input::Event::kReset:
      break;
  }
}

}  // namespace shelly
