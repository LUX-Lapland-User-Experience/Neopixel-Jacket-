#include <bluefruit.h>
#include <Adafruit_NeoPixel.h>

// ---------------- Hardware ----------------
#define PIN_LEFT    A0      // Data pin, left strip
#define PIN_RIGHT   A1      // Data pin, right strip
#define NUM_PIXELS  20      // Pixels per strip
#define FRAME_MS    20      // Animation frame interval

// ---------------- Power limits ----------------
// Sized for a 5 V / 1 A power bank output feeding both strips.
#define MAX_BRIGHTNESS   100   // Hard ceiling, cannot be exceeded from the app
#define START_BRIGHTNESS 60
#define MAX_CHANNEL_SUM  400   // Per-pixel r+g+b budget (765 would be full white)

// Many power banks shut down below ~50-100 mA draw. When KEEPALIVE_LEVEL is
// non-zero, pixel 0 of each strip stays lit in "off" mode to hold the bank
// awake. Set to 0 if your bank stays on by itself.
#define KEEPALIVE_LEVEL  120

Adafruit_NeoPixel stripLeft (NUM_PIXELS, PIN_LEFT,  NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripRight(NUM_PIXELS, PIN_RIGHT, NEO_GRB + NEO_KHZ800);

// ---------------- BLE ----------------
BLEDis bledis;
BLEUart bleuart;

// ---------------- State ----------------
enum Mode { MODE_SOLID, MODE_RAINBOW, MODE_CHASE, MODE_BREATHE, MODE_OFF, MODE_COUNT };

struct Channel {
  Adafruit_NeoPixel* strip;
  uint8_t  r, g, b;
  uint8_t  mode;
  uint16_t step;
};

Channel channels[2] = {
  { &stripLeft,  0, 0, 255, MODE_SOLID, 0 },   // 0 = left
  { &stripRight, 0, 0, 255, MODE_SOLID, 0 }    // 1 = right
};

uint8_t  target     = 0x03;   // bit0 = left, bit1 = right
uint8_t  brightness = START_BRIGHTNESS;
uint32_t lastFrame  = 0;

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);

  stripLeft.begin();
  stripRight.begin();
  stripLeft.setBrightness(brightness);
  stripRight.setBrightness(brightness);
  stripLeft.clear();  stripLeft.show();
  stripRight.clear(); stripRight.show();

  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName("Feather LEDs");
  Bluefruit.Periph.setConnectCallback(connectCallback);
  Bluefruit.Periph.setDisconnectCallback(disconnectCallback);

  bledis.setManufacturer("Adafruit Industries");
  bledis.setModel("Bluefruit Feather nRF52832");
  bledis.begin();

  bleuart.begin();

  startAdvertising();
}

void startAdvertising() {
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

void connectCallback(uint16_t handle) {
  (void) handle;
  bleuart.println("Connected. Controller -> Color Picker / Control Pad");
}

void disconnectCallback(uint16_t handle, uint8_t reason) {
  (void) handle; (void) reason;
}

// ---------------- Main loop ----------------
void loop() {
  readBle();

  uint32_t now = millis();
  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    renderChannel(channels[0]);
    renderChannel(channels[1]);
  }
}

// ---------------- Controller packet parsing ----------------
// Bluefruit Connect Controller sends: '!' + type + payload + checksum
uint8_t packetBuffer[24];
uint8_t packetLen   = 0;
uint8_t expectedLen = 0;

uint8_t packetLengthFor(char type) {
  switch (type) {
    case 'B': return 5;                          // Button
    case 'C': return 6;                          // Color picker
    case 'A': case 'G': case 'M': case 'L': return 15;
    case 'Q': return 19;
    default:  return 0;
  }
}

bool checksumOk(const uint8_t* buf, uint8_t len) {
  uint8_t sum = 0;
  for (uint8_t i = 0; i < len - 1; i++) sum += buf[i];
  return (uint8_t)(~sum) == buf[len - 1];
}

void readBle() {
  while (bleuart.available()) {
    uint8_t c = bleuart.read();

    if (packetLen == 0) {
      if (c == '!') packetBuffer[packetLen++] = c;
      else          handleTextByte(c);
      continue;
    }

    if (packetLen == 1) {
      expectedLen = packetLengthFor((char) c);
      if (expectedLen == 0) { packetLen = 0; continue; }   // Unknown type, resync
      packetBuffer[packetLen++] = c;
      continue;
    }

    packetBuffer[packetLen++] = c;
    if (packetLen >= expectedLen) {
      if (checksumOk(packetBuffer, expectedLen)) handlePacket(packetBuffer);
      packetLen = 0;
    }
  }
}

void handlePacket(const uint8_t* buf) {
  if (buf[1] == 'C') {                    // Color picker
    setColor(buf[2], buf[3], buf[4]);
    return;
  }

  if (buf[1] == 'B') {                    // Control pad button
    char button  = (char) buf[2];
    bool pressed = (buf[3] == '1');
    if (!pressed) return;                 // Act on press only

    switch (button) {
      case '1': target = 0x01; report("Target: left");  break;
      case '2': target = 0x02; report("Target: right"); break;
      case '3': target = 0x03; report("Target: both");  break;
      case '4': toggleOff();                            break;
      case '5': changeBrightness(+20);                  break;   // Up
      case '6': changeBrightness(-20);                  break;   // Down
      case '7': cycleMode(-1);                          break;   // Left
      case '8': cycleMode(+1);                          break;   // Right
    }
  }
}

// ---------------- Text commands (UART module) ----------------
// Examples: "l 255 0 0", "r 0 0 255", "a 0 255 0", "b 80", "m 1"
char    textBuffer[32];
uint8_t textLen = 0;

void handleTextByte(uint8_t c) {
  if (c == '\r') return;
  if (c == '\n') {
    textBuffer[textLen] = '\0';
    if (textLen > 0) handleTextCommand(textBuffer);
    textLen = 0;
    return;
  }
  if (textLen < sizeof(textBuffer) - 1) textBuffer[textLen++] = (char) c;
}

void handleTextCommand(char* line) {
  char cmd = tolower(line[0]);
  int  a = 0, b = 0, c = 0;
  int  n = sscanf(line + 1, "%d %d %d", &a, &b, &c);

  switch (cmd) {
    case 'l': target = 0x01; if (n >= 3) setColor(a, b, c); break;
    case 'r': target = 0x02; if (n >= 3) setColor(a, b, c); break;
    case 'a': target = 0x03; if (n >= 3) setColor(a, b, c); break;
    case 'b': if (n >= 1) { brightness = constrain(a, 1, MAX_BRIGHTNESS); applyBrightness(); } break;
    case 'm': if (n >= 1) setMode(constrain(a, 0, MODE_COUNT - 1)); break;
    default:  report("Commands: l/r/a R G B, b <1-100>, m <0-4>"); break;
  }
}

// ---------------- Actions ----------------
// Scale a colour down so no single pixel exceeds the current budget
void limitColor(uint8_t& r, uint8_t& g, uint8_t& b) {
  uint16_t sum = (uint16_t) r + g + b;
  if (sum <= MAX_CHANNEL_SUM) return;
  r = (uint32_t) r * MAX_CHANNEL_SUM / sum;
  g = (uint32_t) g * MAX_CHANNEL_SUM / sum;
  b = (uint32_t) b * MAX_CHANNEL_SUM / sum;
}

void setColor(uint8_t r, uint8_t g, uint8_t b) {
  limitColor(r, g, b);

  for (uint8_t i = 0; i < 2; i++) {
    if (!(target & (1 << i))) continue;
    channels[i].r = r;
    channels[i].g = g;
    channels[i].b = b;
    // A picked colour should be visible immediately
    if (channels[i].mode == MODE_OFF || channels[i].mode == MODE_RAINBOW)
      channels[i].mode = MODE_SOLID;
  }
}

void setMode(uint8_t mode) {
  for (uint8_t i = 0; i < 2; i++) {
    if (!(target & (1 << i))) continue;
    channels[i].mode = mode;
    channels[i].step = 0;
  }
  report("Mode set");
}

void cycleMode(int8_t delta) {
  for (uint8_t i = 0; i < 2; i++) {
    if (!(target & (1 << i))) continue;
    channels[i].mode = (channels[i].mode + MODE_COUNT + delta) % MODE_COUNT;
    channels[i].step = 0;
  }
  report("Mode changed");
}

void toggleOff() {
  for (uint8_t i = 0; i < 2; i++) {
    if (!(target & (1 << i))) continue;
    channels[i].mode = (channels[i].mode == MODE_OFF) ? MODE_SOLID : MODE_OFF;
  }
}

void changeBrightness(int16_t delta) {
  brightness = constrain((int16_t) brightness + delta, 1, MAX_BRIGHTNESS);
  applyBrightness();
}

void applyBrightness() {
  stripLeft.setBrightness(brightness);
  stripRight.setBrightness(brightness);
  char msg[24];
  snprintf(msg, sizeof(msg), "Brightness %u", brightness);
  report(msg);
}

void report(const char* msg) {
  bleuart.println(msg);
  Serial.println(msg);
}

// ---------------- Rendering ----------------
void renderChannel(Channel& ch) {
  Adafruit_NeoPixel& s = *ch.strip;

  switch (ch.mode) {
    case MODE_OFF:
      s.clear();
      if (KEEPALIVE_LEVEL > 0) {
        // Hold a minimum load so the power bank does not switch itself off
        s.setPixelColor(0, s.Color(KEEPALIVE_LEVEL, KEEPALIVE_LEVEL, KEEPALIVE_LEVEL));
      }
      break;

    case MODE_SOLID:
      s.fill(s.Color(ch.r, ch.g, ch.b));
      break;

    case MODE_RAINBOW:
      for (uint16_t i = 0; i < NUM_PIXELS; i++) {
        uint16_t hue = (uint16_t)((uint32_t) ch.step * 256
                     + (uint32_t) i * 65536UL / NUM_PIXELS);
        s.setPixelColor(i, s.gamma32(s.ColorHSV(hue)));
      }
      ch.step += 2;
      break;

    case MODE_CHASE: {
      s.clear();
      uint16_t head = (ch.step / 4) % NUM_PIXELS;
      for (uint8_t t = 0; t < 6; t++) {
        int16_t idx = (int16_t) head - t;
        if (idx < 0) idx += NUM_PIXELS;
        uint8_t f = 255 >> t;                       // Fading tail
        s.setPixelColor(idx, s.Color((ch.r * f) / 255,
                                     (ch.g * f) / 255,
                                     (ch.b * f) / 255));
      }
      ch.step++;
      break;
    }

    case MODE_BREATHE: {
      float   phase = (ch.step % 200) / 200.0f;
      float   f     = (1.0f - cosf(phase * 2.0f * PI)) / 2.0f;
      uint8_t level = 8 + (uint8_t)(f * 247.0f);
      s.fill(s.Color((ch.r * level) / 255,
                     (ch.g * level) / 255,
                     (ch.b * level) / 255));
      ch.step++;
      break;
    }
  }

  s.show();
}
