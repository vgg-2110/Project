
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUDP.h>
#include "esp_camera.h"
#include "esp_log.h"

#define WIFI_SSID       "THAO"
#define WIFI_PASSWORD   "newthao79"

#define UDP_SERVER_IP   "192.168.0.108"
#define UDP_SERVER_PORT 5000
#define UDP_LOCAL_PORT  4999

#define CHUNK_PAYLOAD_SIZE  1400         
#define HEADER_SIZE         10          
#define PACKET_TOTAL_SIZE   (CHUNK_PAYLOAD_SIZE + HEADER_SIZE)

#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

static const char *TAG = "ESP32_CAM_UDP";

WiFiUDP udp;
IPAddress serverIP;


typedef struct __attribute__((packed)) {
    uint32_t seq_number;   
    uint8_t  chunk_index;
    uint8_t  total_chunks;
    int16_t  rssi;
    uint16_t chunk_size;
} PacketHeader;


bool init_camera() {
    camera_config_t config;

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_sscb_sda = CAM_PIN_SIOD;
    config.pin_sscb_scl = CAM_PIN_SIOC;
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;

    config.xclk_freq_hz = 20000000;          
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_VGA;

  
    if (psramFound()) {
        config.jpeg_quality = 12;
        config.fb_count     = 2;
        config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    } else {
        config.jpeg_quality = 20;
        config.fb_count     = 1;
        config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
        config.frame_size   = FRAMESIZE_QVGA;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khởi tạo camera thất bại: 0x%x", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != nullptr) {
        s->set_brightness(s, 0);           
        s->set_contrast(s, 0);             
        s->set_saturation(s, 0);         
        s->set_sharpness(s, 0);             
        s->set_whitebal(s, 1);          
        s->set_awb_gain(s, 1);              
        s->set_wb_mode(s, 0);              
        s->set_exposure_ctrl(s, 1);     
        s->set_aec2(s, 0);                 
        s->set_gain_ctrl(s, 1);           
        s->set_agc_gain(s, 0);             
        s->set_gainceiling(s, GAINCEILING_2X);
        s->set_bpc(s, 0);                 
        s->set_wpc(s, 1);                  
        s->set_raw_gma(s, 1);              
        s->set_lenc(s, 1);                  
        s->set_hmirror(s, 0);               
        s->set_vflip(s, 0);                 
        s->set_dcw(s, 1);                   
        s->set_colorbar(s, 0);            
    }

    ESP_LOGI(TAG, "Camera OV2640 khởi tạo thành công. PSRAM: %s",
             psramFound() ? "CÓ" : "KHÔNG");
    return true;
}

void connect_wifi() {
    ESP_LOGI(TAG, "Đang kết nối WiFi: %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint8_t retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
        delay(500);
        Serial.print(".");
        retries++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGE(TAG, "Kết nối WiFi thất bại! Restart sau 5s...");
        delay(5000);
        ESP.restart();
    }

    ESP_LOGI(TAG, "\nWiFi kết nối! IP: %s | RSSI: %d dBm",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

int16_t read_rssi() {
    return (int16_t)WiFi.RSSI();  
}

void send_frame_udp(camera_fb_t *fb, uint32_t seq) {
    if (!fb || fb->len == 0) return;

    int16_t rssi         = read_rssi();
    size_t  total_bytes  = fb->len;
    uint8_t total_chunks = (total_bytes + CHUNK_PAYLOAD_SIZE - 1) / CHUNK_PAYLOAD_SIZE;

    uint8_t packet[HEADER_SIZE + CHUNK_PAYLOAD_SIZE];

    ESP_LOGI(TAG, "[Frame #%lu] Size: %zu bytes | Chunks: %u | RSSI: %d dBm",
             (unsigned long)seq, total_bytes, total_chunks, rssi);

    for (uint8_t i = 0; i < total_chunks; i++) {
        size_t offset      = (size_t)i * CHUNK_PAYLOAD_SIZE;
        size_t chunk_bytes = total_bytes - offset;
        if (chunk_bytes > CHUNK_PAYLOAD_SIZE)
            chunk_bytes = CHUNK_PAYLOAD_SIZE;

        PacketHeader *hdr = (PacketHeader *)packet;
        hdr->seq_number  = seq;
        hdr->chunk_index = i;
        hdr->total_chunks= total_chunks;
        hdr->rssi        = rssi;
        hdr->chunk_size  = (uint16_t)chunk_bytes;
        memcpy(packet + HEADER_SIZE, fb->buf + offset, chunk_bytes);
        udp.beginPacket(serverIP, UDP_SERVER_PORT);
        udp.write(packet, HEADER_SIZE + chunk_bytes);
        udp.endPacket();
        delayMicroseconds(500);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== ESP32-CAM UDP Streamer ===");

    connect_wifi();
    if (!init_camera()) {
        ESP_LOGE(TAG, "Không thể khởi tạo camera! Dừng lại.");
        while (true) delay(1000);
    }

    serverIP.fromString(UDP_SERVER_IP);
    udp.begin(UDP_LOCAL_PORT);

    ESP_LOGI(TAG, "Sẵn sàng stream tới %s:%d", UDP_SERVER_IP, UDP_SERVER_PORT);
}

void loop() {
    static uint32_t frame_seq = 0;
    static uint32_t last_reconnect = 0;
    if (WiFi.status() != WL_CONNECTED) {
        uint32_t now = millis();
        if (now - last_reconnect > 5000) {
            ESP_LOGW(TAG, "WiFi mất kết nối, đang kết nối lại...");
            WiFi.reconnect();
            last_reconnect = now;
        }
        delay(500);
        return;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Chụp ảnh thất bại!");
        delay(100);
        return;
    }

    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGE(TAG, "Frame không phải JPEG!");
        esp_camera_fb_return(fb);
        delay(50);
        return;
    }

     uint32_t t_start = millis();
    send_frame_udp(fb, frame_seq++);
    
    esp_camera_fb_return(fb); 
    fb = nullptr;         
    
    uint32_t t_elapsed = millis() - t_start;
    ESP_LOGI(TAG, "Gửi xong trong %lu ms | FPS: %.1f",
             (unsigned long)t_elapsed,
             t_elapsed > 0 ? 1000.0f / t_elapsed : 0.0f);

    delay(33);
}