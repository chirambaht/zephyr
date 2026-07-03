/*
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-WIFI-MESH basic sample for Zephyr RTOS on ESP32-S3.
 *
 * Every node in the mesh runs identical firmware. The mesh stack
 * automatically elects a root node (best RSSI to the external router).
 * The root connects to the router and becomes the internet gateway for
 * all other nodes.
 *
 * Application behaviour:
 *  - Logs mesh topology events (parent/child connect, layer changes, etc.)
 *  - Leaf / intermediate nodes periodically send a ping payload toward
 *    the root using esp_mesh_send() with MESH_DATA_P2P.
 *  - The root node logs received payloads.
 *
 * Build:
 *   west build -b esp32s3_devkitm \
 *       samples/net/esp_wifi_mesh \
 *       -- -DCONF_FILE="prj.conf;boards/esp32s3_devkitm.conf"
 *
 * Flash and monitor:
 *   west flash && west espressif monitor
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "esp_mesh.h"
#include "esp_event.h"
#include "esp_mac.h"

LOG_MODULE_REGISTER(mesh_sample, LOG_LEVEL_INF);

/* Provided by esp_mesh_drv.c */
extern bool esp32_mesh_is_root(void);
extern bool esp32_mesh_tods_reachable(void);
extern int  esp32_mesh_get_layer(void);
extern int  esp32_mesh_send(const uint8_t *dest_mac,
			    const uint8_t *data, size_t len);

/* ------------------------------------------------------------------ */
/*  Application receive thread                                          */
/* ------------------------------------------------------------------ */

#define APP_RX_STACK_SIZE 2048
#define APP_RX_PRIORITY   6

static K_THREAD_STACK_DEFINE(app_rx_stack, APP_RX_STACK_SIZE);
static struct k_thread app_rx_thread_data;

static void app_rx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static uint8_t rx_buf[MESH_MPS];

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
			continue;
		}

		/* Null-terminate so we can log it as a string */
		size_t safe_len = MIN(data.size, sizeof(rx_buf) - 1);
		rx_buf[safe_len] = '\0';

		LOG_INF("RX from " MACSTR " (layer %d): %s",
			MAC2STR(from.addr),
			esp32_mesh_get_layer(),
			(char *)rx_buf);
	}
}

/* ------------------------------------------------------------------ */
/*  Periodic transmit: non-root nodes send toward root every 5 s       */
/* ------------------------------------------------------------------ */

static void tx_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (!esp32_mesh_is_root()) {
		char msg[64];
		uint8_t mac[6];

		esp_read_mac(mac, ESP_MAC_WIFI_STA);
		snprintf(msg, sizeof(msg),
			 "ping from " MACSTR " layer=%d",
			 MAC2STR(mac), esp32_mesh_get_layer());

		/*
		 * NULL destination → MESH_DATA_TODS: delivered to root.
		 * The root can forward this to the internet if desired.
		 */
		int ret = esp32_mesh_send(NULL,
					  (const uint8_t *)msg, strlen(msg));
		if (ret == -ENETDOWN) {
			/* mesh not yet formed — skip silently */
		} else if (ret != 0) {
			LOG_WRN("send failed: %d", ret);
		} else {
			LOG_INF("TX -> root: %s", msg);
		}
	}
}

K_TIMER_DEFINE(tx_timer, tx_timer_handler, NULL);

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
	LOG_INF("ESP-WIFI-MESH sample starting");
	LOG_INF("Mesh init is handled by SYS_INIT in esp_mesh_drv.c");
	LOG_INF("Waiting for mesh to form...");

	/* Application receive thread */
	k_thread_create(&app_rx_thread_data, app_rx_stack,
			K_THREAD_STACK_SIZEOF(app_rx_stack),
			app_rx_thread, NULL, NULL, NULL,
			APP_RX_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&app_rx_thread_data, "mesh_app_rx");

	/*
	 * Start periodic TX after 10 s to give the mesh time to form and
	 * elect a root before the first message is sent.
	 */
	k_timer_start(&tx_timer, K_SECONDS(10), K_SECONDS(5));

	return 0;
}
