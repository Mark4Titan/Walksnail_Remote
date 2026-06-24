#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>

#define PIN_PWM_VTL5C3  5  // GPIO5 (D1) -> Вазистор
#define PIN_BIND_BUTTON 4  // GPIO4 (D2) -> Оптопара бінду
#define PIN_TRIGER     16  // GPIO16 (D0) -> "Рубильник" PC817 (ON/OFF)

SoftwareSerial remoteSerial(D3, D4); // RX=D3, TX=D4

const byte DNS_PORT = 53;
DNSServer dnsServer;
ESP8266WebServer server(80);

const char *ssid = "Walksnail_Remote";
const char *password = "12345678";

bool wifiActive = false; 
unsigned long lastUartPacketTime = 0;
const unsigned long WIFI_TIMEOUT_MS = 10000; 

String lastReceivedHex = "00 00 00 (No data yet)";

// Масив імен розширено до 9 елементів
const char* keyNames[9] = {"UP", "DOWN", "LEFT", "RIGHT", "ENTER", "BACK", "REC", "BIND", "FUNC"};

struct ButtonItem {
  int pwm;
  uint8_t b1, b2, b3;
};

struct MainConfig {
  uint16_t magic;     
  ButtonItem keys[9]; // Матриця на 9 кнопок
} cfg;

uint8_t uartBuffer[3];

void saveConfig() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void loadDefaults() {
  cfg.magic = 0xAA55;
  cfg.keys[0].pwm = 5;   cfg.keys[0].b1 = 0x46; cfg.keys[0].b2 = 0xFF; cfg.keys[0].b3 = 0x29; // UP
  cfg.keys[1].pwm = 48;  cfg.keys[1].b1 = 0xC6; cfg.keys[1].b2 = 0xFB; cfg.keys[1].b3 = 0x27; // DOWN
  cfg.keys[2].pwm = 10;  cfg.keys[2].b1 = 0x46; cfg.keys[2].b2 = 0xFB; cfg.keys[2].b3 = 0x25; // LEFT
  cfg.keys[3].pwm = 21;  cfg.keys[3].b1 = 0xB7; cfg.keys[3].b2 = 0x23; cfg.keys[3].b3 = 0x23; // RIGHT
  cfg.keys[4].pwm = 32;  cfg.keys[4].b1 = 0x0C; cfg.keys[4].b2 = 0xFF; cfg.keys[4].b3 = 0x2B; // ENTER
  cfg.keys[5].pwm = 90;  cfg.keys[5].b1 = 0xC6; cfg.keys[5].b2 = 0xDD; cfg.keys[5].b3 = 0x3F; // BACK
  cfg.keys[6].pwm = 128; cfg.keys[6].b1 = 0x46; cfg.keys[6].b2 = 0xDD; cfg.keys[6].b3 = 0x3D; // REC 
  
  // Сервісні кнопки (для них PWM умовний, керують цифрою чи іншою логікою)
  cfg.keys[7].pwm = 0;   cfg.keys[7].b1 = 0xAC; cfg.keys[7].b2 = 0xFF; cfg.keys[7].b3 = 0x53; // BIND
  cfg.keys[8].pwm = 0;   cfg.keys[8].b1 = 0x4C; cfg.keys[8].b2 = 0xFF; cfg.keys[8].b3 = 0x3B; // FUNC (Нова кнопка)
  saveConfig();
}

void executeTriplePulseScan(int f1, int f2, int f3) {
  analogWrite(PIN_PWM_VTL5C3, f1);  delay(150); 
  analogWrite(PIN_PWM_VTL5C3, f2);  delay(30); 
  digitalWrite(PIN_TRIGER, HIGH);   delay(250); 
  digitalWrite(PIN_TRIGER, LOW);    delay(10); 
  analogWrite(PIN_PWM_VTL5C3, f3);  delay(100); 
  analogWrite(PIN_PWM_VTL5C3, 0);
}

// Менеджер дій під 8 кнопок
void executeAction(int id) {
  if (id == 7) {
    // Твій чіткий бінд на 500мс
    digitalWrite(PIN_BIND_BUTTON, HIGH);
    delay(500); 
    digitalWrite(PIN_BIND_BUTTON, LOW);
    Serial.println("[ACTION] Hardware BIND executed (500ms).");
  } 
  else if (id == 8) {
    // Нова зарезервована кнопка FUNC. Поки просто пишемо в дебаг, 
    // сюди можна додати будь-яку логіку (наприклад, перемикання реле чи інший пін)
    Serial.println("[ACTION] Custom FUNC button executed!");
  } 
  else {
    // Усі штатні кнопки OSD + REC йдуть через вазистор
    executeTriplePulseScan(0, cfg.keys[id].pwm, 0);
    Serial.printf("[ACTION] Button %s executed (PWM: %d).\n", keyNames[id], cfg.keys[id].pwm);
  }
}

void startWiFiAP() {
  if (wifiActive) return;
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid, password);
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));
  server.begin();
  wifiActive = true;
  Serial.println("[SYSTEM] Wi-Fi AP Activated!");
}

void stopWiFiAP() {
  if (!wifiActive) return;
  WiFi.mode(WIFI_OFF);
  wifiActive = false;
  Serial.println("[SYSTEM] Wi-Fi AP Disabled.");
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>";
  html += "<style>body{font-family:Arial;background:#1e1e24;color:white;text-align:center;padding:10px;user-select:none;}";
  html += "h2{color:#00adb5;margin:10px 0;} table{margin:0 auto;background:#222831;border-radius:10px;padding:10px;border-spacing:5px;}";
  html += "th{font-size:12px;color:#00adb5;} input[type='number'],input[type='text']{width:55px;padding:5px;background:#393e46;color:white;border:1px solid #00adb5;border-radius:5px;text-align:center;}";
  html += ".btn{background:#393e46;color:#00adb5;font-weight:bold;padding:8px 15px;border:2px solid #00adb5;border-radius:8px;cursor:pointer;}";
  html += ".btn-save{background:#00adb5;color:#1e1e24;width:95%;padding:12px;font-size:16px;margin-top:15px;font-weight:bold;border:none;border-radius:10px;}";
  html += ".btn-action{width:70px;height:70px;border-radius:15px;margin:5px;font-size:20px;box-shadow: 0 4px #222831;} .btn-action:active{transform: translateY(2px);box-shadow:0 2px #222831;}";
  html += ".monitor-box { background:#393e46; border:2px solid #ff9f43; border-radius:10px; padding:10px; margin: 15px auto; width:90%; max-width:320px; }";
  html += ".monitor-box h4 { margin: 0 0 5px 0; color:#ff9f43; font-size:14px; text-transform: uppercase; }";
  html += ".monitor-data { font-family: 'Courier New', monospace; font-size:18px; font-weight:bold; color:#fff; }";
  html += "</style></head><body><h2>Walksnail VRX Control</h2>";
  
  html += "<div class='monitor-box'><h4>Live UART Receiver (D3)</h4><div class='monitor-data' id='uart-stream'>" + lastReceivedHex + "</div></div>";

  // Кнопки управління
  html += "<div>";
  html += "  <button class='btn btn-action' style='width:110px;height:50px;' onclick=\"fetch('/click?id=5')\">BACK</button>";
  html += "  <button class='btn btn-action' style='width:110px;height:50px;' onclick=\"fetch('/click?id=6')\">REC</button><br>"; // Повернули кнопку REC в UI
  html += "  <button class='btn btn-action' onclick=\"fetch('/click?id=0')\">&uarr;</button><br>";
  html += "  <button class='btn btn-action' onclick=\"fetch('/click?id=2')\">&larr;</button>";
  html += "  <button class='btn btn-action' style='background:#222831;color:white;' onclick=\"fetch('/click?id=4')\">OK</button>";
  html += "  <button class='btn btn-action' onclick=\"fetch('/click?id=3')\">&rarr;</button><br>";
  html += "  <button class='btn btn-action' onclick=\"fetch('/click?id=1')\">&darr;</button><br><br>";
  
  html += "  <button class='btn' style='border-color:#ff2e63;color:#ff2e63;width:110px;height:45px;font-size:14px;margin-right:10px;' onclick=\"if(confirm('Start BIND process?')){fetch('/click?id=7');}\">BIND</button>";
  html += "  <button class='btn' style='border-color:#ff9f43;color:#ff9f43;width:110px;height:45px;font-size:14px;' onclick=\"fetch('/click?id=8')\">FUNC</button>"; // Нова кнопка FUNC в UI
  html += "</div><br>";

  // Форма таблиці
  html += "<form action='/save' method='POST'><table><tr><th>Key</th><th>PWM</th><th>HEX1</th><th>HEX2</th><th>HEX3</th></tr>";
  for (int i = 0; i < 9; i++) {
    char h1[3], h2[3], h3[3]; sprintf(h1, "%02X", cfg.keys[i].b1); sprintf(h2, "%02X", cfg.keys[i].b2); sprintf(h3, "%02X", cfg.keys[i].b3);
    html += "<tr><td><b>" + String(keyNames[i]) + "</b></td>";
    
    // Блокуємо PWM для сервісних кнопок BIND та FUNC
    if (i >= 7) {
      html += "<td><input type='number' value='0' disabled style='opacity:0.5;'></td>";
    } else {
      html += "<td><input type='number' name='p" + String(i) + "' value='" + String(cfg.keys[i].pwm) + "'></td>";
    }
    html += "<td><input type='text' name='b1_" + String(i) + "' value='" + String(h1) + "'></td><td><input type='text' name='b2_" + String(i) + "' value='" + String(h2) + "'></td><td><input type='text' name='b3_" + String(i) + "' value='" + String(h3) + "'></td></tr>";
  }
  html += "</table><input type='submit' class='btn btn-save' value='SAVE CONFIG MATRIX'></form>";
  html += "<script>setInterval(function(){fetch('/get_uart').then(r=>r.text()).then(d=>{document.getElementById('uart-stream').innerText=d;});},800);</script></body></html>";
  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  remoteSerial.begin(115200);
  EEPROM.begin(sizeof(MainConfig));
  EEPROM.get(0, cfg);
  if (cfg.magic != 0xAA55) loadDefaults();

  pinMode(PIN_PWM_VTL5C3, OUTPUT); analogWrite(PIN_PWM_VTL5C3, 0);
  pinMode(PIN_BIND_BUTTON, OUTPUT); digitalWrite(PIN_BIND_BUTTON, LOW);
  pinMode(PIN_TRIGER, OUTPUT); digitalWrite(PIN_TRIGER, LOW);
  analogWriteRange(1023); analogWriteFreq(5000);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, [](){
    for(int i=0; i<9; i++){
      if(i < 7 && server.hasArg("p"+String(i))) cfg.keys[i].pwm = server.arg("p"+String(i)).toInt();
      if(server.hasArg("b1_"+String(i))) cfg.keys[i].b1 = strtol(server.arg("b1_"+String(i)).c_str(),NULL,16);
      if(server.hasArg("b2_"+String(i))) cfg.keys[i].b2 = strtol(server.arg("b2_"+String(i)).c_str(),NULL,16);
      if(server.hasArg("b3_"+String(i))) cfg.keys[i].b3 = strtol(server.arg("b3_"+String(i)).c_str(),NULL,16);
    }
    saveConfig(); server.sendHeader("Location","/"); server.send(302,"text/plain","");
  });
  server.on("/get_uart", [](){ server.send(200, "text/plain", lastReceivedHex); });
  server.on("/click", [](){
    int id = server.arg("id").toInt();
    if(id >= 0 && id < 9) executeAction(id); 
    server.sendHeader("Location","/"); server.send(302,"text/plain","");
  });
  
  server.onNotFound([](){ server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", ""); });
  lastUartPacketTime = millis();
}

void loop() {
  int wifiClients = WiFi.softAPgetStationNum();

  if (wifiActive) {
    dnsServer.processNextRequest();
    server.handleClient();
    
    if (wifiClients == 0 && (millis() - lastUartPacketTime < WIFI_TIMEOUT_MS)) {
      stopWiFiAP();
    }
  } else {
    if (millis() - lastUartPacketTime > WIFI_TIMEOUT_MS) {
      startWiFiAP();
    }
  }

  // ОБРОБНИК UART (матриця на 9 станів кнопок)
  if (remoteSerial.available() >= 3) {
    uartBuffer[0] = remoteSerial.read();
    if (uartBuffer[0] != 0xC6 && uartBuffer[0] != 0x46 && uartBuffer[0] != 0xAC && uartBuffer[0] != 0x0C && uartBuffer[0] != 0x4C && uartBuffer[0] != 0xB7) {
      return; 
    }
    uartBuffer[1] = remoteSerial.read();
    uartBuffer[2] = remoteSerial.read();

    lastUartPacketTime = millis(); 

    char hexStr[32];
    sprintf(hexStr, "%02X %02X %02X", uartBuffer[0], uartBuffer[1], uartBuffer[2]);
    
    if (uartBuffer[0] == 0xC6 && uartBuffer[1] == 0xFF && uartBuffer[2] == 0x67) {
      lastReceivedHex = String(hexStr) + " (IDLE)";
      return; 
    }

    lastReceivedHex = String(hexStr) + " (OK)";
    Serial.printf("[UART] Captured: %02X %02X %02X\n", uartBuffer[0], uartBuffer[1], uartBuffer[2]);

    for (int i = 0; i < 9; i++) {
      if (uartBuffer[0] == cfg.keys[i].b1 && uartBuffer[1] == cfg.keys[i].b2 && uartBuffer[2] == cfg.keys[i].b3) {
        executeAction(i); 
        delay(200); 
        break;
      }
    }
  }
}