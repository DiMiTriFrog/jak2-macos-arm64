#include "input_manager.h"

#include <atomic>
#include <cmath>
#include <unordered_set>

#include "input_manager.h"
#include "sdl_util.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/graphics/pipelines/opengl.h"
#include "game/runtime.h"

#include "third-party/SDL/include/SDL3/SDL_hints.h"
#include "third-party/imgui/imgui.h"

namespace {
struct SDLFreeDeleter {
  template <typename T>
  void operator()(T* ptr) const noexcept {
    SDL_free(ptr);
  }
};
}  // namespace

InputManager::InputManager(SDL_Window* window)
    : m_window(window),
      // Load user settings
      m_settings(std::make_shared<game_settings::InputSettings>(game_settings::InputSettings())) {
  prof().instant_event("ROOT");
  {
    auto p = scoped_prof("input_manager::init");
    m_settings->load_settings();
#ifdef WIN32
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER, "1")) {
      sdl_util::log_error("Unable to set SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER to true!");
    }
#else
    if (!SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1")) {
      sdl_util::log_error("Unable to set SDL_HINT_JOYSTICK_HIDAPI_PS3 to true!");
    }
#endif
    {
      auto p = scoped_prof("input_manager::init::sdl_init_subsystem");
      // initializing the controllers on startup can sometimes take a very long time
      // so we isolate that to here instead
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        sdl_util::log_error(
            "Could not initialize SDL Controller support, controllers will not work!");
      }
    }

    // Update to latest controller DB file
    std::string mapping_path =
        (file_util::get_jak_project_dir() / "game" / "assets" / "sdl_controller_db.txt").string();
    if (file_util::file_exists(mapping_path)) {
      SDL_AddGamepadMappingsFromFile(mapping_path.c_str());
    } else {
      lg::error("Could not find SDL Controller DB at path `{}`", mapping_path);
    }
    // Initialize atleast 2 ports, because that's normal for Jak
    // more will be allocated if more controllers are found
    m_data[0] = std::make_shared<PadData>();
    m_data[1] = std::make_shared<PadData>();
    m_keyboard = KeyboardDevice(m_settings);
    m_mouse = MouseDevice(m_window, m_settings);

    if (m_data.find(m_keyboard_and_mouse_port) == m_data.end()) {
      m_data[m_keyboard_and_mouse_port] = std::make_shared<PadData>();
    }
    m_command_binds = CommandBindingGroups();
    refresh_device_list();
    ignore_background_controller_events(false);
    hide_cursor(m_auto_hide_mouse);
  }
}

InputManager::~InputManager() {
  prof().instant_event("ROOT");
  {
    auto p = scoped_prof("input_manager::destroy");
    for (auto& device : m_available_controllers) {
      device->close_device();
    }
    m_settings->save_settings();
  }
}

void InputManager::refresh_device_list() {
  prof().instant_event("ROOT");
  {
    auto p = scoped_prof("input_manager::refresh_device_list");
    std::unordered_map<SDL_JoystickID, std::shared_ptr<GameController>> existing_controllers;
    for (const auto& controller : m_available_controllers) {
      if (controller && controller->is_loaded()) {
        existing_controllers.emplace(controller->get_instance_id(), controller);
      }
    }

    std::vector<std::shared_ptr<GameController>> refreshed_controllers;
    std::unordered_set<SDL_JoystickID> retained_ids;

    int num_joysticks = 0;
    std::unique_ptr<SDL_JoystickID, SDLFreeDeleter> joysticks{
        SDL_GetJoysticks(&num_joysticks)};
    if (!joysticks && num_joysticks > 0) {
      sdl_util::log_error("Unable to enumerate SDL joysticks");
    }

    for (int i = 0; joysticks && i < num_joysticks; i++) {
      const auto instance_id = joysticks.get()[i];
      if (!SDL_IsGamepad(instance_id)) {
        lg::debug("SDL joystick {} is not available through the Gamepad API", instance_id);
        continue;
      }

      std::shared_ptr<GameController> controller;
      if (const auto existing = existing_controllers.find(instance_id);
          existing != existing_controllers.end()) {
        controller = existing->second;
      } else {
        controller = std::make_shared<GameController>(instance_id, m_settings);
      }
      if (!controller->is_loaded()) {
        lg::error("Unable to load GameController with instance id {}, skipping", instance_id);
        continue;
      }
      refreshed_controllers.push_back(std::move(controller));
      retained_ids.insert(instance_id);
    }

    // Clear aggregate state before removing devices so no held input survives
    // a disconnect. Reused handles stay open; only disappeared devices close.
    clear_inputs();
    for (const auto& old_controller : m_available_controllers) {
      if (old_controller &&
          !retained_ids.contains(old_controller->get_instance_id())) {
        old_controller->close_device();
      }
    }

    m_available_controllers = std::move(refreshed_controllers);
    m_controller_port_mapping.clear();

    // Keep SDL instance IDs, vector indices and logical PS2 ports separate.
    std::unordered_set<int> used_ports;
    auto first_free_port = [&used_ports]() {
      int port = 0;
      while (used_ports.contains(port)) {
        port++;
      }
      return port;
    };
    for (size_t controller_index = 0; controller_index < m_available_controllers.size();
         controller_index++) {
      const auto& controller = m_available_controllers.at(controller_index);
      const auto saved = m_settings->controller_port_mapping.find(controller->get_guid());
      int port = -1;
      if (saved != m_settings->controller_port_mapping.end() && saved->second >= 0 &&
          !used_ports.contains(saved->second)) {
        port = saved->second;
      } else {
        if (saved != m_settings->controller_port_mapping.end() && saved->second >= 0) {
          lg::warn("Controller port {} is already in use; assigning {} to the next free port",
                   saved->second, controller->get_guid());
        }
        port = first_free_port();
      }
      used_ports.insert(port);
      // The value is the filtered vector index, never the SDL enumeration index.
      m_controller_port_mapping[port] = static_cast<int>(controller_index);
      m_settings->controller_port_mapping[controller->get_guid()] = port;
      m_data.try_emplace(port, std::make_shared<PadData>());
    }

    // Preserve the user's last-selected controller as logical port 0 without
    // creating duplicate port mappings.
    if (!m_settings->last_selected_controller_guid.empty()) {
      int selected_index = -1;
      int selected_port = -1;
      for (const auto& [port, controller_index] : m_controller_port_mapping) {
        if (controller_index >= 0 &&
            static_cast<size_t>(controller_index) < m_available_controllers.size() &&
            m_available_controllers.at(controller_index)->get_guid() ==
                m_settings->last_selected_controller_guid) {
          selected_index = controller_index;
          selected_port = port;
          break;
        }
      }
      if (selected_index >= 0 && selected_port != 0) {
        if (const auto port_zero = m_controller_port_mapping.find(0);
            port_zero != m_controller_port_mapping.end()) {
          const auto displaced_index = port_zero->second;
          int replacement_port = 1;
          while (used_ports.contains(replacement_port)) {
            replacement_port++;
          }
          m_controller_port_mapping[replacement_port] = displaced_index;
          if (displaced_index >= 0 &&
              static_cast<size_t>(displaced_index) < m_available_controllers.size()) {
            m_settings->controller_port_mapping[
                m_available_controllers.at(displaced_index)->get_guid()] = replacement_port;
          }
          m_data.try_emplace(replacement_port, std::make_shared<PadData>());
          m_controller_port_mapping.erase(port_zero);
        }
        m_controller_port_mapping.erase(selected_port);
        m_controller_port_mapping[0] = selected_index;
        m_settings->controller_port_mapping[m_settings->last_selected_controller_guid] = 0;
      }
    }
    if (m_available_controllers.empty()) {
      lg::warn(
          "No active game controllers could be found or loaded successfully - inputs will not "
          "work!");
      m_settings->_keyboard_temp_enabled = true;
    } else {
      lg::info("Found {} controllers", m_available_controllers.size());
      m_settings->_keyboard_temp_enabled = false;
    }
  }
}

void InputManager::enqueue_ignore_background_controller_events(const bool ignore) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::IGNORE_BACKGROUND_CONTROLLER_EVENTS, ignore, {}, {}, {}});
}

void InputManager::ignore_background_controller_events(const bool ignore) {
  m_ignore_background_controller_events = ignore;
  // TODO - ignoring return value (atleast log it)
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, ignore ? "0" : "1");
}

void InputManager::hide_cursor(const bool hide_cursor) {
  if (hide_cursor == m_mouse_currently_hidden) {
    return;
  }
  // NOTE - seems like an SDL bug, but the cursor will be visible / locked to the center of the
  // screen if you use the 'start menu' to exit the window / return to it (atleast in windowed mode)
  if (hide_cursor && !SDL_HideCursor()) {
    sdl_util::log_error("Unable to hide mouse cursor");
    return;
  }
  if (!hide_cursor && !SDL_ShowCursor()) {
    sdl_util::log_error("Unable to show mouse cursor");
    return;
  }
  m_mouse_currently_hidden = hide_cursor;
}

void InputManager::process_sdl_event(const SDL_Event& event) {
  const bool controller_topology_event = sdl_util::is_any_event_type(
      event.type, {SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_REMOVED,
                   SDL_EVENT_GAMEPAD_REMAPPED});
  if (controller_topology_event) {
    // SDL can emit a burst of topology events. Refresh once at the end of the
    // frame and do not dispatch an event to a handle that is about to close.
    m_controller_refresh_pending = true;
  }

  if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
    m_keyboard.process_event(event, m_command_binds, m_data.at(m_keyboard_and_mouse_port),
                             m_waiting_for_bind);
    m_mouse.process_event(event, m_command_binds, m_data.at(m_keyboard_and_mouse_port),
                          m_waiting_for_bind);
  }

  // Send event to active controller device
  // This goes last so it takes precedence
  if (!controller_topology_event) {
    for (const auto& mapping : m_controller_port_mapping) {
      const auto port = mapping.first;
      if (m_data.find(port) != m_data.end()) {
        if (auto* controller = controller_for_port(port)) {
          controller->process_event(event, m_command_binds, m_data.at(port), m_waiting_for_bind);
        }
      }
    }
  }

  // Clear the binding assignment if we got one
  if (m_waiting_for_bind && m_waiting_for_bind->assigned) {
    stop_waiting_for_bind();
    // NOTE - this is a total hack, but it's to prevent immediately re-assigning the "confirmation"
    // bind if you use a source that is polled
    // TODO: There's a correct way to do this....figure it out eventually
    m_skip_polling_for_n_frames = 60;
  }

  // Adjust mouse cursor visibility
  if (m_auto_hide_mouse) {
    if (event.type == SDL_EVENT_MOUSE_MOTION && !m_mouse.is_camera_being_controlled()) {
      hide_cursor(false);
    } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
      hide_cursor(true);
    }
  }
}

void InputManager::poll_keyboard_data() {
  if (is_keyboard_enabled() && m_skip_polling_for_n_frames <= 0 && !m_waiting_for_bind) {
    if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
      m_keyboard.poll_state(m_data.at(m_keyboard_and_mouse_port));
    }
  }
}

void InputManager::clear_keyboard_actions() {
  if (is_keyboard_enabled()) {
    if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
      m_keyboard.clear_actions(m_data.at(m_keyboard_and_mouse_port));
    }
  }
}

void InputManager::poll_mouse_data() {
  if (m_mouse_enabled && m_skip_polling_for_n_frames <= 0 && !m_waiting_for_bind) {
    if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
      m_mouse.poll_state(m_data.at(m_keyboard_and_mouse_port));
    }
  }
}

void InputManager::clear_mouse_actions() {
  if (m_mouse_enabled && !m_waiting_for_bind) {
    if (m_data.find(m_keyboard_and_mouse_port) != m_data.end()) {
      m_mouse.clear_actions(m_data.at(m_keyboard_and_mouse_port));
    }
  }
}

void InputManager::finish_polling() {
  if (m_controller_refresh_pending) {
    m_controller_refresh_pending = false;
    refresh_device_list();
  }
  m_skip_polling_for_n_frames--;
  if (m_skip_polling_for_n_frames < 0) {
    m_skip_polling_for_n_frames = 0;
  }
}

void InputManager::process_ee_events() {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  // Fully process any events from the EE
  while (!ee_event_queue.empty()) {
    const auto& evt = ee_event_queue.front();
    switch (evt.type) {
      case EEInputEventType::IGNORE_BACKGROUND_CONTROLLER_EVENTS:
        ignore_background_controller_events(std::get<bool>(evt.param1));
        break;
      case EEInputEventType::UPDATE_RUMBLE:
        controller_send_rumble(std::get<int>(evt.param1), std::get<u8>(evt.param2),
                               std::get<u8>(evt.param3));
        break;
      case EEInputEventType::SET_CONTROLLER_LED:
        set_controller_led(std::get<int>(evt.param1), std::get<u8>(evt.param2),
                           std::get<u8>(evt.param3), std::get<u8>(evt.param4));
        break;
      case EEInputEventType::UPDATE_MOUSE_OPTIONS:
        update_mouse_options(std::get<bool>(evt.param1), std::get<bool>(evt.param2),
                             std::get<bool>(evt.param3));
        break;
      case EEInputEventType::SET_AUTO_HIDE_MOUSE:
        set_auto_hide_mouse(std::get<bool>(evt.param1));
        break;
      case EEInputEventType::CONTROLLER_CLEAR_TRIGGER_EFFECT:
        controller_clear_trigger_effect(
            std::get<int>(evt.param1),
            std::get<dualsense_effects::TriggerEffectOption>(evt.param2));
        break;
      case EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_FEEDBACK:
        controller_send_trigger_effect_feedback(
            std::get<int>(evt.param1), std::get<dualsense_effects::TriggerEffectOption>(evt.param2),
            std::get<u8>(evt.param3), std::get<u8>(evt.param4));
        break;
      case EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_VIBRATE:
        controller_send_trigger_effect_vibrate(
            std::get<int>(evt.param1), std::get<dualsense_effects::TriggerEffectOption>(evt.param2),
            std::get<u8>(evt.param3), std::get<u8>(evt.param4), std::get<u8>(evt.param5));
        break;
      case EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_WEAPON:
        controller_send_trigger_effect_weapon(
            std::get<int>(evt.param1), std::get<dualsense_effects::TriggerEffectOption>(evt.param2),
            std::get<u8>(evt.param3), std::get<u8>(evt.param4), std::get<u8>(evt.param5));
        break;
      case EEInputEventType::CONTROLLER_SEND_TRIGGER_RUMBLE:
        controller_send_trigger_rumble(std::get<int>(evt.param1), std::get<u16>(evt.param2),
                                       std::get<u16>(evt.param3), std::get<u32>(evt.param4));
        break;
      case EEInputEventType::SET_TRIGGER_EFFECTS_ENABLED:
        set_trigger_effects_enabled(std::get<bool>(evt.param1));
        break;
    }
    ee_event_queue.pop();
  }
}

void InputManager::register_command(const CommandBinding::Source source,
                                    const CommandBinding bind) {
  switch (source) {
    case CommandBinding::Source::CONTROLLER:
      if (m_command_binds.controller_binds.find(bind.host_key) ==
          m_command_binds.controller_binds.end()) {
        m_command_binds.controller_binds[bind.host_key] = {};
      }
      m_command_binds.controller_binds[bind.host_key].push_back(bind);
      break;
    case CommandBinding::Source::KEYBOARD:
      if (m_command_binds.keyboard_binds.find(bind.host_key) ==
          m_command_binds.keyboard_binds.end()) {
        m_command_binds.keyboard_binds[bind.host_key] = {};
      }
      m_command_binds.keyboard_binds[bind.host_key].push_back(bind);
      break;
    case CommandBinding::Source::MOUSE:
      if (m_command_binds.mouse_binds.find(bind.host_key) == m_command_binds.mouse_binds.end()) {
        m_command_binds.mouse_binds[bind.host_key] = {};
      }
      m_command_binds.mouse_binds[bind.host_key].push_back(bind);
      break;
  }
}

std::optional<std::shared_ptr<PadData>> InputManager::get_current_data(const int port) const {
  if (m_data.find(port) == m_data.end()) {
    return {};
  }
  return m_data.at(port);
}

GameController* InputManager::controller_for_port(const int port) noexcept {
  const auto mapping = m_controller_port_mapping.find(port);
  if (mapping == m_controller_port_mapping.end() || mapping->second < 0 ||
      static_cast<size_t>(mapping->second) >= m_available_controllers.size()) {
    return nullptr;
  }
  const auto& controller = m_available_controllers.at(mapping->second);
  return controller ? controller.get() : nullptr;
}

const GameController* InputManager::controller_for_port(const int port) const noexcept {
  const auto mapping = m_controller_port_mapping.find(port);
  if (mapping == m_controller_port_mapping.end() || mapping->second < 0 ||
      static_cast<size_t>(mapping->second) >= m_available_controllers.size()) {
    return nullptr;
  }
  const auto& controller = m_available_controllers.at(mapping->second);
  return controller ? controller.get() : nullptr;
}

std::string InputManager::get_controller_name(const int controller_id) {
  if (controller_id < 0 || static_cast<size_t>(controller_id) >= m_available_controllers.size()) {
    return "";
  }
  return m_available_controllers.at(controller_id)->get_name();
}

std::string InputManager::get_current_bind(const int port,
                                           const InputDeviceType device_type,
                                           const bool buttons,
                                           const int input_idx,
                                           const bool analog_for_minimum) {
  std::vector<InputBindingInfo> binding_info;
  switch (device_type) {
    case InputDeviceType::CONTROLLER:
      if (const auto* controller = controller_for_port(port);
          controller && m_settings->controller_binds.find(controller->get_guid()) !=
                             m_settings->controller_binds.end()) {
        binding_info =
            m_settings->controller_binds.at(controller->get_guid()).lookup_button_binds(
                (PadData::ButtonIndex)input_idx);
      }
      break;
    case InputDeviceType::KEYBOARD:
      if (!buttons) {
        binding_info = m_settings->keyboard_binds.lookup_analog_binds(
            (PadData::AnalogIndex)input_idx, analog_for_minimum);
      } else {
        binding_info =
            m_settings->keyboard_binds.lookup_button_binds((PadData::ButtonIndex)input_idx);
      }
      break;
    case InputDeviceType::MOUSE:
      binding_info = m_settings->mouse_binds.lookup_button_binds((PadData::ButtonIndex)input_idx);
      break;
  }
  if (binding_info.empty()) {
    return "";
  }
  return binding_info.front().host_name;
}

int InputManager::get_controller_index(const int port) {
  if (!controller_for_port(port)) {
    return 0;
  }
  return m_controller_port_mapping.at(port);
}

void InputManager::set_controller_for_port(const int controller_id, const int port) {
  if (port >= 0 && controller_id >= 0 &&
      static_cast<size_t>(controller_id) < m_available_controllers.size() &&
      m_available_controllers.at(controller_id)) {
    // Reset inputs as this device won't be able to be read from again!
    clear_inputs();
    auto& controller = m_available_controllers.at(controller_id);
    m_controller_port_mapping[port] = controller_id;
    m_settings->controller_port_mapping[controller->get_guid()] = port;
    // NOTE - only tracking port 0 for now
    if (port == 0) {
      m_settings->last_selected_controller_guid = controller->get_guid();
    }
    m_settings->save_settings();
  }
}

bool InputManager::controller_has_led(const int port) {
  const auto* controller = controller_for_port(port);
  return controller && controller->has_led();
}

bool InputManager::controller_has_rumble(const int port) {
  const auto* controller = controller_for_port(port);
  return controller && controller->has_rumble();
}

bool InputManager::controller_has_pressure_sensitivity_support(const int port) {
  const auto* controller = controller_for_port(port);
  return controller && controller->has_pressure_sensitivity_support();
}

bool InputManager::controller_has_trigger_effect_support(const int port) {
  const auto* controller = controller_for_port(port);
  return controller && controller->has_trigger_effect_support();
}

int InputManager::controller_send_rumble(int port, u8 low_intensity, u8 high_intensity) {
  auto* controller = controller_for_port(port);
  if (!controller) {
    return 0;
  }
  return controller->send_rumble(low_intensity, high_intensity);
}

void InputManager::controller_send_trigger_rumble(const int port,
                                                  const u16 left_rumble,
                                                  const u16 right_rumble,
                                                  const u32 duration_ms) {
  auto* controller = controller_for_port(port);
  if (!controller) {
    return;
  }
  controller->send_trigger_rumble(left_rumble, right_rumble, duration_ms);
}

void InputManager::controller_clear_trigger_effect(const int port,
                                                   dualsense_effects::TriggerEffectOption option) {
  auto* controller = controller_for_port(port);
  if (!controller) {
    return;
  }
  controller->clear_trigger_effect(option);
}

void InputManager::controller_send_trigger_effect_feedback(
    const int port,
    dualsense_effects::TriggerEffectOption option,
    u8 position,
    u8 strength) {
  auto* controller = controller_for_port(port);
  if (!controller) {
    return;
  }
  controller->send_trigger_effect_feedback(option, position, strength);
}

void InputManager::controller_send_trigger_effect_vibrate(
    const int port,
    dualsense_effects::TriggerEffectOption option,
    u8 position,
    u8 amplitude,
    u8 frequency) {
  auto* controller = controller_for_port(port);
  if (!controller) {
    return;
  }
  controller->send_trigger_effect_vibrate(option, position, amplitude, frequency);
}

void InputManager::controller_send_trigger_effect_weapon(
    const int port,
    dualsense_effects::TriggerEffectOption option,
    u8 start_position,
    u8 end_position,
    u8 strength) {
  auto* controller = controller_for_port(port);
  if (!controller) {
    return;
  }
  controller->send_trigger_effect_weapon(option, start_position, end_position, strength);
}

bool InputManager::set_trigger_effects_enabled(bool enabled) {
  controller_clear_trigger_effect(0, dualsense_effects::TriggerEffectOption::BOTH);
  return m_settings->enable_trigger_effects = enabled;
};

void InputManager::enqueue_set_controller_led(const int port,
                                              const u8 red,
                                              const u8 green,
                                              const u8 blue) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::SET_CONTROLLER_LED, port, red, green, blue});
}

void InputManager::set_controller_led(const int port, const u8 red, const u8 green, const u8 blue) {
  auto* controller = controller_for_port(port);
  if (!controller) {
    return;
  }
  controller->set_led(red, green, blue);
}

void InputManager::enqueue_update_rumble(const int port,
                                         const u8 low_intensity,
                                         const u8 high_intensity) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::UPDATE_RUMBLE, port, low_intensity, high_intensity, {}});
}

void InputManager::enable_keyboard(const bool enabled) {
  m_settings->keyboard_enabled = enabled;
  if (!m_settings->keyboard_enabled) {
    // Reset inputs as this device won't be able to be read from again!
    clear_inputs();
  }
}

void InputManager::enqueue_update_mouse_options(const bool enabled,
                                                const bool control_camera,
                                                const bool control_movement) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push(
      {EEInputEventType::UPDATE_MOUSE_OPTIONS, enabled, control_camera, control_movement, {}});
}

void InputManager::update_mouse_options(const bool enabled,
                                        const bool control_camera,
                                        const bool control_movement) {
  m_mouse_enabled = enabled;
  if (!m_mouse_enabled) {
    // Reset inputs as this device won't be able to be read from again!
    clear_inputs();
  }
  // Switch relevant flags over related to the mouse
  m_mouse.enable_camera_control(enabled && control_camera);
  m_mouse.enable_movement_control(enabled && control_movement);
}

void InputManager::set_wait_for_bind(const InputDeviceType device_type,
                                     const bool for_analog,
                                     const bool for_minimum_analog,
                                     const int input_idx) {
  m_waiting_for_bind = InputBindAssignmentMeta();
  m_waiting_for_bind->device_type = device_type;
  m_waiting_for_bind->pad_idx = input_idx;
  m_waiting_for_bind->for_analog = for_analog;
  m_waiting_for_bind->for_analog_minimum = for_minimum_analog;
  m_waiting_for_bind->seen_keyboard_confirm_up = false;
  m_waiting_for_bind->keyboard_confirmation_binds =
      m_settings->keyboard_binds.lookup_button_binds(PadData::CROSS);
  m_waiting_for_bind->seen_controller_confirm_neutral = false;
  const auto* controller = controller_for_port(0);
  if (controller && m_settings->controller_binds.find(controller->get_guid()) !=
                       m_settings->controller_binds.end()) {
    m_waiting_for_bind->controller_confirmation_binds =
        m_settings->controller_binds.at(controller->get_guid()).lookup_button_binds(
            PadData::CROSS);
  }
  if (g_game_version == GameVersion::Jak1) {
    auto keyboard_circle_binds = m_settings->keyboard_binds.lookup_button_binds(PadData::CIRCLE);
    m_waiting_for_bind->keyboard_confirmation_binds.insert(
        m_waiting_for_bind->keyboard_confirmation_binds.end(), keyboard_circle_binds.begin(),
        keyboard_circle_binds.end());
    if (controller && m_settings->controller_binds.find(controller->get_guid()) !=
                         m_settings->controller_binds.end()) {
      auto controller_circle_binds =
          m_settings->controller_binds.at(controller->get_guid()).lookup_button_binds(
              PadData::CIRCLE);
      m_waiting_for_bind->controller_confirmation_binds.insert(
          m_waiting_for_bind->controller_confirmation_binds.end(), controller_circle_binds.begin(),
          controller_circle_binds.end());
    }
  }
}

void InputManager::set_camera_sens(const float xsens, const float ysens) {
  m_mouse.set_camera_sens(xsens, ysens);
}

void InputManager::reset_input_bindings_to_defaults(const int port,
                                                    const InputDeviceType device_type) {
  switch (device_type) {
    case InputDeviceType::CONTROLLER:
      if (auto* controller = controller_for_port(port);
          controller && m_settings->controller_binds.find(controller->get_guid()) !=
                             m_settings->controller_binds.end()) {
        m_settings->controller_binds.at(controller->get_guid()).set_bindings(
            DEFAULT_CONTROLLER_BINDS);
      }
      break;
    case InputDeviceType::KEYBOARD:
      m_settings->keyboard_binds.set_bindings(DEFAULT_KEYBOARD_BINDS);
      break;
    case InputDeviceType::MOUSE:
      m_settings->mouse_binds.set_bindings(DEFAULT_MOUSE_BINDS);
      break;
  }
}

void InputManager::enqueue_set_auto_hide_mouse(const bool auto_hide_mouse) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({EEInputEventType::SET_AUTO_HIDE_MOUSE, auto_hide_mouse, {}, {}, {}});
}

void InputManager::enqueue_controller_clear_trigger_effect(
    const int port,
    const dualsense_effects::TriggerEffectOption option) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_CLEAR_TRIGGER_EFFECT,
                       .param1 = port,
                       .param2 = option});
}

void InputManager::enqueue_controller_send_trigger_effect_feedback(
    const int port,
    const dualsense_effects::TriggerEffectOption option,
    const u8 position,
    const u8 strength) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_FEEDBACK,
                       .param1 = port,
                       .param2 = option,
                       .param3 = position,
                       .param4 = strength});
}

void InputManager::enqueue_controller_send_trigger_effect_vibrate(
    const int port,
    const dualsense_effects::TriggerEffectOption option,
    const u8 position,
    const u8 amplitude,
    const u8 frequency) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_VIBRATE,
                       .param1 = port,
                       .param2 = option,
                       .param3 = position,
                       .param4 = amplitude,
                       .param5 = frequency});
}

void InputManager::enqueue_controller_send_trigger_effect_weapon(
    const int port,
    const dualsense_effects::TriggerEffectOption option,
    const u8 start_position,
    const u8 end_position,
    const u8 strength) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_SEND_TRIGGER_EFFECT_WEAPON,
                       .param1 = port,
                       .param2 = option,
                       .param3 = start_position,
                       .param4 = end_position,
                       .param5 = strength});
}

void InputManager::enqueue_controller_send_trigger_rumble(const int port,
                                                          const u16 left_rumble,
                                                          const u16 right_rumble,
                                                          const u32 duration_ms) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::CONTROLLER_SEND_TRIGGER_RUMBLE,
                       .param1 = port,
                       .param2 = left_rumble,
                       .param3 = right_rumble,
                       .param4 = duration_ms});
}

void InputManager::enqueue_set_trigger_effects_enabled(const bool enabled) {
  const std::lock_guard<std::mutex> lock(m_event_queue_mtx);
  ee_event_queue.push({.type = EEInputEventType::SET_TRIGGER_EFFECTS_ENABLED, .param1 = enabled});
}

void InputManager::set_auto_hide_mouse(const bool auto_hide_mouse) {
  m_auto_hide_mouse = auto_hide_mouse;
  if (!auto_hide_mouse) {
    hide_cursor(false);
  }
}

void InputManager::clear_inputs() {
  // Reset inputs as this device won't be able to be read from again!
  for (auto& [port, data] : m_data) {
    data->clear();
  }
}
