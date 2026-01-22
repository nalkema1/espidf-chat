#include "http_server.h"

#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "HTTP_SERVER";
static httpd_handle_t server = NULL;

/* HTML page content */
static const char *INDEX_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>ESP32-P4 Web Server</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<style>"
    "body { font-family: Arial, sans-serif; margin: 40px; background: #f0f0f0; }"
    ".container { background: white; padding: 30px; border-radius: 10px; max-width: 600px; margin: auto; }"
    "h1 { color: #333; }"
    ".status { padding: 15px; background: #e7f3e7; border-radius: 5px; margin: 20px 0; }"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"container\">"
    "<h1>ESP32-P4 WiFi Web Server</h1>"
    "<div class=\"status\">"
    "<p><strong>Status:</strong> Running</p>"
    "<p><strong>Board:</strong> Waveshare ESP32-P4-WIFI6-M</p>"
    "</div>"
    "<p>The HTTP server is working correctly!</p>"
    "</div>"
    "</body>"
    "</html>";

/* Handler for root path "/" */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving index page");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

/* Handler for "/api/status" */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving status API");
    httpd_resp_set_type(req, "application/json");
    const char *json = "{\"status\":\"ok\",\"board\":\"ESP32-P4-WIFI6-M\"}";
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* URI handlers */
static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_status = {
    .uri       = "/api/status",
    .method    = HTTP_GET,
    .handler   = status_get_handler,
    .user_ctx  = NULL
};

esp_err_t http_server_start(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register URI handlers */
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_status);

    ESP_LOGI(TAG, "HTTP server started successfully");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (server == NULL) {
        ESP_LOGW(TAG, "Server not running");
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(server);
    if (ret == ESP_OK) {
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ret;
}
