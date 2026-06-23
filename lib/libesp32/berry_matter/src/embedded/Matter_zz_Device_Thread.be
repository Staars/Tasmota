#
# Matter_zz_Device_Thread.be - Thread-specific Matter device (commissionee)
#
# Copyright (C) 2026  Stephan Hadinger, Christian Baars & Theo Arends
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

#@ solidify:Matter_Device_Thread,weak
#if USE_MATTER_THREAD
#################################################################################
# Matter_Device_Thread
#
# Thread-specific Matter device. Subclasses `Matter_Device_BLE` to reuse the
# BLE GATT bring-up, BTP wiring, GATT event dispatcher and message
# multiplexing. Overrides the transport layer to use OpenThread UDP instead of
# Wi-Fi UDP, adds native OT SRP client management, and provides Thread network
# lifecycle (dataset provisioning, role tracking, radio reclaim).
#
# When the device is already commissioned, BLE is skipped entirely and the
# device rejoins Thread directly from persisted OT settings.
#################################################################################
class Matter_Device_Thread : Matter_Device_BLE
    # Thread-specific fields (BLE fields inherited from Matter_Device_BLE)
    var thread_dataset                 # bytes: stored Thread Operational Dataset TLV
    var srp_pase_instance              # string: SRP instance name for PASE (nil if not active)
    var srp_host_announced             # bool: srp_announce_hostnames already invoked
    var _srp_fabrics                   # list: fabrics collected during commissioning (before start)
    var case_grace_until               # int millis: deadline to wait for CASE after AddNOC
    var packets_sent                   # list: OT UDP packets awaiting ack (retransmission)
    var pending_radio_reclaim
    var wifi_was_up                    # bool: last known WiFi state (for detecting OFF transitions)
    var ot_started                     # bool: OpenThread initialized
    var thread_connected               # bool: Thread network attached

    #############################################################
    # init — full custom, does NOT call super.init()
    #
    # Structure mirrors Matter_Device_BLE.init() but adds OT init,
    # uses Matter_Thread_Commissioning, skips BLE when already
    # commissioned, and turns WiFi OFF during BLE commissioning.
    #############################################################
    def init()
        import global
        if global.matter_device
            global.matter_device.stop()
            global.matter_device = self
            tasmota.gc()
        end
        matter.profiler = matter.Profiler()

        self.plugins = []
        self.plugins_persist = false
        self.plugins_config_remotes = {}
        self.next_ep = self.EP
        self.ipv4only = false
        self.commissioning = matter.Commissioning_Thread(self)
        self.load_param()
        self.sessions = matter.Session_Store(self)
        self.sessions.load_fabrics()
        self._srp_fabrics = []
        self.tick = 0
        self.message_handler = matter.MessageHandler(self)
        self.events = matter.EventHandler(self)
        self.pending_radio_reclaim = false
        self.autoconf_device()
        tasmota.add_driver(self)

        # Initialize OpenThread
        self.ot_started = false
        self.thread_connected = false
        self.srp_host_announced = false
        self.packets_sent = []
        try
            import OT
            self.ot_started = true
            log("MTR: OpenThread initialized", 2)
        except .. as e, m
            log(format("MTR: OpenThread init FAILED: %s %s", str(e), str(m)), 2)
        end

        var commissioned = self.sessions.count_active_fabrics() > 0
        if commissioned
            # Already commissioned — rejoin Thread, no BLE needed
            try
                import OT
                OT.start()
                log("MTR: Thread dataset restored from OT settings, rejoining network", 2)
            except .. as e, m
                log(format("MTR: Thread rejoin FAILED: %s %s", str(e), str(m)), 2)
            end
            log("MTR: already commissioned, skipping BLE advertising", 2)
        else
            # Not commissioned — set up BLE GATT for commissioning
            self.ble_ready = false
            try
                import BLE
                import cb
                self.cbuf = bytes(-255)
                var cbp = cb.gen_cb(/e,o,u,h->self.cb(e,o,u,h))
                BLE.conn_cb(cbp, self.cbuf)
                self.current_func = /->self.init_C1()
                BLE.set_svc("FFF6")
                self.btp = matter.BTP(self)
                self.commissioning.init_basic_commissioning()
                tasmota.cmd("wifi 0")
                self.pending_radio_reclaim = true
                tasmota.add_fast_loop(/-> BLE.loop())
                self.init_light()
                self.ble_ready = true
                log(format("MTR: start MATTER Thread+BLE commissionee, discriminator:%i", self.root_discriminator), 1)
            except .. as e, m
                log(format("MTR: BLE init FAILED — device cannot be commissioned via BLE: %s %s", str(e), str(m)), 2)
            end
        end
    end

    # Default empty hook — overridden in root script for app-specific feedback
    def init_light()
    end

    #############################################################
    # handle_disconnect — Thread override
    #
    # Unlike Matter_Device_BLE, Thread does not track WiFi-specific
    # fields (deferred_connect_network, last_wifi_result, etc.).
    # Delegates entirely to check_final() which has 90s CASE grace.
    #############################################################
    def handle_disconnect()
        log("MTR: BLE disconnected, delegating to check_final()", 2)
        self.check_if_commissioned = true
    end

    #############################################################
    # Override autoconf to use Thread Root plugin
    #############################################################
    def autoconf_device()
        import json
        if size(self.plugins) > 0   return end
        if (self.autoconf == nil)   self.autoconf = matter.Autoconf(self)   end
        if !self.plugins_persist
            self.plugins_config = self.autoconf.autoconf_device_map()
            self.plugins_config_remotes = {}
            self.adjust_next_ep()
        end
        self.plugins.push(matter.Plugin_Root_Thread(self, 0, self.plugins_config.find("0", {})))
        log("MTR: Configuring endpoints", 2)
        log(format("MTR:   endpoint = %5i type:%s%s", 0, 'root(thread)', ''), 2)
        self.plugins.push(matter.Plugin_Aggregator(self, 1, {}))
        log(format("MTR:   endpoint = %5i type:%s%s", 1, 'aggregator', ''), 2)
        var endpoints = self.k2l_num(self.plugins_config)
        for ep: endpoints
            if ep == 0  continue end
            try
                var plugin_conf = self.plugins_config[str(ep)]
                var pi_class_name = plugin_conf.find('type')
                if pi_class_name == nil   continue end
                if pi_class_name == 'root'  continue end
                var pi_class = self.plugins_classes.find(pi_class_name)
                if pi_class == nil  continue  end
                var pi = pi_class(self, ep, plugin_conf)
                self.plugins.push(pi)
                log(format("MTR:   endpoint = %5i type:%s%s", ep, pi_class_name, self.conf_to_log(plugin_conf)), 2)
            except .. as e, m
                log(format("MTR: Exception %s|%s", str(e), str(m)), 2)
            end
        end
        if !self.plugins_persist && self.sessions.count_active_fabrics() > 0
            self.plugins_persist = true
            self.save_param()
        end
    end

    #############################################################
    # Thread role callback (polled from every_50ms)
    #############################################################
    def ot_state_changed(role)
        var roles = ["disabled", "detached", "child", "router", "leader"]
        var role_str = (role >= 0 && role < size(roles)) ? roles[role] : "unknown"
        log(format("MTR: Thread role changed: %s (%d)", role_str, role), 2)
        if role >= 2
            if !self.thread_connected
                self.thread_connected = true
                log("MTR: Thread network attached", 2)
                self.start()
                if self.commissioning.is_commissioning_open()
                    self.commissioning.mdns_announce_PASE()
                end
            end
        else
            self.thread_connected = false
        end
    end

    #############################################################
    # Start OT UDP + SRP
    #############################################################
    def start()
        if self.started   return end
        self.started = true
        import OT
        OT.udp_open(self.UDP_PORT)
        self.packets_sent = []
        self._start_srp_client()
        self.srp_announce_hostnames()
        tasmota.set_timer(2000, /-> self._log_srp_state())
        tasmota.set_timer(8000, /-> self._log_srp_state())
        log("MTR: Thread device started, OT UDP on port " + str(self.UDP_PORT), 2)
    end

    def stop()
        tasmota.remove_driver(self)
        try
            import OT
            OT.udp_close()
            OT.stop()
        except .. as e, m
            log(format("MTR: OT.stop FAILED: %s %s", str(e), str(m)), 2)
        end
        self.started = false
    end

    #############################################################
    # Message send — BLE (delegated to super) + OT UDP
    #############################################################
    def msg_send(msg)
        if msg.remote_ip != "BLE"
            import OT
            var packet = matter.UDPPacket_sent(msg)
            try
                OT.udp_send(packet.addr, packet.port, packet.raw)
                if tasmota.loglevel(4)
                    log(format("MTR: OT UDP sent %i bytes to [%s]:%i", size(packet.raw), packet.addr, packet.port), 4)
                end
            except .. as e, m
                log(format("MTR: OT UDP send FAILED: %s %s", str(e), str(m)), 2)
            end
            if packet.msg_id
                self.packets_sent.push(packet)
            end
        else
            super(self).msg_send(msg)
        end
    end

    #############################################################
    # ACK tracking — OT UDP packet removal
    #############################################################
    def received_ack(msg)
        self.heart_beat()
        if msg.remote_ip == "BLE"  return end
        var id = msg.ack_message_counter
        var exch = msg.exchange_id
        if id == nil  return end
        var idx = 0
        while idx < size(self.packets_sent)
            var packet = self.packets_sent[idx]
            if packet.msg_id == id && packet.exchange_id == exch
                self.packets_sent.remove(idx)
                if tasmota.loglevel(4)
                    log("MTR: OT UDP removed acked packet id=" + str(id), 4)
                end
            else
                idx += 1
            end
        end
    end

    #############################################################
    # Retransmit unacked OT UDP packets with exponential backoff
    #############################################################
    def _resend_packets()
        import OT
        var idx = 0
        while idx < size(self.packets_sent) && idx < 4
            var packet = self.packets_sent[idx]
            if tasmota.time_reached(packet.next_try)
                if packet.retries <= 5
                    log(format("MTR: OT UDP resend id=%i retry=%i", packet.msg_id, packet.retries), 3)
                    try
                        OT.udp_send(packet.addr, packet.port, packet.raw)
                    except .. as e, m
                        log(format("MTR: OT UDP resend FAILED: %s %s", str(e), str(m)), 2)
                    end
                    packet.next_try = tasmota.millis() + matter.UDPServer._backoff_time(packet.retries)
                    packet.retries += 1
                    idx += 1
                else
                    self.packets_sent.remove(idx)
                    log(format("MTR: OT UDP unacked packet [%s]:%i id=%i", packet.addr, packet.port, packet.msg_id), 3)
                end
            else
                idx += 1
            end
        end
    end

    #############################################################
    # Check_final — 90s CASE grace for Thread
    #############################################################
    def check_final()
        if self.commissioning && self.commissioning.is_commissioning_open()
            return
        end
        if self.thread_dataset != nil && !self.thread_connected
            return
        end
        if self.sessions.count_active_fabrics() > 0
            log("MTR: Successfully finished BLE commissioning.")
            if self.have_light
                import light
                light.set({'bri':50,'hue':120})
            end
            self.check_if_commissioned = false
            return
        end
        var has_candidate = self.sessions && self.sessions.fabrics && size(self.sessions.fabrics) > 0
        if has_candidate
            if self.case_grace_until == nil
                self.case_grace_until = tasmota.millis() + 90000
                log("MTR: candidate fabric present, waiting up to 90s for CASE", 3)
                return
            end
            if !tasmota.time_reached(self.case_grace_until)
                return
            end
            log("MTR: commissioning failure (CASE did not complete)")
            try  import OT  OT.srp_stop()  except .. end
            tasmota.cmd("restart 1")
        else
            log("MTR: commissioning ended without AddNOC, no fabric expected", 3)
        end
        self.case_grace_until = nil
        self.check_if_commissioned = false
    end

    ###########################################################
    # SRP — native OT SRP client
    ###########################################################
    def srp_announce_hostnames()
        import string
        import OT
        try
            if !self.srp_host_announced
                var eui64 = OT.get_eui64()
                var hostname = string.tolower(string.replace(eui64, ':', ''))
                OT.srp_set_hostname(hostname)
                self.srp_host_announced = true
                log(format("MTR: SRP hostname set (native OT): %s", hostname), 3)
            elif tasmota.loglevel(4)
                log(format("MTR: SRP hostname already announced (native state=%s)",
                           str(OT.srp_get_host_state())), 4)
            end
        except .. as e, m
            log(format("MTR: SRP host FAILED: %s %s", str(e), str(m)), 2)
        end
        try
            var fabs = self.sessions.fabrics
            if fabs != nil
                var i = 0
                while i < size(fabs)
                    var fabric = fabs[i]
                    if fabric != nil && fabric._persist
                        if fabric.get_device_id() != nil && fabric.get_fabric_id() != nil
                            self.srp_announce_op_discovery(fabric)
                        end
                    end
                    i += 1
                end
            end
        except .. as e, m
            log(format("MTR: SRP op announce FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    def srp_announce_op_discovery(fabric)
        import string
        import OT
        try
            var device_id = fabric.get_device_id().copy().reverse()
            var k_fabric = fabric.get_fabric_compressed()
            var op_node = string.tolower(k_fabric.tohex() + "-" + device_id.tohex())
            log("MTR: SRP Operational Discovery node = " + op_node, 3)
            var found = false
            var i = 0
            while i < size(self._srp_fabrics)
                if self._srp_fabrics[i] == fabric
                    found = true    break
                end
                i += 1
            end
            if !found
                self._srp_fabrics.push(fabric)
            end
            var subtypes_str = "_I" + string.tolower(k_fabric.tohex())
            var txt_str = "SII=500,SAI=300,SAT=4000,T=0"
            OT.srp_add_service(op_node, "_matter._tcp", self.UDP_PORT, subtypes_str, txt_str)
            log(format("MTR: SRP registered _matter._tcp instance '%s' (native OT)", op_node), 3)
        except .. as e, m
            log(format("MTR: SRP op_discovery FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    def srp_announce_PASE()
        import crypto
        import string
        import OT
        try
            var instance = string.tolower(crypto.random(8).tohex())
            var disc = self.commissioning.commissioning_discriminator
            var subtypes_str = format("_L%i,_S%i,_V%i,_CM1",
                disc & 0xFFF, (disc & 0xF00) >> 8, self.VENDOR_ID)
            var txt_str = format("VP=%d+%d,D=%d,CM=1,T=0,SII=500,SAI=300,SAT=4000",
                self.VENDOR_ID, self.PRODUCT_ID, disc)
            OT.srp_add_service(instance, "_matterc._udp", self.UDP_PORT, subtypes_str, txt_str)
            self.srp_pase_instance = instance
            log(format("MTR: SRP registered _matterc._udp instance '%s' (native OT)", instance), 2)
        except .. as e, m
            log(format("MTR: SRP PASE announce FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    def srp_remove_PASE()
        import OT
        try
            if self.srp_pase_instance
                OT.srp_remove_service(self.srp_pase_instance, "_matterc._udp")
                log(format("MTR: SRP removed _matterc._udp instance '%s'", self.srp_pase_instance), 3)
                self.srp_pase_instance = nil
            end
        except .. as e, m
            log(format("MTR: SRP remove PASE FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    def srp_remove_op_discovery(fabric)
        import string
        import OT
        try
            var device_id = fabric.get_device_id().copy().reverse()
            var k_fabric = fabric.get_fabric_compressed()
            var op_node = string.tolower(k_fabric.tohex() + "-" + device_id.tohex())
            OT.srp_remove_service(op_node, "_matter._tcp")
            log(format("MTR: SRP removed _matter._tcp instance '%s'", op_node), 3)
        except .. as e, m
            log(format("MTR: SRP remove FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    ###########################################################
    # SRP server discovery + client start
    ###########################################################
    def _aloc_from_rloc(rloc16)
        import string
        import OT
        var a = OT.get_ipaddr()
        if a == nil    return nil end
        var i = 0
        while i < size(a)
            var s = a[i]
            i += 1
            var idx = string.find(s, ":0:ff:fe00:")
            if idx >= 0
                return s[0 .. idx - 1] + ":0:ff:fe00:" + rloc16
            end
        end
        return nil
    end

    def _discover_srp_server()
        import string
        try
            import OT
            var services = OT.netdata_services()
            if services == nil    return nil end
            var unicast_addr = nil
            var unicast_port = 0
            var i = 0
            while i < size(services)
                var s = services[i]
                i += 1
                if type(s) != "string" || size(s) == 0    continue end
                if string.find(s, "SRP-anycast") >= 0
                    var rp = string.find(s, "rloc=0x")
                    if rp >= 0
                        var rloc16 = s[rp + 7 .. rp + 10]
                        var addr = self._aloc_from_rloc(rloc16)
                        if addr != nil
                            log(format("MTR: SRP server anycast %s:53", addr), 2)
                            return {"addr": addr, "port": 53}
                        end
                    end
                end
                if string.find(s, "SRP-unicast") >= 0 && unicast_addr == nil
                    var ob = string.find(s, "[")
                    var cb = string.find(s, "]:")
                    if ob >= 0 && cb >= 0
                        unicast_addr = s[ob + 1 .. cb - 1]
                        unicast_port = int(s[cb + 2 .. size(s) - 1])
                    end
                end
            end
            if unicast_addr != nil && unicast_port > 0
                log(format("MTR: SRP server unicast %s:%i (fallback)", unicast_addr, unicast_port), 2)
                return {"addr": unicast_addr, "port": unicast_port}
            end
        except .. as e, m
            log(format("MTR: SRP server discover FAILED: %s %s", str(e), str(m)), 2)
        end
        return nil
    end

    def _start_srp_client()
        import OT
        try
            if !self.srp_host_announced
                import string
                var eui64 = OT.get_eui64()
                var hostname = string.tolower(string.replace(eui64, ':', ''))
                OT.srp_set_hostname(hostname)
                self.srp_host_announced = true
                log(format("MTR: SRP hostname set (native OT): %s", hostname), 3)
            end
            OT.srp_set_lease_interval(7200, 1209600)
            log("MTR: SRP lease intervals set (7200s, 1209600s)", 3)
            log("MTR: SRP auto-start enabled, OT handles server discovery", 2)
        except .. as e, m
            log(format("MTR: SRP setup FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    def _log_srp_state()
        try
            import OT
            var host_st = OT.srp_get_host_state()
            var running  = OT.srp_is_running()
            var srv      = OT.srp_get_server()
            log(format("MTR: SRP host_state=%s running=%s server=%s",
                       str(host_st), str(running),
                       (srv != nil && size(srv) > 0) ? str(srv) : "<none>"), 2)
            var addrs = OT.get_ipaddr()
            if addrs != nil
                var i = 0
                while i < size(addrs)
                    log("MTR: own addr " + str(addrs[i]), 2)
                    i += 1
                end
            end
        except .. as e, m
            log(format("MTR: SRP state log FAILED: %s %s", str(e), str(m)), 2)
        end
        try
            import OT
            var services = OT.netdata_services()
            if services != nil && size(services) > 0
                var i = 0
                while i < size(services)
                    log("MTR: netdata svc " + str(services[i]), 2)
                    i += 1
                end
            else
                log("MTR: netdata svc <none>", 2)
            end
        except .. as e, m
            log(format("MTR: netdata svc log FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    #############################################################
    # Provision Thread network (called from NetworkCommissioning)
    #############################################################
    def provision_thread_network(dataset_tlv)
        if dataset_tlv == nil
            log("MTR: provision_thread_network FAILED: no dataset", 2)
            return
        end
        import OT
        self.thread_dataset = dataset_tlv
        OT.set_dataset(dataset_tlv)
        OT.start()
        log("MTR: Thread network provisioning started", 2)
    end

    #############################################################
    # Timer callbacks
    #############################################################
    def every_second()
        self.sessions.every_second()
        self.message_handler.every_second()
        self.events.every_second()
        self.commissioning.every_second()
        if self.check_if_commissioned == true
            self.check_final()
        end
    end

    def every_50ms()
        if self.current_func  self.current_func()  end
        self.tick += 1
        self.message_handler.every_50ms()
        # drain stuck BTP queue
        if self.btp && self.ble_ready && self.btp.can_send && size(self.btp.send_queue) > 0
            var p = self.btp.get_packet()
            self.btp.can_send = false
            self.ble_send(p)
        end
        # OT state poll
        if self.ot_started
            try
                import OT
                var role = OT.poll_state()
                if role != nil
                    self.ot_state_changed(role)
                end
            except .. as e, m
                log(format("MTR: OT state poll FAILED: %s %s", str(e), str(m)), 2)
            end
        end
        # radio reclaim on WiFi OFF transition
        if self.ot_started
            var wifi_up = tasmota.wifi().find("up") == true
            if (self.pending_radio_reclaim || (self.wifi_was_up && !wifi_up)) && !wifi_up
                try
                    import OT
                    OT.radio_reclaim()
                    log("MTR: 802.15.4 radio reclaimed after WiFi shutdown", 2)
                except .. as e, m
                    log(format("MTR: radio reclaim FAILED: %s %s", str(e), str(m)), 2)
                end
            end
            self.pending_radio_reclaim = false
            self.wifi_was_up = wifi_up
        end
        # OT UDP receive
        if self.started
            import OT
            var count = 0
            while count < 4
                var pkt = nil
                try
                    pkt = OT.udp_poll()
                except .. as e, m
                    log(format("MTR: OT UDP poll FAILED: %s %s", str(e), str(m)), 2)
                    break
                end
                if pkt == nil  break end
                log(format("MTR: OT UDP recv %i bytes from [%s]:%i", size(pkt[0]), pkt[1], pkt[2]), 4)
                try
                    self.msg_received(pkt[0], pkt[1], pkt[2])
                except .. as e, m
                    log(format("MTR: OT UDP dispatch FAILED: %s %s", str(e), str(m)), 2)
                end
                count += 1
            end
            try
                self._resend_packets()
            except .. as e, m
                log(format("MTR: OT UDP resend FAILED: %s %s", str(e), str(m)), 2)
            end
        end
    end
end

matter.Device_Thread = Matter_Device_Thread
#else
class Matter_Device_Thread end
#endif //USE_MATTER_THREAD
