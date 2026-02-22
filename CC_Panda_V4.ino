#include <FS.h>
// #include <LITTLEFS.h> 
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebSocketsClient_Generic.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>

// ---- CONFIGURATION ----
const char* ssid = "NEUMANN WIFI";
const char* password = "12345678";
// const char* elegoo = "192.168.40.177";
static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;

WiFiUDP udp;
const int udpPort = 3000;
char incomingPacket[1024];

String printerIP = ""; // Global to store the found IP

// ---- UI STRUCTURE ----
struct {
    // Top Bar
    lv_obj_t *label_nozzle;
    lv_obj_t *label_bed;
    lv_obj_t *label_box;

    // Center Area
    lv_obj_t *progress_arc;
    lv_obj_t *label_pct;
    lv_obj_t *label_file;
    lv_obj_t *label_layer;

    // Bottom Bar
    lv_obj_t *label_state;
    lv_obj_t *label_fan;
} ui;

// ---- GLOBALS ----
TFT_eSPI tft = TFT_eSPI();
WebSocketsClient ws;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 8];

// ---- PROTOTYPES ----
void ui_create_main_screen();
void update_ui_elements(float nozzle, int nozzleTarget, float bed, int bedTarget, float box, int progress, int layer, int totalLayer, const char* filename, int status);

// ---- LVGL DISPLAY FLUSHING ----
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

void init_lvgl() {
    lv_init();
    tft.begin();
    tft.setRotation(1);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 8);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
}
// ---- Printer Discovery
bool discoverPrinterUDP() {
    Serial.println(F("Starting UDP Discovery..."));
    
    // Check if we are connected to WiFi first
    if (WiFi.status() != WL_CONNECTED) return false;

    udp.begin(udpPort);
    IPAddress broadcastIP(255, 255, 255, 255);
    
    udp.beginPacket(broadcastIP, udpPort);
    udp.print("M99999");
    udp.endPacket();

    unsigned long startMs = millis();
    // Use a small local buffer instead of a large global one
    char packetBuffer[512]; 

    while (millis() - startMs < 5000) {
        int packetSize = udp.parsePacket();
        if (packetSize) {
            int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
            if (len > 0) {
                packetBuffer[len] = 0;
                
                // Reduced JSON doc size to save DRAM
                StaticJsonDocument<512> doc;
                if (!deserializeJson(doc, packetBuffer)) {
                    const char* ip = doc["Data"]["MainboardIP"];
                    if (ip) {
                        printerIP = String(ip);
                        Serial.print(F("Printer Found: "));
                        Serial.println(printerIP);
                        udp.stop();
                        return true;
                    }
                }
            }
        }
        lv_timer_handler(); // Keep screen responsive
        yield(); 
    }
    
    udp.stop();
    Serial.println(F("Discovery Timed Out"));
    return false;
}

// ---- SDCP MESSAGES ----
String makeHandshake() {
    StaticJsonDocument<256> doc;
    doc["Id"] = "esp32-handshake-001";
    JsonObject data = doc.createNestedObject("Data");
    data["Cmd"] = 0;
    data["RequestID"] = "req-001";
    data["TimeStamp"] = millis();
    data["From"] = 1;
    String out;
    serializeJson(doc, out);
    return out;
}

String makeSubscribe() {
    StaticJsonDocument<256> doc;
    doc["Id"] = "esp32-subscribe-001";
    JsonObject data = doc.createNestedObject("Data");
    data["Cmd"] = 0;
    data["RequestID"] = "req-sub-001";
    data["TimeStamp"] = millis();
    data["From"] = 1;
    JsonObject inner = data.createNestedObject("Data");
    inner["Subscribe"] = "Status";
    String out;
    serializeJson(doc, out);
    return out;
}


// ---- DATA PARSING ----
void parseStatus(JsonDocument& doc) {
    if (!doc.containsKey("Status")) return;

    JsonObject s = doc["Status"];
    float bed = s["TempOfHotbed"];
    float nozzle = s["TempOfNozzle"];
    float box = s["TempOfBox"];
    int bedTarget = s["TempTargetHotbed"];
    int nozzleTarget = s["TempTargetNozzle"];
    
    int status = s["PrintInfo"]["Status"];
    int progress = s["PrintInfo"]["Progress"];
    int layer = s["PrintInfo"]["CurrentLayer"];
    int totalLayer = s["PrintInfo"]["TotalLayer"];
    const char* filename = s["PrintInfo"]["Filename"];
    
    // Note: Add logic here to extract fan speed if available in your JSON
    int fanSpeed = s["CurrentFanSpeed"]["BoxFan"]; 
    
    // Debug code to see actual values returned from websockets
    Serial.println("---- Printer Status ----");
    Serial.printf("Nozzle: %.2f / %d\n", nozzle, nozzleTarget);
    Serial.printf("Bed: %.2f / %d\n", bed, bedTarget);
    Serial.printf("Chamber: %.2f\n", box);
    Serial.printf("Progress: %d%%\n", progress);
    Serial.printf("Layer: %d / %d\n", layer, totalLayer);
    Serial.printf("Fan Speed: %d\n", fanSpeed);
    Serial.printf("File: %s\n", filename);
    Serial.printf("State Code: %d\n", status);
    Serial.println("------------------------");

    update_ui_elements(nozzle, nozzleTarget, bed, bedTarget, box, progress, layer, totalLayer, filename, status, fanSpeed);
}

// ---- UI UPDATES ----
void update_ui_elements(float nozzle, int nozzleTarget, float bed, int bedTarget, float box, int progress, int layer, int totalLayer, const char* filename, int status, int fanSpeed) {
    // Temps with explicit double casting for LV_SPRINTF_USE_FLOAT [cite: 93, 94]
    lv_label_set_text_fmt(ui.label_nozzle, "N: %.1f/%d°C", (double)nozzle, nozzleTarget);
    lv_label_set_text_fmt(ui.label_bed, "B: %.1f/%d°C", (double)bed, bedTarget);
    lv_label_set_text_fmt(ui.label_box, "C: %.1f°C", (double)box);

    // Center Progress
    lv_arc_set_value(ui.progress_arc, progress);
    lv_label_set_text_fmt(ui.label_pct, "%d%%", progress);
    lv_label_set_text_fmt(ui.label_layer, "Layer: %d/%d", layer, totalLayer);
    lv_label_set_text_fmt(ui.label_file, "File: %s", filename ? filename : "None");
    lv_label_set_text_fmt(ui.label_fan, "Fan: %d", fanSpeed);

    // State
    // For testing just display the actual code
    // const char* stateStr = (status == 0) ? "Idle" : (status == 1) ? "Heating" : (status == 2) ? "Printing" : (status == 3) ? "Paused" : "Error";
    // lv_label_set_text_fmt(ui.label_state, "State: %s", stateStr);
    lv_label_set_text_fmt(ui.label_state, "Status: %d", status);
}

// ---- WEBSOCKET EVENT HANDLER ----
void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            ws.sendTXT(makeHandshake());
            delay(200);
            ws.sendTXT(makeSubscribe());
            break;
        case WStype_TEXT: {
            StaticJsonDocument<2048> doc;
            auto err = deserializeJson(doc, payload, length);
            if (!err) parseStatus(doc);
            break;
        }
        case WStype_DISCONNECTED:
            lv_label_set_text(ui.label_state, "State: Offline");
            break;
    }
}

// ---- UI LAYOUT ----
void ui_create_main_screen() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    // TOP BAR
    ui.label_nozzle = lv_label_create(scr);
    lv_obj_align(ui.label_nozzle, LV_ALIGN_TOP_LEFT, 10, 10);
    
    ui.label_bed = lv_label_create(scr);
    lv_obj_align(ui.label_bed, LV_ALIGN_TOP_MID, 0, 10);
    
    ui.label_box = lv_label_create(scr);
    lv_obj_align(ui.label_box, LV_ALIGN_TOP_RIGHT, -10, 10);

    // CENTER
    ui.progress_arc = lv_arc_create(scr);
    lv_obj_set_size(ui.progress_arc, 150, 150);
    lv_obj_align(ui.progress_arc, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_bg_angles(ui.progress_arc, 0, 360);
    lv_arc_set_rotation(ui.progress_arc, 270);

    ui.label_pct = lv_label_create(scr);
    lv_obj_align_to(ui.label_pct, ui.progress_arc, LV_ALIGN_CENTER, 0, -10);

    ui.label_layer = lv_label_create(scr);
    lv_obj_align_to(ui.label_layer, ui.label_pct, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);

    ui.label_file = lv_label_create(scr);
    lv_obj_set_width(ui.label_file, 280);
    lv_label_set_long_mode(ui.label_file, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui.label_file, LV_ALIGN_CENTER, 0, 85);

    // BOTTOM BAR
    ui.label_state = lv_label_create(scr);
    lv_obj_align(ui.label_state, LV_ALIGN_BOTTOM_LEFT, 10, -10);

    ui.label_fan = lv_label_create(scr);
    lv_obj_align(ui.label_fan, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_label_set_text(ui.label_fan, "Fan: --%");
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  lv_label_set_text(ui.label_state, "State: CONFIG PORTAL ACTIVE");
  lv_label_set_text(ui.label_file, "Connect phone to: Panda-Monitor-Setup");
  
  // Force a UI refresh since WiFiManager will now block
  for(int i=0; i<20; i++) {
    lv_timer_handler();
    delay(10);
  }
}

void setup() {
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH); 
    Serial.begin(115200);

    init_lvgl();
    ui_create_main_screen();


    WiFiManager wm;
    // This will block until connected or timed out


    bool res = wm.autoConnect("Panda-Monitor-Setup"); 
    if(!res) {
        Serial.println("Failed to connect");
        ESP.restart();
    }

    //If you reach here, you are connected to WiFi!
    // WiFi.begin(ssid, password);
    // while (WiFi.status() != WL_CONNECTED) {
    //     lv_timer_handler();
    //     delay(100);
    // }
    lv_label_set_text(ui.label_state, "State: Connecting to Printer...");   

    MDNS.begin("panda-monitor");
    // Inside setup()
    if (discoverPrinterUDP()) {
        Serial.println("Found Printer");
        ws.begin(printerIP.c_str(), 3030, "/websocket");
    } else {
        Serial.println("No Printer found");
        lv_label_set_text(ui.label_state, "State: MdNS name Not Found");
        // ws.begin(elegoo, 3030, "/websocket");
    }
    
    ws.onEvent(onWebSocketEvent);
    ws.setReconnectInterval(5000);
}

void loop() {
    ws.loop();
    lv_tick_inc(5);
    lv_timer_handler();
    delay(5);
}