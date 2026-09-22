/*
 * ============================================================================
 *  CHRONO COURSE - Adafruit MatrixPortal S3 + 6 panneaux HUB75 (2 x 3)
 *  Ecran virtuel 192 x 64 - Affichage 7 segments rectangulaire, format H:MM:SS
 *  Lib : mrcodetastic ESP32-HUB75-MatrixPanel-DMA (VirtualMatrixPanel_T, version actuelle)
 * ----------------------------------------------------------------------------
 *  COMMANDES (bouton externe guidon ET bouton UP embarque, memes gestes) :
 *    Appui COURT  = demarre (top de depart immediat) / reprend apres pause
 *    DOUBLE appui = pause          (evite toute pause accidentelle)
 *    Appui LONG   = reset          (evite tout reset accidentel)
 *    Bouton DOWN (GPIO 7, embarque) = reset instantane (confort etabli, hors course)
 * ============================================================================
 */

#include <Arduino.h>
#include <ESP32-HUB75-VirtualMatrixPanel_T.hpp>  // lib mrcodetastic (version actuelle)
#include "logo147.h"                              // logo 147 (bitmap RGB565, meme dossier)

// ------------------------- Config panneaux ---------------------------------
#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define NUM_ROWS    2
#define NUM_COLS    3
#define PANEL_CHAIN (NUM_ROWS * NUM_COLS)   // = 6
#define PANEL_CHAIN_TYPE CHAIN_TOP_RIGHT_DOWN

// ------------------------- Pins HUB75 du MatrixPortal S3 -------------------
#define R1_PIN 42
#define G1_PIN 41
#define B1_PIN 40
#define R2_PIN 38
#define G2_PIN 39
#define B2_PIN 37
#define A_PIN  45
#define B_PIN  36
#define C_PIN  48
#define D_PIN  35
#define E_PIN  21
#define LAT_PIN 47
#define OE_PIN  14
#define CLK_PIN 2

// ------------------------- Boutons -----------------------------------------
#define BTN_START   6      // bouton UP embarque
#define BTN_RESET   7      // bouton DOWN embarque
#define EXT_BTN_PIN A1     // bouton externe (guidon) sur A1 du MatrixPortal S3 ; -1 pour desactiver.

// ------------------------- Reglages ----------------------------------------
#define BRIGHTNESS  180    // 0-255. Monte vers 255 en plein jour (=> plus de conso)

// Timing des gestes du bouton (ms)
#define LONG_MS     900    // maintien au-dela = appui long (reset)
#define DOUBLE_MS   300    // delai max entre 2 clics pour un "double appui"
#define DEBOUNCE_MS  30    // anti-rebond

// Couleur des segments, en R, V, B (0 a 255). Une ligne par etat du chrono.
// Pour une couleur unique quel que soit l'etat, mets les trois lignes identiques.
//   Primaires :  ROUGE 255,0,0   |  VERT 0,255,0   |  BLEU 0,0,255
//   Autres    :  blanc 255,255,255 | jaune 255,255,0 | cyan 0,255,255
//                magenta 255,0,255 | orange 255,140,0 | violet 128,0,255
#define COLOR_READY   255, 255, 255   // pret     : blanc
#define COLOR_RUNNING   0, 255,   0   // en cours : vert
#define COLOR_PAUSED  255, 140,   0   // en pause : orange

// Taille des chiffres (le bloc H:MM:SS se recentre tout seul quand tu changes ca).
#define DIGIT_W    28      // largeur d'un chiffre
#define DIGIT_H    44      // hauteur d'un chiffre
#define DIGIT_T     6      // epaisseur d'un segment
#define DIGIT_GAP   2      // espace entre 2 chiffres voisins
#define COLON_W     6      // largeur d'un bloc deux-points
#define COLON_GAP   3      // espace de chaque cote d'un deux-points

// Ecran d'accueil (etat READY) : logo a gauche, texte a droite, baseline en bas.
// Ecris les textes normalement avec accents (ils sont convertis a l'affichage).
#define ACCUEIL_TEXT       "Prêt ?"                          // a droite du logo ("Prêts ?" possible)
#define ACCUEIL_TEXT_SIZE  2                                 // taille 1..3 (2 conseille ici)
#define ACCUEIL_TEXT_COLOR 255, 255, 255                     // couleur du texte (R,V,B)
#define TAGLINE            "La course qui éclaire ta ville"  // baseline en bas ("" = aucune)
#define TAGLINE_COLOR      196, 214, 0                       // vert lime du logo (R,V,B)

const uint16_t VDISP_W = NUM_COLS * PANEL_RES_X;  // 192
const uint16_t VDISP_H = NUM_ROWS * PANEL_RES_Y;  // 64

MatrixPanel_I2S_DMA                    *dma_display = nullptr;
VirtualMatrixPanel_T<PANEL_CHAIN_TYPE> *disp        = nullptr;

// ------------------------- Etat du chrono ----------------------------------
enum State { READY, RUNNING, PAUSED };
State state = READY;
unsigned long accumulatedMs = 0;
unsigned long startMs       = 0;

// Gestes bouton : evenement + etat. DEFINIS ICI, avant toute fonction, sinon
// l'auto-generation de prototypes de l'IDE Arduino ne connait pas encore ces types.
enum BtnEvent { EVT_NONE, EVT_DOWN, EVT_SHORT, EVT_DOUBLE, EVT_LONG };
struct BtnState {
  bool          pressed   = false;  // etat stable (true = appuye)
  bool          lastRead  = false;  // derniere lecture brute
  unsigned long tChange   = 0;      // debut de la lecture brute courante
  unsigned long tDown     = 0;      // instant du dernier front descendant
  unsigned long tLastUp   = 0;      // instant du dernier relacher
  bool          longFired = false;  // appui long deja declenche pour cet appui
  uint8_t       clicks    = 0;      // clics accumules dans la fenetre double
};

// ------------------------- Table 7 segments --------------------------------
// bits : A=0 B=1 C=2 D=3 E=4 F=5 G=6
const uint8_t SEG[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

uint16_t colFor(State s) {
  switch (s) {
    case RUNNING: return disp->color565(COLOR_RUNNING);
    case PAUSED:  return disp->color565(COLOR_PAUSED);
    default:      return disp->color565(COLOR_READY);
  }
}

// Chiffre 7 segments 100% rectangulaire (que des fillRect, aucun triangle) :
//  - les 4 barres VERTICALES vont du haut/bas jusqu'au MILIEU et se rejoignent la
//    (verticales continues => pas de coupure haut/bas, meme sur 0/1/7) ;
//  - elles occupent les colonnes de bord => elles remplissent les COINS ;
//  - les 3 barres HORIZONTALES sont en retrait de DIGIT_T et butent contre elles.
void drawDigit(int x, int y, int w, int h, int t, int value, uint16_t c) {
  if (value < 0 || value > 9) return;
  uint8_t s = SEG[value];
  int mid = y + h / 2;
  int hw  = w - 2 * t;     // largeur des barres horizontales
  int upH = mid - y;       // verticale haute : haut  -> milieu
  int loH = (y + h) - mid; // verticale basse : milieu -> bas

  if (s & 0x01) disp->fillRect(x + t,     y,             hw, t,   c); // A (haut)
  if (s & 0x40) disp->fillRect(x + t,     y + (h - t)/2, hw, t,   c); // G (milieu)
  if (s & 0x08) disp->fillRect(x + t,     y + h - t,     hw, t,   c); // D (bas)
  if (s & 0x20) disp->fillRect(x,         y,             t,  upH, c); // F (haut-gauche)
  if (s & 0x02) disp->fillRect(x + w - t, y,             t,  upH, c); // B (haut-droite)
  if (s & 0x10) disp->fillRect(x,         mid,           t,  loH, c); // E (bas-gauche)
  if (s & 0x04) disp->fillRect(x + w - t, mid,           t,  loH, c); // C (bas-droite)
}

// Deux-points : dots a 1/4 et 3/4 de hauteur.
void drawColon(int x, int yTop, int h, int t, uint16_t c) {
  disp->fillRect(x, yTop +     h / 4 - t / 2, t, t, c);
  disp->fillRect(x, yTop + 3 * h / 4 - t / 2, t, t, c);
}

// Ecrit une chaine UTF-8 (accents FR) via la police integree (CP437).
// Les circonflexes (ê a i o u) sont rendus "lettre + vrai chapeau ^" dessine a la
// main, car le glyphe circonflexe de la police integree ressort en barre horizontale.
// draw=false : ne dessine rien, retourne juste le nombre de glyphes (pour centrer).
int frText(const char* s, bool draw, uint8_t size, uint16_t color) {
  int n = 0;
  while (*s) {
    uint8_t c = (uint8_t)*s;
    char base = 0;                                   // != 0 -> lettre + chapeau manuel
    if (c == 0xC3 && *(s + 1)) {                     // accent FR : 2 octets UTF-8
      s++;
      switch ((uint8_t)*s) {
        case 0xA9: c = 0x82; break;                  // é
        case 0xA8: c = 0x8A; break;                  // è
        case 0xAB: c = 0x89; break;                  // ë
        case 0xA0: c = 0x85; break;                  // à
        case 0xA7: c = 0x87; break;                  // ç
        case 0xAF: c = 0x8B; break;                  // ï
        case 0xB9: c = 0x97; break;                  // ù
        case 0x89: c = 0x90; break;                  // É
        case 0xAE: c = 0x8C; break;                  // î (point remplace par l'accent)
        case 0xAA: base = 'e'; break;                // ê -> e + chapeau dessine
        case 0xA2: base = 'a'; break;                // â
        case 0xB4: base = 'o'; break;                // ô
        case 0xBB: base = 'u'; break;                // û
        default:   c = '?';   break;
      }
    }
    if (draw) {
      if (base) {
        int gx = disp->getCursorX(), gy = disp->getCursorY();
        disp->write(base);                           // lettre de base
        int cx = gx + (5 * size) / 2;                // centre horizontal du glyphe
        int aw = 2 * size - 1;                       // demi-largeur : un peu + etroit que la lettre
        int ah = size;                               // hauteur compacte -> ~1 px d'ecart avant la lettre
        for (int t = 0; t < size; t++) {             // epaissir SANS descendre (garde l'ecart)
          disp->drawLine(cx + t, gy, cx - aw + t, gy + ah, color);
          disp->drawLine(cx - t, gy, cx + aw - t, gy + ah, color);
        }
      } else {
        disp->write(c);
      }
    }
    n++;
    s++;
  }
  return n;
}

// Ecran d'accueil (etat READY) : logo a gauche, texte a droite, baseline en bas.
void drawAccueil() {
  disp->fillScreen(0);
  disp->setFont();                                  // police integree
  const int baseH = (strlen(TAGLINE) > 0) ? 10 : 0; // hauteur reservee en bas
  const int topH  = VDISP_H - baseH;                // zone haute (logo + texte)

  // Logo (mark vert) a gauche, centre verticalement dans la zone haute
  const int lx = 3;
  const int ly = (topH - LOGO147_H) / 2;
  disp->drawRGBBitmap(lx, ly, logo147, LOGO147_W, LOGO147_H);

  // Texte principal a droite, centre dans l'espace restant
  disp->setTextSize(ACCUEIL_TEXT_SIZE);
  const uint16_t cMain = disp->color565(ACCUEIL_TEXT_COLOR);
  disp->setTextColor(cMain);
  const int regL  = lx + LOGO147_W + 6;
  const int regW  = VDISP_W - regL;
  const int wMain = frText(ACCUEIL_TEXT, false, ACCUEIL_TEXT_SIZE, cMain) * 6 * ACCUEIL_TEXT_SIZE;
  disp->setCursor(regL + (regW - wMain) / 2, (topH - 8 * ACCUEIL_TEXT_SIZE) / 2);
  frText(ACCUEIL_TEXT, true, ACCUEIL_TEXT_SIZE, cMain);

  // Baseline en bas, pleine largeur (TAGLINE vide -> rien)
  if (baseH) {
    disp->setTextSize(1);
    const uint16_t cTag = disp->color565(TAGLINE_COLOR);
    disp->setTextColor(cTag);
    const int wTag = frText(TAGLINE, false, 1, cTag) * 6;
    disp->setCursor((VDISP_W - wTag) / 2, VDISP_H - 8);
    frText(TAGLINE, true, 1, cTag);
  }
}

void render(unsigned long ms, bool colonOn) {
  if (state == READY) { drawAccueil(); return; }   // avant le depart : logo 147

  disp->fillScreen(0);
  uint16_t c = colFor(state);

  unsigned long totalSec = ms / 1000;
  int hh = (totalSec / 3600) % 10;   // heures 0-9
  int mm = (totalSec / 60)   % 60;   // minutes 00-59
  int ss =  totalSec         % 60;   // secondes 00-59

  const int totalW = 5 * DIGIT_W + 2 * DIGIT_GAP + 4 * COLON_GAP + 2 * COLON_W;
  const int Y = (VDISP_H - DIGIT_H) / 2;
  const int colDotX = (COLON_W - DIGIT_T) / 2;
  int x = (VDISP_W - totalW) / 2;

  drawDigit(x, Y, DIGIT_W, DIGIT_H, DIGIT_T, hh, c);          x += DIGIT_W + COLON_GAP;
  if (colonOn) drawColon(x + colDotX, Y, DIGIT_H, DIGIT_T, c);x += COLON_W + COLON_GAP;
  drawDigit(x, Y, DIGIT_W, DIGIT_H, DIGIT_T, mm / 10, c);     x += DIGIT_W + DIGIT_GAP;
  drawDigit(x, Y, DIGIT_W, DIGIT_H, DIGIT_T, mm % 10, c);     x += DIGIT_W + COLON_GAP;
  if (colonOn) drawColon(x + colDotX, Y, DIGIT_H, DIGIT_T, c);x += COLON_W + COLON_GAP;
  drawDigit(x, Y, DIGIT_W, DIGIT_H, DIGIT_T, ss / 10, c);     x += DIGIT_W + DIGIT_GAP;
  drawDigit(x, Y, DIGIT_W, DIGIT_H, DIGIT_T, ss % 10, c);
}

unsigned long elapsed() {
  if (state == RUNNING) return accumulatedMs + (millis() - startMs);
  return accumulatedMs;
}

// --- Actions du chrono ---
void doRun()   { startMs = millis(); state = RUNNING; }                       // demarre ou reprend
void doPause() { if (state == RUNNING) { accumulatedMs += millis() - startMs; state = PAUSED; } }
void doReset() { accumulatedMs = 0; state = READY; }

// ------------------------- Detecteur de gestes -----------------------------
// Un bouton actif-bas (INPUT_PULLUP) -> evenements court / double / long.
BtnEvent readGesture(int pin, BtnState &b) {
  if (pin < 0) return EVT_NONE;
  BtnEvent ev = EVT_NONE;
  unsigned long now = millis();
  bool raw = (digitalRead(pin) == LOW);          // LOW = appuye

  if (raw != b.lastRead) { b.lastRead = raw; b.tChange = now; }   // anti-rebond
  if (now - b.tChange >= DEBOUNCE_MS && raw != b.pressed) {
    b.pressed = raw;
    if (raw) {                                   // front descendant : appui
      b.tDown = now; b.longFired = false;
      ev = EVT_DOWN;
    } else if (!b.longFired) {                    // front montant : relacher
      b.clicks++; b.tLastUp = now;
    }
  }

  // maintien -> appui long (annule les clics en cours)
  if (b.pressed && !b.longFired && (now - b.tDown >= LONG_MS)) {
    b.longFired = true; b.clicks = 0;
    ev = EVT_LONG;
  }
  // fin de fenetre double-clic -> court ou double
  if (!b.pressed && b.clicks > 0 && (now - b.tLastUp >= DOUBLE_MS)) {
    ev = (b.clicks >= 2) ? EVT_DOUBLE : EVT_SHORT;
    b.clicks = 0;
  }
  return ev;
}

// Applique un evenement geste a l'etat du chrono.
void handleEvent(BtnEvent ev) {
  switch (ev) {
    case EVT_DOWN:   if (state == READY)  doRun();   break; // depart IMMEDIAT et precis
    case EVT_SHORT:  if (state == PAUSED) doRun();   break; // reprise (en RUNNING : ignore)
    case EVT_DOUBLE: if (state == RUNNING) doPause();break; // double appui = pause
    case EVT_LONG:   doReset();                      break; // appui long = reset
    default: break;
  }
}

BtnState btnExt, btnUp;                 // etats des 2 boutons a gestes
bool sReset = HIGH; unsigned long tReset = 0;   // bouton DOWN (reset direct etabli)

void setup() {
  pinMode(BTN_START, INPUT_PULLUP);
  pinMode(BTN_RESET, INPUT_PULLUP);
  if (EXT_BTN_PIN >= 0) pinMode(EXT_BTN_PIN, INPUT_PULLUP);

  HUB75_I2S_CFG::i2s_pins _pins = {
    R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
    A_PIN, B_PIN, C_PIN, D_PIN, E_PIN, LAT_PIN, OE_PIN, CLK_PIN
  };
  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN, _pins);
  mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_10M;
  mxconfig.clkphase = false;
  // mxconfig.driver = HUB75_I2S_CFG::FM6126A; // decommente si l'ecran reste NOIR

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(BRIGHTNESS);
  dma_display->clearScreen();

  disp = new VirtualMatrixPanel_T<PANEL_CHAIN_TYPE>(NUM_ROWS, NUM_COLS,
                                                    PANEL_RES_X, PANEL_RES_Y);
  disp->setDisplay(*dma_display);
  disp->cp437(true);                 // mapping correct des glyphes accentues (CP437)

  render(0, true);
}

unsigned long lastSec = 0xFFFFFFFF;
bool  lastColon = true;
State lastState = (State)255;

void loop() {
  // Gestes : bouton externe (guidon) + bouton UP embarque (meme comportement)
  handleEvent(readGesture(EXT_BTN_PIN, btnExt));
  handleEvent(readGesture(BTN_START,   btnUp));
  // Bouton DOWN : reset instantane (confort etabli, pas utilise en course)
  bool rDown = digitalRead(BTN_RESET);
  if (rDown != sReset && (millis() - tReset) > DEBOUNCE_MS) {
    sReset = rDown; tReset = millis();
    if (rDown == LOW) doReset();
  }

  unsigned long ms  = elapsed();
  unsigned long sec = ms / 1000;
  bool colonOn = (state == PAUSED) ? (((millis() / 500) % 2) == 0) : true;

  if (sec != lastSec || state != lastState || colonOn != lastColon) {
    render(ms, colonOn);
    lastSec = sec; lastState = state; lastColon = colonOn;
  }
}

/*
 * ============================================================================
 *  NOTES
 * ----------------------------------------------------------------------------
 *  - Ecran d'accueil (READY) : logo (logo147.h) a gauche, ACCUEIL_TEXT a droite,
 *    TAGLINE en bas. Textes editables avec accents (convertis par frText). Mets
 *    TAGLINE a "" pour n'afficher que le logo + le texte principal.
 *  - IMPORTANT : place logo147.h dans le MEME dossier que ce .ino.
 *  - Bouton a gestes (externe + UP) : court=start/reprise, double=pause, long=reset.
 *    Le start est pris des l'appui (front descendant) pour un top precis ; la pause
 *    ne se declenche qu'au double appui -> pas de pause/reset par erreur en course.
 *    Regle LONG_MS / DOUBLE_MS en haut du fichier si besoin.
 *  - Tout est en fillRect : rendu net, aucun pixel parasite. Verticales continues
 *    (pas de coupure haut/bas) et coins remplis. Bloc centre H + V.
 *  - Envie de barres plus fines/epaisses -> DIGIT_T. Plus etroit/large -> DIGIT_W.
 *  - 2 chiffres d'heures : hh = (totalSec/3600)%100, ajoute drawDigit hh/10 & hh%10,
 *    reduis DIGIT_W (~22) pour tenir dans 192 px.
 * ============================================================================
 */