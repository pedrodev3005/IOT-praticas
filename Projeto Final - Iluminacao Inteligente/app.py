from flask import Flask, redirect, render_template_string
from datetime import datetime
import sqlite3
import serial
import threading
import time

app = Flask(__name__)

DB_NAME = "iluminacao.db"

SERIAL_PORT = "/dev/ttyS1"
BAUD_RATE = 115200

estado_led = "OFF"
modo = "AUTO"
ultimo_estado = None

ser = None
serial_lock = threading.Lock()


def init_db():
    con = sqlite3.connect(DB_NAME)
    cur = con.cursor()

    cur.execute("""
        CREATE TABLE IF NOT EXISTS eventos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            tipo TEXT NOT NULL,
            valor TEXT NOT NULL,
            data_hora TEXT NOT NULL
        )
    """)

    con.commit()
    con.close()


def salvar_evento(tipo, valor):
    data_hora = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    con = sqlite3.connect(DB_NAME)
    cur = con.cursor()

    cur.execute(
        "INSERT INTO eventos (tipo, valor, data_hora) VALUES (?, ?, ?)",
        (tipo, valor, data_hora)
    )

    con.commit()
    con.close()


def listar_eventos(limite=15):
    con = sqlite3.connect(DB_NAME)
    cur = con.cursor()

    cur.execute("""
        SELECT id, tipo, valor, data_hora
        FROM eventos
        ORDER BY id DESC
        LIMIT ?
    """, (limite,))

    eventos = cur.fetchall()
    con.close()

    return eventos


def abrir_serial():
    global ser

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print(f"Serial aberta em {SERIAL_PORT}")
    except Exception as erro:
        ser = None
        print(f"Erro ao abrir serial: {erro}")


def enviar_serial(comando):
    global ser

    if ser is None:
        print("Serial nao conectada")
        return

    with serial_lock:
        mensagem = comando + "\n"
        ser.write(mensagem.encode())
        print(f"Enviado para Node B: {comando}")


def tratar_linha_serial(linha):
    global estado_led, modo, ultimo_estado

    if not linha or linha.strip() == "":
        return

    linha = linha.strip()

    mensagens_validas = [
        "STATUS:ON",
        "STATUS:OFF",
        "MODE:AUTO",
        "MODE:MANUAL"
    ]

    if linha not in mensagens_validas:
        return

    print(f"Recebido do Node B: {linha}")

    if linha == "STATUS:ON":
        if ultimo_estado != "ON":
            estado_led = "ON"
            ultimo_estado = "ON"
            salvar_evento("STATUS", "ON")

    elif linha == "STATUS:OFF":
        if ultimo_estado != "OFF":
            estado_led = "OFF"
            ultimo_estado = "OFF"
            salvar_evento("STATUS", "OFF")

    elif linha == "MODE:AUTO":
        if modo != "AUTO":
            modo = "AUTO"
            salvar_evento("MODE", "AUTO")

    elif linha == "MODE:MANUAL":
        if modo != "MANUAL":
            modo = "MANUAL"
            salvar_evento("MODE", "MANUAL")


def leitor_serial():
    global ser

    while True:
        if ser is None:
            time.sleep(1)
            continue

        try:
            linha = ser.readline().decode(errors="ignore").strip()

            if linha == "":
                continue

            tratar_linha_serial(linha)

        except Exception as erro:
            print(f"Erro lendo serial: {erro}")
            time.sleep(1)


@app.route("/")
def index():
    eventos = listar_eventos(15)

    html = """
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="UTF-8">
        <meta http-equiv="refresh" content="2">
        <title>Iluminação Inteligente</title>

        <style>
            body {
                font-family: Arial, sans-serif;
                background: #f2f2f2;
                margin: 40px;
            }

            .card {
                background: white;
                max-width: 750px;
                margin: auto;
                padding: 25px;
                border-radius: 12px;
                box-shadow: 0 2px 8px rgba(0,0,0,0.15);
            }

            h1 {
                text-align: center;
            }

            .status {
                font-size: 22px;
                margin: 15px 0;
            }

            .on {
                color: green;
                font-weight: bold;
            }

            .off {
                color: red;
                font-weight: bold;
            }

            .mode {
                color: #0055aa;
                font-weight: bold;
            }

            button {
                padding: 12px 18px;
                margin: 6px;
                border: none;
                border-radius: 8px;
                cursor: pointer;
                font-size: 15px;
            }

            button:disabled {
                opacity: 0.5;
                cursor: not-allowed;
            }

            .btn-on {
                background: #28a745;
                color: white;
            }

            .btn-off {
                background: #dc3545;
                color: white;
            }

            .btn-mode {
                background: #007bff;
                color: white;
            }

            .aviso {
                color: #666;
                font-size: 14px;
                margin-top: 8px;
            }

            table {
                width: 100%;
                border-collapse: collapse;
                margin-top: 20px;
            }

            th, td {
                padding: 8px;
                border-bottom: 1px solid #ddd;
                text-align: left;
            }

            a {
                color: #007bff;
                text-decoration: none;
            }
        </style>
    </head>

    <body>
        <div class="card">
            <h1>Iluminação Inteligente</h1>

            <p class="status">
                Estado da lâmpada:
                {% if estado_led == "ON" %}
                    <span class="on">LIGADA</span>
                {% else %}
                    <span class="off">DESLIGADA</span>
                {% endif %}
            </p>

            <p class="status">
                Modo atual:
                <span class="mode">{{ modo }}</span>
            </p>

            <hr>

            <h3>Controle manual</h3>

            {% if modo == "MANUAL" %}
                <a href="/ligar"><button class="btn-on">Ligar</button></a>
                <a href="/desligar"><button class="btn-off">Desligar</button></a>
            {% else %}
                <button class="btn-on" disabled>Ligar</button>
                <button class="btn-off" disabled>Desligar</button>
                <p class="aviso">O controle manual está desativado no modo automático.</p>
            {% endif %}

            <h3>Modo de operação</h3>
            <a href="/modo/auto"><button class="btn-mode">Modo Automático</button></a>
            <a href="/modo/manual"><button class="btn-mode">Modo Manual</button></a>

            <h3>Últimos eventos</h3>
            <a href="/banco">Ver banco completo</a>

            <table>
                <tr>
                    <th>ID</th>
                    <th>Tipo</th>
                    <th>Valor</th>
                    <th>Data/Hora</th>
                </tr>

                {% for evento in eventos %}
                <tr>
                    <td>{{ evento[0] }}</td>
                    <td>{{ evento[1] }}</td>
                    <td>{{ evento[2] }}</td>
                    <td>{{ evento[3] }}</td>
                </tr>
                {% endfor %}
            </table>
        </div>
    </body>
    </html>
    """

    return render_template_string(
        html,
        estado_led=estado_led,
        modo=modo,
        eventos=eventos
    )


@app.route("/banco")
def banco():
    eventos = listar_eventos(1000)

    html = """
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="UTF-8">
        <title>Banco de Dados</title>

        <style>
            body {
                font-family: Arial, sans-serif;
                background: #f2f2f2;
                margin: 40px;
            }

            .card {
                background: white;
                max-width: 900px;
                margin: auto;
                padding: 25px;
                border-radius: 12px;
                box-shadow: 0 2px 8px rgba(0,0,0,0.15);
            }

            h1 {
                text-align: center;
            }

            a {
                display: inline-block;
                margin-bottom: 20px;
                color: #007bff;
                text-decoration: none;
            }

            table {
                width: 100%;
                border-collapse: collapse;
                margin-top: 20px;
            }

            th, td {
                padding: 8px;
                border-bottom: 1px solid #ddd;
                text-align: left;
            }

            th {
                background: #f0f0f0;
            }
        </style>
    </head>

    <body>
        <div class="card">
            <h1>Registros do Banco de Dados</h1>

            <a href="/">← Voltar para o painel</a>

            <table>
                <tr>
                    <th>ID</th>
                    <th>Tipo</th>
                    <th>Valor</th>
                    <th>Data/Hora</th>
                </tr>

                {% for evento in eventos %}
                <tr>
                    <td>{{ evento[0] }}</td>
                    <td>{{ evento[1] }}</td>
                    <td>{{ evento[2] }}</td>
                    <td>{{ evento[3] }}</td>
                </tr>
                {% endfor %}
            </table>
        </div>
    </body>
    </html>
    """

    return render_template_string(html, eventos=eventos)


@app.route("/ligar")
def ligar():
    global estado_led, ultimo_estado, modo

    if modo == "AUTO":
        return redirect("/")

    estado_led = "ON"
    ultimo_estado = "ON"

    salvar_evento("WEB_CMD", "ON")
    enviar_serial("CMD:ON")

    return redirect("/")


@app.route("/desligar")
def desligar():
    global estado_led, ultimo_estado, modo

    if modo == "AUTO":
        return redirect("/")

    estado_led = "OFF"
    ultimo_estado = "OFF"

    salvar_evento("WEB_CMD", "OFF")
    enviar_serial("CMD:OFF")

    return redirect("/")


@app.route("/modo/auto")
def modo_auto():
    global modo

    if modo != "AUTO":
        modo = "AUTO"
        salvar_evento("MODE", "AUTO")
        enviar_serial("MODE:AUTO")

    return redirect("/")


@app.route("/modo/manual")
def modo_manual():
    global modo

    if modo != "MANUAL":
        modo = "MANUAL"
        salvar_evento("MODE", "MANUAL")
        enviar_serial("MODE:MANUAL")

    return redirect("/")


if __name__ == "__main__":
    init_db()
    abrir_serial()

    thread = threading.Thread(target=leitor_serial, daemon=True)
    thread.start()

    app.run(host="0.0.0.0", port=5000, debug=False)