#include <FastLED.h>
#include "dotfont5x7.h"   // verified GLCD 5x7 font (column-encoded, ASCII-indexed)
#include "index_html.h"   // web control page (kept out of the .ino prototype scanner)

#include <WiFi.h>
#include <WiFiManager.h>          // tzapu/WiFiManager  (captive-portal WiFi setup)
#include <ESPmDNS.h>              // built-in           (wallart.local)
#include <WebServer.h>           // built-in synchronous web server (no extra library)
#include <time.h>                // struct tm / NTP time helpers
#include <Preferences.h>         // built-in NVS key-value storage (settings persistence)
#include <ArduinoOTA.h>          // built-in (wireless firmware updates)

// ---------------- USER CONFIG ----------------
#define RESET_BTN_PIN 0           // onboard BOOT button: HOLD 3s while running to wipe WiFi.
                                  // Change to a free GPIO if you wire an external button.
#define AP_PASSWORD   "wallart1"  // setup-hotspot password (WPA2 needs 8+ chars; "wallart" alone is too short)
#define OTA_PASSWORD  "wallart1"  // password required to upload firmware over WiFi (OTA)

#define NUM_ROWS      10
#define LEDS_PER_ROW  20

CRGB leds[NUM_ROWS][LEDS_PER_ROW];

// ---------------- TUNABLES ----------------
uint8_t  MAX_BRIGHTNESS = 240;   // peak each cell fades UP to (0-255)
uint8_t  MIN_BRIGHTNESS = 80;    // rainbow trough floor (random pattern ignores this)
uint16_t FADE_IN_MS     = 500;   // time to fade in
uint16_t FADE_OUT_MS    = 500;   // time to fade out
uint8_t  TIME_JITTER    = 12;    // % per-cell fade variation (keep low so rainbow bands stay in phase)
const uint16_t FRAME_MS = 20;    // update interval (~50 fps)

uint8_t  COLOR_SAT      = 255;   // 255 = vivid; lower washes toward white
uint8_t  gHueOffset     = 0;     // scroll position of the rainbow
uint8_t  RAINBOW_SPEED  = 2;     // hue shift per frame (0 = frozen)
uint8_t  RAINBOW_BRIGHT = 120;   // rainbow brightness, no fade (turn up/down to taste)

uint8_t  MASTER_BRIGHTNESS = 255;  // overall brightness cap (web slider)
uint8_t  WEB_SPEED         = 2;    // unified 1..10 speed slider (initial position; live via /set)
uint8_t  WEB_GLOW          = 5;    // generic 1..10 "glow/softness" slider (each pattern interprets it)
uint8_t  WEB_FREQ          = 5;    // generic 1..10 "frequency/density" slider (each pattern interprets it)

// ------------- PER-CELL FADE STATE -------------
uint8_t cellHue  [NUM_ROWS][LEDS_PER_ROW];  // color (used by random pattern)
uint8_t cellLevel[NUM_ROWS][LEDS_PER_ROW];  // current brightness 0..MAX_BRIGHTNESS
int16_t cellStep [NUM_ROWS][LEDS_PER_ROW];  // +fading in / -fading out

uint32_t lastFrame = 0;

// ---------------- FADE HELPERS ----------------
int16_t stepForMs(uint16_t ms) {
  uint16_t frames = ms / FRAME_MS;
  if (frames < 1) frames = 1;
  int16_t s = MAX_BRIGHTNESS / frames;
  if (s < 1) s = 1;
  return s;
}

uint16_t jitter(uint16_t base) {
  if (TIME_JITTER == 0) return base;
  uint8_t  pct = TIME_JITTER > 100 ? 100 : TIME_JITTER;
  uint16_t d   = (uint32_t)base * pct / 100;
  uint16_t lo  = base - d;
  if (lo < FRAME_MS) lo = FRAME_MS;          // never collapse below one frame
  return random16(lo, base + d + 1);
}

// Advance one cell's fade. Returns true if it just bottomed out (restarted).
bool advanceCell(int r, int c) {
  int level = cellLevel[r][c] + cellStep[r][c];
  bool restarted = false;
  if (level >= MAX_BRIGHTNESS) {
    level = MAX_BRIGHTNESS;
    cellStep[r][c] = -stepForMs(jitter(FADE_OUT_MS));
  } else if (level <= 0) {
    level = 0;
    cellStep[r][c] = stepForMs(jitter(FADE_IN_MS));
    restarted = true;
  }
  cellLevel[r][c] = level;
  return restarted;
}

// ========================= XY MAPPING =========================
// The ONE place that maps a logical pixel (x = column left->right,
// y = row top->bottom) to its physical LED. Fix orientation here. 
bool TEXT_FLIP_X     = true;    // mirror left/right
bool TEXT_FLIP_Y     = false;   // mirror top/bottom
bool TEXT_SERPENTINE = false;   // true if alternate rows are wired in reverse

CRGB& XY(int x, int y) {
  static CRGB junk;                                  // safe sink for off-panel writes
  if (x < 0 || x >= LEDS_PER_ROW || y < 0 || y >= NUM_ROWS) return junk;
  int row = TEXT_FLIP_Y ? (NUM_ROWS - 1 - y) : y;
  int col = TEXT_FLIP_X ? (LEDS_PER_ROW - 1 - x) : x;
  if (TEXT_SERPENTINE && (row & 1)) col = (LEDS_PER_ROW - 1) - col;
  return leds[row][col];
}

// ============ PATTERN FUNCTIONS (each renders one full frame) ============

// Random twinkle: each cell fades fully out, picks a new random color, fades in.
void patternRandomFade() {
  for (int r = 0; r < NUM_ROWS; r++) {
    for (int c = 0; c < LEDS_PER_ROW; c++) {
      if (advanceCell(r, c)) cellHue[r][c] = random8();   // new color on restart
      leds[r][c] = CHSV(cellHue[r][c], COLOR_SAT, cellLevel[r][c]);
    }
  }
}

// "Compute": Burroughs-B205-style console -- a DENSE grid of white lamps (~60%
// lit) that continuously flicker on/off with a quick incandescent fade, like the
// "computer" panel in old movies/TV. Tunables: DENSITY (of 255), flips-per-frame,
// and the +30/-30 fade step.
#define COMPUTE_DENSITY 150     // ~59% of lamps lit
bool computeOn[NUM_ROWS][LEDS_PER_ROW];
bool computeInit = false;

void patternCompute() {
  if (!computeInit) {                                  // seed the grid
    for (int r = 0; r < NUM_ROWS; r++)
      for (int c = 0; c < LEDS_PER_ROW; c++) computeOn[r][c] = (random8() < COMPUTE_DENSITY);
    computeInit = true;
  }
  for (int n = 0; n < 8; n++)                          // flip a few lamps each frame -> shifting data
    computeOn[random8(NUM_ROWS)][random8(LEDS_PER_ROW)] = (random8() < COMPUTE_DENSITY);

  for (int r = 0; r < NUM_ROWS; r++) {
    for (int c = 0; c < LEDS_PER_ROW; c++) {
      if (computeOn[r][c]) { int v = cellLevel[r][c] + 30; cellLevel[r][c] = (v > MAX_BRIGHTNESS) ? MAX_BRIGHTNESS : v; }
      else cellLevel[r][c] = (cellLevel[r][c] > 30) ? cellLevel[r][c] - 30 : 0;
      leds[r][c] = CHSV(0, 0, cellLevel[r][c]);        // white
    }
  }
}

// Diagonal rainbow (top-right -> bottom-left), scrolling at constant brightness.
// No fading. Hue matches the original VB formula: hue ~ (row + col).
void patternRainbowFade() {
  gHueOffset += RAINBOW_SPEED;                            // scroll lives in the pattern
  for (int r = 0; r < NUM_ROWS; r++) {
    for (int c = 0; c < LEDS_PER_ROW; c++) {
      uint8_t hue = gHueOffset + (uint16_t)(r + c) * 256 / (NUM_ROWS + LEDS_PER_ROW);
      leds[r][c]  = CHSV(hue, COLOR_SAT, RAINBOW_BRIGHT);
    }
  }
}

// ========================= SCROLLING TEXT =========================
#define FONT_W 5
#define FONT_H 7

// --- text tunables ---
char     TEXT_MSG[256]  = "70 YEARS OLD    AND YET     A KID AT HEART     ";  // mutable (web-settable), up to 255 chars
uint8_t  TEXT_HUE       = 150;   // solid text hue (0-255); ignored if TEXT_RAINBOW
uint8_t  TEXT_SAT       = 255;   // 255 = vivid color, 0 = white
uint8_t  TEXT_BRIGHT    = 140;   // text brightness (applies to solid AND rainbow)
bool     TEXT_RAINBOW   = false; // true = letters ride a per-column rainbow
uint16_t TEXT_SCROLL_MS = 110;    // ms per 1-pixel step (lower = faster)
int      TEXT_Y         = (NUM_ROWS - FONT_H) / 2;   // vertical centering (=1 on 10 rows)

// Font: verified GLCD 5x7 table from dotfont5x7.h (included at top).
// Column-encoded: 5 bytes/char, one byte per column, bit 0 = top row, indexed
// directly by ASCII code. Covers uppercase, lowercase, digits, and punctuation.

int textWidthCols() { return (int)strlen(TEXT_MSG) * (FONT_W + 1); }

// is virtual text-column vx, glyph-row gy lit?
bool textPixel(int vx, int gy) {
  if (gy < 0 || gy >= FONT_H || vx < 0) return false;
  int charIndex = vx / (FONT_W + 1);
  int colInChar = vx % (FONT_W + 1);
  if (charIndex >= (int)strlen(TEXT_MSG)) return false;
  if (colInChar >= FONT_W) return false;           // 1px gap between chars
  uint8_t ch   = (uint8_t)TEXT_MSG[charIndex];
  uint8_t bits = pgm_read_byte(&font5x7[ch * 5 + colInChar]);  // one column, bit 0 = top
  return (bits >> gy) & 0x01;
}

long     textScrollX    = 0;
uint32_t lastScrollStep = 0;

// Scrolling marquee: text on black, enters from the right, scrolls left, wraps.
void patternScrollText() {
  if (millis() - lastScrollStep >= TEXT_SCROLL_MS) {
    lastScrollStep = millis();
    textScrollX++;
    if (textScrollX >= textWidthCols() + LEDS_PER_ROW) textScrollX = 0;
  }

  FastLED.clear();                                 // blank, then draw the text

  for (int x = 0; x < LEDS_PER_ROW; x++) {
    int vx = x + textScrollX - LEDS_PER_ROW;       // enter from right, scroll left
    for (int gy = 0; gy < FONT_H; gy++) {
      if (textPixel(vx, gy)) {
        CRGB col = TEXT_RAINBOW
          ? (CRGB)CHSV(gHueOffset + x * 256 / LEDS_PER_ROW, 255, TEXT_BRIGHT)
          : (CRGB)CHSV(TEXT_HUE, TEXT_SAT, TEXT_BRIGHT);
        XY(x, TEXT_Y + gy) = col;
      }
    }
  }
  if (TEXT_RAINBOW) gHueOffset += 1;
}

// ---- pattern: simulated spectrum / frequency analyzer ----
// 20 bars (one per column), fast attack + gravity fall, green->red by height,
// with white peak-hold dots. Perlin noise drives each band so it "dances".
uint8_t  barLvl [LEDS_PER_ROW];   // current bar height, 0..255
uint8_t  barPk  [LEDS_PER_ROW];   // peak-hold height, 0..255
uint8_t  barHold[LEDS_PER_ROW];   // frames left to hold the peak before it falls
uint16_t analyzerT = 0;           // animation time

void patternAnalyzer() {
  analyzerT += WEB_SPEED * 2;                       // animation speed (web Speed slider 1..10)
  FastLED.clear();
  for (int c = 0; c < LEDS_PER_ROW; c++) {
    // organic per-band level, contrast-expanded so bars use the full range
    int n = ((int)inoise8(c * 55, analyzerT) - 70) * 3;
    if (n < 0) n = 0; else if (n > 255) n = 255;
    uint8_t target = (uint8_t)n;

    if (target > barLvl[c]) barLvl[c] = target;                    // fast attack
    else barLvl[c] = (barLvl[c] > 6) ? barLvl[c] - 6 : 0;          // slow gravity

    if (barLvl[c] >= barPk[c]) { barPk[c] = barLvl[c]; barHold[c] = 12; }   // new peak
    else if (barHold[c]) barHold[c]--;                                      // hold
    else barPk[c] = (barPk[c] > 3) ? barPk[c] - 3 : 0;                      // peak falls

    int h = (barLvl[c] * NUM_ROWS + 200) / 256;       // bar height in rows
    for (int k = 0; k < h && k < NUM_ROWS; k++) {     // k = 0 at bottom, up
      uint8_t hue = 96 - (uint8_t)((uint16_t)k * 96 / (NUM_ROWS - 1));   // green(96)->red(0)
      XY(c, NUM_ROWS - 1 - k) = CHSV(hue, 255, 200);
    }
    int ph = (barPk[c] * NUM_ROWS + 200) / 256;       // peak-dot row
    if (ph > 0 && ph <= NUM_ROWS) XY(c, NUM_ROWS - ph) = CRGB::White;
  }
}

// ---- pattern: Star Trek-style starfield ----
// Single points (no trails). Brightness fades in from dim at the center to bright
// at the edge, and speed is a SINE function of distance (slow near center, faster
// as it travels out) -- overall slow drift. Stars are heat-colored (deep red near the
// center -> orange -> yellow -> white-hot at the edge); the color slider sets the heat base hue.
#define NUM_STARS 320
float starDX[NUM_STARS], starDY[NUM_STARS];   // unit direction from center
float starR [NUM_STARS];                       // distance from center
bool  starsInit = false;

void spawnStar(int i, float r0) {
  float a = random16() * (TWO_PI / 65536.0f);  // random direction
  starDX[i] = cosf(a);
  starDY[i] = sinf(a);
  starR[i]  = r0;
}

void patternStarfield() {
  const float cx = (LEDS_PER_ROW - 1) / 2.0f, cy = (NUM_ROWS - 1) / 2.0f;
  const float maxR = sqrtf(cx * cx + cy * cy);
  if (!starsInit) {
    for (int i = 0; i < NUM_STARS; i++) spawnStar(i, (random8() / 255.0f) * maxR);  // start spread out
    starsInit = true;
  }

  FastLED.clear();
  float sm = WEB_SPEED / 8.0f;                  // web Speed slider (slow; nominal 0.25 at WEB_SPEED=2)
  for (int i = 0; i < NUM_STARS; i++) {
    float rn = starR[i] / maxR;
    if (rn > 1.0f) rn = 1.0f;
    // speed as a sine of distance: a visible floor at center, faster outward.
    // (floor avoids stars crawling/piling up at the center)
    float speed = sm * (0.22f + 0.45f * sinf(rn * HALF_PI));
    starR[i] += speed;

    float x = cx + starDX[i] * starR[i];
    float y = cy + starDY[i] * starR[i];
    int ix = (int)lroundf(x), iy = (int)lroundf(y);
    if (ix < 0 || ix >= LEDS_PER_ROW || iy < 0 || iy >= NUM_ROWS) { spawnStar(i, 0.0f); continue; }

    int b = 15 + (int)(rn * 240.0f);            // fade level: dim at center -> bright at edge
    if (b > 255) b = 255;
    uint8_t bb  = (uint8_t)b;                                    // star temperature
    uint8_t hue = TEXT_HUE + scale8(bb, 56);                     // heat: base hue -> warmer as it brightens
    uint8_t sat = (bb < 170) ? 255 : (uint8_t)(255 - (int)(bb - 170) * 2);  // hottest stars wash white
    XY(ix, iy) = CHSV(hue, sat, bb);                            // heat-colored star (slider sets the base hue)
  }
}

// ---- pattern: Clock ----
// HH:MM in a 3x5 font (5 glyphs span 19 of 20 cols) on real NTP time, with a
// blinking colon, plus a seconds dot on the bottom row that advances every 3s
// (synced to the clock) and crossfades like Twinkle. Color = Text color slider.
uint8_t clockLevel[LEDS_PER_ROW];    // bottom-row seconds-dot brightness

const char* const CLK_DIG[10][5] = {
  {"###","#.#","#.#","#.#","###"}, // 0
  {".#.","##.",".#.",".#.","###"}, // 1
  {"###","..#","###","#..","###"}, // 2
  {"###","..#",".##","..#","###"}, // 3
  {"#.#","#.#","###","..#","..#"}, // 4
  {"###","#..","###","..#","###"}, // 5
  {"###","#..","###","#.#","###"}, // 6
  {"###","..#","..#","..#","..#"}, // 7
  {"###","#.#","###","#.#","###"}, // 8
  {"###","#.#","###","..#","###"}, // 9
};
const char* const CLK_COLON[5] = { "...", ".#.", "...", ".#.", "..." };

void drawGlyph3(int x0, int y0, const char* const g[5], uint8_t val) {
  for (int r = 0; r < 5; r++)
    for (int k = 0; k < 3; k++)
      if (g[r][k] == '#') XY(x0 + k, y0 + r) = CHSV(TEXT_HUE, TEXT_SAT, val);
}

void patternClock() {
  FastLED.clear();
  struct tm t;
  bool haveTime = getLocalTime(&t, 0);

  if (haveTime) {                                   // HH:MM digits, rows 2..6
    int hh = t.tm_hour % 12; if (hh == 0) hh = 12;  // 12-hour format (12am/12pm -> 12)
    int mm = t.tm_min;
    int d[4]  = { hh / 10, hh % 10, mm / 10, mm % 10 };
    int xs[4] = { 0, 4, 12, 16 };                   // H H : M M  (3-wide glyphs + 1 gap)
    for (int i = 0; i < 4; i++) {
      if (i == 0 && d[0] == 0) continue;            // blank the leading zero (1..9 -> "9:05")
      drawGlyph3(xs[i], 2, CLK_DIG[d[i]], 200);
    }
    if (t.tm_sec % 2 == 0) drawGlyph3(8, 2, CLK_COLON, 200);   // colon blinks 1Hz
  }

  // seconds dot on the bottom row: position = current 3-second slot of the minute
  int target = haveTime ? (t.tm_sec / 3) : (int)((millis() / 3000) % LEDS_PER_ROW);
  if (target >= LEDS_PER_ROW) target = LEDS_PER_ROW - 1;
  int up = stepForMs(FADE_IN_MS), dn = stepForMs(FADE_OUT_MS);  // same fade as Twinkle
  for (int c = 0; c < LEDS_PER_ROW; c++) {
    if (c == target) { int v = clockLevel[c] + up; clockLevel[c] = (v > MAX_BRIGHTNESS) ? MAX_BRIGHTNESS : v; }
    else             { clockLevel[c] = (clockLevel[c] > dn) ? clockLevel[c] - dn : 0; }
    if (clockLevel[c]) XY(c, NUM_ROWS - 1) = CHSV(TEXT_HUE, TEXT_SAT, clockLevel[c]);
  }
}

// ---- pattern: Matrix digital rain ----
// Green streams fall down each column: a bright (whiter) head with a fading green
// tail behind it, each column at a random speed. Speed slider scales the fall.
#define RAIN_TRAIL 6
float rainY[LEDS_PER_ROW], rainSpd[LEDS_PER_ROW];
bool  rainInit = false;

void rainRespawn(int c) {
  rainY[c]   = -(float)random8(0, NUM_ROWS + RAIN_TRAIL);  // start staggered above the top
  rainSpd[c] = random8(12, 45) / 100.0f;                   // 0.12..0.44 rows/frame (before slider)
}

void patternMatrix() {
  if (!rainInit) { for (int c = 0; c < LEDS_PER_ROW; c++) rainRespawn(c); rainInit = true; }
  FastLED.clear();
  float sm = WEB_SPEED / 2.0f;                              // speed slider
  for (int c = 0; c < LEDS_PER_ROW; c++) {
    rainY[c] += rainSpd[c] * sm;
    if (rainY[c] - RAIN_TRAIL > NUM_ROWS) rainRespawn(c);   // head + tail fully past the bottom
    int h = (int)lroundf(rainY[c]);
    for (int t = 0; t < RAIN_TRAIL; t++) {                  // t=0 head, then the tail upward
      int y = h - t;
      if (y < 0 || y >= NUM_ROWS) continue;
      uint8_t val = (t == 0) ? 255 : (uint8_t)(200 * (RAIN_TRAIL - t) / RAIN_TRAIL);
      uint8_t sat = (t == 0) ? 80 : 255;                    // head whiter-green, tail full green
      XY(c, y) = CHSV(96, sat, val);                        // green
    }
  }
}

// ---- pattern: Conway's Game of Life ----
// Classic B3/S23 cellular automaton on a wrap-around (toroidal) grid: the left
// edge neighbors the right, top neighbors bottom, so gliders travel forever.
// Live cells fade up, dying cells fade down (same fade feel as Twinkle/Clock).
// When the board stalls (empty, or any oscillator up to LIFE_HIST-period) it reseeds;
// a generation cap is a final backstop against rare long/high-period lock-ins.
#define LIFE_HIST 8
bool    lifeGrid[NUM_ROWS][LEDS_PER_ROW];   // current generation (alive/dead)
bool    lifeNext[NUM_ROWS][LEDS_PER_ROW];   // next generation scratch buffer
uint8_t lifeLevel[NUM_ROWS][LEDS_PER_ROW];  // per-cell brightness, for the fade
bool    lifeInit = false;
uint32_t lifeLastGen = 0;
uint32_t lifeHist[LIFE_HIST] = {0};         // ring of recent generation fingerprints
uint8_t  lifeHistIdx = 0;
uint8_t  lifeStale = 0;                      // consecutive stalled generations
uint16_t lifeGen = 0;                        // generations since last seed (watchdog)
uint8_t  lifeAge[NUM_ROWS][LEDS_PER_ROW];    // generations each live cell has survived (for color)
uint8_t  lifeMode = 0;                        // cell-color mode: 0=Solid 1=Age 2=Heat 3=Health
const char* const LIFE_MODES[] = { "Solid", "Age", "Heat", "Health" };
const int LIFE_MODE_COUNT = sizeof(LIFE_MODES) / sizeof(LIFE_MODES[0]);

void lifeSeed() {
  for (int r = 0; r < NUM_ROWS; r++)
    for (int c = 0; c < LEDS_PER_ROW; c++)
      lifeGrid[r][c] = (random8() < 90);    // ~35% alive -> lively starting soup
  lifeStale = 0; lifeGen = 0; lifeHistIdx = 0;
  for (int i = 0; i < LIFE_HIST; i++) lifeHist[i] = 0;
  memset(lifeAge, 0, sizeof(lifeAge));       // fresh soup = all newborn
}

int lifeNeighbors(int r, int c) {           // 8-neighbor count, wrapping at the edges
  int n = 0;
  for (int dr = -1; dr <= 1; dr++)
    for (int dc = -1; dc <= 1; dc++) {
      if (dr == 0 && dc == 0) continue;
      int rr = (r + dr + NUM_ROWS) % NUM_ROWS;
      int cc = (c + dc + LEDS_PER_ROW) % LEDS_PER_ROW;
      if (lifeGrid[rr][cc]) n++;
    }
  return n;
}

void lifeStep() {                           // advance one generation into lifeGrid
  uint32_t h = 2166136261u; int pop = 0;    // FNV-1a fingerprint + population
  for (int r = 0; r < NUM_ROWS; r++) {
    for (int c = 0; c < LEDS_PER_ROW; c++) {
      int n = lifeNeighbors(r, c);
      bool alive = lifeGrid[r][c] ? (n == 2 || n == 3) : (n == 3);   // B3/S23
      lifeNext[r][c] = alive;
      if (alive) {
        if (lifeGrid[r][c]) { if (lifeAge[r][c] < 254) lifeAge[r][c]++; }  // survived -> older
        else lifeAge[r][c] = 0;                                            // newborn
      }   // dead cells keep their age so they fade out in their last color
      h ^= alive ? 1u : 0u; h *= 16777619u;
      if (alive) pop++;
    }
  }
  memcpy(lifeGrid, lifeNext, sizeof(lifeGrid));
  // stall = empty board, or this generation repeats one of the recent ones
  // (catches still lifes and any oscillator with period <= LIFE_HIST)
  bool repeat = false;
  for (int i = 0; i < LIFE_HIST; i++) if (lifeHist[i] == h) { repeat = true; break; }
  if (pop == 0 || repeat) lifeStale++; else lifeStale = 0;
  lifeHist[lifeHistIdx] = h; lifeHistIdx = (lifeHistIdx + 1) % LIFE_HIST;
  if (pop == 0 || lifeStale >= 6 || ++lifeGen >= 600) lifeSeed();   // reseed on stall, or after a long run
}

// Map a cell's age + brightness to a color according to the selected lifeMode.
CHSV lifeColorFor(uint8_t age, uint8_t val) {
  switch (lifeMode) {
    case 1:  // Age: newborn at the slider hue, rotating forward the longer it survives
      return CHSV(TEXT_HUE + (age > 40 ? 40 : age) * 4, 255, val);   // ~0..160 hue sweep
    case 2: { // Heat: white-hot newborns cooling to the slider color as they age
      uint8_t sat = (age >= 15) ? 255 : age * 17;                    // 0(white)->255(full color), even ramp
      return CHSV(TEXT_HUE, sat, val);
    }
    case 3:  // Health: young green -> old red
      return CHSV(96 - (age > 24 ? 24 : age) * 4, 255, val);         // hue 96(green)->0(red)
    default: // Solid: the original single-color Life
      return CHSV(TEXT_HUE, TEXT_SAT, val);
  }
}

void patternLife() {
  if (!lifeInit) { lifeSeed(); lifeInit = true; }
  uint32_t genMs = 1000 / (WEB_SPEED < 1 ? 1 : WEB_SPEED);   // WEB_SPEED generations/sec (1..10)
  if (millis() - lifeLastGen >= genMs) { lifeLastGen = millis(); lifeStep(); }

  FastLED.clear();
  // fade like Twinkle, but never slower than one full generation -- otherwise fast
  // generations would kill cells before they brighten, washing the board out dim.
  int framesPerGen = genMs / FRAME_MS; if (framesPerGen < 1) framesPerGen = 1;
  int genStep = MAX_BRIGHTNESS / framesPerGen; if (genStep < 1) genStep = 1;
  int softUp = stepForMs(FADE_IN_MS), softDn = stepForMs(FADE_OUT_MS);
  int up = (softUp > genStep) ? softUp : genStep;
  int dn = (softDn > genStep) ? softDn : genStep;
  for (int r = 0; r < NUM_ROWS; r++) {
    for (int c = 0; c < LEDS_PER_ROW; c++) {
      int tgt = lifeGrid[r][c] ? MAX_BRIGHTNESS : 0;            // fade toward alive/dead
      if      (lifeLevel[r][c] < tgt) { int v = lifeLevel[r][c] + up; lifeLevel[r][c] = (v > tgt) ? tgt : v; }
      else if (lifeLevel[r][c] > tgt) { int v = lifeLevel[r][c] - dn; lifeLevel[r][c] = (v < tgt) ? tgt : v; }
      if (lifeLevel[r][c]) XY(c, r) = lifeColorFor(lifeAge[r][c], lifeLevel[r][c]);
    }
  }
}

// ---- pattern: Plasma (demoscene color field) ----
// Sum of moving sines folds smooth blobs across the whole panel -- pure sin8 lookups,
// no state. The color slider centers a narrow hue band (defaults to blue); the plasma
// value also drives brightness and lightens the peaks, so it glows with depth.
uint16_t plasmaT = 0;

void patternPlasma() {
  plasmaT += (uint16_t)WEB_SPEED;                          // flow speed (gentle; halved from the original)
  for (int y = 0; y < NUM_ROWS; y++) {
    for (int x = 0; x < LEDS_PER_ROW; x++) {
      int dx = x - 10, dy = y - 5;
      uint8_t rad = sqrt16((uint16_t)(dx * dx + dy * dy));  // distance from centre
      uint8_t v = sin8(x * 16 + plasmaT)
                + sin8(y * 24 - plasmaT)
                + sin8((x + y) * 12 + (plasmaT >> 1))
                + sin8(rad * 16 + plasmaT);                 // uint8 wrap = the plasma fold
      uint8_t hue = TEXT_HUE - 24 + scale8(v, 48);          // narrow band around the slider hue
      uint8_t sat = 255 - scale8(v, 90);                    // peaks wash to a lighter blue (still ~65% sat)
      uint8_t val = scale8(v, 205) + 50;                    // 50..255 glow, never fully black
      XY(x, y) = CHSV(hue, sat, val);
    }
  }
}

// ---- pattern: Fire (Fire2012-style rising flames, tintable) ----
// Per column: cool every cell, let heat drift upward, spark new heat at the base.
// Heat maps to a fire ramp (deep red -> orange -> yellow -> white tips); the color
// slider shifts the base hue for green/blue/purple "magic fire". Evolves at a
// slider-controlled rate, paced the same gentle way as Plasma.
#define FIRE_COOLING  40
#define FIRE_SPARKING 120
uint8_t  fireHeat[NUM_ROWS][LEDS_PER_ROW];
uint32_t fireLast = 0;

void patternFire() {
  uint32_t fireMs = 1000 / (10 + WEB_SPEED * 3);   // ~13/s (slow) .. 40/s (fast)
  if (millis() - fireLast >= fireMs) {
    fireLast = millis();
    for (int c = 0; c < LEDS_PER_ROW; c++) {
      for (int r = 0; r < NUM_ROWS; r++)                        // 1) cool every cell
        fireHeat[r][c] = qsub8(fireHeat[r][c], random8(0, ((FIRE_COOLING * 10) / NUM_ROWS) + 2));
      for (int r = 0; r <= NUM_ROWS - 3; r++)                   // 2) heat drifts up (row 0 = top)
        fireHeat[r][c] = (fireHeat[r + 1][c] + fireHeat[r + 2][c] + fireHeat[r + 2][c]) / 3;
      if (random8() < FIRE_SPARKING) {                          // 3) ignite a spark at the base
        int r = NUM_ROWS - 1 - random8(2);
        fireHeat[r][c] = qadd8(fireHeat[r][c], random8(160, 255));
      }
    }
  }
  for (int r = 0; r < NUM_ROWS; r++) {                          // render heat -> tinted fire
    for (int c = 0; c < LEDS_PER_ROW; c++) {
      uint8_t h = fireHeat[r][c];
      uint8_t hue = TEXT_HUE + scale8(h, 56);                   // warms (red->yellow) as it heats
      uint8_t sat = (h < 170) ? 255 : (uint8_t)(255 - (int)(h - 170) * 2);   // hottest tips wash white
      XY(c, r) = CHSV(hue, sat, h);                             // brightness = heat
    }
  }
}

// ---- pattern: Tunnel (expanding rings rushing outward, with trails) ----
// Radial sawtooth field centred on the panel: each ring has a bright leading edge and
// a fading trail toward the centre, so rings appear to rush outward like a time tunnel.
// Single shade: the whole tunnel is the color slider's hue, rings shown via brightness only.
uint16_t tunnelT = 0;

void patternTunnel() {
  tunnelT += WEB_SPEED;                                    // gentle flow, like Plasma/Fire
  for (int y = 0; y < NUM_ROWS; y++) {
    for (int x = 0; x < LEDS_PER_ROW; x++) {
      int dx = 2 * x - (LEDS_PER_ROW - 1);                 // centre at (9.5, 4.5), half-cell units
      int dy = 2 * y - (NUM_ROWS - 1);
      uint8_t d = sqrt16((uint16_t)(dx * dx + dy * dy));   // radius, 0..~21
      uint8_t bright = (uint8_t)(d * 22 - tunnelT);        // sawtooth: sharp front + trailing fade
      XY(x, y) = CHSV(TEXT_HUE, 255, bright);              // single shade; rings = brightness only
    }
  }
}

// ---- pattern: Scope (oscilloscope sine wave with a glowing trace) ----
// A sine wave scrolls slowly across the panel; the trace is bright and fades like a CRT
// phosphor glow above and below the line. Single shade from the color slider (default green).
// Uses the generic Freq slider (cycles across the panel) and Glow slider (trace softness).
uint16_t scopeT = 0;

void patternScope() {
  scopeT += WEB_SPEED;                                     // scroll speed (gentle)
  uint8_t freq    = WEB_FREQ * 4;                          // 4..40 cycles factor
  uint8_t glowDiv = 26 - WEB_GLOW * 2;                     // 24 (tight) .. 6 (wide) -- higher slider = more glow
  FastLED.clear();
  for (int x = 0; x < LEDS_PER_ROW; x++) {
    int s = (int)sin8(x * freq + scopeT) - 128;           // -128..127
    int waveY16 = (NUM_ROWS - 1) * 8 + s / 2;             // trace height, 1/16-row units (centre +/- ~4 rows)
    for (int y = 0; y < NUM_ROWS; y++) {
      int dist = abs(y * 16 - waveY16);                    // vertical distance to the trace
      int b = 255 - dist * glowDiv;                        // bright core, glow falloff
      if (b > 0) XY(x, y) = CHSV(TEXT_HUE, 255, (uint8_t)b);
    }
  }
}

// ---- pattern: Pong (self-playing, predictive AI, impact-angle returns) ----
// Two AI paddles rally a ball on steep diagonals. Each return angle is set by WHERE the
// ball strikes the paddle (centre = shallow, edge = steep) plus a little randomness, and
// is capped so a crossing takes at most ~2-3 wall bounces. Each paddle PREDICTS the ball's
// intercept (folding in wall bounces) once the ball is within PONG_LOOKAHEAD columns (until
// then the paddles bob up and down), aiming to strike it at a random offset. The ball is
// a white core whose glow PULSES a fresh colour on each paddle hit, then fades.
// Speed -> ball/paddle speed; Glow -> pulse intensity; Freq -> colour change per hit; Color -> paddle colour.
#define PONG_PH        3        // paddle height (rows)
#define PONG_MAX_ANG   1.00f    // steepest return (~57 deg) -> ~2-3 wall bounces per crossing
#define PONG_MIN_ANG   0.35f    // never dead-flat
#define PONG_RAND      0.18f    // random jitter (rad, ~+/-10 deg) added to each return angle
#define PONG_LOOKAHEAD 5        // ball must be within this many columns for a paddle to commit (settable)
float    pbx, pby, pvx, pvy;               // ball position + unit direction
float    plY = 3, prY = 3;                 // left/right paddle top row
float    pongAimL = 0, pongAimR = 0;       // where each paddle aims to strike the ball (-1..1)
uint8_t  pongHue = 0;                       // pulse hue (advances on each paddle hit)
uint8_t  pongFlash = 0;                     // impact-pulse intensity (fades between hits)
int      pongWait = 0;                      // frames paused at centre before serving
bool     pongInit = false;
float    pongIdle = 0;                       // idle-bob phase (paddles move up/down until they commit)

float pongRand1() { return (random8() / 255.0f) * 2.0f - 1.0f; }   // -1..1

// Predict the ball's row when it reaches column targetX, folding top/bottom bounces.
float pongPredictY(float targetX) {
  if (fabsf(pvx) < 0.001f) return pby;
  float t = (targetX - pbx) / pvx;
  if (t < 0) return pby;                                  // moving away
  float y = pby + pvy * t;
  float period = 2.0f * (NUM_ROWS - 1);
  float m = fmodf(y, period); if (m < 0) m += period;
  return (m <= NUM_ROWS - 1) ? m : period - m;
}

// New velocity after a paddle hit: angle from impact offset o (-1..1) + randomness, capped.
void pongReturn(float o, int dir) {
  o = constrain(o, -1.0f, 1.0f);
  float ang = o * PONG_MAX_ANG + pongRand1() * PONG_RAND;
  ang = constrain(ang, -PONG_MAX_ANG, PONG_MAX_ANG);
  if (fabsf(ang) < PONG_MIN_ANG) ang = (ang < 0 ? -PONG_MIN_ANG : PONG_MIN_ANG);
  pvx = dir * cosf(ang);
  pvy = sinf(ang);
  pongFlash = 255; pongHue += 40 + WEB_FREQ * 12;         // pulse a fresh colour on impact
}

void pongServe(int dir) {                  // dir = +1 serve right, -1 serve left
  pbx = (LEDS_PER_ROW - 1) / 2.0f;
  pby = (NUM_ROWS - 1) / 2.0f;
  float ang = PONG_MIN_ANG + (random8() / 255.0f) * (PONG_MAX_ANG - PONG_MIN_ANG);
  pvx = dir * cosf(ang);
  pvy = ((random8() & 1) ? 1.0f : -1.0f) * sinf(ang);
  if (dir > 0) pongAimR = pongRand1(); else pongAimL = pongRand1();   // receiving paddle's aim
  pongWait = 14;
}

// Move one paddle: predict the intercept and aim to strike the ball at offset `aim` when the
// ball is heading in and within reach; otherwise ease back toward centre.
void pongAI(float &pY, float targetX, bool incoming, float aim, float pStep, float idleOff) {
  float mid  = (NUM_ROWS - PONG_PH) / 2.0f;
  float want = mid + mid * sinf(pongIdle + idleOff);                 // bob up/down while idle
  if (incoming && fabsf(targetX - pbx) <= PONG_LOOKAHEAD)            // ...until it's time to commit
    want = pongPredictY(targetX) - (PONG_PH - 1) / 2.0f * (1.0f + aim);
  pY += constrain(want - pY, -pStep, pStep);
  pY  = constrain(pY, 0.0f, (float)(NUM_ROWS - PONG_PH));
}

void patternPong() {
  if (!pongInit) { pongServe((random8() & 1) ? 1 : -1); pongInit = true; }
  float spd = 0.10f + WEB_SPEED * 0.035f;            // ball speed (cells/frame)
  pongFlash = qsub8(pongFlash, 12);                  // impact pulse fades each frame
  pongIdle += 0.05f; if (pongIdle > TWO_PI) pongIdle -= TWO_PI;   // idle-bob animation

  if (pongWait > 0) {
    pongWait--;
  } else {
    pbx += pvx * spd;  pby += pvy * spd;
    if (pby < 0)            { pby = 0;            pvy = -pvy; }
    if (pby > NUM_ROWS - 1) { pby = NUM_ROWS - 1; pvy = -pvy; }

    float pStep = 0.15f + WEB_SPEED * 0.06f;          // paddle tracking speed (fast enough to intercept)
    pongAI(plY, 1.0f,                pvx < 0, pongAimL, pStep, 0.0f);   // left paddle
    pongAI(prY, LEDS_PER_ROW - 2.0f, pvx > 0, pongAimR, pStep, PI);     // right paddle (opposite bob)

    int   by   = lroundf(pby);
    float half = (PONG_PH - 1) / 2.0f;
    if (pvx < 0 && pbx <= 1.0f) {                     // reached left paddle
      if (by >= lroundf(plY) && by < lroundf(plY) + PONG_PH) {
        pbx = 1.0f;
        pongReturn((pby - (plY + half)) / half, +1); // return angle from where it hit
        pongAimR = pongRand1();                       // right paddle picks its next aim
      }
    } else if (pvx > 0 && pbx >= LEDS_PER_ROW - 2.0f) {  // reached right paddle
      if (by >= lroundf(prY) && by < lroundf(prY) + PONG_PH) {
        pbx = LEDS_PER_ROW - 2.0f;
        pongReturn((pby - (prY + half)) / half, -1);
        pongAimL = pongRand1();
      }
    }
    if (pbx < 0)                 pongServe(+1);         // safety: ball got past -> re-serve
    if (pbx > LEDS_PER_ROW - 1)  pongServe(-1);
  }

  FastLED.clear();
  int lTop = lroundf(plY), rTop = lroundf(prY);
  for (int k = 0; k < PONG_PH; k++) {
    XY(0, lTop + k)                = CHSV(TEXT_HUE, 220, 130);   // paddles = court colour
    XY(LEDS_PER_ROW - 1, rTop + k) = CHSV(TEXT_HUE, 220, 130);
  }
  int bx = lroundf(pbx), by = lroundf(pby);
  uint8_t haloHue = pongHue;                                    // colour set at the last paddle hit
  uint8_t peak = 60 + WEB_GLOW * 18;                            // impact-pulse brightness (Glow slider)
  uint8_t base = 18 + WEB_GLOW * 5;                             // faint steady glow between hits
  uint16_t hb  = base + scale8(peak, pongFlash);               // flares on impact, then fades
  uint8_t haloBr = hb > 255 ? 255 : (uint8_t)hb;
  CRGB halo = CHSV(haloHue, 255, haloBr);
  CRGB hd   = CHSV(haloHue, 255, haloBr / 2);
  XY(bx + 1, by)     += halo; XY(bx - 1, by)     += halo;
  XY(bx, by + 1)     += halo; XY(bx, by - 1)     += halo;
  XY(bx + 1, by + 1) += hd;   XY(bx - 1, by - 1) += hd;
  XY(bx + 1, by - 1) += hd;   XY(bx - 1, by + 1) += hd;
  XY(bx, by) = CRGB::White;                                      // bright core on top
}

// ---- pattern: Defender (self-playing side-scroller) ----
// A chevron ship holds station on the left while the world scrolls past: 3 rows of
// jagged terrain at the bottom, red landers drifting in from the right. The ship weaves
// to line up with the nearest lander and fires; hits burst into an expanding spark.
// Color -> ship colour; Speed -> scroll/fire/bullet speed. (Parallel arrays, no structs.)
#define DEF_SHIPX    1      // ship's fixed column (far left)
#define DEF_RANGE    10     // hold fire until a lander is within this many columns
#define DEF_ENEMIES  5
#define DEF_BULLETS  4
#define DEF_BOOMS    4
#define DEF_TROWS    3      // terrain rows along the bottom
float   defScroll = 0, defShipY = 3;
int     defFireCd = 0, defSpawnCd = 20;
bool    defInit = false;
bool    eOn[DEF_ENEMIES]; float eX[DEF_ENEMIES], eY[DEF_ENEMIES];   // landers
bool    bOn[DEF_BULLETS]; float bX[DEF_BULLETS], bY[DEF_BULLETS];   // ship bullets
bool    kOn[DEF_BOOMS];   float kX[DEF_BOOMS], kY[DEF_BOOMS]; uint8_t kT[DEF_BOOMS];  // explosions

int defFreeSlot(bool* on, int n) { for (int i = 0; i < n; i++) if (!on[i]) return i; return -1; }

uint8_t defTerrain(int wx) {                       // ground height 1..DEF_TROWS at world column wx
  int v = sin8(wx * 16) + sin8(wx * 37 + 90);      // 0..510, jagged ridge
  return 1 + (uint8_t)((v * DEF_TROWS) / 511);
}

void patternDefender() {
  if (!defInit) {
    for (int i = 0; i < DEF_ENEMIES; i++) eOn[i] = false;
    for (int i = 0; i < DEF_BULLETS; i++) bOn[i] = false;
    for (int i = 0; i < DEF_BOOMS;   i++) kOn[i] = false;
    defShipY = 3; defScroll = 0; defFireCd = 0; defSpawnCd = 20; defInit = true;
  }
  float scr = 0.05f + WEB_SPEED * 0.03f;                     // world scroll per frame
  defScroll += scr;

  if (--defSpawnCd <= 0) {                                   // spawn a lander from the right
    int i = defFreeSlot(eOn, DEF_ENEMIES);
    if (i >= 0) { eOn[i] = true; eX[i] = LEDS_PER_ROW + 1; eY[i] = 1 + random8(5); }
    defSpawnCd = 45 + random8(70);
  }
  for (int i = 0; i < DEF_ENEMIES; i++) if (eOn[i]) {        // drift left, retire off-screen
    eX[i] -= scr;
    if (eX[i] < -1) eOn[i] = false;
  }

  int tgt = -1; float best = 1e9f;                           // nearest lander ahead AND within range
  for (int i = 0; i < DEF_ENEMIES; i++)
    if (eOn[i] && eX[i] > DEF_SHIPX && (eX[i] - DEF_SHIPX) <= DEF_RANGE && eX[i] < best) { best = eX[i]; tgt = i; }
  float targetY = (tgt >= 0) ? eY[tgt] : 3.0f + 1.6f * sinf(defScroll * 0.4f);  // seek when in range, else bob
  defShipY += constrain(targetY - defShipY, -0.4f, 0.4f);
  defShipY  = constrain(defShipY, 1.0f, 5.0f);

  if (--defFireCd <= 0 && tgt >= 0 && fabsf(defShipY - eY[tgt]) < 0.6f) {       // fire when lined up
    int b = defFreeSlot(bOn, DEF_BULLETS);
    if (b >= 0) { bOn[b] = true; bX[b] = DEF_SHIPX + 2; bY[b] = defShipY; }
    defFireCd = 8;
  }

  float bsp = 0.7f + WEB_SPEED * 0.06f;                      // bullets fly right, check hits
  for (int b = 0; b < DEF_BULLETS; b++) if (bOn[b]) {
    bX[b] += bsp;
    if (bX[b] > LEDS_PER_ROW) { bOn[b] = false; continue; }
    for (int e = 0; e < DEF_ENEMIES; e++) if (eOn[e]) {
      if (lroundf(bY[b]) == lroundf(eY[e]) && bX[b] >= eX[e] - 0.5f && bX[b] <= eX[e] + 1.5f) {
        eOn[e] = false; bOn[b] = false;
        int k = defFreeSlot(kOn, DEF_BOOMS);
        if (k >= 0) { kOn[k] = true; kX[k] = eX[e]; kY[k] = eY[e]; kT[k] = 0; }
        break;
      }
    }
  }
  for (int k = 0; k < DEF_BOOMS; k++) if (kOn[k]) { if (++kT[k] > 8) kOn[k] = false; }

  // ---- render ----
  FastLED.clear();
  int soff = (int)defScroll;
  for (int x = 0; x < LEDS_PER_ROW; x++) {                   // terrain
    uint8_t h = defTerrain(x + soff);
    for (int r = 0; r < h; r++) XY(x, NUM_ROWS - 1 - r) = CHSV(96, 255, 90);   // green ground
  }
  for (int k = 0; k < DEF_BOOMS; k++) if (kOn[k]) {          // explosions (expanding yellow)
    uint8_t br = 255 - kT[k] * 28;
    int r = kT[k] / 3, cx = lroundf(kX[k]), cy = lroundf(kY[k]);
    XY(cx, cy) = CHSV(40, 255, br);
    XY(cx + r, cy) = CHSV(30, 255, br); XY(cx - r, cy) = CHSV(30, 255, br);
    XY(cx, cy + r) = CHSV(30, 255, br); XY(cx, cy - r) = CHSV(30, 255, br);
  }
  for (int e = 0; e < DEF_ENEMIES; e++) if (eOn[e]) {        // landers (red, 2px)
    int ex = lroundf(eX[e]), ey = lroundf(eY[e]);
    XY(ex, ey) = CHSV(0, 255, 200); XY(ex + 1, ey) = CHSV(0, 255, 200);
  }
  for (int b = 0; b < DEF_BULLETS; b++) if (bOn[b]) XY(lroundf(bX[b]), lroundf(bY[b])) = CRGB::White;
  int sy = lroundf(defShipY);                                // chevron ship, pointing right
  XY(DEF_SHIPX,     sy - 1) = CHSV(TEXT_HUE, 255, 220);
  XY(DEF_SHIPX + 1, sy)     = CHSV(TEXT_HUE, 255, 255);
  XY(DEF_SHIPX,     sy + 1) = CHSV(TEXT_HUE, 255, 220);
}

// ---- pattern: Balls (bouncing balls that leave fading vapor trails) ----
// Builds up one ball at a time to BALLS_MAX, each a rainbow colour bouncing off the walls
// and leaving a fading trail (the whole buffer fades a little each frame). Once full it
// holds ~5s then wipes and rebuilds. Speed -> velocity; Glow -> trail length; Color -> palette.
#define BALLS_MAX 30
float    ballX[BALLS_MAX], ballY[BALLS_MAX], ballVX[BALLS_MAX], ballVY[BALLS_MAX];
uint8_t  ballHue[BALLS_MAX];
int      ballCount = 0;
uint32_t ballLastBirth = 0, ballFullSince = 0;
bool     ballsInit = false;

void ballSpawn() {
  if (ballCount >= BALLS_MAX) return;
  int i = ballCount++;
  ballX[i] = random8(LEDS_PER_ROW);
  ballY[i] = random8(NUM_ROWS);
  float a = random16() * (TWO_PI / 65536.0f);
  ballVX[i] = cosf(a);  ballVY[i] = sinf(a);
  ballHue[i] = i * 41;                                     // spread hues around the wheel
}

void ballsReset() {
  ballCount = 0;  FastLED.clear();
  ballSpawn();  ballLastBirth = millis();  ballFullSince = 0;
}

void patternBalls() {
  if (!ballsInit) { ballsReset(); ballsInit = true; }
  uint32_t now = millis();
  if (ballCount < BALLS_MAX) {
    if (now - ballLastBirth > 900) { ballSpawn(); ballLastBirth = now; }   // a new ball ~ every 0.9s
  } else {
    if (ballFullSince == 0) ballFullSince = now;
    if (now - ballFullSince > 5000) ballsReset();                          // hold 5s at full, then rebuild
  }
  uint8_t fade = 44 - WEB_GLOW * 3;                        // higher Glow = longer trail
  fadeToBlackBy(&leds[0][0], NUM_ROWS * LEDS_PER_ROW, fade);
  float spd = 0.10f + WEB_SPEED * 0.05f;
  for (int i = 0; i < ballCount; i++) {
    ballX[i] += ballVX[i] * spd;  ballY[i] += ballVY[i] * spd;
    if (ballX[i] < 0)                  { ballX[i] = 0;                ballVX[i] = -ballVX[i]; }
    else if (ballX[i] > LEDS_PER_ROW-1){ ballX[i] = LEDS_PER_ROW - 1; ballVX[i] = -ballVX[i]; }
    if (ballY[i] < 0)                  { ballY[i] = 0;                ballVY[i] = -ballVY[i]; }
    else if (ballY[i] > NUM_ROWS-1)    { ballY[i] = NUM_ROWS - 1;     ballVY[i] = -ballVY[i]; }
    XY(lroundf(ballX[i]), lroundf(ballY[i])) = CHSV(ballHue[i] + TEXT_HUE, 255, 255);
  }
}

// ---- pattern: Meteors (white meteors streak down and burst into fire on impact) ----
// Up to METEOR_MAX white meteors fall at random angles + random speeds, leaving fading
// trails; when one reaches the ground it dies and spawns a small fire-coloured explosion.
// New meteors arrive after random delays. Speed -> fall rate; Glow -> trail length.
#define METEOR_MAX 3
#define MBOOM_MAX  4
bool     mtOn[METEOR_MAX]; float mtX[METEOR_MAX], mtY[METEOR_MAX], mtVX[METEOR_MAX], mtVY[METEOR_MAX];
bool     mbOn[MBOOM_MAX];  float mbX[MBOOM_MAX]; uint8_t mbT[MBOOM_MAX];
uint32_t meteorNextBirth = 0;
bool     meteorInit = false;

void meteorSpawn() {
  int i = defFreeSlot(mtOn, METEOR_MAX);
  if (i < 0) return;
  mtOn[i] = true;
  mtX[i] = 3 + random8(LEDS_PER_ROW - 6);                 // start away from the edges (3..16)
  mtY[i] = -1.0f;                                          // just above the top
  mtVX[i] = (random8() / 255.0f - 0.5f) * 0.6f;           // angle: -0.3 .. 0.3
  mtVY[i] = 0.30f + (random8() / 255.0f) * 0.30f;         // random fall speed 0.30 .. 0.60
}

void patternMeteors() {
  if (!meteorInit) {
    for (int i = 0; i < METEOR_MAX; i++) mtOn[i] = false;
    for (int k = 0; k < MBOOM_MAX; k++)  mbOn[k] = false;
    meteorNextBirth = millis(); meteorInit = true; FastLED.clear();
  }
  uint32_t now = millis();
  if (now >= meteorNextBirth) { meteorSpawn(); meteorNextBirth = now + random16(400, 1600); }  // random birth delay

  uint8_t fade = 60 - WEB_GLOW * 4;                        // higher Glow = longer trail
  fadeToBlackBy(&leds[0][0], NUM_ROWS * LEDS_PER_ROW, fade);
  for (int x = 0; x < LEDS_PER_ROW; x++) XY(x, NUM_ROWS - 1) = CHSV(18, 160, 22);   // dim ground

  for (int k = 0; k < MBOOM_MAX; k++) if (mbOn[k]) {       // fire explosions
    uint8_t t = mbT[k];
    if (t >= 16) { mbOn[k] = false; continue; }
    uint8_t v = 255 - t * 15;                              // fade 255 -> 30
    int cx = lroundf(mbX[k]);
    int r = (t < 5) ? 1 : 2;                               // small, brief growth
    for (int dx = -r; dx <= r; dx++) for (int dy = -r; dy <= 0; dy++) {
      int dd = dx * dx + dy * dy;
      if (dd <= r * r) {
        uint8_t hue = (dd <= 1) ? 38 : (dd <= 4 ? 16 : 2); // yellow core -> orange -> red
        uint8_t sat = (dd <= 1) ? 130 : 255;               // core whiter-hot
        XY(cx + dx, (NUM_ROWS - 1) + dy) = CHSV(hue, sat, v);
      }
    }
    mbT[k]++;
  }

  float spd = 0.6f + WEB_SPEED * 0.12f;                    // fall-rate multiplier
  for (int i = 0; i < METEOR_MAX; i++) if (mtOn[i]) {
    mtX[i] += mtVX[i] * spd; mtY[i] += mtVY[i] * spd;
    if (mtY[i] >= NUM_ROWS - 1) {                          // impact -> fire burst
      mtOn[i] = false;
      int k = defFreeSlot(mbOn, MBOOM_MAX);
      if (k >= 0) { mbOn[k] = true; mbX[k] = mtX[i]; mbT[k] = 0; }
      continue;
    }
    if (mtX[i] < -1 || mtX[i] > LEDS_PER_ROW) { mtOn[i] = false; continue; }   // drifted off-side
    XY(lroundf(mtX[i]), lroundf(mtY[i])) = CRGB::White;    // bright white head
  }
}

// ---- pattern: Lightning (strikes hitting the ground, staggered) ----
// Jagged bolts fall from the sky and strike the ground with a horizontal impact
// flash; a faint sky-flash lights the panel on each birth. Each bolt is a white-hot
// core with a colour-tinted glow (Color slider = hue). It flashes at full brightness
// then fades over a "fade time" set by Speed (higher = faster fade), and after a
// random gap that's ALWAYS shorter than the fade time it restarts -- so the on-screen
// count keeps overlapping. Freq (1..10) maps to how many strike at once (1..5). New
// strikes start at least LTG_MIN_SEP columns from every currently-active strike.
// Speed=fade, Glow=width, Freq=count, Color=hue.
#define LTG_MAX     5     // hard cap on simultaneous strikes (Freq tops out here)
#define LTG_MIN_SEP 5     // no new strike may start within this many columns of another
uint32_t ltgStart[LTG_MAX];              // millis() when the strike began (fade clock)
uint16_t ltgLife [LTG_MAX];              // fade duration for this strike (ms)
uint32_t ltgNext [LTG_MAX];              // when an idle slot is allowed to restart
bool     ltgOn   [LTG_MAX];              // slot currently striking?
int8_t   ltgPath [LTG_MAX][NUM_ROWS];    // bolt column at each row (top -> ground)
bool     ltgInit = false;

uint16_t ltgLifeMs() {                    // Speed slider -> fade time (sp1~1520ms .. sp10~350ms)
  return (uint16_t)(1650 - WEB_SPEED * 130);
}

// Choose a start column >= LTG_MIN_SEP from every other active strike; if none of the
// tries clear the gap (only possible at high Freq on a 20-wide panel) keep the roomiest.
int ltgPickStart(int self, int count) {
  int best = 2 + random8(LEDS_PER_ROW - 4), bestGap = -1;
  for (int a = 0; a < 24; a++) {
    int cand = 2 + random8(LEDS_PER_ROW - 4);         // 2..17 (margin so the glow fits)
    int gap = 99;
    for (int i = 0; i < count; i++) {
      if (i == self || !ltgOn[i]) continue;
      int dd = abs(cand - ltgPath[i][0]); if (dd < gap) gap = dd;
    }
    if (gap >= LTG_MIN_SEP) return cand;              // satisfies the rule -> take it
    if (gap > bestGap) { bestGap = gap; best = cand; }
  }
  return best;
}

void ltgSpawn(int self, int count, uint32_t now) {
  int x = ltgPickStart(self, count);
  for (int r = 0; r < NUM_ROWS; r++) {               // jagged wander top -> ground
    ltgPath[self][r] = x;
    uint8_t j = random8(4);                           // -1, 0, 0, +1
    if (j == 0) x--; else if (j == 3) x++;
    if (x < 1) x = 1; if (x > LEDS_PER_ROW - 2) x = LEDS_PER_ROW - 2;
  }
  ltgStart[self] = now;
  ltgLife[self]  = ltgLifeMs();
  ltgOn[self]    = true;
}

void patternLightning() {
  int count = map(WEB_FREQ, 1, 10, 1, LTG_MAX);       // Freq 1..10 -> 1..5 on screen
  if (count < 1) count = 1; if (count > LTG_MAX) count = LTG_MAX;
  uint32_t now = millis();
  if (!ltgInit) {                                     // stagger first strikes within < one fade time
    for (int i = 0; i < LTG_MAX; i++) { ltgOn[i] = false; ltgNext[i] = now + random16((uint16_t)(ltgLifeMs() * 0.6f)); }
    ltgInit = true;
  }

  FastLED.clear();
  for (int x = 0; x < LEDS_PER_ROW; x++) XY(x, NUM_ROWS - 1) += CRGB(9, 13, 24);   // faint ground line

  float spread = (WEB_GLOW - 1) / 3.0f;               // Glow -> half-width 0 (tight) .. 3 (wide)
  CRGB  gcol   = CHSV(TEXT_HUE, 190, 255);            // tinted glow colour (core stays white)
  float sky    = 0;                                   // brightest birth-flash this frame

  for (int i = 0; i < count; i++) {
    if (!ltgOn[i]) { if ((int32_t)(now - ltgNext[i]) >= 0) ltgSpawn(i, count, now); }
    if (!ltgOn[i]) continue;

    uint32_t age = now - ltgStart[i];
    if (age >= ltgLife[i]) {                           // finished -> idle, restart after a short gap
      ltgOn[i] = false;
      float g  = (0.12f + (random8() / 255.0f) * 0.45f) * ltgLife[i];   // gap < fade time
      ltgNext[i] = now + (uint32_t)g;
      continue;
    }

    float inten = 1.0f - (float)age / ltgLife[i];      // full at strike, fades to 0
    if (age < 90) { float s = (1.0f - age / 90.0f) * 0.4f * inten; if (s > sky) sky = s; }

    for (int r = 0; r < NUM_ROWS; r++) {               // bolt: white core + tinted glow
      int cx = ltgPath[i][r];
      for (int dx = -4; dx <= 4; dx++) {
        int d = abs(dx); if (d > spread + 0.6f) continue;
        float w = 1.0f - d / (spread + 0.75f); if (w <= 0) continue;
        CRGB c = gcol; c.nscale8((uint8_t)(inten * w * 255)); XY(cx + dx, r) += c;
      }
      uint8_t v = (uint8_t)(inten * 255); XY(cx, r) += CRGB(v, v, v);
    }

    int   imp = ltgPath[i][NUM_ROWS - 1];              // ground impact flash (spreads sideways)
    float fl  = inten * inten;
    for (int dx = -4; dx <= 4; dx++) {
      float w = 1.0f - fabsf((float)dx) / 4.2f; if (w <= 0) continue;
      uint8_t vv = (uint8_t)(255 * fl * w);
      XY(imp + dx, NUM_ROWS - 1) += CRGB(vv, vv, vv);
      XY(imp + dx, NUM_ROWS - 2) += CRGB((uint8_t)(120 * fl * w), (uint8_t)(120 * fl * w), (uint8_t)(140 * fl * w));
    }
  }

  if (sky > 0) {                                       // ambient sky-flash over the whole panel
    CRGB s = CRGB((uint8_t)(40 * sky), (uint8_t)(48 * sky), (uint8_t)(70 * sky));
    for (int i = 0; i < NUM_ROWS * LEDS_PER_ROW; i++) (&leds[0][0])[i] += s;
  }
}

// ---------------- PATTERN REGISTRY ----------------
// Named list so the (coming) web UI can list/select patterns by name.
// Parallel arrays (no struct in a function signature -> no Arduino prototype issue).
typedef void (*PatternFn)();
const char* const PATTERN_NAMES[] = { "Twinkle", "Rainbow", "Text", "Analyzer", "Starfield", "Compute", "Clock", "Matrix", "Life", "Plasma", "Fire", "Tunnel", "Scope", "Pong", "Defender", "Balls", "Meteors", "Lightning" };
const PatternFn   PATTERN_FNS[]   = { patternRandomFade, patternRainbowFade, patternScrollText, patternAnalyzer, patternStarfield, patternCompute, patternClock, patternMatrix, patternLife, patternPlasma, patternFire, patternTunnel, patternScope, patternPong, patternDefender, patternBalls, patternMeteors, patternLightning };
const int PATTERN_COUNT = sizeof(PATTERN_FNS) / sizeof(PATTERN_FNS[0]);
int gPatternIndex = 1;   // default: Rainbow

// per-effect saved settings. Sized to a fixed max (not PATTERN_COUNT) so adding
// patterns later doesn't change the NVS blob size and wipe saved per-effect settings.
#define MAX_PATTERNS 20   // room to grow; loadSettings tolerates a smaller saved blob
uint8_t fxSpeed [MAX_PATTERNS];   // 1..10
uint8_t fxBright[MAX_PATTERNS];   // 0..255 master brightness
uint8_t fxHue   [MAX_PATTERNS];   // 0..255 color
uint8_t fxGlow  [MAX_PATTERNS];   // 1..10 generic glow/softness
uint8_t fxFreq  [MAX_PATTERNS];   // 1..10 generic frequency/density

void applySpeed(uint8_t n);   // defined later (NETWORK section)

// Activate a pattern and load ITS saved speed/brightness/color into the live globals.
void selectPattern(int idx) {
  if (idx < 0 || idx >= PATTERN_COUNT) return;
  gPatternIndex     = idx;
  WEB_SPEED         = fxSpeed[idx];
  MASTER_BRIGHTNESS = fxBright[idx];
  TEXT_HUE          = fxHue[idx];
  WEB_GLOW          = fxGlow[idx];
  WEB_FREQ          = fxFreq[idx];
  FastLED.setBrightness(MASTER_BRIGHTNESS);
  applySpeed(WEB_SPEED);
  fxSpeed[idx] = WEB_SPEED;    // re-sync slot to the clamped speed
}

// Select a pattern by name; returns its index, or -1 if not found.
int setPatternByName(const char* name) {
  for (int i = 0; i < PATTERN_COUNT; i++)
    if (strcmp(PATTERN_NAMES[i], name) == 0) { selectPattern(i); return i; }
  return -1;
}

// ---------------- NETWORK ----------------
WebServer server(80);
const char* HOSTNAME = "wallart";   // reached at http://wallart.local

// ---- time & daily on/off schedule ----
const char* const TZ_NAMES[] = { "Eastern", "Central", "Mountain", "Pacific" };
const char* const TZ_POSIX[] = { "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0" };
const int TZ_COUNT = sizeof(TZ_NAMES) / sizeof(TZ_NAMES[0]);
uint8_t  TZ_INDEX = 0;        // selected timezone (web)
bool     POWER_ON = true;     // manual master on/off (web On/Off buttons)
bool     SCHED_ON = false;    // daily on/off schedule enabled (web)
uint16_t ON_MIN   = 7 * 60;   // turn ON  at 07:00 (minutes since midnight)
uint16_t OFF_MIN  = 23 * 60;  // turn OFF at 23:00

void applyTimezone() {
  configTzTime(TZ_POSIX[TZ_INDEX], "pool.ntp.org", "time.nist.gov");
}
String hhmm(uint16_t mins) {
  char b[6]; snprintf(b, sizeof(b), "%02u:%02u", mins / 60, mins % 60); return String(b);
}
String currentTimeStr() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return String("--:--");
  int hh = t.tm_hour % 12; if (hh == 0) hh = 12;          // 12-hour format
  const char* ap = (t.tm_hour < 12) ? "AM" : "PM";
  char b[12]; snprintf(b, sizeof(b), "%d:%02d %s", hh, t.tm_min, ap); return String(b);
}
uint16_t parseHHMM(const String& s) {
  int c = s.indexOf(':'); int h = 0, m = 0;
  if (c > 0) { h = s.substring(0, c).toInt(); m = s.substring(c + 1).toInt(); }
  if (h < 0) h = 0; if (h > 23) h = 23;
  if (m < 0) m = 0; if (m > 59) m = 59;
  return (uint16_t)(h * 60 + m);
}
String jsonEscape(const char* s) {
  String o; for (; *s; s++) { char c = *s; if (c == '"' || c == '\\') o += '\\'; o += c; } return o;
}
// Daily schedule as an OVERRIDABLE auto-toggle: at the on-time it switches the
// panel ON, at the off-time it switches it OFF. A manual On/Off press (POWER_ON)
// wins in between and holds until the next scheduled transition. Edge-triggered
// (acts only at the boundary) + throttled, so it never fights manual control and
// never stalls the render loop on getLocalTime() before NTP has synced.
bool schedDirty = true;   // set when schedule settings change -> re-sync immediately
void applySchedule() {
  if (!SCHED_ON) { schedDirty = false; return; }            // schedule off: manual control only
  static uint32_t last = 0;
  if (!schedDirty && millis() - last < 1000) return;        // poll ~1x/sec
  last = millis();
  struct tm t;
  if (!getLocalTime(&t, 0)) return;                         // time not synced yet -> leave as-is
  uint16_t now = t.tm_hour * 60 + t.tm_min;
  bool inWindow = (ON_MIN == OFF_MIN) ? true
                : (ON_MIN < OFF_MIN)  ? (now >= ON_MIN && now < OFF_MIN)   // daytime window
                                      : (now >= ON_MIN || now < OFF_MIN);  // overnight window
  static bool prev = false;
  if (schedDirty) {                 // settings changed / first run -> match the schedule now
    schedDirty = false;
    prev = inWindow;
    POWER_ON = inWindow;
    return;
  }
  if (inWindow && !prev) POWER_ON = true;     // reached the on-time  -> auto on
  if (!inWindow && prev) POWER_ON = false;    // reached the off-time -> auto off
  prev = inWindow;
}

// Unified 1..10 speed knob -> per-pattern speeds.
void applySpeed(uint8_t n) {
  if (n < 1) n = 1;
  if (n > 10) n = 10;
  WEB_SPEED      = n;
  RAINBOW_SPEED  = n;                     // hue shift per frame
  TEXT_SCROLL_MS = 30 + (10 - n) * 20;    // n=1 -> 210ms (slow), n=10 -> 30ms (fast)
}

// ---- persistence (NVS via Preferences) ----
Preferences prefs;
bool     settingsDirty = false;
uint32_t lastChange = 0;
void markDirty() { settingsDirty = true; lastChange = millis(); }   // schedule a debounced save

void saveSettings() {
  prefs.begin("wallart", false);
  prefs.putInt("pat", gPatternIndex);
  prefs.putBytes("fxspd", fxSpeed,  sizeof(fxSpeed));    // per-effect settings
  prefs.putBytes("fxbri", fxBright, sizeof(fxBright));
  prefs.putBytes("fxhue", fxHue,    sizeof(fxHue));
  prefs.putBytes("fxglw", fxGlow,   sizeof(fxGlow));
  prefs.putBytes("fxfrq", fxFreq,   sizeof(fxFreq));
  prefs.putBool("rnb", TEXT_RAINBOW);
  prefs.putString("msg", TEXT_MSG);
  prefs.putUChar("tz", TZ_INDEX);
  prefs.putBool("sch", SCHED_ON);
  prefs.putUShort("on", ON_MIN);
  prefs.putUShort("off", OFF_MIN);
  prefs.putUChar("lifem", lifeMode);
  prefs.end();
  Serial.println("settings saved");
}

void loadSettings() {
  prefs.begin("wallart", true);          // read-only
  gPatternIndex     = prefs.getInt("pat", gPatternIndex);
  // POWER_ON not persisted -> panel always boots On so the schedule governs after a power loss
  // load a saved blob even if it's shorter than the array (e.g. after MAX_PATTERNS grew):
  // getBytes copies the stored bytes into the first slots; the rest keep their defaults.
  if (prefs.getBytesLength("fxspd") > 0) prefs.getBytes("fxspd", fxSpeed,  sizeof(fxSpeed));
  if (prefs.getBytesLength("fxbri") > 0) prefs.getBytes("fxbri", fxBright, sizeof(fxBright));
  if (prefs.getBytesLength("fxhue") > 0) prefs.getBytes("fxhue", fxHue,    sizeof(fxHue));
  if (prefs.getBytesLength("fxglw") > 0) prefs.getBytes("fxglw", fxGlow,   sizeof(fxGlow));
  if (prefs.getBytesLength("fxfrq") > 0) prefs.getBytes("fxfrq", fxFreq,   sizeof(fxFreq));
  for (int i = 0; i < MAX_PATTERNS; i++) {              // keep loaded 1..10 sliders in range
    if (fxSpeed[i] < 1) fxSpeed[i] = 1;  if (fxSpeed[i] > 10) fxSpeed[i] = 10;
    if (fxGlow[i]  < 1) fxGlow[i]  = 1;  if (fxGlow[i]  > 10) fxGlow[i]  = 10;
    if (fxFreq[i]  < 1) fxFreq[i]  = 1;  if (fxFreq[i]  > 10) fxFreq[i]  = 10;
  }
  TEXT_RAINBOW      = prefs.getBool("rnb", TEXT_RAINBOW);
  String m          = prefs.getString("msg", String(TEXT_MSG));
  TZ_INDEX          = prefs.getUChar("tz", TZ_INDEX);
  SCHED_ON          = prefs.getBool("sch", SCHED_ON);
  ON_MIN            = prefs.getUShort("on", ON_MIN);
  OFF_MIN           = prefs.getUShort("off", OFF_MIN);
  lifeMode          = prefs.getUChar("lifem", lifeMode);
  prefs.end();
  if (lifeMode >= LIFE_MODE_COUNT) lifeMode = 0;
  strncpy(TEXT_MSG, m.c_str(), sizeof(TEXT_MSG) - 1);
  TEXT_MSG[sizeof(TEXT_MSG) - 1] = 0;
  if (gPatternIndex < 0 || gPatternIndex >= PATTERN_COUNT) gPatternIndex = 1;
  if (TZ_INDEX >= TZ_COUNT) TZ_INDEX = 0;
  if (ON_MIN  > 1439) ON_MIN  = 7 * 60;
  if (OFF_MIN > 1439) OFF_MIN = 23 * 60;
  selectPattern(gPatternIndex);   // apply the active pattern's saved speed/brightness/color
}

// ---- control page is defined in index_html.h (const char INDEX_HTML[]) ----

void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

void handleState() {
  String j; j.reserve(800);   // room for a long message in the JSON
  j = "{";
  j += "\"pattern\":\"" + String(PATTERN_NAMES[gPatternIndex]) + "\",";
  j += "\"power\":"   + String(POWER_ON ? 1 : 0) + ",";
  j += "\"bri\":"     + String(MASTER_BRIGHTNESS) + ",";
  j += "\"speed\":"   + String(WEB_SPEED) + ",";
  j += "\"glow\":"    + String(WEB_GLOW) + ",";
  j += "\"freq\":"    + String(WEB_FREQ) + ",";
  j += "\"hue\":"     + String(TEXT_HUE) + ",";
  j += "\"rainbow\":" + String(TEXT_RAINBOW ? 1 : 0) + ",";
  j += "\"sched\":"   + String(SCHED_ON ? 1 : 0) + ",";
  j += "\"on\":\""    + hhmm(ON_MIN) + "\",";
  j += "\"off\":\""   + hhmm(OFF_MIN) + "\",";
  j += "\"tz\":"      + String(TZ_INDEX) + ",";
  j += "\"time\":\""  + currentTimeStr() + "\",";
  j += "\"msg\":\""   + jsonEscape(TEXT_MSG) + "\",";
  j += "\"lifemode\":" + String(lifeMode) + ",";
  j += "\"tzs\":[";
  for (int i = 0; i < TZ_COUNT; i++) { if (i) j += ","; j += "\"" + String(TZ_NAMES[i]) + "\""; }
  j += "],\"lifemodes\":[";
  for (int i = 0; i < LIFE_MODE_COUNT; i++) { if (i) j += ","; j += "\"" + String(LIFE_MODES[i]) + "\""; }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleSet() {
  if (server.hasArg("pattern")) setPatternByName(server.arg("pattern").c_str());
  if (server.hasArg("power"))   POWER_ON = server.arg("power").toInt() != 0;
  if (server.hasArg("bri")) {
    MASTER_BRIGHTNESS = (uint8_t)server.arg("bri").toInt();
    fxBright[gPatternIndex] = MASTER_BRIGHTNESS;          // remember per effect
    FastLED.setBrightness(MASTER_BRIGHTNESS);
  }
  if (server.hasArg("speed")) {
    applySpeed((uint8_t)server.arg("speed").toInt());
    fxSpeed[gPatternIndex] = WEB_SPEED;                   // remember per effect
  }
  if (server.hasArg("glow")) {
    int g = server.arg("glow").toInt(); if (g < 1) g = 1; if (g > 10) g = 10;
    WEB_GLOW = (uint8_t)g; fxGlow[gPatternIndex] = WEB_GLOW;   // remember per effect
  }
  if (server.hasArg("freq")) {
    int f = server.arg("freq").toInt(); if (f < 1) f = 1; if (f > 10) f = 10;
    WEB_FREQ = (uint8_t)f; fxFreq[gPatternIndex] = WEB_FREQ;   // remember per effect
  }
  if (server.hasArg("hue")) {
    TEXT_HUE = (uint8_t)server.arg("hue").toInt();
    fxHue[gPatternIndex] = TEXT_HUE;                      // remember per effect
  }
  if (server.hasArg("rainbow")) TEXT_RAINBOW = server.arg("rainbow").toInt() != 0;
  if (server.hasArg("sched")) { SCHED_ON = server.arg("sched").toInt() != 0; schedDirty = true; }
  if (server.hasArg("on")  && server.arg("on").indexOf(':')  > 0) { ON_MIN  = parseHHMM(server.arg("on"));  schedDirty = true; }
  if (server.hasArg("off") && server.arg("off").indexOf(':') > 0) { OFF_MIN = parseHHMM(server.arg("off")); schedDirty = true; }
  if (server.hasArg("tz")) {
    int idx = server.arg("tz").toInt();
    if (idx >= 0 && idx < TZ_COUNT) { TZ_INDEX = (uint8_t)idx; applyTimezone(); }
  }
  if (server.hasArg("lifemode")) {
    int m = server.arg("lifemode").toInt();
    if (m >= 0 && m < LIFE_MODE_COUNT) lifeMode = (uint8_t)m;
  }
  if (server.hasArg("msg")) {
    String m = server.arg("msg");
    strncpy(TEXT_MSG, m.c_str(), sizeof(TEXT_MSG) - 1);
    TEXT_MSG[sizeof(TEXT_MSG) - 1] = 0;
    textScrollX = 0;                       // restart scroll with the new message
  }
  markDirty();                             // persist this change (debounced)
  server.send(200, "text/plain", "ok");
}

// Draw a short status word centered on the panel using the 5x7 font (via XY()).
void drawText5x7(const char* s, int x0, int y0, uint8_t hue, uint8_t sat, uint8_t val) {
  int x = x0;
  for (; *s; s++) {
    uint8_t ch = (uint8_t)*s;
    for (int col = 0; col < FONT_W; col++) {
      uint8_t bits = pgm_read_byte(&font5x7[ch * 5 + col]);
      for (int gy = 0; gy < FONT_H; gy++)
        if ((bits >> gy) & 1) XY(x + col, y0 + gy) = CHSV(hue, sat, val);
    }
    x += FONT_W + 1;
  }
}
void showStatus(const char* s, uint8_t hue) {
  FastLED.clear();
  int w  = (int)strlen(s) * (FONT_W + 1) - 1;
  int x0 = (LEDS_PER_ROW - w) / 2; if (x0 < 0) x0 = 0;
  drawText5x7(s, x0, (NUM_ROWS - FONT_H) / 2, hue, 255, 200);
  FastLED.show();
}

bool servicesStarted = false;

// Runs once when WiFi connects: bring up mDNS, NTP, OTA, and the web server.
void onConnected() {
  servicesStarted = true;
  prefs.begin("wallart", false); prefs.putBool("wifiok", true); prefs.end();   // remember WiFi is configured
  Serial.print("WiFi OK, IP: "); Serial.println(WiFi.localIP());
  if (MDNS.begin(HOSTNAME)) { MDNS.addService("http", "tcp", 80); Serial.printf("Open http://%s.local\n", HOSTNAME); }
  applyTimezone();
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { FastLED.clear(true); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
  ArduinoOTA.begin();
  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/set", HTTP_GET, handleSet);
  server.begin();
  Serial.println("services up (web + OTA)");
}

// Start networking WITHOUT blocking: if WiFi was configured before, just begin the
// async connection (loop() shows NET and retries until it's up). Only an
// unconfigured device opens the blocking setup portal.
void startNetwork() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  showStatus("NET", 32);
  prefs.begin("wallart", true);
  bool configured = prefs.getBool("wifiok", false);
  prefs.end();
  if (configured) {
    WiFi.begin();                                  // stored creds, async -- do NOT block
  } else {
    WiFiManager wm;                                // first-time setup: blocking portal
    wm.setConfigPortalTimeout(180);
    wm.setAPCallback([](WiFiManager*){ showStatus("SET", 32); });
    wm.autoConnect("WallArt-Setup", AP_PASSWORD);
  }
}

// Hold RESET_BTN_PIN (default = onboard BOOT button) for 3s to wipe saved WiFi
// and reboot into the setup hotspot. Panel flashes red to confirm.
uint32_t btnDownSince = 0;
void checkResetButton() {
  if (digitalRead(RESET_BTN_PIN) == LOW) {            // active-low (pressed)
    if (btnDownSince == 0) btnDownSince = millis();
    else if (millis() - btnDownSince > 3000) {
      Serial.println("Reset button held -> clearing WiFi settings");
      fill_solid(&leds[0][0], NUM_ROWS * LEDS_PER_ROW, CRGB::Red);   // visual confirm
      FastLED.show();
      if (settingsDirty) { settingsDirty = false; saveSettings(); }  // don't lose a pending change
      prefs.begin("wallart", false); prefs.putBool("wifiok", false); prefs.end();  // force setup portal next boot
      WiFiManager wm;
      wm.resetSettings();
      delay(500);
      ESP.restart();
    }
  } else {
    btnDownSince = 0;                                  // released -> reset the timer
  }
}

void setup() {
  delay(500);                       // let the 5V rail stabilize before drawing current
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] v78 Lightning pattern");
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  for (int i = 0; i < MAX_PATTERNS; i++) { fxSpeed[i] = 2; fxBright[i] = 255; fxHue[i] = 150; fxGlow[i] = 5; fxFreq[i] = 5; }  // per-effect defaults
  for (int i = 0; i < PATTERN_COUNT; i++) {              // per-effect default colors / shaping
    if (!strcmp(PATTERN_NAMES[i], "Plasma")) fxHue[i] = 160;   // blue
    if (!strcmp(PATTERN_NAMES[i], "Fire"))   fxHue[i] = 0;     // warm red/orange base
    if (!strcmp(PATTERN_NAMES[i], "Starfield")) fxHue[i] = 0;  // heat (warm) base
    if (!strcmp(PATTERN_NAMES[i], "Scope")) { fxHue[i] = 96; fxGlow[i] = 7; fxFreq[i] = 5; }  // green scope, glow~12/freq~20
    if (!strcmp(PATTERN_NAMES[i], "Balls")) { fxSpeed[i] = 4; fxGlow[i] = 7; }  // lively bounce, medium trail
    if (!strcmp(PATTERN_NAMES[i], "Meteors")) { fxSpeed[i] = 4; fxGlow[i] = 6; }  // brisk fall, streaky trail
    if (!strcmp(PATTERN_NAMES[i], "Lightning")) { fxHue[i] = 160; fxSpeed[i] = 6; fxGlow[i] = 4; fxFreq[i] = 6; }  // blue-white, medium fade/width, ~3 on screen
  }
  loadSettings();                 // restore saved settings (or first-boot defaults)

  FastLED.addLeds<WS2812B, 23, GRB>(leds[0], LEDS_PER_ROW);
  FastLED.addLeds<WS2812B, 22, GRB>(leds[1], LEDS_PER_ROW);
  FastLED.addLeds<WS2812B, 21, GRB>(leds[2], LEDS_PER_ROW);
  FastLED.addLeds<WS2812B, 19, GRB>(leds[3], LEDS_PER_ROW);
  FastLED.addLeds<WS2812B, 18, GRB>(leds[4], LEDS_PER_ROW);
  FastLED.addLeds<WS2812B,  5, GRB>(leds[5], LEDS_PER_ROW);
  FastLED.addLeds<WS2812B, 17, GRB>(leds[6], LEDS_PER_ROW);
  FastLED.addLeds<WS2812B, 16, GRB>(leds[7], LEDS_PER_ROW);
  FastLED.addLeds<WS2812B,  2, GRB>(leds[8], LEDS_PER_ROW);   // rows 9 & 10 pins swapped (were wired backwards)
  FastLED.addLeds<WS2812B,  4, GRB>(leds[9], LEDS_PER_ROW);

  FastLED.setBrightness(MASTER_BRIGHTNESS);
  applySpeed(WEB_SPEED);          // apply saved/default speed to both patterns
  random16_set_seed(esp_random());

  // stagger every cell so the random-pattern fades don't pulse in unison
  for (int r = 0; r < NUM_ROWS; r++) {
    for (int c = 0; c < LEDS_PER_ROW; c++) {
      cellHue[r][c]   = random8();
      cellLevel[r][c] = random8(1, MAX_BRIGHTNESS);
      bool goingIn    = random8(2);
      cellStep[r][c]  = goingIn ?  stepForMs(jitter(FADE_IN_MS))
                                : -stepForMs(jitter(FADE_OUT_MS));
    }
  }
  FastLED.clear(true);

  startNetwork();   // captive-portal WiFi, mDNS, web server
}

void loop() {
  if (WiFi.status() == WL_CONNECTED && !servicesStarted) onConnected();  // first connect -> bring up services
  if (servicesStarted) {          // only valid once WiFi is up
    server.handleClient();        // service web requests every iteration
    ArduinoOTA.handle();          // wireless firmware updates
  }
  checkResetButton();             // hold BOOT button 3s to clear WiFi
  if (settingsDirty && millis() - lastChange > 2000) { settingsDirty = false; saveSettings(); }  // debounced save

  if (millis() - lastFrame < FRAME_MS) return;
  lastFrame = millis();

  applySchedule();                 // may auto-toggle POWER_ON at the on/off times
  static bool wasOn = true;
  if (!POWER_ON) {                 // off (manual or scheduled) -> dark
    if (wasOn) { FastLED.clear(); FastLED.show(); wasOn = false; }   // blank once, then idle
    return;
  }
  wasOn = true;

  static uint32_t lastReconnect = 0;
  if (WiFi.status() != WL_CONNECTED) {              // WiFi lost -> show NET, keep retrying
    showStatus("NET", 32);
    if (millis() - lastReconnect > 8000) { lastReconnect = millis(); WiFi.reconnect(); }
    return;
  }

  PATTERN_FNS[gPatternIndex]();   // run the active pattern
  FastLED.show();
}
