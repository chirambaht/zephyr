/*
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(esp32_mesh, CONFIG_WIFI_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/wifi_mgmt.h>

#include "esp_mesh.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_wifi.h"

/*
 * Parse a hex mesh ID string of the form "AA:BB:CC:DD:EE:FF" into a 6-byte
 * array. Returns 0 on success, -EINVAL on malformed input.
 */
static int parse_mesh_id(const char *str, uint8_t out[6])
{
	unsigned int b[6];
	int n = sscanf(str, "%x:%x:%x:%x:%x:%x",
		       &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
	if (n != 6) {
		return -EINVAL;
	}
	for (int i = 0; i < 6; i++) {
		out[i] = (uint8_t)b[i];
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/*  State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	bool started;
	bool is_root;
	bool tods_reachable;
	int  layer;
} mesh_state;

/* ------------------------------------------------------------------ */
/*  Mesh event handler — called by esp_mesh_event_dispatch below       */
/* ------------------------------------------------------------------ */

static void mesh_event_handler(void *arg, esp_event_base_t base,
				int32_t event_id, void *event_data)
{
	switch (event_id) {
	case MESH_EVENT_STARTED:
		mesh_state.started = true;
		mesh_state.layer = esp_mesh_get_layer();
		LOG_INF("Mesh started (layer %d)", mesh_state.layer);
		break;

	case MESH_EVENT_STOPPED:
		mesh_state.started = false;
		mesh_state.is_root = false;
		mesh_state.tods_reachable = false;
		LOG_INF("Mesh stopped");
		break;

	case MESH_EVENT_PARENT_CONNECTED: {
		mesh_event_connected_t *conn = event_data;

		mesh_state.layer = conn->self_layer;
		mesh_state.is_root = esp_mesh_is_root();
		LOG_INF("Parent connected (layer %d, root=%d)",
			mesh_state.layer, mesh_state.is_root);
		if (mesh_state.is_root) {
			LOG_INF("This node is the mesh ROOT");
		}
		break;
	}

	case MESH_EVENT_PARENT_DISCONNECTED: {
		mesh_event_disconnected_t *disc = event_data;

		LOG_WRN("Parent disconnected (reason %d)", disc->reason);
		mesh_state.is_root = false;
		mesh_state.tods_reachable = false;
		break;
	}

	case MESH_EVENT_NO_PARENT_FOUND: {
		mesh_event_no_parent_found_t *npf = event_data;

		LOG_WRN("No parent found after %d scan(s)", npf->scan_times);
		break;
	}

	case MESH_EVENT_LAYER_CHANGE: {
		mesh_event_layer_change_t *lc = event_data;

		mesh_state.layer = lc->new_layer;
		LOG_INF("Layer changed to %d", mesh_state.layer);
		break;
	}

	case MESH_EVENT_CHILD_CONNECTED: {
		mesh_event_child_connected_t *cc = event_data;

		LOG_INF("Child connected: " MACSTR, MAC2STR(cc->mac));
		break;
	}

	case MESH_EVENT_CHILD_DISCONNECTED: {
		mesh_event_child_disconnected_t *cd = event_data;

		LOG_INF("Child disconnected: " MACSTR, MAC2STR(cd->mac));
		break;
	}

	case MESH_EVENT_TODS_STATE: {
		mesh_event_toDS_state_t state =
			*((mesh_event_toDS_state_t *)event_data);

		mesh_state.tods_reachable = (state == MESH_TODS_REACHABLE);
		LOG_INF("External IP network %sreachable",
			mesh_state.tods_reachable ? "" : "un");
		break;
	}

	case MESH_EVENT_ROUTING_TABLE_ADD: {
		mesh_event_routing_table_change_t *rt = event_data;

		LOG_DBG("Routing table +%d (total %d)",
			rt->rt_size_change, rt->rt_size_new);
		break;
	}

	case MESH_EVENT_ROUTING_TABLE_REMOVE: {
		mesh_event_routing_table_change_t *rt = event_data;

		LOG_DBG("Routing table -%d (total %d)",
			rt->rt_size_change, rt->rt_size_new);
		break;
	}

	case MESH_EVENT_NETWORK_STATE: {
		mesh_event_network_state_t *ns = event_data;

		LOG_INF("Network %s a root",
			ns->is_rootless ? "has NO" : "has");
		break;
	}

	case MESH_EVENT_ROOT_ADDRESS: {
		mesh_event_root_address_t *ra = event_data;

		LOG_INF("Root MAC: " MACSTR, MAC2STR(ra->addr));
		break;
	}

	case MESH_EVENT_VOTE_STARTED:
		LOG_INF("Root vote started");
		break;

	case MESH_EVENT_VOTE_STOPPED:
		LOG_INF("Root vote finished");
		break;

	case MESH_EVENT_CHANNEL_SWITCH: {
		mesh_event_channel_switch_t *cs = event_data;

		LOG_INF("Channel switched to %d", cs->channel);
		break;
	}

	case MESH_EVENT_FIND_NETWORK: {
		mesh_event_find_network_t *fn = event_data;

		LOG_INF("Found mesh network on channel %d", fn->channel);
		break;
	}

	default:
		LOG_DBG("Mesh event %d", (int)event_id);
		break;
	}
}

/*
 * Strong definition of the weak hook declared in wifi_compat_stubs.c.
 * esp_event_post() in wifi_compat_stubs.c calls this for every event so that
 * mesh events (MESH_EVENT base) are handled here without needing the full
 * IDF esp_event machinery.
 */
void esp_mesh_event_dispatch(esp_event_base_t event_base,
			     int32_t event_id,
			     void *event_data,
			     size_t event_data_size)
{
	ARG_UNUSED(event_data_size);

	if (event_base == MESH_EVENT) {
		LOG_DBG("MESH_EVENT %d", (int)event_id);
		mesh_event_handler(NULL, event_base, event_id, event_data);
	}
}

/* ------------------------------------------------------------------ */
/*  RX thread — blocks on esp_mesh_recv()                              */
/* ------------------------------------------------------------------ */

static K_THREAD_STACK_DEFINE(mesh_rx_stack,
			     CONFIG_ESP32_MESH_RX_THREAD_STACK_SIZE);
static struct k_thread mesh_rx_thread_data;

static void mesh_rx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static uint8_t rx_buf[MESH_MPS];

	LOG_DBG("Mesh RX thread started");

	while (true) {
		mesh_addr_t from;
		mesh_data_t data = {
			.data = rx_buf,
			.size = sizeof(rx_buf),
		};
		int flag = 0;

		esp_err_t err = esp_mesh_recv(&from, &data, -1,
					      &flag, NULL, 0);
		if (err != ESP_OK) {
			if (err != ESP_ERR_MESH_TIMEOUT) {
				LOG_WRN("esp_mesh_recv error 0x%x", err);
			}
			continue;
		}

		LOG_DBG("RX %u bytes from " MACSTR " (flag=0x%x)",
			data.size, MAC2STR(from.addr), flag);
	}
}

/* ------------------------------------------------------------------ */
/*  Root DS receive thread — only runs when this node is root          */
/* ------------------------------------------------------------------ */

static K_THREAD_STACK_DEFINE(mesh_rx_ds_stack,
			     CONFIG_ESP32_MESH_RX_THREAD_STACK_SIZE);
static struct k_thread mesh_rx_ds_thread_data;

static void mesh_rx_ds_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static uint8_t rx_buf[MESH_MPS];

	LOG_DBG("Mesh root DS-RX thread waiting for root role");

	/* recv_toDS is only valid on the root node — poll until elected */
	while (!mesh_state.is_root) {
		k_sleep(K_MSEC(500));
	}

	LOG_INF("DS-RX thread active (this node is root)");

	while (true) {
		/* If we lost root role, pause until re-elected */
		if (!mesh_state.is_root) {
			k_sleep(K_MSEC(500));
			continue;
		}

		mesh_addr_t from;
		mesh_addr_t to;
		mesh_data_t data = {
			.data = rx_buf,
			.size = sizeof(rx_buf),
		};
		int flag = 0;

		esp_err_t err = esp_mesh_recv_toDS(&from, &to, &data,
						   -1, &flag,
						   NULL, 0);
		if (err != ESP_OK) {
			if (err != ESP_ERR_MESH_TIMEOUT &&
			    err != ESP_ERR_MESH_RECV_RELEASE) {
				LOG_WRN("esp_mesh_recv_toDS error 0x%x", err);
			}
			continue;
		}

		LOG_DBG("DS RX %u bytes from " MACSTR " to %u.%u.%u.%u",
			data.size, MAC2STR(from.addr),
			(to.mip.ip4.addr) & 0xff,
			(to.mip.ip4.addr >> 8) & 0xff,
			(to.mip.ip4.addr >> 16) & 0xff,
			(to.mip.ip4.addr >> 24) & 0xff);
	}
}

/* ------------------------------------------------------------------ */
/*  Initialisation — called via SYS_INIT after WiFi driver is up       */
/* ------------------------------------------------------------------ */

static int esp_mesh_drv_init(void)
{
	esp_err_t err;

	LOG_INF("Initialising ESP-WIFI-MESH");

	err = esp_mesh_init();
	if (err != ESP_OK) {
		LOG_ERR("esp_mesh_init failed: 0x%x", err);
		return -EIO;
	}

	/* Disable WiFi power save — required for reliable mesh operation */
	esp_wifi_set_ps(WIFI_PS_NONE);

	/* Build mesh_cfg_t from Kconfig */
	mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();

	cfg.channel = CONFIG_ESP32_MESH_CHANNEL;
	cfg.allow_channel_switch = (CONFIG_ESP32_MESH_CHANNEL == 0);

	/* Mesh network ID */
#if defined(CONFIG_ESP32_MESH_ID_FROM_MAC)
	err = esp_read_mac(cfg.mesh_id.addr, ESP_MAC_WIFI_STA);
	if (err != ESP_OK) {
		LOG_ERR("Failed to read STA MAC for mesh ID: 0x%x", err);
		return -EIO;
	}
	LOG_INF("Mesh ID from MAC: " MACSTR, MAC2STR(cfg.mesh_id.addr));
#else
	if (parse_mesh_id(CONFIG_ESP32_MESH_ID, cfg.mesh_id.addr) != 0) {
		LOG_ERR("Invalid CONFIG_ESP32_MESH_ID: %s",
			CONFIG_ESP32_MESH_ID);
		return -EINVAL;
	}
	LOG_INF("Mesh ID: " MACSTR, MAC2STR(cfg.mesh_id.addr));
#endif

	/* External router */
	if (sizeof(CONFIG_ESP32_MESH_ROUTER_SSID) > 1) {
		cfg.router.ssid_len =
			MIN(strlen(CONFIG_ESP32_MESH_ROUTER_SSID),
			    sizeof(cfg.router.ssid));
		memcpy(cfg.router.ssid, CONFIG_ESP32_MESH_ROUTER_SSID,
		       cfg.router.ssid_len);
		memcpy(cfg.router.password, CONFIG_ESP32_MESH_ROUTER_PASS,
		       MIN(strlen(CONFIG_ESP32_MESH_ROUTER_PASS),
			   sizeof(cfg.router.password)));
		LOG_INF("Router SSID: %s", CONFIG_ESP32_MESH_ROUTER_SSID);
	} else {
		LOG_WRN("No router SSID configured — node will not connect to internet");
	}

	/* Mesh SoftAP */
	memcpy(cfg.mesh_ap.password, CONFIG_ESP32_MESH_AP_PASS,
	       MIN(strlen(CONFIG_ESP32_MESH_AP_PASS),
		   sizeof(cfg.mesh_ap.password)));
	cfg.mesh_ap.max_connection = CONFIG_ESP32_MESH_MAX_CONNECTION;

	err = esp_mesh_set_config(&cfg);
	if (err != ESP_OK) {
		LOG_ERR("esp_mesh_set_config failed: 0x%x", err);
		return -EIO;
	}

	err = esp_mesh_set_max_layer(CONFIG_ESP32_MESH_MAX_LAYER);
	if (err != ESP_OK) {
		LOG_WRN("esp_mesh_set_max_layer failed: 0x%x", err);
	}

	err = esp_mesh_start();
	if (err != ESP_OK) {
		LOG_ERR("esp_mesh_start failed: 0x%x", err);
		return -EIO;
	}

	LOG_INF("ESP-WIFI-MESH started");

	/* Spawn RX thread for intra-mesh packets */
	k_thread_create(&mesh_rx_thread_data, mesh_rx_stack,
			K_THREAD_STACK_SIZEOF(mesh_rx_stack),
			mesh_rx_thread, NULL, NULL, NULL,
			CONFIG_ESP32_MESH_RX_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&mesh_rx_thread_data, "esp_mesh_rx");

	/* Spawn root DS receive thread */
	k_thread_create(&mesh_rx_ds_thread_data, mesh_rx_ds_stack,
			K_THREAD_STACK_SIZEOF(mesh_rx_ds_stack),
			mesh_rx_ds_thread, NULL, NULL, NULL,
			CONFIG_ESP32_MESH_RX_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&mesh_rx_ds_thread_data, "esp_mesh_rx_ds");

	return 0;
}

/*
 * Run after APPLICATION init so WiFi driver (CONFIG_WIFI_INIT_PRIORITY) is
 * already up and esp_wifi_init() + esp_wifi_start() have been called.
 */
SYS_INIT(esp_mesh_drv_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ------------------------------------------------------------------ */
/*  Public helpers (usable by sample application)                      */
/* ------------------------------------------------------------------ */

bool esp32_mesh_is_root(void)
{
	return mesh_state.is_root;
}

bool esp32_mesh_tods_reachable(void)
{
	return mesh_state.tods_reachable;
}

int esp32_mesh_get_layer(void)
{
	return mesh_state.layer;
}

/*
 * Convenience wrapper: send raw bytes toward the root (or to a specific node).
 * Pass NULL for `to` to send toward the root.
 */
int esp32_mesh_send(const uint8_t *dest_mac, const uint8_t *data, size_t len)
{
	if (!mesh_state.started || mesh_state.layer < 1) {
		return -ENETDOWN;
	}
	if (len > MESH_MPS) {
		return -EMSGSIZE;
	}

	mesh_addr_t to;
	int flag = MESH_DATA_P2P;

	if (dest_mac == NULL) {
		/* Send toward root using TODS flag */
		flag = MESH_DATA_TODS;
		memset(&to, 0, sizeof(to));
	} else {
		memcpy(to.addr, dest_mac, 6);
	}

	mesh_data_t tx = {
		.data = (uint8_t *)data,
		.size = (uint16_t)len,
		.proto = MESH_PROTO_BIN,
		.tos   = MESH_TOS_P2P,
	};

	esp_err_t err = esp_mesh_send(&to, &tx, flag, NULL, 0);

	if (err != ESP_OK) {
		LOG_WRN("esp_mesh_send error 0x%x", err);
		return -EIO;
	}
	return 0;
}
