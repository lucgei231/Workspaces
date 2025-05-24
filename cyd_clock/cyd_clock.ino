namespace fs { class FS; }
using FS = fs::FS;

#include <Arduino.h>
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
#define SPK_PIN 26 // Speaker pin (change as needed)
#define ALARM_BTN_PIN 0 // GPIO 0 for alarm stop

// --- UI Sizing Constants ---
const int btn_width = 180;
const int btn_height = 50;

// --- Timer Feature Globals ---
lv_obj_t *timer_btn = NULL;
lv_obj_t *timer_menu = NULL;
lv_obj_t *timer_label = NULL;
lv_obj_t *timer_remain_label = NULL;
unsigned long timer_end_millis = 0;
bool timer_active = false;
bool timer_popup_active = false;

// Add these as file-scope globals so all functions can access them
static bool alarm_pending = false;
static unsigned long popup_shown_time = 0;
static unsigned long last_alarm_time = 0;
static bool alarm_playing = false;

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

// --- Add this helper at the top (before showSettingsMenu or as a static function) ---
static void textarea_focus_cb(lv_event_t *e) {
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
    lv_keyboard_set_textarea(kb, (lv_obj_t *)lv_event_get_target(e));
}

static void wifi_submit_cb(lv_event_t *e) {
    lv_obj_t *win = (lv_obj_t *)lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e));
    lv_obj_t *ssid_ta = (lv_obj_t *)lv_obj_get_child(win, 1);
    lv_obj_t *pass_ta = (lv_obj_t *)lv_obj_get_child(win, 2);
    String new_ssid = lv_textarea_get_text(ssid_ta);
    String new_pass = lv_textarea_get_text(pass_ta);
    SETTING_SSID = new_ssid;
    SETTING_PASSWORD = new_pass;
    WiFi.disconnect();
    WiFi.begin(SETTING_SSID.c_str(), SETTING_PASSWORD.c_str());
    lv_obj_del(win);
}

static void wifi_cancel_cb(lv_event_t *e) {
    lv_obj_t *win = (lv_obj_t *)lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e));
    lv_obj_del(win);
}

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
    pinMode(ALARM_BTN_PIN, INPUT_PULLUP);
    pinMode(SPK_PIN, OUTPUT);
    attachInterrupt(digitalPinToInterrupt(ALARM_BTN_PIN), stopAlarmISR, FALLING);

    while (alarm_active) {
        // Play tone for 0.2s, then off for 0.2s
        tone(SPK_PIN, 2000); // 2kHz tone
        delay(200);
        noTone(SPK_PIN);
        delay(200);
        server.handleClient(); // Allow webserver to process stop requests
        lv_timer_handler();    // Keep UI responsive
    }

    noTone(SPK_PIN);
    detachInterrupt(digitalPinToInterrupt(ALARM_BTN_PIN));
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

// --- WiFi Auto-Reconnect Logic ---
void tryConnectWiFi() {
  WiFi.disconnect();
  WiFi.begin(SETTING_SSID.c_str(), SETTING_PASSWORD.c_str());
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    // Fallback to default credentials
    WiFi.disconnect();
    WiFi.begin(ssid, password);
    startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
      delay(100);
    }
  }
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
  // WiFiManager wifiManager;
  // wifiManager.autoConnect("CYD-Clock-Setup");
  tryConnectWiFi();
  // Now connected to WiFi, start NTP
  timeClient.begin();
  timeClient.update();
  currentEpoch = timeClient.getEpochTime();
  lastMillis = millis();
  lastNtpSync = millis();

  setupWebServer();

  // --- Timer Feature ---
  lv_obj_t *timer_btn = lv_btn_create(lv_scr_act());
  lv_obj_align(timer_btn, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_set_size(timer_btn, 60, 40);
  lv_obj_t *timer_btn_label = lv_label_create(timer_btn);
  lv_label_set_text(timer_btn_label, "Timer");
  lv_obj_set_style_text_line_space(timer_btn_label, 0, 0); // Remove extra line spacing
  lv_obj_set_style_text_align(timer_btn_label, LV_TEXT_ALIGN_CENTER, 0); // Ensure center alignment
  lv_obj_add_event_cb(timer_btn, [](lv_event_t *e) { showTimerMenu(); }, LV_EVENT_CLICKED, NULL);
  // Timer remaining label at bottom
  lv_obj_t *timer_remain_label = lv_label_create(lv_scr_act());
  lv_obj_align(timer_remain_label, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_label_set_text(timer_remain_label, "");
}

void loop() {
  // Main loop: update time, UI, and handle LVGL tasks
  updateClockTime();
  showClock();
  lv_timer_handler();
  lv_tick_inc(5);
  delay(5);

  server.handleClient();

  // --- Timer Update Logic ---
  if (timer_active) {
    long remain = (long)(timer_end_millis - millis());
    if (remain > 0) {
      int sec = (remain / 1000) % 60;
      int min = (remain / 60000) % 60;
      int hr = (remain / 3600000);
      char tbuf[32];
      snprintf(tbuf, sizeof(tbuf), "Timer: %02d:%02d:%02d", hr, min, sec);
      lv_label_set_text(timer_remain_label, tbuf);
    } else {
      timer_active = false;
      lv_label_set_text(timer_remain_label, "");
      showTimerPopup();
      // Do not start alarm here
    }
  }

  // --- Handle TIME UP popup and alarm ---
  static bool alarm_pending = false;
  static unsigned long popup_shown_time = 0;
  static unsigned long last_alarm_time = 0;
  static bool alarm_playing = false;

  // If popup is active, record when it was shown
  if (timer_popup_active && popup_shown_time == 0) {
    popup_shown_time = millis();
    alarm_pending = true;
    last_alarm_time = millis();
    alarm_playing = false;
  }

  // Play alarm 0.5s after popup is shown, without closing popup
  if (alarm_pending && timer_popup_active && !alarm_playing && millis() - last_alarm_time >= 500) {
    alarm_pending = false;
    alarm_playing = true;
    startTestAlarm();
    alarm_playing = false;
  }

  // If popup just closed (by GPIO 0 or programmatically), clean up
  if (!timer_popup_active && popup_shown_time != 0) {
    popup_shown_time = 0;
    detachInterrupt(digitalPinToInterrupt(ALARM_BTN_PIN));
    alarm_pending = false;
    alarm_playing = false;
    // Remove popup if still present
    // (handled by close_timer_popup_cb or GPIO ISR)
  }
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

// --- Show Fullscreen Settings Menu (LVGL 9.x compatible, improved layout) ---
void showSettingsMenu() {
  settings_open = true;
  lv_obj_add_flag(clock_label, LV_OBJ_FLAG_HIDDEN); // Hide clock
  lv_obj_add_flag(settings_btn, LV_OBJ_FLAG_HIDDEN); // Hide settings button

  // Create main container (fullscreen)
  settings_menu = lv_obj_create(lv_scr_act());
  lv_obj_set_size(settings_menu, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_align(settings_menu, LV_ALIGN_CENTER, 0, 0);
  lv_obj_clear_flag(settings_menu, LV_OBJ_FLAG_SCROLLABLE); // Main container not scrollable

  // Create a scrollable child for settings content
  lv_obj_t *settings_scroll = lv_obj_create(settings_menu);
  lv_obj_set_size(settings_scroll, SCREEN_WIDTH, SCREEN_HEIGHT - 80); // Leave space for close button
  lv_obj_align(settings_scroll, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_scroll_dir(settings_scroll, LV_DIR_VER);
  lv_obj_set_scroll_snap_y(settings_scroll, LV_SCROLL_SNAP_CENTER);
  lv_obj_set_style_pad_row(settings_scroll, 12, 0);
  lv_obj_set_style_pad_top(settings_scroll, 12, 0);
  lv_obj_set_style_pad_bottom(settings_scroll, 12, 0);
  lv_obj_set_style_bg_opa(settings_scroll, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(settings_scroll, LV_OPA_TRANSP, 0);

  // Use a vertical flex layout for settings_scroll
  lv_obj_set_flex_flow(settings_scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(settings_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Brightness slider label
  lv_obj_t *slider_label = lv_label_create(settings_scroll);
  lv_label_set_text(slider_label, "Brightness");

  // Brightness slider
  lv_obj_t *brightness_slider = lv_slider_create(settings_scroll);
  lv_obj_set_width(brightness_slider, btn_width);
  lv_slider_set_range(brightness_slider, BRIGHTNESS_MIN, 255);
  lv_slider_set_value(brightness_slider, SETTING_BRIGHTNESS, LV_ANIM_OFF);
  lv_obj_add_event_cb(brightness_slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // Test Alarm button
  lv_obj_t *alarm_btn = lv_btn_create(settings_scroll);
  lv_obj_set_size(alarm_btn, btn_width, btn_height);
  lv_obj_add_event_cb(alarm_btn, [](lv_event_t *e) { startTestAlarm(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *alarm_label = lv_label_create(alarm_btn);
  lv_label_set_text(alarm_label, "Test Alarm");

  // Time Zone changer
  lv_obj_t *tz_label = lv_label_create(settings_scroll);
  lv_label_set_text_fmt(tz_label, "GMT +%d", SETTING_GMT_OFFSET);
  lv_obj_t *tz_btn = lv_btn_create(settings_scroll);
  lv_obj_set_size(tz_btn, btn_width, btn_height);
  lv_obj_t *tz_btn_label = lv_label_create(tz_btn);
  lv_label_set_text(tz_btn_label, "Change Time Zone");
  lv_obj_add_event_cb(tz_btn, [](lv_event_t *e) {
    // Capture tz_label from parent scope
    lv_obj_t *settings_scroll = lv_obj_get_parent((const lv_obj_t *)lv_event_get_target(e));
    lv_obj_t *tz_label = lv_obj_get_child(settings_scroll, 3); // 3rd child is tz_label
    lv_obj_t *win = lv_win_create(lv_scr_act());
    lv_win_add_title(win, "Set Time Zone");
    lv_obj_t *ta = lv_textarea_create(win);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "Enter GMT offset (e.g. 1)");
    lv_obj_set_width(ta, 120);
    lv_obj_t *kb = lv_keyboard_create(win);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);
    // Pass tz_label as user data to the keyboard event callback
    lv_obj_add_event_cb(kb, [](lv_event_t *e) {
      lv_event_code_t code = lv_event_get_code(e);
      lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);
      lv_obj_t *win = (lv_obj_t *)lv_obj_get_parent(kb);
      lv_obj_t *ta = (lv_obj_t *)lv_keyboard_get_textarea(kb);
      lv_obj_t *tz_label = (lv_obj_t *)lv_event_get_user_data(e);
      if (code == LV_EVENT_READY) {
        const char* text = lv_textarea_get_text(ta);
        if (text && strlen(text) > 0) {
          int new_offset = atoi(text);
          SETTING_GMT_OFFSET = new_offset;
          timeClient.setTimeOffset(SETTING_GMT_OFFSET * 3600);
          if (tz_label) {
            lv_label_set_text_fmt(tz_label, "GMT +%d", SETTING_GMT_OFFSET);
          }
        }
        lv_obj_del(win);
      } else if (code == LV_EVENT_CANCEL) {
        lv_obj_del(win);
      }
    }, LV_EVENT_ALL, tz_label);
  }, LV_EVENT_CLICKED, NULL);

  // Change WiFi Network button
  lv_obj_t *wifi_btn = lv_btn_create(settings_scroll);
  lv_obj_set_size(wifi_btn, btn_width, btn_height);
  lv_obj_t *wifi_btn_label = lv_label_create(wifi_btn);
  lv_label_set_text(wifi_btn_label, "Change WiFi Network");
  lv_obj_add_event_cb(wifi_btn, [](lv_event_t *e) {
    lv_obj_t *win = lv_win_create(lv_scr_act());
    lv_win_add_title(win, "WiFi Setup");
    lv_obj_t *ssid_ta = lv_textarea_create(win);
    lv_textarea_set_one_line(ssid_ta, true);
    lv_textarea_set_placeholder_text(ssid_ta, "Enter SSID");
    lv_obj_set_width(ssid_ta, 120);
    lv_obj_t *pass_ta = lv_textarea_create(win);
    lv_textarea_set_one_line(pass_ta, true);
    lv_textarea_set_password_mode(pass_ta, true);
    lv_textarea_set_placeholder_text(pass_ta, "Enter Password");
    lv_obj_set_width(pass_ta, 120);
    lv_obj_t *kb = lv_keyboard_create(win);
    lv_keyboard_set_textarea(kb, ssid_ta);
    lv_obj_add_event_cb(ssid_ta, textarea_focus_cb, LV_EVENT_FOCUSED, kb);
    lv_obj_add_event_cb(pass_ta, textarea_focus_cb, LV_EVENT_FOCUSED, kb);
    lv_obj_t *submit_btn = lv_btn_create(win);
    lv_obj_set_style_bg_color(submit_btn, lv_color_hex(0x00FF00), 0);
    lv_obj_set_size(submit_btn, 80, 40);
    lv_obj_t *submit_label = lv_label_create(submit_btn);
    lv_label_set_text(submit_label, "Confirm");
    lv_obj_add_event_cb(submit_btn, wifi_submit_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_btn = lv_btn_create(win);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x888888), 0);
    lv_obj_set_size(cancel_btn, 80, 40);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_add_event_cb(cancel_btn, wifi_cancel_cb, LV_EVENT_CLICKED, NULL);
  }, LV_EVENT_CLICKED, NULL);

  // Close button at bottom (fixed, not scrollable)
  close_btn = lv_btn_create(settings_menu);
  lv_obj_set_size(close_btn, SCREEN_WIDTH - 40, 60);
  lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_add_event_cb(close_btn, close_btn_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *close_label = lv_label_create(close_btn);
  lv_obj_set_style_text_font(close_label, &lv_font_montserrat_28, 0);
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

// --- Timer Feature ---
void close_timer_menu_cb(lv_event_t *e) {
    if (timer_menu) {
        lv_obj_del(timer_menu);
        timer_menu = NULL;
    }
}

void close_timer_popup_cb(lv_event_t *e) {
    if (timer_popup_active) {
        lv_obj_t *popup = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_del(popup);
        timer_popup_active = false;
    }
}

// Helper to close TIME UP popup from GPIO 0
void IRAM_ATTR close_timeup_popup_isr() {
    timer_popup_active = false;
}

// --- Show Timer Popup (no X button, closes on GPIO 0) ---
void showTimerPopup() {
    timer_popup_active = true;
    lv_obj_t *popup = lv_obj_create(lv_scr_act());
    lv_obj_set_size(popup, SCREEN_WIDTH - 40, 100);
    lv_obj_align(popup, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *label = lv_label_create(popup);
    lv_label_set_text(label, "TIME UP!");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    // No X button
    pinMode(ALARM_BTN_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ALARM_BTN_PIN), close_timeup_popup_isr, FALLING);
    lv_obj_set_user_data(popup, popup);
}

// --- Remove X button from set timer window ---
void showTimerMenu() {
    if (timer_menu) return;
    timer_menu = lv_obj_create(lv_scr_act());
    lv_obj_set_size(timer_menu, SCREEN_WIDTH - 40, SCREEN_HEIGHT - 80);
    lv_obj_align(timer_menu, LV_ALIGN_CENTER, 0, 0);
    // No close (X) button here

    // --- Keyboard for numeric entry (half screen at bottom) ---
    lv_coord_t kb_height = SCREEN_HEIGHT / 2;
    lv_obj_t *kb = lv_keyboard_create(lv_scr_act());
    lv_obj_set_size(kb, SCREEN_WIDTH, kb_height);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);

    // Hours
    lv_obj_t *ta_h = lv_textarea_create(timer_menu);
    lv_textarea_set_one_line(ta_h, true);
    lv_textarea_set_placeholder_text(ta_h, "Hours");
    lv_obj_set_width(ta_h, 80);
    lv_obj_align(ta_h, LV_ALIGN_TOP_MID, -60, 60);
    lv_obj_add_event_cb(ta_h, textarea_focus_cb, LV_EVENT_FOCUSED, kb);

    // Minutes
    lv_obj_t *ta_m = lv_textarea_create(timer_menu);
    lv_textarea_set_one_line(ta_m, true);
    lv_textarea_set_placeholder_text(ta_m, "Minutes");
    lv_obj_set_width(ta_m, 80);
    lv_obj_align(ta_m, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_event_cb(ta_m, textarea_focus_cb, LV_EVENT_FOCUSED, kb);

    // Seconds
    lv_obj_t *ta_s = lv_textarea_create(timer_menu);
    lv_textarea_set_one_line(ta_s, true);
    lv_textarea_set_placeholder_text(ta_s, "Seconds");
    lv_obj_set_width(ta_s, 80);
    lv_obj_align(ta_s, LV_ALIGN_TOP_MID, 60, 60);
    lv_obj_add_event_cb(ta_s, textarea_focus_cb, LV_EVENT_FOCUSED, kb);

    // Set initial textarea for keyboard
    lv_keyboard_set_textarea(kb, ta_h);

    // Submit
    lv_obj_t *submit_btn = lv_btn_create(timer_menu);
    lv_obj_set_size(submit_btn, 120, 40);
    lv_obj_align(submit_btn, LV_ALIGN_BOTTOM_MID, 0, -kb_height - 10);
    lv_obj_t *submit_label = lv_label_create(submit_btn);
    lv_label_set_text(submit_label, "Submit");
    lv_obj_add_event_cb(submit_btn, timer_submit_cb, LV_EVENT_CLICKED, NULL);
}

// --- Timer Submit Callback (unchanged) ---
void timer_submit_cb(lv_event_t *e) {
    lv_obj_t *menu = timer_menu;
    if (!menu) return;
    lv_obj_t *ta_h = lv_obj_get_child(menu, 1);
    lv_obj_t *ta_m = lv_obj_get_child(menu, 2);
    lv_obj_t *ta_s = lv_obj_get_child(menu, 3);
    int h = atoi(lv_textarea_get_text(ta_h));
    int m = atoi(lv_textarea_get_text(ta_m));
    int s = atoi(lv_textarea_get_text(ta_s));
    unsigned long total_ms = (h * 3600UL + m * 60UL + s) * 1000UL;
    if (total_ms > 0) {
        timer_end_millis = millis() + total_ms;
        timer_active = true;
        if (!timer_popup_active) {
            alarm_pending = false;
            popup_shown_time = 0;
            last_alarm_time = 0;
            alarm_playing = false;
        }
        // Defensive: delete the old label if it exists and is valid, then create a new one
        if (timer_remain_label && lv_obj_is_valid(timer_remain_label)) {
            lv_obj_del(timer_remain_label);
            timer_remain_label = NULL;
        }
        timer_remain_label = lv_label_create(lv_scr_act());
        lv_obj_align(timer_remain_label, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_label_set_text(timer_remain_label, "Timer: 00:00:00");
    }
    // Delete the keyboard if it exists (search for any keyboard on the screen)
    lv_obj_t *child = NULL;
    uint32_t child_cnt = lv_obj_get_child_cnt(lv_scr_act());
    for (uint32_t i = 0; i < child_cnt; ++i) {
        child = lv_obj_get_child(lv_scr_act(), i);
        if (child && lv_obj_check_type(child, &lv_keyboard_class)) {
            lv_obj_del(child);
            break;
        }
    }
    if (timer_menu) {
        lv_obj_del(timer_menu);
        timer_menu = NULL;
    }
}
