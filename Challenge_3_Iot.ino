#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PubSubClient.h>

// ===================== Configuración WiFi y Servidor =====================
const char* ssid = "Zflip de Valentina";
const char* password = "v4l32006";
WebServer server(80);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Dirección IP de la Raspberry Pi
const char* mqtt_server = "192.168.58.152";
const int mqtt_port = 1883;
const char* mqtt_topic = "iot/fire/telemetry";

// ===================== Parámetros de Sensores y Actuadores =====================
#define TEMP_THRESHOLD 27
#define GAS_THRESHOLD 700
#define FLAME_BUTTON_PIN 15

const int buzzer = 27;
const int gasSensorPin = 35;
int red_led = 19;
int green_led = 18;
int yellow_led = 5;

LiquidCrystal_I2C lcd(0x27, 16, 2);
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ===================== Variables Globales =====================
volatile float currentTemp = 0.0;
volatile int currentGas = 0;
volatile bool currentFlame = false;
volatile bool alarmActive = false;
volatile int alarmLevel = 0;
unsigned long previousAlarmTime = 0;
int alarmInterval = 0;
volatile bool manualReset = false;

// ===================== Historial de Mediciones =====================
struct Measurement {
  float temp;
  int gas;
  bool flame;
  unsigned long timestamp;
};

#define HISTORY_SIZE 10
Measurement history[HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;

// ===================== Sincronización =====================
SemaphoreHandle_t dataMutex;
SemaphoreHandle_t historyMutex;
SemaphoreHandle_t alarmMutex;
QueueHandle_t sensorQueue;

struct SensorData {
  float temp;
  int gas;
  bool flame;
  int alarm;
};

// ===================== Tarea de Medición de Sensores =====================
void sensorTask(void * parameter) {
  sensors.begin();
  analogReadResolution(10);
  static unsigned long conditionStartTime = 0;

  for(;;) {
    if (xSemaphoreTake(alarmMutex, portMAX_DELAY) == pdTRUE) {
      if (manualReset) {
        conditionStartTime = 0;
        alarmActive = false;
        alarmLevel = 0;
        digitalWrite(red_led, LOW);
        digitalWrite(yellow_led, LOW);
        digitalWrite(green_led, HIGH);
        noTone(buzzer);
        manualReset = false;
      }
      xSemaphoreGive(alarmMutex);
    }

    sensors.requestTemperatures();
    float temp = sensors.getTempCByIndex(0);
    int gasValue = analogRead(gasSensorPin);
    bool buttonFlame = (digitalRead(FLAME_BUTTON_PIN) == LOW);

    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      currentTemp = temp;
      currentGas = gasValue;
      currentFlame = buttonFlame;
      xSemaphoreGive(dataMutex);
    }

    if (xSemaphoreTake(historyMutex, portMAX_DELAY) == pdTRUE) {
      Measurement m;
      m.temp = temp;
      m.gas = gasValue;
      m.flame = buttonFlame;
      m.timestamp = millis();
      history[historyIndex] = m;
      historyIndex = (historyIndex + 1) % HISTORY_SIZE;
      if (historyCount < HISTORY_SIZE) historyCount++;
      xSemaphoreGive(historyMutex);
    }

    SensorData data = {temp, gasValue, buttonFlame, alarmLevel};
    xQueueSend(sensorQueue, &data, portMAX_DELAY);

    int conditionsMet = 0;
    if (temp > TEMP_THRESHOLD) conditionsMet++;
    if (gasValue > GAS_THRESHOLD) conditionsMet++;
    if (buttonFlame) conditionsMet++;

    if (conditionsMet >= 2) {
      if (conditionStartTime == 0) {
        conditionStartTime = millis();
      } else if (millis() - conditionStartTime >= 3000) {
        if (xSemaphoreTake(alarmMutex, portMAX_DELAY) == pdTRUE) {
          alarmActive = true;
          alarmLevel = (conditionsMet == 2) ? 2 : 3;
          xSemaphoreGive(alarmMutex);
        }
        lcd.clear();
        lcd.setCursor(0, 0);
        digitalWrite(green_led, LOW);

        if (conditionsMet == 2) {
          digitalWrite(red_led, LOW);
          digitalWrite(yellow_led, HIGH);
          lcd.print("POSIBLE INCENDIO!");
          alarmInterval = 2000;
        } else {
          digitalWrite(red_led, HIGH);
          digitalWrite(yellow_led, LOW);
          lcd.print("INCENDIO SEGURO!");
          alarmInterval = 500;
        }
        if (millis() - previousAlarmTime >= alarmInterval) {
          tone(buzzer, 1000);
          delay(500);
          noTone(buzzer);
          previousAlarmTime = millis();
        }
      }
    } else {
      conditionStartTime = 0;
      if (xSemaphoreTake(alarmMutex, portMAX_DELAY) == pdTRUE) {
        if (alarmActive) {
          digitalWrite(red_led, LOW);
          digitalWrite(yellow_led, LOW);
          digitalWrite(green_led, HIGH);
        }
        alarmActive = false;
        alarmLevel = 0;
        xSemaphoreGive(alarmMutex);
      }
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Temp: ");
      lcd.print(temp);
      lcd.print("C");
      lcd.setCursor(0, 1);
      lcd.print("Gas: ");
      lcd.print(gasValue);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ===================== Tarea de Gestión MQTT =====================
void mqttTask(void * parameter) {
  mqttClient.setCallback(mqttCallback);
  mqttClient.setServer(mqtt_server, mqtt_port);


  for(;;) {
    if (!mqttClient.connected()) {
      while (!mqttClient.connected()) {
        Serial.print("Conectando a MQTT...");
        if (mqttClient.connect("ESP32_FireClient")) {
          Serial.println("Conectado.");
          mqttClient.subscribe("iot/fire/alarm/reset");
        } else {
          Serial.print("Error, rc=");
          Serial.print(mqttClient.state());
          Serial.println(" Reintentando en 5s");
          vTaskDelay(pdMS_TO_TICKS(5000));
        }
        
      }
    }
    mqttClient.loop();

    SensorData data;
    if (xQueueReceive(sensorQueue, &data, portMAX_DELAY)) {
      int is_alarm;
      if(data.alarm == 2 || data.alarm ==3){
        is_alarm=1;
      }
      else{
        is_alarm=0;
      }
      String payload = "{";
      payload += "\"temp\":" + String(data.temp, 1) + ",";
      payload += "\"gas\":" + String(data.gas) + ",";
      payload += "\"flame\":" + String(data.flame ? "true" : "false") + ",";
      payload += "\"alarm\":" + String(data.alarm)+",";
      payload += "\"alarm_control\":" + String(is_alarm);
      payload += "}";
      mqttClient.publish(mqtt_topic, payload.c_str());
      Serial.println("Datos enviados via MQTT: " + payload);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Callback para el control de la alarma
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  if (String(topic) == "iot/fire/alarm/reset") {
    // Desactivar alarma
    if (xSemaphoreTake(alarmMutex, portMAX_DELAY) == pdTRUE) {
      manualReset = true;  // Activar el reseteo manual
      xSemaphoreGive(alarmMutex);
      Serial.println("Alarma desactivada por Ubidots.");
    }
  }
}


// ===================== Funciones del Servidor Web =====================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Tablero de Control - Incendios</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; background-color: #f4f4f4; color: #333; }
    h1, h2 { color: #FF8000; }
    .container { width: 90%; max-width: 900px; margin: auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.2); }
    table { width: 100%; border-collapse: collapse; margin-top: 20px; }
    th, td { border: 1px solid #ddd; padding: 10px; text-align: center; }
    th { background-color: #FF8000; color: white; }
    button { padding: 10px 20px; font-size: 16px; margin-top: 20px; border: none; border-radius: 5px; cursor: pointer; background-color: #007BFF; color: white; }
    button:hover { opacity: 0.8; }
    #notification { font-size: 18px; margin-top: 20px; padding: 10px; color: white; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Tablero de Control - Prevención de Incendios</h1>
    <div id="notification"></div>
    <table>
      <tr>
        <th>Variable</th>
        <th>Valor Actual</th>
      </tr>
      <tr>
        <td>Temperatura</td>
        <td id="temp-value">-- °C</td>
      </tr>
      <tr>
        <td>Gas</td>
        <td id="gas-value">--</td>
      </tr>
      <tr>
        <td>Llama</td>
        <td id="flame-value">--</td>
      </tr>
    </table>
    <button onclick="resetAlarm()">Desactivar Alarmas</button>
    <h2>Historial de Mediciones</h2>
    <table id="history-table">
      <tr>
        <th>Tiempo (s)</th>
        <th>Temperatura</th>
        <th>Gas</th>
        <th>Llama</th>
      </tr>
    </table>
  </div>
  <script>
    let lastAlarm = 0;
    function actualizarDatos() {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          document.getElementById("temp-value").innerText = data.temperatura + " °C";
          document.getElementById("gas-value").innerText = data.gas;
          document.getElementById("flame-value").innerText = data.llama === true ? "SI" : "NO";
          actualizarNotificacion(data.alarm);
        })
        .catch(err => console.error("Error al obtener datos: " + err));
    }
    function actualizarNotificacion(alarmLevel) {
      const notifDiv = document.getElementById("notification");
      if (alarmLevel === 2 && lastAlarm !== 2) {
        notifDiv.innerText = "¡Alarma! POSIBLE INCENDIO (2 factores)";
        notifDiv.style.backgroundColor = "orange";
        lastAlarm = 2;
      } else if (alarmLevel === 3 && lastAlarm !== 3) {
        notifDiv.innerText = "¡Alarma! INCENDIO SEGURO (3 factores)";
        notifDiv.style.backgroundColor = "red";
        lastAlarm = 3;
      } else if (alarmLevel === 0) {
        notifDiv.innerText = "";
        notifDiv.style.backgroundColor = "";
        lastAlarm = 0;
      }
    }
    function actualizarHistorial() {
      fetch('/history')
        .then(response => response.json())
        .then(history => {
          let table = document.getElementById("history-table");
          table.innerHTML = "<tr><th>Tiempo (s)</th><th>Temperatura</th><th>Gas</th><th>Llama</th></tr>";
          for (let i = 0; i < history.length; i++) {
            let row = table.insertRow();
            row.insertCell(0).innerText = history[i].timestamp;
            row.insertCell(1).innerText = history[i].temp + " °C";
            row.insertCell(2).innerText = history[i].gas;
            row.insertCell(3).innerText = history[i].flame === true ? "SI" : "NO";
          }
        })
        .catch(err => console.error("Error al obtener historial: " + err));
    }
    function resetAlarm() {
      fetch('/reset')
        .then(response => response.text())
        .then(msg => {
          alert(msg);
          actualizarDatos();
        })
        .catch(err => console.error("Error al desactivar alarmas: " + err));
    }
    setInterval(() => {
      actualizarDatos();
      actualizarHistorial();
    }, 1000);
    actualizarDatos();
    actualizarHistorial();
  </script>
</body>
</html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handleData() {
  String json;
  if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
    json = "{";
    json += "\"temperatura\":" + String(currentTemp, 1) + ",";
    json += "\"gas\":" + String(currentGas) + ",";
    json += "\"llama\":" + String(currentFlame ? "true" : "false") + ",";
    json += "\"alarm\":" + String(alarmLevel);
    json += "}";
    xSemaphoreGive(dataMutex);
  }
  server.send(200, "application/json", json);
}

void handleHistory() {
  String json = "[";
  if (xSemaphoreTake(historyMutex, portMAX_DELAY) == pdTRUE) {
    int count = historyCount;
    int start = (historyCount < HISTORY_SIZE) ? 0 : historyIndex;
    for (int i = 0; i < count; i++) {
      int idx = (start + i) % HISTORY_SIZE;
      json += "{";
      json += "\"timestamp\":" + String(history[idx].timestamp / 1000) + ",";
      json += "\"temp\":" + String(history[idx].temp, 1) + ",";
      json += "\"gas\":" + String(history[idx].gas) + ",";
      json += "\"flame\":" + String(history[idx].flame ? "true" : "false");
      json += "}";
      if (i < count - 1) json += ",";
    }
    xSemaphoreGive(historyMutex);
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleReset() {
  if (xSemaphoreTake(alarmMutex, portMAX_DELAY) == pdTRUE) {
    manualReset = true;
    xSemaphoreGive(alarmMutex);
  }
  server.send(200, "text/plain", "Alarmas desactivadas y sistema reiniciado.");
}

// ===================== Setup Principal =====================
void setup() {
  pinMode(buzzer, OUTPUT);
  pinMode(red_led, OUTPUT);
  pinMode(green_led, OUTPUT);
  pinMode(yellow_led, OUTPUT);
  pinMode(FLAME_BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  digitalWrite(green_led, HIGH);
  digitalWrite(red_led, LOW);
  digitalWrite(yellow_led, LOW);
  analogReadResolution(10);
  lcd.setCursor(0, 0);
  lcd.print("Sistema Listo");
  sensors.begin();

  Serial.begin(115200);

  Serial.println("Conectando a WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Conectando a WiFi...");
  }
  Serial.println("Conectado a WiFi");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  dataMutex = xSemaphoreCreateMutex();
  historyMutex = xSemaphoreCreateMutex();
  alarmMutex = xSemaphoreCreateMutex();
  sensorQueue = xQueueCreate(10, sizeof(SensorData));

  if (dataMutex == NULL || historyMutex == NULL || alarmMutex == NULL || sensorQueue == NULL) {
    Serial.println("Error al crear mutex o cola");
    while (1);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/reset", HTTP_GET, handleReset);
  server.begin();
  Serial.println("Servidor iniciado");

  xTaskCreate(sensorTask, "SensorTask", 4096, NULL, 1, NULL);
  xTaskCreate(mqttTask, "MQTTTask", 4096, NULL, 1, NULL);
}

// ===================== Loop Principal =====================
void loop() {
  server.handleClient();
  vTaskDelay(pdMS_TO_TICKS(10));
}