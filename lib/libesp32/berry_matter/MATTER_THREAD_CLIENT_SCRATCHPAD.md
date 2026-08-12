# Matter-over-Thread — Client Path & WIP Scratchpad

> Snapshot taken 2026-06-30 21:54:27 on branch `matter_thread` at commit `d14e55cac`.
>
> Purpose: preserve all uncommitted "new" work so the working tree can be
> `git stash`ed back to the last commit, and individual solutions cherry-picked later.
>
> **Excluded from this snapshot** (intentionally — build noise, not code):
> - `sdkconfig.defaults` (2838 lines of auto-generated IDF config leaked in)
> - `CMakeLists.txt` (a deletion, not new code — must be restored for the build)
> - `MATTER_THREAD_PLAN.md` (documentation; its own diff lives in git)

---

## How to use

```
# 1. (optional) keep this scratchpad out of the stash:
git add lib/libesp32/berry_matter/MATTER_THREAD_CLIENT_SCRATCHPAD.md

# 2. stash everything else back to HEAD (tracked + untracked):
git stash push -u -m 'matter-thread client WIP'

# 3. later, re-apply a single hunk interactively from the stash, or copy
#    snippets out of this file by hand.
```

---

## Part 1 — Diff of modified tracked files (vs HEAD)

Apply with `git apply` after stashing, or copy individual hunks.

````diff
diff --git a/lib/libesp32/BearThread/include/bearthread-core-config.h b/lib/libesp32/BearThread/include/bearthread-core-config.h
index ec5cfec1b..12b673fea 100644
--- a/lib/libesp32/BearThread/include/bearthread-core-config.h
+++ b/lib/libesp32/BearThread/include/bearthread-core-config.h
@@ -83,7 +83,7 @@
 #define OPENTHREAD_CONFIG_SRP_CLIENT_AUTO_START_DEFAULT_MODE   1
 #define OPENTHREAD_CONFIG_SRP_CLIENT_BUFFERS_MAX_SERVICES      5
 #define OPENTHREAD_CONFIG_ECDSA_ENABLE                         1
-#define OPENTHREAD_CONFIG_DNS_CLIENT_ENABLE                    0
+#define OPENTHREAD_CONFIG_DNS_CLIENT_ENABLE                    1
 #define OPENTHREAD_CONFIG_COAP_API_ENABLE                      0
 #define OPENTHREAD_CONFIG_IP6_SLAAC_ENABLE                     1
 #define OPENTHREAD_CONFIG_TMF_NETDATA_SERVICE_ENABLE           1
@@ -102,7 +102,7 @@
 #define OPENTHREAD_CONFIG_LOG_LEVEL_DYNAMIC_ENABLE             1
 
 /* ---- Resource sizing ---- */
-#define OPENTHREAD_CONFIG_NUM_MESSAGE_BUFFERS                  65
+#define OPENTHREAD_CONFIG_NUM_MESSAGE_BUFFERS                  96 //65
 #define OPENTHREAD_CONFIG_6LOWPAN_REASSEMBLY_TIMEOUT          10
 #define OPENTHREAD_CONFIG_MAC_MAX_CSMA_BACKOFFS_DIRECT         4
 #define OPENTHREAD_CONFIG_TMF_ADDRESS_QUERY_TIMEOUT           3
diff --git a/lib/libesp32/BearThread/include/bt_platform.h b/lib/libesp32/BearThread/include/bt_platform.h
index a7568a99c..5003b4e93 100644
--- a/lib/libesp32/BearThread/include/bt_platform.h
+++ b/lib/libesp32/BearThread/include/bt_platform.h
@@ -49,7 +49,6 @@ esp_err_t bt_launch_mainloop(void);
 /* ---- Radio platform ---- */
 esp_err_t bt_radio_init(void);
 void      bt_radio_deinit(void);
-void      bt_radio_reclaim(void);
 void      bt_radio_update(fd_set *read_fds, int *max_fd);
 esp_err_t bt_radio_process(otInstance *instance, const fd_set *read_fds);
 
diff --git a/lib/libesp32/BearThread/src/bt_radio.c b/lib/libesp32/BearThread/src/bt_radio.c
index 5c47bf4b0..a9844bdde 100644
--- a/lib/libesp32/BearThread/src/bt_radio.c
+++ b/lib/libesp32/BearThread/src/bt_radio.c
@@ -126,18 +126,6 @@ esp_err_t bt_radio_init(void)
     return ESP_OK;
 }
 
-void bt_radio_reclaim(void)
-{
-    if (!s_radio_initialized) return;
-    esp_ieee802154_enable();
-    esp_ieee802154_set_panid(s_panid);
-    esp_ieee802154_set_extended_address(s_extaddr);
-    esp_ieee802154_set_short_address(s_shortaddr);
-    esp_ieee802154_set_rx_when_idle(true);
-    esp_ieee802154_set_channel(s_channel);
-    esp_ieee802154_receive();
-}
-
 void bt_radio_deinit(void)
 {
     if (s_radio_event_fd >= 0) {
diff --git a/lib/libesp32/berry_matter/src/be_matter_module.c b/lib/libesp32/berry_matter/src/be_matter_module.c
index e6baf0290..cc9faf06a 100644
--- a/lib/libesp32/berry_matter/src/be_matter_module.c
+++ b/lib/libesp32/berry_matter/src/be_matter_module.c
@@ -285,8 +285,15 @@ extern const bclass be_class_Matter_TLV;   // need to declare it upfront because
 #include "solidify/solidified_Matter_Plugin_1_z_Root_Thread.h"
 #include "solidify/solidified_Matter_z_Commissioning_Thread.h"
 #include "solidify/solidified_Matter_zz_Device_Thread.h"
+#include "solidify/solidified_Matter_Operational_Discovery_Thread.h"
 #endif //USE_MATTER_THREAD
 
+#if USE_MATTER_CLIENT
+#include "solidify/solidified_Matter_Commissioning_Initiator.h"
+#include "solidify/solidified_Matter_IM_Message_Client.h"
+#include "solidify/solidified_Matter_Client.h"
+#endif //USE_MATTER_CLIENT
+
 #include "solidify/solidified_Matter_zz_Device.h"
 
 #include "be_fixed_matter.h"
@@ -460,6 +467,20 @@ module matter (scope: global, strings: weak) {
   // optional Matter Thread Device core class
   Device_Thread, class(be_class_Matter_Device_Thread), USE_MATTER_THREAD
 
+  // optional Matter Thread operational discovery
+  OperationalDiscovery, class(be_class_Matter_Operational_Discovery), USE_MATTER_THREAD
+
+  // optional CASE initiator
+  Commissioning_Initiator, class(be_class_Matter_Commissioning_Initiator), USE_MATTER_CLIENT
+
+  // optional outgoing IM requests (client)
+  IM_ReadRequest_Out, class(be_class_Matter_IM_ReadRequest_Out), USE_MATTER_CLIENT
+  IM_InvokeRequest_Out, class(be_class_Matter_IM_InvokeRequest_Out), USE_MATTER_CLIENT
+  IM_WriteRequest_Out, class(be_class_Matter_IM_WriteRequest_Out), USE_MATTER_CLIENT
+
+  // optional application-facing client API
+  Client, class(be_class_Matter_Client), USE_MATTER_CLIENT
+
   // Matter Device core class
   Device, class(be_class_Matter_Device)
 
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Context.be b/lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Context.be
index 0a7b38e24..351720d6a 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Context.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Context.be
@@ -35,7 +35,10 @@ class Matter_Commisioning_Context
 
   var responder                       # reference to the caller, sending packets
   var device                          # root device object
-  
+#if USE_MATTER_CLIENT
+  var initiator                       # CASE initiator (Matter_Commissioning_Initiator), nil if none
+#endif 
+
   def init(responder)
     import crypto
     self.responder = responder
@@ -59,6 +62,18 @@ class Matter_Commisioning_Context
     end
 
     # log("MTR: received message " + matter.inspect(msg), 4)
+#if USE_MATTER_CLIENT
+    # Route to CASE initiator if active
+    if self.initiator != nil && self.initiator.is_pending()
+      if   msg.opcode == 0x31
+        return self.initiator.parse_Sigma2(msg)
+      elif msg.opcode == 0x33
+        return self.initiator.parse_Sigma2Resume(msg)
+      elif msg.opcode == 0x40
+        return self.initiator.parse_StatusReport(msg)
+      end
+    end
+#endif
     if   msg.opcode == 0x10
       # don't need to do anything, the message is acked already before this call
     elif msg.opcode == 0x20
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Data.be b/lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Data.be
index 0d3d30a58..f88934c01 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Data.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Data.be
@@ -238,6 +238,23 @@ class Matter_Sigma2
     s2.add_TLV(7, 0x05 #-TLV.U2-#, 1)                               # MAX_PATHS_PER_INVOKE
     return s.tlv2raw(b)
   end
+
+#if USE_MATTER_CLIENT
+  def parse(b, idx)
+    if idx == nil    idx = 0 end
+    var val = matter.TLV.parse(b, idx)
+    self.responderRandom = val.getsubval(1)
+    self.responderSessionId = val.getsubval(2)
+    self.responderEphPubKey = val.getsubval(3)
+    self.encrypted2 = val.getsubval(4)
+    var responderSEDParams = val.findsub(5)
+    if responderSEDParams != nil
+      self.SLEEPY_IDLE_INTERVAL = responderSEDParams.findsubval(1)
+      self.SLEEPY_ACTIVE_INTERVAL = responderSEDParams.findsubval(2)
+    end
+    return self
+  end
+#endif //USE_MATTER_CLIENT
 end
 matter.Sigma2 = Matter_Sigma2
 
@@ -269,6 +286,17 @@ class Matter_Sigma2Resume
     s2.add_TLV(7, 0x05 #-TLV.U2-#, 1)                               # MAX_PATHS_PER_INVOKE
     return s.tlv2raw(b)
   end
+
+#if USE_MATTER_CLIENT
+  def parse(b, idx)
+    if idx == nil    idx = 0 end
+    var val = matter.TLV.parse(b, idx)
+    self.resumptionID = val.getsubval(1)
+    self.sigma2ResumeMIC = val.getsubval(2)
+    self.responderSessionID = val.getsubval(3)
+    return self
+  end
+#endif //USE_MATTER_CLIENT
 end
 matter.Sigma2Resume = Matter_Sigma2Resume
 
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_Fabric.be b/lib/libesp32/berry_matter/src/embedded/Matter_Fabric.be
index a6f6ed3ed..fee339d2f 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_Fabric.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_Fabric.be
@@ -69,6 +69,9 @@ class Matter_Fabric : Matter_Expirable
   # Admin info extracted from NOC/ICAC
   var admin_subject
   var admin_vendor
+  # TrustedTimeSource (Time Synchronization cluster 0x0038, attribute 0x0003)
+  var trusted_time_node_id        # bytes(8) node_id of designated time source, or nil
+  var trusted_time_endpoint       # int endpoint number, or nil
 
   #############################################################
   def init(store)
@@ -92,10 +95,11 @@ class Matter_Fabric : Matter_Expirable
   def get_device_id()         return self.device_id         end
   def get_fabric_compressed() return self.fabric_compressed end
   def get_fabric_label()      return self.fabric_label      end
-  def get_admin_subject()     return self.admin_subject     end
   def get_admin_vendor()      return self.admin_vendor      end
   def get_ca()                return self.root_ca_certificate end
   def get_fabric_index()      return self.fabric_index      end
+  def get_trusted_time_node_id()    return self.trusted_time_node_id    end
+  def get_trusted_time_endpoint()   return self.trusted_time_endpoint   end
 
   def get_fabric_id_as_int64()
     return int64.frombytes(self.fabric_id)
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_IM.be b/lib/libesp32/berry_matter/src/embedded/Matter_IM.be
index b7b30de9a..6af8b0ed9 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_IM.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_IM.be
@@ -82,23 +82,35 @@ class Matter_IM
       self.send_ack_now(msg)
       return self.subscribe_request(msg, val)
     elif opcode == 0x04   # Subscribe Response
-      # return self.subscribe_response(msg, val)
-      return false                                    # not implemented for Matter device
+#if USE_MATTER_CLIENT
+      return self.subscribe_response(msg, val)
+#else
+      return false
+#endif
     elif opcode == 0x05   # Report Data
-      # return self.report_data(msg, val)
-      return false                                    # not implemented for Matter device
+#if USE_MATTER_CLIENT
+      return self.report_data(msg, val)
+#else
+      return false
+#endif
     elif opcode == 0x06   # Write Request
       self.send_ack_now(msg)
       return self.process_write_request(msg, val)
     elif opcode == 0x07   # Write Response
-      # return self.process_write_response(msg, val)  # not implemented for Matter device
+#if USE_MATTER_CLIENT
+      return self.process_write_response(msg, val)
+#else
       return false
+#endif
     elif opcode == 0x08   # Invoke Request
       # self.send_ack_now(msg)      # to improve latency, we don't automatically Ack on invoke request
       return self.process_invoke_request(msg, val)
     elif opcode == 0x09   # Invoke Response
-      # return self.process_invoke_response(msg, val)
-      return false                                    # not implemented for Matter device
+#if USE_MATTER_CLIENT
+      return self.process_invoke_response(msg, val)
+#else
+      return false
+#endif
     elif opcode == 0x0A   # Timed Request
       return self.process_timed_request(msg, val)
     end
@@ -1061,20 +1073,34 @@ class Matter_IM
   #############################################################
   # process IM 0x04 Subscribe Response
   #
-  # def subscribe_response(msg, val)
-  #   var query = matter.SubscribeResponseMessage().from_TLV(val)
-  #   # log("MTR: received SubscribeResponsetMessage=" + str(query), 4)
-  #   return false
-  # end
+#if USE_MATTER_CLIENT
+  def subscribe_response(msg, val)
+    var query = matter.SubscribeResponseMessage().from_TLV(val)
+    log(format("MTR: SubscribeResponse sub_id=%i", query.subscription_id), 3)
+    var pending = self.find_sendqueue_by_exchangeid(msg.exchange_id)
+    if pending && pending.callback
+      pending.callback(query)
+    end
+    self.remove_sendqueue_by_exchangeid(msg.exchange_id)
+    return true
+  end
+#endif
 
   #############################################################
   # process IM 0x05 ReportData
   #
-  # def report_data(msg, val)
-  #   var query = matter.ReportDataMessage().from_TLV(val)
-  #   # log("MTR: received ReportDataMessage=" + str(query), 4)
-  #   return false
-  # end
+#if USE_MATTER_CLIENT
+  def report_data(msg, val)
+    var query = matter.ReportDataMessage().from_TLV(val)
+    log(format("MTR: ReportData rcvd attrs=%s", str(query.attribute_reports)), 4)
+    var pending = self.find_sendqueue_by_exchangeid(msg.exchange_id)
+    if pending && pending.callback
+      pending.callback(query)
+    end
+    self.remove_sendqueue_by_exchangeid(msg.exchange_id)
+    return true
+  end
+#endif
 
 
   #############################################################
@@ -1186,20 +1212,34 @@ class Matter_IM
   #############################################################
   # process IM 0x07 Write Response
   #
-  # def process_write_response(msg, val)
-  #   var query = matter.WriteResponseMessage().from_TLV(val)
-  #   # log("MTR: received WriteResponseMessage=" + str(query), 4)
-  #   return false
-  # end
+#if USE_MATTER_CLIENT
+  def process_write_response(msg, val)
+    var query = matter.WriteResponseMessage().from_TLV(val)
+    log(format("MTR: WriteResponse rcvd"), 4)
+    var pending = self.find_sendqueue_by_exchangeid(msg.exchange_id)
+    if pending && pending.callback
+      pending.callback(query)
+    end
+    self.remove_sendqueue_by_exchangeid(msg.exchange_id)
+    return true
+  end
+#endif
 
   #############################################################
   # process IM 0x09 Invoke Response
   #
-  # def process_invoke_response(msg, val)
-  #   var query = matter.InvokeResponseMessage().from_TLV(val)
-  #   # log("MTR: received InvokeResponseMessage=" + str(query), 4)
-  #   return false
-  # end
+#if USE_MATTER_CLIENT
+  def process_invoke_response(msg, val)
+    var query = matter.InvokeResponseMessage().from_TLV(val)
+    log(format("MTR: InvokeResponse rcvd"), 4)
+    var pending = self.find_sendqueue_by_exchangeid(msg.exchange_id)
+    if pending && pending.callback
+      pending.callback(query)
+    end
+    self.remove_sendqueue_by_exchangeid(msg.exchange_id)
+    return true
+  end
+#endif
 
   #############################################################
   # process IM 0x0A Timed Request
@@ -1260,7 +1300,7 @@ class Matter_IM
       fake_read.attributes_requests.push(p1)
     end
 
-    log(format("MTR: <Sub_Data  (%6i) sub=%i", session.local_session_id, sub.subscription_id), 3)
+    log(format("MTR: <Sub_Data  (%6i) sub=%i to [%s]:%i", session.local_session_id, sub.subscription_id, session._ip, session._port), 3)
     sub.is_keep_alive = false             # sending an actual data update
 
     var generator_or_arr = self.process_read_or_subscribe_request_pull(fake_read, nil #-no msg-#)
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_IM_Data.be b/lib/libesp32/berry_matter/src/embedded/Matter_IM_Data.be
index 85355633a..f8c654cc4 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_IM_Data.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_IM_Data.be
@@ -615,18 +615,19 @@ class Matter_ReadRequestMessage : Matter_IM_Message_base
     return self
   end
 
-  # to_TLV not used in Matter Device
-  # def to_TLV()
-  #   var TLV = matter.TLV
-  #   var s = TLV.Matter_TLV_struct()
-  #   self.to_TLV_array(s, 0, self.attributes_requests)
-  #   self.to_TLV_array(s, 1, self.event_requests)
-  #   self.to_TLV_array(s, 2, self.event_filters)
-  #   s.add_TLV(3, 0x08 #-TLV.BOOL-#, self.fabric_filtered)
-  #   self.to_TLV_array(s, 4, self.data_version_filters)
-  #   s.add_TLV(0xFF, 0x04 #-TLV.U1-#, self.InteractionModelRevision)
-  #   return s
-  # end
+#if USE_MATTER_CLIENT
+  def to_TLV()
+    var TLV = matter.TLV
+    var s = TLV.Matter_TLV_struct()
+    self.to_TLV_array(s, 0, self.attributes_requests)
+    self.to_TLV_array(s, 1, self.event_requests)
+    self.to_TLV_array(s, 2, self.event_filters)
+    s.add_TLV(3, 0x08 #-TLV.BOOL-#, self.fabric_filtered)
+    self.to_TLV_array(s, 4, self.data_version_filters)
+    s.add_TLV(0xFF, 0x04 #-TLV.U1-#, self.InteractionModelRevision)
+    return s
+  end
+#endif
 end
 matter.ReadRequestMessage = Matter_ReadRequestMessage
 
@@ -876,16 +877,17 @@ class Matter_ReportDataMessage : Matter_IM_Message_base
   var suppress_response           # bool
 
   # decode from TLV
-  # from_TLV not used in Matter Device
-  # def from_TLV(val)
-  #   if val == nil     return nil end
-  #   self.subscription_id = val.findsubval(0)
-  #   self.attribute_reports = self.from_TLV_array(val.findsubval(1), matter.AttributeReportIB)
-  #   self.event_reports = self.from_TLV_array(val.findsubval(2), matter.EventReportIB)
-  #   self.more_chunked_messages = val.findsubval(3)
-  #   self.suppress_response = val.findsubval(4)
-  #   return self
-  # end
+#if USE_MATTER_CLIENT
+  def from_TLV(val)
+    if val == nil     return nil end
+    self.subscription_id = val.findsubval(0)
+    self.attribute_reports = self.from_TLV_array(val.findsubval(1), matter.AttributeReportIB)
+    self.event_reports = self.from_TLV_array(val.findsubval(2), matter.EventReportIB)
+    self.more_chunked_messages = val.findsubval(3)
+    self.suppress_response = val.findsubval(4)
+    return self
+  end
+#endif
 
   def to_TLV()
     var TLV = matter.TLV
@@ -954,13 +956,14 @@ class Matter_SubscribeResponseMessage : Matter_IM_Message_base
   var max_interval                # u16
 
   # decode from TLV
-  # from_TLV not used in Matter Device
-  # def from_TLV(val)
-  #   if val == nil     return nil end
-  #   self.subscription_id = val.findsubval(0)
-  #   self.max_interval = val.findsubval(2)
-  #   return self
-  # end
+#if USE_MATTER_CLIENT
+  def from_TLV(val)
+    if val == nil     return nil end
+    self.subscription_id = val.findsubval(0)
+    self.max_interval = val.findsubval(2)
+    return self
+  end
+#endif
 
   def to_TLV()
     var TLV = matter.TLV
@@ -992,17 +995,18 @@ class Matter_WriteRequestMessage : Matter_IM_Message_base
     return self
   end
 
-  # to_TLV not used in Matter Device
-  # def to_TLV()
-  #   var TLV = matter.TLV
-  #   var s = TLV.Matter_TLV_struct()
-  #   s.add_TLV(0, 0x08 #-TLV.BOOL-#, self.suppress_response)
-  #   s.add_TLV(1, 0x08 #-TLV.BOOL-#, self.timed_request)
-  #   self.to_TLV_array(s, 2, self.write_requests)
-  #   s.add_TLV(3, 0x08 #-TLV.BOOL-#, self.more_chunked_messages)
-  #   s.add_TLV(0xFF, 0x04 #-TLV.U1-#, self.InteractionModelRevision)
-  #   return s
-  # end
+#if USE_MATTER_CLIENT
+  def to_TLV()
+    var TLV = matter.TLV
+    var s = TLV.Matter_TLV_struct()
+    s.add_TLV(0, 0x08 #-TLV.BOOL-#, self.suppress_response)
+    s.add_TLV(1, 0x08 #-TLV.BOOL-#, self.timed_request)
+    self.to_TLV_array(s, 2, self.write_requests)
+    s.add_TLV(3, 0x08 #-TLV.BOOL-#, self.more_chunked_messages)
+    s.add_TLV(0xFF, 0x04 #-TLV.U1-#, self.InteractionModelRevision)
+    return s
+  end
+#endif
 end
 matter.WriteRequestMessage = Matter_WriteRequestMessage
 
@@ -1013,12 +1017,13 @@ class Matter_WriteResponseMessage : Matter_IM_Message_base
   var write_responses             # array of AttributeStatusIB
 
   # decode from TLV
-  # from_TLV not used in Matter Device
-  # def from_TLV(val)
-  #   if val == nil     return nil end
-  #   self.write_requests = self.from_TLV_array(val.findsubval(0), matter.AttributeStatusIB)
-  #   return self
-  # end
+#if USE_MATTER_CLIENT
+  def from_TLV(val)
+    if val == nil     return nil end
+    self.write_responses = self.from_TLV_array(val.findsubval(0), matter.AttributeStatusIB)
+    return self
+  end
+#endif
 
   def to_TLV()
     var TLV = matter.TLV
@@ -1071,16 +1076,17 @@ class Matter_InvokeRequestMessage : Matter_IM_Message_base
     return self
   end
 
-  # to_TLV not used in Matter Device
-  # def to_TLV()
-  #   var TLV = matter.TLV
-  #   var s = TLV.Matter_TLV_struct()
-  #   s.add_TLV(0, 0x08 #-TLV.BOOL-#, self.suppress_response)
-  #   s.add_TLV(1, 0x08 #-TLV.BOOL-#, self.timed_request)
-  #   self.to_TLV_array(s, 2, self.invoke_requests)
-  #   s.add_TLV(0xFF, 0x04 #-TLV.U1-#, self.InteractionModelRevision)
-  #   return s
-  # end
+#if USE_MATTER_CLIENT
+  def to_TLV()
+    var TLV = matter.TLV
+    var s = TLV.Matter_TLV_struct()
+    s.add_TLV(0, 0x08 #-TLV.BOOL-#, self.suppress_response)
+    s.add_TLV(1, 0x08 #-TLV.BOOL-#, self.timed_request)
+    self.to_TLV_array(s, 2, self.invoke_requests)
+    s.add_TLV(0xFF, 0x04 #-TLV.U1-#, self.InteractionModelRevision)
+    return s
+  end
+#endif
 end
 matter.InvokeRequestMessage = Matter_InvokeRequestMessage
 
@@ -1092,13 +1098,14 @@ class Matter_InvokeResponseMessage : Matter_IM_Message_base
   var invoke_responses            # array of InvokeResponseIB
 
   # decode from TLV
-  # from_TLV not used in Matter Device
-  # def from_TLV(val)
-  #   if val == nil     return nil end
-  #   self.suppress_response = val.findsubval(0)
-  #   self.invoke_responses = self.from_TLV_array(val.findsubval(1), matter.InvokeResponseIB)
-  #   return self
-  # end
+#if USE_MATTER_CLIENT
+  def from_TLV(val)
+    if val == nil     return nil end
+    self.suppress_response = val.findsubval(0)
+    self.invoke_responses = self.from_TLV_array(val.findsubval(1), matter.InvokeResponseIB)
+    return self
+  end
+#endif
 
   def to_TLV()
     var TLV = matter.TLV
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_IM_Message.be b/lib/libesp32/berry_matter/src/embedded/Matter_IM_Message.be
index e78995ce6..415bcbc1f 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_IM_Message.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_IM_Message.be
@@ -469,8 +469,8 @@ class Matter_IM_ReportDataSubscribed_Pull : Matter_IM_ReportData_Pull
 
   #################################################################################
   def reached_timeout()
-    # log(f"MTR: IM_ReportDataSubscribed_Pull reached_timeout()", 3)
-    self.sub.remove_self()
+    # log(f"MTR: IM_ReportDataSubscribed_Pull reached_timeout() sub={self.sub.subscription_id}", 3)
+    self.sub.re_arm()
   end
 
   #################################################################################
@@ -584,8 +584,8 @@ class Matter_IM_SubscribedHeartbeat : Matter_IM_ReportData_Pull
   # then the controller is not expecting any more answers,
   # remove the subscription 
   def reached_timeout()
-    # log(f"MTR: IM_SubscribedHeartbeat reached_timeout()", 3)
-    self.sub.remove_self()
+    # log(f"MTR: IM_SubscribedHeartbeat reached_timeout() sub={self.sub.subscription_id}", 3)
+    self.sub.re_arm()
   end
 
   #################################################################################
@@ -670,7 +670,8 @@ class Matter_IM_SubscribeResponse_Pull : Matter_IM_ReportData_Pull
       self.last_counter = resp.message_counter
       # log(f"MTR: Send SubscribeResponseMessage sub={self.sub.subscription_id} id={resp.message_counter}", 3)
       self.sub.re_arm()
-      self.finishing = true          # remove exchange
+      self.ready = false              # prevent re-send until ack arrives
+      self.finishing = true          # wait for final ack
     end
   end
 
@@ -682,9 +683,6 @@ class Matter_IM_SubscribeResponse_Pull : Matter_IM_ReportData_Pull
       log(format("MTR: >Sub_OK    (%6i) sub=%i", msg.session.local_session_id, self.sub.subscription_id), 3)
     end
     return super(self).status_ok_received(msg)
-    if !self.report_data_phase
-      self.finishing = true
-    end
   end
 
 end
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_Message.be b/lib/libesp32/berry_matter/src/embedded/Matter_Message.be
index 4291fd64a..6a7b4f202 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_Message.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_Message.be
@@ -332,7 +332,7 @@ class Matter_Frame
       resp.local_session_id = 0
     end
       
-    resp.x_flag_i = 1                                     # we are the initiator
+    resp.x_flag_i = 0                                     # we are the session responder
     resp.opcode = opcode
     session._exchange_id += 1                            # move to next exchange_id
     resp.exchange_id = session._exchange_id | 0x10000    # special encoding for local exchange_id
@@ -420,8 +420,11 @@ class Matter_Frame
     var payload_idx = self.payload_idx
     var tag_len = 16
 
-    # encrypt the message with `i2r` key
-    var r2i = session.get_r2i()
+    # Pick the key matching x_flag_i so the controller uses the correct key
+    var key = session.get_r2i()       # default: we are session responder
+    if self.x_flag_i == 1
+      key = session.get_i2r()         # we are session initiator
+    end
 
     # recompute nonce
     var n = self.message_handler._n_bytes     # use cached bytes() object to avoid allocation
@@ -435,7 +438,7 @@ class Matter_Frame
 
     # encrypt
     raw.resize(size(raw) + tag_len)   # make room for MIC
-    var ret = crypto.AES_CCM.encrypt1(r2i,                    # secret key
+    var ret = crypto.AES_CCM.encrypt1(key,                   # secret key
                                       n, 0, size(n),          # nonce / IV
                                       raw, 0, payload_idx,    # aad
                                       raw, payload_idx, size(raw) - payload_idx - tag_len,  # encrypted - decrypted in-place
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_0.be b/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_0.be
index 3433a1497..95cab969b 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_0.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_0.be
@@ -110,6 +110,7 @@ class Matter_Plugin
     0x0006: 0x01,                           # On/Off: Lighting feature (bit 0)
     0x0008: 0x03,                           # Level Control: On/Off (bit 0) + Lighting (bit 1)
     0x0031: 0x05,                           # Eth + WiFi - the latter is needed for Bluetooth commissioning
+    0x0038: 0x08,                           # Time Synchronization: TSC (TimeSyncClient, bit 3)
     0x0046: 0x00,                           # ICD Management: 0x00 = no optional features (base SIT mode, no CIP/UAT/LITS)
     0x0062: 0x01,                           # Scenes Management: SceneNames (bit 0)
     0x0102: 1 + 4,                          # Window Covering: Lift (bit 0) + PA_LF (bit 2)
@@ -135,6 +136,7 @@ class Matter_Plugin
     # 0x0033: 1,                            # General Diagnostics - Initial Release
     # 0x0034: 1,                            # Software Diagnostics - Initial Release
     0x0038: 2,                              # Time Synchronization
+    # Note: We should also define a FEATURE_MAP for 0x0038 with TSC bit set
     # 0x003B: 1,                            # Switch - Initial Release
     # 0x003C: 1,                            # Administrator Commissioning - Initial Release
     # 0x003E: 1,                            # Node Operational Credentials - Initial Release
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_Root.be b/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_Root.be
index 9e9323a60..e05cdc58a 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_Root.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_Root.be
@@ -234,8 +234,13 @@ class Matter_Plugin_Root : Matter_Plugin
 # -------|-------------|----------|------------|---------|---------|--------|-----
 # 0x0000 | UTCTime     | epoch-us | all        | X       | null    | R V    | O
 # 0x0001 | Granularity | enum8    | 0-5        | -       | -       | R V    | M
+# 0x0003 | TrustedTimeSource | struct | -      | X       | null    | R V    | O
 # 0x0007 | LocalTime   | epoch-us | all        | X       | null    | R V    | O
 #
+# TrustedTimeSourceStruct:
+#   - NodeID (node_id): Node ID of designated time source
+#   - Endpoint (endpoint_no): Endpoint number
+#
 # Granularity Enum:
 #   0=NoTimeGranularity, 1=MinutesGranularity, 2=SecondsGranularity,
 #   3=MillisecondsGranularity, 4=MicrosecondsGranularity
@@ -728,6 +733,16 @@ class Matter_Plugin_Root : Matter_Plugin
           return tlv_solo.set(0x04 #-TLV.U1-#, 0)     # NoTimeGranularity - device has no valid time
         end
         return tlv_solo.set(0x04 #-TLV.U1-#, 3)     # MillisecondsGranularity (NTP every hour, i.e. 36ms max drift)
+      elif attribute == 0x0003          #  ---------- TrustedTimeSource / TrustedTimeSourceStruct ----------
+        var fabric = session.get_fabric()
+        if fabric.trusted_time_node_id != nil && fabric.trusted_time_endpoint != nil
+          var res = TLV.Matter_TLV_struct()
+          res.add_TLV(0, 0x07 #-TLV.U8-#, int64.frombytes(fabric.trusted_time_node_id))  # NodeID
+          res.add_TLV(1, 0x05 #-TLV.U2-#, fabric.trusted_time_endpoint)                  # Endpoint
+          return res
+        else
+          return tlv_solo.set(0x14 #-TLV.NULL-#, nil)     # nullable - not set
+        end
       # TODO add some missing args
       elif attribute == 0x0007          #  ---------- LocalTime / epoch_us ----------
         var epoch_us = int64(tasmota.rtc('local')) * int64(1000000)
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_z_Root_Thread.be b/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_z_Root_Thread.be
index 25fb4680f..cb1e740dd 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_z_Root_Thread.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_Plugin_1_z_Root_Thread.be
@@ -206,6 +206,27 @@ class Matter_Plugin_Root_Thread : Matter_Plugin_Root
                 log(format("MTR: SetUTCTime utc=%s granularity=%s", str(utc), str(gran)), 2)
                 ctx.status = 0x00 #-SUCCESS-#
                 return true
+            elif command == 0x01            #  ---------- SetTrustedTimeSource ----------
+                var fabric = session.get_fabric()
+                var tts = val.findsubval(0)     # FabricScopedTrustedTimeSourceStruct or nil
+                if tts == nil
+                    # Clear TrustedTimeSource
+                    fabric.trusted_time_node_id = nil
+                    fabric.trusted_time_endpoint = nil
+                    log("MTR: SetTrustedTimeSource cleared", 2)
+                else
+                    # Set TrustedTimeSource
+                    var node_id = tts.findsubval(0)     # NodeID (int64)
+                    var endpoint = tts.findsubval(1)    # Endpoint (int)
+                    if node_id != nil && endpoint != nil
+                        fabric.trusted_time_node_id = int64.tobytes(node_id)
+                        fabric.trusted_time_endpoint = endpoint
+                        log(format("MTR: SetTrustedTimeSource node=%s endpoint=%i", fabric.trusted_time_node_id.tohex(), endpoint), 2)
+                    end
+                end
+                fabric.save()
+                ctx.status = 0x00 #-SUCCESS-#
+                return true
             end
 
         end
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_Session.be b/lib/libesp32/berry_matter/src/embedded/Matter_Session.be
index 117c02eb6..4336257c6 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_Session.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_Session.be
@@ -258,7 +258,6 @@ class Matter_Session : Matter_Expirable
   def get_device_id()         return self._fabric ? self._fabric.device_id : nil        end
   def get_fabric_compressed() return self._fabric ? self._fabric.fabric_compressed : nil end
   def get_fabric_label()      return self._fabric ? self._fabric.fabric_label : nil     end
-  def get_admin_subject()     return self._fabric ? self._fabric.admin_subject : nil    end
   def get_admin_vendor()      return self._fabric ? self._fabric.admin_vendor : nil     end
   def get_node_id()           return self._fabric ? self._fabric.device_id : nil        end
 
diff --git a/lib/libesp32/berry_matter/src/embedded/Matter_zz_Device_Thread.be b/lib/libesp32/berry_matter/src/embedded/Matter_zz_Device_Thread.be
index 4b0937274..2971227c3 100644
--- a/lib/libesp32/berry_matter/src/embedded/Matter_zz_Device_Thread.be
+++ b/lib/libesp32/berry_matter/src/embedded/Matter_zz_Device_Thread.be
@@ -41,10 +41,11 @@ class Matter_Device_Thread : Matter_Device_BLE
     var _srp_fabrics                   # list: fabrics collected during commissioning (before start)
     var case_grace_until               # int millis: deadline to wait for CASE after AddNOC
     var packets_sent                   # list: OT UDP packets awaiting ack (retransmission)
-    var pending_radio_reclaim
-    var wifi_was_up                    # bool: last known WiFi state (for detecting OFF transitions)
     var ot_started                     # bool: OpenThread initialized
     var thread_connected               # bool: Thread network attached
+    var _time_sync_state               # 0=idle, 1=case, 2=reading, 3=done
+    var _time_sync_retries             # retry counter
+    var operational_discovery          # DNS-SD resolver
 
     #############################################################
     # init — full custom, does NOT call super.init()
@@ -75,7 +76,9 @@ class Matter_Device_Thread : Matter_Device_BLE
         self.tick = 0
         self.message_handler = matter.MessageHandler(self)
         self.events = matter.EventHandler(self)
-        self.pending_radio_reclaim = false
+        self._time_sync_state = 0       # 0=idle, 1=case, 2=reading, 3=done
+        self._time_sync_retries = 0
+        self.operational_discovery = matter.OperationalDiscovery()
         self.autoconf_device()
         tasmota.add_driver(self)
 
@@ -117,7 +120,6 @@ class Matter_Device_Thread : Matter_Device_BLE
                 self.btp = matter.BTP(self)
                 self.commissioning.init_basic_commissioning()
                 tasmota.cmd("wifi 0")
-                self.pending_radio_reclaim = true
                 tasmota.add_fast_loop(/-> BLE.loop())
                 self.on_ble_init()
                 self.ble_ready = true
@@ -587,6 +589,130 @@ class Matter_Device_Thread : Matter_Device_BLE
         log("MTR: Thread network provisioning started", 2)
     end
 
+    #############################################################
+    # Time sync — pull UTCTime from controller on boot if RTC unset
+    #############################################################
+    def _time_sync_check()
+        if self._time_sync_state == 3   return   end    # done
+        # State 1 (CASE handshake), 2 (reading UTCTime), and 5 (waiting for retry) must block any new actions
+        if self._time_sync_state == 1 || self._time_sync_state == 2 || self._time_sync_state == 5   return   end
+
+        if tasmota.rtc_utc() >= 1451602800          # clock already valid
+            self._time_sync_state = 3
+            return
+        end
+
+        if !self.thread_connected   return   end
+
+        var fabrics = self.sessions.fabrics
+        if fabrics == nil || size(fabrics) == 0   return   end
+
+        # If waiting for DNS (4) or retry backoff (5), return immediately.
+        if self._time_sync_state == 4 || self._time_sync_state == 5   return   end
+
+        # --- Slow path: DNS-SD → CASE → Read ------------------------------------
+        # Only use TrustedTimeSource (controller will reject UTCTime reads with UNSUPPORTED_ACCESS)
+        var fabric = nil
+        var target = nil
+        var fi = 0
+        while fi < size(fabrics)
+            var f = fabrics[fi]
+            if f.trusted_time_node_id != nil
+                fabric = f
+                target = f.trusted_time_node_id
+                log(format("MTR: TimeSync: using TrustedTimeSource node=%s", target.tohex()), 3)
+                break
+            end
+            fi += 1
+        end
+        if fabric == nil
+            log("MTR: TimeSync: TrustedTimeSource not set, disabled", 3)
+            self._time_sync_state = 3
+            return
+        end
+
+        var target_str = target.tohex()
+
+        # Resolve controller's IP via DNS-SD (cached).
+        # State 4 (DNS pending) is caught by the early-return above, so we only
+        # reach here from state 0 — safe to launch a new DNS query.
+        var cached = self.operational_discovery.get_cached(target, 5)
+        if cached == nil
+            log(format("MTR: TimeSync: launching DNS for %s (Thread attached)", target_str), 3)
+            self.operational_discovery.resolve_by_fabric(fabric, target)
+            self._time_sync_state = 4
+            return
+        end
+        var ip = cached["ip"]
+        var port = cached["port"]
+
+        log(format("MTR: TimeSync: CASE to %s [%s]:%i", target_str, ip, port), 2)
+        self._time_sync_state = 1
+        # NB: do NOT reset _time_sync_retries here. Resetting on every CASE launch
+        # let persistent CASE failures loop forever (each retry comes back through
+        # the cached path and re-launched CASE, never reaching the SNTP fallback).
+        # The counter is initialized in init() and only accumulates across failures.
+        self._time_sync_case(fabric, ip, port, target)
+    end
+
+    def _time_sync_case(fabric, ip, port, node_id)
+        var initiator = matter.Commissioning_Initiator(self.message_handler)
+        self.message_handler.commissioning.initiator = initiator
+        initiator.on_ready = /sess -> self._time_sync_read(sess)
+        initiator.on_error = /-> self._time_sync_retry()
+        initiator.start(fabric, ip, port, node_id)
+    end
+
+    def _time_sync_read(session)
+        log("MTR: TimeSync: reading UTCTime", 2)
+        self._time_sync_state = 2
+        var client = matter.Client(self)
+        client.read_attribute(session, 0, 0x0038, 0x0000,
+            /report -> self._time_sync_got_time(report))
+    end
+
+    def _time_sync_got_time(report_data)
+        log("MTR: TimeSync: got report_data=" + str(report_data), 2)
+        if report_data != nil
+            log("MTR: TimeSync: got attribute_reports=" + str(report_data.attribute_reports), 2)
+            var attr_reports = report_data.attribute_reports
+            if attr_reports != nil && size(attr_reports) > 0
+                log("MTR: TimeSync: got attribute_data=" + str(attr_reports[0].attribute_data), 2)
+                var attr_data = attr_reports[0].attribute_data
+                if attr_data != nil
+                    log("MTR: TimeSync: got data=" + str(attr_data.data) + " type=" + type(attr_data.data), 2)
+                    if type(attr_data.data) == 'int' && attr_data.data > 1451602800
+                        var epoch = attr_data.data
+                        log(format("MTR: TimeSync: UTCTime=%i, setting RTC", epoch), 2)
+                        tasmota.cmd("time " + str(epoch))
+                        self._time_sync_state = 3
+                        return
+                    end
+                end
+            end
+        end
+        log("MTR: TimeSync: invalid UTCTime, retrying", 3)
+        self._time_sync_retry()
+    end
+
+    def _time_sync_retry()
+        self._time_sync_state = 5  # state 5 = waiting for retry backoff timer
+        self._time_sync_retries += 1
+        if self._time_sync_retries < 5
+            var delay = 30000 * self._time_sync_retries
+            log(format("MTR: TimeSync: retry %i/5 in %ims", self._time_sync_retries, delay), 3)
+            tasmota.set_timer(delay, def ()
+                if self._time_sync_state == 5
+                    self._time_sync_state = 0
+                    self._time_sync_check()
+                end
+            end)
+        else
+            log("MTR: TimeSync: max retries, falling back to SNTP", 2)
+            self._time_sync_state = 3
+        end
+    end
+
     #############################################################
     # Timer callbacks
     #############################################################
@@ -598,6 +724,20 @@ class Matter_Device_Thread : Matter_Device_BLE
         if self.check_if_commissioned == true
             self.check_final()
         end
+        # Fix 2b: act on DNS poll result to drive the state-4 (DNS-pending) transition.
+        # On success: cache is populated; reset to state 0 so _time_sync_check() proceeds.
+        # On failure: call _time_sync_retry() for backoff (sets state back to 0 with delay).
+        var dns_result = self.operational_discovery.poll()
+        if dns_result != nil && self._time_sync_state == 4
+            if dns_result["error"] == 0
+                log("MTR: TimeSync: DNS resolved, will proceed to CASE", 3)
+                self._time_sync_state = 0   # _time_sync_check() will pick up the cache
+            else
+                log(format("MTR: TimeSync: DNS failed (err=%i), scheduling retry", dns_result["error"]), 3)
+                self._time_sync_retry()
+            end
+        end
+        self._time_sync_check()
     end
 
     def every_50ms()
@@ -622,21 +762,6 @@ class Matter_Device_Thread : Matter_Device_BLE
                 log(format("MTR: OT state poll FAILED: %s %s", str(e), str(m)), 2)
             end
         end
-        # radio reclaim on WiFi OFF transition
-        if self.ot_started
-            var wifi_up = tasmota.wifi().find("up") == true
-            if (self.pending_radio_reclaim || (self.wifi_was_up && !wifi_up)) && !wifi_up
-                try
-                    import OT
-                    OT.radio_reclaim()
-                    log("MTR: 802.15.4 radio reclaimed after WiFi shutdown", 2)
-                except .. as e, m
-                    log(format("MTR: radio reclaim FAILED: %s %s", str(e), str(m)), 2)
-                end
-            end
-            self.pending_radio_reclaim = false
-            self.wifi_was_up = wifi_up
-        end
         # OT UDP receive
         if self.started
             import OT
diff --git a/lib/libesp32/berry_tasmota/src/be_OT_lib.c b/lib/libesp32/berry_tasmota/src/be_OT_lib.c
index e0c2066e2..eeea050dd 100644
--- a/lib/libesp32/berry_tasmota/src/be_OT_lib.c
+++ b/lib/libesp32/berry_tasmota/src/be_OT_lib.c
@@ -29,9 +29,6 @@ extern int be_OT_get_ipaddr(bvm *vm);
 extern int be_OT_netdata_services(bvm *vm);
 extern int be_OT_poll_state(bvm *vm);
 
-extern void be_OT_radio_reclaim(void);
-BE_FUNC_CTYPE_DECLARE(be_OT_radio_reclaim, "", "");
-
 extern void be_OT_udp_open(struct bvm *vm, int32_t port);
 BE_FUNC_CTYPE_DECLARE(be_OT_udp_open, "", "@i");
 
@@ -70,6 +67,11 @@ BE_FUNC_CTYPE_DECLARE(be_OT_srp_get_server, "s", "");
 extern void be_OT_srp_set_lease_interval(struct bvm *vm, int32_t lease, int32_t key_lease);
 BE_FUNC_CTYPE_DECLARE(be_OT_srp_set_lease_interval, "", "@ii");
 
+extern bbool be_OT_dns_resolve_service(struct bvm *vm, const char *instance_label, const char *service_name);
+BE_FUNC_CTYPE_DECLARE(be_OT_dns_resolve_service, "b", "@ss");
+
+extern int be_OT_dns_poll_result(bvm *vm);
+
 #include "be_fixed_OT.h"
 
 /* @const_object_info_begin
@@ -82,7 +84,6 @@ module OT (scope: global) {
   get_ipaddr,     func(be_OT_get_ipaddr)
   poll_state,     func(be_OT_poll_state)
   netdata_services, func(be_OT_netdata_services)
-  radio_reclaim,  ctype_func(be_OT_radio_reclaim)
   udp_open,       ctype_func(be_OT_udp_open)
   udp_send,       ctype_func(be_OT_udp_send)
   udp_poll,       func(be_OT_udp_poll)
@@ -96,6 +97,8 @@ module OT (scope: global) {
   srp_get_host_state, ctype_func(be_OT_srp_get_host_state)
   srp_get_server,   ctype_func(be_OT_srp_get_server)
   srp_set_lease_interval, ctype_func(be_OT_srp_set_lease_interval)
+  dns_resolve_service, ctype_func(be_OT_dns_resolve_service)
+  dns_poll_result,     func(be_OT_dns_poll_result)
 }
 @const_object_info_end */
 
diff --git a/tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino b/tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino
index db5ce7880..3f3ba09fb 100644
--- a/tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino
+++ b/tasmota/tasmota_xdrv_driver/xdrv_52_3_berry_thread.ino
@@ -36,12 +36,14 @@
 #include <openthread/message.h>
 #include <openthread/logging.h>
 #include <openthread/srp_client.h>
+#include <openthread/dns_client.h>
 
 // 3. ESP-IDF base includes (still available from framework)
 #include "esp_vfs_eventfd.h"
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "freertos/queue.h"
+#include "be_mapping.h"
 
 
 /*********************************************************************************************\
@@ -77,6 +79,16 @@ static struct {
   volatile int32_t state_pending_role = -1;   // -1 means "no pending event"
 } OT_State;
 
+// ---- DNS client state (polling pattern, like OT_State) ----
+static struct {
+  bool      pending = false;
+  volatile bool done = false;
+  otError   error;
+  uint16_t  port;
+  otIp6Address address;
+  char      hostname[256];
+} OT_DNS_State;
+
 // ---- Forward declarations ----
 // Use void* / uint32_t to avoid OT types in signatures (Arduino prototype generator
 // places prototypes before includes, so otInstance/otChangedFlags are not yet defined).
@@ -87,6 +99,7 @@ extern "C" void srp_client_callback(otError aError, const otSrpClientHostInfo *a
                                     const otSrpClientService *aServices,
                                     const otSrpClientService *aRemovedServices, void *aContext);
 static void srp_server_state_change(const otSockAddr *aServerSockAddr, void *aContext);
+static void dns_resolve_callback(otError aError, const otDnsServiceResponse *aResponse, void *aContext);
 
 // ---- Helper: lock and get OT instance ----
 // Returns OT instance as void* to avoid otInstance in function signature.
@@ -370,11 +383,6 @@ extern "C" int be_OT_netdata_services(bvm *vm) {
   be_return(vm);
 }
 
-extern "C" void be_OT_radio_reclaim(void) {
-  bt_radio_reclaim();
-}
-
-
 // ---- UDP receive callback (runs in OT task context) ----
 static void ot_udp_receive_callback(void *aContext, void *aMessage, const void *aMessageInfo) {
   otMessage *msg = (otMessage *)aMessage;
@@ -853,5 +861,109 @@ extern "C" const char* be_OT_srp_get_server(void) {
   ot_unlock();
   return result;
 }
+
+// ---- DNS client async callback (runs in OT task context) ----
+// Stores the result in OT_DNS_State for Berry to poll via OT.dns_poll_result().
+static void dns_resolve_callback(otError aError, const otDnsServiceResponse *aResponse, void *aContext) {
+  (void)aContext;
+  OT_DNS_State.error = aError;
+  OT_DNS_State.port = 0;
+  memset(&OT_DNS_State.address, 0, sizeof(OT_DNS_State.address));
+  OT_DNS_State.hostname[0] = '\0';
+
+  if (aError == OT_ERROR_NONE && aResponse != NULL) {
+    otDnsServiceInfo serviceInfo;
+    memset(&serviceInfo, 0, sizeof(serviceInfo));
+    serviceInfo.mHostNameBuffer = OT_DNS_State.hostname;
+    serviceInfo.mHostNameBufferSize = sizeof(OT_DNS_State.hostname);
+
+    if (otDnsServiceResponseGetServiceInfo(aResponse, &serviceInfo) == OT_ERROR_NONE) {
+      OT_DNS_State.port = serviceInfo.mPort;
+      memcpy(&OT_DNS_State.address, &serviceInfo.mHostAddress, sizeof(otIp6Address));
+    }
+  }
+  OT_DNS_State.done = true;
+}
+
+// ---- OT.dns_resolve_service(instance_label, service_name) -> bool ----
+// Start async DNS-SD resolution of a service instance (SRV + TXT + AAAA).
+// Returns true if the query was launched, false if busy or error.
+extern "C" bbool be_OT_dns_resolve_service(struct bvm *vm, const char *instance_label, const char *service_name) {
+  if (OT_DNS_State.pending) {
+    return bfalse;
+  }
+  otInstance *instance = (otInstance*)ot_lock_and_get();
+  if (!instance) {
+    be_raisef(vm, "ot_error", "OT: not initialized");
+    return bfalse;
+  }
+
+  OT_DNS_State.pending = true;
+  OT_DNS_State.done = false;
+
+  otError err = otDnsClientResolveServiceAndHostAddress(instance, instance_label, service_name,
+                                                         dns_resolve_callback, nullptr, nullptr);
+  ot_unlock();
+
+  if (err != OT_ERROR_NONE) {
+    OT_DNS_State.pending = false;
+    AddLog(LOG_LEVEL_DEBUG, PSTR("OT : dns_resolve_service failed: %d"), err);
+    return bfalse;
+  }
+  AddLog(LOG_LEVEL_DEBUG, PSTR("OT : dns_resolve_service started: %s.%s"), instance_label, service_name);
+  return btrue;
+}
+
+// ---- OT.dns_poll_result() -> map or nil ----
+// Poll for a completed DNS resolution. Returns nil if still pending,
+// or a map {error, port, address, hostname} when done.
+extern "C" int be_OT_dns_poll_result(bvm *vm) {
+  if (!OT_DNS_State.done) {
+    be_pushnil(vm);
+    be_return(vm);
+  }
+
+  be_newobject(vm, "map");
+
+  // Fix A2: validate that we actually got a usable address+port.
+  // otDnsClientResolveServiceAndHostAddress may resolve the SRV record but leave
+  // mHostAddress as :: (all zeros) when the AAAA record was not in the response.
+  // Treat that as NOT_FOUND so Berry's retry path fires instead of a CASE to ::.
+  otError effective_error = OT_DNS_State.error;
+  if (effective_error == OT_ERROR_NONE) {
+    bool addr_is_zero = true;
+    for (int i = 0; i < 16; i++) {
+      if (OT_DNS_State.address.mFields.m8[i] != 0) { addr_is_zero = false; break; }
+    }
+    if (addr_is_zero || OT_DNS_State.port == 0) {
+      AddLog(LOG_LEVEL_DEBUG, PSTR("OT : dns_poll_result: resolved but address/port invalid, treating as NOT_FOUND"));
+      effective_error = OT_ERROR_NOT_FOUND;
+    }
+  }
+
+  be_map_insert_int(vm, "error", (bint)effective_error);
+
+  if (effective_error == OT_ERROR_NONE) {
+    be_map_insert_int(vm, "port", (bint)OT_DNS_State.port);
+
+    // Fix A1: return address as a human-readable IPv6 string, not raw bytes.
+    // All other OT address consumers (e.g. be_OT_srp_get_server) already do this.
+    char ip_str[46];
+    otIp6AddressToString(&OT_DNS_State.address, ip_str, sizeof(ip_str));
+    be_map_insert_str(vm, "address", ip_str);
+
+    be_pushstring(vm, "hostname");
+    be_pushstring(vm, OT_DNS_State.hostname);
+    be_data_insert(vm, -3);
+    be_pop(vm, 2);
+  }
+
+  OT_DNS_State.pending = false;
+  OT_DNS_State.done = false;
+
+  be_pop(vm, 1);  // pop map ref, leave map instance
+  be_return(vm);
+}
+
 #endif  // USE_MATTER_THREAD
 #endif  // USE_BERRY
````

## Part 2 — New untracked files (full content)

These are brand-new files (the core client/initiator implementation).
They are gated behind `#if USE_MATTER_CLIENT` (never defined), except
`Matter_Operational_Discovery_Thread.be` which is gated `#if USE_MATTER_THREAD`.

### `lib/libesp32/berry_matter/src/embedded/Matter_Client.be`

````berry
#
# Matter_Client_Thread.be - Application-facing client API
#
# Thin orchestrator combining Phase 3 (IM initiator):
# takes an established operational session and sends IM requests.
#
# Copyright (C) 2026 Christian Baars
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

#@ solidify:Matter_Client,weak
#if USE_MATTER_CLIENT
class Matter_Client
  var device                # root device

  def init(device)
    self.device = device
  end

  #################################################################################
  # read_attribute - Read a remote attribute via an established operational session
  #
  # session: Matter_Session (operational, must already be established)
  # endpoint, cluster, attribute: int
  # callback: function(reportDataMessage) invoked when ReportData arrives
  #
  # Callback receives the parsed ReportDataMessage. The caller should extract
  # attribute values from it.
  #################################################################################
  def read_attribute(session, endpoint, cluster, attribute, callback)
    var responder = self.device.message_handler
    var msg = matter.IM_ReadRequest_Out(responder, session, endpoint, cluster, attribute, callback)
    self._push_and_send(responder, msg)
  end

  #################################################################################
  # invoke_command - Invoke a command on a remote node
  #
  # session: established operational session
  # endpoint, cluster, command: int
  # command_fields: TLV struct (command arguments)
  # callback: function(invokeResponseMessage)
  #################################################################################
  def invoke_command(session, endpoint, cluster, command, command_fields, callback)
    var responder = self.device.message_handler
    var msg = matter.IM_InvokeRequest_Out(responder, session, endpoint, cluster, command, command_fields, callback)
    self._push_and_send(responder, msg)
  end

  #################################################################################
  # write_attribute - Write a remote attribute
  #
  # session: established operational session
  # endpoint, cluster, attribute: int
  # write_data: TLV value to write
  # callback: function(writeResponseMessage)
  #################################################################################
  def write_attribute(session, endpoint, cluster, attribute, write_data, callback)
    var responder = self.device.message_handler
    var msg = matter.IM_WriteRequest_Out(responder, session, endpoint, cluster, attribute, write_data, callback)
    self._push_and_send(responder, msg)
  end

  #################################################################################
  def _push_and_send(responder, msg)
    responder.im.send_queue.push(msg)
    responder.im.send_enqueued(responder)
  end
end
matter.Client = Matter_Client
#endif //USE_MATTER_CLIENT
````

### `lib/libesp32/berry_matter/src/embedded/Matter_Commissioning_Initiator.be`

````berry
#
# Matter_Commissioning_Initiator.be - CASE Initiator
#
# Copyright (C) 2026 Christian Baars
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

#@ solidify:Matter_Commissioning_Initiator,weak
#if USE_MATTER_CLIENT
#################################################################################
# Matter_Commissioning_Initiator
#
# Initiates a CASE handshake to a peer whose operational address has been
# resolved via DNS-SD (Phase 1). Handles Sigma1 -> Sigma2 -> Sigma3 -> StatusReport.
#
# After successful handshake an operational session is created and stored
# in self.session.
#################################################################################
class Matter_Commissioning_Initiator
  var responder                       # reference to the message handler (sending packets)
  var device                          # root device object

  # Handshake state
  var state                           # 0=idle, 1=sigma1_sent, 2=sigma2_rcvd, 3=sigma3_sent, 4=done
  var error                           # error code

  # Callbacks
  var on_ready                        # function(session) called when CASE handshake succeeds
  var on_error                        # function() called when CASE handshake fails

  # Target peer
  var peer_ip
  var peer_port
  var peer_node_id                    # target's node id (bytes 8)

  # Fabric to use
  var fabric

  # Our generated values
  var initiatorRandom                 # bytes 32
  var initiator_session_id            # uint16
  var initiatorPriv                   # bytes 32
  var initiatorPub                    # bytes 65

  # Received from peer
  var responderRandom                 # bytes 32
  var responder_session_id            # uint16 (responderSessionId from Sigma2)
  var responderPub                    # bytes 65
  var encrypted2                      # bytes (from Sigma2)
  var resumptionID                    # bytes 16 (from TBEData2)

  # Derived crypto
  var shared_secret                   # ECDH shared secret

  # Stored messages for Transcript Hash
  var sigma1_raw                      # Msg1
  var sigma2_raw                      # Msg2
  var sigma3_raw                      # Msg3

  # Result
  var session                         # operational session after handshake

  #############################################################
  def init(responder)
    import crypto
    self.responder = responder
    self.device = responder.device
    self.state = 0
  end

  #############################################################
  # Start a CASE handshake to a discovered peer.
  #
  # fabric: the fabric to use (has NOC, device_id, etc.)
  # ip: peer IPv6 address string
  # port: peer UDP port
  # node_id: peer node id (bytes 8)
  #
  # Returns true if Sigma1 was sent successfully.
  #############################################################
  def start(fabric, ip, port, node_id)
    import crypto
    self.fabric = fabric
    self.peer_ip = ip
    self.peer_port = port

    # Fix B: admin_subject may arrive as an int (32-bit Berry value of a 64-bit Matter
    # node ID). Normalize to bytes(8) little-endian to match fabric_id / device_id
    # (Matter_Fabric.be lines 60-62 document both as LE). bytes.set(0, val, 4) packs
    # val in the low 4 bytes LE; the upper 4 bytes stay zero.
    if type(node_id) == 'int'
      var nid_bytes = bytes(8)
      nid_bytes.set(0, node_id, 4)
      node_id = nid_bytes
    end
    self.peer_node_id = node_id

    # Generate ephemeral key pair and random
    self.initiatorRandom = crypto.random(32)
    self.initiatorPriv = crypto.random(32)
    self.initiatorPub = crypto.EC_P256().public_key(self.initiatorPriv)

    # Generate a unique session id for our side
    self.initiator_session_id = self.device.sessions.gen_local_session_id()

    # Compute destinationId
    var destinationMessage = self.initiatorRandom + fabric.get_ca_pub() + fabric.fabric_id + node_id
    var key = fabric.get_ipk_group_key()
    var destinationId = crypto.HMAC_SHA256(key).update(destinationMessage).out()

    log(format("MTR: CASE initiator: destinationId=%s target=%s", destinationId.tohex(), node_id.tohex()), 3)


    # Build Sigma1 TLV
    var TLV = matter.TLV
    var s = TLV.Matter_TLV_struct()
    s.add_TLV(1, 0x10, self.initiatorRandom)
    s.add_TLV(2, 0x05, self.initiator_session_id)
    s.add_TLV(3, 0x10, destinationId)
    s.add_TLV(4, 0x10, self.initiatorPub)
    var s2 = s.add_struct(5)
    s2.add_TLV(1, 0x06, 500)
    s2.add_TLV(2, 0x06, 300)
    s2.add_TLV(3, 0x05, 4000)
    s2.add_TLV(4, 0x05, 18)
    s2.add_TLV(5, 0x05, 12)
    s2.add_TLV(6, 0x06, 0x01040100)
    s2.add_TLV(7, 0x05, 1)
    self.sigma1_raw = s.tlv2raw()

    # Build the unsecured frame
    var session = self.device.sessions.find_session_source_id_unsecure(self.fabric.get_device_id(), 90)
    session._ip = ip
    session._port = port
    session._message_handler = self.responder

    var resp = matter.Frame(self.responder)
    resp.remote_ip = ip
    resp.remote_port = port
    resp.session = session
    resp.flag_s = 1
    resp.source_node_id = self.fabric.get_device_id()
    resp.flag_dsiz = 0x01
    resp.dest_node_id_8 = node_id
    resp.local_session_id = 0
    resp.message_counter = session._counter_insecure_snd.next()
    resp.x_flag_i = 1
    resp.x_flag_r = 1
    resp.opcode = 0x30
    session._exchange_id += 1
    resp.exchange_id = session._exchange_id | 0x10000
    resp.protocol_id = 0

    resp.encode_frame(self.sigma1_raw)
    self.responder.send_response_frame(resp)

    self.state = 1
    log(format("MTR: CASE initiator: Sigma1 sent to [%s]:%i node_id=%s", ip, port, node_id.tohex()), 2)
    return true
  end

  #############################################################
  # Handle incoming Sigma2 (opcode 0x31) from responder.
  #
  # Parses Sigma2, verifies signature, builds and sends Sigma3.
  #############################################################
  def parse_Sigma2(msg)
    import crypto
    var sigma2 = matter.Sigma2().parse(msg.raw, msg.app_payload_idx)
    self.sigma2_raw = msg.raw[msg.app_payload_idx..]

    self.responderRandom = sigma2.responderRandom
    self.responder_session_id = sigma2.responderSessionId
    self.responderPub = sigma2.responderEphPubKey
    self.encrypted2 = sigma2.encrypted2

    log(format("MTR: CASE initiator: Sigma2 rcvd responder_session_id=%i", self.responder_session_id), 3)

    # Compute ECDH shared secret
    self.shared_secret = crypto.EC_P256().shared_key(self.initiatorPriv, self.responderPub)
    log("MTR: CASE initiator: shared_secret computed", 4)

    # TranscriptHash = Hash(Msg1)
    var TranscriptHash = crypto.SHA256().update(self.sigma1_raw).out()

    # Compute S2K
    var s2k = crypto.HKDF_SHA256().derive(self.shared_secret,
                                           self.fabric.get_ipk_group_key() + self.responderRandom + self.responderPub + TranscriptHash,
                                           bytes().fromstring("Sigma2"), 16)

    # Decrypt encrypted2
    var enc = self.encrypted2[0..-17]
    var tag = self.encrypted2[-16..]
    var aes = crypto.AES_CCM(s2k, bytes().fromstring("NCASE_Sigma2N"), bytes(), size(enc), 16)
    var TBEData2 = aes.decrypt(enc)
    var tbe_tag = aes.tag()

    if tbe_tag != tag
      log("MTR: CASE initiator: Sigma2 decrypt tag mismatch", 3)
      return false
    end

    # Parse TBEData2
    var tbe = matter.TLV.parse(TBEData2)
    var responderNOC = tbe.findsubval(1)
    var responderICAC = tbe.findsubval(2)
    var tbsSignature = tbe.findsubval(3)
    self.resumptionID = tbe.findsubval(4)

    # Verify TBS Data 2 signature
    var sigma2_tbs = matter.TLV.Matter_TLV_struct()
    sigma2_tbs.add_TLV(1, 0x11, responderNOC)
    if responderICAC != nil   sigma2_tbs.add_TLV(2, 0x11, responderICAC) end
    sigma2_tbs.add_TLV(3, 0x11, self.responderPub)
    sigma2_tbs.add_TLV(4, 0x11, self.initiatorPub)
    var sigma2_tbs_raw = sigma2_tbs.tlv2raw()

    var nocTLV = matter.TLV.parse(responderNOC)
    var responderPubKey = nocTLV.findsubval(9)

    var valid = crypto.EC_P256().ecdsa_verify_sha256(responderPubKey, sigma2_tbs_raw, tbsSignature)
    if !valid
      log("MTR: CASE initiator: Sigma2 signature verification FAILED", 3)
      return false
    end

    log("MTR: CASE initiator: Sigma2 verified, building Sigma3", 3)

    # TranscriptHash = Hash(Msg1 || Msg2)
    TranscriptHash = crypto.SHA256().update(self.sigma1_raw).update(self.sigma2_raw).out()

    # Compute S3K
    var s3k = crypto.HKDF_SHA256().derive(self.shared_secret,
                                           self.fabric.get_ipk_group_key() + TranscriptHash,
                                           bytes().fromstring("Sigma3"), 16)

    # Build TBS Data 3
    var sigma3_tbs = matter.TLV.Matter_TLV_struct()
    sigma3_tbs.add_TLV(1, 0x10, self.fabric.get_noc())
    if self.fabric.get_icac() != nil   sigma3_tbs.add_TLV(2, 0x10, self.fabric.get_icac()) end
    sigma3_tbs.add_TLV(3, 0x10, self.initiatorPub)
    sigma3_tbs.add_TLV(4, 0x10, self.responderPub)
    var sigma3_tbs_raw = sigma3_tbs.tlv2raw()

    # Sign with our operational key
    var sigma3_signature = crypto.EC_P256().ecdsa_sign_sha256(self.fabric.get_pk(), sigma3_tbs_raw)

    # Build TBE Data 3
    var sigma3_tbedata = matter.TLV.Matter_TLV_struct()
    sigma3_tbedata.add_TLV(1, 0x11, self.fabric.get_noc())
    if self.fabric.get_icac() != nil   sigma3_tbedata.add_TLV(2, 0x11, self.fabric.get_icac()) end
    sigma3_tbedata.add_TLV(3, 0x11, sigma3_signature)
    var sigma3_tbedata_raw = sigma3_tbedata.tlv2raw()

    # Encrypt TBEData3 with S3K
    var aes3 = crypto.AES_CCM(s3k, bytes().fromstring("NCASE_Sigma3N"), bytes(), size(sigma3_tbedata_raw), 16)
    var TBEData3Encrypted = aes3.encrypt(sigma3_tbedata_raw) + aes3.tag()

    # Build Sigma3 TLV
    var sigma3_s = matter.TLV.Matter_TLV_struct()
    sigma3_s.add_TLV(1, 0x10, TBEData3Encrypted)
    self.sigma3_raw = sigma3_s.tlv2raw()

    # Build response frame (opcode 0x32)
    var resp = msg.build_response(0x32, true)
    resp.protocol_id = 0
    var raw = resp.encode_frame(self.sigma3_raw)
    self.responder.send_response_frame(resp)

    self.state = 3
    log("MTR: CASE initiator: Sigma3 sent", 3)
    return true
  end

  #############################################################
  # Handle incoming Sigma2Resume (opcode 0x33).
  #############################################################
  def parse_Sigma2Resume(msg)
    log("MTR: CASE initiator: Sigma2Resume received (resumption not yet supported)", 3)
    return false
  end

  #############################################################
  # Handle incoming StatusReport (opcode 0x40).
  #
  # On SUCCESS (general_code=0x00, protocol_code=0x0000):
  #   derive session keys and create operational session.
  #############################################################
  def parse_StatusReport(msg)
    var status_raw = msg.raw[msg.app_payload_idx..]
    var general_code = status_raw.get(0, 2)
    var protocol_id = status_raw.get(2, 4)
    var protocol_code = status_raw.get(6, 4)

    log(format("MTR: CASE initiator: StatusReport general=0x%04X proto=0x%08X code=0x%08X",
               general_code, protocol_id, protocol_code), 3)

    if general_code == 0x00 && protocol_code == 0x0000
      import crypto
      # TranscriptHash = Hash(Msg1 || Msg2 || Msg3)
      var TranscriptHash = crypto.SHA256().update(self.sigma1_raw).update(self.sigma2_raw).update(self.sigma3_raw).out()

      # Derive session keys (initiator perspective)
      var session_keys = crypto.HKDF_SHA256().derive(self.shared_secret,
                                                      self.fabric.get_ipk_group_key() + TranscriptHash,
                                                      bytes().fromstring("SessionKeys"), 48)
      var i2r = session_keys[0..15]
      var r2i = session_keys[16..31]
      var ac = session_keys[32..47]
      var created = tasmota.rtc_utc()

      # Create operational session
      var new_session = self.device.sessions.create_session(self.initiator_session_id, self.responder_session_id)
      new_session._fabric = self.fabric
      new_session.peer_node_id = self.peer_node_id
      new_session._ip = self.peer_ip
      new_session._port = self.peer_port
      new_session._message_handler = self.responder
      new_session.set_keys(i2r, r2i, ac, created)
      new_session.set_mode_CASE()
      new_session.resumption_id = self.resumptionID
      new_session.shared_secret = self.shared_secret
      new_session._breadcrumb = 0
      new_session.counter_snd_next()
      new_session.set_persist(true)
      new_session.set_no_expiration()
      new_session.persist_to_fabric()
      new_session.save()

      self.session = new_session
      self.state = 4
      log(format("MTR: CASE initiator: Session established local=%i peer_node=%s",
                 new_session.local_session_id, self.peer_node_id.tohex()), 2)
      if self.on_ready   self.on_ready(new_session)   end
      return false      # return false to trigger standalone ack
    else
      log("MTR: CASE initiator: Handshake FAILED", 3)
      self.state = 0
      if self.on_error   self.on_error()   end
      return false
    end
  end

  #############################################################
  # Check if the initiator is waiting for a response.
  # Used by message router to decide whether to dispatch
  # incoming Sigma2/StatusReport to this initiator.
  #############################################################
  def is_pending()
    return self.state > 0 && self.state < 4
  end
end

matter.Commissioning_Initiator = Matter_Commissioning_Initiator

#else
class Matter_Commissioning_Initiator end
#endif //USE_MATTER_CLIENT
````

### `lib/libesp32/berry_matter/src/embedded/Matter_IM_Message_Client.be`

````berry
#
# Matter_IM_Message_Client.be - Outgoing IM request classes
#
# Copyright (C) 2026 Christian Baars
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

#@ solidify:Matter_IM_ReadRequest_Out,weak
#@ solidify:Matter_IM_InvokeRequest_Out,weak
#@ solidify:Matter_IM_WriteRequest_Out,weak
#if USE_MATTER_CLIENT
#################################################################################
# Matter_IM_ReadRequest_Out
#
# Outgoing ReadRequest (opcode 0x02) — waits for ReportData (0x05) response.
# Pushes itself to the IM send_queue; the caller enqueues it via:
#   message_handler.im.send_queue.push(msg)
#################################################################################
class Matter_IM_ReadRequest_Out : Matter_IM_Message
  var callback              # function(reportDataMessage) called on response
  var endpoint              # int
  var cluster               # int
  var attribute             # int

  #################################################################################
  def init(message_handler, session, endpoint, cluster, attribute, callback)
    super(self).init(nil, 0x02 #-Read-#, true)
    self.resp = matter.Frame.initiate_response(message_handler, session, 0x02 #-Read-#, true)
    var rr = matter.ReadRequestMessage()
    rr.attributes_requests = [matter.AttributePathIB()]
    rr.attributes_requests[0].endpoint = endpoint
    rr.attributes_requests[0].cluster = cluster
    rr.attributes_requests[0].attribute = attribute
    rr.fabric_filtered = false
    self.data = rr
    self.callback = callback
    self.endpoint = endpoint
    self.cluster = cluster
    self.attribute = attribute
  end
end
matter.IM_ReadRequest_Out = Matter_IM_ReadRequest_Out

#################################################################################
# Matter_IM_InvokeRequest_Out
#
# Outgoing InvokeRequest (opcode 0x08) — waits for InvokeResponse (0x09).
#################################################################################
class Matter_IM_InvokeRequest_Out : Matter_IM_Message
  var callback              # function(invokeResponseMessage) called on response
  var endpoint              # int
  var cluster               # int
  var command               # int
  var command_fields        # TLV struct (command arguments)

  #################################################################################
  def init(message_handler, session, endpoint, cluster, command, command_fields, callback)
    super(self).init(nil, 0x08 #-Invoke Request-#, true)
    self.resp = matter.Frame.initiate_response(message_handler, session, 0x08 #-Invoke Request-#, true)
    var ir = matter.InvokeRequestMessage()
    ir.suppress_response = false
    ir.invoke_requests = [matter.CommandDataIB()]
    ir.invoke_requests[0].command_path = matter.CommandPathIB()
    ir.invoke_requests[0].command_path.endpoint = endpoint
    ir.invoke_requests[0].command_path.cluster = cluster
    ir.invoke_requests[0].command_path.command = command
    ir.invoke_requests[0].command_fields = command_fields
    self.data = ir
    self.callback = callback
    self.endpoint = endpoint
    self.cluster = cluster
    self.command = command
    self.command_fields = command_fields
  end
end
matter.IM_InvokeRequest_Out = Matter_IM_InvokeRequest_Out

#################################################################################
# Matter_IM_WriteRequest_Out
#
# Outgoing WriteRequest (opcode 0x06) — waits for WriteResponse (0x07).
#################################################################################
class Matter_IM_WriteRequest_Out : Matter_IM_Message
  var callback              # function(writeResponseMessage) called on response
  var endpoint              # int
  var cluster               # int
  var attribute             # int
  var write_data            # TLV value to write

  #################################################################################
  def init(message_handler, session, endpoint, cluster, attribute, write_data, callback)
    super(self).init(nil, 0x06 #-Write Request-#, true)
    self.resp = matter.Frame.initiate_response(message_handler, session, 0x06 #-Write Request-#, true)
    var wr = matter.WriteRequestMessage()
    wr.suppress_response = false
    wr.write_requests = [matter.AttributeDataIB()]
    wr.write_requests[0].data_version = nil
    wr.write_requests[0].path = matter.AttributePathIB()
    wr.write_requests[0].path.endpoint = endpoint
    wr.write_requests[0].path.cluster = cluster
    wr.write_requests[0].path.attribute = attribute
    wr.write_requests[0].data = write_data
    self.data = wr
    self.callback = callback
    self.endpoint = endpoint
    self.cluster = cluster
    self.attribute = attribute
    self.write_data = write_data
  end
end
matter.IM_WriteRequest_Out = Matter_IM_WriteRequest_Out
#endif //USE_MATTER_CLIENT
````

### `lib/libesp32/berry_matter/src/embedded/Matter_Operational_Discovery_Thread.be`

````berry
#
# Matter_Operational_Discovery_Thread.be - DNS-SD operational discovery
#
# Copyright (C) 2026  Christian Baars
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

#@ solidify:Matter_Operational_Discovery,weak
#if USE_MATTER_THREAD
#################################################################################
# Matter_Operational_Discovery
#
# Resolves a Matter peer's operational address via DNS-SD over Thread.
# Instance name format: <compressed_fabric_id_hex>-<node_id_hex>._matter._tcp
#
# Uses the native OT DNS client (otDnsClientResolveServiceAndHostAddress)
# which queries the Border Router acting as DNS-SD discovery proxy.
#################################################################################
class Matter_Operational_Discovery
  var cache            # map: target_node_id -> {ip, port, ts}
  var pending          # target_node_id currently being resolved (nil if idle)

  def init()
    self.cache = {}
    self.pending = nil
  end

  #############################################################
  # Build the operational instance label:
  #   <fc_hex>-<nid_hex>._matter._tcp
  # fc is bytes(8), nid is int or bytes(8)
  #############################################################
  def _make_label(fc, nid)
    import string
    var fc_hex = string.tolower(fc.tohex())
    var nid_hex
    if type(nid) == 'int'
      nid_hex = string.tolower(format("%016X", nid))
    elif type(nid) == 'bytes'
      # node_id is stored little-endian (int64.tobytes); the Matter operational
      # DNS-SD instance name requires the 64-bit node id as big-endian hex, so
      # reverse before formatting (matches srp_announce_op_discovery()).
      nid_hex = string.tolower(nid.copy().reverse().tohex())
    else
      nid_hex = str(nid)
    end
    return fc_hex + "-" + nid_hex
  end

  #############################################################
  # Start async resolution of a peer's operational address.
  # Returns true if query was launched, false if busy.
  #############################################################
  def resolve(session, target_node_id)
    import OT
    self.pending = target_node_id
    var fc = session.get_fabric_compressed()
    if fc == nil   return false   end
    var label = self._make_label(fc, target_node_id)
    return OT.dns_resolve_service(label, "_matter._tcp")
  end

  #############################################################
  # Same as resolve() but takes a fabric object directly
  # instead of a session. Used when we don't have a live
  # session object but do know the fabric.
  #############################################################
  def resolve_by_fabric(fabric, target_node_id)
    import OT
    self.pending = target_node_id
    var fc = fabric.fabric_compressed
    if fc == nil   return false   end
    var label = self._make_label(fc, target_node_id)
    return OT.dns_resolve_service(label, "_matter._tcp")
  end

  #############################################################
  # Poll for a completed DNS resolution.
  # Returns nil if still pending, or a map {error, port, address, hostname}
  # when the result is available. On success (error == 0) the result
  # is cached for later retrieval via get_cached().
  #############################################################
  def poll()
    import OT
    var result = OT.dns_poll_result()
    if result != nil
      if result["error"] == 0
        self.cache[self.pending] = {"ip": result["address"], "port": result["port"], "ts": tasmota.millis()}
      end
      self.pending = nil   # Fix 2a: clear on both success AND failure so caller can act
    end
    return result
  end

  #############################################################
  # Return cached address for target_node_id if within ttl_min.
  #############################################################
  def get_cached(target_node_id, ttl_min)
    var entry = self.cache.find(target_node_id)
    if entry != nil && (tasmota.millis() - entry["ts"]) < (ttl_min * 60000)
      return entry
    end
    return nil
  end
end

matter.OperationalDiscovery = Matter_Operational_Discovery

#else
class Matter_Operational_Discovery end
#endif //USE_MATTER_THREAD
````
