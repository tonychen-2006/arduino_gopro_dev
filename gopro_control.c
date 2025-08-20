#include <WiFiS3.h>
#include <SPI.h>
#include <RTC.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ======= USER SETTINGS =======
const char* SSID = "GP26354747";
const char* PASS = "scuba0828";
const char* HOST = "10.5.5.9";
const int   PORT = 80;

// Poll cadence and thresholds
#define POLL_MS        1000   // check every 1s
#define FLAT_THRESH    3      // end after N flat size reads (no growth)
#define LOCK_THRESH    2      // start after N consecutive lock/busy reads (e.g., 503)

// Debug prints (0 = CSV only, 1 = CSV + debug)
#define VERBOSE 0

WiFiClient client;

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect(); delay(150);
  WiFi.begin(SSID, PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 12000) delay(50);
}

bool httpGETtoBuf(const char* path, char* body, size_t cap) {
  ensureWiFi();
  client.setTimeout(4000);
  if (!client.connect(HOST, PORT)) return false;

  client.print(String("GET ") + path + " HTTP/1.1\r\n"
               "Host: " + HOST + "\r\n"
               "Connection: close\r\n\r\n");

  String s = client.readStringUntil('\n'); s.trim();
  if (s.indexOf(" 200") < 0) { client.stop(); return false; }

  while (client.connected()) {
    String h = client.readStringUntil('\n');
    if (h == "\r" || h.length()==0) break;
  }

  size_t n = 0;
  while (client.connected() || client.available()) {
    while (client.available()) {
      char c = (char)client.read();
      if (n + 1 < cap) body[n++] = c;
    }
  }
  body[n] = '\0';
  client.stop();
  return true;
}

// Range GET 1 byte; returns size via Content-Range/Length and HTTP code.
// true => size OK (code 206/200). false => no size (e.g., 503/416/etc.)
static bool httpGETfilesize(const char* path, long* outTotal, int* httpCode) {
  *outTotal = -1; if (httpCode) *httpCode = -1;

  ensureWiFi();
  client.setTimeout(4000);
  if (!client.connect(HOST, PORT)) return false;

  client.print(String("GET ") + path + " HTTP/1.1\r\n"
               "Host: " + HOST + "\r\n"
               "Range: bytes=0-0\r\n"
               "Connection: close\r\n\r\n");

  String status = client.readStringUntil('\n'); status.trim();
  int code = 0;
  int sp1 = status.indexOf(' '), sp2 = status.indexOf(' ', sp1+1);
  if (sp1 > 0 && sp2 > sp1) code = status.substring(sp1+1, sp2).toInt();
  if (httpCode) *httpCode = code;

  long total = -1;
  bool sizeOK = false;

  if (code == 206 || code == 200) {
    while (client.connected()) {
      String h = client.readStringUntil('\n');
      if (h == "\r" || h.length()==0) break;
      h.trim();
      if (h.startsWith("Content-Range:")) {
        int slash = h.lastIndexOf('/');
        if (slash > 0) { total = atol(h.c_str() + slash + 1); sizeOK = true; }
      }
      if (!sizeOK && h.startsWith("Content-Length:")) {
        total = atol(h.c_str() + strlen("Content-Length:"));
        sizeOK = true;
      }
    }
  } else {
    while (client.connected()) {
      String h = client.readStringUntil('\n');
      if (h == "\r" || h.length()==0) break;
    }
  }

  while (client.connected() || client.available()) { while (client.available()) client.read(); }
  client.stop();

  if (sizeOK && total >= 0) { *outTotal = total; return true; }
  return false;
}

// ======= TIME / RTC =======
static Month monFrom3(const char *m) {
  if (!strncmp(m,"Jan",3)) return Month::JANUARY;  if (!strncmp(m,"Feb",3)) return Month::FEBRUARY;
  if (!strncmp(m,"Mar",3)) return Month::MARCH;    if (!strncmp(m,"Apr",3)) return Month::APRIL;
  if (!strncmp(m,"May",3)) return Month::MAY;      if (!strncmp(m,"Jun",3)) return Month::JUNE;
  if (!strncmp(m,"Jul",3)) return Month::JULY;     if (!strncmp(m,"Aug",3)) return Month::AUGUST;
  if (!strncmp(m,"Sep",3)) return Month::SEPTEMBER;if (!strncmp(m,"Oct",3)) return Month::OCTOBER;
  if (!strncmp(m,"Nov",3)) return Month::NOVEMBER; return Month::DECEMBER;
}
static uint8_t monToInt(Month m) {
  switch (m) {
    case Month::JANUARY: return 1; case Month::FEBRUARY: return 2; case Month::MARCH: return 3;
    case Month::APRIL: return 4;   case Month::MAY: return 5;      case Month::JUNE: return 6;
    case Month::JULY: return 7;    case Month::AUGUST: return 8;   case Month::SEPTEMBER: return 9;
    case Month::OCTOBER: return 10;case Month::NOVEMBER: return 11;default: return 12;
  }
}
uint8_t dow(uint8_t d, uint8_t m, uint16_t y){ if(m<3){m+=12;y-=1;}int K=y%100,J=y/100;int h=(d+(13*(m+1))/5+K+K/4+J/4+5*J)%7;return (h==0)?7:h; }

void seedRTC() {
  char mm[4] = {__DATE__[0],__DATE__[1],__DATE__[2],'\0'};
  Month M = monFrom3(mm);
  uint8_t d = (uint8_t)atoi(__DATE__+4);
  uint16_t y = (uint16_t)atoi(__DATE__+7);
  uint8_t hh = (uint8_t)atoi(__TIME__+0);
  uint8_t mi = (uint8_t)atoi(__TIME__+3);
  uint8_t ss = (uint8_t)atoi(__TIME__+6);
  RTCTime t(d, M, y, hh, mi, ss, (DayOfWeek)dow(d, monToInt(M), y), SaveLight::SAVING_TIME_INACTIVE);
  RTC.setTime(t);
}

void nowISO(char *out, size_t n) {
  RTCTime t; RTC.getTime(t);
  snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u",
           t.getYear(), monToInt(t.getMonth()), t.getDayOfMonth(),
           t.getHour(), t.getMinutes(), t.getSeconds());
}
static void durHMS(unsigned long ms, char* out, size_t n) {
  unsigned long s=ms/1000, h=s/3600; s%=3600; unsigned m=s/60; s%=60;
  snprintf(out, n, "%02lu:%02lu:%02lu", h, m, s);
}

// ======= DCIM LIST SCRAPERS (no JSON) =======
// Find highest ###GOPRO directory
static bool findLatestDir(char* out, size_t cap) {
  char page[2048]; if (!httpGETtoBuf("/videos/DCIM/", page, sizeof(page))) return false;
  int best = -1;
  for (size_t i=0; page[i]; ++i) {
    if (isdigit((unsigned char)page[i]) && isdigit((unsigned char)page[i+1]) && isdigit((unsigned char)page[i+2])
        && !strncmp(&page[i+3],"GOPRO",5)) {
      int num = (page[i]-'0')*100 + (page[i+1]-'0')*10 + (page[i+2]-'0');
      if (num > best) best = num;
    }
  }
  if (best < 0) return false;
  snprintf(out, cap, "/videos/DCIM/%03dGOPRO/", best);
  return true;
}

// Parse MP4 name into base/chapter: GOPR0123.MP4 => base=123 chap=0; GP01xxxx.MP4 => base=xxxx chap=1..
static void mp4Key(const char* name, int* base, int* chap) {
  int n = strlen(name);
  *base = -1; *chap = 0;
  int dot = n - 4; // ".MP4"
  int i = dot - 1, digits = 0, val = 0, pow10 = 1;
  while (i >= 0 && isdigit((unsigned char)name[i]) && digits < 4) {
    val = (name[i]-'0') * pow10 + val; pow10 *= 10; digits++; i--;
  }
  if (digits == 4) *base = val;
  if (i >= 2 && name[0]=='G' && name[1]=='P' && isdigit((unsigned char)name[2]) && isdigit((unsigned char)name[3])) {
    *chap = (name[2]-'0')*10 + (name[3]-'0');
  } else {
    *chap = 0;
  }
}

// Pick numerically newest .MP4 in dir (by base then chapter)
static bool findNewestMP4InDir(const char* dir, char* file, size_t cap) {
  char page[4096]; if (!httpGETtoBuf(dir, page, sizeof(page))) return false;

  int bestBase=-1, bestChap=-1;
  char bestName[64]="";

  for (int idx=0; page[idx]; ++idx) {
    if (page[idx]=='.' && toupper(page[idx+1])=='M' && toupper(page[idx+2])=='P' && page[idx+3]=='4') {
      int start = idx - 1;
      while (start >= 0 && !isspace((unsigned char)page[start]) &&
             page[start] != '/' && page[start] != '\"' && page[start] != '>' && page[start] != '=') --start;
      ++start;
      int end = idx + 4;
      int len = end - start; if (len <= 0 || len >= 63) continue;
      char name[64]; memcpy(name, &page[start], len); name[len]='\0';
      int base, chap; mp4Key(name, &base, &chap);
      if (base >= 0) {
        if (base > bestBase || (base == bestBase && chap > bestChap)) {
          bestBase = base; bestChap = chap; strncpy(bestName, name, sizeof(bestName)); bestName[sizeof(bestName)-1]='\0';
        }
      }
    }
  }

  if (bestBase < 0) return false;
  snprintf(file, cap, "%s%s", dir, bestName);
  return true;
}

// ======= CSV helpers =======
static void csvFileAnnounce(const char* path) {
  Serial.print("CSV,FILE,"); Serial.println(path);
}
static void csvStart(const char* ts) {
  Serial.print("CSV,START,"); Serial.println(ts);
}
static void csvEnd(const char* ts, unsigned long durMs) {
  char d[16]; durHMS(durMs, d, sizeof(d));
  Serial.print("CSV,END,"); Serial.print(ts); Serial.print(",DUR,"); Serial.println(d);
}

// ======= STATE =======
static char curDir[64]   = "";
static char curFile[128] = "";
static long lastSize     = -1;
static int  flatCount    = 0;
static int  lockCount    = 0;
static bool rec          = false;
static unsigned long startMs = 0;
static char startTs[32] = "";

// ======= SETUP / LOOP =======
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
  unsigned long t0=millis(); while(!Serial && millis()-t0<4000) {}
#if VERBOSE
  Serial.println("Booting…");
#endif
  WiFi.begin(SSID, PASS);
  t0=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-t0<12000) delay(50);
#if VERBOSE
  Serial.print("WiFi "); Serial.println(WiFi.status()==WL_CONNECTED ? "connected" : "NOT connected");
#endif
  RTC.begin(); seedRTC();
#if VERBOSE
  Serial.println("RTC ok. Watching /videos/DCIM…");
#endif
}

void loop() {
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll < POLL_MS) { delay(5); return; }
  lastPoll = millis();

  if (WiFi.status() != WL_CONNECTED) {
#if VERBOSE
    Serial.println("WiFi=DOWN");
#endif
    return;
  }

  // Ensure we have a directory and a file; announce the file once
  if (curDir[0] == '\0' || curFile[0] == '\0') {
    if (!findLatestDir(curDir, sizeof(curDir))) {
#if VERBOSE
      Serial.println("No DCIM dir yet.");
#endif
      return;
    }
    if (!findNewestMP4InDir(curDir, curFile, sizeof(curFile))) {
#if VERBOSE
      Serial.print("No MP4 in "); Serial.println(curDir);
#endif
      return;
    }
    csvFileAnnounce(curFile);      // <<< tell Mac helper what to download later
    lastSize = -1; flatCount = 0; lockCount = 0;
  }

  // Detect if a NEWER file appeared (new recording or file split)
  char latestFile[128];
  if (findNewestMP4InDir(curDir, latestFile, sizeof(latestFile))) {
    if (strcmp(latestFile, curFile) != 0) {
      strncpy(curFile, latestFile, sizeof(curFile)); curFile[sizeof(curFile)-1]='\0';
      csvFileAnnounce(curFile);    // announce new file path immediately
      lastSize = -1; flatCount = 0; lockCount = 0;
      if (!rec) {
        rec = true; startMs = millis(); nowISO(startTs, sizeof(startTs));
        csvStart(startTs);
#if VERBOSE
        Serial.print("Recording-Started (new file) @ "); Serial.println(startTs);
#endif
      }
    }
  }

  // Probe size or detect "locked"
  long sz = -1; int code = -1;
  bool ok = httpGETfilesize(curFile, &sz, &code);

#if VERBOSE
  char ts[32]; nowISO(ts, sizeof(ts));
  Serial.print(ts); Serial.print(" | file="); Serial.print(curFile);
  if (ok) { Serial.print(" | size="); Serial.print(sz); Serial.print(" | http=OK"); }
  else    { Serial.print(" | size=? | http="); Serial.print(code); }
#endif

  // Decide state
  if (ok) {
    if (lastSize >= 0 && sz > lastSize) {
      if (!rec) {
        rec = true; startMs = millis(); nowISO(startTs, sizeof(startTs));
        csvStart(startTs);
#if VERBOSE
        Serial.print("  >>> Recording-Started (growing) @ "); Serial.println(startTs);
#endif
      }
      flatCount = 0; lockCount = 0;
    } else if (lastSize >= 0 && sz == lastSize) {
      flatCount++;
      lockCount = 0;
      if (rec && flatCount >= FLAT_THRESH) {
        rec = false; char endTs[32]; nowISO(endTs, sizeof(endTs));
        unsigned long dur = millis() - startMs;
        csvEnd(endTs, dur);
#if VERBOSE
        Serial.print("  >>> Recording-Ended @ "); Serial.print(endTs);
        Serial.print(" | DUR="); char dd[16]; durHMS(dur, dd, sizeof(dd)); Serial.println(dd);
#endif
        flatCount = 0;
      }
    } else {
      // first size seen
      flatCount = 0; lockCount = 0;
    }
    lastSize = sz;
  } else {
    // Treat repeated "locked/busy" HTTP codes as recording in-progress
    // Your camera returns 503 while recording; keep others too.
    if (code == 503 || code == 416 || code == 423 || code == 403 || code == 404 || code == 500 || code == -1) {
      lockCount++;
      flatCount = 0;
      if (!rec && lockCount >= LOCK_THRESH) {
        rec = true; startMs = millis(); nowISO(startTs, sizeof(startTs));
        csvStart(startTs);
#if VERBOSE
        Serial.print("  >>> Recording-Started (locked) @ "); Serial.println(startTs);
#endif
      }
    } else {
      // unexpected error; reset counters but don't flip state
      flatCount = 0; lockCount = 0;
    }
  }

#if VERBOSE
  Serial.print(" | rec="); Serial.println(rec ? "1" : "0");
#endif
}
