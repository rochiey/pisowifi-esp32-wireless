/*
 ============================================================================
  PisoWifiBridge.ino
  ----------------------------------------------------------------------------
  Custom Piso Wi-Fi coin-acceptor bridge firmware for a NodeMCU 1.0
  (ESP-12E Module / ESP8266EX). Drop-in replacement for a problematic
  JuanFi setup. Talks directly to a MikroTik hAP Lite through the classic
  RouterOS API on port 8728.

  HARDWARE
    Board .................. NodeMCU 1.0 (ESP-12E Module) / ESP8266EX
    Coin acceptor .......... Allan Universal Multi-Coin Acceptor
    Acceptor DIP switches .. FAST pulse mode (20 ms pulse / 40 ms pause),
                             NO (Normally Open) output
    D6 (GPIO12) ............ Coin pulse signal input (idle HIGH, pulses LOW)
    D7 (GPIO13) ............ Set pin / solenoid power gate control output
                             (HIGH = gate open, LOW = gate closed)
    Power .................. 12 V DC base shield (HW-389)

  FLASHING (Arduino IDE -> Sketch -> Export compiled Binary, or arduino-cli)
    Board .............. "NodeMCU 1.0 (ESP-12E Module)"
    Flash Size ......... "4MB (FS:2MB OTA:~1019KB)" (any 4MB layout works)
    Flash Mode ......... "DOUT"          <-- REQUIRED for generic CH340 boards;
                                            prevents dim-blue-light boot loops
    CPU Frequency ...... "80 MHz"        <-- most stable on weak power rails
    Upload Speed ....... "115200"
    Port ............... your CH340 COM port

    arduino-cli equivalent:
      arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
        --build-property build.flash_mode=dout PisoWifiBridge.ino

    Flash the exported .bin with NodeMCU PyFlasher (Flash mode: DOUT)
    or esptool.py:
      esptool.py --port COMx write_flash --flash_mode dout \
        0x0000 PisoWifiBridge.ino.nodemcuv2.bin

  MIKROTIK ROUTER PRE-REQUISITES (hAP Lite)
    - The "api" service must be enabled on port 8728 (IP -> Services -> api).
    - The API account must have write access to /ip/hotspot/user.
    - Create one hotspot user profile named "coin-hotspot" with the rate
      limit / session settings you want customers to receive. If you need a
      different profile name, change COIN_HOTSPOT_PROFILE below.
    - Crediting is VOUCHER-BASED (JuanFi-compatible): the bridge creates or
      extends a hotspot user named after the voucher code the hotspot page
      sends in /topUp (falling back to the colon-stripped MAC address when no
      voucher is sent). Users are created WITHOUT a password, which matches
      the JuanFi portal's default loginOption=0 (username-only CHAP login).
      Vouchers longer than 31 characters are truncated by the EEPROM recovery
      record (live credits are not affected).

  POWER NOTES (HW-389 12 V base shield)
    The solenoid is the biggest source of rail sag. Sessions follow the
    original JuanFi model: the gate stays OPEN for the whole session window
    and coins are credited in batches during short silence gaps (the brief
    API burst runs while the solenoid is energized - exactly like JuanFi's
    own firmware). The gate closes only when the session ends. The EEPROM
    pending-credit record is written BEFORE any failed credit, so even a
    brownout mid-burst recovers the customer's coins after reboot.
    Add a 1000 uF / 25 V electrolytic capacitor across the 12 V solenoid
    supply and a 470 uF capacitor across the NodeMCU 5 V rail for extra
    stability on weak 12 V adapters.

  DEBUGGING
    Serial 115200 baud, plain ASCII messages only - no raw pointers, no hex
    dumps, no exception register codes. Works with any Android USB serial
    monitor app.

  ----------------------------------------------------------------------------
  REVISION 2026-09-10  (boot-order self-heal + done-press closeout)

  R6 - DONE-PRESS CLOSEOUT (2026-09-10):
    * endSession() now DELIVERS the closeout value synchronously instead of
      only queueing it: pressing "Done" a moment after a coin (before the
      1.5 s silence batch credited it) used to end the session with the value
      routed to the pending-recovery queue and /useVoucher answering
      "coins.wait.expired" - the popup showed an error and the time arrived
      later. The reply now carries the full awarded time; only a genuine
      API failure falls back to the recovery queue.
    * When Done is pressed before any pulse has arrived, the closeout grace
      now runs its full 1.2 s (it used to bail out early on an idle line),
      so an accepted, still-travelling coin can never be cut off uncounted.

  R5 - BOOT-ORDER SELF-HEAL (2026-09-10):
    * watchPortalJoin(): while the setup portal is active, retry the
      configured SSID every 30 s (AP+STA mode - the 192.168.4.1 setup
      portal stays reachable) and leave the portal automatically as soon
      as the router is back. Previously, an ESP that booted before the
      hAP Lite was ready (the 3x15 s boot join window expired) stayed in
      setup mode FOREVER - 10.0.0.5 unreachable, "coinslot busy" - until
      a manual power cycle. This removes the old "router first, ESP
      second" boot-order requirement.
    * startStation(): failure log updated to say the unit keeps retrying.

  REVISION 2026-10-01  (R7: pause-safe accounting + direct sales/event reporting)

  R7.3 - AUTO-LOGIN FILE RELIABILITY (2026-10-02):
    * The voucher-per-MAC auto-login file is now written with /file/print +
      /file/set contents over the SAME RouterOS API connection as the credit.
      The old /tool fetch made the router fetch HTTP from the ESP32 - which can
      time out while the ESP32 is busy inside the credit, leaving the customer
      without auto-login.
    * If that write still fails it is queued in NVS (4 slots) and retried from
      loop() while idle; a 'voucher_file_recovered' event closes the audit loop
      so Errors & Audit shows the problem resolved.
    * New admin endpoint GET /publishTest?key=..&mac=..&voucher=.. writes one
      file on demand (field verification without taking a coin).

  R7 - FIELD RELIABILITY (2026-10-01):
    * Multi-slot pending credit recovery: up to 4 uncredited purchases are kept
      (each with its voucher AND MAC) instead of a single slot that a second
      failed credit could overwrite. A recovered credit also republishes the
      voucher-per-MAC data file so returning-customer auto-login keeps working.
    * Direct sales + event reporting to the dashboard
      (https://pisowifi.rochiey.dev/api/pisowifi): every finished session posts
      its own sale (source=esp32), and credit failures, pending recovery,
      boots, Wi-Fi loss/recovery and heartbeats are posted as audit events.
      A small NVS outbox retries until the server acknowledges, so a sale can
      never be lost with a customer's phone.
    * The voucher data-file write is retried (a transient router fetch failure
      right after a credit used to break returning-customer auto-login).
    * Wi-Fi recovery never hard-resets the radio or reboots the bridge while a
      coin session is open or a credit is still pending - it waits for idle.
    * Firmware revision is reported on / and /stats.

  REVISION 2026-09-08  (deterministic crediting + flash-wear + OTA)

  R3 - LIVE-SESSION KICK-BEFORE-CREDIT (2026-09-08):
    * RouterOS (tested 6.49.17) does NOT extend an ALREADY-RUNNING hotspot
      session when the user record's limit-uptime changes - the running
      session keeps its own old budget and syncs the user record back down
      to it within seconds, silently eating record extensions. The bridge
      therefore ends any live session of the target voucher FIRST (same API
      connection), then writes the credit; the captive portal re-logs the
      phone in (voucher + empty password) and the fresh session reads the
      full credited total.
    * READ-FIRST ordering: the user record is looked up BEFORE the session
      is ended, because /ip/hotspot/user/print can transiently return "no
      rows" right after /ip/hotspot/active/remove - which used to make the
      bridge re-create an existing voucher as a brand-new user (extensions
      vanished into a ghost row). A one-shot re-lookup with a 400 ms settle
      covers genuinely new vouchers.
    * Voucher strings are whitespace-trimmed in /topUp, /checkCoin and
      /useVoucher so phone-keyboard spaces can never create parallel users.

  R4 - NVS PERSISTENCE + HARDENING (2026-09-08):
    * ESP32 persistence migrated to Preferences/NVS (transactional and
      wear-leveled; a power cut mid-write can no longer corrupt neighboring
      records). The legacy EEPROM image is read once and migrated to NVS
      automatically on the first boot of this firmware. ESP8266 keeps
      classic EEPROM so the sketch still compiles for NodeMCU.
    * Nightly-restart marker is now persisted AND loaded - previously it was
      written but never read back, so a reboot inside the restart hour could
      restart a second time.
    * salesRecordFailure() is deferred (coalesced) - no per-failure flash
      writes during a Wi-Fi outage.
    * NTP retries every 5 minutes when the clock is unsynced (zero cost once
      synced) so nightly restart and daily sales buckets keep working after
      a bad first sync.
    * Removed the unused minutesForPulses() session-total pricer; per-coin
      coinBurstValue() is the only pricing function.

  R2 - PER-COIN PRICING (final pricing model, replaces the R1 session-total
  greedy that mispriced mixed small coins):
    * One physical coin = one PULSE BURST (Allan FAST: pulses of a single
      coin are ~60 ms apart; COIN_BURST_GAP_US = 500 ms separates coins).
      Every burst is priced individually via coinBurstValue(), so:
          1x P5 coin (burst of 5)      -> 60 min      (P5 tier)
          5x P1 coins (5 bursts of 1)  -> 5 x 10 = 50 min
          1x P10 coin                  -> 130 min
          P5+P10+5xP1                  -> 60+130+50 = 240 min (4 h)
          2x P10                       -> 130+130 = 260 min (4 h 20 m)
      Bigger coins carry a bonus (per-peso rate rises with the tier), which
      deliberately nudges customers towards P5/P10 coins - five P1 coins
      never quietly upgrade to the P5 bundle.
    * ISR only segments bursts (never touches the rate table); the loop
      banks minutes per finished burst. The 1.5 s silence just paces the
      router API calls - it never prices coins.
    * Trailing-coin protection: when the session closes, the gate closes
      first and a ~1.2 s closeout grace lets an in-flight coin finish its
      train before the counter detaches, so a coin is never cut mid-train.

  R1 - deterministic ledger + flash-wear + OTA:
    * Every credit = full session value so far minus what was already
      delivered; failed credits persist the FULL cumulative deficit
      (monotone, so a single-slot EEPROM recovery record can never
      overwrite an earlier failed batch out of existence).
    * Flash-wear protection: sales counters are coalesced (one commit per
      session instead of one per batch credit); boot counter commits once
      per boot; pending/config/nightly writes always flush dirty sales in
      the same sector write.
    * Session hard cap (3 minutes) and the 60-pulse cap now close the
      session with everything credited or pending - coins are never silently
      dropped at a window edge.
    * New admin endpoints: GET /log?key=<api pass> (in-RAM log ring for
      no-serial field diagnosis) and GET /setRates?key=<api pass>...
      (live promo-table update without a reboot).
    * ESP32: password-protected ArduinoOTA, serviced only while the coinslot
      is idle - started AFTER WiFi.mode() (starting it earlier asserts on a
      NULL queue and boot-loops), so the unit can be patched over the LAN
      after one USB flash.
 ============================================================================
*/

#include <ctype.h>
#include <string.h>
#include <time.h>
#ifdef ESP32
  #include <WiFi.h>
  #include <WebServer.h>
  #include "esp_wifi.h"
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
#endif
#include <WiFiClientSecure.h>
#include <DNSServer.h>
#include <EEPROM.h>
#ifdef ESP32
  #include <Preferences.h>
  #include <ArduinoOTA.h>
#endif

// ---------------------------------------------------------------------------
// Pin map
// ---------------------------------------------------------------------------
#ifdef ESP32
  // ESP32 DevKit 30-pin: GPIO27 = coin pulse input (not a strapping pin,
  // has an internal pull-up). GPIO26 = solenoid gate control output.
  #define PIN_COIN            27
  #define PIN_GATE            26
#else
  #define PIN_COIN            D6    // GPIO12 - coin pulse input (NO, idle HIGH)
  #define PIN_GATE            D7    // GPIO13 - solenoid gate control output
#endif

// ---------------------------------------------------------------------------
// Serial / setup AP
// ---------------------------------------------------------------------------
#define SERIAL_BAUD         115200
#define AP_SSID             "PisoWiFi-Setup"
#define DEFAULT_AP_PASS     "piso-setup"

// ---------------------------------------------------------------------------
// EEPROM configuration
// ---------------------------------------------------------------------------
#define EEPROM_SIZE         1024
#define CONFIG_MAGIC        0x5049534FUL   // "PISO"
#define CONFIG_VERSION      2
#define SALES_EEPROM_OFFSET 512
#define SALES_MAGIC         0x53414C45UL   // "SALE"
#define SALES_DAYS          7
#define PROMO_EEPROM_OFFSET 640
#define PROMO_MAGIC         0x50524F4DUL   // "PROM"
#define PROMO_SLOTS         4

// ---------------------------------------------------------------------------
// Crash-safe pending credit recovery (coins counted, credit not delivered)
// ---------------------------------------------------------------------------
#define PENDING_EEPROM_OFFSET 768
#define PENDING_MAGIC         0x50435233UL   // "PCR3" per-slot marker
#define PENDING_STORE_MAGIC   0x50435333UL   // "PCS3" store marker
#define PENDING_RETRY_MS      15000UL
#define PENDING_NAME_MAX      24   // voucher / username length limit
#define PENDING_MAC_MAX       13   // colon-free uppercase MAC + NUL
#ifdef ESP32
  #define PENDING_SLOTS       8    // R7.1: survive multi-customer outages
#else
  #define PENDING_SLOTS       1    // ESP8266 EEPROM is only 1 KB
#endif

struct PendingSlot {
  uint16_t magic;
  uint16_t minutes;
  char     name[PENDING_NAME_MAX];
  char     mac[PENDING_MAC_MAX];
};

struct PendingStore {
  uint32_t magic;
  uint8_t  count;
  uint8_t  pad[3];
  PendingSlot slot[PENDING_SLOTS];
};

// Legacy single-slot record (firmware <= R6), migrated on first boot.
struct PendingLegacy {
  uint32_t magic;
  uint16_t minutes;
  char     name[32];
};

PendingStore pendingStore;
PendingLegacy pendingLegacy;
uint32_t pendingNextAttemptMs = 0;

// ---------------------------------------------------------------------------
// Deferred auto-login file publication (R7.3)
// ---------------------------------------------------------------------------
// If the inline file write ever fails, the job waits here (NVS on ESP32) and
// tickVoucherPublish() retries it from loop() while the bridge is idle.
#define PUBQ_MAGIC       0x50514233UL   // "PQB3"
#define PUBQ_JOB_MAGIC   0x504A4F42UL   // "PJOB"
#define PUBQ_SLOTS       4
#define PUBQ_VOUCHER_MAX 24
#define PUBQ_MAC_MAX     13
#define PUBQ_RETRY_MS    30000UL

struct PubJob {
  uint32_t magic;
  char voucher[PUBQ_VOUCHER_MAX];
  char mac[PUBQ_MAC_MAX];
};
struct PubQueue {
  uint32_t magic;
  uint8_t count;
  uint8_t pad[3];
  PubJob job[PUBQ_SLOTS];
};
PubQueue pubQueue;
uint32_t pubqNextAttemptMs = 0;

// ---------------------------------------------------------------------------
// Nightly idle-aware maintenance restart (24/7 units)
// ---------------------------------------------------------------------------
// Restarts the bridge once per day at a low-traffic hour, but ONLY when no
// coin session is active - if a customer is mid-insertion, the restart is
// deferred to the next idle minute. Hotspot sessions are unaffected either
// way (they live on the hAP Lite, not here).
#define NIGHTLY_RESTART_OFFSET   800
#define NIGHTLY_RESTART_MAGIC    0x4E525331UL   // "NRS1"
#define NIGHTLY_RESTART_UTC_HOUR 20             // 20:00 UTC = 04:00 Philippine time (UTC+8)
#define NIGHTLY_RESTART_CHECK_MS 60000UL

struct NightlyRestart {
  uint32_t magic;
  uint16_t lastDay;   // epoch day of the last nightly restart
};

NightlyRestart nightly;
uint32_t nightlyLastCheckMs = 0;

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------
#define DEFAULT_API_PORT            8728
#define DEFAULT_PULSES_PER_COIN     1
#define DEFAULT_MINUTES_PER_COIN    5
#define FW_REVISION                 "R7.3-2026-10-01"

// ---------------------------------------------------------------------------
// MikroTik RouterOS API
// ---------------------------------------------------------------------------
#define COIN_HOTSPOT_PROFILE   "coin-hotspot"   // must exist on the router
#define API_CONNECT_TIMEOUT_MS 3000             // bounds each blocking TCP connect
#define API_LOGIN_TIMEOUT_MS   4000
#define API_REPLY_TIMEOUT_MS   4000

// ---------------------------------------------------------------------------
// Wi-Fi join / recovery
// ---------------------------------------------------------------------------
#define WIFI_JOIN_TIMEOUT_MS        15000
#define WIFI_JOIN_ATTEMPTS          3
#define WIFI_RECONNECT_INTERVAL_MS  10000
#define GATEWAY_CHECK_INTERVAL_MS   30000
#define WIFI_HARD_RESET_AFTER_MS    60000
#define WIFI_RESTART_AFTER_MS       600000   // R7: 10 min, and only while idle
#define PORTAL_JOIN_RETRY_MS        30000   // setup portal: retry the router SSID every 30 s
#define PORTAL_JOIN_WAIT_MS         8000    // ...waiting up to 8 s for each join attempt
#define NTP_RETRY_INTERVAL_MS       300000  // re-attempt NTP every 5 min when unsynced

// ---------------------------------------------------------------------------
// Coin cycle timing (Allan FAST 20 ms pulse / 40 ms pause, NO switch)
// ---------------------------------------------------------------------------
#define GATE_OPEN_SETTLE_MS     300   // let the solenoid/12 V rail settle
#define GATE_CLOSE_SETTLE_MS    350   // let the magnetic field collapse
#define COIN_ARM_IDLE_MS        150   // line must stay HIGH this long pre-arm
#define COIN_WINDOW_MS          30000 // max wait per batch; re-armed after each credit
#define COIN_END_IDLE_MS        1500  // credit a batch 1.5 s after the last pulse
#define MIN_PULSE_GAP_US        35000 // min spacing between accepted pulses (> 25 ms phantom noise; < FAST period
#define MIN_HIGH_HOLD_US        30000 // coin line must be HIGH this long before a falling edge counts (rejects 40 Hz oscillation)
// Per-coin burst segmentation: one coin's FAST train has pulses ~60 ms apart;
// a gap LARGER than this means the previous coin finished and a new one
// started. Each burst is priced individually (see coinBurstValue), which is
// what makes 5x P1 = 5 bursts x 10 min = 50 min while one P5 coin = one burst
// of 5 pulses = 60 min. Set high so a coin can never be split (a split would
// shortchange the customer); a too-small inter-coin gap merely merges bursts,
// and merged pricing never pays less than the sum of its parts.
#define COIN_BURST_GAP_US       500000L // 500 ms: >> 60 ms intra-coin, << human pacing
#define COIN_BURST_GAP_MS       500     // same value in ms (line-idle checks)
#define MAX_PULSES              60    // hard safety cap per session
#define MAX_CREDIT_MINUTES      600   // clamp to 10 hours
#define SESSION_HARD_CAP_MS     180000 // longest the gate may stay open in one
                                       // session even if coins keep trickling
                                       // in (bounded, then closed + credited)

// ---------------------------------------------------------------------------
// Flash-wear protection (coalesced commits)
// ---------------------------------------------------------------------------
// ESP32: persistence lives in NVS (Preferences) - transactional and
// wear-leveled, so a power cut mid-write cannot corrupt other records (the
// old EEPROM emulation erased a whole sector per commit). ESP8266: classic
// EEPROM (still supported by this sketch, but the ESP32 is the production
// target). Routine sales-counter updates are batched in RAM and flushed at
// most every EEPROM_COALESCE_MS, or immediately when something critical
// happens (pending credit saved/cleared, config saved, reboot). Critical
// records always flush pending sales first so one write carries everything.
#define EEPROM_COALESCE_MS      10000

// ---------------------------------------------------------------------------
// In-RAM log ring (served by /log?key=... so failures can be diagnosed over
// HTTP without opening the serial port on a deployed unit)
// ---------------------------------------------------------------------------
#ifdef ESP32
  #define LOG_RING_MAX          80
#else
  #define LOG_RING_MAX          24   // NodeMCU has much less heap
#endif
#define LOG_RING_LEN            116

// ---------------------------------------------------------------------------
// Health monitoring
// ---------------------------------------------------------------------------
#define HEAP_LOG_INTERVAL_MS    30000
#define HEAP_WARN_BYTES         9000
#define HEAP_CRITICAL_BYTES     6000

#define REMOTE_QUEUE_MAX            8
#define REMOTE_LINE_MAX             96
#define REMOTE_FLUSH_INTERVAL_MS    30000

// ---------------------------------------------------------------------------
// Direct sales + event reporting (dashboard)
// ---------------------------------------------------------------------------
// The vendo reports its OWN sales instead of trusting the customer's phone.
// Items wait in a small NVS outbox and are retried until the server answers
// 2xx, so a coin sale can never be lost with a phone's browser cache.
#define REPORT_BASE_DEFAULT  "https://pisowifi.rochiey.dev/api/pisowifi"
#define REPORT_LOG_URL       "https://pisowifi.rochiey.dev/api/pisowifi/log"
#define PISO_VENDO_NAME      "Pineda WIFI VENDO"
#define REPORT_MAGIC         0x52505437UL      // "RPT7"
#define REPORT_ITEM_MAGIC    0x49544D37UL      // "ITM7"
#define REPORT_RETRY_MS      20000UL
#define REPORT_HEARTBEAT_MS  1800000UL         // 30 min
#define REPORT_TYPE_MAX      24
#define REPORT_MSG_MAX       112
#define REPORT_VOUCHER_MAX   16
#define REPORT_EEPROM_OFFSET 896
#ifdef ESP32
  #define REPORT_SLOTS       32   // R7.2: ~2 h of busy sales can queue while the internet is down
  #define REPORT_PERSIST     1
  #define REPORT_STORE_KEY   "rpt7"
#else
  #define REPORT_SLOTS       2
  #define REPORT_PERSIST     0
  #define REPORT_STORE_KEY   "rpt7"
#endif

struct ReportItem {
  uint32_t magic;
  uint8_t  kind;         // 0 = sale, 1 = event
  uint8_t  severity;     // 0 = info, 1 = warning, 2 = error
  uint16_t coins;
  uint16_t minutes;
  char     voucher[REPORT_VOUCHER_MAX];
  char     mac[PENDING_MAC_MAX];
  char     type[REPORT_TYPE_MAX];
  char     message[REPORT_MSG_MAX];
};

struct ReportStore {
  uint32_t magic;
  uint16_t head;
  uint16_t count;
  ReportItem item[REPORT_SLOTS];
};

// ---------------------------------------------------------------------------
// Persistent configuration (EEPROM, CRC protected)
// ---------------------------------------------------------------------------
struct Config {
  uint32_t magic;
  uint16_t version;
  uint16_t crc;
  char     ssid[33];
  char     pass[65];
  char     gatewayIp[16];
  char     apiUser[33];
  char     apiPass[65];
  uint16_t apiPort;
  uint16_t pulsesPerCoin;
  uint16_t minutesPerCoin;
  char     apPass[33];
  uint8_t  configured;
  char     logUrl[97];
  char     deviceId[33];
};

// Sales counters kept in EEPROM separately from Config so config re-saves
// never disturb the tally. Daily buckets are keyed by UTC epoch day (NTP).
struct SalesStats {
  uint32_t magic;
  uint32_t crc;
  uint32_t bootCount;
  uint32_t totalCycles;
  uint32_t totalCoins;
  uint32_t totalMinutes;
  uint32_t failedCycles;
  uint32_t lastDay;                      // epoch day of the newest bucket
  uint32_t dayCoins[SALES_DAYS];
  uint32_t dayMinutes[SALES_DAYS];
  uint32_t dayCycles[SALES_DAYS];
};

// Promo rate table kept in its own EEPROM block so Config stays backward
// compatible. In promo mode 1 pulse = P1 and the best matching rate is
// applied greedily (same idea as JuanFi's rates.data).
struct PromoConfig {
  uint32_t magic;
  uint32_t crc;
  uint8_t  enabled;                 // 1 = use promo table, 0 = linear rate
  uint8_t  gateInvert;              // 1 = solenoid opens on LOW, 0 = opens on HIGH
  uint8_t  gatePin;                 // GPIO pin that drives the solenoid gate
  uint8_t  txPowerIdx;              // 0 = 10 dBm, 1 = 15 dBm, 2 = 20.5 dBm
  uint16_t price[PROMO_SLOTS];      // pesos (or pulses)
  uint16_t minutes[PROMO_SLOTS];    // minutes granted
};

// ---------------------------------------------------------------------------
// Coin cycle state machine
//   IDLE -> ARMING -> COUNTING -> CREDITING -> DONE/ERROR -> IDLE
//   (the transition back to IDLE happens at the end of runCoinCycle();
//    the last result is kept in lastCycleText for the status page)
// ---------------------------------------------------------------------------
enum CycleState {
  CYCLE_IDLE,
  CYCLE_ARMING,
  CYCLE_COUNTING,
  CYCLE_CREDITING,
  CYCLE_DONE,
  CYCLE_ERROR
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
#ifdef ESP32
WebServer server(80);   // ESP32 built-in web server
#else
ESP8266WebServer server(80);
#endif
DNSServer dnsServer;

Config config;
bool portalMode = false;

CycleState cycleState = CYCLE_IDLE;
String lastCycleText = "No credit cycle yet";

// Coin pulse tally (updated by ISR, read by the counting loop)
volatile uint16_t volatilePulses = 0;
volatile uint32_t lastPulseUs = 0;
volatile uint32_t lastHighUs = 0;

// Per-coin burst state. The ISR only segments pulses into bursts (a burst =
// one physical coin) and hands finished bursts to the main loop through a
// small ring; the loop assigns minutes (rate-table lookups stay out of the
// ISR). burstBankMinutes is therefore loop-only and never touched by the ISR.
#define BURST_QUEUE_MAX         32
volatile uint16_t openBurstPulses = 0;  // pulses of the coin dropping right now
volatile uint16_t burstQueue[BURST_QUEUE_MAX];  // sizes of finished bursts
volatile uint8_t  burstQHead = 0;
volatile uint8_t  burstQCount = 0;
volatile uint16_t burstOverflowPulses = 0;  // safety merge when the ring is full
volatile uint16_t lastBurstGapMs = 0;       // inter-coin gap of the last close
uint32_t burstBankMinutes = 0;   // minutes banked from finished coins (loop only)
uint16_t burstCoinCount = 0;     // coins banked (loop only; diagnostics/sales)

uint32_t lastHeapLogMs = 0;
uint32_t lastWifiCheckMs = 0;
bool wifiLostLogged = false;
uint32_t lastGatewayCheckMs = 0;
uint32_t wifiDownSince = 0;
uint32_t lastWifiHardResetMs = 0;
uint32_t lastPortalJoinMs = 0;   // R5: setup-portal router-retry pacing
uint8_t gatewayFailCount = 0;

// JuanFi-compatible top-up session (used by the router-hosted hotspot page)
String activeMac = "";
String activeVoucher = "";
uint16_t finalPulses = 0;      // pulses the customer owns (delivered or safely pending)
uint16_t finalCoins = 0;       // pulses counted this session (sales/display)
uint16_t finalMinutes = 0;     // minutes actually DELIVERED to the router
bool sessionDone = false;
bool successDelivered = false;   // /checkCoin success reported exactly once
uint32_t gateSettleDeadline = 0;
uint32_t countingStartMs = 0;
uint32_t lastPulseMs = 0;
uint32_t creditDeadline = 0;
uint32_t sessionStartMs = 0;   // absolute start of the coin session (hard cap)
uint32_t sessionEndMs = 0;     // when the session closed (for EEPROM flush pacing)
// Session value ledger (per-coin pricing): the slot stays OPEN for the whole
// session and every COIN_END_IDLE_MS of silence the pending batch is settled.
// Minutes are the SUM of the per-coin (per-burst) values banked so far:
//     target = burstBankMinutes            (banked when each coin's burst ends)
//     batchMinutes = target - finalMinutes (delta to deliver to the router)
// The 1.5 s cadence only paces the router API calls - it never prices coins.
// A delivery failure persists the FULL cumulative deficit, so no coin is ever
// lost, and pulses that arrive while a credit runs simply join the next
// batch. (Per-coin value of a burst = coinBurstValue() below.)
uint16_t batchBasePulses = 0;    // pulses settled so far (credited or pending)
uint16_t batchPulses = 0;        // pulses in the batch being settled now
uint16_t batchCoins = 0;         // pulses in the batch (sales bookkeeping)
uint16_t batchMinutes = 0;       // DELTA minutes to deliver for this batch
uint16_t lastBatchPulses = 0;    // last seen batch-relative count
bool     finalizeByWindow = false;

// /topUp per-client throttle - file scope so handleCancelTopUp can release
// it when a customer legitimately cancels and restarts immediately.
IPAddress lastTopUpStarterIp;
uint32_t  lastTopUpStartMs = 0;

SalesStats sales;
PromoConfig promo;

// Persistent sales/event report outbox (see REPORT_* above)
ReportStore reportStore;
uint32_t lastReportAttemptMs = 0;
uint32_t lastHeartbeatMs = 0;
bool lastReportOk = false;
String lastReportDetail = "no attempt yet";

// Session sales accumulation: committed ONCE at session end instead of once
// per batch credit (this is where the old firmware wore the flash out).
uint32_t sessionSalesCoins = 0;
uint32_t sessionSalesMinutes = 0;

// Optional online log forwarding ring buffer
char remoteQueue[REMOTE_QUEUE_MAX][REMOTE_LINE_MAX];
uint8_t remoteHead = 0;
uint8_t remoteCount = 0;
uint32_t lastRemoteFlushMs = 0;

// ---------------------------------------------------------------------------
// Human-readable logging (ASCII only - no pointers, no hex dumps)
// ---------------------------------------------------------------------------
// In-RAM log ring for /log?key=... diagnostics. Declared here (above
// logLine) so every log line is mirrored into it.
char logRing[LOG_RING_MAX][LOG_RING_LEN];
uint8_t logRingHead = 0;
uint8_t logRingCount = 0;

void logLine(const String& level, const String& msg) {
  Serial.print(millis());
  Serial.print(' ');
  Serial.print(level);
  Serial.print(' ');
  Serial.println(msg);

  // Mirror every line into the in-RAM ring (used by the /log endpoint).
  char buf[LOG_RING_LEN];
  int n = snprintf(buf, sizeof(buf), "%lu %s %s",
                   (unsigned long)millis(), level.c_str(), msg.c_str());
  if (n < 0) n = 0;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  buf[n] = 0;
  uint8_t idx = (logRingHead + logRingCount) % LOG_RING_MAX;
  memcpy(logRing[idx], buf, sizeof(buf));
  if (logRingCount < LOG_RING_MAX) logRingCount++;
  else logRingHead = (logRingHead + 1) % LOG_RING_MAX;
}

void logInfo(const String& msg) { logLine(F("[INFO]"), msg); }
void logWarn(const String& msg) { logLine(F("[WARN]"), msg); remoteEnqueue(String(F("[WARN] ")) + msg); }
void logErr(const String& msg)  { logLine(F("[ERROR]"), msg); remoteEnqueue(String(F("[ERROR] ")) + msg); }

// Ring buffer for optional online log forwarding. Only WARN/ERROR lines are
// queued automatically; sales/credit events are queued explicitly.
void remoteEnqueue(const String& entry) {
  if (strlen(config.logUrl) == 0) return;
  uint8_t idx = (remoteHead + remoteCount) % REMOTE_QUEUE_MAX;
  strncpy(remoteQueue[idx], entry.c_str(), REMOTE_LINE_MAX - 1);
  remoteQueue[idx][REMOTE_LINE_MAX - 1] = 0;
  if (remoteCount < REMOTE_QUEUE_MAX) remoteCount++;
  else remoteHead = (remoteHead + 1) % REMOTE_QUEUE_MAX;
}

// ---------------------------------------------------------------------------
// Human-readable translations of low-level states
// ---------------------------------------------------------------------------
String resetReasonText() {
#ifdef ESP32
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power-on reset";
    case ESP_RST_SW:        return "Software restart";
    case ESP_RST_PANIC:     return "Firmware panic / crash";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog timeout";
    case ESP_RST_TASK_WDT:  return "Task watchdog timeout";
    case ESP_RST_WDT:       return "Other watchdog timeout";
    case ESP_RST_BROWNOUT:  return "Brownout reset (power sag)";
    case ESP_RST_DEEPSLEEP: return "Deep-sleep wake";
    default:                return "Reset button / external reset";
  }
#else
  String r = ESP.getResetInfo();
  r.trim();
  if (r.startsWith("External"))   return "Reset button / external reset";
  if (r.startsWith("Power"))      return "Power-on reset";
  if (r.startsWith("Hardware"))   return "Hardware watchdog timeout";
  if (r.startsWith("Software"))   return "Software watchdog / system restart";
  if (r.startsWith("Exception"))  return "Firmware crash";
  if (r.startsWith("Deep"))       return "Deep-sleep wake";
  if (r.length() == 0)            return "Unknown reset cause";
  return r; // already human-readable
#endif
}

String wifiStatusText(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:      return "Wi-Fi idle";
    case WL_NO_SSID_AVAIL:    return "SSID not found";
    case WL_SCAN_COMPLETED:   return "Scan completed";
    case WL_CONNECTED:        return "Connected";
    case WL_CONNECT_FAILED:   return "Connection failed (check SSID/password)";
    case WL_CONNECTION_LOST:  return "Connection lost";
    case WL_DISCONNECTED:     return "Disconnected";
    default:                  return "Unknown Wi-Fi state";
  }
}

String cycleStateText() {
  switch (cycleState) {
    case CYCLE_IDLE:      return "Idle";
    case CYCLE_ARMING:    return "Arming coin slot";
    case CYCLE_COUNTING:  return "Counting coins";
    case CYCLE_CREDITING: return "Crediting via MikroTik API";
    case CYCLE_DONE:      return "Credit completed";
    case CYCLE_ERROR:     return "Credit failed";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------
// CRC16 (CCITT) for the EEPROM config block
// ---------------------------------------------------------------------------
uint16_t crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else         crc >>= 1;
    }
  }
  return crc;
}

// ---------------------------------------------------------------------------
// Persistent storage backend
// ---------------------------------------------------------------------------
// ESP32: Preferences / NVS - transactional + wear-leveled, so a brownout can
// never leave a half-erased sector (the old EEPROM emulation rewrote a whole
// sector per commit). The classic EEPROM image is still READ once as a
// one-time migration source for units that ran the older firmware.
// ESP8266: classic EEPROM (kept so the sketch still compiles for NodeMCU).
#ifdef ESP32
  #define PERS_NS   "piso"
#endif

template<typename T>
bool persPut(uint32_t eepromOffset, const char* nvsKey, const T& value) {
#ifdef ESP32
  Preferences p;
  if (!p.begin(PERS_NS, false)) return false;
  size_t n = p.putBytes(nvsKey, &value, sizeof(T));
  p.end();
  return n == sizeof(T);
#else
  (void)nvsKey;
  EEPROM.put(eepromOffset, value);
  EEPROM.commit();
  return true;
#endif
}

// Returns true when the record was read from the primary backend.
template<typename T>
bool persGet(uint32_t eepromOffset, const char* nvsKey, T& value) {
#ifdef ESP32
  Preferences p;
  if (!p.begin(PERS_NS, true)) return false;
  size_t n = p.getBytes(nvsKey, &value, sizeof(T));
  p.end();
  return n == sizeof(T);
#else
  (void)nvsKey;
  EEPROM.get(eepromOffset, value);
  return true;
#endif
}

// One-time migration from the legacy EEPROM image into NVS. Called by the
// loaders when NVS has nothing (fresh flash or first boot of this firmware).
#ifdef ESP32
template<typename T>
bool persGetLegacy(uint32_t eepromOffset, T& value) {
  EEPROM.get(eepromOffset, value);
  return true;
}
#else
template<typename T>
bool persGetLegacy(uint32_t eepromOffset, T& value) {
  (void)eepromOffset;
  return false;
}
#endif

// ---------------------------------------------------------------------------
// Coalesced persistence (flash-wear protection)
// ---------------------------------------------------------------------------
// Routine sales-counter updates only mark the block dirty; the actual write
// happens in eepromCommitNow() (called on critical events and from the
// periodic tick). Critical records always flush dirty sales first, so a
// single write carries as much as possible.
bool eepromDirty = false;    // a persistence flush is owed
bool salesDirty = false;     // the sales block changed and is not on flash yet
uint32_t eepromLastFlushMs = 0;

void eepromCommitNow() {
  // Any dirty sales block is put first so one write carries all data.
  if (salesDirty) {
    sales.crc = 0;
    sales.crc = crc16((const uint8_t*)&sales, sizeof(sales));
    persPut(SALES_EEPROM_OFFSET, "sales", sales);
    salesDirty = false;
  }
#ifndef ESP32
  EEPROM.commit();
#endif
  eepromDirty = false;
  eepromLastFlushMs = millis();
}

// Called from loop(): flushes pending sales at most every EEPROM_COALESCE_MS
// so a long coin session still lands its counters even without a session-end.
void tickEepromFlush() {
  if (!eepromDirty && !salesDirty) return;
  uint32_t now = millis();
  if ((uint32_t)(now - eepromLastFlushMs) < EEPROM_COALESCE_MS) return;
  eepromCommitNow();
}

// ---------------------------------------------------------------------------
// Config load/save
// ---------------------------------------------------------------------------
void configDefaults() {
  memset(&config, 0, sizeof(config));
  config.magic           = CONFIG_MAGIC;
  config.version         = CONFIG_VERSION;
  config.apiPort         = DEFAULT_API_PORT;
  config.pulsesPerCoin   = DEFAULT_PULSES_PER_COIN;
  config.minutesPerCoin  = DEFAULT_MINUTES_PER_COIN;
  strncpy(config.apPass, DEFAULT_AP_PASS, sizeof(config.apPass) - 1);
  config.configured      = 0;
}

void saveConfig() {
  config.crc = 0;
  config.crc = crc16((const uint8_t*)&config, sizeof(config));
  persPut(0, "cfg", config);
  eepromCommitNow();
}

void loadConfig() {
  if (!persGet(0, "cfg", config)) {
    // No NVS record yet (fresh flash OR first boot after migrating from the
    // old EEPROM firmware) - try the legacy EEPROM image once.
    persGetLegacy(0, config);
  }
  bool valid = (config.magic == CONFIG_MAGIC);
  if (valid) {
    uint16_t stored = config.crc;
    config.crc = 0;
    uint16_t calc = crc16((const uint8_t*)&config, sizeof(config));
    config.crc = stored;
    valid = (calc == stored);
  }
  if (!valid) {
    logWarn("[CONFIG] No valid stored configuration found - starting unconfigured");
    configDefaults();
    saveConfig();
    return;
  }
  logInfo("[CONFIG] Loaded configuration (SSID: " + String(config.ssid) +
          ", gateway: " + String(config.gatewayIp) +
          ", API port: " + String(config.apiPort) + ")");
#ifdef ESP32
  // Make sure the loaded image lives in NVS from now on (one-time migration).
  Preferences p;
  if (p.begin(PERS_NS, true)) {
    if (p.getBytesLength("cfg") != sizeof(config)) {
      p.end();
      saveConfig();
      logInfo("[PERS] Migrated configuration to NVS");
    } else {
      p.end();
    }
  }
#endif
}

// ---------------------------------------------------------------------------
// Sales monitoring (persisted in EEPROM at SALES_EEPROM_OFFSET)
// ---------------------------------------------------------------------------
uint32_t salesCurrentDay() {
  time_t now = time(nullptr);
  if (now > 1000000000L) return (uint32_t)(now / 86400UL);
  return 0; // NTP not synced yet
}

void salesShift(uint32_t newDay) {
  if (sales.lastDay == newDay) return;
  uint32_t days;
  if (newDay > sales.lastDay) days = newDay - sales.lastDay;
  else days = 1; // clock went backwards - treat as one day boundary

  if (days >= SALES_DAYS) {
    memset(sales.dayCoins, 0, sizeof(sales.dayCoins));
    memset(sales.dayMinutes, 0, sizeof(sales.dayMinutes));
    memset(sales.dayCycles, 0, sizeof(sales.dayCycles));
  } else {
    for (uint32_t d = 0; d < days; d++) {
      for (uint8_t i = 0; i < SALES_DAYS - 1; i++) {
        sales.dayCoins[i]   = sales.dayCoins[i + 1];
        sales.dayMinutes[i] = sales.dayMinutes[i + 1];
        sales.dayCycles[i]  = sales.dayCycles[i + 1];
      }
      sales.dayCoins[SALES_DAYS - 1]   = 0;
      sales.dayMinutes[SALES_DAYS - 1] = 0;
      sales.dayCycles[SALES_DAYS - 1]  = 0;
    }
  }
  sales.lastDay = newDay;
}

// Deferred (coalesced) sales persistence: marks the block dirty; the actual
// sector write happens in eepromCommitNow()/tickEepromFlush() so per-batch
// credits never hammer the flash. salesSaveNow() forces an immediate write.
void salesSave() {
  salesDirty = true;
  eepromDirty = true;
}

void salesSaveNow() {
  salesDirty = true;
  eepromCommitNow();
}

void salesLoad() {
  if (!persGet(SALES_EEPROM_OFFSET, "sales", sales)) {
    persGetLegacy(SALES_EEPROM_OFFSET, sales);
  }
  bool valid = (sales.magic == SALES_MAGIC);
  if (valid) {
    uint32_t stored = sales.crc;
    sales.crc = 0;
    uint32_t calc = crc16((const uint8_t*)&sales, sizeof(sales));
    sales.crc = stored;
    valid = (calc == stored);
  }
  if (!valid) {
    memset(&sales, 0, sizeof(sales));
    sales.magic = SALES_MAGIC;
    salesSaveNow();
  } else {
    salesSaveNow();   // ensure the loaded image is mirrored to NVS (migration)
  }
}

void salesRecordSale(uint16_t coins, uint16_t minutes) {
  uint32_t day = salesCurrentDay();
  if (day == 0) day = sales.lastDay;      // keep accumulating until NTP syncs
  if (sales.lastDay == 0) sales.lastDay = day;
  salesShift(day);

  sales.totalCycles++;
  sales.totalCoins   += coins;
  sales.totalMinutes += minutes;
  sales.dayCoins[0]   += coins;
  sales.dayMinutes[0] += minutes;
  sales.dayCycles[0]++;
  salesSaveNow();   // rare events (sync cycle / pending recovery) - immediate
}

void salesRecordFailure() {
  sales.failedCycles++;
  salesSave();   // deferred: flushed with the next critical commit or tick
}

// ---------------------------------------------------------------------------
// Promo rate table (persisted in EEPROM at PROMO_EEPROM_OFFSET)
// ---------------------------------------------------------------------------
void promoDefaults() {
  memset(&promo, 0, sizeof(promo));
  promo.magic    = PROMO_MAGIC;
  promo.enabled  = 0;
  promo.gateInvert = 0;
  promo.gatePin = PIN_GATE;
  promo.txPowerIdx = 1;
  promo.price[0] = 1;   promo.minutes[0] = 60;     // P1  = 1 hour
  promo.price[1] = 5;   promo.minutes[1] = 480;    // P5  = 8 hours
  promo.price[2] = 10;  promo.minutes[2] = 1440;   // P10 = 24 hours
  promo.price[3] = 20;  promo.minutes[3] = 2880;   // P20 = 48 hours
}

void promoSave() {
  promo.crc = 0;
  promo.crc = crc16((const uint8_t*)&promo, sizeof(promo));
  persPut(PROMO_EEPROM_OFFSET, "promo", promo);
  eepromCommitNow();
}

void promoLoad() {
  if (!persGet(PROMO_EEPROM_OFFSET, "promo", promo)) {
    persGetLegacy(PROMO_EEPROM_OFFSET, promo);
  }
  bool valid = (promo.magic == PROMO_MAGIC);
  if (valid) {
    uint32_t stored = promo.crc;
    promo.crc = 0;
    uint32_t calc = crc16((const uint8_t*)&promo, sizeof(promo));
    promo.crc = stored;
    valid = (calc == stored);
  }
  if (!valid) {
    promoDefaults();
    promoSave();
  } else {
    promoSave();   // mirror to NVS (migration) - keeps parity with sales
  }
}

// ---------------------------------------------------------------------------
// Optional online log forwarding (plain HTTP or Cloudflare-terminated HTTPS)
// ---------------------------------------------------------------------------
bool parseLogUrl(const String& url, String& host, uint16_t& port, String& path, bool& useTls) {
  String u = url;
  u.trim();
  if (u.startsWith("https://")) { useTls = true;  u = u.substring(8); port = 443; }
  else if (u.startsWith("http://")) { useTls = false; u = u.substring(7); port = 80; }
  else return false;

  int slash = u.indexOf('/');
  String hostPort = (slash < 0) ? u : u.substring(0, slash);
  path = (slash < 0) ? "/" : u.substring(slash);
  if (hostPort.length() == 0) return false;

  int colon = hostPort.lastIndexOf(':');
  if (colon > 0) {
    host = hostPort.substring(0, colon);
    uint16_t p = (uint16_t)hostPort.substring(colon + 1).toInt();
    if (p > 0) port = p;
  } else {
    host = hostPort;
  }
  return host.length() > 0;
}

void sendRemotePost(WiFiClient& client, const String& host, const String& path, const String& json) {
  client.print(F("POST "));
  client.print(path);
  client.println(F(" HTTP/1.1"));
  client.print(F("Host: "));
  client.println(host);
  client.println(F("Content-Type: application/json"));
  client.print(F("Content-Length: "));
  client.println(json.length());
  client.println(F("Connection: close"));
  client.println();
  client.print(json);

  uint32_t start = millis();
  while (!client.available() && (millis() - start < 3000)) {
    delay(1);
    yield();
  }
  String status = client.readStringUntil('\n');
  status.trim();
  if (status.indexOf("200") >= 0) {
    logInfo("[REMOTE] Sent " + String(remoteCount) + " log line(s) to log server");
    remoteCount = 0;
    remoteHead = 0;
  } else {
    logWarn("[REMOTE] Log server replied: " + status);
  }
  client.stop();
}

void flushRemoteLogs() {
  if (remoteCount == 0) return;
  if (cycleState != CYCLE_IDLE) return;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now = millis();
  if (now - lastRemoteFlushMs < REMOTE_FLUSH_INTERVAL_MS) return;
  lastRemoteFlushMs = now;

  String host, path;
  uint16_t port = 80;
  bool useTls = false;
  String logUrl = String(config.logUrl);
  if (logUrl.length() == 0) logUrl = String(REPORT_LOG_URL);   // default: dashboard
  if (!parseLogUrl(logUrl, host, port, path, useTls)) {
    logWarn("[REMOTE] Invalid log URL in configuration - remote logging disabled");
    remoteCount = 0;
    return;
  }

  String json = "{\"device\":\"" + jsonEscape(String(config.deviceId)) + "\",\"logs\":[";
  for (uint8_t i = 0; i < remoteCount; i++) {
    uint8_t idx = (remoteHead + i) % REMOTE_QUEUE_MAX;
    if (i) json += ',';
    json += "{\"l\":\"" + jsonEscape(String(remoteQueue[idx])) + "\"}";
  }
  json += "]}";

  if (useTls) {
    WiFiClientSecure client;
    client.setInsecure(); // Cloudflare terminates TLS; skip cert validation on device
    if (!client.connect(host.c_str(), port)) {
      logWarn("[REMOTE] Could not connect to log server " + host + ":" + String(port));
      return;
    }
    sendRemotePost(client, host, path, json);
  } else {
    WiFiClient client;
    if (!client.connect(host.c_str(), port)) {
      logWarn("[REMOTE] Could not connect to log server " + host + ":" + String(port));
      return;
    }
    sendRemotePost(client, host, path, json);
  }
}

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------
void setField(char* dst, size_t dstSize, const String& src) {
  memset(dst, 0, dstSize);
  if (src.length() > 0) strncpy(dst, src.c_str(), dstSize - 1);
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

String urlDecode(const String& in) {
  String out;
  out.reserve(in.length());
  for (uint16_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.length()) {
      int hi = hexVal(in[i + 1]);
      int lo = hexVal(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += (char)((hi << 4) | lo);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }
  return out;
}

bool parseIPv4(const String& s, IPAddress& out) {
  uint8_t octets[4];
  int idx = 0;
  String cur = "";
  for (uint16_t i = 0; i <= s.length(); i++) {
    char c = (i < s.length()) ? s[i] : '.';
    if (c == '.') {
      if (cur.length() == 0 || cur.length() > 3 || idx >= 4) return false;
      for (uint16_t k = 0; k < cur.length(); k++) {
        if (!isdigit((unsigned char)cur[k])) return false;
      }
      int v = cur.toInt();
      if (v < 0 || v > 255) return false;
      octets[idx++] = (uint8_t)v;
      cur = "";
    } else if (isdigit((unsigned char)c)) {
      cur += c;
    } else {
      return false;
    }
  }
  if (idx != 4 || cur.length() != 0) return false;
  out = IPAddress(octets[0], octets[1], octets[2], octets[3]);
  return true;
}

bool isValidIPv4(const String& s) {
  IPAddress tmp;
  return parseIPv4(s, tmp);
}

// Accepts "AA:BB:CC:DD:EE:FF", "aabbccddeeff", "aa-bb-cc-dd-ee-ff".
bool normalizeMac(const String& in, String& out) {
  String hex;
  hex.reserve(12);
  for (uint16_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == ':' || c == '-' || c == ' ') continue;
    if (!isxdigit((unsigned char)c)) return false;
    hex += (char)toupper((unsigned char)c);
  }
  if (hex.length() != 12) return false;
  out = "";
  out.reserve(17);
  for (uint8_t i = 0; i < 6; i++) {
    if (i) out += ':';
    out += hex.substring(i * 2, i * 2 + 2);
  }
  return true;
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (uint16_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// ===========================================================================
//  Setup portal (WPA2 captive portal at 192.168.4.1) and station join
// ===========================================================================


// ---------------------------------------------------------------------------
// Direct reporting to the sales dashboard (sales + audit events)
// ---------------------------------------------------------------------------
bool reportStoreValid() { return reportStore.magic == REPORT_MAGIC; }

void reportPersist() {
#if REPORT_PERSIST
  persPut(REPORT_EEPROM_OFFSET, REPORT_STORE_KEY, reportStore);
#endif
}

uint16_t pendingReportCount() {
  if (!reportStoreValid()) return 0;
  if (reportStore.count > REPORT_SLOTS) return 0;
  return reportStore.count;
}

void reportEnqueueItem(uint8_t kind, uint8_t severity, uint16_t coins, uint16_t minutes,
                       const String& voucher, const String& mac, const char* type,
                       const String& message) {
  if (!reportStoreValid()) {
    memset(&reportStore, 0, sizeof(reportStore));
    reportStore.magic = REPORT_MAGIC;
  }
  if (reportStore.count >= REPORT_SLOTS) {   // drop the OLDEST, never the newest
    reportStore.head = (uint16_t)((reportStore.head + 1) % REPORT_SLOTS);
    reportStore.count = REPORT_SLOTS - 1;
  }
  uint16_t idx = (uint16_t)((reportStore.head + reportStore.count) % REPORT_SLOTS);
  ReportItem& it = reportStore.item[idx];
  memset(&it, 0, sizeof(it));
  it.magic = REPORT_ITEM_MAGIC;
  it.kind = kind;
  it.severity = severity;
  it.coins = coins;
  it.minutes = minutes;
  setField(it.voucher, sizeof(it.voucher), voucher);
  String mc = mac;
  mc.toUpperCase();
  mc.replace(":", "");
  setField(it.mac, sizeof(it.mac), mc);
  setField(it.type, sizeof(it.type), String(type));
  setField(it.message, sizeof(it.message), message);
  reportStore.count++;
  reportPersist();
}

void reportEvent(const char* type, uint8_t severity, const String& message,
                 const String& voucher, const String& mac,
                 uint16_t coins, uint16_t minutes) {
  reportEnqueueItem(1, severity, coins, minutes, voucher, mac, type, message);
  logInfo(String("[REPORT] queued event ") + type + ": " + message);
}

void reportSale(uint16_t coins, uint16_t minutes, const String& voucher, const String& mac) {
  if (coins == 0 && minutes == 0) return;
  reportEnqueueItem(0, 0, coins, minutes, voucher, mac, "sale", "session sale");
  logInfo("[REPORT] queued sale " + String(coins) + " coin(s) / " + String(minutes) +
          " min for " + voucher);
}

String isoNowUtc() {
  time_t now = time(nullptr);
  if (now < 1000000000L) return String("");
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return String(buf);
}

int httpPostRaw(Client& c, const String& host, const String& path, const String& json) {
  c.print(F("POST "));
  c.print(path);
  c.println(F(" HTTP/1.1"));
  c.print(F("Host: "));
  c.println(host);
  c.println(F("Content-Type: application/json"));
  c.print(F("Content-Length: "));
  c.println(json.length());
  c.println(F("Connection: close"));
  c.println();
  c.print(json);
  int code = 0;
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < 8000UL) {
    if (c.available()) {
      String status = c.readStringUntil('\n');
      int sp = status.indexOf(' ');
      if (sp > 0) code = status.substring(sp + 1).toInt();
      break;
    }
    if (!c.connected()) break;
    delay(1);
    yield();
  }
  return code;
}

bool postJsonUrl(const String& url, const String& json, String& detail) {
  String host, path;
  uint16_t port = 80;
  bool useTls = false;
  if (!parseLogUrl(url, host, port, path, useTls)) { detail = "bad url"; return false; }
  int code = 0;
  if (useTls) {
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(6);
    if (!c.connect(host.c_str(), port)) { detail = "tls connect " + host + " failed"; return false; }
    code = httpPostRaw(c, host, path, json);
    c.stop();
  } else {
    WiFiClient c;
    c.setTimeout(6);
    if (!c.connect(host.c_str(), port)) { detail = "connect " + host + " failed"; return false; }
    code = httpPostRaw(c, host, path, json);
    c.stop();
  }
  if (code >= 200 && code <= 299) { detail = String("HTTP ") + String(code); return true; }
  detail = String("server replied HTTP ") + String(code);
  return false;
}

String reportItemJson(const ReportItem& it) {
  String ts = isoNowUtc();
  String j;
  if (it.kind == 0) {
    j = "{\"event\":\"pisowifi_sale\",\"type\":\"INTERNET\",\"total_amount\":" + String(it.coins) +
        ",\"voucher\":\"" + jsonEscape(String(it.voucher)) + "\"" +
        ",\"mac\":\"" + jsonEscape(String(it.mac)) + "\"" +
        ",\"client_ip\":\"\",\"vendo_name\":\"" PISO_VENDO_NAME "\"" +
        ",\"vendo_ip\":\"" + WiFi.localIP().toString() + "\"" +
        ",\"time_added_seconds\":" + String((uint32_t)it.minutes * 60UL) +
        ",\"validity\":\"\",\"data_mb\":\"\",\"source\":\"esp32\"";
  } else {
    const char* sev = (it.severity == 2) ? "error" : ((it.severity == 1) ? "warning" : "info");
    j = "{\"event\":\"pisowifi_event\",\"type\":\"" + jsonEscape(String(it.type)) + "\"" +
        ",\"severity\":\"" + sev + "\"" +
        ",\"message\":\"" + jsonEscape(String(it.message)) + "\"" +
        ",\"voucher\":\"" + jsonEscape(String(it.voucher)) + "\"" +
        ",\"mac\":\"" + jsonEscape(String(it.mac)) + "\"" +
        ",\"coins\":" + String(it.coins) +
        ",\"minutes\":" + String(it.minutes) +
        ",\"device_id\":\"" + jsonEscape(String(config.deviceId)) + "\"" +
        ",\"vendo_name\":\"" PISO_VENDO_NAME "\"" +
        ",\"vendo_ip\":\"" + WiFi.localIP().toString() + "\"" +
        ",\"source\":\"esp32\"";
  }
  if (ts.length() > 0) j += ",\"timestamp\":\"" + ts + "\"";
  j += ",\"firmware\":\"" FW_REVISION "\"";
  j += "}";
  return j;
}

// Sends the oldest queued item. Returns true when the server acknowledged.
bool reportFlushOne(String& detail) {
  if (!reportStoreValid() || reportStore.count == 0) { detail = "outbox empty"; return true; }
  ReportItem& it = reportStore.item[reportStore.head % REPORT_SLOTS];
  if (it.magic != REPORT_ITEM_MAGIC) {   // corrupt slot - drop it
    reportStore.head = (uint16_t)((reportStore.head + 1) % REPORT_SLOTS);
    reportStore.count--;
    reportPersist();
    detail = "dropped corrupt slot";
    return false;
  }
  String url = String(REPORT_BASE_DEFAULT) + ((it.kind == 0) ? "/sale" : "/event");
  String json = reportItemJson(it);
  bool ok = postJsonUrl(url, json, detail);
  lastReportOk = ok;
  lastReportDetail = detail;
  if (ok) {
    reportStore.head = (uint16_t)((reportStore.head + 1) % REPORT_SLOTS);
    reportStore.count--;
    reportPersist();
  }
  return ok;
}

void tickReportOutbox() {
  if (portalMode) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (cycleState != CYCLE_IDLE) return;   // never block a coin session
  uint32_t now = millis();

  if (reportStoreValid() && reportStore.count > 0) {
    if ((int32_t)(now - lastReportAttemptMs) < 0) return;
    lastReportAttemptMs = now + REPORT_RETRY_MS;
    String detail;
    if (reportFlushOne(detail)) {
      logInfo("[REPORT] Sent queued item (" + String(reportStore.count) + " left)");
    } else {
      logWarn("[REPORT] Send failed: " + detail);
    }
    return;
  }

  // Quiet heartbeat so the dashboard can tell "vendo offline" from "no sales".
  if ((uint32_t)(now - lastHeartbeatMs) >= REPORT_HEARTBEAT_MS) {
    lastHeartbeatMs = now;
    String hb = "heap=" + String(ESP.getFreeHeap());
    hb += " rssi=" + String(WiFi.RSSI());
    hb += " boots=" + String(sales.bootCount);
    hb += " cycles=" + String(sales.totalCycles) + " coins=" + String(sales.totalCoins) +
          " minutes=" + String(sales.totalMinutes) + " failed=" + String(sales.failedCycles);
    reportEvent("heartbeat", 0, hb, "", "", 0, 0);
    lastReportAttemptMs = 0;   // send it on the next tick
  }
}

void handleReportTest() {
  String key = urlDecode(server.arg("key"));
  if (key != String(config.apiPass)) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  reportEvent("report_test", 0, "Manual report test from /reportTest", "", "", 0, 0);
  String detail;
  bool ok = reportFlushOne(detail);
  String j = "{\"ok\":" + String(ok ? "true" : "false") +
             ",\"outbox\":" + String(pendingReportCount()) +
             ",\"detail\":\"" + jsonEscape(lastReportDetail) + "\"}";
  server.send(200, "application/json", j);
}

// ---------------------------------------------------------------------------
// Auto-login file retry queue helpers (R7.3)
// ---------------------------------------------------------------------------
bool pubqValid() { return pubQueue.magic == PUBQ_MAGIC; }

void pubqPersist() {
#ifdef ESP32
  persPut(0, "pubq", pubQueue);
#endif
}

uint8_t pubqCount() {
  if (!pubqValid()) return 0;
  if (pubQueue.count > PUBQ_SLOTS) return 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < PUBQ_SLOTS; i++) {
    if (pubQueue.job[i].magic == PUBQ_JOB_MAGIC && pubQueue.job[i].voucher[0]) n++;
  }
  return n;
}

void pubqEnqueue(const String& voucher, const String& mac) {
  if (voucher.length() == 0) return;
  if (!pubqValid()) { memset(&pubQueue, 0, sizeof(pubQueue)); pubQueue.magic = PUBQ_MAGIC; }
  for (uint8_t i = 0; i < PUBQ_SLOTS; i++) {
    if (pubQueue.job[i].magic == PUBQ_JOB_MAGIC &&
        voucher.equalsIgnoreCase(String(pubQueue.job[i].voucher))) return;
  }
  int8_t idx = -1;
  for (uint8_t i = 0; i < PUBQ_SLOTS; i++) {
    if (pubQueue.job[i].magic != PUBQ_JOB_MAGIC) { idx = (int8_t)i; break; }
  }
  if (idx < 0) {   // full: keep the newest, drop the oldest
    for (uint8_t i = 1; i < PUBQ_SLOTS; i++) pubQueue.job[i - 1] = pubQueue.job[i];
    idx = PUBQ_SLOTS - 1;
  }
  PubJob& j = pubQueue.job[(uint8_t)idx];
  memset(&j, 0, sizeof(j));
  j.magic = PUBQ_JOB_MAGIC;
  setField(j.voucher, sizeof(j.voucher), voucher);
  String mc = mac;
  mc.toUpperCase();
  mc.replace(":", "");
  setField(j.mac, sizeof(j.mac), mc);
  pubQueue.count = pubqCount();
  pubqPersist();
  logWarn("[PUBQ] Queued auto-login file for " + voucher);
}

void pubqRemoveAt(uint8_t idx) {
  if (idx >= PUBQ_SLOTS) return;
  memset(&pubQueue.job[idx], 0, sizeof(pubQueue.job[idx]));
  pubQueue.count = pubqCount();
  pubqPersist();
}

void pubqLoad() {
  memset(&pubQueue, 0, sizeof(pubQueue));
#ifdef ESP32
  if (!persGet(0, "pubq", pubQueue) || pubQueue.magic != PUBQ_MAGIC) {
    pubQueue.magic = PUBQ_MAGIC;
    pubQueue.count = 0;
    return;
  }
#else
  pubQueue.magic = PUBQ_MAGIC;
  pubQueue.count = 0;
  return;
#endif
  uint8_t n = pubqCount();
  if (n > 0) {
    logWarn("[PUBQ] " + String(n) + " auto-login file job(s) survived a reboot - will retry");
    pubqNextAttemptMs = millis() + 10000;
  }
}

String portalPage(const String& errorMsg, const String& adminKey) {
  String h;
  h.reserve(2600);
  h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Piso WiFi Setup</title>"
      "<style>body{font-family:sans-serif;max-width:440px;margin:auto;padding:12px;color:#222}"
      "label{display:block;margin-top:10px;font-weight:bold}"
      "input{width:100%;padding:8px;margin-top:4px;box-sizing:border-box}"
      "button{margin-top:16px;padding:10px;width:100%;background:#0066cc;color:#fff;border:0}"
      ".err{color:#b00000;font-weight:bold}</style></head><body>"
      "<h1>Piso WiFi Setup</h1>";
  if (errorMsg.length() > 0) {
    h += "<p class='err'>";
    h += errorMsg;
    h += "</p>";
  }
  h += "<p>Connect this bridge to your MikroTik hAP Lite.</p>";
  h += "<form method='post' action='/save'>";
  if (adminKey.length() > 0) {
    h += "<input type='hidden' name='key' value='";
    h += adminKey;
    h += "'>";
  }
  h += "<label>Wi-Fi SSID</label><input name='ssid' value='";
  h += String(config.ssid);
  h += "' required>";
  h += "<label>Wi-Fi Password</label><input name='wifipass' type='password' value='";
  h += String(config.pass);
  h += "'>";
  h += "<label>Router Gateway IP</label><input name='gateway' value='";
  h += String(config.gatewayIp);
  h += "' required>";
  h += "<label>MikroTik API Username</label><input name='apiuser' value='";
  h += String(config.apiUser);
  h += "' required>";
  h += "<label>MikroTik API Password</label><input name='apipass' type='password' value='";
  h += String(config.apiPass);
  h += "' required>";
  h += "<label>MikroTik API Port</label><input name='apiport' type='number' value='";
  h += String(config.apiPort);
  h += "' required>";
  h += "<label>Pulses Per Coin</label><input name='pulses' type='number' min='1' value='";
  h += String(config.pulsesPerCoin);
  h += "' required>";
  h += "<label>Minutes Per Coin</label><input name='minutes' type='number' min='1' value='";
  h += String(config.minutesPerCoin);
  h += "' required>";
  h += "<label>Setup AP Password (8-63 chars, WPA2)</label><input name='appass' type='password' value='";
  h += String(config.apPass);
  h += "' required>";
  h += "<label>Remote Log URL (optional, http:// or https://)</label><input name='logurl' value='";
  h += String(config.logUrl);
  h += "' placeholder='https://yourdomain.com/api/piso-log'>";
  h += "<label>Device ID</label><input name='deviceid' value='";
  h += String(config.deviceId);
  h += "' placeholder='branch-1'>";
  h += "<h3>Promo Rates (optional)</h3>";
  h += "<label><input type='checkbox' name='usepromo' value='1'";
  if (promo.enabled == 1) h += " checked";
  h += "> Use promo rate table instead of Pulses/Minutes Per Coin</label>";
  h += "<label><input type='checkbox' name='invertgate' value='1'";
  if (promo.gateInvert == 1) h += " checked";
  h += "> Invert gate output (solenoid opens on LOW)</label>";
#ifdef ESP32
  h += "<label>Gate GPIO pin (default 26 - where your solenoid control is wired)</label><input name='gatepin' type='number' min='0' max='39' value='";
#else
  h += "<label>Gate GPIO pin (default 13 = D7)</label><input name='gatepin' type='number' min='0' max='16' value='";
#endif
  h += String(promo.gatePin);
  h += "'>";
  h += "<label>Wi-Fi TX power</label><select name='txpower'>";
  h += "<option value='0'";
  if (promo.txPowerIdx == 0) h += " selected";
  h += ">Low (10 dBm - most stable, shorter range)</option>";
  h += "<option value='1'";
  if (promo.txPowerIdx == 1) h += " selected";
  h += ">Medium (15 dBm - recommended)</option>";
  h += "<option value='2'";
  if (promo.txPowerIdx == 2) h += " selected";
  h += ">High (20.5 dBm - maximum range)</option>";
  h += "</select>";
  for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
    h += "<label>Promo " + String(i + 1) + " - Price (pesos/pulses)</label><input name='price" + String(i) + "' type='number' min='0' value='";
    h += String(promo.price[i]);
    h += "'>";
    h += "<label>Promo " + String(i + 1) + " - Minutes</label><input name='promomin" + String(i) + "' type='number' min='0' value='";
    h += String(promo.minutes[i]);
    h += "'>";
  }
  h += "<button type='submit'>Save and Reboot</button>";
  h += "</form></body></html>";
  return h;
}

String savedPage() {
  return "<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Saved</title></head>"
         "<body style='font-family:sans-serif;padding:16px'>"
         "<h1>Configuration Saved</h1>"
         "<p>Rebooting. The bridge will join your hAP Lite Wi-Fi shortly.</p>"
         "</body></html>";
}

String statusPage() {
  String h;
  h.reserve(1400);
  h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Piso WiFi Bridge</title></head>"
      "<body style='font-family:sans-serif;padding:16px'>"
      "<h1>Piso WiFi Bridge</h1>";
  h += "<p>Coin slot: " + cycleStateText() + "</p>";
  h += "<p>Last cycle: " + lastCycleText + "</p>";
  h += "<p>Wi-Fi: " + wifiStatusText(WiFi.status());
  if (WiFi.status() == WL_CONNECTED) {
    h += " (RSSI " + String(WiFi.RSSI()) + " dBm, IP " + WiFi.localIP().toString() + ")";
  }
  h += "</p>";
  h += "<p>Free heap: " + String(ESP.getFreeHeap()) + " bytes</p>";
  h += "<p>Sales: " + String(sales.totalCoins) + " coins / " + String(sales.totalMinutes) +
       " minutes (cycles " + String(sales.totalCycles) + ", failed " +
       String(sales.failedCycles) + ", boots " + String(sales.bootCount) + ")</p>";
  h += "<p>Remote log: " + String(strlen(config.logUrl) > 0 ? "enabled" : "off") + "</p>";
  h += "<p>Router: " + String(config.gatewayIp) + " | API port: " + String(config.apiPort) + "</p>";
  if (promo.enabled == 1) {
    h += "<p>Promo rates: ";
    for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
      if (promo.price[i] > 0 && promo.minutes[i] > 0) {
        if (i) h += ", ";
        h += "P" + String(promo.price[i]) + " = " + String(promo.minutes[i]) + " min";
      }
    }
    h += "</p>";
  } else {
    h += "<p>Rate: " + String(config.pulsesPerCoin) + " pulse(s) = " +
         String(config.minutesPerCoin) + " minute(s)</p>";
  }
  h += "<p>Firmware: " FW_REVISION " | Report outbox: " + String(pendingReportCount()) + " item(s)</p>";
  h += "<p>Endpoint: <code>GET /coin?mac=AA:BB:CC:DD:EE:FF</code></p>";
  h += "<p><a href='/insert'>Insert Coin page</a></p>";
  h += "<p><a href='/settings'>Open Settings</a> — admin key is your MikroTik API password.</p>";
  h += "</body></html>";
  return h;
}

// Boot into the WPA2 setup hotspot (unconfigured or failed join recovery).
void startPortal() {
  portalMode = true;
  cycleState = CYCLE_IDLE;
  WiFi.mode(WIFI_AP);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  if (!WiFi.softAP(AP_SSID, config.apPass)) {
    logErr("[AP ERROR] Could not start the WPA2 setup hotspot - falling back to an open hotspot");
    WiFi.softAP(AP_SSID);
  }

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIP);
  server.begin();

  logInfo("[AP] Setup portal online at http://192.168.4.1/ (SSID: " AP_SSID ")");
  logInfo("[AP] Connect your phone to the setup Wi-Fi and open 192.168.4.1");
}

// Join the configured hAP Lite. Falls back to the portal after N failures.
void startStation() {
  portalMode = false;
  WiFi.mode(WIFI_STA);
#ifndef ESP32
  WiFi.setAutoConnect(false);   // ESP8266-only API
#endif
  WiFi.setAutoReconnect(true);
  WiFi.begin(config.ssid, config.pass);

  logInfo("[NETWORK] Connecting to SSID: " + String(config.ssid));

  for (uint8_t attempt = 1; attempt <= WIFI_JOIN_ATTEMPTS; attempt++) {
    uint32_t start = millis();
    while (millis() - start < WIFI_JOIN_TIMEOUT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        logInfo("[NETWORK] Connected to hAP Lite. IP: " + WiFi.localIP().toString());
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
        logInfo("[NETWORK] Coin endpoint ready at http://" + WiFi.localIP().toString() +
                "/coin?mac=AA:BB:CC:DD:EE:FF");
        server.begin();
        return;
      }
      delay(250);
      yield();
    }
    logErr("[NETWORK ERROR] Disconnected from hAP Lite SSID. Attempting recovery... (attempt " +
           String(attempt) + "/" + String(WIFI_JOIN_ATTEMPTS) +
           ", status: " + wifiStatusText(WiFi.status()) + ")");
  }

  logErr("[NETWORK ERROR] Could not join the configured SSID within 45s. Opening the setup portal - the unit keeps retrying the router in the background and starts working on its own once the SSID is back.");
  startPortal();
}

void handleRoot() {
  if (portalMode) {
    server.send(200, "text/html", portalPage("", ""));
  } else {
    server.send(200, "text/html", statusPage());
  }
}

void handleSettings() {
  if (portalMode) {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  String key = urlDecode(server.arg("key"));
  if (key.length() == 0) {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>Settings</title></head>"
               "<body style='font-family:sans-serif;padding:16px'>"
               "<h1>Settings</h1>"
               "<p>Enter the admin key (your MikroTik API password).</p>"
               "<form method='get' action='/settings'>"
               "<input name='key' type='password' style='padding:8px'>"
               "<button type='submit'>Open Settings</button>"
               "</form></body></html>";
    server.send(200, "text/html", h);
    return;
  }
  if (key != String(config.apiPass)) {
    server.send(401, "text/html",
                "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:16px'>"
                "<h1>Wrong key</h1><p><a href='/settings'>Try again</a></p></body></html>");
    return;
  }
  server.send(200, "text/html", portalPage("", key));
}

void handleSave() {
  String key = urlDecode(server.arg("key"));
  if (!portalMode && key != String(config.apiPass)) {
    server.send(401, "text/html",
                "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:16px'>"
                "<h1>Unauthorized</h1><p>Wrong or missing admin key.</p>"
                "<p><a href='/settings'>Back to settings</a></p></body></html>");
    return;
  }

  String ssid   = urlDecode(server.arg("ssid"));
  String pass   = urlDecode(server.arg("wifipass"));
  String gw     = urlDecode(server.arg("gateway"));
  String user   = urlDecode(server.arg("apiuser"));
  String apip   = urlDecode(server.arg("apipass"));
  String port   = urlDecode(server.arg("apiport"));
  String ppc    = urlDecode(server.arg("pulses"));
  String mpc    = urlDecode(server.arg("minutes"));
  String appass = urlDecode(server.arg("appass"));
  String logurl = urlDecode(server.arg("logurl"));
  String devid  = urlDecode(server.arg("deviceid"));
  String usepromo = urlDecode(server.arg("usepromo"));
  String invertgate = urlDecode(server.arg("invertgate"));
  String gatepin = urlDecode(server.arg("gatepin"));
  String txpower = urlDecode(server.arg("txpower"));
  String priceStr[PROMO_SLOTS];
  String promoMinStr[PROMO_SLOTS];
  for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
    priceStr[i]    = urlDecode(server.arg("price" + String(i)));
    promoMinStr[i] = urlDecode(server.arg("promomin" + String(i)));
  }

  String err;
  if (ssid.length() == 0) {
    err = "SSID is required.";
  } else if (!isValidIPv4(gw)) {
    err = "Gateway IP must be a valid IPv4 address (e.g. 192.168.88.1).";
  } else if (user.length() == 0) {
    err = "API username is required.";
  } else if (apip.length() == 0) {
    err = "API password is required.";
  } else if (appass.length() < 8 || appass.length() > 63) {
    err = "Setup AP password must be 8-63 characters (WPA2).";
  } else if (port.toInt() < 1 || port.toInt() > 65535) {
    err = "API port must be between 1 and 65535.";
  } else if (ppc.toInt() < 1 || ppc.toInt() > 100) {
    err = "Pulses per coin must be 1-100.";
  } else if (mpc.toInt() < 1 || mpc.toInt() > 1440) {
    err = "Minutes per coin must be 1-1440.";
  } else if (logurl.length() > 0 &&
             !(logurl.startsWith("http://") || logurl.startsWith("https://"))) {
    err = "Remote Log URL must start with http:// or https:// (or leave empty).";
  } else if (logurl.length() > 96) {
    err = "Remote Log URL is too long (96 characters max).";
  } else if (devid.length() > 32) {
    err = "Device ID must be 32 characters or fewer.";
  }

  for (uint8_t i = 0; i < PROMO_SLOTS && err.length() == 0; i++) {
    int pr = priceStr[i].toInt();
    int mn = promoMinStr[i].toInt();
    if (pr < 0 || pr > 10000 || mn < 0 || mn > 10080) {
      err = "Promo " + String(i + 1) + " is invalid (price 0-10000, minutes 0-10080).";
    }
  }

#ifdef ESP32
  {
    int gp = gatepin.toInt();
    if (err.length() == 0 && (gp < 0 || gp > 39 || (gp >= 6 && gp <= 11))) {
      err = "Gate GPIO pin must be 0-39, excluding 6-11 (those belong to the ESP32's flash memory).";
    }
  }
#else
  if (err.length() == 0 && (gatepin.toInt() < 0 || gatepin.toInt() > 16)) {
    err = "Gate GPIO pin must be between 0 and 16.";
  }
#endif
  if (err.length() == 0 && (txpower.toInt() < 0 || txpower.toInt() > 2)) {
    err = "TX power must be 0 (low), 1 (medium) or 2 (high).";
  }

  if (err.length() > 0) {
    logWarn("[CONFIG] Rejected setup form: " + err);
    server.send(400, "text/html", portalPage(err, key));
    return;
  }

  setField(config.ssid,          sizeof(config.ssid),          ssid);
  setField(config.pass,          sizeof(config.pass),          pass);
  setField(config.gatewayIp,     sizeof(config.gatewayIp),     gw);
  setField(config.apiUser,       sizeof(config.apiUser),       user);
  setField(config.apiPass,       sizeof(config.apiPass),       apip);
  setField(config.apPass,        sizeof(config.apPass),        appass);
  setField(config.logUrl,        sizeof(config.logUrl),        logurl);
  setField(config.deviceId,      sizeof(config.deviceId),      devid);
  config.apiPort        = (uint16_t)port.toInt();
  config.pulsesPerCoin  = (uint16_t)ppc.toInt();
  config.minutesPerCoin = (uint16_t)mpc.toInt();
  config.configured     = 1;

  promo.enabled = (usepromo.length() > 0) ? 1 : 0;
  promo.gateInvert = (invertgate.length() > 0) ? 1 : 0;
  promo.gatePin = (uint8_t)gatepin.toInt();
  promo.txPowerIdx = (uint8_t)txpower.toInt();
  for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
    promo.price[i]   = (uint16_t)priceStr[i].toInt();
    promo.minutes[i] = (uint16_t)promoMinStr[i].toInt();
  }

  saveConfig();
  promoSave();
  logInfo("[CONFIG] Saved new configuration (SSID: " + ssid +
          ", gateway: " + gw + ", API port: " + port +
          ", rate: " + ppc + " pulse(s) = " + mpc + " minute(s))");

  server.send(200, "text/html", savedPage());
  server.client().flush();
  delay(1500);
  ESP.restart();
}

void handleCaptive() {
  if (portalMode) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(204, "text/plain", "");
  }
}

void handleNotFound() {
  if (portalMode) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "application/json",
                "{\"ok\":false,\"error\":\"Not found. Use GET /coin?mac=AA:BB:CC:DD:EE:FF\"}");
  }
}

// ===========================================================================
//  MikroTik RouterOS classic API client (port 8728)
//  Length-prefixed word encoding, sentence protocol, /login + !done/!trap.
// ===========================================================================

class RouterOSApi {
public:
  WiFiClient client;
  IPAddress host;
  uint16_t port;
  String lastError;

  RouterOSApi() : port(DEFAULT_API_PORT) {}

  bool connect(IPAddress ip, uint16_t p) {
    host = ip;
    port = p;
    // Bound the blocking TCP connect so an unreachable router cannot stall
    // the whole sketch (web server + coin state machine) for long.
    client.setTimeout(API_CONNECT_TIMEOUT_MS);
    if (!client.connect(ip, p)) {
      lastError = "[API ERROR] Could not connect to MikroTik at " + ip.toString() +
                  " on Port " + String(p);
      return false;
    }
    client.setNoDelay(true);
    return true;
  }

  int readByteTimed(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (!client.available()) {
      if (!client.connected()) return -1;
      if (millis() - start > timeoutMs) return -1;
      delay(1);
      yield();
    }
    return client.read();
  }

  int32_t readLength(uint32_t timeoutMs) {
    int c = readByteTimed(timeoutMs);
    if (c < 0) return -1;

    if ((c & 0x80) == 0x00) {
      return c;
    }
    if ((c & 0xC0) == 0x80) {
      int c2 = readByteTimed(timeoutMs);
      if (c2 < 0) return -1;
      return ((c & 0x3F) << 8) | c2;
    }
    if ((c & 0xE0) == 0xC0) {
      int c2 = readByteTimed(timeoutMs);
      if (c2 < 0) return -1;
      int c3 = readByteTimed(timeoutMs);
      if (c3 < 0) return -1;
      return ((c & 0x1F) << 16) | (c2 << 8) | c3;
    }
    if ((c & 0xF0) == 0xE0) {
      int c2 = readByteTimed(timeoutMs);
      if (c2 < 0) return -1;
      int c3 = readByteTimed(timeoutMs);
      if (c3 < 0) return -1;
      int c4 = readByteTimed(timeoutMs);
      if (c4 < 0) return -1;
      return ((c & 0x0F) << 24) | (c2 << 16) | (c3 << 8) | c4;
    }
    if ((c & 0xF8) == 0xF0) {
      int c2 = readByteTimed(timeoutMs);
      if (c2 < 0) return -1;
      int c3 = readByteTimed(timeoutMs);
      if (c3 < 0) return -1;
      int c4 = readByteTimed(timeoutMs);
      if (c4 < 0) return -1;
      int c5 = readByteTimed(timeoutMs);
      if (c5 < 0) return -1;
      return ((c & 0x07) << 28) | (c2 << 24) | (c3 << 16) | (c4 << 8) | c5;
    }
    return -1;
  }

  bool writeLength(uint32_t len) {
    if (len < 0x80) {
      return client.write((uint8_t)len) == 1;
    }
    if (len < 0x4000) {
      uint32_t v = len | 0x8000;
      return client.write((uint8_t)((v >> 8) & 0xFF)) == 1 &&
             client.write((uint8_t)(v & 0xFF)) == 1;
    }
    if (len < 0x200000) {
      uint32_t v = len | 0xC00000;
      return client.write((uint8_t)((v >> 16) & 0xFF)) == 1 &&
             client.write((uint8_t)((v >> 8) & 0xFF)) == 1 &&
             client.write((uint8_t)(v & 0xFF)) == 1;
    }
    if (len < 0x10000000) {
      uint32_t v = len | 0xE0000000;
      return client.write((uint8_t)((v >> 24) & 0xFF)) == 1 &&
             client.write((uint8_t)((v >> 16) & 0xFF)) == 1 &&
             client.write((uint8_t)((v >> 8) & 0xFF)) == 1 &&
             client.write((uint8_t)(v & 0xFF)) == 1;
    }
    lastError = "[API ERROR] Command word too long to send to MikroTik";
    return false;
  }

  bool sendSentence(const char* words[], int count) {
    for (int i = 0; i < count; i++) {
      const char* w = words[i];
      uint32_t len = (uint32_t)strlen(w);
      if (!writeLength(len)) {
        lastError = "[API ERROR] Lost connection while sending command to MikroTik on Port " +
                    String(port);
        return false;
      }
      if (client.write((const uint8_t*)w, len) != len) {
        lastError = "[API ERROR] Lost connection while sending command to MikroTik on Port " +
                    String(port);
        return false;
      }
    }
    if (client.write((uint8_t)0) != 1) {
      lastError = "[API ERROR] Lost connection while finalizing command to MikroTik";
      return false;
    }
    return true;
  }

  // Reads one full sentence (words joined with '\n'). Empty word ends it.
  bool readSentence(String& sentence, uint32_t timeoutMs) {
    sentence = "";
    uint32_t start = millis();

    while (true) {
      // Rollover-safe: always subtract two sampled millis() values instead of
      // comparing against a pre-computed deadline (millis() wraps at ~49.7d).
      uint32_t elapsed = millis() - start;
      if (elapsed >= timeoutMs) {
        lastError = "[API ERROR] Timeout waiting for MikroTik reply on Port " + String(port);
        return false;
      }
      int32_t len = readLength(timeoutMs - elapsed);
      if (len < 0) {
        lastError = "[API ERROR] Connection dropped while reading MikroTik reply on Port " +
                    String(port);
        return false;
      }
      if (len == 0) return true;
      if (len > 2048) {
        lastError = "[API ERROR] Oversized word received from MikroTik";
        return false;
      }

      String word;
      word.reserve((unsigned int)len);
      for (int32_t i = 0; i < len; i++) {
        uint32_t elapsedWord = millis() - start;
        if (elapsedWord >= timeoutMs) {
          lastError = "[API ERROR] Timeout waiting for MikroTik reply on Port " + String(port);
          return false;
        }
        int b = readByteTimed(timeoutMs - elapsedWord);
        if (b < 0) {
          lastError = "[API ERROR] Connection dropped while reading MikroTik reply";
          return false;
        }
        word += (char)b;
      }
      if (sentence.length() > 0) sentence += '\n';
      sentence += word;
    }
  }

  static String firstWord(const String& sentence) {
    int idx = sentence.indexOf('\n');
    if (idx < 0) return sentence;
    return sentence.substring(0, idx);
  }

  static String extractPair(const String& sentence, const char* key) {
    String prefix = "=";
    prefix += key;
    prefix += "=";
    int start = 0;
    while (start < (int)sentence.length()) {
      int end = sentence.indexOf('\n', start);
      if (end < 0) end = sentence.length();
      String line = sentence.substring(start, end);
      if (line.startsWith(prefix)) return line.substring(prefix.length());
      start = end + 1;
    }
    return "";
  }

  // Reads sentences until !done (true) or !trap (false + lastError).
  bool waitForTerminal(uint32_t timeoutMs) {
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < timeoutMs) {
      String sentence;
      if (!readSentence(sentence, timeoutMs - (millis() - start))) return false;

      String first = firstWord(sentence);
      if (first == "!done") return true;
      if (first == "!trap") {
        String msg = extractPair(sentence, "message");
        if (msg.length() == 0) msg = sentence;
        lastError = "[API ERROR] MikroTik rejected the command: " + msg;
        return false;
      }
    }
    lastError = "[API ERROR] MikroTik did not answer in time on Port " + String(port);
    return false;
  }

  bool login(const char* user, const char* pass) {
    String u = "=name=" + String(user);
    String p = "=password=" + String(pass);
    const char* words[] = {"/login", u.c_str(), p.c_str()};

    if (!sendSentence(words, 3)) return false;

    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < API_LOGIN_TIMEOUT_MS) {
      String sentence;
      if (!readSentence(sentence, API_LOGIN_TIMEOUT_MS - (millis() - start))) return false;

      String first = firstWord(sentence);
      if (first == "!done") return true;
      if (first == "!trap") {
        lastError = "[API ERROR] MikroTik rejected credentials on Port " + String(port);
        return false;
      }
    }
    lastError = "[API ERROR] MikroTik login timed out on Port " + String(port);
    return false;
  }

  bool runCommand(const char* words[], int count) {
    if (!sendSentence(words, count)) return false;
    return waitForTerminal(API_REPLY_TIMEOUT_MS);
  }

  // Same as runCommand but with a caller-chosen reply budget. /tool/fetch
  // performs a full HTTP download on the router and can exceed the default
  // 4 s under load.
  bool runCommandT(const char* words[], int count, uint32_t timeoutMs) {
    if (!sendSentence(words, count)) return false;
    return waitForTerminal(timeoutMs);
  }

  // Parses a RouterOS duration string into whole minutes. Handles the
  // compact form ("1w2d3h4m5s" subsets) AND the colon form RouterOS uses
  // for times ("hh:mm:ss", "mm:ss", optionally "1d hh:mm:ss"). Sub-minute
  // seconds are ignored (they cannot extend a session). Getting this wrong
  // would corrupt limit-uptime when extending users created by other tools.
  static uint32_t parseUptimeMinutes(const String& v) {
    // Colon form: "[Nd ]hh:mm:ss" or "[Nd ]mm:ss"
    int colon = v.indexOf(':');
    if (colon >= 0) {
      uint32_t days = 0;
      String t = v;
      int space = t.indexOf(' ');
      if (space > 0) {
        days = (uint32_t)t.substring(0, space).toInt();
        t = t.substring(space + 1);
      }
      int first = t.indexOf(':');
      int second = t.indexOf(':', first + 1);
      uint32_t secs;
      if (second >= 0) {   // hh:mm:ss
        secs = (uint32_t)t.substring(0, first).toInt() * 3600UL +
               (uint32_t)t.substring(first + 1, second).toInt() * 60UL +
               (uint32_t)t.substring(second + 1).toInt();
      } else {             // mm:ss
        secs = (uint32_t)t.substring(0, first).toInt() * 60UL +
               (uint32_t)t.substring(first + 1).toInt();
      }
      return days * 1440UL + secs / 60UL;
    }

    uint32_t total = 0;
    uint32_t num = 0;
    bool any = false;
    for (unsigned int i = 0; i < v.length(); i++) {
      char ch = v.charAt(i);
      if (ch >= '0' && ch <= '9') {
        num = num * 10 + (uint32_t)(ch - '0');
        any = true;
      } else {
        switch (ch) {
          case 'w': total += num * 10080UL; break;
          case 'd': total += num * 1440UL;  break;
          case 'h': total += num * 60UL;    break;
          case 'm': total += num;           break;
          default: /* 's' and unknown units: sub-minute, skip */ break;
        }
        num = 0;
      }
    }
    return any ? total : 0;
  }

  // Finds an existing hotspot user by name. Returns true on a completed
  // lookup and fills userId (".id", "" if the user does not exist) and
  // limitUptime (current limit-uptime value, "" if unset).
  bool findUser(const char* name, String& userId, String& limitUptime) {
    userId = "";
    limitUptime = "";
    String query = "?name=" + String(name);
    const char* words[] = {"/ip/hotspot/user/print",
                           "=.proplist=.id,limit-uptime", query.c_str()};

    if (!sendSentence(words, 3)) return false;

    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < API_REPLY_TIMEOUT_MS) {
      String sentence;
      if (!readSentence(sentence, API_REPLY_TIMEOUT_MS - (millis() - start))) return false;

      String first = firstWord(sentence);
      if (first == "!done") return true;
      if (first == "!trap") {
        String msg = extractPair(sentence, "message");
        lastError = "[API ERROR] MikroTik user lookup failed: " + msg;
        return false;
      }
      if (first == "!re") {
        userId = extractPair(sentence, ".id");
        limitUptime = extractPair(sentence, "limit-uptime");
        return true;
      }
    }
    lastError = "[API ERROR] MikroTik user lookup timed out on Port " + String(port);
    return false;
  }

  // Finds a LIVE hotspot session for a user (the phone already logged in).
  // Returns true when the lookup completed; sessionId is "" when the user is
  // not online. Used to kick-and-re-login after a credit (see below).
  bool findActiveSessionId(const char* name, String& sessionId) {
    sessionId = "";
    String query = "?user=" + String(name);
    const char* words[] = {"/ip/hotspot/active/print", "=.proplist=.id", query.c_str()};

    if (!sendSentence(words, 3)) return false;

    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < API_REPLY_TIMEOUT_MS) {
      String sentence;
      if (!readSentence(sentence, API_REPLY_TIMEOUT_MS - (millis() - start))) return false;

      String first = firstWord(sentence);
      if (first == "!done") return true;
      if (first == "!trap") {
        String msg = extractPair(sentence, "message");
        lastError = "[API ERROR] MikroTik active-session lookup failed: " + msg;
        return false;
      }
      if (first == "!re") {
        sessionId = extractPair(sentence, ".id");
        return true;
      }
    }
    lastError = "[API ERROR] MikroTik active-session lookup timed out";
    return false;
  }

  // Ends a RUNNING hotspot session for a user, if any (kick-before-credit).
  // RouterOS keeps running sessions on their own budget and syncs the user
  // record back down to that budget, so the record must ONLY be written
  // while the user is offline. True = lookup completed (removed tells
  // whether a live session actually existed).
  bool removeLiveSession(const char* name, bool& removed) {
    removed = false;
    String sessionId;
    if (!findActiveSessionId(name, sessionId)) return false;
    if (sessionId.length() > 0) {
      String rmId = "=.id=" + sessionId;
      const char* rm[] = {"/ip/hotspot/active/remove", rmId.c_str()};
      if (!runCommand(rm, 2)) return false;
      removed = true;
    }
    return true;
  }

  // Time-preserving credit using an ALREADY-FETCHED user row.
  // performCredit() does the lookup BEFORE ending any live session, because
  // RouterOS (6.49.x tested) can transiently report "no rows" for
  // /ip/hotspot/user/print right after /ip/hotspot/active/remove - which
  // used to make the bridge re-create an existing voucher as a brand-new
  // user and the customer's extension vanished into a ghost row.
  // If the user exists, EXTEND its limit-uptime by the purchased minutes so
  // the customer keeps any remaining time. Never removes a user, so an
  // active session is never interrupted. Only a brand-new voucher gets a
  // fresh /ip/hotspot/user/add.
  //
  // Users are created with NO password: the JuanFi hotspot portal logs
  // customers in with username = voucher code and an empty password
  // (loginOption = 0 CHAP hash of ""), so an empty password is what makes
  // the auto-login succeed.
  bool creditUserPre(const String& name, uint16_t minutes,
                     const String& id, const String& current) {
    if (id.length() > 0) {
      uint32_t existing = parseUptimeMinutes(current);
      uint32_t total = existing + (uint32_t)minutes;
      if (total > 43200UL) total = 43200UL;   // clamp extension to 30 days
      String setId    = "=.id=" + id;
      String setLimit = "=limit-uptime=" + String(total) + "m";
      const char* set[] = {"/ip/hotspot/user/set", setId.c_str(), setLimit.c_str()};
      if (!runCommand(set, 3)) return false;
      logInfo("[API] Extended hotspot user " + name + " from " + String(existing) +
              " to " + String(total) + " minute(s)");
      return true;
    }

    String addName  = "=name=" + name;
    String addProf  = "=profile=" COIN_HOTSPOT_PROFILE;
    String addLimit = "=limit-uptime=" + String(minutes) + "m";
    String addCmt   = "=comment=PisoWiFi coin credit";
    const char* add[] = {
      "/ip/hotspot/user/add",
      addName.c_str(), addProf.c_str(), addLimit.c_str(), addCmt.c_str()
    };

    if (!runCommand(add, 5)) {
      // Fallback: the configured profile may not exist on this router
      // (MikroTik rejects with "input does not match any value of profile").
      // Retry WITHOUT the profile so the customer still gets credited with
      // the hotspot's default profile instead of losing the purchase.
      const char* addNoProf[] = {
        "/ip/hotspot/user/add",
        addName.c_str(), addLimit.c_str(), addCmt.c_str()
      };
      if (!runCommand(addNoProf, 4)) return false;
      logWarn("[API] Profile \"" COIN_HOTSPOT_PROFILE "\" not found on router - user " +
              name + " created with the default profile. Create the profile for custom rates.");
      return true;
    }
    logInfo("[API] Hotspot user " + name + " credited with " + String(minutes) + " minute(s)");
    return true;
  }

  // R7.3: publish the voucher-per-MAC auto-login file with /file/print +
  // /file/set contents over the SAME API connection as the credit. The old
  // /tool fetch made the router fetch HTTP from the ESP32 - that inbound
  // request could time out while the ESP32 was busy inside the credit, which
  // left the customer without auto-login. Verified on RouterOS 6.49.17.
  bool writeVoucherDataFile(const String& macNoColon, const String& voucher) {
    if (macNoColon.length() < 8 || voucher.length() == 0) return false;
    String path = "hotspot/data/" + macNoColon + ".txt";
    String printFile = "=file=" + path;
    const char* mk[] = {"/file/print", printFile.c_str(), "?name=dummyfile"};
    if (!runCommandT(mk, 3, 5000)) return false;
    String numbers  = "=numbers=" + path;
    String contents = "=contents=" + voucher + "#";
    const char* st[] = {"/file/set", numbers.c_str(), contents.c_str()};
    return runCommandT(st, 3, 5000);
  }

  void disconnect() {
    if (client.connected()) client.stop();
  }
};

// ---------------------------------------------------------------------------
// Codeless voucher generation (JuanFi-style "P" + 5 random digits)
// ---------------------------------------------------------------------------

String generateVoucherCode() {
  for (uint8_t attempt = 0; attempt < 8; attempt++) {
    uint32_t r;
#ifdef ESP32
    r = esp_random();
#else
    r = RANDOM_REG32;   // ESP8266 hardware RNG
#endif
    char buf[8];
    snprintf(buf, sizeof(buf), "P%05u", (unsigned)(r % 100000UL));
    String code(buf);
    if (code.length() == 6) return code;
  }
  return String("P00000");   // unreachable safety net
}

// True when the lookup completed; known=true means the hotspot user exists.
// Returns false when the router could not be reached (result unknown).
bool hotspotUserExists(const String& name, bool& known) {
  known = false;
  if (WiFi.status() != WL_CONNECTED) return false;
  IPAddress gw;
  if (!parseIPv4(config.gatewayIp, gw)) return false;

  RouterOSApi api;
  bool connected = false;
  for (uint8_t attempt = 0; attempt < 2 && !connected; attempt++) {
    if (attempt) delay(100);
    connected = api.connect(gw, config.apiPort);
  }
  if (!connected) return false;
  if (!api.login(config.apiUser, config.apiPass)) { api.disconnect(); return false; }

  String id, lim;
  known = api.findUser(name.c_str(), id, lim);
  api.disconnect();
  return true;   // query completed - "known" is trustworthy
}

// ---------------------------------------------------------------------------
// High-level credit operation
// ---------------------------------------------------------------------------
// userName is the hotspot username to credit - the voucher code from the
// hotspot page (preferred, JuanFi-compatible) or the colon-stripped MAC when
// the page sends no voucher. The caller guarantees it is colon-free.
// macNoColon (optional, uppercase, no colons) makes the bridge publish the
// voucher-per-MAC file the login page needs for returning-customer
// auto-login after an idle disconnect (hotspot/data/<MAC>.txt).
bool performCredit(const String& userName, uint16_t minutes, String& err,
                   const String& macNoColon = String("")) {
  if (WiFi.status() != WL_CONNECTED) {
    err = "[NETWORK ERROR] Disconnected from hAP Lite SSID. Attempting recovery...";
    return false;
  }

  IPAddress gw;
  if (!parseIPv4(config.gatewayIp, gw)) {
    err = "[CONFIG ERROR] Stored gateway IP is invalid - reopen the setup portal";
    return false;
  }

  RouterOSApi api;
  // Two connect attempts: a single transient TCP refusal is common right
  // after Wi-Fi reassociation. A genuine outage fails both attempts and is
  // then handled by the pending-credit retry (see tickPendingCredit).
  bool connected = false;
  for (uint8_t attempt = 0; attempt < 2 && !connected; attempt++) {
    if (attempt) delay(250);
    connected = api.connect(gw, config.apiPort);
    if (!connected) err = api.lastError;
  }
  if (!connected) return false;
  if (!api.login(config.apiUser, config.apiPass)) { err = api.lastError; return false; }

  // READ-FIRST, KICK, THEN WRITE (crash-safe, race-free):
  // 1. Look the user up BEFORE ending any session. Right after
  //    /ip/hotspot/active/remove, RouterOS can transiently answer
  //    /ip/hotspot/user/print with no rows; reading first means an existing
  //    voucher can never be mistaken for a brand-new one.
  // 2. End the live session (if any) - the record itself is left untouched,
  //    and a running session would otherwise pin the record down to its own
  //    old budget within seconds (tested on RouterOS 6.49.17).
  // 3. If the lookup found no row, retry once after a short settle delay
  //    (genuinely new vouchers still end up in the add path).
  // 4. Extend the existing row (or add a truly new user).
  String userId, currentLimit;
  if (!api.findUser(userName.c_str(), userId, currentLimit)) {
    err = api.lastError;
    return false;
  }

  bool sessionEnded = false;
  if (api.removeLiveSession(userName.c_str(), sessionEnded)) {
    if (sessionEnded) {
      logInfo("[API] Ended live session of " + userName + " before crediting");
    }
  } else {
    logWarn("[API] Could not check live session of " + userName + ": " + api.lastError);
  }

  if (userId.length() == 0) {
    delay(400);   // let the session teardown settle, then re-lookup once
    if (!api.findUser(userName.c_str(), userId, currentLimit)) {
      err = api.lastError;
      return false;
    }
  }

  if (!api.creditUserPre(userName, minutes, userId, currentLimit)) {
    err = api.lastError;
    reportEvent("credit_fail", 2, err, userName, macNoColon, 0, minutes);
    return false;
  }

  // FIX A: publish the voucher for this MAC while the connection is already
  // open - the login page auto-logs returning phones with it (only the file
  // write can fail here; the credit above already landed).
  if (macNoColon.length() >= 8) {
    // R7.3: one quick attempt on the credit connection. On failure the job is
    // queued in NVS and retried from loop() until it lands, so a transient
    // failure never leaves a customer without auto-login (and the recovery
    // closes the audit trail).
    if (api.writeVoucherDataFile(macNoColon, userName)) {
      logInfo("[DATA] Wrote hotspot/data/" + macNoColon + ".txt -> " + userName);
    } else {
      logWarn("[DATA] Could not write hotspot/data/" + macNoColon + ".txt: " + api.lastError);
      reportEvent("voucher_file_failed", 1,
                  "Auto-login file write failed for " + userName + " (" + api.lastError +
                  ") - queued for automatic retry",
                  userName, macNoColon, 0, minutes);
      pubqEnqueue(userName, macNoColon);
    }
  }

  api.disconnect();
  return true;
}

// ---------------------------------------------------------------------------
// Crash-safe pending credit recovery
// ---------------------------------------------------------------------------
// If coins are counted but the MikroTik credit fails (Wi-Fi drop, router
// reboot, power blip), the customer must not lose money. The credit is
// persisted to EEPROM immediately and retried automatically until it lands.
// (Struct and globals live near the top of the file so the Arduino
// auto-prototype pass sees them before any generated prototypes.)

bool pendingSlotValid(const PendingSlot& s) {
  if (s.magic != PENDING_MAGIC) return false;
  if (s.minutes < 1 || s.minutes > MAX_CREDIT_MINUTES) return false;
  if (s.name[0] == 0) return false;
  if (s.name[PENDING_NAME_MAX - 1] != 0) return false;   // must be NUL-terminated
  for (uint8_t i = 0; i < PENDING_NAME_MAX; i++) {
    char ch = s.name[i];
    if (ch == 0) break;
    if (ch < 0x21 || ch > 0x7E) return false;   // printable ASCII, no whitespace
  }
  for (uint8_t i = 0; i < PENDING_MAC_MAX; i++) {
    char ch = s.mac[i];
    if (ch == 0) break;
    if (ch < 0x21 || ch > 0x7E) return false;
  }
  return true;
}

uint8_t pendingCount() {
  if (pendingStore.magic != PENDING_STORE_MAGIC) return 0;
  if (pendingStore.count > PENDING_SLOTS) return 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < PENDING_SLOTS; i++) {
    if (pendingSlotValid(pendingStore.slot[i])) n++;
  }
  return n;
}

bool pendingAny() { return pendingCount() > 0; }

int8_t pendingFindIndex(const String& name) {
  for (uint8_t i = 0; i < PENDING_SLOTS; i++) {
    if (pendingSlotValid(pendingStore.slot[i]) &&
        name.equalsIgnoreCase(String(pendingStore.slot[i].name))) return (int8_t)i;
  }
  return -1;
}

void pendingPersist() {
  persPut(PENDING_EEPROM_OFFSET, "pend3", pendingStore);
}

void pendingSave(const String& userName, uint16_t minutes, const String& macNoColon) {
  if (minutes < 1) return;
  if (pendingStore.magic != PENDING_STORE_MAGIC) {
    memset(&pendingStore, 0, sizeof(pendingStore));
    pendingStore.magic = PENDING_STORE_MAGIC;
  }
  String nm = userName;
  nm.trim();
  if (nm.length() > PENDING_NAME_MAX - 1) nm = nm.substring(0, PENDING_NAME_MAX - 1);
  String mc = macNoColon;
  mc.toUpperCase();
  mc.replace(":", "");
  if (mc.length() > PENDING_MAC_MAX - 1) mc = mc.substring(0, PENDING_MAC_MAX - 1);

  int8_t idx = pendingFindIndex(nm);
  if (idx < 0) {
    for (uint8_t i = 0; i < PENDING_SLOTS; i++) {
      if (!pendingSlotValid(pendingStore.slot[i])) { idx = (int8_t)i; break; }
    }
  }
  if (idx < 0) {
    // Store full (mid-session race; new sessions are refused while full).
    // MERGE into the smallest record instead of dropping the value, so the
    // total money owed is always preserved. The event names both vouchers so
    // an operator can reconcile the attribution during a refund.
    uint8_t worst = 0;
    for (uint8_t i = 1; i < PENDING_SLOTS; i++) {
      if (pendingStore.slot[i].minutes < pendingStore.slot[worst].minutes) worst = i;
    }
    PendingSlot& w = pendingStore.slot[worst];
    uint32_t merged = (uint32_t)w.minutes + (uint32_t)minutes;
    if (merged > 65535UL) merged = 65535UL;
    String prevName(w.name);
    w.minutes = (uint16_t)merged;
    if (mc.length() > 0) strncpy(w.mac, mc.c_str(), sizeof(w.mac) - 1);
    logErr("[PENDING] Recovery store full - merged " + String(minutes) + " min into " + prevName);
    reportEvent("pending_overflow_merged", 2,
                "Recovery store full: merged " + nm + " (" + String(minutes) + " min) into " + prevName,
                nm, mc, 0, minutes);
    pendingPersist();
    eepromCommitNow();
    return;
  }
  PendingSlot& s = pendingStore.slot[(uint8_t)idx];
  bool existed = pendingSlotValid(s);
  uint16_t keep = (existed && s.minutes > minutes) ? s.minutes : minutes;   // monotone
  memset(&s, 0, sizeof(s));
  s.magic = PENDING_MAGIC;
  s.minutes = keep;
  strncpy(s.name, nm.c_str(), sizeof(s.name) - 1);
  if (mc.length() > 0) strncpy(s.mac, mc.c_str(), sizeof(s.mac) - 1);
  pendingStore.count = pendingCount();
  pendingPersist();
  eepromCommitNow();   // critical record - never coalesce a coin-taking write
  logWarn("[PENDING] Saved uncredited purchase: " + nm + " " + String(keep) +
          " minute(s) mac=" + mc + " - will retry automatically");
}

void pendingRemoveAt(uint8_t idx) {
  if (idx >= PENDING_SLOTS) return;
  memset(&pendingStore.slot[idx], 0, sizeof(pendingStore.slot[idx]));
  pendingStore.count = pendingCount();
  pendingPersist();
  eepromCommitNow();
}

// Removes only THIS customer's records - a successful credit for one voucher
// must never erase a different customer's uncredited purchase.
void pendingClearFor(const String& userName) {
  bool changed = false;
  for (uint8_t i = 0; i < PENDING_SLOTS; i++) {
    if (pendingSlotValid(pendingStore.slot[i]) &&
        userName.equalsIgnoreCase(String(pendingStore.slot[i].name))) {
      memset(&pendingStore.slot[i], 0, sizeof(pendingStore.slot[i]));
      changed = true;
    }
  }
  if (!changed) return;
  pendingStore.count = pendingCount();
  pendingPersist();
  eepromCommitNow();
}

void pendingClear() {
  memset(&pendingStore, 0, sizeof(pendingStore));
  pendingStore.magic = PENDING_STORE_MAGIC;
  pendingPersist();
  eepromCommitNow();
}

void pendingLoad() {
  memset(&pendingStore, 0, sizeof(pendingStore));
  bool have = persGet(PENDING_EEPROM_OFFSET, "pend3", pendingStore);
  if (!have || pendingStore.magic != PENDING_STORE_MAGIC) {
    // Migrate the legacy single-slot record (firmware <= R6) if present.
    memset(&pendingLegacy, 0, sizeof(pendingLegacy));
    if (persGet(PENDING_EEPROM_OFFSET, "pending", pendingLegacy) &&
        pendingLegacy.magic == 0x50435232UL &&
        pendingLegacy.minutes >= 1 && pendingLegacy.minutes <= MAX_CREDIT_MINUTES &&
        pendingLegacy.name[0] != 0) {
      logWarn("[PENDING] Migrating legacy single-slot recovery record");
      pendingSave(String(pendingLegacy.name), pendingLegacy.minutes, String(""));
    } else {
      pendingStore.magic = PENDING_STORE_MAGIC;
      pendingStore.count = 0;
    }
  }
  uint8_t n = pendingCount();
  if (n == 0) {
    logInfo("[PENDING] No uncredited purchases on record");
    return;
  }
  pendingNextAttemptMs = millis() + 5000;
  for (uint8_t i = 0; i < PENDING_SLOTS; i++) {
    if (!pendingSlotValid(pendingStore.slot[i])) continue;
    logWarn("[PENDING] Uncredited purchase found: " + String(pendingStore.slot[i].name) +
            " " + String(pendingStore.slot[i].minutes) + " minute(s) mac=" +
            String(pendingStore.slot[i].mac));
  }
}

// Retry loop, called from loop() while station mode is up. Waits for an idle
// coin state and a healthy Wi-Fi link, then re-attempts the OLDEST credit.
void tickPendingCredit() {
  if (portalMode || !pendingAny()) return;
  if (cycleState != CYCLE_IDLE) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(millis() - pendingNextAttemptMs) < 0) return;

  pendingNextAttemptMs = millis() + PENDING_RETRY_MS;
  for (uint8_t i = 0; i < PENDING_SLOTS; i++) {
    if (!pendingSlotValid(pendingStore.slot[i])) continue;
    String userName(pendingStore.slot[i].name);
    String macNC(pendingStore.slot[i].mac);
    uint16_t mins = pendingStore.slot[i].minutes;
    String err;
    logInfo("[PENDING] Retrying credit for " + userName + " (" + String(mins) + " min)...");
    if (performCredit(userName, mins, err, macNC)) {
      reportEvent("pending_recovered", 0,
                  "Recovered " + String(mins) + " min for " + userName,
                  userName, macNC, 0, mins);
      salesRecordSale(0, mins);   // coin count unknown across the failure
      remoteEnqueue(String("[INFO] RECOVERED credit ") + userName + " " + String(mins) + "min");
      logInfo("[PENDING] Credit recovered for " + userName);
      pendingRemoveAt(i);
    } else {
      reportEvent("pending_retry_failed", 2, err, userName, macNC, 0, mins);
      logWarn("[PENDING] Retry failed: " + err);
      remoteEnqueue(String("[ERROR] Pending credit retry failed: ") + err);
    }
    return;   // one slot per tick
  }
}

// ---------------------------------------------------------------------------
// Deferred auto-login file publication (R7.3)
// ---------------------------------------------------------------------------
// Retries queued voucher-file jobs while the bridge is idle. It uses a fresh
// API connection and the same /file/print + /file/set commands as the credit
// path - no inbound HTTP from the router, so it cannot deadlock against the
// coin session. Success raises voucher_file_recovered so the audit trail shows
// the problem resolved.
void tickVoucherPublish() {
  if (portalMode || pubqCount() == 0) return;
  if (cycleState != CYCLE_IDLE) return;
  if (pendingAny()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if ((int32_t)(millis() - pubqNextAttemptMs) < 0) return;
  pubqNextAttemptMs = millis() + PUBQ_RETRY_MS;

  IPAddress gw;
  if (!parseIPv4(config.gatewayIp, gw)) return;
  for (uint8_t i = 0; i < PUBQ_SLOTS; i++) {
    PubJob& j = pubQueue.job[i];
    if (j.magic != PUBQ_JOB_MAGIC || j.voucher[0] == 0) continue;
    RouterOSApi api;
    bool connected = false;
    for (uint8_t attempt = 0; attempt < 2 && !connected; attempt++) {
      if (attempt) delay(200);
      connected = api.connect(gw, config.apiPort);
    }
    if (!connected) return;
    if (!api.login(config.apiUser, config.apiPass)) { api.disconnect(); return; }
    String vc(j.voucher);
    String mc(j.mac);
    if (api.writeVoucherDataFile(mc, vc)) {
      api.disconnect();
      logInfo("[PUBQ] Recovered auto-login file for " + vc);
      reportEvent("voucher_file_recovered", 0,
                  "Auto-login file written for " + vc + " after an automatic retry",
                  vc, mc, 0, 0);
      pubqRemoveAt(i);
    } else {
      logWarn("[PUBQ] Retry failed for " + vc + ": " + api.lastError);
      api.disconnect();
    }
    return;   // one job per tick
  }
}

// Admin: write one auto-login file on demand so the mechanism can be verified
// in the field without taking a coin.
void handlePublishTest() {
  String key = urlDecode(server.arg("key"));
  if (key != String(config.apiPass)) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  String macRaw = urlDecode(server.arg("mac"));
  String voucher = urlDecode(server.arg("voucher"));
  voucher.trim();
  String macNorm;
  if (!normalizeMac(macRaw, macNorm) || voucher.length() == 0) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"need mac=AA:BB:CC:DD:EE:FF&voucher=P12345\"}");
    return;
  }
  String macNC = macNorm;
  macNC.replace(":", "");
  macNC.toUpperCase();
  if (portalMode || WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"not connected\"}");
    return;
  }
  IPAddress gw;
  if (!parseIPv4(config.gatewayIp, gw)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"bad gateway\"}");
    return;
  }
  RouterOSApi api;
  bool connected = false;
  for (uint8_t attempt = 0; attempt < 2 && !connected; attempt++) {
    if (attempt) delay(200);
    connected = api.connect(gw, config.apiPort);
  }
  if (!connected || !api.login(config.apiUser, config.apiPass)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"api connect/login failed\"}");
    return;
  }
  bool ok = api.writeVoucherDataFile(macNC, voucher);
  String err = api.lastError;
  api.disconnect();
  server.send(200, "application/json",
              String("{\"ok\":") + (ok ? "true" : "false") +
              ",\"mac\":\"" + macNC + "\",\"voucher\":\"" + jsonEscape(voucher) +
              "\",\"error\":\"" + jsonEscape(err) + "\"}");
}

// Loads the nightly-restart marker so a reboot within the same restart hour
// never restarts twice (the marker must survive the restart).
void nightlyLoad() {
  if (!persGet(NIGHTLY_RESTART_OFFSET, "nightly", nightly)) {
    persGetLegacy(NIGHTLY_RESTART_OFFSET, nightly);
  }
  if (nightly.magic != NIGHTLY_RESTART_MAGIC) {
    nightly.magic = 0;
    nightly.lastDay = 0;
  }
}

// Nightly maintenance restart. Runs once per minute; restarts only if:
//   - NTP time is valid,
//   - it is the configured UTC hour,
//   - this epoch day has not had its restart yet (persisted, so the fresh
//     boot does not immediately restart again), and
//   - the coinslot is idle (customer insertion is never interrupted).
void tickNightlyRestart() {
  if ((uint32_t)(millis() - nightlyLastCheckMs) < NIGHTLY_RESTART_CHECK_MS) return;
  nightlyLastCheckMs = millis();

  time_t now = time(nullptr);
  if (now <= 1000000000L) return;   // NTP not synced yet

  if ((uint8_t)((now / 3600UL) % 24UL) != NIGHTLY_RESTART_UTC_HOUR) return;

  uint32_t epochDay = (uint32_t)(now / 86400UL);
  if (nightly.magic == NIGHTLY_RESTART_MAGIC &&
      nightly.lastDay == (uint16_t)(epochDay & 0xFFFFUL)) {
    return;   // already restarted today
  }

  if (cycleState != CYCLE_IDLE) return;   // defer until the customer is done
  if (WiFi.status() != WL_CONNECTED) return;

  nightly.magic = NIGHTLY_RESTART_MAGIC;
  nightly.lastDay = (uint16_t)(epochDay & 0xFFFFUL);
  persPut(NIGHTLY_RESTART_OFFSET, "nightly", nightly);
  eepromCommitNow();

  logWarn("[SYSTEM] Nightly maintenance restart (coinslot idle) - restarting now");
  remoteEnqueue(String("[WARN] Nightly maintenance restart"));
  delay(500);
  ESP.restart();
}

// ===========================================================================
//  Coin insertion cycle + local credit endpoint
// ===========================================================================

// CHANGE-edge ISR with dual rejection: the line must have been HIGH for a
// sustained pause (MIN_HIGH_HOLD_US) before the falling edge, and pulses must
// be spaced at least MIN_PULSE_GAP_US apart. The coin line idles HIGH (NO
// switch); a real FAST pulse pulls it LOW ~20 ms then back HIGH. This rejects
// both bounce and sustained electrical oscillation (e.g. a 40 Hz phantom
// train), because noise never holds the line HIGH long enough to pass.
#ifdef ESP32
IRAM_ATTR void coinIsr() {
#else
ICACHE_RAM_ATTR void coinIsr() {
#endif
  uint32_t now = micros();
  if (digitalRead(PIN_COIN) == HIGH) {
    lastHighUs = now;
  } else if ((now - lastHighUs) >= MIN_HIGH_HOLD_US) {
    uint32_t gap = now - lastPulseUs;
    if (gap >= MIN_PULSE_GAP_US) {
      // Per-coin segmentation: a gap >= COIN_BURST_GAP_US means the previous
      // coin's train finished - hand its pulse count to the main loop ring
      // (the loop turns it into minutes; the ISR only counts).
      if (openBurstPulses > 0 && gap >= COIN_BURST_GAP_US) {
        if (burstQCount < BURST_QUEUE_MAX) {
          uint8_t idx = (uint8_t)((burstQHead + burstQCount) % BURST_QUEUE_MAX);
          burstQueue[idx] = openBurstPulses;
          burstQCount++;
        } else {
          burstOverflowPulses += openBurstPulses;   // safety: merged pricing never underpays
        }
        lastBurstGapMs = (uint16_t)(gap / 1000UL);
        openBurstPulses = 0;
      }
      openBurstPulses++;
      if (volatilePulses < 0xFFFF) volatilePulses++;
      lastPulseUs = now;
    }
  }
}

uint16_t readPulses() {
  noInterrupts();
  uint16_t p = volatilePulses;
  interrupts();
  return p;
}

// Minutes for ONE coin (one burst of N pulses), greedy over the promo rate
// table. Runs in the main loop only (never from the ISR).
uint32_t coinBurstValue(uint16_t pulses) {
  if (promo.enabled == 1) {
    uint16_t remaining = pulses;
    uint32_t minutes = 0;
    uint8_t guard = 0;
    while (remaining > 0 && guard++ < 64) {
      uint16_t bestPrice = 0;
      uint16_t bestMinutes = 0;
      for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
        uint16_t pr = promo.price[i];
        if (pr > 0 && promo.minutes[i] > 0 && pr <= remaining && pr > bestPrice) {
          bestPrice = pr;
          bestMinutes = promo.minutes[i];
        }
      }
      if (bestPrice == 0) break;
      minutes += bestMinutes;
      remaining -= bestPrice;
    }
    if (minutes > MAX_CREDIT_MINUTES) minutes = MAX_CREDIT_MINUTES;
    return minutes;
  }
  // Linear mode: minutesPerCoin per pulsesPerCoin (bursts below one coin
  // price nothing - same floor as the old linear rate math).
  uint32_t coins = (uint32_t)pulses / config.pulsesPerCoin;
  return coins * (uint32_t)config.minutesPerCoin;
}

// Banks every burst the ISR finished since we last looked. Loop-only.
void settleCoinBursts() {
  while (true) {
    uint16_t size;
    noInterrupts();
    if (burstQCount == 0) {
      interrupts();
      break;
    }
    size = burstQueue[burstQHead];
    burstQHead = (uint8_t)((burstQHead + 1) % BURST_QUEUE_MAX);
    burstQCount--;
    interrupts();
    uint32_t v = coinBurstValue(size);
    if (v > 0) {
      burstBankMinutes += v;
      burstCoinCount++;
      logInfo("[COIN] Coin burst closed: " + String(size) + " pulse(s) -> +" +
              String(v) + " min (gap " + String(lastBurstGapMs) + " ms, bank " +
              String(burstBankMinutes) + " min)");
    } else {
      // Burst matched no rate (only possible with an exotic table). The raw
      // pulses stay counted for the customer display; nothing is consumed.
      logWarn("[COIN] Burst of " + String(size) + " pulse(s) matched no rate - kept visible");
    }
  }
  noInterrupts();
  uint16_t overflow = burstOverflowPulses;
  burstOverflowPulses = 0;
  interrupts();
  if (overflow > 0) {
    uint32_t v = coinBurstValue(overflow);
    if (v > 0) {
      burstBankMinutes += v;
      burstCoinCount++;
      logWarn("[COIN] Burst queue overflow - merged " + String(overflow) +
              " pulse(s) -> +" + String(v) + " min (never less than separate coins)");
    }
  }
}

// Closes and banks the trailing coin (the one whose train has finished).
// Only call when the line is idle or the ISR is detached, so no pulse can
// still be in flight. Returns the total session value in minutes (clamped).
uint32_t bankOpenBurst() {
  uint32_t v = 0;
  uint16_t count = 0;
  noInterrupts();
  if (openBurstPulses > 0) {
    count = openBurstPulses;
    v = coinBurstValue(count);
    openBurstPulses = 0;
  }
  interrupts();
  if (v > 0) {
    burstBankMinutes += v;
    burstCoinCount++;
    logInfo("[COIN] Coin burst closed (end): " + String(count) +
            " pulse(s) -> +" + String(v) + " min (bank " +
            String(burstBankMinutes) + " min)");
  }
  if (burstBankMinutes > MAX_CREDIT_MINUTES) burstBankMinutes = MAX_CREDIT_MINUTES;
  return burstBankMinutes;
}

// Total session value right now: banked coins plus the (possibly partial)
// coin still dropping. Estimate only - the open burst is not consumed.
uint32_t sessionValueEstimate() {
  noInterrupts();
  uint32_t v = burstBankMinutes;
  if (openBurstPulses > 0) v += coinBurstValue(openBurstPulses);
  interrupts();
  if (v > MAX_CREDIT_MINUTES) v = MAX_CREDIT_MINUTES;
  return v;
}

// Resets all per-coin burst state (session start / cancel).
void resetBurstState() {
  noInterrupts();
  openBurstPulses = 0;
  burstQHead = 0;
  burstQCount = 0;
  burstOverflowPulses = 0;
  interrupts();
  burstBankMinutes = 0;
  burstCoinCount = 0;
  lastBurstGapMs = 0;
}

// Before arming the ISR, make sure the coin line is not being pulled LOW by
// solenoid switching noise. Returns true if the line stayed HIGH the whole
// time.
bool waitForIdleLine(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    if (digitalRead(PIN_COIN) == LOW) return false;
    delay(1);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Gate polarity helpers
// ---------------------------------------------------------------------------
uint8_t gatePin()        { return promo.gatePin; }
uint8_t gateOpenLevel()  { return (promo.gateInvert == 1) ? LOW : HIGH; }
uint8_t gateCloseLevel() { return (promo.gateInvert == 1) ? HIGH : LOW; }

float txPowerDbm() {
  if (promo.txPowerIdx == 0) return 10.0;
  if (promo.txPowerIdx == 2) return 20.5;
  return 15.0;
}

// Runs the full insertion cycle synchronously:
//   ARM -> gate open -> COUNT -> gate close -> CREDIT -> back to IDLE.
// On every path the gate is closed and the coin ISR is detached before we
// return, and cycleState ends back at CYCLE_IDLE.
bool runCoinCycle(const String& mac, uint16_t& outPulses, uint16_t& outCoins,
                  uint16_t& outMinutes, String& err) {
  cycleState = CYCLE_ARMING;

  // Detach any stale interrupt and zero the tally before arming.
  detachInterrupt(digitalPinToInterrupt(PIN_COIN));
  noInterrupts();
  volatilePulses = 0;
  lastPulseUs = micros();
  lastHighUs = micros();
  interrupts();
  resetBurstState();

  digitalWrite(gatePin(), gateOpenLevel());
  logInfo("[COIN] Gate opened. Waiting for solenoid to settle...");
  delay(GATE_OPEN_SETTLE_MS);

  if (!waitForIdleLine(COIN_ARM_IDLE_MS)) {
    logWarn("[COIN] Coin line not idle after arming - noise will be rejected by debounce");
  }

  attachInterrupt(digitalPinToInterrupt(PIN_COIN), coinIsr, CHANGE);
  cycleState = CYCLE_COUNTING;
  logInfo("[COIN] Coin slot armed. Insert coins now (FAST pulses on D6)...");

  uint32_t countingStart = millis();
  uint32_t lastPulseMs = 0;
  uint16_t lastSeen = 0;
  bool gotAny = false;

  while (true) {
    delay(1);   // keeps the Wi-Fi stack alive while we poll
    yield();

    uint16_t pulses = readPulses();
    if (pulses != lastSeen) {
      lastSeen = pulses;
      lastPulseMs = millis();
      gotAny = true;
      logInfo("[COIN] Pulse detected - total: " + String(pulses));
      if (pulses >= MAX_PULSES) {
        logWarn("[COIN] Maximum pulse count reached - closing gate");
        break;
      }
    }

    uint32_t now = millis();
    if (gotAny && (now - lastPulseMs >= COIN_END_IDLE_MS)) break;
    if (now - countingStart >= COIN_WINDOW_MS) break;
  }

  // Close the gate first so no new coins enter, then give an in-flight coin
  // (train up to ~0.6 s for P10) a closeout grace before detaching - a coin
  // must never be cut mid-train.
  digitalWrite(gatePin(), gateCloseLevel());
  logInfo("[COIN] Gate closed. Final pulse count: " + String(readPulses()));
  uint32_t graceStart = millis();
  while ((uint32_t)(millis() - graceStart) < 1200UL) {
    if (waitForIdleLine(COIN_BURST_GAP_MS)) break;
  }
  detachInterrupt(digitalPinToInterrupt(PIN_COIN));
  uint16_t pulses = readPulses();
  logInfo("[COIN] Counting stopped at " + String(pulses) + " pulse(s)");
  delay(GATE_CLOSE_SETTLE_MS);

  outPulses = pulses;

  if (pulses == 0) {
    err = "No coins detected within the time window";
    lastCycleText = "Error: " + err;
    cycleState = CYCLE_IDLE;
    return false;
  }

  // Per-coin pricing: bank the bursts closed during the cycle, then close
  // the trailing coin (line is idle now) - minutes = per-coin total.
  settleCoinBursts();
  uint32_t minutes = bankOpenBurst();
  if (minutes < 1) {
    err = "Pulse count " + String(pulses) + " did not match any rate";
    lastCycleText = "Error: " + err;
    cycleState = CYCLE_IDLE;
    return false;
  }

  outCoins = pulses;   // raw pulses (peso display)
  outMinutes = (uint16_t)minutes;

  cycleState = CYCLE_CREDITING;
  // Manual /coin endpoint has no page-supplied voucher - credit the
  // colon-stripped MAC as the hotspot username.
  String userName = mac;
  userName.replace(":", "");
  logInfo("[COIN] Crediting hotspot user " + userName + " with " + String(minutes) +
          " minute(s) via MikroTik API...");

  String macNC = mac;
  macNC.replace(":", "");
  macNC.toUpperCase();
  if (!performCredit(userName, (uint16_t)minutes, err, macNC)) {
    lastCycleText = "Error: " + err;
    pendingSave(userName, (uint16_t)minutes, macNC);   // coins taken - must not be lost
    cycleState = CYCLE_IDLE;
    return false;
  }

  lastCycleText = "Credited " + userName + " with " + String(minutes) + " minute(s)";
  cycleState = CYCLE_IDLE;
  return true;
}

// ---------------------------------------------------------------------------
// JuanFi-style asynchronous coin session (advanced from loop())
// ---------------------------------------------------------------------------
void finalizeCoinCount();

// Hotspot user credited by the current session: the page-supplied voucher
// (JuanFi style) or the colon-stripped MAC as a defensive fallback.
String sessionUserName() {
  String userName = activeVoucher;
  if (userName.length() == 0) {
    userName = activeMac;
    userName.replace(":", "");
  }
  return userName;
}

void startCoinSession(const String& mac) {
  activeMac = mac;
  finalPulses = 0;
  finalCoins = 0;
  finalMinutes = 0;
  sessionDone = false;
  successDelivered = false;
  finalizeByWindow = false;
  batchBasePulses = 0;
  batchPulses = 0;
  batchCoins = 0;
  batchMinutes = 0;
  lastBatchPulses = 0;
  lastPulseMs = 0;
  creditDeadline = 0;
  sessionStartMs = millis();
  sessionEndMs = 0;
  sessionSalesCoins = 0;
  sessionSalesMinutes = 0;

  detachInterrupt(digitalPinToInterrupt(PIN_COIN));
  noInterrupts();
  volatilePulses = 0;
  lastPulseUs = micros();
  lastHighUs = micros();
  interrupts();
  resetBurstState();

  digitalWrite(gatePin(), gateOpenLevel());
  cycleState = CYCLE_ARMING;
  gateSettleDeadline = millis() + GATE_OPEN_SETTLE_MS;
  logInfo("[COIN] Gate opened. Waiting for solenoid to settle...");
}

// PER-COIN SESSION CREDITING
// --------------------------
// Minutes are the SUM of per-coin values banked from finished pulse bursts
// (see coinBurstValue). The 1.5 s silence only decides WHEN to call the
// router, never WHAT a coin is worth:
//     target = burstBankMinutes (banked per burst) + trailing coin if quiet
//     batchMinutes = target - finalMinutes       (delta to deliver now)
// A trailing coin that is still mid-train when a window/cap fires is left
// open - endSession() finishes it during its closeout grace, and a delivery
// failure persists the full cumulative deficit, so nothing is ever lost.
void finalizeCoinCount() {
  // Batch boundary only: the gate stays OPEN and the pulse counter keeps
  // running so coins inserted while we credit are never lost.
  batchPulses = readPulses() - batchBasePulses;
  logInfo("[COIN] Batch ended. Pulse count: " + String(batchPulses));

  settleCoinBursts();               // bank every burst the ISR has closed
  bool lineQuiet = ((uint32_t)(millis() - lastPulseMs) >= COIN_BURST_GAP_MS);
  uint32_t target = burstBankMinutes;
  if (lineQuiet) target = bankOpenBurst();   // trailing coin is complete
  uint32_t delta = (target >= finalMinutes) ? (target - finalMinutes) : 0;

  batchCoins = batchPulses;          // batch pulses (sales bookkeeping)
  batchMinutes = (uint16_t)delta;
  logInfo("[COIN] Session value " + String(target) + " min (" +
          String(burstCoinCount) + " coin(s), " + String(readPulses()) +
          " pulses), delivered " + String(finalMinutes) + ", this batch adds " +
          String(batchMinutes) + " min");
  cycleState = CYCLE_CREDITING;
  creditDeadline = millis();
}

// Delivers the batch delta synchronously (blocking, a few hundred ms on the
// LAN). Only DELIVERED minutes join finalMinutes; a failed delivery persists
// the FULL cumulative deficit to the EEPROM recovery record so no coin is
// ever lost - and because the deficit only grows, a single-slot record can
// never overwrite an earlier failed batch out of existence.
void creditBatchNow() {
  String userName = sessionUserName();
  if (userName.length() == 0 || batchMinutes == 0) return;

  String err;
  logInfo("[COIN] Crediting hotspot user " + userName + " with " +
          String(batchMinutes) + " minute(s) via MikroTik API...");
  String macNC = activeMac;
  macNC.replace(":", "");
  macNC.toUpperCase();
  if (performCredit(userName, batchMinutes, err, macNC)) {
    uint32_t total = (uint32_t)finalMinutes + batchMinutes;
    if (total > MAX_CREDIT_MINUTES) total = MAX_CREDIT_MINUTES;
    finalMinutes = (uint16_t)total;
    finalCoins += batchCoins;
    sessionSalesCoins += batchCoins;
    sessionSalesMinutes += batchMinutes;
    lastCycleText = "Credited " + userName + " with " + String(batchMinutes) +
                    " minute(s) (" + String(finalMinutes) + " total this session)";
    // A cumulative credit has just covered any earlier failed batches -
    // clear the stale pending record so the retry loop never double-applies.
    if (pendingFindIndex(userName) >= 0) {
      logInfo("[PENDING] Earlier uncredited amount now covered by this credit - clearing recovery record");
      pendingClearFor(userName);
    }
  } else {
    lastCycleText = "Error: " + err;
    salesRecordFailure();
    remoteEnqueue(String("[ERROR] Credit ") + userName + " failed: " + err);
    settleCoinBursts();   // bank everything that closed while the API call ran
    uint32_t target = sessionValueEstimate();   // incl. a partial trailing coin
    uint32_t deficit = (target >= finalMinutes) ? (target - finalMinutes) : 0;
    if (deficit > 0) {
      // Coins taken - the FULL deficit must not be lost. Monotone, so a
      // later failure overwriting this record can only widen it.
      pendingSave(userName, (uint16_t)deficit, macNC);
    }
  }
}

// Re-arms the slot mid-session for the next batch. Pulses that arrived while
// we were crediting stay pending in the new batch (the base is advanced only
// by the batch just settled - whether it was delivered, is pending recovery,
// or added no new time - never past pulses whose value is still open). The
// window restarts so a customer inserting coins every few seconds is never
// cut off.
void rearmCounting() {
  settleCoinBursts();           // bank bursts that closed during the credit
  batchBasePulses += batchPulses;   // settle this batch
  batchPulses = 0;
  batchCoins = 0;
  batchMinutes = 0;
  finalPulses = readPulses();       // customer owns everything counted so far
  lastBatchPulses = readPulses() - batchBasePulses;
  lastPulseMs = millis();
  cycleState = CYCLE_COUNTING;
  countingStartMs = millis();
  if (lastBatchPulses > 0) {
    logInfo("[COIN] Slot re-armed with " + String(lastBatchPulses) + " pending pulse(s)");
  } else {
    logInfo("[COIN] Slot re-armed. Insert more coins or press Done...");
  }
}

// Ends the session: closes the gate (no new coins), lets an in-flight coin
// finish its train, banks all remaining per-coin value, lands any value that
// was counted but not yet delivered into the pending recovery record, and
// commits the session's sales counters with a single flash write.
void endSession() {
  digitalWrite(gatePin(), gateCloseLevel());   // stop new coins first
  // Closeout grace: a coin that was dropping while we settled must not be
  // cut mid-train (a P10 train lasts ~0.6 s), or its value would be lost.
  // R6: when NOTHING has been counted yet (Done pressed while the coin is
  // still travelling through the acceptor), never take the early idle-line
  // exit - wait the full grace so the arriving pulses are still counted.
  bool noPulsesAtGrace = (readPulses() == 0);
  uint32_t graceStart = millis();
  while ((uint32_t)(millis() - graceStart) < 1200UL) {
    if (!noPulsesAtGrace && waitForIdleLine(COIN_BURST_GAP_MS)) break;
    delay(10);
  }
  detachInterrupt(digitalPinToInterrupt(PIN_COIN));
  finalPulses = readPulses();   // freeze: the customer owns everything counted
  sessionEndMs = millis();

  settleCoinBursts();           // bank bursts closed during the credit burst
  uint32_t target = bankOpenBurst();   // trailing coin is done - bank it

  String userName = sessionUserName();
  uint32_t deficit = (target >= finalMinutes) ? (target - finalMinutes) : 0;
  if (deficit > 0) {
    // R6: deliver the closeout value NOW when possible, so a "Done" press a
    // moment after the coin still awards the time inside the same request
    // (previously it only went to the recovery queue - the popup answered
    // "coins.wait.expired" and the time arrived with a confusing delay).
    uint16_t deficitMin = (deficit > MAX_CREDIT_MINUTES) ? MAX_CREDIT_MINUTES
                                                         : (uint16_t)deficit;
    String err;
    String macNC = activeMac;
    macNC.replace(":", "");
    macNC.toUpperCase();
    bool delivered = (userName.length() > 0) &&
                     performCredit(userName, deficitMin, err, macNC);
    if (delivered) {
      uint32_t total = (uint32_t)finalMinutes + deficitMin;
      if (total > MAX_CREDIT_MINUTES) total = MAX_CREDIT_MINUTES;
      finalMinutes = (uint16_t)total;
      sessionSalesCoins  += (finalPulses >= finalCoins) ? (finalPulses - finalCoins) : 0;
      sessionSalesMinutes += deficitMin;
      finalCoins = finalPulses;
      pendingClearFor(userName);
      logInfo("[COIN] Closeout " + String(deficitMin) + " minute(s) delivered on session end");
      deficit = 0;   // delivered now - nothing is pending
    } else {
      // Uncovered value: persist the full deficit. The deficit is monotone
      // per customer, so a re-save can only widen that customer's record.
      if (userName.length() > 0) {
        pendingSave(userName, (uint16_t)deficit, macNC);
      }
    }
  } else {
    // Everything is delivered - drop this customer's stale recovery record.
    pendingClearFor(userName);
  }

  if (sessionSalesCoins > 0 || sessionSalesMinutes > 0) {
    salesRecordSale(sessionSalesCoins, sessionSalesMinutes);   // one commit
    reportSale((uint16_t)sessionSalesCoins, (uint16_t)sessionSalesMinutes, userName, activeMac);
    sessionSalesCoins = 0;
    sessionSalesMinutes = 0;
  }

  batchPulses = 0;
  batchCoins = 0;
  batchMinutes = 0;
  lastBatchPulses = 0;
  sessionDone = true;
  cycleState = CYCLE_IDLE;
  if (finalMinutes > 0) {
    lastCycleText = "Credited " + String(finalMinutes) + " minute(s) total this session";
    if (deficit > 0) lastCycleText += " (+" + String(deficit) + " min pending recovery)";
  } else if (deficit > 0) {
    lastCycleText = String(deficit) + " minute(s) pending recovery for " +
                    String(finalPulses) + " pulse(s)";
  } else if (finalPulses > 0) {
    lastCycleText = "Error: Pulse count " + String(finalPulses) + " did not match any rate";
  } else {
    lastCycleText = "Error: No coins detected within the time window";
  }
  logInfo("[COIN] Gate closed. " + lastCycleText);
}

void tickCoinSession() {
  settleCoinBursts();   // bank coin bursts as they finish (priced per coin)
  switch (cycleState) {
    case CYCLE_ARMING:
      if ((int32_t)(millis() - gateSettleDeadline) < 0) return;
      if (!waitForIdleLine(COIN_ARM_IDLE_MS)) {
        logWarn("[COIN] Coin line not idle at arming - noise rejected by CHANGE debounce");
      }
      attachInterrupt(digitalPinToInterrupt(PIN_COIN), coinIsr, CHANGE);
      cycleState = CYCLE_COUNTING;
      countingStartMs = millis();
      lastPulseMs = millis();
      lastBatchPulses = 0;
      logInfo("[COIN] Coin slot armed. Insert coins now...");
      break;

    case CYCLE_COUNTING: {
      uint16_t batchNow = readPulses() - batchBasePulses;
      uint32_t now = millis();
      if (batchNow != lastBatchPulses) {
        lastBatchPulses = batchNow;
        lastPulseMs = now;
        logInfo("[COIN] Pulse detected - batch total: " + String(batchNow));
      }

      // Session-level bounds: the 60-pulse cap and the hard time cap close
      // the session (after settling whatever is pending), never silently.
      bool endAfterCredit = false;
      if (readPulses() >= MAX_PULSES) {
        logWarn("[COIN] Maximum pulse count reached for this session");
        endAfterCredit = true;
      } else if ((uint32_t)(now - sessionStartMs) >= SESSION_HARD_CAP_MS) {
        logWarn("[COIN] Session hard time cap reached");
        endAfterCredit = true;
      }

      if (endAfterCredit) {
        if (lastBatchPulses == 0 && finalMinutes == 0 && readPulses() == 0) {
          endSession();                       // nothing inserted - plain close
        } else {
          finalizeByWindow = true;            // credit pending batch, then end
          finalizeCoinCount();
        }
      } else if (lastBatchPulses > 0 && (now - lastPulseMs >= COIN_END_IDLE_MS)) {
        // Short silence: credit what we have and re-arm for more coins.
        finalizeCoinCount();
      } else if ((uint32_t)(now - countingStartMs) >= COIN_WINDOW_MS) {
        // Window over: credit anything pending, then end the session.
        finalizeByWindow = true;
        finalizeCoinCount();
      }
      break;
    }

    case CYCLE_CREDITING:
      if ((int32_t)(millis() - creditDeadline) < 0) return;
      if (batchMinutes > 0) {
        creditBatchNow();
      } else if (batchPulses > 0) {
        // Batch added no new time (its pulses are covered by the session
        // total already, or carry value for future pulses). Nothing to
        // deliver - just settle.
        logInfo("[COIN] Batch pulses already covered by the session total - nothing additional to credit");
      }
      if (finalizeByWindow) {
        endSession();
      } else {
        rearmCounting();
      }
      break;

    default:
      break;
  }
}

void sendCycleError(const String& msg) {
  String j = "{\"ok\":false,\"state\":\"error\",\"error\":\"" + jsonEscape(msg) + "\"}";
  server.send(200, "application/json", j);
}

void handleCoin() {
  if (portalMode) {
    sendCycleError("Setup portal mode - coin slot is offline");
    return;
  }

  String macRaw = server.arg("mac");
  if (macRaw.length() == 0) macRaw = server.arg("plain");
  macRaw = urlDecode(macRaw);

  String mac;
  if (!normalizeMac(macRaw, mac)) {
    sendCycleError("Invalid or missing MAC address - use /coin?mac=AA:BB:CC:DD:EE:FF");
    return;
  }

  // Simple per-client throttle: the same source may only start one coin
  // session every few seconds. Stops a stray script or prankster from
  // holding the physical coinslot busy (the slot is one shared resource).
  static IPAddress lastStarterIp;
  static uint32_t  lastStartMs = 0;
  IPAddress src = server.client().remoteIP();
  if (src == lastStarterIp && (uint32_t)(millis() - lastStartMs) < 5000UL) {
    sendCycleError("Coin cycle already in progress");
    return;
  }
  lastStarterIp = src;
  lastStartMs = millis();

  if (cycleState != CYCLE_IDLE) {
    sendCycleError("Coin cycle already in progress");
    return;
  }
  if (pendingCount() >= PENDING_SLOTS) {
    sendCycleError("Coin slot closed - earlier purchases are still pending recovery");
    return;
  }

  uint16_t pulses = 0, coins = 0, minutes = 0;
  String err;
  if (runCoinCycle(mac, pulses, coins, minutes, err)) {
    String j = "{\"ok\":true,\"state\":\"credited\",\"mac\":\"" + mac +
               "\",\"pulses\":" + String(pulses) +
               ",\"coins\":" + String(coins) +
               ",\"minutes\":" + String(minutes) + "}";
    server.send(200, "application/json", j);
    logInfo("[CREDIT] " + mac + " credited with " + String(minutes) + " minute(s)");
    salesRecordSale(coins, minutes);
    String coinUser = mac;
    coinUser.replace(":", "");
    reportSale(coins, minutes, coinUser, mac);
    remoteEnqueue(String("[INFO] Credit ") + mac + " " + String(minutes) + "min");
  } else {
    sendCycleError(err);
    logErr("[CREDIT] " + mac + " failed: " + err);
    salesRecordFailure();
    remoteEnqueue(String("[ERROR] Credit ") + mac + " failed: " + err);
  }
}

void handlePing() {
  server.send(200, "application/json", "{\"ok\":true}");
}

// FIX A: content source for the router's /tool fetch. The bridge asks the
// router to download hotspot/data/<MAC>.txt from here (RouterOS 6 has no
// /file/add). Only well-formed input is echoed - m = 12 hex digits,
// v = "P" + 5 digits - so nobody can use this endpoint to plant arbitrary
// files (the fetch itself runs on the router with the API credentials).
void handleVoucherData() {
  setupCors();
  String m = urlDecode(server.arg("m"));
  String v = urlDecode(server.arg("v"));
  v.trim();
  String macNorm;
  bool macOk = normalizeMac(m, macNorm);
  bool voucherOk = v.length() == 6 && v.charAt(0) == 'P';
  if (voucherOk) {
    for (uint8_t i = 1; i < 6; i++) {
      if (!isdigit((unsigned char)v.charAt(i))) { voucherOk = false; break; }
    }
  }
  if (!macOk || !voucherOk) {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  server.send(200, "text/plain", v + "#");
}

void handleStats() {
  String j = "{\"ok\":true,\"rev\":\"" FW_REVISION "\",\"device\":\"" + jsonEscape(String(config.deviceId)) +
             "\",\"boots\":" + String(sales.bootCount) +
             ",\"totalCycles\":" + String(sales.totalCycles) +
             ",\"totalCoins\":" + String(sales.totalCoins) +
             ",\"totalMinutes\":" + String(sales.totalMinutes) +
             ",\"failedCycles\":" + String(sales.failedCycles) +
             ",\"daily\":[";
  for (uint8_t i = 0; i < SALES_DAYS; i++) {
    if (i) j += ',';
    j += "{\"coins\":" + String(sales.dayCoins[i]) +
         ",\"minutes\":" + String(sales.dayMinutes[i]) +
         ",\"cycles\":" + String(sales.dayCycles[i]) + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleRates() {
  String j = "{\"ok\":true,\"promo\":" + String(promo.enabled == 1 ? "true" : "false") +
             ",\"rates\":[";
  if (promo.enabled == 1) {
    for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
      if (i) j += ',';
      j += "{\"price\":" + String(promo.price[i]) +
           ",\"minutes\":" + String(promo.minutes[i]) + "}";
    }
  } else {
    j += "{\"price\":" + String(config.pulsesPerCoin) +
         ",\"minutes\":" + String(config.minutesPerCoin) + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleInsert() {
  if (portalMode) {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  String mac = urlDecode(server.arg("mac"));
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>Insert Coin</title></head>"
             "<body style='font-family:sans-serif;padding:16px'>"
             "<h1>Insert Coin</h1>"
             "<p>MAC address:</p>"
             "<input id='mac' value='" + mac + "' placeholder='AA:BB:CC:DD:EE:FF' style='padding:10px;width:100%'>"
             "<button id='go' onclick='start()' style='margin-top:12px;padding:14px;width:100%;font-size:18px'>Insert Coin</button>"
             "<p id='status'>Press Insert Coin, then drop your coins.</p>"
             "<script>"
             "function start(){"
             "var m=document.getElementById('mac').value.trim();"
             "if(!m){document.getElementById('status').innerText='Enter the MAC address first.';return;}"
             "document.getElementById('status').innerText='Waiting for coins... insert 1, 5, or 10 peso coins now.';"
             "document.getElementById('go').disabled=true;"
             "fetch('/coin?mac='+encodeURIComponent(m)).then(function(r){return r.json();}).then(function(j){"
             "document.getElementById('go').disabled=false;"
             "document.getElementById('status').innerText=JSON.stringify(j);"
             "}).catch(function(e){document.getElementById('go').disabled=false;"
             "document.getElementById('status').innerText='Error: '+e;});"
             "}"
             "</script></body></html>";
  server.send(200, "text/html", h);
}

// ---------------------------------------------------------------------------
// JuanFi-compatible API (so the router-hosted hotspot page at 10.0.0.1 works)
// ---------------------------------------------------------------------------
void setupCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

void sendJuanfiError(const char* code) {
  server.send(200, "application/json",
              String("{\"status\":\"false\",\"errorCode\":\"") + code + "\"}");
}

// GET /getRates -> JuanFi text format: label#price#minutes#validity#data
void handleGetRatesJuanfi() {
  setupCors();
  String out = "";
  if (promo.enabled == 1) {
    for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
      if (promo.price[i] > 0 && promo.minutes[i] > 0) {
        if (out.length() > 0) out += '|';
        out += "P" + String(promo.price[i]) + " / " + String(promo.minutes[i]) + " min";
        out += "#" + String(promo.price[i]);
        out += "#" + String(promo.minutes[i]);
        out += "#" + String(promo.minutes[i]);
        out += "#";
      }
    }
  } else {
    out += "Coin / " + String(config.minutesPerCoin) + " min";
    out += "#" + String(config.pulsesPerCoin);
    out += "#" + String(config.minutesPerCoin);
    out += "#" + String(config.minutesPerCoin);
    out += "#";
  }
  server.send(200, "text/plain", out);
}

void handleTopUp() {
  setupCors();
  if (portalMode) { sendJuanfiError("coin.slot.notavailable"); return; }

  String macRaw = urlDecode(server.arg("mac"));
  String mac;
  if (!normalizeMac(macRaw, mac)) { sendJuanfiError("coin.slot.banned"); return; }

  // Per-client throttle (see handleCoin) - protects the shared coinslot.
  IPAddress src = server.client().remoteIP();
  if (src == lastTopUpStarterIp && (uint32_t)(millis() - lastTopUpStartMs) < 5000UL) {
    sendJuanfiError("coinslot.busy");
    return;
  }
  lastTopUpStarterIp = src;
  lastTopUpStartMs = millis();

  if (cycleState != CYCLE_IDLE) { sendJuanfiError("coinslot.busy"); return; }

  // R7.1: never take a coin we cannot record. If the recovery store is full,
  // several earlier purchases are still uncredited (router/API outage) - the
  // slot stays closed until the retry loop drains the backlog.
  if (pendingCount() >= PENDING_SLOTS) {
    reportEvent("coinslot_blocked_pending", 1,
                "Coin slot refused: recovery store full (" + String(pendingCount()) + ")",
                "", "", 0, 0);
    sendJuanfiError("coin.slot.notavailable");
    return;
  }

  String voucher = urlDecode(server.arg("voucher"));
  voucher.trim();   // hidden spaces from phone keyboards must not make ghost users
  if (voucher.length() == 0) {
    // Codeless generation, JuanFi-style: "P" + 5 random digits. The code is
    // checked against the router's hotspot user list so an existing customer
    // voucher is never accidentally extended; if the API is unreachable we
    // fail open so the coinslot keeps working.
    for (uint8_t tries = 0; tries < 5 && voucher.length() == 0; tries++) {
      String candidate = generateVoucherCode();
      bool known = false;
      if (hotspotUserExists(candidate, known)) {
        if (!known) voucher = candidate;   // verified fresh
      } else {
        voucher = candidate;               // router unreachable - fail open
      }
    }
    if (voucher.length() == 0) voucher = generateVoucherCode();
  }

  activeMac = mac;
  activeVoucher = voucher;
  lastCycleText = "Waiting for coins (" + mac + ")";
  startCoinSession(mac);

  server.send(200, "application/json",
              "{\"status\":\"true\",\"voucher\":\"" + jsonEscape(voucher) + "\"}");
}

// The page polls this every second. We answer immediately with the current
// session state; the actual coin counting runs in tickCoinSession()/loop().
void handleCheckCoin() {
  setupCors();
  if (portalMode) { sendJuanfiError("coin.slot.notavailable"); return; }

  String voucher = urlDecode(server.arg("voucher"));
  voucher.trim();
  if (activeVoucher.length() == 0 || voucher != activeVoucher) {
    sendJuanfiError("coinslot.busy");
    return;
  }

  if (cycleState == CYCLE_CREDITING) {
    server.send(200, "application/json",
                "{\"status\":\"false\",\"errorCode\":\"coin.is.reading\"}");
    return;
  }

  if (cycleState == CYCLE_ARMING || cycleState == CYCLE_COUNTING) {
    uint32_t now = millis();
    uint32_t elapsed = now - countingStartMs;   // rollover-safe elapsed math
    uint32_t remain = (cycleState == CYCLE_ARMING || elapsed >= COIN_WINDOW_MS)
                      ? COIN_WINDOW_MS
                      : (COIN_WINDOW_MS - elapsed);
    // Live totals: credited batches plus the pending (uncommitted) batch.
    // The page updates the popup time from timeAdded without any reload.
    server.send(200, "application/json",
                "{\"status\":\"false\",\"errorCode\":\"coin.not.inserted\","
                "\"remainTime\":" + String(remain) +
                ",\"timeAdded\":" + String((uint32_t)finalMinutes * 60) +
                ",\"totalCoin\":" + String(finalPulses + lastBatchPulses) +
                ",\"waitTime\":" + String(COIN_WINDOW_MS) +
                ",\"validity\":" + String(finalMinutes) + ",\"data\":0}");
    return;
  }

  // CYCLE_IDLE
  if (sessionDone) {
    if (finalMinutes > 0) {
      if (!successDelivered) {
        // Report the finished session exactly once so the page shows a
        // single "coin inserted" notification.
        successDelivered = true;
        server.send(200, "application/json",
                    "{\"status\":\"true\",\"newCoin\":" + String(finalPulses) +
                    ",\"timeAdded\":" + String((uint32_t)finalMinutes * 60) +
                    ",\"totalCoin\":" + String(finalPulses) +
                    ",\"validity\":" + String(finalMinutes) + ",\"data\":0}");
      } else {
        // Already reported. Answer in the page's "expired with coins
        // processed" shape (remainTime 0 + totalCoin > 0): it stops the
        // success toasts and moves the flow to auto-login.
        server.send(200, "application/json",
                    "{\"status\":\"false\",\"errorCode\":\"coin.not.inserted\",\"remainTime\":0,"
                    "\"timeAdded\":" + String((uint32_t)finalMinutes * 60) +
                    ",\"totalCoin\":" + String(finalPulses) +
                    ",\"waitTime\":" + String(COIN_WINDOW_MS) +
                    ",\"validity\":" + String(finalMinutes) + ",\"data\":0}");
      }
      return;
    } else {
      if (finalPulses == 0) sendJuanfiError("coins.wait.expired");
      else sendJuanfiError("coin.slot.notavailable");
    }
    return;
  }

  sendJuanfiError("coin.not.inserted");
}

void handleUseVoucher() {
  setupCors();
  String voucher = urlDecode(server.arg("voucher"));
  voucher.trim();
  if (activeVoucher.length() == 0 || voucher != activeVoucher) {
    sendJuanfiError("coinslot.busy");
    return;
  }
  if (!sessionDone) {
    // The customer clicked Done while the bridge was still counting.
    // Finalize + credit the pending batch immediately so coins already
    // inserted are credited instead of answering "coin.not.inserted".
    if (cycleState == CYCLE_ARMING || cycleState == CYCLE_COUNTING) {
      finalizeByWindow = true;   // this click ends the session
      finalizeCoinCount();
    }
    if (cycleState == CYCLE_CREDITING) {
      if (batchMinutes > 0) creditBatchNow();   // endSession() below settles
      endSession();
    }
    if (!sessionDone) { sendJuanfiError("coin.not.inserted"); return; }
  }
  if (finalPulses == 0) { sendJuanfiError("coin.not.inserted"); return; }
  if (finalMinutes == 0) { sendJuanfiError("coins.wait.expired"); return; }

  uint16_t p = finalPulses;
  uint16_t m = finalMinutes;
  activeMac = "";
  activeVoucher = "";
  sessionDone = false;
  successDelivered = false;

  server.send(200, "application/json",
              "{\"status\":\"true\",\"totalCoin\":" + String(p) +
              ",\"timeAdded\":" + String((uint32_t)m * 60) +
              ",\"validity\":" + String(m) + "}");
}

void handleCancelTopUp() {
  setupCors();
  // Cancel = the customer closed the "insert coin" popup. Only honored while
  // the slot is still waiting and NO coin has dropped yet - coins already
  // inserted belong to the customer and must finish crediting.
  if (cycleState == CYCLE_ARMING || cycleState == CYCLE_COUNTING) {
    if ((readPulses() - batchBasePulses) > 0) {
      // A coin already fell into the pending batch: keep the session alive
      // so it credits. (Coins from earlier batches are already credited.)
      sendJuanfiError("coinslot.busy");
      return;
    }
    // Abort the open slot: detach the counter, close the gate, release the
    // session so the next "insert coin" press does not hit "coin slot busy".
    detachInterrupt(digitalPinToInterrupt(PIN_COIN));
    digitalWrite(gatePin(), gateCloseLevel());
    finalPulses = 0;
    finalCoins = 0;
    finalMinutes = 0;
    batchBasePulses = 0;
    batchPulses = 0;
    batchCoins = 0;
    batchMinutes = 0;
    lastBatchPulses = 0;
    sessionStartMs = 0;
    sessionSalesCoins = 0;
    sessionSalesMinutes = 0;
    resetBurstState();
    cycleState = CYCLE_IDLE;
    lastCycleText = "Coin session cancelled by customer - slot released";
    logInfo("[COIN] Session cancelled - gate closed, slot released");
    // Release the throttle so an immediate restart attempt succeeds.
    lastTopUpStartMs = 0;
  } else if (cycleState == CYCLE_CREDITING) {
    // Coins are being credited right now - nothing to cancel.
    sendJuanfiError("coinslot.busy");
    return;
  }
  activeMac = "";
  activeVoucher = "";
  sessionDone = false;
  successDelivered = false;
  server.send(200, "application/json", "{\"status\":\"true\"}");
}

// ---------------------------------------------------------------------------
// Diagnostic: dump the router's hotspot user list so we can see exactly what
// the ESP32 sees (limit-uptime, profile, uptime, etc.). Handy for debugging
// "wrong time on status page" issues.
// ---------------------------------------------------------------------------
void handleDebugUsers() {
  setupCors();
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\":\"WiFi disconnected\"}");
    return;
  }
  IPAddress gw;
  if (!parseIPv4(config.gatewayIp, gw)) {
    server.send(500, "application/json", "{\"error\":\"bad gateway\"}");
    return;
  }
  RouterOSApi api;
  if (!api.connect(gw, config.apiPort)) {
    server.send(500, "application/json", "{\"error\":\"connect failed: " + api.lastError + "\"}");
    return;
  }
  if (!api.login(config.apiUser, config.apiPass)) {
    server.send(500, "application/json", "{\"error\":\"login failed: " + api.lastError + "\"}");
    return;
  }
  // Query ALL hotspot users with the fields we care about
  const char* words[] = {"/ip/hotspot/user/print",
                         "=.proplist=.id,name,limit-uptime,uptime,profile,comment,disabled",
                         ""};
  String result = "{\"ok\":true,\"users\":[";
  if (api.sendSentence(words, 3)) {
    bool first = true;
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < 5000UL) {
      String sentence;
      if (!api.readSentence(sentence, 5000UL - (millis() - start))) break;
      String fw = api.firstWord(sentence);
      if (fw == "!done") break;
      if (fw == "!re") {
        if (!first) result += ",";
        first = false;
        String id = api.extractPair(sentence, ".id");
        String name = api.extractPair(sentence, "name");
        String limit = api.extractPair(sentence, "limit-uptime");
        String uptime = api.extractPair(sentence, "uptime");
        String profile = api.extractPair(sentence, "profile");
        String comment = api.extractPair(sentence, "comment");
        String disabled = api.extractPair(sentence, "disabled");
        result += "{\"id\":\"" + id + "\",\"name\":\"" + name + "\",";
        result += "\"limit-uptime\":\"" + limit + "\",\"uptime\":\"" + uptime + "\",";
        result += "\"profile\":\"" + profile + "\",\"comment\":\"" + comment + "\",";
        result += "\"disabled\":\"" + disabled + "\"}";
      }
    }
  }
  result += "]}";
  api.disconnect();
  server.send(200, "application/json", result);
}

// ---------------------------------------------------------------------------
// Admin: in-RAM log ring (field diagnostics without a serial cable)
// ---------------------------------------------------------------------------
void handleLog() {
  String key = urlDecode(server.arg("key"));
  if (key != String(config.apiPass)) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  String j = "{\"ok\":true,\"lines\":[";
  for (uint8_t i = 0; i < logRingCount; i++) {
    uint8_t idx = (logRingHead + i) % LOG_RING_MAX;
    if (i) j += ',';
    j += "\"" + jsonEscape(String(logRing[idx])) + "\"";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// ---------------------------------------------------------------------------
// Admin: live promo rate table update WITHOUT a reboot. Lets an operator fix
// a missing/zeroed tier (e.g. re-enable P20 = 300 min) or tune rates on a
// deployed unit. Only the price/minute pairs change; gate pin, polarity and
// TX power are untouched. Use:
//   GET /setRates?key=<api password>&price0=1&minutes0=10&price1=5&minutes1=60
//       &price2=10&minutes2=130&price3=20&minutes3=300
// priceN=0 disables tier N+1.
// ---------------------------------------------------------------------------
void handleSetRates() {
  String key = urlDecode(server.arg("key"));
  if (key != String(config.apiPass)) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  if (cycleState != CYCLE_IDLE) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"coin session in progress\"}");
    return;
  }
  for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
    int pr = urlDecode(server.arg("price" + String(i))).toInt();
    int mn = urlDecode(server.arg("minutes" + String(i))).toInt();
    if (pr < 0 || pr > 10000 || mn < 0 || mn > 10080) {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"slot " + String(i + 1) +
                  " invalid (price 0-10000, minutes 0-10080)\"}");
      return;
    }
    promo.price[i]   = (uint16_t)pr;
    promo.minutes[i] = (uint16_t)mn;
  }
  promo.enabled = 1;
  promoSave();
  logInfo("[CONFIG] Promo rates updated over HTTP");
  String j = "{\"ok\":true,\"rates\":[";
  for (uint8_t i = 0; i < PROMO_SLOTS; i++) {
    if (i) j += ',';
    j += "{\"price\":" + String(promo.price[i]) +
         ",\"minutes\":" + String(promo.minutes[i]) + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// ---------------------------------------------------------------------------
// Admin: clean remote reboot (used by a MikroTik scheduler for nightly
// maintenance restarts on 24/7 units)
// ---------------------------------------------------------------------------
void handleReboot() {
  String key = urlDecode(server.arg("key"));
  if (key != String(config.apiPass)) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  logWarn("[SYSTEM] Remote reboot requested - restarting in 1 second");
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  server.client().flush();
  delay(1000);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Solenoid/gate debug tools (admin only)
// ---------------------------------------------------------------------------
void handleGateTest() {
  String key = urlDecode(server.arg("key"));
  if (key != String(config.apiPass)) {
    server.send(401, "text/plain", "unauthorized");
    return;
  }
  int pin = server.arg("pin").toInt();
  int level = server.arg("level").toInt();
  if (pin < 0 || pin > 16 || (level != 0 && level != 1)) {
    server.send(400, "text/plain", "bad pin/level");
    return;
  }
  if (pin == 15) {
    server.send(400, "text/plain", "GPIO15 (D8) is a boot-strap pin - leaving it HIGH prevents boot. Skipped.");
    return;
  }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, level ? HIGH : LOW);
  server.send(200, "application/json",
              "{\"ok\":true,\"pin\":" + String(pin) + ",\"level\":" + String(level) + "}");
}

void handleGateTestPage() {
  String key = urlDecode(server.arg("key"));
  if (key.length() == 0) {
    String h = "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:16px'>"
               "<h1>Gate Test</h1><p>Admin key:</p>"
               "<form method='get' action='/gatetest'><input name='key' type='password'>"
               "<button>Open</button></form></body></html>";
    server.send(200, "text/html", h);
    return;
  }
  if (key != String(config.apiPass)) {
    server.send(401, "text/plain", "wrong key");
    return;
  }
  String h = "<!DOCTYPE html><html><body style='font-family:sans-serif;padding:16px'>"
             "<h1>Gate Test</h1>"
             "<p>Tap Set HIGH / Set LOW and watch the coin slot solenoid. "
             "When you find the pin that OPENS the solenoid, put that GPIO number "
             "in Settings -> Gate GPIO pin (and tick Invert if it opens on LOW).</p>";
  int pins[] = {16, 5, 4, 14, 12, 13};
  const char* names[] = {"D0", "D1", "D2", "D5", "D6", "D7"};
  for (uint8_t i = 0; i < 6; i++) {
    h += "<p><b>GPIO" + String(pins[i]) + " (" + names[i] + ")</b> ";
    h += "<a href='/gate?key=" + key + "&pin=" + String(pins[i]) + "&level=1'>Set HIGH</a> | ";
    h += "<a href='/gate?key=" + key + "&pin=" + String(pins[i]) + "&level=0'>Set LOW</a></p>";
  }
  h += "</body></html>";
  server.send(200, "text/html", h);
}

// ---------------------------------------------------------------------------
// Wi-Fi watchdog and memory watchdog
// ---------------------------------------------------------------------------
bool gatewayTcpCheck() {
  if (config.configured != 1 || portalMode) return true;
  IPAddress gw;
  if (!gw.fromString(config.gatewayIp)) return true;   // no valid gateway to test
  WiFiClient c;
  c.setTimeout(3000);
  if (!c.connect(gw, config.apiPort)) return false;
  c.stop();
  return true;
}

void watchWifi() {
  if (portalMode) return;
  uint32_t now = millis();
  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    if (wifiLostLogged) {
      wifiLostLogged = false;
      wifiDownSince = 0;
      lastWifiHardResetMs = 0;
      gatewayFailCount = 0;
      logInfo("[NETWORK] Reconnected to hAP Lite. IP: " + WiFi.localIP().toString());
      reportEvent("wifi_restored", 0, "Reconnected at " + WiFi.localIP().toString(), "", "", 0, 0);
    }

    // A station can stay "connected" while the link is actually dead.
    // Periodically prove we can reach the router API port.
    if (cycleState == CYCLE_IDLE && now - lastGatewayCheckMs >= GATEWAY_CHECK_INTERVAL_MS) {
      lastGatewayCheckMs = now;
      if (gatewayTcpCheck()) {
        gatewayFailCount = 0;
      } else {
        gatewayFailCount++;
        logWarn("[NETWORK] Router " + String(config.gatewayIp) + ":" +
                String(config.apiPort) + " unreachable (" +
                String(gatewayFailCount) + "/3)");
        if (gatewayFailCount >= 3) {
          gatewayFailCount = 0;
          logErr("[NETWORK ERROR] Link appears dead - forcing a Wi-Fi restart");
          WiFi.disconnect(true);
          delay(100);
          WiFi.begin(config.ssid, config.pass);
          wifiDownSince = now;
          lastWifiHardResetMs = now;
        }
      }
    }
    return;
  }

  if (!wifiLostLogged) {
    wifiLostLogged = true;
    wifiDownSince = now;
    lastWifiHardResetMs = now;
    logErr("[NETWORK ERROR] Disconnected from hAP Lite SSID. Status: " +
           wifiStatusText(st) + ". Attempting recovery...");
    reportEvent("wifi_lost", 1, "Disconnected: " + wifiStatusText(st), "", "", 0, 0);
  }

  if (now - lastWifiCheckMs >= WIFI_RECONNECT_INTERVAL_MS) {
    lastWifiCheckMs = now;
    logWarn("[NETWORK] Retrying Wi-Fi connection...");
    WiFi.reconnect();
  }

  // R7: never reset the radio or reboot while a coin session is open or a
  // credit is still queued for recovery - a reboot there can lose coins.
  bool safeToRecover = (cycleState == CYCLE_IDLE) && !pendingAny();
  if (safeToRecover &&
      now - wifiDownSince >= WIFI_HARD_RESET_AFTER_MS &&
      now - lastWifiHardResetMs >= WIFI_HARD_RESET_AFTER_MS) {
    lastWifiHardResetMs = now;
    logErr("[NETWORK ERROR] Still offline after 60s - re-initializing the Wi-Fi stack");
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(config.ssid, config.pass);
  } else if (!safeToRecover && now - wifiDownSince >= WIFI_HARD_RESET_AFTER_MS) {
    logWarn("[NETWORK] Offline but a coin session/credit is active - deferring radio reset");
  }

  if (safeToRecover && now - wifiDownSince >= WIFI_RESTART_AFTER_MS) {
    logErr("[NETWORK ERROR] Still offline after 10 min - rebooting the ESP");
    delay(100);
    ESP.restart();
  }
}

// R5 - BOOT-ORDER SELF-HEAL: while the setup portal is up (e.g. the ESP
// booted before the hAP Lite finished booting, so the 3x15s boot join
// window expired), keep retrying the configured SSID and leave the portal
// automatically as soon as the router is reachable. Without this, the unit
// stayed in setup mode FOREVER - 10.0.0.5 unreachable, coinslot busy -
// until someone power-cycled it (the old "router first, ESP second" rule).
void watchPortalJoin() {
  if (config.configured != 1) return;          // nothing to join yet
  uint32_t now = millis();
  if (now - lastPortalJoinMs < PORTAL_JOIN_RETRY_MS) return;
  lastPortalJoinMs = now;

  logInfo("[NETWORK] Setup portal is active - retrying SSID \"" +
          String(config.ssid) + "\"");
  WiFi.mode(WIFI_AP_STA);   // keep the 192.168.4.1 setup portal reachable
  WiFi.begin(config.ssid, config.pass);

  uint32_t start = millis();
  while (millis() - start < PORTAL_JOIN_WAIT_MS && WiFi.status() != WL_CONNECTED) {
    delay(100);
    yield();
  }

  if (WiFi.status() != WL_CONNECTED) {
    logWarn("[NETWORK] hAP Lite still unreachable - setup portal stays up, retrying every 30s");
    return;
  }

  // Router is back: adopt it and serve the coin endpoints again.
  logInfo("[NETWORK] Reconnected to hAP Lite. IP: " + WiFi.localIP().toString());
  dnsServer.stop();
  WiFi.softAPdisconnect(true);            // drop the setup hotspot
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  portalMode = false;
  wifiLostLogged = false;
  wifiDownSince = 0;
  lastWifiHardResetMs = 0;
  gatewayFailCount = 0;
  logInfo("[NETWORK] Coin endpoint ready at http://" + WiFi.localIP().toString() +
          "/coin?mac=AA:BB:CC:DD:EE:FF");
}


void heapWatchdog() {
  uint32_t now = millis();
  if (now - lastHeapLogMs < HEAP_LOG_INTERVAL_MS) return;
  lastHeapLogMs = now;

  uint32_t free = ESP.getFreeHeap();
  String mem = "[MEMORY] Free heap: " + String(free) + " bytes";
  if (!portalMode && WiFi.status() == WL_CONNECTED) {
    mem += " | RSSI: " + String(WiFi.RSSI()) + " dBm";
  }
  logInfo(mem);

  if (free < HEAP_WARN_BYTES) {
    logWarn("[MEMORY] Low heap - consider rebooting the bridge");
  }
  if (free < HEAP_CRITICAL_BYTES) {
    logErr("[MEMORY] Heap critically low - restarting to recover");
    delay(100);
    ESP.restart();
  }
}

// NTP is configured once at boot (startStation); if that first attempt
// failed (router still starting, DNS hiccup), retry quietly every 5 minutes
// until the clock is valid. One time() call per interval - negligible cost
// once synced, and the nightly restart + daily sales buckets keep working.
uint32_t lastNtpRetryMs = 0;
void tickNtp() {
  if (portalMode || config.configured != 1) return;
  if (time(nullptr) > 1000000000L) return;   // synced - nothing to do
  uint32_t now = millis();
  if ((uint32_t)(now - lastNtpRetryMs) < NTP_RETRY_INTERVAL_MS) return;
  lastNtpRetryMs = now;
  logWarn("[NTP] Clock not synced - retrying NTP...");
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
}

// ---------------------------------------------------------------------------
// HTTP route table
// ---------------------------------------------------------------------------
void registerRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/coin", HTTP_GET, handleCoin);
  server.on("/coin", HTTP_POST, handleCoin);
  server.on("/ping", HTTP_GET, handlePing);
  server.on("/voucherdata", HTTP_GET, handleVoucherData);
  server.on("/stats", HTTP_GET, handleStats);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/insert", HTTP_GET, handleInsert);
  server.on("/rates", HTTP_GET, handleRates);
  server.on("/getRates", HTTP_GET, handleGetRatesJuanfi);
  server.on("/topUp", HTTP_GET, handleTopUp);
  server.on("/topUp", HTTP_POST, handleTopUp);
  server.on("/checkCoin", HTTP_GET, handleCheckCoin);
  server.on("/checkCoin", HTTP_POST, handleCheckCoin);
  server.on("/useVoucher", HTTP_GET, handleUseVoucher);
  server.on("/useVoucher", HTTP_POST, handleUseVoucher);
  server.on("/cancelTopUp", HTTP_GET, handleCancelTopUp);
  server.on("/cancelTopUp", HTTP_POST, handleCancelTopUp);
  server.on("/gate", HTTP_GET, handleGateTest);
  server.on("/gatetest", HTTP_GET, handleGateTestPage);
  server.on("/reboot", HTTP_GET, handleReboot);
  server.on("/debugUsers", HTTP_GET, handleDebugUsers);
  server.on("/reportTest", HTTP_GET, handleReportTest);
  server.on("/publishTest", HTTP_GET, handlePublishTest);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/setRates", HTTP_GET, handleSetRates);
  server.on("/setRates", HTTP_POST, handleSetRates);
  server.on("/generate_204", HTTP_GET, handleCaptive);
  server.on("/fwlink", HTTP_GET, handleCaptive);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptive);
  server.onNotFound(handleNotFound);
}

// R7: OTA must also start when the bridge BOOTED INTO THE SETUP PORTAL and
// later self-healed onto the router (watchPortalJoin). The old code only
// called ArduinoOTA.begin() during a station-mode boot, so after a power
// event that put the ESP in the portal first, OTA stayed dead until the next
// reboot even though the unit was back online.
#ifdef ESP32
bool otaStarted = false;
void ensureOtaStarted() {
  if (otaStarted || portalMode || config.configured != 1) return;
  ArduinoOTA.setHostname("piso-bridge");
  ArduinoOTA.setPassword(config.apiPass);
  ArduinoOTA.onStart([]() {
    logWarn("[OTA] Update starting - coinslot must be idle");
  });
  ArduinoOTA.onEnd([]() { logInfo("[OTA] Update finished - restarting"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    if (p % 25 == 0) logInfo("[OTA] Progress: " + String(p * 100 / t) + "%");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    logErr("[OTA ERROR] Code " + String((int)e));
  });
  ArduinoOTA.begin();
  otaStarted = true;
  logInfo("[OTA] ArduinoOTA ready (password-protected)");
}
#endif

// ---------------------------------------------------------------------------
// setup() / loop()
// ---------------------------------------------------------------------------
void setup() {
  // Bootlock prevention: force safe pin states before anything else touches
  // the GPIOs. D7 gate closed (LOW), D6 coin line pulled HIGH internally.
  pinMode(PIN_GATE, OUTPUT);
  digitalWrite(PIN_GATE, LOW);
  pinMode(PIN_COIN, INPUT_PULLUP);
#ifdef ESP32
  // Install the GPIO ISR service once so attachInterrupt/detachInterrupt
  // work reliably (the coin pulse line is a CHANGE-edge interrupt).
  gpio_install_isr_service(0);
#endif

  Serial.begin(SERIAL_BAUD);
  Serial.setDebugOutput(false);   // suppress SDK debug/exception hex dumps
  delay(300);

  Serial.println();
  logInfo("PisoWifiBridge starting...");
#ifdef ESP32
  logInfo("Board: ESP32 DevKit (30-pin) | Flash mode: DIO");
#else
  logInfo("Board: NodeMCU 1.0 (ESP-12E Module) | Flash mode: DOUT");
#endif
  logInfo("Reset reason: " + resetReasonText());
  logInfo("Free heap: " + String(ESP.getFreeHeap()) + " bytes");

  // Power optimization: keep the modem out of sleep so TX bursts are steady
  // and never overlap with solenoid switching (see runCoinCycle ordering).
  WiFi.persistent(false);
#ifdef ESP32
  WiFi.setSleep(false);   // keep the modem responsive
#else
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif

  EEPROM.begin(EEPROM_SIZE);
  loadConfig();
  salesLoad();
  promoLoad();
  pendingLoad();
  pubqLoad();
  nightlyLoad();
  pinMode(gatePin(), OUTPUT);
  digitalWrite(gatePin(), gateCloseLevel());   // apply configured gate pin/polarity
#ifdef ESP32
  // ESP32 TX power lives in the Wi-Fi driver, in 0.25 dBm units (8..84).
  int8_t pwr = (int8_t)(txPowerDbm() * 4.0f);
  if (pwr < 8)  pwr = 8;
  if (pwr > 84) pwr = 84;
  esp_wifi_set_max_tx_power(pwr);   // lower TX power = less current draw
#else
  WiFi.setOutputPower(txPowerDbm());  // lower TX power = less current draw
#endif
  logInfo("[NETWORK] Wi-Fi TX power set to " + String(txPowerDbm()) + " dBm");
  sales.bootCount++;
  salesSave();
  eepromCommitNow();   // one commit per boot (persist the boot counter)
  logInfo("[SALES] Boot count: " + String(sales.bootCount));
  reportEvent("boot", 0,
              "boot #" + String(sales.bootCount) + " reset=" + resetReasonText() +
              " fw=" FW_REVISION,
              "", "", 0, 0);

  registerRoutes();

  if (config.configured == 1) {
    startStation();
  } else {
    logWarn("[CONFIG] Device is unconfigured - starting setup portal");
    startPortal();
  }

#ifdef ESP32
  ensureOtaStarted();
#endif
}

void loop() {
  if (portalMode) dnsServer.processNextRequest();
  tickCoinSession();
  tickEepromFlush();
  server.handleClient();
#ifdef ESP32
  // OTA only when the coinslot is idle: an update must never interrupt a
  // credit burst or a pending recovery retry.
  if (!portalMode && cycleState == CYCLE_IDLE && !pendingAny()) {
    ensureOtaStarted();   // R7: also starts after a portal-mode self-heal
    ArduinoOTA.handle();
  }
#endif
  if (!portalMode) {
    watchWifi();
  } else {
    watchPortalJoin();   // R5: boot-order safety net - leave the setup portal on our own
  }
  if (!portalMode) tickPendingCredit();
  if (!portalMode) tickVoucherPublish();
  if (!portalMode) tickNtp();
  if (!portalMode) tickNightlyRestart();
  if (!portalMode) flushRemoteLogs();
  if (!portalMode) tickReportOutbox();
  heapWatchdog();
  yield();
}
