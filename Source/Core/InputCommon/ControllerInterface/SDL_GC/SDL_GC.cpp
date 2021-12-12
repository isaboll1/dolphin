// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/SDL_GC/SDL_GC.h"

#include <algorithm>
#include <thread>

#include <SDL_events.h>

#include "Common/Event.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

#ifdef _WIN32
#pragma comment(lib, "SDL2.lib")
#endif

namespace ciface::SDL_GC
{

// Definition for inputs/axis associated with the SDL_gamecontroller device
struct ButtonDef
{
    const char* name;
    SDL_GameControllerButton bitmask;
};

struct AxisDef
{
    const char* name;
    SDL_GameControllerAxis bitmask;
};

static constexpr std::array<ButtonDef, 16> named_buttons{{
    {"Button A", SDL_CONTROLLER_BUTTON_A},
    {"Button B", SDL_CONTROLLER_BUTTON_B},
    {"Button X", SDL_CONTROLLER_BUTTON_X},
    {"Button Y", SDL_CONTROLLER_BUTTON_Y},
    {"Pad N", SDL_CONTROLLER_BUTTON_UP},
    {"Pad S", SDL_CONTROLLER_BUTTON_DONW},
    {"Pad W", SDL_CONTROLLER_BUTTON_LEFT},
    {"Pad E", SDL_CONTROLLER_BUTTON_RIGHT},
    {"Start", SDL_CONTROLLER_BUTTON_START},
    {"Back", SDL_CONTROLLER_BUTTON_BACK},
    {"Shoulder L", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"Shoulder R", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"Guide", SDL_CONTROLLER_BUTTON_GUIDE},
    {"Thumb L", SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {"Thumb R", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {"Touchpad", SDL_CONTROLLER_BUTTON_TOUCHPAD}
}};

static constexpr std::array<AxisDef, 2> named_trigger_axis{{
    {"Trigger L", SDL_CONTROLLER_AXIS_TRIGGERLEFT},
    {"Trigger R", SDL_CONTROLLER_AXIS_TRIGGERRIGHT}
}};

static constexpr std::array<AxisDef, 4> named_stick_axis{{
    {"Left X", SDL_CONTROLLER_AXIS_LEFTX},
    {"Left Y", SDL_CONTROLELR_AXIS_LEFTY},
    {"Right X", SDL_CONTROLLER_AXIS_RIGHTX},
    {"Right Y", SDL_CONTROLLER_AXIS_RIGHTY}
}};

static constexpr std::array named_motors{"Motor L", "Motor R"};

// button class
class Button final : public Core::Device::Input{
public:
  Button(u8 index, SDL_GameController* gc) : m_index(index), m_gc(gc){}
  std::string GetName() const override { return named_buttons[m_index].name; }
  ControlState GetState() const override
  {
      return ControlState(SDL_GameControllerGetButton(m_gc, named_buttons[m_index].bitmask)) > 0;
  }
private:
  const u8 m_index;
  SDL_GameController* const m_gc;
};

//axis class
class Axis final : public Core::Device::Input
{
public:
  Axis(u8 index, SDL_GameController* gc, Uint16 range) : m_index(index), m_gc(gc), m_range(range){};
  std::string GetName() const override
  {
      return std::string(named_stick_axis[m_index].name) + (m_range < 0 ? '-' : '+');
  }
  ControlState GetState() const override 
  { 
      return ControlState(SDL_GameControllerGetAxis(m_gc, named_stick_axis[m_index].bitmask) / m_range); 
  }
private:
  SDL_GameController* const m_gc;
  const Uint16 m_range;
  const u8 m_index;
};

// trigger class
class Trigger final : public Core::Device::Input
{
public:
  Trigger(u8 index, SDL_GameController* gc, Uint16 range)
      : m_index(index), m_gc(gc), m_range(range)
  {}
  std::string GetName() const override { return named_trigger_axis[m_index].name; }
  ControlState GetState() const override 
  { 
      return ControlState(SDL_GameControllerGetAxis(m_gc, named_trigger_axis[m_index].bitmask) / m_range); 
  }
private:
  SDL_GameController* const m_gc;
  const Uint16 m_range;
  const u8 m_index;
};

// motor class
class Motor final : public Core::Device::Output
{
public:
  Motor(u8 index, Device* parent, Uint16 range)
      : m_range(range), m_index(index), m_parent(parent)
  {}

  std::string GetName() const override { return named_motors[m_index]; }
  void SetState(ControlState state) override
  {
    Uint16 old_motor = 0; 
    Uint16 m_motor = ((Uint16)(state * m_range));
    if (m_index){
      old_motor = m_parent->motor_val.right;
      if (m_motor != old_motor){
          m_parent->motor_val.right = m_motor;
          m_parent->UpdateMotors();
      }
      return;
    }
    old_motor = m_parent->motor_val.left;
    if (m_motor != old_motor){
        m_parent->motor_val.left = m_motor;
        m_parent->UpdateMotors();
    }
  }

private:
  const Uint16 m_range;
  const u8 m_index;
  Device* m_parent;
};

// battery class
class Battery final : public Core::Device::Input
{
public:
  Battery(const ControlState* level) : m_level(*level) {}
  std::string GetName() const override { return "Battery"; }
  ControlState GetState() const override { return m_level; }
  bool IsDetectable() const override { return false; }

private:
  const ControlState& m_level;
};

// event related things
static void OpenAndAddDevice(int index){
    SDL_GameController* const dev = SDL_GameControllerOpen(index);
    if (dev){
        auto gc = std::make_shared<Device>(dev, index);
        if (!gc->Inputs().empty() || !gc->Outputs().empty()){
            g_controller_interface.AddDevice(std::move(gc));
        }
    }
}

static Common::Event s_init_event;
static Uint32 s_stop_event_type;
static Uint32 s_populate_event_type;
static std::thread s_hotplug_thread;

static bool HandleEventAndContinue(const SDL_Event& e)
{
  if (e.type == SDL_CONTROLLERDEVICEADDED)
  {
    OpenAndAddDevice(e.jdevice.which);
  }
  else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
  {
    g_controller_interface.RemoveDevice([&e](const auto* device) {
      const Device* game_controller = dynamic_cast<const Device*>(device);
      return game_controller && (SDL_JoystickInstanceID(game_controller->GetSDLJoystick()) == e.cdevice.which);
    });
  }
  else if (e.type == s_populate_event_type)
  {
    g_controller_interface.PlatformPopulateDevices([] {
      for (int i = 0; i < SDL_NumJoysticks(); ++i)
        OpenAndAddDevice(i);
    });
  }
  else if (e.type == s_stop_event_type)
  {
    return false;
  }

  return true;
}

void Init()
{ 
  s_hotplug_thread = std::thread([] {
    Common::ScopeGuard quit_guard([] {
      // TODO: there seems to be some sort of memory leak with SDL, quit isn't freeing everything up
      SDL_Quit();
    });

    {
      Common::ScopeGuard init_guard([] { s_init_event.Set(); });

      if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) == 0)
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE, "SDL failed to initialize");
        return; 
      }
      SDL_version linked;
      SDL_GetVersion(&linked);

      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
      if ((linked.major == 2) && (linked.minor == 0) && (linked.patch >= 14)){
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
      }
      SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
      
      const Uint32 custom_events_start = SDL_RegisterEvents(2);
      if (custom_events_start == static_cast<Uint32>(-1))
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE, "SDL failed to register custom events");
        return;
      }
      s_stop_event_type = custom_events_start;
      s_populate_event_type = custom_events_start + 1;

      // Drain all of the events and add the initial joysticks before returning. Otherwise, the
      // individual joystick events as well as the custom populate event will be handled _after_
      // ControllerInterface::Init/RefreshDevices has cleared its list of devices, resulting in
      // duplicate devices. Adding devices will actually "fail" here, as the ControllerInterface
      // hasn't finished initializing yet.
      SDL_Event e;
      while (SDL_PollEvent(&e) != 0)
      {
        if (!HandleEventAndContinue(e))
          return;
      }
    }

    SDL_Event e;
    while (SDL_WaitEvent(&e) != 0)
    {
      if (!HandleEventAndContinue(e))
        return;
    }
  });

  s_init_event.Wait();
}

void DeInit()
{
  if (!s_hotplug_thread.joinable())
    return;

  SDL_Event stop_event{s_stop_event_type};
  SDL_PushEvent(&stop_event);

  s_hotplug_thread.join();  
}

void PopulateDevices()
{
  if (!s_hotplug_thread.joinable())
    return;

  SDL_Event populate_event{s_populate_event_type};
  SDL_PushEvent(&populate_event);
}

Device::Device(SDL_GameController * const controller, u8 index)
     : m_controller(controller), m_index(index)
{
  m_joystick = SDL_GameControllerGetJoystick(m_controller);
  m_name = StripSpaces(SDL_GameControllerName(m_controller));

  // Buttons
  for (size_t i = 0; i != size(named_buttons); ++i)
  {
    AddInput(new Button(u8(i), m_controller));
  }

  // Triggers
  for (size_t i = 0; i != size(named_trigger_axis); ++i){
    AddInput(new Trigger(u8(i), m_controller, SDL_JOYSTICK_AXIS_MAX));
  }

  // Axes
  for (size_t i = 0; i != size(named_stick_axis); ++i){
    // Each axis gets a negative and a positive input instance associated with it.
    AddAnalogInputs(new Axis(u8(i), m_controller, SDL_JOYSTICK_AXIS_MIN), 
                    new Axis(u8(i), m_controller, SDL_JOYSTICK_AXIS_MAX));

  }

  // Rumble motors
  for (size_t i = 0; i != size(named_motors); ++i)
    AddOutput(new Motor(u8(i), this, 0xFFFF));

  // Battery level
  AddInput(new Battery(&m_battery_level));
}

Device::~Device(){
    // close game controller
  SDL_GameControllerClose(m_controller);
}

std::string Device::GetName() const
{
  return m_name;
}

std::string Device::GetSource() const
{
  return "SDL_gamecontroller";
}


void Device::UpdateInput()
{
  // TODO: Don't call this for every controller, only once per ControllerInterface::UpdateInput()
  SDL_GameControllerUpdate();

  SDL_JoystickPowerLevel battery_info = {};
  battery_info = SDL_JoystickCurrentPowerLevel(this->GetSDLJoystick());
  switch (battery_info)
  {
    case SDL_JOYSTICK_POWER_WIRED:
    case SDL_JOYSTICK_POWER_MAX:
      m_battery_level = 1.0;
      break;
    case SDL_JOYSTICK_POWER_MEDIUM:
      m_battery_level = 0.5;
      break;
    case SDL_JOYSTICK_POWER_LOW:
      m_battery_level = 0.3;
      break;
    default:
      m_battery_level = 0;
      break;
        
  }
}

void Device::UpdateMotors()
{
  SDL_GameControllerRumble(m_controller, motor_val.left, motor_val.right, RUMBLE_LENGTH_MS);
}

std::optional<int> Device::GetPreferredId() const
{
  return m_index;
}

}