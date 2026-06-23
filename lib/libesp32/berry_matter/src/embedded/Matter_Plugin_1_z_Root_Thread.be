#
# Matter_Plugin_1_Root_Thread.be - Thread variant of the Root Node plugin
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

#@ solidify:Matter_Plugin_Root_Thread,weak
#if USE_MATTER_THREAD
#################################################################################
# Matter_Plugin_Root_Thread
#
# Thread-specific Root Node plugin. Subclasses the generic `Matter_Plugin_Root`
# and overrides the Network Commissioning cluster (0x0031), the General
# Diagnostics NetworkInterfaces attribute (0x0033) and the Time Synchronization
# SetUTCTime command (0x0038) so they operate on a Thread Operational Dataset
# (managed via the native `OT` module) instead of Wi-Fi credentials.
#
# Wi-Fi commands on 0x0031 are explicitly rejected so they never reach the
# base-class Wi-Fi handler.
#################################################################################
class Matter_Plugin_Root_Thread : Matter_Plugin_Root
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

matter.Plugin_Root_Thread = Matter_Plugin_Root_Thread
#else
class Matter_Plugin_Root_Thread end # will be discarded
#endif //USE_MATTER_THREAD
