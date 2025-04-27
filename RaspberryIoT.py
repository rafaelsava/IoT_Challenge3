import paho.mqtt.client as mqtt
import sqlite3
import json
import time
import threading
import queue
import logging

# Configuración de logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

# Configuración de brokers
LOCAL_MQTT_HOST = "localhost"
LOCAL_MQTT_PORT = 1883
LOCAL_MQTT_TOPIC = "iot/fire/telemetry"
UBIDOTS_MQTT_HOST = "industrial.api.ubidots.com"
UBIDOTS_MQTT_PORT = 1883
UBIDOTS_TOKEN = "BBUS-GKYhgb5YytH3Fq4eaLXAQezK7DXYwW"  # Reemplaza con tu token
UBIDOTS_DEVICE_LABEL = "fire-system"
UBIDOTS_TOPICS = {
    "temperature": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/temperature",
    "gas": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/gas",
    "flame": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/flame",
    "alarm": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/alarm",
    #"alarm_control": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/alarm_control"
}
DB_PATH = "iot_data.db"

# Cola para mensajes MQTT
message_queue = queue.Queue()

# Clientes MQTT
local_mqtt_client = mqtt.Client(client_id="local_client")
ubidots_mqtt_client = mqtt.Client(client_id="ubidots_client")


# Funciones de SQLite
def connect_db():
    try:
        conn = sqlite3.connect(DB_PATH, check_same_thread=False)
        return conn
    except sqlite3.Error as e:
        logging.error(f"Error al conectar a SQLite: {e}")
        return None

def save_to_db(temperature, gas, flame, alarm):
    conn = connect_db()
    if conn:
        try:
            cursor = conn.cursor()
            cursor.execute(
                "INSERT INTO fire_telemetry (temperature, gas, flame, alarm) VALUES (?, ?, ?, ?)",
                (temperature, gas, int(flame), alarm)
            )
            conn.commit()
            logging.info(f"Datos guardados en SQLite: temp={temperature}, gas={gas}, flame={flame}, alarm={alarm}")
        except sqlite3.Error as e:
            logging.error(f"Error al guardar en SQLite: {e}")
        finally:
            conn.close()

def on_ubidots_message(client, userdata, message):
    try:
        logging.info("Mensaje recibido de Ubidots")
        payload = message.payload.decode("utf-8")
        value = float(payload)
        
        if value == 0:  # Desactivar la alarma
            mqtt_publish_to_esp32("alarm/reset")
    except Exception as e:
        logging.error(f"Error al procesar el mensaje de Ubidots: {e}")

def mqtt_publish_to_esp32(topic):
    try:
        local_mqtt_client.publish(f"iot/fire/{topic}", "1")
        logging.info(f"Mensaje enviado a ESP32 para {topic}")
    except Exception as e:
        logging.error(f"Error al enviar mensaje a ESP32: {e}")


# Función para procesar mensajes en un hilo separado
def process_messages():
    while True:
        try:
            # Obtener mensaje de la cola
            message = message_queue.get()
            payload = message["payload"]
            logging.info(f"Procesando mensaje: {payload}")

            # Parsear JSON
            data = json.loads(payload)
            temperature = float(data.get("temp"))
            gas = int(data.get("gas"))
            flame = data.get("flame")
            alarm = int(data.get("alarm"))
            #alarm_control = int(data.get("alarm_control"))

            # Almacenar en SQLite
            save_to_db(temperature, gas, flame, alarm)

            # Publicar a Ubidots
            ubidots_payloads = {
                "temperature": json.dumps({"value": temperature}),
                "gas": json.dumps({"value": gas}),
                "flame": json.dumps({"value": 1 if flame else 0}),
                "alarm": json.dumps({"value": alarm}),
                #"alarm_control": json.dumps({"value": alarm_control})
            }
            for var, topic in UBIDOTS_TOPICS.items():
                ubidots_mqtt_client.publish(topic, ubidots_payloads[var])
                logging.info(f"Datos enviados a Ubidots: topic={topic}, payload={ubidots_payloads[var]}")

            message_queue.task_done()
        except Exception as e:
            logging.error(f"Error al procesar mensaje: {e}")

# Callbacks MQTT
def on_local_message(client, userdata, message):
    try:
        payload = message.payload.decode("utf-8")
        # Enviar mensaje a la cola
        message_queue.put({"payload": payload})
    except Exception as e:
        logging.error(f"Error al recibir mensaje MQTT: {e}")

def on_local_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.info("Conectado al broker local")
        client.subscribe(LOCAL_MQTT_TOPIC)
        logging.info(f"Suscrito al topic {LOCAL_MQTT_TOPIC}")
    else:
        logging.error(f"Fallo al conectar al broker local, código: {rc}")

def on_ubidots_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.info("Conectado al broker de Ubidots")
        control_topic = f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/alarm_control/lv"
        client.subscribe(control_topic)
        logging.info(f"Suscrito a {control_topic}")
    else:
        logging.error(f"Fallo al conectar al broker de Ubidots, código: {rc}")


# Configurar callbacks
local_mqtt_client.on_connect = on_local_connect
local_mqtt_client.on_message = on_local_message
ubidots_mqtt_client.on_connect = on_ubidots_connect
ubidots_mqtt_client.on_message = on_ubidots_message#


# Función para iniciar clientes MQTT
def start_mqtt_clients():
    try:
        local_mqtt_client.connect(LOCAL_MQTT_HOST, LOCAL_MQTT_PORT)
        ubidots_mqtt_client.username_pw_set(UBIDOTS_TOKEN, "")
        ubidots_mqtt_client.connect(UBIDOTS_MQTT_HOST, UBIDOTS_MQTT_PORT)
        local_mqtt_client.loop_start()
        ubidots_mqtt_client.loop_start()
    except Exception as e:
        logging.error(f"Error al conectar a los brokers: {e}")
        raise

# Hilo principal
if _name_ == "_main_":
    # Crear tabla SQLite si no existe
    conn = connect_db()
    if conn:
        try:
            cursor = conn.cursor()
            cursor.execute("""
                CREATE TABLE IF NOT EXISTS fire_telemetry (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    temperature REAL NOT NULL,
                    gas INTEGER NOT NULL,
                    flame BOOLEAN NOT NULL,
                    alarm INTEGER NOT NULL,
                    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
                )
            """)
            conn.commit()
        except sqlite3.Error as e:
            logging.error(f"Error al crear la tabla: {e}")
        finally:
            conn.close()

    # Iniciar hilo para procesar mensajes
    processing_thread = threading.Thread(target=process_messages, daemon=True)
    processing_thread.start()

    # Iniciar clientes MQTT
    start_mqtt_clients()

    # Bucle principal (solo monitoreo)
    try:
        while True:
            logging.info(f"Cola de mensajes: {message_queue.qsize()} mensajes pendientes")
            time.sleep(10)
    except KeyboardInterrupt:
        logging.info("Deteniendo el script...")
        local_mqtt_client.loop_stop()
        ubidots_mqtt_client.loop_stop()
        local_mqtt_client.disconnect()
        ubidots_mqtt_client.disconnect()