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
  # discovery_info
  #
  # Convert Tasmota discovery fields to the canonical remote info map
  static def discovery_info(config)
    var info = {}
    var value = config.find("dn")
    if value != nil   info['name'] = value       end
    value = config.find("sw")
    if value != nil   info['version'] = value    end
    value = config.find("mac")
    if value != nil   info['mac'] = value        end
    value = config.find("md")
    if value != nil   info['hardware'] = value   end
    return info
  end

  # Update remote metadata from discovery, returning true only on change
  def set_info_from_discovery(config)
    var info = self.discovery_info(config)
    if size(self.info) == size(info)
      var same = true
      for key: info.keys()
        if self.info.find(key) != info[key]   same = false  break   end
      end
      if same   return false   end
    end
    self.info = info
    return true
  end

  #############################################################
  # generate_config_from_discovery
  #
  # Auto-generate endpoint configuration from discovery config and sensors
  # Similar to Matter_UI.generate_config_from_status() but uses discovery data
  static def generate_config_from_discovery(device, config, sensors)
    var config_list = []

    # rl entries are: 0=unused, 1=relay, 2=light, 3=shutter
    var rl = config.find("rl")
    if rl != nil && size(rl) > 0
      var lt_st = config.find("lt_st", 0)
      var so = config.find("so", {})
      var pwm_multi = bool(so.find("68", 0))
      for i: 0 .. size(rl) - 1
        var relay_type = rl[i]
        if relay_type == 1
          config_list.push({'type': 'light0', 'relay': i + 1})
        elif relay_type == 2
          var light_type = 'light0'
          if pwm_multi || lt_st == 1
            light_type = 'light1'
          elif lt_st == 2
            light_type = 'light2'
          elif lt_st >= 3
            # RGB, RGBW and RGBCW all expose the Matter extended-color features
            light_type = 'light3'
          end
          config_list.push({'type': light_type, 'relay': i + 1})
        elif relay_type == 3
          # No remote shutter bridge plug-in exists; never expose its relays as switches.
          log(f"MTR: MQTT discovery skipping unsupported shutter relay {i + 1}", 3)
        end
      end
    end

    if sensors != nil
      config_list += device.autoconf.autoconf_sensors_list(sensors.find("sn", {}))
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

      # RESULT is often partial; dispatch the merged snapshot so unrelated
      # endpoints retain their last known fields.
      self.dispatch_cb(11, self.cached_state)

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
