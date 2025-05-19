#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFiManager.h>  // Add this at the top


#define XPT2046_IRQ 36   // T_IRQ
#define XPT2046_MOSI 32  // T_DIN
#define XPT2046_MISO 39  // T_OUT
#define XPT2046_CLK 25   // T_CLK
#define XPT2046_CS 33    // T_CS
#define LCD_BACKLIGHT_PIN 21
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
int x, y, z;

// --- WiFi & NTP ---
const char* ssid = "VM6842809";
const char* password = "h2hFnrycpprn";
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// --- LVGL & Display ---
TFT_eSPI tft = TFT_eSPI();
uint32_t draw_buf[DRAW_BUF_SIZE / 4];
lv_obj_t *clock_label;
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

// --- Feature Flags ---
bool isAnalog = false;
int currentMenu = 0;

// --- Function Prototypes ---
void showClock();

void touchscreen_read(lv_indev_t *indev, lv_indev_data_t *data) {
  if (touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();

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

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1); // 1 = 90 degrees (landscape)
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);

  lv_init();

  // Init touchscreen
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(90);

  lv_display_t *disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchscreen_read);

  // --- Create UI ---
  clock_label = lv_label_create(lv_scr_act());
  lv_obj_align(clock_label, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(clock_label, "Connecting...");

  // --- WiFiManager section ---
  WiFiManager wifiManager;
  wifiManager.autoConnect("CYD-Clock-Setup"); // Portal SSID

  // Now connected to WiFi
  timeClient.begin();

  // You can create more LVGL objects for menus, etc.
}

void loop() {
  // handleTouchOrButton();
  showClock();
  lv_timer_handler(); // LVGL task handler
  lv_tick_inc(5);
  delay(5);
}

void showClock() {
  timeClient.update();
  char buf[32];
  snprintf(buf, sizeof(buf), "%s", timeClient.getFormattedTime().c_str());
  lv_label_set_text(clock_label, buf);
}

//