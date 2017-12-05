/**
 * Garage door control via WIFI (both AP and Station simultaneous).
 *
 * <p>This program is for an ESP32 with a relay connected to a GPIO pin for the following purposes.</p>
 *
 * <ul>
 *
 * <li>It configures a GPIO pin such that it can trigger a relay for
 * 0.1 seconds to simulate a "garage door button" press.</li>
 *
 * <li>It runs a Access Point with its own SSID and Password.</li>
 *
 * <li>It also connects to another Access Point (your home
 * WIFI). Providing a second (and often more convenient) means of
 * access.</li>
 *
 * <li>It runs a web server (HTTPS).</li>
 *
 * <li>Users may then connect to the web server, enter an optional
 * password and press a button to trigger the relay connected to your
 * garage door.</li>
 *
 * </ul>
 *
 * <p>Much of this code is copy/paste/modifications of the following
 * examples found under the esp-idf/examples area that comes with the
 * ESP32 development code.</p>
 *
 * <dl>
 *
 * <dt>get-started/blink</dt><dd>Example of controlling a GPIO
 * pin.</dd>
 *
 * <dt>wifi/iperf</dt><dd>Example of setting up WIFI AP mode and
 * Station mode on a ESP32 (used this as a starting point with some
 * google queries to find the compination for setting up both modes
 * simultaneously).</dd>
 *
 * <dt>get-started/blink</dt><dd>Example of controlling a GPIO
 * pin.</dd>
 *
 * <dt>protocols/mdns</dt><dd>To enable mDNS (avahi) so that
 * "https://garage.local" resolves to an IPv4 address for mDNS enabled
 * clients.</dd>
 *
 * <dt>protocols/openssl_server</dt><dd>For running a simple secure
 * https:// server.</dd>
 *
 * </dl>
 *
 * <p>NOTE: Prior to deploying, you should verify that after
 * connecting the ESP32 and relay to your garage door that dropping
 * and restoring power to the ESP32 des NOT trigger your garage door
 * to open!</p>
 *
 * <p>This example code is in the Public Domain (or CC0 licensed, at
 * your option.)</p>
 *
 * <p>Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.</p>
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"

#include "nvs_flash.h"

#include "tcpip_adapter.h"
#include "openssl/ssl.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mdns.h"

/*
 * You can run 'make menuconfig' to configure SSIDs, Passwords and the
 * GPIO pin connected to the relay (alternatively, you can hard code
 * the values here).
 */
#define RELAY_GPIO CONFIG_RELAY_GPIO

#define AP_SSID CONFIG_AP_SSID
#define AP_PASSWORD CONFIG_AP_PASSWORD

#define STA_SSID CONFIG_STA_SSID
#define STA_PASSWORD CONFIG_STA_PASSWORD

#define OPEN_PASSWORD CONFIG_OPEN_PASSWORD

#define ENABLE_HTTPS CONFIG_ENABLE_HTTPS

//
// Remaining settings are not set via make menuconfig
//
#if ENABLE_HTTPS
const char* OPEN_PASSWORD_MATCH = "password=" OPEN_PASSWORD;
#endif

// Used for diagnostic log messages
static const char *TAG="gdoor";

// Used to track Station connection
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;
static bool reconnect = true;

// Used to configure mDNS (avahi)
#define MDNS_NAME CONFIG_MDNS_NAME
#define MDNS_INSTANCE CONFIG_MDNS_INSTANCE

// Template for HTTP header response (sprintf to set content-length)
#define HTTP_HEADER \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Type: text/html\r\n" \
  "Content-Length: %d\r\n\r\n"

// The HTML document user interacts with
#if ENABLE_HTTPS
const char* HTML_MAIN = \
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
  "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\r\n" \
  "<html xmlns=\"http://www.w3.org/1999/xhtml\">\r\n" \
  "<head>\r\n" \
  "<title>Garage</title></head><body>\r\n" \
  "<form action=\"/\" method=\"post\">\r\n" \
  "<input type=\"hidden\" name=\"action\" id=\"action\" value=\"open\"/>\r\n" \
  "<div>\r\n" \
  "<input type=\"submit\" value=\"Open\" id=\"action\"/>\r\n" \
  "<input type=\"text\" name=\"username\" value=\"\" id=\"username\" style=\"display: none;\"/>\r\n" \
  "<br/>\r\n" \
  "Password: <input type=\"password\" name=\"password\" value=\"\" id=\"password\"/>\r\n" \
  "<input type=\"submit\" value=\"Open2\" id=\"action2\" style=\"display: none;\"/>\r\n" \
  "</div>\r\n" \
  "</form>\r\n" \
  "</body>\r\n" \
  "</html>\r\n";
#else
const char* HTML_MAIN = \
  "<!DOCTYPE html>\r\n" \
  "<html>\r\n" \
  "<head>\r\n" \
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\r\n" \
  "<title>Garage Door</title></head><body>\r\n" \
  "<form action=\"/\" method=\"post\">\r\n" \
  "<div style=\"margin-top: 100px;\">\r\n" \
  "<input type=\"hidden\" name=\"action\" id=\"action\" value=\"open\"/>\r\n" \
  "<input type=\"submit\" value=\"Garage Door\" id=\"action\" style=\"display:block; font-size: 24pt; text-align: center; padding: 1em; width: 10em; margin-left:auto; margin-right: auto;\"/>\r\n" \
  "</div>\r\n" \
  "</form>\r\n" \
  "</body>\r\n" \
  "</html>\r\n";
#endif

//
// SSL configuration
//
#if ENABLE_HTTPS
#define OPENSSL_TASK_NAME        "https"
#define OPENSSL_TASK_STACK_WORDS 10240
#define OPENSSL_TASK_PRIORITY    8
#define OPENSSL_RECV_BUF_LEN       2048
#define OPENSSL_LOCAL_TCP_PORT     443
#else
#define HTTP_TASK_NAME        "http"
#define HTTP_TASK_STACK_WORDS 10240
#define HTTP_TASK_PRIORITY    8
#define HTTP_RECV_BUF_LEN       2048
#define HTTP_LOCAL_TCP_PORT     80
#endif

/**
 * Sends brief pulse to relay to simulate user pressing garage door button.
 *
 * gpioPin - GPIO output pin the relay is connected to.
 */
void open_door(uint32_t gpioPin) {
  // Turn on for brief time, then turn off
  gpio_set_level(gpioPin, 1);
  vTaskDelay(100 / portTICK_PERIOD_MS);
  gpio_set_level(gpioPin, 0);
}

/**
 * Task function to send brief pulse to relay to simulate user
 * pressing garage door button.
 *
 * pvPatameter - Actually a uint_32 value identifying the GPIO output
 * pin the relay is connected to.
 */
void open_door_task(void *pvParameter) {
  // GPIO pin to blink is passed as parameter
  uint32_t gpio = (uint32_t) pvParameter;

  // Simulate press of garage door button via relay
  open_door(gpio);

  // Done with task
  vTaskDelete(NULL);
}

#if ENABLE_HTTPS
/**
 * Writes out HTML document (form) user interacts with to open the
 * garage door.
 *
 * @param ssl SSL connection to write response header and content on.
 *
 * @param html HTML document (content) of HTTP response.
 *
 * @return Integer value larger than 0 if data successfully sent.
 */
static int write_html(SSL* ssl, const char* html) {
  int html_len;
  int ret;
  char http_header[sizeof(HTTP_HEADER) + 10];

  // Determine length of content to write out and put out header
  // section with content length set appropriately
  html_len = strlen(html);
  snprintf(http_header, sizeof(http_header), HTTP_HEADER, html_len);
  const int http_header_len = strlen(http_header);
  ret = SSL_write(ssl, http_header, http_header_len);

  // Write out content of document
  if (ret > 0) {
    ESP_LOGD(TAG, "HTTP header write of %d bytes OK (%d)\n%s", http_header_len, ret, http_header);
    ret = SSL_write(ssl, html, html_len);
    if (ret > 0) {
      ESP_LOGD(TAG, "HTML document write of %d bytes OK (%d)\n%s", html_len, ret, html);
    } else {
      ESP_LOGE(TAG, "HTML document write of %d bytes FAILED (%d)", html_len, ret);
    }
  } else {
    ESP_LOGE(TAG, "HTTP header write of %d bytes FAILED (%d)", http_header_len, ret);
  }

  return ret;
}

/**
 * Task that handles SSL requests (based of ESP32 example code).
 *
 * <p>This code is pretty close to identical to what is found in the
 * esp-idf/exampls/protocols/openssl_server.c. The main difference is
 * how the user response is checked. If the response from the user is
 * a POST action and the "action=open" and "password=PASSWORD" strings
 * are found in the response, we will start a task to trigger the
 * "pressing of the garage door button".</p>
 *
 * <p>NOTE: Don't ask me or think too much about the rest of it.</p>
 *
 * @param p Task parameter (not used).
 */
static void openssl_task(void *ignored) {
  int ret;

  SSL_CTX *ctx;
  SSL *ssl;

  int socket, new_socket;
  socklen_t addr_len;
  struct sockaddr_in sock_addr;

  char recv_buf[OPENSSL_RECV_BUF_LEN];

  extern const unsigned char cacert_pem_start[] asm("_binary_cacert_pem_start");
  extern const unsigned char cacert_pem_end[]   asm("_binary_cacert_pem_end");
  const unsigned int cacert_pem_bytes = cacert_pem_end - cacert_pem_start;

  extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
  extern const unsigned char prvtkey_pem_end[]   asm("_binary_prvtkey_pem_end");
  const unsigned int prvtkey_pem_bytes = prvtkey_pem_end - prvtkey_pem_start;   

  ESP_LOGI(TAG, "SSL server context create ......");
  /* For security reasons, it is best if you can use
     TLSv1_2_server_method() here instead of TLS_server_method().
     However some old browsers may not support TLS v1.2.
  */
  ctx = SSL_CTX_new(TLSv1_2_server_method());
  if (!ctx) {
    ESP_LOGE(TAG, "failed");
    goto failed1;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "SSL server context set own certification......");
  ret = SSL_CTX_use_certificate_ASN1(ctx, cacert_pem_bytes, cacert_pem_start);
  if (!ret) {
    ESP_LOGE(TAG, "failed");
    goto failed2;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "SSL server context set private key......");
  ret = SSL_CTX_use_PrivateKey_ASN1(0, ctx, prvtkey_pem_start, prvtkey_pem_bytes);
  if (!ret) {
    ESP_LOGE(TAG, "failed");
    goto failed2;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "SSL server create socket ......");
  socket = socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) {
    ESP_LOGE(TAG, "failed");
    goto failed2;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "SSL server socket bind ......");
  memset(&sock_addr, 0, sizeof(sock_addr));
  sock_addr.sin_family = AF_INET;
  sock_addr.sin_addr.s_addr = 0;
  sock_addr.sin_port = htons(OPENSSL_LOCAL_TCP_PORT);
  ret = bind(socket, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
  if (ret) {
    ESP_LOGE(TAG, "failed");
    goto failed3;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "SSL server socket listen ......");
  ret = listen(socket, 32);
  if (ret) {
    ESP_LOGE(TAG, "failed");
    goto failed3;
  }
  ESP_LOGD(TAG, "OK");

 reconnect:
  ESP_LOGI(TAG, "SSL server create ......");
  ssl = SSL_new(ctx);
  if (!ssl) {
    ESP_LOGE(TAG, "failed");
    goto failed3;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "SSL server socket accept client ......");
  new_socket = accept(socket, (struct sockaddr *)&sock_addr, &addr_len);
  if (new_socket < 0) {
    ESP_LOGE(TAG, "failed" );
    goto failed4;
  }
  ESP_LOGD(TAG, "OK");

  SSL_set_fd(ssl, new_socket);

  ESP_LOGI(TAG, "SSL server accept client ......");
  ret = SSL_accept(ssl);
  if (!ret) {
    ESP_LOGE(TAG, "failed");
    goto failed5;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "SSL server read message ......");
  do {
    memset(recv_buf, 0, OPENSSL_RECV_BUF_LEN);
    ret = SSL_read(ssl, recv_buf, OPENSSL_RECV_BUF_LEN - 1);
    if (ret <= 0) {
      break;
    }
    ESP_LOGD(TAG, "SSL read:\n%s\n", recv_buf);
    if (strstr(recv_buf, "POST ") &&
	strstr(recv_buf, " HTTP/1.1") &&
	strstr(recv_buf, "action=open") &&
	strstr(recv_buf, OPEN_PASSWORD_MATCH)) {

      ESP_LOGI(TAG, "Triggering garage door");
      xTaskCreate(&open_door_task, "open_door_task", configMINIMAL_STACK_SIZE, (void*) RELAY_GPIO, 5, NULL);
    }
	
    // ESP_LOGI(TAG, "SSL write message:\n%s\n", send_data)
    // ret = SSL_write(ssl, send_data, send_bytes);
    ret = write_html(ssl, HTML_MAIN);
	
    if (ret <= 0) {
      ESP_LOGE(TAG, "Failed to send HTML document to client");
      break;
    }
    ESP_LOGD(TAG, "Sent HTML document to client");
  } while (1);
    
  SSL_shutdown(ssl);
 failed5:
  close(new_socket);
  new_socket = -1;
 failed4:
  SSL_free(ssl);
  ssl = NULL;
  goto reconnect;
 failed3:
  close(socket);
  socket = -1;
 failed2:
  SSL_CTX_free(ctx);
  ctx = NULL;
 failed1:
  vTaskDelete(NULL);
  return ;
} 

/**
 * Starts up the HTTPS server.
 *
 * <p>This code starts up the task that waits for incoming https
 * connections.</p>
 */
static void openssl_server_init(void) {
  static int created = 0;
  int ret;
  xTaskHandle openssl_handle;

  if (created) {
    return;
  }

  ret = xTaskCreate(openssl_task,
		    OPENSSL_TASK_NAME,
		    OPENSSL_TASK_STACK_WORDS,
		    NULL,
		    OPENSSL_TASK_PRIORITY,
		    &openssl_handle); 

  if (ret != pdPASS)  {
    ESP_LOGE(TAG, "create task %s failed", OPENSSL_TASK_NAME);
  } else {
    created = 1;
  }
}
#else
/**
 * Writes out HTML document (form) user interacts with to open the
 * garage door.
 *
 * @param fd File descriptor for socket connetion.
 *
 * @param html HTML document (content) of HTTP response.
 *
 * @return Integer value larger than 0 if data successfully sent.
 */
static int write_html(int fd, const char* html) {
  int html_len;
  int ret;
  char http_header[sizeof(HTTP_HEADER) + 10];

  // Determine length of content to write out and put out header
  // section with content length set appropriately
  html_len = strlen(html);
  snprintf(http_header, sizeof(http_header), HTTP_HEADER, html_len);
  const int http_header_len = strlen(http_header);
  ret = write(fd, http_header, http_header_len);

  // Write out content of document
  if (ret > 0) {
    ESP_LOGD(TAG, "HTTP header write of %d bytes OK (%d)\n%s", http_header_len, ret, http_header);
    ret = write(fd, html, html_len);
    if (ret > 0) {
      ESP_LOGD(TAG, "HTML document write of %d bytes OK (%d)\n%s", html_len, ret, html);
    } else {
      ESP_LOGE(TAG, "HTML document write of %d bytes FAILED (%d)", html_len, ret);
    }
  } else {
    ESP_LOGE(TAG, "HTTP header write of %d bytes FAILED (%d)", http_header_len, ret);
  }

  return ret;
}

/**
 * Task that handles SSL requests (based of ESP32 example code).
 *
 * <p>This code is pretty close to identical to what is found in the
 * esp-idf/exampls/protocols/openssl_server.c. The main difference is
 * how the user response is checked. If the response from the user is
 * a POST action and the "action=open" and "password=PASSWORD" strings
 * are found in the response, we will start a task to trigger the
 * "pressing of the garage door button".</p>
 *
 * <p>NOTE: Don't ask me or think too much about the rest of it.</p>
 *
 * @param p Task parameter (not used).
 */
static void http_task(void *ignored) {
  int ret;
  int socket, new_socket;
  socklen_t addr_len;
  struct sockaddr_in sock_addr;

  char recv_buf[HTTP_RECV_BUF_LEN];

  ESP_LOGI(TAG, "HTTP server create socket ......");
  socket = socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) {
    ESP_LOGE(TAG, "failed");
    goto failed2;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "HTTP server socket bind ......");
  memset(&sock_addr, 0, sizeof(sock_addr));
  sock_addr.sin_family = AF_INET;
  sock_addr.sin_addr.s_addr = 0;
  sock_addr.sin_port = htons(HTTP_LOCAL_TCP_PORT);
  ret = bind(socket, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
  if (ret) {
    ESP_LOGE(TAG, "failed");
    goto failed3;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "HTTP server socket listen ......");
  ret = listen(socket, 32);
  if (ret) {
    ESP_LOGE(TAG, "failed");
    goto failed3;
  }
  ESP_LOGD(TAG, "OK");

 reconnect:
  ESP_LOGI(TAG, "HTTP server socket accept client ......");
  new_socket = accept(socket, (struct sockaddr *)&sock_addr, &addr_len);
  if (new_socket < 0) {
    ESP_LOGE(TAG, "failed" );
    goto failed4;
  }
  ESP_LOGD(TAG, "OK");

  ESP_LOGI(TAG, "HTTP server read message ......");

  memset(recv_buf, 0, HTTP_RECV_BUF_LEN);
  ret = read(new_socket, recv_buf, HTTP_RECV_BUF_LEN - 1);
  if (ret <= 0) {
    goto failed5;
  }
  ESP_LOGD(TAG, "HTTP read:\n%s\n", recv_buf);
  if (strstr(recv_buf, "POST ") &&
      strstr(recv_buf, " HTTP/1.1") &&
      strstr(recv_buf, "action=open")) {

    ESP_LOGI(TAG, "Triggering garage door");
    xTaskCreate(&open_door_task, "open_door_task", configMINIMAL_STACK_SIZE, (void*) RELAY_GPIO, 5, NULL);
  }
  ret = write_html(new_socket, HTML_MAIN);
	
  if (ret <= 0) {
    ESP_LOGE(TAG, "Failed to send HTML document to client");
    goto failed5;
  }
  ESP_LOGD(TAG, "Sent HTML document to client");

 failed5:
  close(new_socket);
  new_socket = -1;

 failed4:
  goto reconnect;
 failed3:
  close(socket);
  socket = -1;
 failed2:
  vTaskDelete(NULL);
  return ;
} 

/**
 * Starts up the HTTP server.
 *
 * <p>This code starts up the task that waits for incoming http
 * connections.</p>
 */
static void http_server_init(void) {
  static int created = 0;
  int ret;
  xTaskHandle http_handle;

  if (created) {
    return;
  }

  ret = xTaskCreate(http_task,
		    HTTP_TASK_NAME,
		    HTTP_TASK_STACK_WORDS,
		    NULL,
		    HTTP_TASK_PRIORITY,
		    &http_handle); 

  if (ret != pdPASS)  {
    ESP_LOGE(TAG, "create task %s failed", HTTP_TASK_NAME);
  } else {
    created = 1;
  }
}
#endif

/**
 * Processes WIFI related events as they occur.
 *
 * @param ctx Context associated with event.
 * @param event Event information (we care primarily about the event_id).
 */
static esp_err_t wifi_event_handler(void *ctx, system_event_t *event) {

  switch(event->event_id) {

  case SYSTEM_EVENT_STA_START:
    esp_wifi_connect();
    break;

  case SYSTEM_EVENT_STA_GOT_IP:
    xEventGroupClearBits(wifi_event_group, DISCONNECTED_BIT);
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
#if ENABLE_HTTPS    
    ESP_LOGI(TAG, "Starting HTTPS server...");
    openssl_server_init();
#else
    ESP_LOGI(TAG, "Starting HTTP server...");
    http_server_init();
#endif
    break;

  case SYSTEM_EVENT_STA_DISCONNECTED:
    if (reconnect) {
      ESP_LOGI(TAG, "sta disconnect, reconnect...");
      esp_wifi_connect();
    } else {
      ESP_LOGI(TAG, "sta disconnect");
    }
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    xEventGroupSetBits(wifi_event_group, DISCONNECTED_BIT);
    break;

  case SYSTEM_EVENT_AP_START: {
    tcpip_adapter_ip_info_t ip;
    memset(&ip, 0, sizeof(tcpip_adapter_ip_info_t));
    // From event_default_handlers.c (trying to peak at AP IPv4 info)
    tcpip_adapter_get_ip_info(ESP_IF_WIFI_AP, &ip);
    ESP_LOGI(TAG, "AP ip: " IPSTR ", mask: " IPSTR ", gw: " IPSTR,
	     IP2STR(&ip.ip),
	     IP2STR(&ip.netmask),
	     IP2STR(&ip.gw));
    // Start up DHCP server on access point
    // Not required, dhcp server is started automatically in AP mode
    // ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(ESP_IF_WIFI_AP));
    break;
  }

  case SYSTEM_EVENT_AP_STOP:
    break;

  case SYSTEM_EVENT_AP_STACONNECTED:
    break;

  case SYSTEM_EVENT_AP_STADISCONNECTED:
    break;
    
  default:
    break;
  }
  return ESP_OK;
}

/**
 * Enables both AP and Station mode (resulting in two IP addresses).
 *
 * @return true If set up was successful, false if not.
 */
static bool wifi_cmd_apsta() {
  tcpip_adapter_init();
  wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK( esp_event_loop_init(wifi_event_handler, NULL) );
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

  ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
  ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

  ESP_LOGI(TAG, "AP (%s, %s) and STA (%s, %s)  mode", AP_SSID, AP_PASSWORD, STA_SSID, STA_PASSWORD);

  //
  // Set up STA connection
  //
  int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);

  wifi_config_t sta_config = { 0 };

  strlcpy((char*) sta_config.sta.ssid, STA_SSID, sizeof(sta_config.sta.ssid));
  strncpy((char*) sta_config.sta.password, STA_PASSWORD, sizeof(sta_config.sta.password));

  if (bits & CONNECTED_BIT) {
    reconnect = false;
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    ESP_ERROR_CHECK( esp_wifi_disconnect() );
    xEventGroupWaitBits(wifi_event_group, DISCONNECTED_BIT, 0, 1, portTICK_RATE_MS);
  }

  reconnect = true;

  xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 5000/portTICK_RATE_MS);

  //
  // Set up AP
  //
  wifi_config_t ap_config = {
    .ap = {
      .ssid = "",
      .ssid_len = 0,
      .max_connection = 4,
      .password = "",
      .authmode = WIFI_AUTH_WPA_WPA2_PSK
    },
  };

  reconnect = false; 
  strncpy((char*) ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid));
  if (strlen(AP_PASSWORD) != 0 && strlen(AP_PASSWORD) < 8) {
    reconnect = true;
    ESP_LOGE(TAG, "password less than 8");
    return false;
  }
  strncpy((char*) ap_config.ap.password, AP_PASSWORD, sizeof(ap_config.ap.password));

  if (strlen(AP_PASSWORD) == 0) {
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
  ESP_ERROR_CHECK( esp_wifi_start() );

  return true;
}

/**
 * Helper method to enable mDNS on a particular interface.
 *
 * <p>NOTE: We were unable to get this to work on both the
 * AP and Station interfaces simultaneously. So, we went
 * with just the Station interface since that one has a
 * dynamic address.</p>
 *
 * @param if_adapt - The interface to associate with the server.
 * @param mdns - Pointer to a pointer of the mDNS server data
 * structure (if points to 0 we will initialize and configure,
 * otherwise we assume already done from prior invocation).
 */
static void enable_mdns(int if_adapt, mdns_server_t** mdns) {
    
  if (!*mdns) {
    esp_err_t err = mdns_init(if_adapt, mdns);
    if (err) {
      ESP_LOGE(TAG, "Failed starting MDNS: %u", err);
      return;
    }

    ESP_ERROR_CHECK(mdns_set_hostname(*mdns, MDNS_NAME));
    ESP_ERROR_CHECK(mdns_set_instance(*mdns, MDNS_INSTANCE));

#if ENABLE_HTTPS      
    ESP_ERROR_CHECK(mdns_service_add(*mdns, "_https", "_tcp", 443));
#else
    ESP_ERROR_CHECK(mdns_service_add(*mdns, "_http", "_tcp", 80));
    ESP_ERROR_CHECK(mdns_service_instance_set(*mdns, "_http", "_tcp", "ESP32 Garage Door Relay") );
    ESP_LOGD(TAG, "Finished starting MDNS (Avahi) service");
#endif
  }
}

/**
 * Starts up nDNS/avahi task.
 *
 * @param pvParameters - Task parameters (ignored).
 */
static void mdns_task(void *pvParameters) {
  mdns_server_t * mdns = NULL;
  //mdns_server_t * mdnsAp = NULL;

  while(1) {
    /*
     * Wait for the callback to set the CONNECTED_BIT in the
     * event group.
     */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
			false, true, portMAX_DELAY);

    // NOTE: Trying to enable AP and Station with same name did not
    // seem to work (did not seem to hurt though).
    //if (!mdnsAp) {
    //  ESP_LOGI(TAG, "Starting AP mDNS/avahi");
    //  enable_mdns(TCPIP_ADAPTER_IF_AP, &mdnsAp);
    //}
    if (!mdns) {
      ESP_LOGI(TAG, "Connected to AP - Starting Station mDNS/avahi");
      enable_mdns(TCPIP_ADAPTER_IF_STA, &mdns);
    }
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

/**
 * Main entry point into the application.
 *
 * <ul>
 *
 * <li>Initializes GPIO pin used to control relay connected to garage
 * door opener.</li>
 *
 * <li>Initializes the flash memory.</li>
 *
 * <li>Set ups WIFI (which then sets up HTTPS or HTTP server).</li>
 *
 * <li>Starts up mDNS (avahi) task so you can find on home network as
 * http://garage.local/.</li>
 *
 * </ul>
 */
void app_main() {
  gpio_pad_select_gpio(RELAY_GPIO);
  gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(RELAY_GPIO, 0);

  ESP_ERROR_CHECK(nvs_flash_init());
  wifi_cmd_apsta();

  xTaskCreate(&mdns_task, "mdns_task", 2048, NULL, 5, NULL);
}
