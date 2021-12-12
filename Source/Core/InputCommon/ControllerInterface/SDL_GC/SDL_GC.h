// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The minimum supported SDL2 version should be 2.0.12
#include <SDL.h>

#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ciface::SDL_GC
{
void Init();
void DeInit();
void PopulateDevices();


class Device final : public Core::Device
{
public:
struct MotorVal
  {
    Uint16 left;
    Uint16 right;
  } motor_val; 

  Device(SDL_GameController * const gamecontroller, u8 index);
  ~Device();

  std::string GetName() const override;
  std::string GetSource() const override;
  std::optional<int> GetPreferredId() const override;
  int GetSortPriority() const override { return -1; }

  SDL_Joystick * GetSDLJoystick() const{ return m_joystick;}
  void UpdateInput() override;
  void UpdateMotors();

private:
  SDL_GameController * m_controller;
  SDL_Joystick * m_joystick;
  std::string m_name;
  ControlState m_battery_level{};
  const u8 m_index;
};
}