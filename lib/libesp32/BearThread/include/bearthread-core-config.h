/*
 * bearthread-core-config.h - OpenThread core configuration for BearThread
 *
 * Self-contained config for MTD (Minimal Thread Device) with BearSSL crypto.
 * Replaces both openthread-core-esp32x-mtd-config.h and openthread-core-bearssl-config.h
 */

#ifndef BEARTHREAD_CORE_CONFIG_H_
#define BEARTHREAD_CORE_CONFIG_H_

/* ---- Device type: MTD only ---- */
#define OPENTHREAD_MTD 1

/* ---- Platform info ---- */
#define OPENTHREAD_CONFIG_PLATFORM_INFO "BearThread-ESP32"
#define PACKAGE_NAME "BearThread"

/* ---- BearSSL crypto backend ---- */
/* Use platform-provided crypto library (BearSSL) instead of mbedTLS */
#define OPENTHREAD_CONFIG_CRYPTO_LIB                           2  /* OPENTHREAD_CONFIG_CRYPTO_LIB_PLATFORM */
#define OPENTHREAD_CONFIG_AES_CONTEXT_SIZE                   256
#define OPENTHREAD_CONFIG_HMAC_SHA256_CONTEXT_SIZE           512
#define OPENTHREAD_CONFIG_HKDF_CONTEXT_SIZE                  520
#define OPENTHREAD_CONFIG_SHA256_CONTEXT_SIZE                 256
#define OPENTHREAD_CONFIG_ENABLE_BUILTIN_MBEDTLS               0
#define OPENTHREAD_CONFIG_ENABLE_BUILTIN_MBEDTLS_MANAGEMENT    0

/* ---- Disable features we don't need ---- */
#define OPENTHREAD_CONFIG_COMMISSIONER_ENABLE                  0
#define OPENTHREAD_CONFIG_JOINER_ENABLE                        0
#define OPENTHREAD_CONFIG_DIAG_ENABLE                          0
#define OPENTHREAD_CONFIG_BORDER_ROUTER_ENABLE                 0
#define OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE                0
#define OPENTHREAD_CONFIG_BORDER_AGENT_ENABLE                  0
#define OPENTHREAD_CONFIG_BACKBONE_ROUTER_ENABLE               0
#define OPENTHREAD_CONFIG_TCP_ENABLE                            0
#define OPENTHREAD_CONFIG_TIME_SYNC_ENABLE                     0
#define OPENTHREAD_CONFIG_RADIO_STATS_ENABLE                   0
#define OPENTHREAD_CONFIG_MAC_CSL_RECEIVER_ENABLE              0
#define OPENTHREAD_CONFIG_MAC_FILTER_ENABLE                    0
#define OPENTHREAD_CONFIG_PING_SENDER_ENABLE                   0
#define OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE              0
#define OPENTHREAD_CONFIG_NCP_HDLC_ENABLE                      0
#define OPENTHREAD_CONFIG_NAT64_TRANSLATOR_ENABLE              0
#define OPENTHREAD_CONFIG_NAT64_BORDER_ROUTING_ENABLE          0
#define OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE                  0
#define OPENTHREAD_CONFIG_SRP_SERVER_ENABLE                    0
#define OPENTHREAD_CONFIG_TMF_NETDIAG_CLIENT_ENABLE            0
#define OPENTHREAD_CONFIG_LINK_METRICS_INITIATOR_ENABLE        0
#define OPENTHREAD_CONFIG_LINK_METRICS_SUBJECT_ENABLE          0
#define OPENTHREAD_CONFIG_MLE_LINK_METRICS_SUBJECT_ENABLE      0
#define OPENTHREAD_CONFIG_MLE_LINK_METRICS_INITIATOR_ENABLE    0
#define OPENTHREAD_CONFIG_DUA_ENABLE                           0
#define OPENTHREAD_CONFIG_MLR_ENABLE                           0
#define OPENTHREAD_CONFIG_SNTP_CLIENT_ENABLE                   0
#define OPENTHREAD_CONFIG_DHCP6_CLIENT_ENABLE                  0
#define OPENTHREAD_CONFIG_DHCP6_SERVER_ENABLE                  0
#define OPENTHREAD_CONFIG_DNS_DSO_ENABLE                       0
#define OPENTHREAD_CONFIG_COAP_SECURE_API_ENABLE               0
#define OPENTHREAD_CONFIG_JAM_DETECTION_ENABLE                 0
#define OPENTHREAD_CONFIG_CHANNEL_MONITOR_ENABLE               0
#define OPENTHREAD_CONFIG_CHANNEL_MANAGER_ENABLE               0
#define OPENTHREAD_CONFIG_HISTORY_TRACKER_ENABLE               0
#define OPENTHREAD_CONFIG_MESH_DIAG_ENABLE                     0
#define OPENTHREAD_CONFIG_DATASET_UPDATER_ENABLE               0
#define OPENTHREAD_CONFIG_BLE_TCAT_ENABLE                      0
#define OPENTHREAD_CONFIG_MULTI_RADIO_ENABLE                   0
#define OPENTHREAD_CONFIG_RADIO_LINK_TREL_ENABLE               0

/* ---- Enable features we need ---- */
#define OPENTHREAD_CONFIG_SRP_CLIENT_ENABLE                    1
#define OPENTHREAD_CONFIG_SRP_CLIENT_BUFFERS_MAX_SERVICES      5
#define OPENTHREAD_CONFIG_ECDSA_ENABLE                         1
#define OPENTHREAD_CONFIG_DNS_CLIENT_ENABLE                    0
#define OPENTHREAD_CONFIG_COAP_API_ENABLE                      1
#define OPENTHREAD_CONFIG_IP6_SLAAC_ENABLE                     1
#define OPENTHREAD_CONFIG_PLATFORM_NETIF_ENABLE                0   /* We don't use esp_netif */
#define OPENTHREAD_CONFIG_PLATFORM_UDP_ENABLE                  0   /* We use OT's internal UDP */

/* ---- Platform integration ---- */
#define OPENTHREAD_CONFIG_PLATFORM_ASSERT_MANAGEMENT           1
#define OPENTHREAD_CONFIG_HEAP_EXTERNAL_ENABLE                 1
#define OPENTHREAD_CONFIG_PLATFORM_USEC_TIMER_ENABLE           1
#define OPENTHREAD_CONFIG_MAX_STATECHANGE_HANDLERS             3

/* ---- Logging ---- */
#define OPENTHREAD_CONFIG_LOG_OUTPUT OPENTHREAD_CONFIG_LOG_OUTPUT_PLATFORM_DEFINED
#define OPENTHREAD_CONFIG_LOG_LEVEL                            OT_LOG_LEVEL_INFO
#define OPENTHREAD_CONFIG_LOG_LEVEL_DYNAMIC_ENABLE             1

/* ---- Resource sizing ---- */
#define OPENTHREAD_CONFIG_NUM_MESSAGE_BUFFERS                  44
#define OPENTHREAD_CONFIG_DTLS_MAX_CONTENT_LEN               768
#define OPENTHREAD_CONFIG_MAC_MAX_CSMA_BACKOFFS_DIRECT         4
#define OPENTHREAD_CONFIG_TMF_ADDRESS_QUERY_TIMEOUT           3
#define OPENTHREAD_CONFIG_TMF_ADDRESS_QUERY_INITIAL_RETRY_DELAY 15
#define OPENTHREAD_CONFIG_TMF_ADDRESS_QUERY_MAX_RETRY_DELAY   28800

/* ---- Parent search ---- */
#define OPENTHREAD_CONFIG_PARENT_SEARCH_ENABLE                 1
#define OPENTHREAD_CONFIG_PARENT_SEARCH_CHECK_INTERVAL       600  /* 10 minutes */
#define OPENTHREAD_CONFIG_PARENT_SEARCH_BACKOFF_INTERVAL      36000 /* 10 hours */
#define OPENTHREAD_CONFIG_PARENT_SEARCH_RSS_THRESHOLD        -65

/* ---- Delay-aware queue ---- */
#define OPENTHREAD_CONFIG_DELAY_AWARE_QUEUE_MANAGEMENT_MARK_ECN_INTERVAL 1000

#endif /* BEARTHREAD_CORE_CONFIG_H_ */
