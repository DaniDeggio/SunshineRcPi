/**
 * @file src/platform/linux/picamera_capture.h
 * @brief Raspberry Pi PiCamera capture backend declarations.
 */
#pragma once

#include <string>
#include <vector>

#include "src/platform/common.h"

namespace platf::picamera {

  /**
   * @brief Factory for PiCamera-based display capture.
   * @return PiCamera capture implementation.
   */
  std::shared_ptr<platf::display_t> create_display(const std::string &device, const video::config_t &config);

  /**
   * @brief PiCamera capture specific initialization hook.
   * @return true on success.
   */
  bool initialize();

  /**
   * @brief Enumerate available PiCamera capture sources.
   * @return Vector containing a single entry when capture is available.
   */
  std::vector<std::string> display_names();

}  // namespace platf::picamera
