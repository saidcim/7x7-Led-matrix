// 7x7 LED Matrix - ESP32-C6 SuperMini + HT16K33
// Each button press cycles to the next animation
// SDA = GP6, SCL = GP7, Button = GP8 (pull-up, active LOW)
//
// Pin mapping (verified from schematic):
//   COM0..COM6  →  Vcc1..Vcc7  →  LED ROWS    (anode)
//   ROW0..ROW6  →  Gnd1..Gnd7  →  LED COLUMNS (cathode)
//   ROW0/A2, ROW1/A1, ROW2/A0 serve dual purpose: address pull-down (10k→GND) and column drive
//
// HT16K33 register logic:
//   display[row]  =  bitmask of the 7 columns in that row
//   bit[col]      =  state of the LED in that column
//   → setPixel(row, col): display[row] |= (1 << col)

#include <Wire.h>

// ── Pin definitions ───────────────────────────────────────
#define SDA_PIN   6
#define SCL_PIN   7
#define BTN_PIN   8

// ── HT16K33 I2C address ────────────────────────────────────
#define HT16K33_ADDR 0x70

// ── Matrix dimensions ──────────────────────────────────────
#define ROWS 7
#define COLS 7

// ── Animation list ─────────────────────────────────────────
// HT16K33: COM0-COM6 = rows, ROW0-ROW6 = columns
// Bits in display[row] indicate which columns are lit
// display[row] bit0 = ROW0, bit1 = ROW1, ... bit6 = ROW6

uint16_t display[8] = {0}; // HT16K33 has 16 half-word registers; we use 7 (one per row)

// ── Button debounce ────────────────────────────────────────
unsigned long lastBtnPress = 0;
#define DEBOUNCE_MS 250

// ── Animation count & index ────────────────────────────────
#define ANIM_COUNT 27
int currentAnim = 0;
unsigned long lastStep = 0;
int animStep = 0;

// Animation names (for serial monitor)
const char* animNames[ANIM_COUNT] = {
  "Wave ->", "Wave |", "Snake", "Spiral",
  "Propeller", "Rain", "Checker", "Pulse",
  "Random", "Corners", "Cross", "Fire",
  "Digital Rain", "Radar", "Border", "Diamond",
  "Equalizer", "Ripple", "Life", "Explosion",
  "Plasma", "Heart", "Clock", "SOS",
  "Knight", "Diagonal", "Fill"
};

// ── Helper: clear matrix buffer ────────────────────────────
void clearBuf() {
  for (int i = 0; i < 8; i++) display[i] = 0; // 8 registers (COM0..COM6 used)
}

// ── Helper: set a single pixel ─────────────────────────────
// HT16K33: COM = row, ROW = column
// COM0..COM6 → maps to display[0..6] registers
// ROW0..ROW6 → bits 0..6
void setPixel(int row, int col, bool on) {
  if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
  if (on) display[row] |=  (1 << col);
  else    display[row] &= ~(1 << col);
}

bool getPixel(int row, int col) {
  if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return false;
  return (display[row] >> col) & 1;
}

void fillAll(bool on) {
  uint16_t val = on ? 0x7F : 0x00; // 7 bit
  for (int r = 0; r < ROWS; r++) display[r] = val;
}

// ── HT16K33 driver functions ───────────────────────────────
void ht16k33_cmd(uint8_t cmd) {
  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(cmd);
  Wire.endTransmission();
}

void ht16k33_init() {
  ht16k33_cmd(0x21);        // Turn on oscillator
  ht16k33_cmd(0x81);        // Display ON, blink OFF
  ht16k33_cmd(0xEF);        // Maximum brightness (0xE0 | 0x0F)
}

void ht16k33_write() {
  Wire.beginTransmission(HT16K33_ADDR);
  Wire.write(0x00); // Starting register address
  for (int i = 0; i < 8; i++) {
    Wire.write(display[i] & 0xFF);         // Column bitmask for COM[i] row
    Wire.write((display[i] >> 8) & 0xFF);  // High byte (upper 9 bits unused in HT16K33)
  }
  Wire.endTransmission();
}

// ══════════════════════════════════════════════════════════════
// ANIMATION STATE VARIABLES
// ══════════════════════════════════════════════════════════════

// Fire animation buffer
bool fireGrid[ROWS][COLS] = {};

// Rain animation buffer
uint8_t rainDrops[COLS] = {};

// Digital Rain drop positions
int matrixDrops[COLS];

// Conway's Life next-generation buffer
bool lifeNext[ROWS][COLS] = {};

// Life initialization flag
bool lifeInited = false;

// Radar sweep angle
float radarAngle = 0.0f;

// Plasma time counter
float plasmaT = 0.0f;

// Ripple
float rippleRadius = 0.0f;
int rippleOriginR = 3, rippleOriginC = 3;

// Knight trail positions
int knightR = 3, knightC = 3;
int knightTrailR[8], knightTrailC[8];
int knightTrailLen = 0;

// Propeller rotation angle
float propAngle = 0.0f;

// Equalizer bar heights and targets
int eqHeight[COLS];
int eqTarget[COLS];

// Clock tick counter
int clockTick = 0;

// SOS morse code index
const uint8_t sosUnits[] = {1,0,1,0,1,0, 0,0, 1,1,1,0,1,1,1,0,1,1,1,0, 0,0, 1,0,1,0,1};
#define SOS_LEN 27
int sosIndex = 0;

// Spiral traversal path
int spiralPath[ROWS * COLS][2];
int spiralLen = 0;
bool spiralBuilt = false;

// Border traversal path
int borderPath[4 * ROWS][2];
int borderLen = 0;
bool borderBuilt = false;

// Heartbeat phase counter
int heartBeat = 0;

// ── Build spiral traversal path ───────────────────────────
void buildSpiral() {
  int top = 0, bottom = ROWS-1, left = 0, right = COLS-1;
  spiralLen = 0;
  while (top <= bottom && left <= right) {
    for (int c = left; c <= right; c++) { spiralPath[spiralLen][0] = top;    spiralPath[spiralLen][1] = c; spiralLen++; }
    top++;
    for (int r = top; r <= bottom; r++) { spiralPath[spiralLen][0] = r;      spiralPath[spiralLen][1] = right; spiralLen++; }
    right--;
    if (top <= bottom) { for (int c = right; c >= left; c--) { spiralPath[spiralLen][0] = bottom; spiralPath[spiralLen][1] = c; spiralLen++; } bottom--; }
    if (left <= right) { for (int r = bottom; r >= top;  r--) { spiralPath[spiralLen][0] = r;      spiralPath[spiralLen][1] = left;  spiralLen++; } left++; }
  }
  spiralBuilt = true;
}

// ── Build border traversal path ───────────────────────────
void buildBorder() {
  int last = ROWS - 1;
  borderLen = 0;
  for (int c = 0; c < COLS; c++) { borderPath[borderLen][0] = 0;    borderPath[borderLen][1] = c; borderLen++; }
  for (int r = 1; r <= last; r++) { borderPath[borderLen][0] = r;    borderPath[borderLen][1] = last; borderLen++; }
  for (int c = last-1; c >= 0; c--) { borderPath[borderLen][0] = last; borderPath[borderLen][1] = c; borderLen++; }
  for (int r = last-1; r > 0; r--) { borderPath[borderLen][0] = r;    borderPath[borderLen][1] = 0; borderLen++; }
  borderBuilt = true;
}

// ── Initialize Conway's Life with random cells ────────────
void lifeInit() {
  clearBuf();
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++)
      setPixel(r, c, random(100) < 32);
  lifeInited = true;
}

// ── Draw propeller blades ──────────────────────────────────
void drawPropeller(float angle) {
  clearBuf();
  float cx = 3.0f, cy = 3.0f;
  int blades = 3;
  for (int b = 0; b < blades; b++) {
    float ba = angle + b * (2.0f * PI / blades);
    float perp = ba + PI / 2.0f;
    for (int i = 0; i <= 12; i++) {
      float dist = (i / 12.0f) * 3.5f;
      for (int w = -1; w <= 1; w++) {
        float wr = w * 0.5f;
        float row = cy + dist * sinf(ba) + wr * sinf(perp);
        float col = cx + dist * cosf(ba) + wr * cosf(perp);
        setPixel((int)roundf(row), (int)roundf(col), true);
      }
    }
  }
  setPixel(3, 3, true);
}

// ── Draw heart shape ───────────────────────────────────────
void drawHeart(float scale, float offR, float offC) {
  clearBuf();
  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      float x = (c - offC) / scale;
      float y = -(r - offR) / scale;
      float v = powf(x*x + y*y - 1.0f, 3.0f) - x*x * y*y*y;
      if (v <= 0.05f) setPixel(r, c, true);
    }
  }
}

// ── Draw clock hand ────────────────────────────────────────
void drawHand(float cr, float cc, float angle, float len) {
  for (float d = 0; d <= len; d += 0.5f) {
    int r = (int)roundf(cr + d * sinf(angle));
    int c = (int)roundf(cc + d * cosf(angle));
    setPixel(r, c, true);
  }
}

// ══════════════════════════════════════════════════════════════
// ANIMATION STEP FUNCTION (advances one frame per call)
// ══════════════════════════════════════════════════════════════

// Returns the interval in ms before the next frame should run
int runAnimation() {
  switch (currentAnim) {

    // 0 - Wave →
    case 0: {
      int col = animStep % COLS;
      clearBuf();
      for (int r = 0; r < ROWS; r++) setPixel(r, col, true);
      return 80;
    }

    // 1 - Wave ↓
    case 1: {
      int row = animStep % ROWS;
      clearBuf();
      for (int c = 0; c < COLS; c++) setPixel(row, c, true);
      return 80;
    }

    // 2 - Snake (trail=6)
    case 2: {
      // Build snake-order traversal path
      static int snakePath[ROWS*COLS][2];
      static bool snakeBuilt = false;
      if (!snakeBuilt) {
        int idx = 0;
        for (int r = 0; r < ROWS; r++) {
          if (r % 2 == 0) { for (int c = 0; c < COLS; c++) { snakePath[idx][0]=r; snakePath[idx][1]=c; idx++; } }
          else             { for (int c = COLS-1; c >= 0; c--) { snakePath[idx][0]=r; snakePath[idx][1]=c; idx++; } }
        }
        snakeBuilt = true;
      }
      int total = ROWS * COLS;
      int head = animStep % total;
      int trail = 6;
      clearBuf();
      for (int t = 0; t < trail; t++) {
        int p = (head - t + total) % total;
        setPixel(snakePath[p][0], snakePath[p][1], true);
      }
      return 60;
    }

    // 3 - Spiral
    case 3: {
      if (!spiralBuilt) buildSpiral();
      int total = spiralLen;
      int head = animStep % (total + 8);
      clearBuf();
      for (int t = 0; t <= head && t < total; t++) {
        setPixel(spiralPath[t][0], spiralPath[t][1], true);
      }
      if (head >= total + 7) animStep = -1; // wrap back to start
      return 60;
    }

    // 4 - Propeller
    case 4: {
      drawPropeller(propAngle);
      propAngle += PI / 10.0f;
      if (propAngle >= 2 * PI) propAngle -= 2 * PI;
      return 45;
    }

    // 5 - Rain
    case 5: {
      // Shift columns downward
      for (int c = 0; c < COLS; c++) {
        for (int r = ROWS-1; r > 0; r--) {
          bool above = getPixel(r-1, c);
          setPixel(r, c, above);
        }
        setPixel(0, c, random(100) < 35);
      }
      return 120;
    }

    // 6 - Checker
    case 6: {
      int phase = animStep % 2;
      clearBuf();
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
          if ((r + c + phase) % 2 == 0) setPixel(r, c, true);
      return 240;
    }

    // 7 - Pulse
    case 7: {
      float radius = (animStep % 14) * 0.5f;
      float cx = 3.0f, cy = 3.0f;
      clearBuf();
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float d = sqrtf((r-cx)*(r-cx) + (c-cy)*(c-cy));
          if (fabsf(d - radius) < 0.6f) setPixel(r, c, true);
        }
      return 70;
    }

    // 8 - Random
    case 8: {
      int r = random(ROWS);
      int c = random(COLS);
      setPixel(r, c, !getPixel(r, c));
      return 80;
    }

    // 9 - Corners
    case 9: {
      int last = ROWS - 1;
      int corners[4][2] = {{0,0},{0,last},{last,0},{last,last}};
      clearBuf();
      for (int i = 0; i < 4; i++) setPixel(corners[i][0], corners[i][1], true);
      int active = animStep % 4;
      int ar = corners[active][0], ac = corners[active][1];
      for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++)
          setPixel(ar+dr, ac+dc, true);
      return 320;
    }

    // 10 - Cross
    case 10: {
      int mid = ROWS / 2;
      clearBuf();
      if (animStep % 2 == 0) {
        for (int c = 0; c < COLS; c++) setPixel(mid, c, true);
      } else {
        for (int r = 0; r < ROWS; r++) setPixel(r, mid, true);
      }
      return 240;
    }

    // 11 - Fire
    case 11: {
      // First frame: seed the bottom row randomly
      if (animStep == 0) {
        for (int c = 0; c < COLS; c++)
          fireGrid[ROWS-1][c] = random(100) < 75;
      }
      // Compute next fire grid
      bool nextGrid[ROWS][COLS] = {};
      for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
          if (r == ROWS-1) {
            nextGrid[r][c] = random(100) < 65;
          } else {
            bool b  = fireGrid[r+1][c];
            bool bl = (c > 0)      && fireGrid[r+1][c-1];
            bool br = (c < COLS-1) && fireGrid[r+1][c+1];
            nextGrid[r][c] = (b || bl || br) && (random(100) < 78);
          }
        }
      }
      clearBuf();
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          fireGrid[r][c] = nextGrid[r][c];
          if (nextGrid[r][c]) setPixel(r, c, true);
        }
      return 60;
    }

    // 12 - Digital Rain
    case 12: {
      if (animStep == 0) {
        for (int c = 0; c < COLS; c++)
          matrixDrops[c] = random(-ROWS, 0);
      }
      clearBuf();
      for (int c = 0; c < COLS; c++) {
        for (int t = 0; t < 5; t++) {
          int r = matrixDrops[c] - t;
          if (r >= 0 && r < ROWS) setPixel(r, c, true);
        }
        matrixDrops[c]++;
        if (matrixDrops[c] > ROWS + 5) matrixDrops[c] = random(-4, 0);
      }
      return 52;
    }

    // 13 - Radar
    case 13: {
      float cx = 3.0f, cy = 3.0f;
      clearBuf();
      setPixel(3, 3, true);
      for (float d = 1.0f; d < 6.0f; d += 0.5f) {
        for (int t = 0; t < 6; t++) {
          float a = radarAngle - t * 0.12f;
          int r = (int)roundf(cx + d * sinf(a));
          int c = (int)roundf(cy + d * cosf(a));
          setPixel(r, c, true);
        }
      }
      radarAngle += 0.18f;
      if (radarAngle >= 2 * PI) radarAngle -= 2 * PI;
      return 52;
    }

    // 14 - Border
    case 14: {
      if (!borderBuilt) buildBorder();
      int trail = 5;
      clearBuf();
      int head = animStep % borderLen;
      for (int t = 0; t < trail; t++) {
        int p = (head - t + borderLen) % borderLen;
        setPixel(borderPath[p][0], borderPath[p][1], true);
      }
      return 56;
    }

    // 15 - Diamond
    case 15: {
      float mid = 3.0f;
      float size = (animStep % 20) * 0.5f;
      clearBuf();
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
          if (fabsf((float)(r) - mid) + fabsf((float)(c) - mid) <= size + 0.35f &&
              fabsf((float)(r) - mid) + fabsf((float)(c) - mid) >= size - 0.35f)
            setPixel(r, c, true);
      return 64;
    }

    // 16 - Equalizer
    case 16: {
      if (animStep == 0) {
        for (int c = 0; c < COLS; c++) { eqHeight[c] = 3; eqTarget[c] = 3; }
      }
      for (int c = 0; c < COLS; c++) {
        if (random(100) < 18) eqTarget[c] = random(0, ROWS);
        if (eqHeight[c] < eqTarget[c]) eqHeight[c]++;
        else if (eqHeight[c] > eqTarget[c]) eqHeight[c]--;
      }
      clearBuf();
      for (int c = 0; c < COLS; c++)
        for (int r = ROWS - eqHeight[c]; r < ROWS; r++)
          setPixel(r, c, true);
      return 44;
    }

    // 17 - Ripple
    case 17: {
      clearBuf();
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float d = sqrtf((float)(r-rippleOriginR)*(r-rippleOriginR) + (float)(c-rippleOriginC)*(c-rippleOriginC));
          if (fabsf(d - rippleRadius) < 0.55f) setPixel(r, c, true);
        }
      rippleRadius += 0.55f;
      if (rippleRadius > 9.0f) {
        rippleRadius = 0;
        rippleOriginR = random(ROWS);
        rippleOriginC = random(COLS);
      }
      return 56;
    }

    // 18 - Life (Conway)
    case 18: {
      if (!lifeInited || animStep == 0) lifeInit();
      for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
          int n = 0;
          for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++) {
              if (!dr && !dc) continue;
              n += getPixel(r+dr, c+dc) ? 1 : 0;
            }
          bool alive = getPixel(r, c);
          lifeNext[r][c] = (n == 3) || (alive && (n == 2 || n == 3));
        }
      }
      clearBuf();
      bool anyAlive = false;
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          if (lifeNext[r][c]) { setPixel(r, c, true); anyAlive = true; }
        }
      if (!anyAlive) lifeInited = false; // reset if population dies out
      return 144;
    }

    // 19 - Explosion
    case 19: {
      float cx = 3.0f, cy = 3.0f;
      float radius = (animStep % 14) * 0.65f;
      clearBuf();
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float d = sqrtf((r-cx)*(r-cx) + (c-cy)*(c-cy));
          if (d >= radius - 0.7f && d <= radius + 0.4f) setPixel(r, c, true);
        }
      return 40;
    }

    // 20 - Plasma
    case 20: {
      clearBuf();
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
          float v = sinf(r * 0.55f + plasmaT)
                  + sinf(c * 0.45f + plasmaT * 1.3f)
                  + sinf((r + c) * 0.35f + plasmaT * 0.7f);
          if (v > 0.85f) setPixel(r, c, true);
        }
      plasmaT += 0.28f;
      return 36;
    }

    // 21 - Heart
    case 21: {
      float cx = 3.0f, cy = 3.0f;
      float scale;
      int phase = heartBeat % 14;
      if (phase < 2)       scale = 1.85f;
      else if (phase == 2) scale = 2.45f;
      else if (phase == 3) scale = 2.15f;
      else                 scale = 2.1f;
      drawHeart(scale, cx, cy);
      heartBeat++;
      return 56;
    }

    // 22 - Clock
    case 22: {
      float cx = 3.0f, cy = 3.0f;
      clearBuf();
      setPixel(3, 3, true);
      float secA = (clockTick / 8.0f) * 2 * PI - PI / 2;
      float minA = (clockTick / 48.0f) * 2 * PI - PI / 2;
      drawHand(cx, cy, secA, 3.0f * 0.42f);
      drawHand(cx, cy, minA, 3.0f * 0.30f);
      // Hour markers
      for (int i = 0; i < 12; i++) {
        float a = (i / 12.0f) * 2 * PI - PI / 2;
        setPixel((int)roundf(cx + 2.65f * sinf(a)),
                 (int)roundf(cy + 2.65f * cosf(a)), true);
      }
      clockTick++;
      return 120;
    }

    // 23 - SOS
    case 23: {
      clearBuf();
      if (sosUnits[sosIndex % SOS_LEN]) {
        for (int r = 2; r <= 4; r++)
          for (int c = 0; c < COLS; c++)
            setPixel(r, c, true);
      }
      sosIndex++;
      return 28;
    }

    // 24 - Knight
    case 24: {
      int moves[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
      if (animStep == 0) {
        knightR = 3; knightC = 3; knightTrailLen = 1;
        knightTrailR[0] = 3; knightTrailC[0] = 3;
      }
      // Find valid knight moves
      int opts[8][2]; int optCount = 0;
      for (int m = 0; m < 8; m++) {
        int nr = knightR + moves[m][0];
        int nc = knightC + moves[m][1];
        if (nr >= 0 && nr < ROWS && nc >= 0 && nc < COLS) {
          opts[optCount][0] = nr; opts[optCount][1] = nc; optCount++;
        }
      }
      if (optCount > 0) {
        int pick = random(optCount);
        knightR = opts[pick][0]; knightC = opts[pick][1];
        int maxTrail = 6;
        if (knightTrailLen < maxTrail) {
          knightTrailR[knightTrailLen] = knightR;
          knightTrailC[knightTrailLen] = knightC;
          knightTrailLen++;
        } else {
          for (int i = 0; i < maxTrail-1; i++) { knightTrailR[i] = knightTrailR[i+1]; knightTrailC[i] = knightTrailC[i+1]; }
          knightTrailR[maxTrail-1] = knightR;
          knightTrailC[maxTrail-1] = knightC;
        }
      }
      clearBuf();
      for (int i = 0; i < knightTrailLen; i++) setPixel(knightTrailR[i], knightTrailC[i], true);
      return 68;
    }

    // 25 - Diagonal
    case 25: {
      int offset = animStep % COLS;
      clearBuf();
      for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
          if ((r + c) % COLS == offset) setPixel(r, c, true);
      return 72;
    }

    // 26 - Fill
    case 26: {
      int total = ROWS * COLS;
      int step = animStep % (total + 8);
      clearBuf();
      for (int i = 0; i <= step && i < total; i++) {
        setPixel(i / COLS, i % COLS, true);
      }
      return 32;
    }

    default: return 100;
  }
}

// ══════════════════════════════════════════════════════════════
// SETUP & LOOP
// ══════════════════════════════════════════════════════════════

void resetAnimState() {
  animStep = 0;
  propAngle = 0;
  radarAngle = 0;
  plasmaT = 0;
  rippleRadius = 0;
  rippleOriginR = 3; rippleOriginC = 3;
  knightR = 3; knightC = 3; knightTrailLen = 0;
  heartBeat = 0;
  clockTick = 0;
  sosIndex = 0;
  lifeInited = false;
  spiralBuilt = false;
  // border path is built once in setup, no need to rebuild
  clearBuf();
  ht16k33_write();
  Serial.print("Animation: ");
  Serial.println(animNames[currentAnim]);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Init I2C with custom pins
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000); // 400 kHz Fast Mode

  // Button input
  pinMode(BTN_PIN, INPUT_PULLUP);

  // Init HT16K33 driver
  ht16k33_init();
  clearBuf();
  ht16k33_write();

  // Pre-build border traversal path
  buildBorder();

  Serial.println("7x7 LED Matrix Ready");
  Serial.print("Animation: ");
  Serial.println(animNames[currentAnim]);
}

void loop() {
  // ── Button check ───────────────────────────────────────────
  if (digitalRead(BTN_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastBtnPress > DEBOUNCE_MS) {
      lastBtnPress = now;
      currentAnim = (currentAnim + 1) % ANIM_COUNT;
      resetAnimState();
      delay(50); // Short delay for extra debounce safety
    }
  }

  // ── Animation step ─────────────────────────────────────────
  unsigned long now = millis();
  int interval = runAnimation();

  if (now - lastStep >= (unsigned long)interval) {
    lastStep = now;
    ht16k33_write();
    animStep++;
  }
}
