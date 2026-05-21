#
# Matter_zz_Device_BLE.be - generic Matter-over-BLE commissionee
#
# Copyright (C) 2023  Stephan Hadinger & Theo Arends
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

#if USE_MI_EXT_GUI
import matter

#@ solidify:Matter_Device_BLE,weak

#################################################################################
# Matter_Device_BLE
#
# Generic Matter-over-BLE commissionee. Subclasses `matter.Device` and owns the
# BLE bring-up (GATT service FFF6, characteristics C1/C2, advertising), BTP
# wiring, msg multiplexing between BTP ("BLE" remote_ip marker) and UDP, the
# GATT callback dispatcher, deferred ConnectNetwork polling and commissioning
# finalization.
#
# Subclasses can override the following hooks for application-specific behavior:
#   - on_ble_init()              - called once at the end of init()
#   - on_ble_ready()             - called when the BLE stack signals ready (op 229)
#   - on_ble_disconnected()      - called on BLE disconnect (op 228)
#   - on_commissioning_success() - called from check_final() on success
#   - on_commissioning_failure() - called from check_final() on failure
#   - heart_beat()               - periodic feedback while commissioning is open
#################################################################################
class Matter_Device_BLE : Matter_Device
    var current_func, next_func, btp, send_over_udp, check_if_commissioned
    var ble_ready
    var deferred_connect_network       # holds msg frame for deferred ConnectNetwork response
    var cbuf                           # shared BLE I/O buffer (bytes(-255))

    def init()
        import cb
        import global
        import BLE
        if global.matter_device
            tasmota.remove_driver(global.matter_device)
            global.matter_device = self
            tasmota.gc()
        end
        self.cbuf = bytes(-255)
        matter.profiler = matter.Profiler()
        var cbp = cb.gen_cb(/e,o,u,h->self.cb(e,o,u,h))
        BLE.serv_cb(cbp, self.cbuf)
        self.current_func = /->self.init_C1()
        BLE.set_svc("FFF6")
        self.btp = matter.BTP(self)
        self.plugins = []
        self.plugins_persist = false
        self.plugins_config_remotes = {}
        self.next_ep = self.EP
        self.ipv4only = false
        self.commissioning = matter.Commissioning(self)
        self.load_param()
        self.sessions = matter.Session_Store(self)
        self.sessions.load_fabrics()
        self.tick = 0
        self.message_handler = matter.MessageHandler(self)
        self.events = matter.EventHandler(self)
        self.autoconf_device()
        self.commissioning.init_basic_commissioning()
        tasmota.add_fast_loop(/-> BLE.loop())
        tasmota.add_driver(self)
        self.on_ble_init()
        self.ble_ready = true
        log(format("BLE: start MATTER commissionee, discriminator:%i", self.root_discriminator), 1)
    end

    # ---------------------------------------------------------------------------
    # Subclass hooks - empty defaults
    # ---------------------------------------------------------------------------
    def on_ble_init()            end
    def on_ble_ready()           end
    def on_ble_disconnected()    end
    def on_commissioning_success() end
    def on_commissioning_failure() end
    def heart_beat()             end

    # ---------------------------------------------------------------------------
    # Driver lifecycle
    # ---------------------------------------------------------------------------
    def start()
        tasmota.when_network_up(def ()
            self._start_udp(self.UDP_PORT)
            self.commissioning.start_mdns_announce_hostnames()
            self.send_over_udp = true
        end)
    end

    def stop()
        tasmota.remove_driver(self)
    end

    # Override: prevent flush_socket during WiFi stabilization after BLE commissioning
    def network_down()
    end

    def every_second()
        self.sessions.every_second()
        self.message_handler.every_second()
        self.events.every_second()
        self.commissioning.every_second()
        if self.check_if_commissioned == true
            self.check_final()
        end
        self.poll_deferred_connect_network()
    end

    # Poll wifitest1 for deferred ConnectNetwork response
    def poll_deferred_connect_network()
        if self.deferred_connect_network == nil  return end
        var r = tasmota.cmd("wifitest1")["WiFiTest"]
        if r == "Testing"  return end

        var msg = self.deferred_connect_network
        self.deferred_connect_network = nil

        import global
        var TLV = matter.TLV
        var im = self.message_handler.im

        var ctx = matter.Path()
        ctx.endpoint = 0
        ctx.cluster = 0x0031
        ctx.command = 0x07              # ConnectNetworkResponse

        var cnresp = TLV.Matter_TLV_struct()
        if r == "Successful"
            cnresp.add_TLV(0, 0x04 #-TLV.U1-#, 0x00 #-SUCCESS-#)
            cnresp.add_TLV(1, 0x0D #-TLV.UTF2-#, format("ip6=%s", tasmota.wifi()['ip6local']))
            log("MTR: ConnectNetwork deferred -> Successful", 2)
            global.matter_device.start()
            try
                self.commissioning._mdns_announce_hostname(false)
            except .. as e, m
                log(format("MTR: mDNS hostname FAILED: %s %s", str(e), str(m)), 2)
            end
            for fabric : self.sessions.fabrics
                if fabric.get_device_id() && fabric.get_fabric_id()
                    try
                        self.commissioning.mdns_announce_op_discovery(fabric)
                    except .. as e, m
                        log(format("MTR: mDNS op_discovery FAILED: %s %s", str(e), str(m)), 2)
                    end
                end
            end
        else
            cnresp.add_TLV(0, 0x04 #-TLV.U1-#, 0x01 #-FAILURE-#)
            cnresp.add_TLV(1, 0x0D #-TLV.UTF2-#, format("wifi connect failed: %s", r))
            log(format("MTR: ConnectNetwork deferred -> Failed: %s", r), 2)
        end
        cnresp.add_TLV(2, 0x02 #-TLV.I4-#, 0)    # NetworkIndex

        var raw = bytes(32)
        im.invokeresponse2raw(raw, ctx, cnresp)

        var ret = matter.InvokeResponseMessage()
        ret.suppress_response = false
        ret.invoke_responses = [raw]

        im.send_queue.push(matter.IM_InvokeResponse(msg, ret))
        im.send_enqueued(self.message_handler)
    end

    def every_50ms()
        self.current_func()
        self.tick += 1
        self.message_handler.every_50ms()
    end

    def wait()
    end

    def then(func)
        self.next_func = func
        self.current_func = /->self.wait()
    end

    # ---------------------------------------------------------------------------
    # Message multiplexing: BLE/BTP vs UDP
    # ---------------------------------------------------------------------------
    def received_ack(msg)
        self.heart_beat()
        if msg.remote_ip != "BLE" && self.udp_server
            self.udp_server.received_ack(msg)
        end
    end

    def msg_send(msg)
        if msg.remote_ip == "BLE"
            self.btp.create_send_queue(msg.raw)
            self.heart_beat()
            tasmota.delay(30)
            if self.btp.can_send == false || self.ble_ready == false
                return
            end
            var p = self.btp.get_packet()
            self.btp.can_send = false
            self.ble_send(p)
        else
            self.udp_server.send_UDP(msg)
        end
    end

    def parse()
        var msg = self.btp
        var is_synced = msg.parse(self.cbuf[1..self.cbuf[0]])
        if !(msg.flags & matter.BTP.F_HANDSHAKE) && msg.combined_payload
            if (msg.flags & matter.BTP.F_END)
                if size(msg.combined_payload) == 0  return end
                if is_synced
                    log(format("BLE: <<< %i bytes", self.cbuf[0]))
                    self.message_handler.msg_received(msg.combined_payload, "BLE", 0)
                end
                msg.delete()
            end
        end
    end

    def handshake_ack()
        self.btp.create_handshake_response()
        self.ble_send(self.btp.serialize())
    end

    def check_final()
        if self.commissioning && self.commissioning.is_commissioning_open()
            return
        end
        import path
        if path.exists("_matter_fabrics.json")
            log("MTR: Successfully finished BLE commissioning.")
            self.on_commissioning_success()
        else
            log("MTR: commissioning failure")
            self.on_commissioning_failure()
            tasmota.cmd("restart 1")
        end
        self.check_if_commissioned = false
    end

    def ble_send(payload)
       import BLE
        self.cbuf[0] = size(payload)
        self.cbuf.setbytes(1, payload)
        BLE.set_chr("18EE2EF5-263D-4559-959F-4F9C429F9D12")
        BLE.run(211, true)
        self.ble_ready = false
        log(format("BLE: >>> %i bytes", self.cbuf[0]))
        self.then(/->self.wait())
        tasmota.defer(/->self.heart_beat())
        return true
    end

    # ---------------------------------------------------------------------------
    # GATT event dispatcher
    # ---------------------------------------------------------------------------
    def cb(error, op, uuid, handle)
        if op == 201
            log(format("BLE: Handles created: %s", self.cbuf[1..self.cbuf[0]].tohex()))
        elif op == 222
            self.parse()
        elif op == 224 || op == 225
            log(format("BLE: Subscribed to %s", op == 224 ? "notification" : "indication"))
            self.next_func = /->self.handshake_ack()
        elif op == 227
            log(format("BLE: peer MAC: %s", self.cbuf[1..self.cbuf[0]].tohex()))
        elif op == 228
            log("BLE: Disconnected")
            self.check_if_commissioned = true
            self.on_ble_disconnected()
        elif op == 229
            self.ble_ready = true
            log("BLE: stack ready")
            self.on_ble_ready()
            return
        end
        if error == 0 && op != 229
            self.current_func = self.next_func
        end
    end

    # ---------------------------------------------------------------------------
    # BLE GATT service setup
    # ---------------------------------------------------------------------------
    def init_C1()
       import BLE
        BLE.set_chr("18EE2EF5-263D-4559-959F-4F9C429F9D11")
        self.cbuf.setbytes(0, bytes("0100"))
        BLE.run(211, true, 8)
        self.then(/->self.init_C2())
    end

    def init_C2()
       import BLE
        BLE.set_chr("18EE2EF5-263D-4559-959F-4F9C429F9D12")
        self.cbuf.setbytes(0, bytes("0100"))
        BLE.run(211, true, 32)
        self.then(/->self.add_ScanResp())
    end

    def add_ADV()
        import BLE
        var descriptor = bytes("05025000A000") # connectable mode = 2, itvl_min = 50 ms, itvl_max = 100 ms
        self.cbuf.setbytes(0, descriptor)
        BLE.run(232)
        var payload = bytes("0201060B16F6FF00")
        payload.add(self.root_discriminator, 2)
        payload.add(self.VENDOR_ID, 2)
        payload.add(self.PRODUCT_ID, 2)
        payload.add(0x00)
        self.cbuf[0] = size(payload)
        self.cbuf.setbytes(1, payload)
        BLE.run(201)
        self.then(/->self.wait())
        log("BLE: advertising Matter accessory")
    end

    def add_ScanResp()
        import BLE
        var local_name = "Tasmota Matter"
        var payload = bytes("0201060008") + bytes().fromstring(local_name)
        payload[3] = size(local_name) + 1
        self.cbuf[0] = size(payload)
        self.cbuf.setbytes(1, payload)
        BLE.run(202)
        self.then(/->self.add_ADV())
    end
end

matter.Device_BLE = Matter_Device_BLE
#endif //USE_MI_EXT_GUI