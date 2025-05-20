namespace fs { class FS; }
using FS = fs::FS;

#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFiManager.h>
#include <FS.h>
#include <WebServer.h>

// --- UI Object Handles ---
lv_obj_t *settings_btn;      // Settings button object
lv_obj_t *settings_menu;     // Settings menu container
lv_obj_t *close_btn;         // Close button in settings menu
bool settings_open = false;  // Is settings menu open?

// --- Pin Definitions ---
#define XPT2046_IRQ 36   // Touchscreen IRQ pin
#define XPT2046_MOSI 32  // Touchscreen MOSI pin
#define XPT2046_MISO 39  // Touchscreen MISO pin
#define XPT2046_CLK 25   // Touchscreen CLK pin
#define XPT2046_CS 33    // Touchscreen CS pin
#define LCD_BACKLIGHT_PIN 21 // LCD backlight control pin

// --- Display Settings ---
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
int x, y, z; // Touchscreen coordinates

// --- WiFi & NTP ---
const char* ssid = "VM6842809";
const char* password = "h2hFnrycpprn";
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600); // GMT+1 offset in seconds

// --- LVGL & Display ---
TFT_eSPI tft = TFT_eSPI(); // TFT display object
uint32_t draw_buf[DRAW_BUF_SIZE / 4]; // LVGL draw buffer
lv_obj_t *clock_label; // Clock label object
SPIClass touchscreenSPI = SPIClass(VSPI); // SPI for touchscreen
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ); // Touchscreen object

// --- Feature Flags ---
bool isAnalog = false; // Analog clock mode flag
int currentMenu = 0;   // Current menu index

// --- Settings Variables (persistent) ---
String SETTING_SSID = "VM6842809";
String SETTING_PASSWORD = "h2hFnrycpprn";
int SETTING_BRIGHTNESS = 255; // 0-255, min 26 (10%)
int SETTING_GMT_OFFSET = 1; // hours, default GMT+1

#define BRIGHTNESS_MIN 26 // 10% of 255
#define SPK_PIN 22 // Speaker pin (change as needed)
#define ALARM_BTN_PIN 0 // GPIO 0 for alarm stop

// --- Function Prototypes ---
void showClock();
void showSettingsMenu();
void closeSettingsMenu();
void settings_btn_event_cb(lv_event_t *e);
void close_btn_event_cb(lv_event_t *e);
void brightness_slider_event_cb(lv_event_t *e);
void setupWebServer();
void handleTestAlarm();
void startTestAlarm();

// --- Touchscreen Read Callback for LVGL ---
void touchscreen_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();

    // Map raw touch coordinates to screen coordinates
    x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
    y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
    z = p.z;

    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}
// --- Timekeeping Variables ---
unsigned long lastNtpSync = 0;
unsigned long lastMillis = 0;
unsigned long currentEpoch = 0;
const unsigned long ntpSyncInterval = 60 * 60 * 1000UL; // Sync every hour

// --- Webserver and Alarm Logic ---
WebServer server(80);
volatile bool alarm_active = false;

void IRAM_ATTR stopAlarmISR() {
  alarm_active = false;
}

void startTestAlarm() {
  alarm_active = true;
  pinMode(SPK_PIN, OUTPUT);
  pinMode(ALARM_BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ALARM_BTN_PIN), stopAlarmISR, FALLING);
  while (alarm_active) {
    digitalWrite(SPK_PIN, HIGH);
    delay(100);
    digitalWrite(SPK_PIN, LOW);
    delay(100);
    server.handleClient(); // Allow webserver to process stop requests
    lv_timer_handler(); // Keep UI responsive
  }
  detachInterrupt(digitalPinToInterrupt(ALARM_BTN_PIN));
  digitalWrite(SPK_PIN, LOW);
}

void handleTestAlarm() {
  server.send(200, "text/html", "<html><body><h1>Alarm triggered!</h1><p>Press the button on the device to stop.</p></body></html>");
  startTestAlarm();
}

void setupWebServer() {
  server.on("/test-alarm", handleTestAlarm);
  server.on("/", []() {
    server.send(200, "text/html", "<html><body><button onclick=\"location.href='/test-alarm'\">Test Alarm</button></body></html>");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(0); // Set display to landscape
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);

  lv_init(); // Initialize LVGL

  // Init touchscreen hardware
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(0); // Set touchscreen orientation

  // Initialize LVGL display and input
  lv_display_t *disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchscreen_read);

  // --- Create UI ---
  clock_label = lv_label_create(lv_scr_act()); // Create clock label
  lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(clock_label, "Connecting...");

  // Settings button (top right)
  settings_btn = lv_btn_create(lv_scr_act());
  lv_obj_align(settings_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_event_cb(settings_btn, settings_btn_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_label = lv_label_create(settings_btn);
  lv_label_set_text(btn_label, LV_SYMBOL_SETTINGS);

  // Increase font size for clock
  static lv_style_t style_large_font;
  lv_style_init(&style_large_font);
  lv_style_set_text_font(&style_large_font, &lv_font_montserrat_36);
  lv_obj_add_style(clock_label, &style_large_font, 0);

  // --- WiFiManager section ---
  WiFiManager wifiManager;
  wifiManager.autoConnect("CYD-Clock-Setup");

  // Now connected to WiFi, start NTP
  timeClient.begin();
  timeClient.update();
  currentEpoch = timeClient.getEpochTime();
  lastMillis = millis();
  lastNtpSync = millis();

  setupWebServer();
}

void loop() {
  // Main loop: update time, UI, and handle LVGL tasks
  updateClockTime();
  showClock();
  lv_timer_handler();
  lv_tick_inc(5);
  delay(5);

  server.handleClient();
}

// --- Update NTP Time Periodically ---
void updateClockTime() {
  // Sync with NTP every hour
  if (millis() - lastNtpSync > ntpSyncInterval) {
    timeClient.update();
    currentEpoch = timeClient.getEpochTime();
    lastMillis = millis();
    lastNtpSync = millis();
  }
}

// --- Show Digital Clock on Main Screen ---
void showClock() {
  // Calculate current time using millis since last sync
  unsigned long elapsed = (millis() - lastMillis) / 1000;
  unsigned long displayEpoch = currentEpoch + elapsed;

  // Convert epoch to HH:MM:SS
  int hours = (displayEpoch  % 86400L) / 3600;
  int minutes = (displayEpoch % 3600) / 60;
  int seconds = displayEpoch % 60;

  // Convert to 12-hour format and determine AM/PM
  int displayHour = hours % 12;
  if (displayHour == 0) displayHour = 12;
  const char* ampm = (hours < 12) ? "AM" : "PM";

  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", displayHour, minutes, seconds, ampm);
  lv_label_set_text(clock_label, buf);
}

// --- Show Fullscreen Settings Menu ---
void showSettingsMenu() {
  settings_open = true;
  lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN); // Hide clock
  lv_obj_add_flag(settings_btn, LV_OBJ_FLAG_HIDDEN); // Hide settings button

  // Create settings menu container (fullscreen)
  settings_menu = lv_obj_create(lv_scr_act());
  lv_obj_set_size(settings_menu, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_align(settings_menu, LV_ALIGN_CENTER, 0, 0);

  // Brightness slider label
  lv_obj_t *slider_label = lv_label_create(settings_menu);
  lv_label_set_text(slider_label, "Brightness");
  lv_obj_align(slider_label, LV_ALIGN_TOP_MID, 0, 20);

  // Brightness slider
  lv_obj_t *brightness_slider = lv_slider_create(settings_menu);
  lv_obj_set_width(brightness_slider, SCREEN_WIDTH - 60);
  lv_obj_align(brightness_slider, LV_ALIGN_TOP_MID, 0, 60);
  lv_slider_set_range(brightness_slider, BRIGHTNESS_MIN, 255);
  lv_slider_set_value(brightness_slider, SETTING_BRIGHTNESS, LV_ANIM_OFF);
  lv_obj_add_event_cb(brightness_slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // Test Alarm button
  lv_obj_t *alarm_btn = lv_btn_create(settings_menu);
  lv_obj_align(alarm_btn, LV_ALIGN_TOP_MID, 0, 120);
  lv_obj_add_event_cb(alarm_btn, [](lv_event_t *e) {
    startTestAlarm();
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *alarm_label = lv_label_create(alarm_btn);
  lv_label_set_text(alarm_label, "Test Alarm");

  // Time Zone changer
  lv_obj_t *tz_label = lv_label_create(settings_menu);
  lv_label_set_text_fmt(tz_label, "GMT +%d", SETTING_GMT_OFFSET);
  lv_obj_align(tz_label, LV_ALIGN_TOP_MID, 0, 170);
  lv_obj_t *tz_btn = lv_btn_create(settings_menu);
  lv_obj_align(tz_btn, LV_ALIGN_TOP_MID, 0, 200);
  lv_obj_t *tz_btn_label = lv_label_create(tz_btn);
  lv_label_set_text(tz_btn_label, "Change Time Zone");
  lv_obj_add_event_cb(tz_btn, [](lv_event_t *e) {
    // Open number keyboard for GMT offset
    static lv_obj_t *kb = NULL;
    static lv_obj_t *ta = NULL;
    if (!kb) {
      ta = lv_textarea_create(lv_scr_act());
      lv_textarea_set_one_line(ta, true);
      lv_textarea_set_text(ta, "");
      lv_obj_align(ta, LV_ALIGN_CENTER, 0, -40);
      kb = lv_keyboard_create(lv_scr_act());
      lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
      lv_keyboard_set_textarea(kb, ta);
      lv_obj_add_event_cb(kb, [](lv_event_t *e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_READY) {
          int new_offset = atoi(lv_textarea_get_text(ta));
          SETTING_GMT_OFFSET = new_offset;
          timeClient.setTimeOffset(SETTING_GMT_OFFSET * 3600);
          lv_obj_del(kb);
          lv_obj_del(ta);
          kb = NULL;
          ta = NULL;
        } else if (code == LV_EVENT_CANCEL) {
          lv_obj_del(kb);
          lv_obj_del(ta);
          kb = NULL;
          ta = NULL;
        }
      }, LV_EVENT_ALL, NULL);
    }
  }, LV_EVENT_CLICKED, NULL);

  // Change WiFi Network button
  lv_obj_t *wifi_btn = lv_btn_create(settings_menu);
  lv_obj_align(wifi_btn, LV_ALIGN_TOP_MID, 0, 250);
  lv_obj_t *wifi_btn_label = lv_label_create(wifi_btn);
  lv_label_set_text(wifi_btn_label, "Change WiFi Network");
  lv_obj_add_event_cb(wifi_btn, [](lv_event_t *e) {
    static lv_obj_t *ssid_ta = NULL;
    static lv_obj_t *pass_ta = NULL;
    static lv_obj_t *kb = NULL;
    static lv_obj_t *submit_btn = NULL;
    static lv_obj_t *cancel_btn = NULL;
    if (!kb) {
      ssid_ta = lv_textarea_create(lv_scr_act());
      lv_textarea_set_one_line(ssid_ta, true);
      lv_textarea_set_placeholder_text(ssid_ta, "Enter SSID");
      lv_obj_align(ssid_ta, LV_ALIGN_CENTER, 0, -60);
      pass_ta = lv_textarea_create(lv_scr_act());
      lv_textarea_set_one_line(pass_ta, true);
      lv_textarea_set_password_mode(pass_ta, true);
      lv_textarea_set_placeholder_text(pass_ta, "Enter Password");
      lv_obj_align(pass_ta, LV_ALIGN_CENTER, 0, -20);
      kb = lv_keyboard_create(lv_scr_act());
      lv_keyboard_set_textarea(kb, ssid_ta);
      lv_obj_align(kb, LV_ALIGN_CENTER, 0, 60);
      // Submit button
      submit_btn = lv_btn_create(lv_scr_act());
      lv_obj_set_style_bg_color(submit_btn, lv_color_hex(0x00FF00), 0); // Green
      lv_obj_align(submit_btn, LV_ALIGN_CENTER, -40, 120);
      lv_obj_t *submit_label = lv_label_create(submit_btn);
      lv_label_set_text(submit_label, "Confirm");
      lv_obj_add_event_cb(submit_btn, [](lv_event_t *e) {
        String new_ssid = lv_textarea_get_text(ssid_ta);
        String new_pass = lv_textarea_get_text(pass_ta);
        SETTING_SSID = new_ssid;
        SETTING_PASSWORD = new_pass;
        WiFi.disconnect();
        WiFi.begin(SETTING_SSID.c_str(), SETTING_PASSWORD.c_str());
        lv_obj_del(kb);
        lv_obj_del(ssid_ta);
        lv_obj_del(pass_ta);
        lv_obj_del(submit_btn);
        lv_obj_del(cancel_btn);
        kb = NULL; ssid_ta = NULL; pass_ta = NULL; submit_btn = NULL; cancel_btn = NULL;
      }, LV_EVENT_CLICKED, NULL);
      // Cancel button
      cancel_btn = lv_btn_create(lv_scr_act());
      lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x888888), 0); // Grey
      lv_obj_align(cancel_btn, LV_ALIGN_CENTER, 40, 120);
      lv_obj_t *cancel_label = lv_label_create(cancel_btn);
      lv_label_set_text(cancel_label, "Cancel");
      lv_obj_add_event_cb(cancel_btn, [](lv_event_t *e) {
        lv_obj_del(kb);
        lv_obj_del(ssid_ta);
        lv_obj_del(pass_ta);
        lv_obj_del(submit_btn);
        lv_obj_del(cancel_btn);
        kb = NULL; ssid_ta = NULL; pass_ta = NULL; submit_btn = NULL; cancel_btn = NULL;
      }, LV_EVENT_CLICKED, NULL);
    }
  }, LV_EVENT_CLICKED, NULL);

  // Close button at bottom
  close_btn = lv_btn_create(settings_menu);
  lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_event_cb(close_btn, close_btn_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *close_label = lv_label_create(close_btn);
  lv_label_set_text(close_label, "Close");
}

// --- Close Settings Menu and Show Clock Again ---
void closeSettingsMenu() {
  settings_open = false;
  lv_obj_clear_flag(clock_label, LV_OBJ_FLAG_HIDDEN); // Show clock
  lv_obj_clear_flag(settings_btn, LV_OBJ_FLAG_HIDDEN); // Show settings button

  if (settings_menu) {
    lv_obj_del(settings_menu); // Delete settings menu
    settings_menu = NULL;
    close_btn = NULL;
  }
}

// --- Event Callback: Open Settings Menu ---
void settings_btn_event_cb(lv_event_t *e) {
  showSettingsMenu();
}

// --- Event Callback: Close Settings Menu ---
void close_btn_event_cb(lv_event_t *e) {
  closeSettingsMenu();
}

// --- Event Callback: Adjust Brightness Slider ---
void brightness_slider_event_cb(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e); // Get slider object
  int value = lv_slider_get_value(slider);               // Get slider value (0-255)
  if (value < BRIGHTNESS_MIN) value = BRIGHTNESS_MIN;
  SETTING_BRIGHTNESS = value;
  analogWrite(LCD_BACKLIGHT_PIN, value);                 // Set LCD backlight brightness
}

// Implement other features using LVGL widgets for UI...