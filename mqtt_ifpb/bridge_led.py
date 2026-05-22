import sqlite3
from datetime import datetime
import paho.mqtt.client as mqtt
import requests

LOCAL_BROKER = "localhost"
LOCAL_TOPIC = "ifpb/projeto/led"

ADAFRUIT_USER = "pedro3005"
ADAFRUIT_KEY = "aio_NmbK70Fm4w7zJiC2vDtY2I3yrPvC"
ADAFRUIT_BROKER = "io.adafruit.com"
ADAFRUIT_PORT = 8883
ADAFRUIT_TOPIC_STATUS = f"{ADAFRUIT_USER}/feeds/led-status"
ADAFRUIT_TOPIC_HISTORICO = f"{ADAFRUIT_USER}/feeds/led-historico"

DB_NAME = "eventos_led.db"

def salvar_no_banco(estado):
    data_hora = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    conexao = sqlite3.connect(DB_NAME)
    cursor = conexao.cursor()

    cursor.execute(
        "INSERT INTO eventos (estado, data_hora) VALUES (?, ?)",
        (estado, data_hora)
    )

    conexao.commit()
    conexao.close()

    print(f"Salvo no banco: {estado} em {data_hora}")

def publicar_na_nuvem(estado):
    valor_historico = "1" if estado == "ON" else "0"

    url_status = f"https://io.adafruit.com/api/v2/{ADAFRUIT_USER}/feeds/led-status/data"
    url_historico = f"https://io.adafruit.com/api/v2/{ADAFRUIT_USER}/feeds/led-historico/data"

    headers = {
        "X-AIO-Key": ADAFRUIT_KEY,
        "Content-Type": "application/json"
    }

    resposta_status = requests.post(url_status, headers=headers, json={"value": estado}, timeout=10)
    resposta_historico = requests.post(url_historico, headers=headers, json={"value": valor_historico}, timeout=10)

    if resposta_status.status_code in [200, 201] and resposta_historico.status_code in [200, 201]:
        print(f"Publicado na nuvem: {estado} / {valor_historico}")
    else:
        print("Erro ao publicar na nuvem")
        print(resposta_status.status_code, resposta_status.text)
        print(resposta_historico.status_code, resposta_historico.text)

def on_connect(client, userdata, flags, rc):
    print("Conectado ao broker local")
    client.subscribe(LOCAL_TOPIC)

ultimo_estado = None

def on_message(client, userdata, msg):
    global ultimo_estado

    estado = msg.payload.decode().strip()

    if estado not in ["ON", "OFF"]:
        return

    if estado == ultimo_estado:
        return

    ultimo_estado = estado

    print(f"Mensagem recebida: {estado}")
    salvar_no_banco(estado)
    publicar_na_nuvem(estado)
    
client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect(LOCAL_BROKER, 1883, 60)
client.loop_forever()
