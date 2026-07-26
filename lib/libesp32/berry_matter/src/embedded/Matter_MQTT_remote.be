#
# Matter_MQTT_remote.be - implements an interface to query remotely Tasmota device via MQTT
#
# Copyright (C) 2024  Stephan Hadinger, Christian Baars & Theo Arends
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

import matter

#@ solidify:Matter_MQTT_remote,weak

#############################################################
# This class implements the MQTT transport for remote Tasmota devices.
#
# Subscriptions (4 total, per-device):
#   - tele/<topic>/STATE             - full state (TelePeriod, default 300s)
#   - tele/<topic>/SENSOR            - sensor data
#   - tele/<topic>/LWT               - online/offline status
#   - stat/<topic>/RESULT            - immediate command feedback
#
# Publishes:
#   - cmnd/<topic>/<Command>         - send commands to remote device
#
# Design principles:
#   - Subscribe-only, no polling
#   - Discovery handled globally by Matter_Device (not per-remote)
#   - Cached state for optimistic updates
#   - LWT handling for reachability

class Matter_MQTT_remote
  var device                                        # reference to matter_device
  var topic                                         # MQTT topic of the remote device
  var reachable                                     # is the device reachable
  var reachable_utc                                 # last tick when reachability was seen
  var info                                          # map with name, version, mac, hardware
  var cached_state                                  # cached state from RESULT/STATE
  var subscribed                                    # list of subscribed topics for cleanup

  #############################################################
  # init
  def init(device, topic)
    self.device = device
    self.topic = topic
    self.reachable = false
    self.reachable_utc = nil
    self.info = {}
    self.cached_state = {}
    self.subscribed = []

    # subscribe to topics
    self.subscribe_topics()
  end

  #############################################################
  # subscribe_topics
  #
  # Subscribe to all relevant MQTT topics
  def subscribe_topics()
    var base = "tele/" + self.topic + "/"
    var stat_base = "stat/" + self.topic + "/"

    # tele topics
    self.subscribe(base + "STATE", / topic, idx, data, databytes -> self.handle_mqtt_state(data))
    self.subscribe(base + "SENSOR", / topic, idx, data, databytes -> self.handle_mqtt_sensor(data))
    self.subscribe(base + "LWT", / topic, idx, data, databytes -> self.handle_mqtt_lwt(data))

    # stat topics
    self.subscribe(stat_base + "RESULT", / topic, idx, data, databytes -> self.handle_mqtt_result(data))
  end

  #############################################################
  # subscribe helper
  #
  # Subscribe to a topic and track it for cleanup
  def subscribe(topic, closure)
    import mqtt
    mqtt.subscribe(topic, closure)
    self.subscribed.push(topic)
  end

  #############################################################
  # unsubscribe helper
  #
  # Unsubscribe from a specific topic
  def unsubscribe(topic)
    import mqtt
    mqtt.unsubscribe(topic)
    self.subscribed.remove(topic)
  end

  #############################################################
  # generate_config_from_discovery
  #
  # Auto-generate endpoint configuration from discovery config
  # Similar to Matter_UI.generate_config_from_status() but uses discovery data
  def generate_config_from_discovery(config)
    var config_list = []

    # extract relay count from rl array
    var rl = config.find("rl")
    var power_cnt = 0
    if rl != nil && size(rl) > 0
      # count non-zero elements in rl array (first element is relay 1)
      for i: 0 .. size(rl) - 1
        if rl[i] != 0
          power_cnt = i + 1
        end
      end
    end

    # extract light type from lt_st
    # 0=none (relay only), 1=dimmer, 2=CT, 3=RGB
    var lt_st = config.find("lt_st", 0)

    # detect lights based on light type
    var light1, light2, light3
    if lt_st == 3 && power_cnt > 0
      light3 = power_cnt
      power_cnt -= 1
    elif lt_st == 2 && power_cnt > 0
      light2 = power_cnt
      power_cnt -= 1
    elif lt_st == 1 && power_cnt > 0
      light1 = power_cnt
      power_cnt -= 1
    end

    # remaining are relays
    for i: 1 .. power_cnt
      config_list.push({'type': 'light0', 'relay': i})
    end

    # add lights
    if light1 != nil
      config_list.push({'type': 'light1', 'relay': light1})
    end
    if light2 != nil
      config_list.push({'type': 'light2', 'relay': light2})
    end
    if light3 != nil
      config_list.push({'type': 'light3', 'relay': light3})
    end

    return config_list
  end

  #############################################################
  # handle_mqtt_state
  #
  # Handle tele/<topic>/STATE messages
  # This is the full state published every TelePeriod
  def handle_mqtt_state(data)
    if data == nil   return   end

    var j = data
    if type(j) == 'string'
      import json
      j = json.load(j)
    end

    if j != nil
      # mark device as alive
      self.device_is_alive(true)

      # merge into cached state
      self.merge_state(j)

      # dispatch to any registered callbacks
      self.dispatch_cb(11, j)

      log(f"MTR: MQTT STATE received from {self.topic}: {data}", 3)
    end
  end

  #############################################################
  # handle_mqtt_sensor
  #
  # Handle tele/<topic>/SENSOR messages
  def handle_mqtt_sensor(data)
    if data == nil   return   end

    var j = data
    if type(j) == 'string'
      import json
      j = json.load(j)
    end

    if j != nil
      # mark device as alive
      self.device_is_alive(true)

      # merge into cached state
      self.merge_state(j)

      # dispatch to any registered callbacks
      self.dispatch_cb(10, j)

      log(f"MTR: MQTT SENSOR received from {self.topic}: {data}", 3)
    end
  end

  #############################################################
  # handle_mqtt_lwt
  #
  # Handle tele/<topic>/LWT messages
  # "Online" or "Offline"
  def handle_mqtt_lwt(data)
    if data == nil   return   end

    if data == "Online"
      self.device_is_alive(true)
      log(f"MTR: MQTT device {self.topic} came online", 3)
    elif data == "Offline"
      self.device_is_alive(false)
      log(f"MTR: MQTT device {self.topic} went offline", 3)
    end
  end

  #############################################################
  # handle_mqtt_result
  #
  # Handle stat/<topic>/RESULT messages
  # This is the immediate command feedback
  def handle_mqtt_result(data)
    if data == nil   return   end

    var j = data
    if type(j) == 'string'
      import json
      j = json.load(j)
    end

    if j != nil
      # mark device as alive
      self.device_is_alive(true)

      # merge into cached state
      self.merge_state(j)

      # dispatch to any registered callbacks
      self.dispatch_cb(11, j)

      log(f"MTR: MQTT RESULT received from {self.topic}: {data}", 3)
    end
  end

  #############################################################
  # merge_state
  #
  # Merge new state into cached_state
  def merge_state(new_state)
    for k: new_state.keys()
      self.cached_state[k] = new_state[k]
    end
  end

  #############################################################
  # get/set remote_info map
  def get_info()      return self.info                    end
  def set_info(v)     self.info = v                       end
  def info_changed()  self.device.save_param()            end

  #############################################################
  # device is alive, update reachable_utc
  def device_is_alive(alive)
    if alive
      self.reachable = true
      self.reachable_utc = tasmota.rtc_utc()
    else
      self.reachable = false
    end
  end

  #############################################################
  # call_sync
  #
  # Synchronous (non-blocking for MQTT)
  # Returns nil - actual state update comes via RESULT subscription
  def call_sync(cmd, timeout)
    import mqtt
    import string
    # publish command via MQTT
    var space_idx = string.find(cmd, " ")
    var cmnd_topic = "cmnd/" + self.topic + "/"
    var payload = ""
    if space_idx > 0
      cmnd_topic += cmd[0 .. space_idx - 1]
      payload = cmd[space_idx + 1 ..]
    else
      cmnd_topic += cmd
    end
    mqtt.publish(cmnd_topic, payload)
    log(f"MTR: MQTT command sent to {self.topic}: {cmnd_topic} payload='{payload}'", 3)

    # return nil - actual state update comes via RESULT subscription
    return nil
  end

  #############################################################
  # dispatch_cb
  #
  # Dispatch status response to registered callbacks
  var async_cb_map

  def add_async_cb(cb, cmd)
    if self.async_cb_map == nil    self.async_cb_map = {}    end
    self.async_cb_map[cb] = cmd
  end

  def dispatch_cb(status, payload)
    if self.async_cb_map == nil    return    end
    for cb: self.async_cb_map.keys()
      var cmd_filter = self.async_cb_map[cb]
      if cmd_filter == nil || cmd_filter == status
        cb(status, payload, nil)
      end
    end
  end

  #############################################################
  # web_last_seen
  #
  # Show when the device was last seen
  def web_last_seen()
    var seconds = -1                      # default if no known value
    if self.reachable_utc != nil
      seconds = tasmota.rtc_utc() - self.reachable_utc
    end
    return matter.seconds_to_dhm(seconds)
  end

  #############################################################
  # close
  #
  # Unsubscribe from all topics and clean up
  def close()
    import mqtt
    for topic: self.subscribed
      mqtt.unsubscribe(topic)
      log(f"MTR: MQTT unsubscribed from {topic}", 3)
    end
    self.subscribed = []
    self.reachable = false
    self.cached_state = {}
    self.info = {}
    self.async_cb_map = nil
    log(f"MTR: MQTT remote {self.topic} closed", 3)
  end
end
matter.MQTT_remote = Matter_MQTT_remote
