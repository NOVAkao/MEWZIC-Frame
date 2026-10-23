/*
 * Copyright (C) 2023 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions of binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "a2dp_sink_demo.c"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "config.h"
#include "display.h"
#include "nvs.h"

// *****************************************************************************
// AVRCP Controller + Cover Art Client
// *****************************************************************************

#ifndef ENABLE_AVRCP_COVER_ART
#define ENABLE_AVRCP_COVER_ART 1
#endif

#ifndef COVER_ART_DEMO
#define COVER_ART_DEMO 1
#endif

#undef HAVE_BTSTACK_STDIN

static btstack_packet_callback_registration_t hci_event_callback_registration;

static uint8_t  sdp_avdtp_sink_service_buffer[150];
static uint8_t  sdp_avrcp_target_service_buffer[150];
static uint8_t  sdp_avrcp_controller_service_buffer[200];
static uint8_t  device_id_sdp_service_buffer[100];

static uint8_t media_sbc_codec_capabilities[] = {
    0xFF,
    0xFF,
    2, 53
};

static bd_addr_t device_addr = {0};
static const char *device_addr_string = "00:00:00:00:00:00";
static char a2dp_sink_title[128] = "";
static char a2dp_sink_artist[128] = "";
static int volume_percentage = 0;
static avrcp_battery_status_t battery_status = AVRCP_BATTERY_STATUS_WARNING;
static uint8_t a2dp_sink_volume = 64;

static bool address_is_zero(const bd_addr_t address){
    for (unsigned i = 0; i < sizeof(bd_addr_t); ++i) {
        if (address[i] != 0) return false;
    }
    return true;
}

static void save_peer_address(const bd_addr_t address){
    nvs_handle_t handle;
    if (address_is_zero(address) || nvs_open(BT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, BT_NVS_KEY_ADDR, address, sizeof(bd_addr_t)) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
    memcpy(device_addr, address, sizeof(bd_addr_t));
}

#ifdef ENABLE_AVRCP_COVER_ART
static char a2dp_sink_demo_image_handle[8];
static avrcp_cover_art_client_t a2dp_sink_demo_cover_art_client;
static bool a2dp_sink_demo_cover_art_client_connected;
static uint16_t a2dp_sink_demo_cover_art_cid;
static uint8_t a2dp_sink_demo_ertm_buffer[2000];
static l2cap_ertm_config_t a2dp_sink_demo_ertm_config = {
        1,  // ertm mandatory
        2,  // max transmit
        2000,
        12000,
        512,    // l2cap ertm mtu
        2,
        2,
        1,      // 16-bit FCS
};
static bool a2dp_sink_cover_art_download_active;
static uint32_t a2dp_sink_cover_art_file_size;
static bool a2dp_sink_cover_art_handle_available;
#ifdef COVER_ART_DEMO
static bool a2dp_sink_track_changed;
static uint8_t *cover_art_jpeg;
#define COVER_ART_JPEG_CAPACITY MAX_COVER_ART_SIZE
static btstack_timer_source_t cover_art_timer;
#endif
static btstack_timer_source_t metadata_timer;
static uint8_t metadata_refresh_attempts;
#endif

typedef struct {
    uint8_t  reconfigure;
    uint8_t  num_channels;
    uint16_t sampling_frequency;
    uint8_t  block_length;
    uint8_t  subbands;
    uint8_t  min_bitpool_value;
    uint8_t  max_bitpool_value;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} media_codec_configuration_sbc_t;

static int media_processing_init(media_codec_configuration_sbc_t *configuration) {
    UNUSED(configuration);
    return 0;
}

static void media_processing_start(void) {}
static void media_processing_pause(void) {}
static void media_processing_close(void) {}

typedef enum {
    STREAM_STATE_CLOSED,
    STREAM_STATE_OPEN,
    STREAM_STATE_PLAYING,
    STREAM_STATE_PAUSED,
} stream_state_t;

typedef struct {
    uint8_t  a2dp_local_seid;
    uint8_t  media_sbc_codec_configuration[4];
} a2dp_sink_demo_stream_endpoint_t;
static a2dp_sink_demo_stream_endpoint_t a2dp_sink_demo_stream_endpoint;

typedef struct {
    bd_addr_t addr;
    uint16_t  a2dp_cid;
    uint8_t   a2dp_local_seid;
    stream_state_t stream_state;
    media_codec_configuration_sbc_t sbc_configuration;
} a2dp_sink_demo_a2dp_connection_t;
static a2dp_sink_demo_a2dp_connection_t a2dp_sink_demo_a2dp_connection;

typedef struct {
    bd_addr_t addr;
    uint16_t  avrcp_cid;
    bool playing;
    uint16_t notifications_supported_by_target;
} a2dp_sink_demo_avrcp_connection_t;
static a2dp_sink_demo_avrcp_connection_t a2dp_sink_demo_avrcp_connection;
static bool a2dp_sink_avrcp_connected;
static bool a2dp_sink_avrcp_connect_pending;
static bool a2dp_sink_playback_started;

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t * packet, uint16_t event_size);
static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t *packet, uint16_t size);
static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void metadata_refresh_timer_handler(btstack_timer_source_t *timer) {
    UNUSED(timer);
    if (!a2dp_sink_avrcp_connected) return;

    uint8_t status = avrcp_controller_get_now_playing_info(a2dp_sink_demo_avrcp_connection.avrcp_cid);
    printf("AVRCP Controller: metadata refresh status 0x%02x (attempt %u)\n",
           status, (unsigned)(metadata_refresh_attempts + 1));

    // Keep retrying regardless of playing state - metadata may arrive late
    // and we might connect when phone is already playing
    if (metadata_refresh_attempts++ < 7) {
        btstack_run_loop_set_timer(timer, 1000);
        btstack_run_loop_add_timer(timer);
    }
}

static void schedule_metadata_refresh(uint32_t delay_ms) {
    metadata_refresh_attempts = 0;
    btstack_run_loop_remove_timer(&metadata_timer);
    btstack_run_loop_set_timer_handler(&metadata_timer, &metadata_refresh_timer_handler);
    btstack_run_loop_set_timer(&metadata_timer, delay_ms);
    btstack_run_loop_add_timer(&metadata_timer);
}

static int setup_demo(void){

#ifdef COVER_ART_DEMO
    if (!cover_art_jpeg) cover_art_jpeg = (uint8_t *)malloc(COVER_ART_JPEG_CAPACITY);
    if (!cover_art_jpeg) printf("Cover Art: JPEG buffer allocation failed\n");
#endif

    // init protocols
    l2cap_init();
    sdp_init();
#ifdef ENABLE_BLE
    sm_init();
#endif
#ifdef ENABLE_AVRCP_COVER_ART
    goep_client_init();
    avrcp_cover_art_client_init();
#endif

    // Init AVRCP profiles
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();

    // Configure AVRCP Controller + Target
    avrcp_register_packet_handler(&avrcp_packet_handler);
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);

    // Configure SDP
    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    uint16_t controller_supported_features = 1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER;
#ifdef AVRCP_BROWSING_ENABLED
    controller_supported_features |= 1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_BROWSING;
#endif
#ifdef ENABLE_AVRCP_COVER_ART
    controller_supported_features |= 1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_COVER_ART_GET_LINKED_THUMBNAIL;
#endif
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, sdp_create_service_record_handle(),
                                       controller_supported_features, NULL, NULL);
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    uint16_t target_supported_features = 1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER;
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer,
                                   sdp_create_service_record_handle(), target_supported_features, NULL, NULL);
    sdp_register_service(sdp_avrcp_target_service_buffer);

    memset(device_id_sdp_service_buffer, 0, sizeof(device_id_sdp_service_buffer));
    device_id_create_sdp_record(device_id_sdp_service_buffer,
                                sdp_create_service_record_handle(), DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH, BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    sdp_register_service(device_id_sdp_service_buffer);

    gap_set_local_name(BT_DEVICE_NAME);
    gap_discoverable_control(1);
    gap_set_bondable_mode(1);
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    gap_set_security_level(LEVEL_1);
    gap_set_class_of_device(0x240408);
    gap_set_default_link_policy_settings( LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE );
    gap_set_allow_role_switch(true);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    if (!btstack_audio_sink_get_instance()){
        printf("No audio playback.\n");
    } else {
        printf("Audio playback supported.\n");
    }
    return 0;
}

static void handle_l2cap_media_data_packet(uint8_t seid, uint8_t *packet, uint16_t size){
    UNUSED(seid);
    UNUSED(packet);
    UNUSED(size);
}

static int read_sbc_header(uint8_t * packet, int size, int * offset, avdtp_sbc_codec_header_t * sbc_header){
    int sbc_header_len = 12;
    int pos = *offset;

    if (size - pos < sbc_header_len){
        printf("Not enough data to read SBC header, expected %d, received %d\n", sbc_header_len, size-pos);
        return 0;
    }

    sbc_header->fragmentation = get_bit16(packet[pos], 7);
    sbc_header->starting_packet = get_bit16(packet[pos], 6);
    sbc_header->last_packet = get_bit16(packet[pos], 5);
    sbc_header->num_frames = packet[pos] & 0x0f;
    pos++;
    *offset = pos;
    return 1;
}

static int read_media_data_header(uint8_t *packet, int size, int *offset, avdtp_media_packet_header_t *media_header){
    int media_header_len = 12;
    int pos = *offset;

    if (size - pos < media_header_len){
        printf("Not enough data to read media packet header, expected %d, received %d\n", media_header_len, size-pos);
        return 0;
    }

    media_header->version = packet[pos] & 0x03;
    media_header->padding = get_bit16(packet[pos],2);
    media_header->extension = get_bit16(packet[pos],3);
    media_header->csrc_count = (packet[pos] >> 4) & 0x0F;
    pos++;

    media_header->marker = get_bit16(packet[pos],0);
    media_header->payload_type  = (packet[pos] >> 1) & 0x7F;
    pos++;

    media_header->sequence_number = big_endian_read_16(packet, pos);
    pos+=2;

    media_header->timestamp = big_endian_read_32(packet, pos);
    pos+=4;

    media_header->synchronization_source = big_endian_read_32(packet, pos);
    pos+=4;
    *offset = pos;
    return 1;
}

static void dump_sbc_configuration(media_codec_configuration_sbc_t * configuration){
    printf("    - num_channels: %d\n", configuration->num_channels);
    printf("    - sampling_frequency: %d\n", configuration->sampling_frequency);
    printf("    - channel_mode: %d\n", configuration->channel_mode);
    printf("    - block_length: %d\n", configuration->block_length);
    printf("    - subbands: %d\n", configuration->subbands);
    printf("    - allocation_method: %d\n", configuration->allocation_method);
    printf("    - bitpool_value [%d, %d] \n", configuration->min_bitpool_value, configuration->max_bitpool_value);
    printf("\n");
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t event = hci_event_packet_get_type(packet);
    if (event == BTSTACK_EVENT_STATE && btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
        printf("Bluetooth ready; waiting for ACL connection before AVRCP\n");
    }
    if (event == HCI_EVENT_PIN_CODE_REQUEST) {
        bd_addr_t address;
        printf("Pin code request - using '0000'\n");
        hci_event_pin_code_request_get_bd_addr(packet, address);
        gap_pin_code_response(address, "0000");
    }
    if (event == HCI_EVENT_CONNECTION_COMPLETE &&
        hci_event_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
        bd_addr_t address;
        hci_event_connection_complete_get_bd_addr(packet, address);
        save_peer_address(address);
        printf("ACL: connected to %s\n", bd_addr_to_str(address));
        if (!a2dp_sink_avrcp_connected && !a2dp_sink_avrcp_connect_pending) {
            a2dp_sink_avrcp_connect_pending = true;
            uint8_t status = avrcp_connect(address, &a2dp_sink_demo_avrcp_connection.avrcp_cid);
            printf("AVRCP: connect after ACL -> 0x%02x\n", status);
            if (status != ERROR_CODE_SUCCESS) {
                a2dp_sink_avrcp_connect_pending = false;
                a2dp_sink_demo_avrcp_connection.avrcp_cid = 0;
            }
        }
    }
}

#ifdef ENABLE_AVRCP_COVER_ART
static void a2dp_sink_demo_cover_art_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    uint8_t status;
    uint16_t cid;
    switch (packet_type){
        case BIP_DATA_PACKET:
            if (a2dp_sink_cover_art_download_active){
#ifdef COVER_ART_DEMO
                if (cover_art_jpeg && a2dp_sink_cover_art_file_size + size < COVER_ART_JPEG_CAPACITY){
                    memcpy(&cover_art_jpeg[a2dp_sink_cover_art_file_size], packet, size);
                }
#endif
                a2dp_sink_cover_art_file_size += size;
            }
            break;
        case HCI_EVENT_PACKET:
            switch (hci_event_packet_get_type(packet)){
                case HCI_EVENT_AVRCP_META:
                    switch (hci_event_avrcp_meta_get_subevent_code(packet)){
                        case AVRCP_SUBEVENT_COVER_ART_CONNECTION_ESTABLISHED:
                            status = avrcp_subevent_cover_art_connection_established_get_status(packet);
                            cid = avrcp_subevent_cover_art_connection_established_get_cover_art_cid(packet);
                            if (status == ERROR_CODE_SUCCESS){
                                printf("Cover Art       : connection established, cover art cid 0x%02x\n", cid);
                                a2dp_sink_demo_cover_art_client_connected = true;
                                if (a2dp_sink_cover_art_handle_available && a2dp_sink_demo_cover_art_cid != 0) {
                                    a2dp_sink_cover_art_download_active = true;
                                    a2dp_sink_cover_art_file_size = 0;
                                    uint8_t request_status = avrcp_cover_art_client_get_linked_thumbnail(
                                        a2dp_sink_demo_cover_art_cid, a2dp_sink_demo_image_handle);
                                    printf("AVRCP Controller: deferred cover art request status 0x%02x\n", request_status);
                                }
                            } else {
                                printf("Cover Art       : connection failed, status 0x%02x\n", status);
                                a2dp_sink_demo_cover_art_cid = 0;
                            }
                            break;
                        case AVRCP_SUBEVENT_COVER_ART_OPERATION_COMPLETE:
                            if (a2dp_sink_cover_art_download_active){
                                a2dp_sink_cover_art_download_active = false;
                                a2dp_sink_cover_art_handle_available = false;
#ifdef COVER_ART_DEMO
                                a2dp_sink_track_changed = false;
                                display_set_cover_art_jpeg(cover_art_jpeg, a2dp_sink_cover_art_file_size);
#endif
                                printf("Cover Art: download completed (%" PRIu32 " bytes)\n", a2dp_sink_cover_art_file_size);
                            }
                            break;
                        case AVRCP_SUBEVENT_COVER_ART_CONNECTION_RELEASED:
                            a2dp_sink_demo_cover_art_client_connected = false;
                            a2dp_sink_demo_cover_art_cid = 0;
                            a2dp_sink_cover_art_download_active = false;
                            a2dp_sink_cover_art_file_size = 0;
                            printf("Cover Art       : connection released 0x%02x\n",
                                   avrcp_subevent_cover_art_connection_released_get_cover_art_cid(packet));
                            break;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static uint8_t a2dp_sink_demo_cover_art_connect(void) {
    return avrcp_cover_art_client_connect(&a2dp_sink_demo_cover_art_client, a2dp_sink_demo_cover_art_packet_handler,
                                            device_addr, a2dp_sink_demo_ertm_buffer,
                                            sizeof(a2dp_sink_demo_ertm_buffer), &a2dp_sink_demo_ertm_config,
                                            &a2dp_sink_demo_cover_art_cid);
}
#endif

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint16_t local_cid;
    uint8_t  status;
    bd_addr_t address;

    a2dp_sink_demo_avrcp_connection_t * connection = &a2dp_sink_demo_avrcp_connection;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    switch (packet[2]){
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: {
            local_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            status = avrcp_subevent_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("AVRCP: Connection failed, status 0x%02x\n", status);
                a2dp_sink_avrcp_connected = false;
                a2dp_sink_avrcp_connect_pending = false;
                connection->avrcp_cid = 0;
                return;
            }

            connection->avrcp_cid = local_cid;
            a2dp_sink_avrcp_connected = true;
            a2dp_sink_avrcp_connect_pending = false;
            a2dp_sink_playback_started = false;
            connection->playing = false;
            a2dp_sink_title[0] = '\0';
            a2dp_sink_artist[0] = '\0';
#ifdef COVER_ART_DEMO
            a2dp_sink_track_changed = true;
            a2dp_sink_cover_art_handle_available = false;
            display_set_cover_art_jpeg(NULL, 0);
#endif
            avrcp_subevent_connection_established_get_bd_addr(packet, address);
            save_peer_address(address);
            printf("AVRCP: Connected to %s, cid 0x%02x\n", bd_addr_to_str(address), connection->avrcp_cid);

            avrcp_target_support_event(connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_target_support_event(connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            avrcp_target_battery_status_changed(connection->avrcp_cid, battery_status);

            // query supported events:
            avrcp_controller_get_supported_events(connection->avrcp_cid);
            schedule_metadata_refresh(300);
            return;
        }

        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            printf("AVRCP: Channel released: cid 0x%02x\n", avrcp_subevent_connection_released_get_avrcp_cid(packet));
            connection->avrcp_cid = 0;
            a2dp_sink_avrcp_connected = false;
            a2dp_sink_avrcp_connect_pending = false;
            a2dp_sink_playback_started = false;
            connection->notifications_supported_by_target = 0;
            return;
        default:
            break;
    }
}

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    uint8_t avrcp_subevent_value[256];
    uint8_t play_status;
    uint8_t event_id;
    uint8_t status;

    a2dp_sink_demo_avrcp_connection_t * avrcp_connection = &a2dp_sink_demo_avrcp_connection;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (avrcp_connection->avrcp_cid == 0) return;

    memset(avrcp_subevent_value, 0, sizeof(avrcp_subevent_value));
    switch (packet[2]){
        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID:
            avrcp_connection->notifications_supported_by_target |= (1 << avrcp_subevent_get_capability_event_id_get_event_id(packet));
            break;
        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID_DONE:

            printf("AVRCP Controller: supported notifications by target:\n");
            for (event_id = (uint8_t) AVRCP_NOTIFICATION_EVENT_FIRST_INDEX; event_id < (uint8_t) AVRCP_NOTIFICATION_EVENT_LAST_INDEX; event_id++){
                printf("   - [%s] %s\n",
                    (avrcp_connection->notifications_supported_by_target & (1 << event_id)) != 0 ? "X" : " ",
                    avrcp_notification2str((avrcp_notification_event_id_t)event_id));
            }
            printf("\n\n");

            // Enable notifications
            avrcp_controller_enable_notification(avrcp_connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(avrcp_connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_controller_enable_notification(avrcp_connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);

#ifdef ENABLE_AVRCP_COVER_ART
            avrcp_controller_enable_notification(a2dp_sink_demo_avrcp_connection.avrcp_cid, AVRCP_NOTIFICATION_EVENT_UIDS_CHANGED);
            status = a2dp_sink_demo_cover_art_connect();
            printf("Cover Art: connect -> status %02x\n", status);
#endif
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_STATE:
            event_id = (avrcp_notification_event_id_t)avrcp_subevent_notification_state_get_event_id(packet);
            printf("AVRCP Controller: %s notification registered\n", avrcp_notification2str((avrcp_notification_event_id_t)event_id));
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_POS_CHANGED:
            printf("AVRCP Controller: Playback position changed, position %d ms\n", (unsigned int) avrcp_subevent_notification_playback_pos_changed_get_playback_position_ms(packet));
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            printf("AVRCP Controller: Playback status changed %s\n", avrcp_play_status2str(avrcp_subevent_notification_playback_status_changed_get_play_status(packet)));
            play_status = avrcp_subevent_notification_playback_status_changed_get_play_status(packet);
            avrcp_controller_enable_notification(avrcp_connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            switch (play_status){
                case AVRCP_PLAYBACK_STATUS_PLAYING:
                    avrcp_connection->playing = true;
                    a2dp_sink_playback_started = true;
                    // Fetch immediately AND schedule a delayed retry
                    avrcp_controller_get_now_playing_info(avrcp_connection->avrcp_cid);
                    schedule_metadata_refresh(1000);
                    break;
                default:
                    avrcp_connection->playing = false;
                    break;
            }
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_NOW_PLAYING_CONTENT_CHANGED:
            printf("AVRCP Controller: Playing content changed\n");
            avrcp_controller_enable_notification(avrcp_connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            #ifdef COVER_ART_DEMO
                a2dp_sink_track_changed = true;
            #endif
            // Always fetch metadata on content change, regardless of playing state
            schedule_metadata_refresh(300);
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            printf("AVRCP Controller: Track changed\n");
            avrcp_controller_enable_notification(avrcp_connection->avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
#ifdef COVER_ART_DEMO
            a2dp_sink_track_changed = true;
            display_set_cover_art_jpeg(NULL, 0);
#endif
            // Always fetch metadata on track change, regardless of playing state
            schedule_metadata_refresh(300);
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_AVAILABLE_PLAYERS_CHANGED:
            printf("AVRCP Controller: Available Players Changed\n");
            break;

        case AVRCP_SUBEVENT_SHUFFLE_AND_REPEAT_MODE:{
            uint8_t shuffle_mode = avrcp_subevent_shuffle_and_repeat_mode_get_shuffle_mode(packet);
            uint8_t repeat_mode  = avrcp_subevent_shuffle_and_repeat_mode_get_repeat_mode(packet);
            printf("AVRCP Controller: %s, %s\n", avrcp_shuffle2str(shuffle_mode), avrcp_repeat2str(repeat_mode));
            break;
        }
        case AVRCP_SUBEVENT_NOW_PLAYING_TRACK_INFO:
            printf("AVRCP Controller: Track %d\n", avrcp_subevent_now_playing_track_info_get_track(packet));
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_TOTAL_TRACKS_INFO:
            printf("AVRCP Controller: Total Tracks %d\n", avrcp_subevent_now_playing_total_tracks_info_get_total_tracks(packet));
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO: {
            uint8_t value_len = avrcp_subevent_now_playing_title_info_get_value_len(packet);
            if (value_len > 0){
                memset(avrcp_subevent_value, 0, sizeof(avrcp_subevent_value));
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_title_info_get_value(packet), value_len);
                printf("AVRCP Controller: Title '%s'\n", avrcp_subevent_value);
                strncpy(a2dp_sink_title, (char *)avrcp_subevent_value, sizeof(a2dp_sink_title) - 1);
                a2dp_sink_title[sizeof(a2dp_sink_title) - 1] = '\0';
            }
            break;
        }

        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO: {
            uint8_t value_len = avrcp_subevent_now_playing_artist_info_get_value_len(packet);
            if (value_len > 0){
                memset(avrcp_subevent_value, 0, sizeof(avrcp_subevent_value));
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_artist_info_get_value(packet), value_len);
                printf("AVRCP Controller: Artist '%s'\n", avrcp_subevent_value);
                strncpy(a2dp_sink_artist, (char *)avrcp_subevent_value, sizeof(a2dp_sink_artist) - 1);
                a2dp_sink_artist[sizeof(a2dp_sink_artist) - 1] = '\0';
            }
            break;
        }

        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            if (avrcp_subevent_now_playing_album_info_get_value_len(packet) > 0){
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_album_info_get_value(packet), avrcp_subevent_now_playing_album_info_get_value_len(packet));
                printf("AVRCP Controller: Album %s\n", avrcp_subevent_value);
                UNUSED(avrcp_subevent_value);
            }
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_GENRE_INFO:
            if (avrcp_subevent_now_playing_genre_info_get_value_len(packet) > 0){
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_genre_info_get_value(packet), avrcp_subevent_now_playing_genre_info_get_value_len(packet));
                printf("AVRCP Controller: Genre %s\n", avrcp_subevent_value);
            }
            break;

        case AVRCP_SUBEVENT_PLAY_STATUS:
            printf("AVRCP Controller: Song length %" PRIu32" ms, Song position %" PRIu32" ms, Play status %s\n",
                avrcp_subevent_play_status_get_song_length(packet),
                avrcp_subevent_play_status_get_song_position(packet),
                avrcp_play_status2str(avrcp_subevent_play_status_get_play_status(packet)));
            break;

        case AVRCP_SUBEVENT_OPERATION_COMPLETE:
            printf("AVRCP Controller: %s complete, status 0x%02x\n",
                   avrcp_operation2str(avrcp_subevent_operation_complete_get_operation_id(packet)),
                   avrcp_subevent_operation_complete_get_status(packet));
            break;

        case AVRCP_SUBEVENT_OPERATION_START:
            printf("AVRCP Controller: %s start\n", avrcp_operation2str(avrcp_subevent_operation_start_get_operation_id(packet)));
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_EVENT_TRACK_REACHED_END:
            printf("AVRCP Controller: Track reached end\n");
            break;

        case AVRCP_SUBEVENT_PLAYER_APPLICATION_VALUE_RESPONSE:
            printf("AVRCP Controller: Set Player App Value %s\n", avrcp_ctype2str(avrcp_subevent_player_application_value_response_get_command_type(packet)));
            break;

#ifdef ENABLE_AVRCP_COVER_ART
        case AVRCP_SUBEVENT_NOTIFICATION_EVENT_UIDS_CHANGED:
            if (a2dp_sink_demo_cover_art_client_connected){
                printf("AVRCP Controller: UIDs changed -> disconnect cover art client\n");
                avrcp_cover_art_client_disconnect(a2dp_sink_demo_cover_art_cid);
            }
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_COVER_ART_INFO: {
            uint8_t value_len = avrcp_subevent_now_playing_cover_art_info_get_value_len(packet);
            printf("AVRCP Controller: Cover Art Info received, length %d\n", value_len);
            if (value_len > 0 && value_len <= 7){
                memset(a2dp_sink_demo_image_handle, 0, sizeof(a2dp_sink_demo_image_handle));
                memcpy(a2dp_sink_demo_image_handle, avrcp_subevent_now_playing_cover_art_info_get_value(packet), value_len);
                a2dp_sink_cover_art_handle_available = true;
                printf("AVRCP Controller: Cover Art handle '%s'\n", a2dp_sink_demo_image_handle);
#ifdef COVER_ART_DEMO
                if (a2dp_sink_demo_cover_art_client_connected && a2dp_sink_demo_cover_art_cid != 0) {
                    a2dp_sink_cover_art_download_active = true;
                    a2dp_sink_cover_art_file_size = 0;
                    uint8_t status = avrcp_cover_art_client_get_linked_thumbnail(a2dp_sink_demo_cover_art_cid, a2dp_sink_demo_image_handle);
                    printf("AVRCP Controller: Cover Art thumbnail request status 0x%02x\n", status);
                } else {
                    printf("AVRCP Controller: Cover Art client not connected, cannot download\n");
                }
#endif
            } else {
                printf("AVRCP Controller: Cover Art Info invalid length (%d), skipping\n", value_len);
            }
            break;
        }
#endif

        default:
            break;
    }
}

static void avrcp_volume_changed(uint8_t volume){
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        audio->set_volume(volume);
    }
}

static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    uint8_t volume;
    char const * button_state;
    avrcp_operation_id_t operation_id;

    switch (packet[2]){
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
            volume_percentage = volume * 100 / 127;
            printf("AVRCP Target    : Volume set to %d%% (%d)\n", volume_percentage, volume);
            avrcp_volume_changed(volume);
            break;

        case AVRCP_SUBEVENT_OPERATION:
            operation_id = (avrcp_operation_id_t) avrcp_subevent_operation_get_operation_id(packet);
            button_state = avrcp_subevent_operation_get_button_pressed(packet) > 0 ? "PRESS" : "RELEASE";
            switch (operation_id){
                case AVRCP_OPERATION_ID_VOLUME_UP:
                    printf("AVRCP Target    : VOLUME UP (%s)\n", button_state);
                    break;
                case AVRCP_OPERATION_ID_VOLUME_DOWN:
                    printf("AVRCP Target    : VOLUME DOWN (%s)\n", button_state);
                    break;
                default:
                    return;
            }
            break;
        default:
            printf("AVRCP Target    : Event 0x%02x is not parsed\n", packet[2]);
            break;
    }
}

static void a2dp_sink_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);
    uint8_t status;
    uint8_t allocation_method;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    a2dp_sink_demo_a2dp_connection_t * a2dp_conn = &a2dp_sink_demo_a2dp_connection;

    switch (packet[2]){
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
            printf("A2DP  Sink      : Received non SBC codec - not implemented\n");
            break;
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:{
            printf("A2DP  Sink      : Received SBC codec configuration\n");
            a2dp_conn->sbc_configuration.reconfigure = a2dp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
            a2dp_conn->sbc_configuration.num_channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            a2dp_conn->sbc_configuration.sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            a2dp_conn->sbc_configuration.block_length = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            a2dp_conn->sbc_configuration.subbands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            a2dp_conn->sbc_configuration.min_bitpool_value = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            a2dp_conn->sbc_configuration.max_bitpool_value = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);

            allocation_method = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
            a2dp_conn->sbc_configuration.allocation_method = (btstack_sbc_allocation_method_t)(allocation_method - 1);

            switch (a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet)){
                case AVDTP_CHANNEL_MODE_JOINT_STEREO:
                    a2dp_conn->sbc_configuration.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
                    break;
                case AVDTP_CHANNEL_MODE_STEREO:
                    a2dp_conn->sbc_configuration.channel_mode = SBC_CHANNEL_MODE_STEREO;
                    break;
                case AVDTP_CHANNEL_MODE_DUAL_CHANNEL:
                    a2dp_conn->sbc_configuration.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
                    break;
                case AVDTP_CHANNEL_MODE_MONO:
                    a2dp_conn->sbc_configuration.channel_mode = SBC_CHANNEL_MODE_MONO;
                    break;
                default:
                    btstack_assert(false);
                    break;
            }
            dump_sbc_configuration(&a2dp_conn->sbc_configuration);
            break;
        }

        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            status = a2dp_subevent_stream_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                printf("A2DP  Sink      : Streaming connection failed, status 0x%02x\n", status);
                break;
            }

            a2dp_subevent_stream_established_get_bd_addr(packet, a2dp_conn->addr);
            a2dp_conn->a2dp_cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            a2dp_conn->a2dp_local_seid = a2dp_subevent_stream_established_get_local_seid(packet);
            a2dp_conn->stream_state = STREAM_STATE_OPEN;

            printf("A2DP  Sink      : Streaming connection is established, address %s, cid 0x%02x, local seid %d\n",
                   bd_addr_to_str(a2dp_conn->addr), a2dp_conn->a2dp_cid, a2dp_conn->a2dp_local_seid);
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            printf("A2DP  Sink      : Stream started\n");
            a2dp_conn->stream_state = STREAM_STATE_PLAYING;
            if (a2dp_conn->sbc_configuration.reconfigure){
                media_processing_close();
            }
            media_processing_init(&a2dp_conn->sbc_configuration);
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            printf("A2DP  Sink      : Stream paused\n");
            a2dp_conn->stream_state = STREAM_STATE_PAUSED;
            media_processing_pause();
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            printf("A2DP  Sink      : Stream released\n");
            a2dp_conn->stream_state = STREAM_STATE_CLOSED;
            media_processing_close();
            break;

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            printf("A2DP  Sink      : Signaling connection released\n");
            a2dp_conn->a2dp_cid = 0;
            media_processing_close();
            break;

        default:
            break;
    }
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    UNUSED(argc);
    (void)argv;

    setup_demo();

    printf("Starting BTstack ...\n");
    hci_power_control(HCI_POWER_ON);
    return 0;
}

// Public API for the ESP32 application
int a2dp_sink_setup(void){
    return setup_demo();
}

int a2dp_sink_start(const bd_addr_t address){
    if (address) memcpy(device_addr, address, sizeof(bd_addr_t));
    uint8_t status = hci_power_control(HCI_POWER_ON);
    if (status != ERROR_CODE_SUCCESS) return status;
    btstack_run_loop_execute();
    return ERROR_CODE_SUCCESS;
}

bool a2dp_sink_is_connected(void){
    return a2dp_sink_avrcp_connected;
}

uint16_t a2dp_sink_get_cid(void){
    return a2dp_sink_demo_avrcp_connection.avrcp_cid;
}

uint8_t a2dp_sink_volume_up(void){
    if (!a2dp_sink_is_connected()) return ERROR_CODE_COMMAND_DISALLOWED;
    return avrcp_controller_volume_up(a2dp_sink_get_cid());
}

uint8_t a2dp_sink_volume_down(void){
    if (!a2dp_sink_is_connected()) return ERROR_CODE_COMMAND_DISALLOWED;
    return avrcp_controller_volume_down(a2dp_sink_get_cid());
}

uint8_t a2dp_sink_play_pause(void){
    if (!a2dp_sink_is_connected()) return ERROR_CODE_COMMAND_DISALLOWED;
    return a2dp_sink_demo_avrcp_connection.playing ? avrcp_controller_pause(a2dp_sink_get_cid()) : avrcp_controller_play(a2dp_sink_get_cid());
}

uint8_t a2dp_sink_forward(void){
    if (!a2dp_sink_is_connected()) return ERROR_CODE_COMMAND_DISALLOWED;
    return avrcp_controller_forward(a2dp_sink_get_cid());
}

uint8_t a2dp_sink_backward(void){
    if (!a2dp_sink_is_connected()) return ERROR_CODE_COMMAND_DISALLOWED;
    return avrcp_controller_backward(a2dp_sink_get_cid());
}

const char * a2dp_sink_get_title(unsigned max_len){
    UNUSED(max_len);
    return a2dp_sink_title;
}

const char * a2dp_sink_get_artist(unsigned max_len){
    UNUSED(max_len);
    return a2dp_sink_artist;
}

bool a2dp_sink_get_playing(void){
    return a2dp_sink_demo_avrcp_connection.playing;
}

bool a2dp_sink_has_started_playing(void){
    return a2dp_sink_playback_started;
}

bool a2dp_sink_has_cover_art(void){
    return display_has_cover_art();
}

unsigned a2dp_sink_get_cover_art_width(void){
    return display_get_cover_art_width();
}

unsigned a2dp_sink_get_cover_art_height(void){
    return display_get_cover_art_height();
}

unsigned a2dp_sink_get_cover_art_data(uint8_t *buffer, unsigned bufsize){
    return display_get_cover_art_data(buffer, bufsize);
}