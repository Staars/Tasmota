#
# Matter_zz_Thread_Commissioning.be - Thread-specific Commissioning subclass
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

#@ solidify:Matter_Thread_Commissioning,weak
#if USE_MATTER_THREAD
#################################################################################
# Matter_Thread_Commissioning
#
# Thread-specific Commissioning subclass. Routes mDNS/SRP calls to the native
# OT SRP client instead of the Wi-Fi-based mDNS module.
#################################################################################
class Matter_Thread_Commissioning : Matter_Commissioning
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
matter.Commissioning_Thread = Matter_Thread_Commissioning
#else
class Matter_Thread_Commissioning end
#endif //USE_MATTER_THREAD
