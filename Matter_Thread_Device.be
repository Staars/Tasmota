# Matter Thread Device with BLE commissioning
import OT
import matter

# NOTE: The BTP (Bluetooth Transport Protocol) implementation now lives in
# upstream Matter_BTP.be as matter.BTP, guarded by #if USE_MI_EXT_GUI.
# Previously this file contained a duplicate local `class BTP`; removed in
# favour of the upstream class. Use matter.BTP(self) to instantiate and
# matter.BTP.F_HANDSHAKE / matter.BTP.F_END for static flag access.

class Matter_Thread_Commissioning : matter.Commissioning
    def start_mdns_announce_hostnames()
        self.device.srp_announce_hostnames()
    end
    def mdns_announce_PASE()
        self.device.srp_announce_PASE()
    end
    def mdns_remove_PASE()
        self.device.srp_remove_PASE()
    end
    def mdns_announce_op_discovery(fabric)
        self.device.srp_announce_op_discovery(fabric)
    end
    def mdns_remove_op_discovery(fabric)
        self.device.srp_remove_op_discovery(fabric)
    end
    def start_basic_commissioning(timeout_s, iterations, discriminator, salt, w0, L, admin_fabric)
        self.commissioning_open = tasmota.millis() + timeout_s * 1000
        self.commissioning_iterations = iterations
        self.commissioning_discriminator = discriminator
        self.commissioning_salt = salt
        self.commissioning_w0 = w0
        self.commissioning_L  = L
        self.commissioning_admin_fabric = admin_fabric
        if self.device.thread_connected
            self.mdns_announce_PASE()
        end
    end
end

###########################################################
# Thread-specific Root Node plugin
# Overrides NetworkCommissioning cluster (0x0031) for Thread
###########################################################
class Matter_Plugin_Root_Thread : matter.Plugin_Root
    static var CLUSTERS = matter.consolidate_clusters(_class, {
        0x0031: [0,1,2,3,4,5,6,7,9,0x0A],  # Network Commissioning — Thread variant
    })
    static var FEATURE_MAPS = {
        0x0006: 0x01,                       # On/Off: Lighting feature (bit 0)
        0x0008: 0x03,                       # Level Control: On/Off (bit 0) + Lighting (bit 1)
        0x0031: 0x02,                       # Thread Network Interface (bit 1)
        0x0046: 0x00,                       # ICD Management: no optional features
        0x0062: 0x01,                       # Scenes Management: SceneNames (bit 0)
        0x0102: 1 + 4,                      # Window Covering: Lift (bit 0) + PA_LF (bit 2)
        0x0202: 2,                          # Fan Control: Auto (bit 1)
    }

    #############################################################
    # read_attribute — Thread variant of NetworkCommissioning
    #############################################################
    def read_attribute(session, ctx, tlv_solo)
        import string
        var TLV = matter.TLV
        var cluster = ctx.cluster
        var attribute = ctx.attribute

        if cluster == 0x0031
            if   attribute == 0x0000          #  ---------- MaxNetworks / uint8 ----------
                return tlv_solo.set(0x04 #-TLV.U1-#, 1)
            elif attribute == 0x0001          #  ---------- Networks / list[NetworkInfoStruct] ----------
                var nl = TLV.Matter_TLV_array()
                if self.device.thread_connected
                    var ni = nl.add_struct(nil)
                    var xpanid = bytes("0000000000000000")
                    var td = self.device.thread_dataset
                    if td
                        var i = 0
                        while i + 1 < size(td)
                            var t = td[i]
                            var l = td[i + 1]
                            if t == 2 && l == 8 && i + 2 + l <= size(td)
                                xpanid = td[i + 2 .. i + 2 + l - 1]
                                break
                            end
                            i += 2 + l
                        end
                    end
                    ni.add_TLV(0, 0x10 #-TLV.B1-#, xpanid)
                    ni.add_TLV(1, 0x08 #-TLV.BOOL-#, true)
                end
                return nl
            elif attribute == 0x0002          #  ---------- ScanMaxTimeSeconds / uint8 ----------
                return tlv_solo.set(0x04 #-TLV.U1-#, 30)
            elif attribute == 0x0003          #  ---------- ConnectMaxTimeSeconds / uint8 ----------
                return tlv_solo.set(0x04 #-TLV.U1-#, 30)
            elif attribute == 0x0004          #  ---------- InterfaceEnabled / bool ----------
                return tlv_solo.set(0x08 #-TLV.BOOL-#, true)
            elif attribute == 0x0005          #  ---------- LastNetworkingStatus ----------
                return tlv_solo.set(0x14 #-TLV.NULL-#, nil)
            elif attribute == 0x0006          #  ---------- LastNetworkID ----------
                return tlv_solo.set(0x14 #-TLV.NULL-#, nil)
            elif attribute == 0x0007          #  ---------- LastConnectErrorValue ----------
                return tlv_solo.set(0x14 #-TLV.NULL-#, nil)
            elif attribute == 0x0009          #  ---------- SupportedThreadFeatures / map16 ----------
                return tlv_solo.set(0x05 #-TLV.U2-#, 0x000A)
            elif attribute == 0x000A          #  ---------- ThreadVersion / uint16 ----------
                return tlv_solo.set(0x05 #-TLV.U2-#, 4)
            elif attribute == 0xFFFC          #  ---------- FeatureMap / map32 ----------
                return tlv_solo.set(0x06 #-TLV.U4-#, 0x02)  # Thread Network Interface (bit 1)
            end
        elif cluster == 0x0033
            if attribute == 0x0000            #  ---------- NetworkInterfaces ----------
                var nwi = TLV.Matter_TLV_array()
                var thr = nwi.add_struct(nil)
                thr.add_TLV(0, 0x0C #-TLV.UTF1-#, 'thread')     # Name
                thr.add_TLV(1, 0x08 #-TLV.BOOL-#, self.device.thread_connected ? true : false) # IsOperational
                thr.add_TLV(2, 0x14 #-TLV.NULL-#, nil)          # OffPremiseServicesReachableIPv4
                thr.add_TLV(3, 0x14 #-TLV.NULL-#, nil)          # OffPremiseServicesReachableIPv6
                import OT
                var eui64 = bytes().fromhex(string.replace(OT.get_eui64(), ':', ''))
                thr.add_TLV(4, 0x10 #-TLV.B1-#, eui64)         # HardwareAddress (EUI-64)
                var ip4 = thr.add_array(5)                       # IPv4Addresses (empty for Thread)
                var ip6 = thr.add_array(6)                       # IPv6Addresses
                if self.device.thread_connected
                    var addrs = OT.get_ipaddr()
                    if addrs
                        for addr : addrs
                            ip6.add_TLV(nil, 0x10 #-TLV.B1-#, matter.get_ip_bytes(addr))
                        end
                    end
                end
                thr.add_TLV(7, 0x04 #-TLV.U1-#, 3)             # InterfaceType: 3 = Thread
                return nwi
            end
        end
        return super(self).read_attribute(session, ctx, tlv_solo)
    end

    #############################################################
    # invoke_request — Thread variant of NetworkCommissioning
    #############################################################
    def invoke_request(session, val, ctx)
        var TLV = matter.TLV
        var cluster = ctx.cluster
        var command = ctx.command

        if cluster == 0x0031

            if   command == 0x0000          #  ---------- ScanNetworks ----------
                var snresp = TLV.Matter_TLV_struct()
                snresp.add_TLV(0, 0x04 #-TLV.U1-#, 0x00)
                snresp.add_TLV(1, 0x0D #-TLV.UTF2-#, "")
                snresp.add_array(3)
                ctx.command = 0x01
                return snresp

            elif command == 0x0002          #  ---------- AddOrUpdateWiFiNetwork (BLOCKED) ----------
                log("MTR: AddOrUpdateWiFiNetwork rejected, this is a Thread device", 2)
                ctx.status = 0x81 #-UNSUPPORTED_COMMAND-#
                return nil

            elif command == 0x0003          #  ---------- AddOrUpdateThreadNetwork ----------
                var dataset = val.findsubval(0)
                self.device.thread_dataset = dataset
                log(format("MTR: AddOrUpdateThreadNetwork dataset=%i bytes", size(dataset)), 2)

                var ncresp = TLV.Matter_TLV_struct()
                ncresp.add_TLV(0, 0x04 #-TLV.U1-#, 0x00)
                ncresp.add_TLV(1, 0x0D #-TLV.UTF2-#, "thread dataset stored")
                ncresp.add_TLV(2, 0x04 #-TLV.U1-#, 0)
                ctx.command = 0x05
                return ncresp

            elif command == 0x0004          #  ---------- RemoveNetwork ----------
                self.device.thread_dataset = nil
                import OT
                OT.stop()
                # FUTURE: also clear OT's Active Dataset from /ot_settings.bin
                # via otDatasetSetActiveTlvs() with an empty dataset, so the
                # device doesn't try to rejoin on next reboot if the fabric
                # was not also removed.
                log("MTR: RemoveNetwork Thread dataset cleared", 2)
                var rmresp = TLV.Matter_TLV_struct()
                rmresp.add_TLV(0, 0x04 #-TLV.U1-#, 0x00)
                rmresp.add_TLV(1, 0x0D #-TLV.UTF2-#, "")
                rmresp.add_TLV(2, 0x04 #-TLV.U1-#, 0)
                ctx.command = 0x05
                return rmresp

            elif command == 0x0006          #  ---------- ConnectNetwork ----------
                log("MTR: ConnectNetwork, starting Thread", 2)
                self.device.provision_thread_network(self.device.thread_dataset)
                var ncresp = TLV.Matter_TLV_struct()
                ncresp.add_TLV(0, 0x04 #-TLV.U1-#, 0x00 #-SUCCESS-#)
                ncresp.add_TLV(1, 0x0D #-TLV.UTF2-#, "thread provisioning started")
                ncresp.add_TLV(2, 0x02 #-TLV.I4-#, 0)    # NetworkIndex
                ctx.command = 0x07 #-ConnectNetworkResponse-#
                return ncresp

            else
                # Block any other 0x0031 commands from reaching base class WiFi handler
                ctx.status = 0x81 #-UNSUPPORTED_COMMAND-#
                return nil
            end

        elif cluster == 0x0038
            if command == 0x0000            #  ---------- SetUTCTime ----------
                var utc = val.findsubval(0)
                var gran = val.findsubval(1)
                if utc != nil && utc > 0
                    var unix_sec = int(utc / int64(1000000)) + 946684800
                    tasmota.cmd("time " + str(unix_sec))
                end
                log(format("MTR: SetUTCTime utc=%s granularity=%s", str(utc), str(gran)), 2)
                ctx.status = 0x00 #-SUCCESS-#
                return true
            end

        end
        return super(self).invoke_request(session, val, ctx)
    end
end

class MATTER_THREAD : matter.Device
    var current_func, next_func, btp, have_light
    var ble_ready
    var ot_started                     # bool: OpenThread initialized
    var thread_connected               # bool: Thread network attached
    var thread_dataset                 # bytes: stored Thread Operational Dataset TLV
    var srp_pase_instance              # string: SRP instance name for PASE (nil if not active)
    var srp_host_announced             # bool: srp_announce_hostnames already invoked
    var _srp_fabrics                   # list: fabrics collected during commissioning (before start)
    var check_if_commissioned          # bool: check commissioning after BLE disconnect
    var case_grace_until               # int millis: deadline to wait for CASE after AddNOC (nil = not started)
    var packets_sent                   # list: OT UDP packets awaiting ack (retransmission)
    var ble_serv_cb                    # held reference to cb.gen_cb closure for BLE.serv_cb (must outlive GC)
    var pending_radio_reclaim
    var wifi_was_up                       # bool: last known WiFi state (for detecting OFF transitions)
    var ble_cbuf                          # bytes: BLE callback buffer (nil if BLE unavailable)
    # No srp field — SRP is driven via native OT.srp_* API (OT SRP client with BearSSL ECDSA)

    # Override autoconf to use Thread Root plugin instead of WiFi Root plugin.
    # matter.Plugin_Root is a solidified module member — runtime assignment to it
    # is stored in global['.matter'] but GETMBR resolves the solidified constant
    # first, so the monkey-patch never takes effect. We override autoconf_device()
    # to directly instantiate Matter_Plugin_Root_Thread.
    def autoconf_device()
        import json
        if size(self.plugins) > 0   return end
        if (self.autoconf == nil)   self.autoconf = matter.Autoconf(self)   end
        if !self.plugins_persist
            self.plugins_config = self.autoconf.autoconf_device_map()
            self.plugins_config_remotes = {}
            self.adjust_next_ep()
        end
        # Use Thread Root plugin directly instead of matter.Plugin_Root
        self.plugins.push(Matter_Plugin_Root_Thread(self, 0, self.plugins_config.find("0", {})))
        log("MTR: Configuring endpoints", 2)
        log(format("MTR:   endpoint = %5i type:%s%s", 0, 'root(thread)', ''), 2)
        # Aggregator
        self.plugins.push(matter.Plugin_Aggregator(self, 1, {}))
        log(format("MTR:   endpoint = %5i type:%s%s", 1, 'aggregator', ''), 2)
        # Remaining endpoints via standard autoconf
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
        self.commissioning = Matter_Thread_Commissioning(self)
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
            # NOTE: We no longer rely on a cross-task cb.gen_cb callback for OT
            # state changes — that runs on the OpenThread FreeRTOS task and
            # corrupts the (single-threaded) Berry VM, causing instruction-
            # access faults. Instead we poll OT.poll_state() from every_50ms.
            self.ot_started = true
            log("MTR: OpenThread initialized", 2)
        except .. as e, m
            log(format("MTR: OpenThread init FAILED: %s %s", str(e), str(m)), 2)
        end

        var commissioned = self.sessions.count_active_fabrics() > 0
        if commissioned
            # Already commissioned — rejoin Thread, no BLE needed.
            # OT already loaded the Active Dataset from /ot_settings.bin
            # during otInstanceInitSingle() (called by import OT).
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
                self.ble_cbuf = bytes(-255)
                import cb
                self.ble_serv_cb = cb.gen_cb(/e,o,u,h->self.cb(e,o,u,h))
                BLE.serv_cb(self.ble_serv_cb, self.ble_cbuf)
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
                self.ble_serv_cb = nil
            end
        end
    end

    def init_light()
        try
            import light
            light.set({'bri': 5, 'hue': 150, 'power': true, 'sat': 255})
            self.have_light = (light.get() != nil)
        except ..
            self.have_light = false
        end
    end

    def heart_beat()
        if self.commissioning.is_commissioning_open() == false  return end
        if self.have_light
            var l = light.get()
            var hue = (l['hue'] + 1) % 256
            light.set({'bri': l['bri'] == 50 ? 5 : 50, 'hue': hue})
        end
    end

    # Callback from OT when device role changes
    # role is OT enum: 0=disabled, 1=detached, 2=child, 3=router, 4=leader
    def ot_state_changed(role)
        var roles = ["disabled", "detached", "child", "router", "leader"]
        var role_str = (role >= 0 && role < size(roles)) ? roles[role] : "unknown"
        log(format("MTR: Thread role changed: %s (%d)", role_str, role), 2)
        if role >= 2  # child, router, or leader
            if !self.thread_connected
                self.thread_connected = true
                log("MTR: Thread network attached", 2)
                self.start()
                # If commissioning window is open, announce PASE via SRP
                if self.commissioning.is_commissioning_open()
                    self.commissioning.mdns_announce_PASE()
                end
            end
        else
            self.thread_connected = false
        end
    end

    # Start OT UDP socket on Thread netif + start native OT SRP client.
    # SRP hostname and services queued during BLE commissioning (via
    # srp_announce_op_discovery) are already in the OT SRP client's internal
    # queue and will be sent in the first UPDATE when _start_srp_client starts.
    def start()
        if self.started   return end
        self.started = true
        import OT
        OT.udp_open(self.UDP_PORT)
        self.packets_sent = []
        # Start native OT SRP client (discovers server from netdata,
        # sets hostname, starts client)
        self._start_srp_client()
        # Re-register operational discovery services for all active fabrics
        self.srp_announce_hostnames()
        # Log SRP state shortly after start so we can see if SRP server was found
        tasmota.set_timer(2000, /-> self._log_srp_state())
        tasmota.set_timer(8000, /-> self._log_srp_state())
        log("MTR: Thread device started, OT UDP on port " + str(self.UDP_PORT), 2)
    end

    # Diagnostic helper: log current native OT SRP client state and own IPv6 addresses
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
        # Also dump Thread Network Data services so we can see what the
        # leader has published — anycast/unicast SRP entries, the BR's
        # actual RLOC16 etc. This is critical when SRP autostart picks an
        # anycast ALOC and updates time out (OT_ERROR_RESPONSE_TIMEOUT).
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

    def network_down()
    end

    ###########################################################
    # Message send: BLE during PASE commissioning, OT UDP after Thread attached
    ###########################################################
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
            # OT UDP/Thread path
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
        end
    end

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

    # Resend unacknowledged OT UDP packets with exponential backoff
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

    ###########################################################
    # BLE BTP frame reassembly → feed to message_handler
    ###########################################################
    def parse()
        var msg = self.btp
        var is_synced = msg.parse(self.ble_cbuf[1..self.ble_cbuf[0]])
        if !(msg.flags & matter.BTP.F_HANDSHAKE) && msg.combined_payload
            if (msg.flags & matter.BTP.F_END)
                if size(msg.combined_payload) == 0  return end
                if is_synced
                    log(format("BLE: <<< %i bytes", self.ble_cbuf[0]))
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

    ###########################################################
    # BLE GATT send
    ###########################################################
    def ble_send(payload)
        import BLE
        if self.ble_cbuf == nil   return end
        self.ble_cbuf[0] = size(payload)
        self.ble_cbuf.setbytes(1,payload)
        BLE.set_chr("18EE2EF5-263D-4559-959F-4F9C429F9D12")
        BLE.run(211, true)
        self.ble_ready = false
        log(format("BLE: >>> %i bytes", self.ble_cbuf[0]))
        self.then(/->self.wait())
        tasmota.defer(/->self.heart_beat())
        return true
    end

    ###########################################################
    # BLE GATT event callback
    ###########################################################
    def cb(error,op,uuid,handle)
        if self.ble_cbuf == nil   return end
        if op == 201
            log(format("BLE: Handles created: %s", self.ble_cbuf[1..self.ble_cbuf[0]].tohex()))
        elif op == 222
            self.parse()
        elif op == 224 || op == 225
            log(format("BLE: Subscribed to %s", op == 224 ? "notification" : "indication"))
            self.next_func = /->self.handshake_ack()
        elif op == 227
            log(format("BLE: peer MAC: %s", self.ble_cbuf[1..self.ble_cbuf[0]].tohex()))
        elif op == 228
            log("BLE: Disconnected")
            self.check_if_commissioned = true
        elif op == 229
            self.ble_ready = true
            log("BLE: stack ready")
            return
        end
        if error == 0 && op != 229
            self.current_func = self.next_func
        end
    end

    ###########################################################
    # BLE GATT service setup (Matter FFF6 service)
    ###########################################################
    def init_C1()
        import BLE
        if self.ble_cbuf == nil   return end
        BLE.set_chr("18EE2EF5-263D-4559-959F-4F9C429F9D11")
        self.ble_cbuf.setbytes(0,bytes("0100"))
        BLE.run(211,true, 8)
        self.then(/->self.init_C2())
    end

    def init_C2()
        import BLE
        if self.ble_cbuf == nil   return end
        BLE.set_chr("18EE2EF5-263D-4559-959F-4F9C429F9D12")
        self.ble_cbuf.setbytes(0,bytes("0100"))
        BLE.run(211,true,32)
        self.then(/->self.add_ScanResp())
    end

    def add_ADV()
        import BLE
        if self.ble_cbuf == nil   return end
        var descriptor = bytes("05025000A000")
        self.ble_cbuf.setbytes(0,descriptor)
        BLE.run(232)
        var payload = bytes("0201060B16F6FF00")
        payload.add(self.root_discriminator,2)
        payload.add(self.VENDOR_ID, 2)
        payload.add(self.PRODUCT_ID, 2)
        payload.add(0x00)
        self.ble_cbuf[0] = size(payload)
        self.ble_cbuf.setbytes(1,payload)
        BLE.run(201)
        self.then(/->self.wait())
        log("BLE: advertising Matter accessory")
    end

    def add_ScanResp()
        import BLE
        if self.ble_cbuf == nil   return end
        var local_name = "Tasmota Matter"
        var payload = bytes("0201060008") + bytes().fromstring(local_name)
        payload[3] = size(local_name) + 1
        self.ble_cbuf[0] = size(payload)
        self.ble_cbuf.setbytes(1,payload)
        BLE.run(202)
        self.then(/->self.add_ADV())
    end

    def wait()
    end

    def then(func)
        self.next_func = func
        self.current_func = /->self.wait()
    end

    ###########################################################
    # Check commissioning result after BLE disconnect
    #
    # For Thread devices, BLE disconnect does NOT mean commissioning is
    # complete: the controller will provision Thread credentials and then
    # CASE happens over Thread UDP. We must wait for Thread to attach
    # (or for the commissioning window to definitively close with no
    # progress) before declaring success or failure.
    ###########################################################
    def check_final()
        # Still in the active commissioning window — wait
        if self.commissioning && self.commissioning.is_commissioning_open()
            return
        end
        # Thread provisioning has started but Thread hasn't attached yet — wait
        if self.thread_dataset != nil && !self.thread_connected
            return
        end
        # Use the in-memory fabric count, not just the persisted file:
        # AddNOC adds the fabric in RAM and the persist may lag slightly.
        if self.sessions.count_active_fabrics() > 0
            log("MTR: Successfully finished BLE commissioning.")
            if self.have_light  light.set({'bri':50,'hue':120}) end
            self.check_if_commissioned = false
            return
        end
        # No persisted fabric yet. Distinguish two cases:
        #   1. A candidate fabric exists (AddNOC ran) → CASE is in flight.
        #      Give it a grace window before declaring failure; only restart
        #      if it still hasn't completed.
        #   2. No fabric at all (PASE never reached AddNOC) → commissioner
        #      gave up early or BLE dropped pre-AddNOC. Nothing to recover by
        #      restarting; just stop checking silently.
        var has_candidate = self.sessions && self.sessions.fabrics && size(self.sessions.fabrics) > 0
        if has_candidate
            if self.case_grace_until == nil
                self.case_grace_until = tasmota.millis() + 90000   # 90 s for CASE
                log("MTR: candidate fabric present, waiting up to 90s for CASE", 3)
                return
            end
            if !tasmota.time_reached(self.case_grace_until)
                return
            end
            log("MTR: commissioning failure (CASE did not complete)")
            try  import OT  OT.srp_stop()  except .. end
            tasmota.cmd("restart 1")
            if self.have_light  light.set({'bri':50,'hue':0}) end
        else
            # No AddNOC ever happened — not a recoverable failure, just stop.
            log("MTR: commissioning ended without AddNOC, no fabric expected", 3)
        end
        self.case_grace_until = nil
        self.check_if_commissioned = false
    end

    ###########################################################
    # SRP-based service announcement via native OT SRP client
    ###########################################################
    def srp_announce_hostnames()
        import string
        import OT
        # Step 1: set hostname from the EUI-64 (once)
        try
            if !self.srp_host_announced
                var eui64 = OT.get_eui64()
                var hostname = string.tolower(string.replace(eui64, ':', ''))
                # Enable auto host address — OT will pick the ML-EID from its
                # own address table. Must be called before any service is added.
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

        # Step 2: register operational discovery for all active fabrics.
        # Iterate the underlying list directly to avoid the persistables() closure
        # iterator, which has triggered intermittent type errors when called from
        # the OT state-change context.
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

    # SRP operational discovery for a fabric (native OT SRP client)
    def srp_announce_op_discovery(fabric)
        import string
        import OT
        try
            var device_id = fabric.get_device_id().copy().reverse()
            var k_fabric = fabric.get_fabric_compressed()
            var op_node = string.tolower(k_fabric.tohex() + "-" + device_id.tohex())
            log("MTR: SRP Operational Discovery node = " + op_node, 3)
            # Save fabric so srp_announce_hostnames() can re-add services
            # when client state is cleaned before start.
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
            # Subtypes: _I<CompressedFabricID> (comma-separated string for native API)
            var subtypes_str = "_I" + string.tolower(k_fabric.tohex())
            # TXT records per Matter 1.4.1 §4.3.2 (comma-separated key=value string)
            var txt_str = "SII=500,SAI=300,SAT=4000,T=0"
            OT.srp_add_service(op_node, "_matter._tcp", self.UDP_PORT, subtypes_str, txt_str)
            log(format("MTR: SRP registered _matter._tcp instance '%s' (native OT)", op_node), 3)
        except .. as e, m
            log(format("MTR: SRP op_discovery FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    # SRP commissionable discovery (native OT SRP client)
    def srp_announce_PASE()
        import crypto
        import string
        import OT
        try
            var instance = string.tolower(crypto.random(8).tohex())
            var disc = self.commissioning.commissioning_discriminator
            # Subtypes per Matter spec: _L<discriminator>, _S<short_disc>, _V<vendor>, _CM1
            # Passed as comma-separated string to native OT API
            var subtypes_str = format("_L%i,_S%i,_V%i,_CM1",
                disc & 0xFFF, (disc & 0xF00) >> 8, self.VENDOR_ID)
            # TXT records per Matter 1.4.1 §4.3.4 (comma-separated key=value)
            var txt_str = format("VP=%d+%d,D=%d,CM=1,T=0,SII=500,SAI=300,SAT=4000",
                self.VENDOR_ID, self.PRODUCT_ID, disc)
            OT.srp_add_service(instance, "_matterc._udp", self.UDP_PORT, subtypes_str, txt_str)
            self.srp_pase_instance = instance
            log(format("MTR: SRP registered _matterc._udp instance '%s' (native OT)", instance), 2)
        except .. as e, m
            log(format("MTR: SRP PASE announce FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    # SRP remove PASE announce (native OT SRP client)
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

    # SRP remove operational discovery (native OT SRP client)
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
    # SRP server discovery + client start (native OT SRP client)
    ###########################################################

    # Build an anycast ALOC "<mesh-local-prefix>:0:ff:fe00:<rloc16>" by reusing
    # the mesh-local prefix from one of our own RLOC-based addresses (those
    # contain the marker ":0:ff:fe00:").
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

    # Discover SRP server from Thread Network Data and return { addr, port }.
    # Parses the human-readable descriptors from OT.netdata_services().
    # Prefers SRP-anycast (ALOC + port 53) over SRP-unicast (explicit IPv6+port).
    # The unicast server (port 63218) is unreliable on some BRs; the anycast
    # matches what upstream esp-matter uses and works consistently.
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
                # Prefer anycast: symmetrical to OT auto-start behavior
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
                # Remember unicast as fallback
                if string.find(s, "SRP-unicast") >= 0 && unicast_addr == nil
                    var ob = string.find(s, "[")
                    var cb = string.find(s, "]:")
                    if ob >= 0 && cb >= 0
                        unicast_addr = s[ob + 1 .. cb - 1]
                        unicast_port = int(s[cb + 2 .. size(s) - 1])
                    end
                end
            end
            # No anycast: fall back to unicast with its discovered port
            if unicast_addr != nil && unicast_port > 0
                log(format("MTR: SRP server unicast %s:%i (fallback)", unicast_addr, unicast_port), 2)
                return {"addr": unicast_addr, "port": unicast_port}
            end
        except .. as e, m
            log(format("MTR: SRP server discover FAILED: %s %s", str(e), str(m)), 2)
        end
        return nil
    end

    # With auto-start (enabled in C init), OT discovers the SRP server from
    # Thread Network Data automatically. This function just ensures hostname
    # and lease intervals are set before the first UPDATE goes out.
    def _start_srp_client()
        import OT
        try
            # Set hostname if not already set during BLE commissioning
            if !self.srp_host_announced
                import string
                var eui64 = OT.get_eui64()
                var hostname = string.tolower(string.replace(eui64, ':', ''))
                OT.srp_set_hostname(hostname)
                self.srp_host_announced = true
                log(format("MTR: SRP hostname set (native OT): %s", hostname), 3)
            end
            # Set lease intervals matching OpenThread defaults (chip SDK
            # also sets these explicitly before AddService).
            OT.srp_set_lease_interval(7200, 1209600)
            log("MTR: SRP lease intervals set (7200s, 1209600s)", 3)
            log("MTR: SRP auto-start enabled, OT handles server discovery", 2)
        except .. as e, m
            log(format("MTR: SRP setup FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    ###########################################################
    # Provision Thread network (called from NetworkCommissioning cluster)
    ###########################################################
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

    ###########################################################
    # Periodic callbacks
    ###########################################################
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
        if self.current_func self.current_func() end
        self.tick += 1
        self.message_handler.every_50ms()
        # Native OT SRP client manages its own retry/refresh timers internally.
        # No Berry-side tick needed.
        # drain stuck BTP queue (BTP packets can cross causing both can_send and ble_ready to be false when msg_send is called)
        if self.btp && self.ble_ready && self.btp.can_send && size(self.btp.send_queue) > 0
            var p = self.btp.get_packet()
            self.btp.can_send = false
            self.ble_send(p)
        end
        # Drain pending OT role transitions on the main task (the OT task can't
        # safely call into the Berry VM, so it just stashes the role).
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
        # Poll OT UDP receive queue
        # Detect WiFi OFF transition and reclaim the shared 802.15.4 radio for Thread
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

        if self.started
            import OT
            # Drain up to 4 packets per tick. Each packet is dispatched independently
            # so a faulty packet does not poison the rest of the queue, and the
            # retransmit pass below always runs.
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

return MATTER_THREAD()

#-
Build-time: Define USE_MATTER_THREAD in your build (platformio env or user_config_override.h). Target must be ESP32-H2, C6, or C5 with OpenThread enabled in sdkconfig.

Commissioning flow:
  Device starts BLE advertising (Matter FFF6 service with discriminator)
  Commissioner discovers via BLE, scans QR code → PASE handshake
  Commissioner sends AddOrUpdateThreadNetwork (dataset TLV)
  Commissioner sends ConnectNetwork → OT.set_dataset() + OT.start()
  WiFi is temporarily turned OFF to force commissioning over Thread
  Device joins Thread mesh, registers via SRP (DNS-SD)
  Commissioner discovers device via SRP, completes CASE over Thread/UDP
  Subsequent operation is over Thread; WiFi remains off for the session
  (next boot restores WiFi to user's configured state)
-#
