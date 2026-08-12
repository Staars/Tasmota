/*
 * bearthread-core-config.h - OpenThread core configuration for BearThread
 *
 * Custom settings shared by ESP-IDF's generated FTD and MTD configurations.
 * The device role is selected by sdkconfig and must not be defined here.
 */

#ifndef BEARTHREAD_CORE_CONFIG_H_
#define BEARTHREAD_CORE_CONFIG_H_

/* ---- BearSSL crypto backend (CRYPTO_LIB_PLATFORM) ---- */
/* Uses bt_crypto_bearssl.cpp to provide otPlatCrypto*() callbacks via BearSSL.
 * These context sizes must be >= sizeof() of the BearSSL structs stored in the
 * opaque OT crypto context buffer. */
#define OPENTHREAD_CONFIG_CRYPTO_LIB                           2  /* OPENTHREAD_CONFIG_CRYPTO_LIB_PLATFORM */
#define OPENTHREAD_CONFIG_AES_CONTEXT_SIZE                   256
#define OPENTHREAD_CONFIG_HMAC_SHA256_CONTEXT_SIZE           512
#define OPENTHREAD_CONFIG_SHA256_CONTEXT_SIZE                128
#define OPENTHREAD_CONFIG_HKDF_CONTEXT_SIZE                  512
#define OPENTHREAD_CONFIG_ENABLE_BUILTIN_MBEDTLS               0
#define OPENTHREAD_CONFIG_ENABLE_BUILTIN_MBEDTLS_MANAGEMENT    0
#define OPENTHREAD_CONFIG_PLATFORM_KEY_REFERENCES_ENABLE       0

/* ---- Disable features we don't need ---- */
#define OPENTHREAD_CONFIG_BORDER_ROUTER_ENABLE                 0
#define OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE                0
#define OPENTHREAD_CONFIG_BACKBONE_ROUTER_ENABLE               0
#define OPENTHREAD_CONFIG_TCP_ENABLE                           0
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

/* ---- Enable features we need ---- */
/* SRP client is compiled into BearThread and driven via native OT API calls
 * (OT.srp_set_hostname, OT.srp_add_service, OT.srp_is_running, etc.) exposed
 * to Berry in be_OT_lib.c / xdrv_52_3_berry_thread.ino. Auto-start mode is
 * ON by default (matching esp-matter/chip SDK) so OT selects the SRP server
 * from Thread Network Data automatically — no manual server discovery needed.
 * ECDSA is used by the OT SRP client for SIG(0) signing — BearSSL implementation
 * in bt_crypto_bearssl.cpp provides otPlatCryptoEcdsa*. */
#define OPENTHREAD_CONFIG_THREAD_VERSION                       OT_THREAD_VERSION_1_4
#define OPENTHREAD_CONFIG_SRP_CLIENT_AUTO_START_API_ENABLE     1
#define OPENTHREAD_CONFIG_SRP_CLIENT_AUTO_START_DEFAULT_MODE   1
#define OPENTHREAD_CONFIG_ECDSA_ENABLE                         1
#define OPENTHREAD_CONFIG_COAP_API_ENABLE                      0
#define OPENTHREAD_CONFIG_IP6_SLAAC_ENABLE                     1
#define OPENTHREAD_CONFIG_TMF_NETDATA_SERVICE_ENABLE           1
#define OPENTHREAD_CONFIG_PLATFORM_NETIF_ENABLE                0   /* We don't use esp_netif */
#define OPENTHREAD_CONFIG_PLATFORM_UDP_ENABLE                  0   /* We use OT's internal UDP */

/* ---- Platform integration ---- */
#define OPENTHREAD_CONFIG_PLATFORM_ASSERT_MANAGEMENT           1
#define OPENTHREAD_CONFIG_HEAP_EXTERNAL_ENABLE                 1
#define OPENTHREAD_CONFIG_PLATFORM_USEC_TIMER_ENABLE           1
#define OPENTHREAD_CONFIG_MAX_STATECHANGE_HANDLERS             3

/* ---- Logging ---- */
#define OPENTHREAD_CONFIG_LOG_OUTPUT OPENTHREAD_CONFIG_LOG_OUTPUT_PLATFORM_DEFINED

/* ---- Resource sizing ---- */
#define OPENTHREAD_CONFIG_6LOWPAN_REASSEMBLY_TIMEOUT          10

/* ---- Parent search ---- */
#define OPENTHREAD_CONFIG_PARENT_SEARCH_ENABLE                 1

/* ---- Delay-aware queue ---- */
#define OPENTHREAD_CONFIG_DELAY_AWARE_QUEUE_MANAGEMENT_MARK_ECN_INTERVAL 1000

#endif /* BEARTHREAD_CORE_CONFIG_H_ */
