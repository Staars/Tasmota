#
# self.be - SRP client (RFC 9665) implemented in pure Berry
#
# Copyright (C) 2026 Stephan Hadinger, Christian Baars & Theo Arends
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
# ----------------------------------------------------------------------------
# SRP client per RFC 9665 (https://datatracker.ietf.org/doc/rfc9665/).
#
# This file replaces the previous OpenThread C-side SRP client. BearThread is
# transport-only now; the actual SRP/DNS/SIG(0) state machine lives here.
#
# The C-side exposes UDP transport (OT.udp_open/send/poll/close) plus a few
# helpers (`OT.netdata_services()`,
# `OT.get_eui64()`).
#
# Wire format references (from OpenThread reference impl):
#   - DNS header: 12 bytes (mMessageId, mFlags[2], counts)
#   - UpdateHeader: same as Header with OpCode=5 (UPDATE)
#   - Zone section: 1 zone record (`<domain>.`, type=SOA, class=IN)
#   - Update section: 1..N resource records (host delete, AAAA, KEY, PTR, SRV, TXT)
#   - Additional section: OPT (lease) + SIG(0) [always exactly 2]
#
# SIG(0) per RFC 2931:
#   - SHA-256 over (SIG RDATA wire with empty signature, but signer's name in
#     canonical uncompressed form) || DNS message from byte 0 up to start of
#     the SIG record (with header counts adjusted to exclude the SIG record).
#   - ECDSA-P-256 signature, low-S normalized, encoded as ASN.1/DER.
#
# The KEY record is in the Update section for both host and service
# descriptions. KEY flags: NOC=0, ZNZ=2, Signatory=General=1.
# ----------------------------------------------------------------------------

import matter

#@ solidify:Matter_SRP_Client,weak
#if USE_MATTER_THREAD

class Matter_SRP_Client
    #########################################################################
    # Constants
    #########################################################################

    # DNS RR types
    static kTypeA     = 1
    static kTypePtr   = 12
    static kTypeTxt   = 16
    static kTypeSig   = 24
    static kTypeKey   = 25
    static kTypeAaaa  = 28
    static kTypeSrv   = 33
    static kTypeOpt   = 41
    static kTypeSoa   = 6
    static kTypeAny   = 255

    # DNS RR classes
    static kClassIn    = 1
    static kClassNone  = 254     # for "delete an RR from an RRset" (RFC 2136)
    static kClassAny   = 255     # SIG(0) uses class=ANY

    # KEY record fields
    static kProtocolDnsSec = 3
    static kAlgorithmEcdsaP256Sha256 = 13
    # Key flags: (aUseFlags << 8) | aOwnerFlags in mFlags[0];
    # aSignatoryFlags in low nibble of mFlags[1].
    # We use NOC=0 (no confidentiality), ZNZ=2 (non-zone), Signatory=General=1.
    static kKeyFlagsHigh = 0x00  # NOC, no confidentiality
    static kKeyFlagsLow  = 0x02  # ZNZ
    static kSignatoryGeneral = 0x01

    # Update Lease OPT option code
    static kOptUpdateLease = 2

    # Wire / protocol
    static kUdpPayloadSize = 400           # below OT MTU 1152

    # Leases
    static kDefaultLeaseSec    = 3600      # 1h, typical for Matter
    static kDefaultKeyLeaseSec = 86400     # 24h, Apple BR is fine with 1d

    # Backoff
    static kInitialBackoffMs = 5000
    static kMaxBackoffMs     = 60000
    static kRefreshFraction  = 0.75        # refresh at 75% of lease
    static kMsgIdMax         = 0xFFFF

    # secp256r1 curve order n and n/2 (for low-S normalization)
    # RFC 6979 / FIPS 186-4. Big-endian, 32 bytes.
    static kN = bytes(
      "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551"
    )
    static kNHalf = bytes(
      "7FFFFFFF800000007FFFFFFFFFFFFFFFDE737D56D38BCF4279DCE5617E3192A8"
    )

    # State machine
    static kStateStopped    = 0
    static kStateToAdd      = 1
    static kStateAdding     = 2
    static kStateRegistered = 3
    static kStateToRefresh  = 4
    static kStateRefreshing = 5
    static kStateToRemove   = 6
    static kStateRemoving   = 7
    static kStateRemoved    = 8
    static kStateError      = 9

    #########################################################################
    # State
    #########################################################################

    var hostname              # string (lower-case, no trailing dot, no domain)
    var services              # list of [instance, service_type, port, subtypes(list of str), txt(dict of str->str), state, lease_end_ms, key_lease_end_ms, msg_id]
    var key_priv              # bytes(32) - private key (big-endian, RFC 6979 form)
    var key_pub               # bytes(64) - public key X||Y (big-endian)
    var state                 # kState*
    var state_str_            # cached human-readable state string
    var last_attempt_ms       # int (tasmota.millis())
    var backoff_ms            # current backoff
    var msg_id                # int (next message id to use)
    var server_addr           # string (IPv6) or nil
    var server_port           # int (default 53)
    var use_unicast           # bool - if true, prefer unicast server; else anycast ALOC
    var lease_sec             # int - current lease
    var key_lease_sec         # int - current key lease
    var last_response         # "ok" / "timeout" / "error" / "noerror" (RCODE text)
    var last_error_logged     # int ms - throttle error logging
    var started               # bool
    var udp_open              # bool

    #########################################################################
    # Constructor
    #########################################################################
    def init()
        self.hostname = nil
        self.services = []
        self.key_priv = nil
        self.key_pub = nil
        self.state = self.kStateStopped
        self.state_str_ = "Stopped"
        self.last_attempt_ms = 0
        self.backoff_ms = self.kInitialBackoffMs
        self.msg_id = 0
        self.server_addr = nil
        self.server_port = 53
        self.use_unicast = false
        self.lease_sec = self.kDefaultLeaseSec
        self.key_lease_sec = self.kDefaultKeyLeaseSec
        self.last_response = ""
        self.last_error_logged = 0
        self.started = false
        self.udp_open = false
    end

    #########################################################################
    # Public API
    #########################################################################

    # Set the SRP key pair (called after construction by the host).
    def set_key(priv_hex, pub_hex)
        self.key_priv = priv_hex
        self.key_pub = pub_hex
    end

    # Set the SRP hostname. Must be lower-case, no domain, no trailing dot.
    def set_hostname(h)
        import string
        h = string.tolower(h)
        if h == self.hostname
            return
        end
        self.hostname = h
        if self.state == self.kStateStopped
            self.state = self.kStateToAdd
            self.state_str_ = "ToAdd"
        end
    end

    # Add (or update) a service instance.
    # instance    : string, e.g. "abc123def456789a-1a2b3c4d"
    # service_type: string, e.g. "_matter._tcp" (with leading underscores and dot)
    # port        : int
    # subtypes    : list of strings, e.g. ["_Iabcd1234ef567890", "_L123", ...]
    # txt         : map of string->string, e.g. {"SII":"500", "T":"0"}
    def add_service(instance, service_type, port, subtypes, txt)
        # Look for existing
        var i = 0
        while i < size(self.services)
            var s = self.services[i]
            if s[0] == instance && s[1] == service_type
                # update
                s[2] = port
                s[3] = subtypes
                s[4] = txt
                s[5] = self.kStateToAdd
                self.services[i] = s
                if self.state == self.kStateStopped
                    self.state = self.kStateToAdd
                    self.state_str_ = "ToAdd"
                end
                return
            end
            i += 1
        end
        # add new
        self.services.push([instance, service_type, port, subtypes, txt,
                            self.kStateToAdd, 0, 0, 0])
        if self.state == self.kStateStopped
            self.state = self.kStateToAdd
            self.state_str_ = "ToAdd"
        end
    end

    # Mark a service for removal.
    def remove_service(instance, service_type)
        var i = 0
        while i < size(self.services)
            var s = self.services[i]
            if s[0] == instance && s[1] == service_type
                if s[5] != self.kStateRemoved && s[5] != self.kStateToRemove
                    s[5] = self.kStateToRemove
                    self.services[i] = s
                end
                return
            end
            i += 1
        end
    end

    # Set flag to prefer unicast server (vs anycast ALOC)
    def use_unicast_server(flag)
        if flag == nil
            self.use_unicast = !self.use_unicast
        else
            self.use_unicast = !!flag
        end
    end

    # Read-only accessors
    def state_str()
        return self.state_str_
    end

    def is_registered()
        return self.state == self.kStateRegistered
    end

    def current_server()
        if self.server_addr == nil
            return nil
        end
        return format("%s:%i", self.server_addr, self.server_port)
    end

    #########################################################################
    # Main tick (called from Matter_Thread_Device.every_50ms)
    #########################################################################
    def every_50ms()
        # Close UDP socket if no longer needed
        if (self.state == self.kStateStopped || self.state == self.kStateRemoved) && self.udp_open
            import OT
            OT.udp_close()
            self.udp_open = false
        end
        # Drain any UDP responses that came in
        self._drain_responses()

        if self.hostname == nil
            return
        end
        # Server discovery
        if self.server_addr == nil
            self.server_addr = self._pick_server()
        end

        # Backoff: only act when the timer expires
        var now = tasmota.millis()
        if now - self.last_attempt_ms < self.backoff_ms
            return
        end

        # State transitions
        if self.state == self.kStateToAdd
            self._begin_send()
        elif self.state == self.kStateAdding
            # waiting for response; if backoff elapsed and no response, retry
            # (timeout from response handler already moves us back)
            self._begin_send()
        elif self.state == self.kStateToRefresh
            self._begin_send()
        elif self.state == self.kStateRefreshing
            self._begin_send()
        elif self.state == self.kStateRegistered
            # Check for refresh and service state changes
            if now >= self._next_refresh_ms()
                self.state = self.kStateToRefresh
                self.state_str_ = "ToRefresh"
            end
            # Promote any service that was re-added
            var i = 0
            while i < size(self.services)
                var s = self.services[i]
                if s[5] == self.kStateToAdd
                    self.state = self.kStateToRefresh
                    self.state_str_ = "ToRefresh"
                    return
                end
                i += 1
            end
            # Demote any service that was removed
            i = 0
            while i < size(self.services)
                var s = self.services[i]
                if s[5] == self.kStateToRemove
                    self.state = self.kStateToRefresh
                    self.state_str_ = "ToRefresh"
                    return
                end
                i += 1
            end
        end
    end

    def _next_refresh_ms()
        # Refresh when 75% of any service's lease is reached
        var now = tasmota.millis()
        var soonest = -1
        var i = 0
        while i < size(self.services)
            var s = self.services[i]
            if s[5] == self.kStateRegistered || s[5] == self.kStateToAdd
                var refresh_at = s[6] - int((self.lease_sec * (1 - self.kRefreshFraction)) * 1000)
                if soonest < 0 || refresh_at < soonest
                    soonest = refresh_at
                end
            end
            i += 1
        end
        if soonest < 0
            return 0
        end
        return soonest
    end

    #########################################################################
    # Server discovery
    #########################################################################
    def _pick_server()
        # OT.netdata_services() returns human-readable descriptor strings, e.g.
        #   "id=2 ent=44970 rloc=0xfc12 stable=1 kind=SRP-unicast sd=5D \
        #    svr=FD1D... [fd1d:bc81:cb5e:0:9c2c:bb11:ffe1:2398]:64970"
        #   "id=1 ent=44970 rloc=0xfc11 stable=1 kind=SRP-anycast sd=5C09 \
        #    svr= seq=9"
        # The unicast entry carries an explicit "[addr]:port" tail. The anycast
        # entry carries only the server RLOC16, from which we derive the ALOC
        # "<mesh-local-prefix>:0:ff:fe00:<rloc16>".
        import string
        try
            import OT
            var services = OT.netdata_services()
            if services == nil
                return nil
            end
            var anycast_rloc = nil
            var i = 0
            while i < size(services)
                var s = services[i]
                i += 1
                if type(s) != "string" || size(s) == 0
                    continue
                end
                # Prefer unicast: extract the explicit "[addr]:port" tail.
                if string.find(s, "SRP-unicast") >= 0
                    var lb = string.find(s, "[")
                    var rb = string.find(s, "]:")
                    if lb >= 0 && rb > lb
                        var addr = s[lb + 1 .. rb - 1]
                        var port = int(s[rb + 2 .. size(s) - 1])
                        if size(addr) > 0 && port > 0
                            self.server_addr = addr
                            self.server_port = port
                            return addr
                        end
                    end
                # Remember the first anycast entry as a fallback.
                elif string.find(s, "SRP-anycast") >= 0 && anycast_rloc == nil
                    var rp = string.find(s, "rloc=0x")
                    if rp >= 0
                        anycast_rloc = s[rp + 7 .. rp + 10]   # 4 hex chars
                    end
                end
            end
            # No unicast entry: build the anycast ALOC from the RLOC16.
            if anycast_rloc != nil
                var aloc = self._aloc_from_rloc(anycast_rloc)
                if aloc != nil
                    self.server_addr = aloc
                    self.server_port = 53
                    return aloc
                end
            end
        except .. as e, m
            log(format("MTR: SRP server pick FAILED: %s %s", str(e), str(m)), 2)
        end
        return nil
    end

    # Build an anycast ALOC "<mesh-local-prefix>:0:ff:fe00:<rloc16>" by reusing
    # the mesh-local prefix from one of our own RLOC-based addresses (those
    # contain the marker ":0:ff:fe00:").
    def _aloc_from_rloc(rloc16)
        import string
        import OT
        var a = OT.get_ipaddr()
        if a == nil
            return nil
        end
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

    #########################################################################
    # Send UPDATE
    #########################################################################
    def _begin_send()
        if self.hostname == nil
            return
        end
        if self.server_addr == nil
            self.server_addr = self._pick_server()
            if self.server_addr == nil
                # No server yet; back off and try again
                self.last_attempt_ms = tasmota.millis()
                self.backoff_ms = self.kInitialBackoffMs
                return
            end
        end

        # Build the message
        var msg
        try
            msg = self._build_update_message()
        except .. as e, m
            log(format("MTR: SRP build message FAILED: %s %s", str(e), str(m)), 1)
            self._on_failure("build")
            return
        end

        # Send via UDP (DNS UPDATE)
        try
            import OT
            if !self.udp_open
                OT.udp_open(0)
                self.udp_open = true
            end
            OT.udp_send(self.server_addr, self.server_port, msg)
            self.msg_id = (self.msg_id + 1) & self.kMsgIdMax
            self.last_attempt_ms = tasmota.millis()
            if self.state == self.kStateToAdd || self.state == self.kStateToRefresh
                if self.state == self.kStateToAdd
                    self.state = self.kStateAdding
                    self.state_str_ = "Adding"
                else
                    self.state = self.kStateRefreshing
                    self.state_str_ = "Refreshing"
                end
            end
        except .. as e, m
            log(format("MTR: SRP udp_send FAILED: %s %s", str(e), str(m)), 2)
            self._on_failure("send")
        end
    end

    def _drain_responses()
        if !self.udp_open
            return
        end
        try
            import OT
            while true
                var resp = OT.udp_poll()
                if resp == nil
                    return
                end
                # resp = [payload_bytes, addr_str, port]
                var payload = resp[0]
                self._on_success(payload)
            end
        except .. as e, m
            log(format("MTR: SRP drain responses FAILED: %s %s", str(e), str(m)), 2)
        end
    end

    def _on_success(payload)
        if size(payload) < 12
            self._on_failure("short response")
            return
        end
        var rcode = payload[3] & 0x0F
        if rcode == 0
            self._mark_registered()
        else
            self._on_failure(format("rcode=%i", rcode))
        end
    end

    def _mark_registered()
        self.state = self.kStateRegistered
        self.state_str_ = "Registered"
        var now = tasmota.millis()
        var i = 0
        while i < size(self.services)
            var s = self.services[i]
            if s[5] == self.kStateAdding || s[5] == self.kStateToAdd
                s[5] = self.kStateRegistered
                s[6] = now + self.lease_sec * 1000
                s[7] = now + self.key_lease_sec * 1000
                self.services[i] = s
            elif s[5] == self.kStateRefreshing || s[5] == self.kStateToRefresh
                s[5] = self.kStateRegistered
                s[6] = now + self.lease_sec * 1000
                s[7] = now + self.key_lease_sec * 1000
                self.services[i] = s
            elif s[5] == self.kStateToRemove || s[5] == self.kStateRemoving
                s[5] = self.kStateRemoved
                self.services[i] = s
            end
            i += 1
        end
        # Compact services list: drop removed entries
        var new_list = []
        i = 0
        while i < size(self.services)
            if self.services[i][5] != self.kStateRemoved
                new_list.push(self.services[i])
            end
            i += 1
        end
        self.services = new_list
        self.backoff_ms = self.kInitialBackoffMs
    end

    def _on_failure(reason)
        if self.state == self.kStateToAdd || self.state == self.kStateAdding
            self.state = self.kStateToAdd
            self.state_str_ = "ToAdd"
        elif self.state == self.kStateToRefresh || self.state == self.kStateRefreshing
            self.state = self.kStateToRefresh
            self.state_str_ = "ToRefresh"
        end
        self.last_attempt_ms = tasmota.millis()
        self.backoff_ms = self.backoff_ms * 2
        if self.backoff_ms > self.kMaxBackoffMs
            self.backoff_ms = self.kMaxBackoffMs
        end
        # Re-pick server in case the network changed
        self.server_addr = nil

        # Throttle log
        var now = tasmota.millis()
        if now - self.last_error_logged > 10000
            log(format("MTR: SRP failed (%s), backoff %i ms", str(reason), self.backoff_ms), 2)
            self.last_error_logged = now
        end
    end

    #########################################################################
    # Pure Berry big-int helpers
    #########################################################################
    # Compare two big-endian unsigned integers of equal length. Returns
    # -1 if a < b, 0 if equal, 1 if a > b.
    def _cmp_be(a, b)
        var n = size(a)
        if n != size(b)
            # shouldn't happen; treat shorter as smaller
            return (n < size(b)) ? -1 : 1
        end
        var i = 0
        while i < n
            if a[i] != b[i]
                return (a[i] < b[i]) ? -1 : 1
            end
            i += 1
        end
        return 0
    end

    # Subtract a from b (b - a) where a <= b. Result is big-endian of the
    # same length. Caller must ensure a <= b.
    def _sub_be(b, a)
        var n = size(b)
        if size(a) != n
            return nil
        end
        var out = bytes(-n)
        var borrow = 0
        var i = n - 1
        while i >= 0
            var bi = b[i] - borrow
            if bi < a[i]
                bi += 256
                borrow = 1
            else
                borrow = 0
            end
            out[i] = bi - a[i]
            i -= 1
        end
        return out
    end

    # Check if a 32-byte big-endian integer s is > n/2 (i.e. high-S).
    def _is_high_s(s)
        return self._cmp_be(s, self.kNHalf) > 0
    end

    # Normalize a 64-byte raw ECDSA signature (r||s) to low-S form.
    # Returns 64 bytes.
    def _normalize_low_s(sig_raw)
        if size(sig_raw) != 64
            return sig_raw
        end
        var r = bytes(-32)
        var s = bytes(-32)
        var i = 0
        while i < 32
            r[i] = sig_raw[i]
            s[i] = sig_raw[32 + i]
            i += 1
        end
        if self._is_high_s(s)
            s = self._sub_be(self.kN, s)
        end
        var out = bytes(-64)
        i = 0
        while i < 32
            out[i] = r[i]
            out[32 + i] = s[i]
            i += 1
        end
        return out
    end

    #########################################################################
    # ASN.1/DER encoding for ECDSA signature (RFC 3279 §2.2.3)
    #########################################################################
    # Encode an unsigned 32-byte big-endian integer as a DER INTEGER.
    # Prepends 0x00 if MSB is set, to keep it positive.
    def _der_int(x)
        # Strip leading zeros (but keep at least one byte).
        var start = 0
        while start < size(x) - 1 && x[start] == 0
            start += 1
        end
        var need_pad = x[start] >= 0x80
        var len = (size(x) - start) + (need_pad ? 1 : 0)
        var out = bytes()
        out.add(0x02)  # INTEGER
        out.add(len)
        if need_pad
            out.add(0x00)
        end
        var i = start
        while i < size(x)
            out.add(x[i])
            i += 1
        end
        return out
    end

    # Build the SEQUENCE wrapper around a content. `content` is a bytes
    # object whose length is < 128 (single-byte length encoding).
    def _der_seq(content)
        var out = bytes()
        out.add(0x30)  # SEQUENCE
        var lc = size(content)
        if lc < 0x80
            out.add(lc)
        elif lc < 0x100
            out.add(0x81)
            out.add(lc)
        else
            out.add(0x82)
            out.add((lc >> 8) & 0xFF)
            out.add(lc & 0xFF)
        end
        out .. content
        return out
    end

    # Convert a 64-byte raw r||s signature to ASN.1/DER form.
    def _ecdsa_sig_der(sig_raw)
        if size(sig_raw) != 64
            return nil
        end
        var r = bytes(-32)
        var s = bytes(-32)
        var i = 0
        while i < 32
            r[i] = sig_raw[i]
            s[i] = sig_raw[32 + i]
            i += 1
        end
        var content = bytes()
        content .. self._der_int(r)
        content .. self._der_int(s)
        return self._der_seq(content)
    end

    #########################################################################
    # DNS name encoders
    #########################################################################
    # Encode a DNS name from a list of labels (strings). A trailing dot
    # (the root label) is added automatically. Labels are length-prefixed.
    def _encode_name(labels)
        var out = bytes()
        var i = 0
        while i < size(labels)
            var l = labels[i]
            var lc = size(l)
            if lc > 63
                lc = 63
            end
            out.add(lc)
            out .. l
            i += 1
        end
        out.add(0x00)  # root
        return out
    end

    # Convert a dotted string (e.g. "abc._matter._tcp.default.service.arpa")
    # to a list of labels.
    def _name_to_labels(name)
        # Strip trailing dot
        var s = name
        if size(s) > 0 && s[size(s) - 1] == '.'
            s = s[0 .. size(s) - 2]
        end
        if size(s) == 0
            return []
        end
        # Split by '.' — note: Berry's string.split exists but is not used
        # here because we need to handle trailing dot and empty components.
        var labels = []
        var start = 0
        var i = 0
        while i <= size(s)
            if i == size(s) || s[i] == '.'
                if i > start
                    labels.push(s[start .. i - 1])
                end
                start = i + 1
            end
            i += 1
        end
        return labels
    end

    # Build the full FQDN: split a dotted name into a list of labels,
    # using self.hostname + service/host domain.
    def _full_name_labels(parts)
        # `parts` is a list of strings; we assemble them as separate
        # labels (e.g. ["_matter", "_tcp", "default", "service", "arpa"]).
        return parts
    end

    #########################################################################
    # DNS resource record encoders
    #########################################################################
    # Encode the fixed RR header (TYPE, CLASS, TTL, RDLENGTH) and return
    # the start offset of RDLENGTH so we can fix it up later.
    def _rr_header(rr_type, rr_class, ttl)
        var out = bytes()
        out.add((rr_type >> 8) & 0xFF)
        out.add(rr_type & 0xFF)
        out.add((rr_class >> 8) & 0xFF)
        out.add(rr_class & 0xFF)
        out.add((ttl >> 24) & 0xFF)
        out.add((ttl >> 16) & 0xFF)
        out.add((ttl >> 8) & 0xFF)
        out.add(ttl & 0xFF)
        # placeholder for RDLENGTH (2 bytes); remember offset 8
        out.add(0)
        out.add(0)
        return out
    end

    def _rr_set_rdlength(msg, rdata_offset)
        # msg is the full message; rdata_offset points to start of RDATA.
        # Update the 2 bytes at rdata_offset-2 (the RDLENGTH field).
        var rdlen = size(msg) - rdata_offset
        msg[rdata_offset - 2] = (rdlen >> 8) & 0xFF
        msg[rdata_offset - 1] = rdlen & 0xFF
    end

    #########################################################################
    # Full UPDATE message builder
    #########################################################################
    # Build the DNS UPDATE message for the current state.
    def _build_update_message()
        import crypto

        var domain = ["default", "service", "arpa"]

        # 1. DNS header (12 bytes), placeholders for counts
        var msg = bytes()
        # Message ID (2 bytes)
        msg.add((self.msg_id >> 8) & 0xFF)
        msg.add(self.msg_id & 0xFF)
        # Flags: QR=0, OpCode=5 (UPDATE), all others 0 -> 0x28 0x00
        msg.add(0x28)
        msg.add(0x00)
        # Counts (placeholders, fixed at end):
        # ZOCOUNT=1, PRCOUNT=0, UPCOUNT=N, ADCOUNT=2 (lease + SIG)
        var zone_count_off  = size(msg); msg.add(0); msg.add(1)
        var prereq_count_off = size(msg); msg.add(0); msg.add(0)
        var update_count_off = size(msg); msg.add(0); msg.add(0)
        var addtl_count_off  = size(msg); msg.add(0); msg.add(2)

        # 2. Zone section: domain, type=SOA, class=IN
        msg .. self._encode_name(domain)
        msg.add(0x00); msg.add(self.kTypeSoa)  # TYPE=SOA
        msg.add(0x00); msg.add(self.kClassIn)  # CLASS=IN

        # Save the domain offset for later use (compression)
        var domain_offset = 12   # domain starts after the 12-byte header

        var update_record_count = 0

        # 3. Update section
        # 3a. For each service, append Service Description and Service Discovery instructions
        var i = 0
        while i < size(self.services)
            var s = self.services[i]
            var inst = s[0]
            var stype = s[1]
            var port = s[2]
            var subtypes = s[3]
            var txt = s[4]
            var sstate = s[5]
            var removing = (sstate == self.kStateToRemove || sstate == self.kStateRemoving)

            # Build the FQDN of the service
            # service type: "_matter._tcp" -> labels: ["_matter", "_tcp"]
            var stype_labels = self._name_to_labels(stype)
            # instance: inst is just a string (no dots)
            var inst_label = inst

            # Service name offset: stype_labels + pointer to domain
            var service_name_off = size(msg)
            var j = 0
            while j < size(stype_labels)
                msg.add(size(stype_labels[j]))
                msg .. stype_labels[j]
                j += 1
            end
            msg.add(0xC0); msg.add(domain_offset & 0xFF)  # pointer to domain

            # PTR record
            var ptr_class = removing ? self.kClassNone : self.kClassIn
            var ptr_ttl = removing ? 0 : self.lease_sec
            var ptr_hdr = self._rr_header(self.kTypePtr, ptr_class, ptr_ttl)
            msg .. ptr_hdr
            # RDATA: instance name + pointer to service name
            var inst_off = size(msg)
            msg.add(size(inst_label))
            msg .. inst_label
            msg.add(0xC0); msg.add(service_name_off & 0xFF)
            self._rr_set_rdlength(msg, inst_off)
            update_record_count += 1

            # Subtypes: <subtype>._sub.<service>
            if !removing && subtypes != nil
                var k = 0
                var sub_off = 0
                while k < size(subtypes)
                    var sub = subtypes[k]
                    # Name: <sub>._sub.<service> = sub + "_sub" + ptr to service_name_off
                    var sub_name_off = size(msg)
                    msg.add(size(sub)); msg .. sub
                    msg.add(4); msg .. "_sub"
                    msg.add(0xC0); msg.add(service_name_off & 0xFF)
                    # PTR RDATA: pointer to instance name
                    msg .. self._rr_header(self.kTypePtr, self.kClassIn, self.lease_sec)
                    var sub_inst_off = size(msg)
                    msg.add(0xC0); msg.add(inst_off & 0xFF)
                    self._rr_set_rdlength(msg, sub_inst_off)
                    update_record_count += 1
                    k += 1
                end
            end

            if !removing
                # Service Description: <instance>.<service> (delete-all then SRV + TXT)
                # "delete all RRsets from a name" (RFC 2136 §2.5.3)
                msg.add(0xC0); msg.add(inst_off & 0xFF)  # pointer to instance name
                var del_hdr = self._rr_header(self.kTypeAny, self.kClassAny, 0)
                msg .. del_hdr
                update_record_count += 1

                # SRV: priority=0, weight=0, port, target=hostname.domain
                msg.add(0xC0); msg.add(inst_off & 0xFF)
                var srv_hdr = self._rr_header(self.kTypeSrv, self.kClassIn, self.lease_sec)
                msg .. srv_hdr
                var srv_data_off = size(msg)
                msg.add(0); msg.add(0)   # priority
                msg.add(0); msg.add(0)   # weight
                msg.add((port >> 8) & 0xFF); msg.add(port & 0xFF)  # port
                # target: <hostname>.<domain> in uncompressed form
                msg.add(size(self.hostname)); msg .. self.hostname
                msg.add(0xC0); msg.add(domain_offset & 0xFF)
                self._rr_set_rdlength(msg, srv_data_off)
                update_record_count += 1

                # TXT records
                msg.add(0xC0); msg.add(inst_off & 0xFF)
                var txt_hdr = self._rr_header(self.kTypeTxt, self.kClassIn, self.lease_sec)
                msg .. txt_hdr
                var txt_data_off = size(msg)
                if txt != nil
                    # Each entry: <len><key>=<value>
                    var entries = self._txt_to_bytes(txt)
                    msg .. entries
                end
                self._rr_set_rdlength(msg, txt_data_off)
                update_record_count += 1
            end
            i += 1
        end

        # 3b. Host description (delete-all, AAAA, KEY)
        var host_name_off = size(msg)
        msg.add(size(self.hostname)); msg .. self.hostname
        msg.add(0xC0); msg.add(domain_offset & 0xFF)

        # delete-all RRset
        var host_del = self._rr_header(self.kTypeAny, self.kClassAny, 0)
        msg .. host_del
        update_record_count += 1

        # AAAA records: one per address (we just use the mesh-local EID).
        # The driver doesn't expose addresses to us; we read them via OT.
        var addrs = self._get_thread_addresses()
        var j = 0
        while j < size(addrs)
            msg.add(0xC0); msg.add(host_name_off & 0xFF)
            var aaaa_hdr = self._rr_header(self.kTypeAaaa, self.kClassIn, self.lease_sec)
            msg .. aaaa_hdr
            var aaaa_data_off = size(msg)
            msg .. addrs[j]
            self._rr_set_rdlength(msg, aaaa_data_off)
            update_record_count += 1
            j += 1
        end

        # KEY record: uncompressed, with our public key
        msg.add(0xC0); msg.add(host_name_off & 0xFF)
        var key_hdr_off = size(msg)
        msg .. self._rr_header(self.kTypeKey, self.kClassIn, self.lease_sec)
        var key_data_off = size(msg)
        # flags
        msg.add(self.kKeyFlagsHigh)
        msg.add(self.kKeyFlagsLow)
        msg.add(self.kProtocolDnsSec)
        msg.add(self.kAlgorithmEcdsaP256Sha256)
        # public key (64 bytes)
        msg .. self.key_pub
        self._rr_set_rdlength(msg, key_data_off)
        update_record_count += 1

        # 4. Additional section
        # 4a. OPT (Update Lease option)
        # OPT has root name (empty) + type=OPT, class=udpsize, ttl=0, rdlen=N
        msg.add(0x00)  # root name
        msg.add(0); msg.add(self.kTypeOpt)
        msg.add((self.kUdpPayloadSize >> 8) & 0xFF); msg.add(self.kUdpPayloadSize & 0xFF)
        msg.add(0); msg.add(0); msg.add(0); msg.add(0)  # TTL
        # RDATA: Update Lease option
        var lease_opt_off = size(msg)
        msg.add(0); msg.add(self.kOptUpdateLease)  # option code
        msg.add(0); msg.add(8)                     # option length (8 = lease + key_lease)
        msg.add((self.lease_sec >> 24) & 0xFF)
        msg.add((self.lease_sec >> 16) & 0xFF)
        msg.add((self.lease_sec >> 8) & 0xFF)
        msg.add(self.lease_sec & 0xFF)
        msg.add((self.key_lease_sec >> 24) & 0xFF)
        msg.add((self.key_lease_sec >> 16) & 0xFF)
        msg.add((self.key_lease_sec >> 8) & 0xFF)
        msg.add(self.key_lease_sec & 0xFF)
        var lease_opt_end = size(msg)
        var lease_opt_len = lease_opt_end - lease_opt_off
        # Fix OPT RDLENGTH (it's at lease_opt_off - 2 in the OPT header)
        msg[lease_opt_off - 2] = (lease_opt_len >> 8) & 0xFF
        msg[lease_opt_off - 1] = lease_opt_len & 0xFF

        # 4b. SIG(0) record (placeholder, to be signed and rewritten)
        # Owner name is root (single 0x00 byte).
        var sig_owner_off = size(msg)
        msg.add(0x00)
        # SIG(0) type covered = 0
        msg.add(0); msg.add(self.kTypeSig)
        # CLASS = ANY (RFC 2931)
        msg.add(0); msg.add(self.kClassAny)
        # TTL = 0
        msg.add(0); msg.add(0); msg.add(0); msg.add(0)
        # RDLENGTH placeholder
        var sig_rdlen_off = size(msg)
        msg.add(0); msg.add(0)
        # SIG RDATA:
        var sig_rdata_off = size(msg)
        # type covered = 0
        msg.add(0); msg.add(0)
        # algorithm = 13
        msg.add(self.kAlgorithmEcdsaP256Sha256)
        # labels = 0
        msg.add(0)
        # original TTL = 0
        msg.add(0); msg.add(0); msg.add(0); msg.add(0)
        # signature expiration = 0
        msg.add(0); msg.add(0); msg.add(0); msg.add(0)
        # signature inception = 0
        msg.add(0); msg.add(0); msg.add(0); msg.add(0)
        # key tag = 0
        msg.add(0); msg.add(0)
        # signer's name in canonical (uncompressed) form: <hostname>.default.service.arpa.
        var signer_name_off = size(msg)
        msg.add(size(self.hostname)); msg .. self.hostname
        var k = 0
        var dn = ["default", "service", "arpa"]
        while k < size(dn)
            msg.add(size(dn[k]))
            msg .. dn[k]
            k += 1
        end
        msg.add(0x00)
        var signer_name_end = size(msg)
        # signature placeholder (will be filled)
        var sig_placeholder_off = size(msg)
        # We don't know the size of the ASN.1/DER yet; reserve the maximum
        # (72 bytes) and fix at the end.
        var max_sig_size = 72
        var sig_i = 0
        while sig_i < max_sig_size
            msg.add(0xFF)
            sig_i += 1
        end

        # Set the SIG RDLENGTH to max_sig_size for now
        var sig_end_off = size(msg)
        msg[sig_rdlen_off]     = (max_sig_size >> 8) & 0xFF
        msg[sig_rdlen_off + 1] = max_sig_size & 0xFF

        # 5. Update the header counts
        # Header was: [ID 2][Flags 2][ZOC 2][PRC 2][UPC 2][ADC 2]
        # We need to *reduce* ADCOUNT by 1 before signing (so SIG is not
        # included in the signature), then sign, then restore ADCOUNT=2.
        msg[addtl_count_off]     = 0
        msg[addtl_count_off + 1] = 1

        # Build the data to sign:
        #   SIG RDATA wire (with empty signature) || DNS message from byte 0
        #   to the start of the SIG record.
        var sig_rdata = bytes()
        sig_rdata .. msg[sig_rdata_off .. sig_placeholder_off - 1]
        var to_sign = sig_rdata + msg[0 .. sig_owner_off - 1]

        # Sign and low-S normalize. Note: ecdsa_sign_sha256 takes the raw
        # message bytes and hashes them with SHA-256 internally — do NOT
        # pre-hash.
        if self.key_priv == nil
            raise "internal_error", "SRP key not set — call set_key() first"
        end
        var sig_raw
        try
            sig_raw = crypto.EC_P256().ecdsa_sign_sha256(self.key_priv, to_sign)
        except .. as e, m
            raise "internal_error", format("ECDSA sign failed: %s %s", str(e), str(m))
        end
        if sig_raw == nil
            raise "internal_error", "ECDSA sign returned nil"
        end
        var sig_norm = self._normalize_low_s(sig_raw)
        var sig_der = self._ecdsa_sig_der(sig_norm)
        if sig_der == nil
            raise "internal_error", "ECDSA sig DER encoding failed"
        end
        var sig_len = size(sig_der)
        if sig_len > max_sig_size
            raise "internal_error", "ECDSA sig DER too large"
        end

        # Copy the actual signature into the placeholder region, then
        # truncate the message so the trailing zero-pad bytes don't
        # bloat the CoAP payload.
        var m = 0
        while m < sig_len
            msg[sig_placeholder_off + m] = sig_der[m]
            m += 1
        end
        # Truncate to exact size (placeholder was 72 bytes, we use sig_len)
        msg.resize(sig_placeholder_off + sig_len)
        # Update SIG RDLENGTH to actual length
        msg[sig_rdlen_off]     = (sig_len >> 8) & 0xFF
        msg[sig_rdlen_off + 1] = sig_len & 0xFF

        # Restore ADCOUNT to 2 (now SIG is included)
        msg[addtl_count_off]     = 0
        msg[addtl_count_off + 1] = 2

        # Update UPCOUNT
        msg[update_count_off]     = (update_record_count >> 8) & 0xFF
        msg[update_count_off + 1] = update_record_count & 0xFF

        return msg
    end

    #########################################################################
    # TXT record serialization (RFC 6763 §6)
    #########################################################################
    def _txt_to_bytes(txt)
        var out = bytes()
        # txt is a map; iterate its keys
        try
            var keys = txt.keys()
            var i = 0
            while i < size(keys)
                var k = keys[i]
                var v = txt[k]
                var s = format("%s=%s", str(k), str(v))
                var slen = size(s)
                if slen > 255
                    slen = 255
                end
                out.add(slen)
                out .. s
                i += 1
            end
        except ..
            # not a map; ignore
        end
        return out
    end

    #########################################################################
    # Get our Thread addresses
    #########################################################################
    def _get_thread_addresses()
        # Try the mesh-local EID first; if multiple, return all unicast
        # addresses that are not link-local.
        var addrs = []
        try
            import OT
            import string
            var a = OT.get_ipaddr()
            if a != nil
                var i = 0
                while i < size(a)
                    var s = a[i]
                    # Skip link-local fe80::/10
                    if size(s) >= 4 && string.find(s, "fe8") == 0
                        # fe80..febf
                        i += 1
                        continue
                    end
                    try
                        addrs.push(self._ipv6_string_to_bytes(s))
                    except .. as e2, m2
                        log(format("MTR: SRP skip bad addr %s: %s %s", str(s), str(e2), str(m2)), 3)
                    end
                    i += 1
                end
            end
        except .. as e, m
            log(format("MTR: SRP get addr FAILED: %s %s", str(e), str(m)), 2)
        end
        return addrs
    end

    def _ipv6_string_to_bytes(s)
        # Parse "XXXX:XXXX:XXXX:XXXX:XXXX:XXXX:XXXX:XXXX" to 16 bytes.
        # NOTE: bytes(-16) creates a fixed, zero-filled 16-byte buffer.
        # bytes(16) would only RESERVE 16 bytes (len stays 0) and indexing fails.
        var out = bytes(-16)
        # Strip optional "[" and "]"
        if size(s) > 0 && s[0] == '['
            s = s[1 .. size(s) - 1]
        end
        if size(s) > 0 && s[size(s) - 1] == ']'
            s = s[0 .. size(s) - 2]
        end
        # Handle "::" (zero compression)
        import string
        var parts = string.split(s, ":")
        # Collapse "::"
        var empty_idx = -1
        var j = 0
        while j < size(parts)
            if size(parts[j]) == 0
                empty_idx = j
            end
            j += 1
        end
        var addr_parts = []
        if empty_idx >= 0
            var k = 0
            while k < empty_idx
                addr_parts.push(parts[k])
                k += 1
            end
            var num_zeros = 8 - (size(parts) - 1)
            k = 0
            while k < num_zeros
                addr_parts.push("0")
                k += 1
            end
            k = empty_idx + 1
            while k < size(parts)
                addr_parts.push(parts[k])
                k += 1
            end
        else
            addr_parts = parts
        end
        if size(addr_parts) != 8
            return out   # parse failed
        end
        var b = 0
        var p = 0
        while p < 8
            # Berry's int() ignores a base argument; prepend "0x" so the
            # string is parsed as hexadecimal (int("0x830b") -> 33547).
            var v = int("0x" + addr_parts[p])
            out[b]     = (v >> 8) & 0xFF
            out[b + 1] = v & 0xFF
            b += 2
            p += 1
        end
        return out
    end
end

matter.SRP_Client = Matter_SRP_Client
#else
class Matter_SRP_Client end # will be discarded
#endif // USE_MATTER_THREAD
