#include <WiFi.h>
#include <WebServer.h>
// --- Incluir librerías para WebSockets y JSON ---
#include <ArduinoJson.h> // Para manejar JSON
#include <WebSockets_Generic.h> // Usamos el nombre del archivo de cabecera correcto

// Define el nombre de la red (SSID) y la contraseña para el AP del ESP32
const char* ap_ssid = "MiESP32_AP";
const char* ap_password = "micontrasenasegura";

// Creamos un objeto servidor web en el puerto 80
WebServer server(80);

// Variables para almacenar las credenciales de la red Wi-Fi a la que el ESP32 se conectará
String target_ssid = "";
String target_password = "";
bool credentialsReceived = false; // Bandera para saber si ya recibimos las credenciales

// --- Configuración de WebSockets Cliente ---
// URL base de tu API en la nube. **CAMBIAR ESTO CUANDO TENGAS LA URL DE RENDER**
const char* websocketHost = "TU_DIRECCION_IP_O_DOMINIO_DE_RENDER"; // Ejemplo: "misplantaapi.render.com"
const int websocketPort = 443; // Puerto (443 para wss://, 80 para ws://)
const char* websocketPath = "/myhub"; // Ruta al Hub de SignalR

// Creamos un objeto cliente de WebSockets
WebSockets webSocket; // <--- ¡CAMBIO AQUÍ! Usamos el tipo 'WebSockets'

// Bandera para saber si el cliente WebSocket esta conectado y si la negociacion SignalR se completo
bool isWebSocketConnected = false;
bool isSignalRNegotiated = false; // Bandera para el estado del protocolo SignalR

// --- Constantes y Variables para el Protocolo SignalR ---
// El protocolo SignalR inicia con una peticion GET de negociacion HTTP
const char* signalrNegotiatePath = "/myhub/negotiate?negotiateVersion=1"; // Ruta de negociacion
// El ID de conexion que nos da la negociacion (necesario para la conexion WebSocket)
String signalrConnectionId = "";

// --- Declaraciones de funciones (Implementadas mas abajo) ---
void handleConfig();
void handleRoot();
void connectToWiFi();
void connectToSignalR(); // Esta funcion ahora inicia la negociacion HTTP
void startWebSocket(const char* connectionToken); // Inicia la conexion WebSocket despues de negociar
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length); // Manejador de eventos de WebSocket
void sendSignalRInvocation(const char* targetMethod, JsonDocument& arguments); // Ayudante para enviar mensajes a la API


void setup() {
  Serial.begin(115200);
  delay(10);
  Serial.println();

  WiFi.mode(WIFI_AP_STA);

  Serial.print("Configurando Punto de Acceso ");
  Serial.print(ap_ssid);

  WiFi.softAP(ap_ssid, ap_password);

  Serial.println(" listo.");
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("IP del AP: ");
  Serial.println(apIP);

  server.on("/config", HTTP_GET, handleConfig);
  server.on("/", HTTP_GET, handleRoot);
  server.begin();
  Serial.println("Servidor HTTP iniciado");

  Serial.println("Esperando credenciales WiFi...");

  // Configurar el manejador de eventos de WebSocket
  webSocket.onEvent(webSocketEvent); // <--- Usamos el objeto 'webSocket'
  // Para SSL/TLS (wss://) necesitas configurar la huella digital del certificado o deshabilitar la validacion (NO recomendado en produccion)
  // webSocket.setExtraHeaders("Origin: ws://localhost"); // Puede ser necesario depending on server config
}

void loop() {
  server.handleClient();

  // Ejecutar el loop de WebSocket para procesar la comunicacion
  if (WiFi.status() == WL_CONNECTED) {
    webSocket.loop(); // <--- Usamos el objeto 'webSocket'
    // Añadir un pequeño delay para evitar saturar el procesador del ESP32
    delay(1);
  } else {
      if (credentialsReceived) { // Mantenemos la logica de intentar conectar a WiFi si se recibieron credenciales
          connectToWiFi();
          credentialsReceived = false;
      }
  }

  // Otras tareas del ESP32
}

// --- Funciones manejadoras del servidor web ---
// (Sin cambios)

void handleConfig() {
  Serial.println("Recibida peticion en /config");
  if (server.hasArg("ssid") && server.hasArg("password")) {
    target_ssid = server.arg("ssid");
    target_password = server.arg("password");
    Serial.print("SSID recibido: "); Serial.println(target_ssid);
    Serial.print("Password recibido: "); Serial.println("********");
    credentialsReceived = true;
    server.send(200, "text/plain", "Credenciales recibidas. Intentando conectar...");
  } else {
    server.send(400, "text/plain", "Faltan parametros 'ssid' o 'password'");
  }
}

void handleRoot() {
  server.send(200, "text/plain", "ESP32 en modo AP. Conectate y ve a /config?ssid=TU_SSID&password=TU_PASSWORD");
}

// --- Función para conectar al WiFi ---
// (Sin cambios significativos)

void connectToWiFi() {
  Serial.print("Intentando conectar a WiFi: "); Serial.println(target_ssid);
  server.stop();
  Serial.println("Servidor HTTP detenido.");
  WiFi.disconnect();
  WiFi.begin(target_ssid.c_str(), target_password.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500); Serial.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado!");
    Serial.print("IP del ESP32 en la red local: "); Serial.println(WiFi.localIP());
    WiFi.softAPdisconnect(true);
    Serial.println("Modo AP desactivado.");

    delay(1000);
    connectToSignalR();
  } else {
    Serial.println("\nError al conectar a WiFi.");
    Serial.println("Verifica el SSID y la contrasena.");
    Serial.println("Reiniciando en modo AP para reintentar la configuracion...");
    ESP.restart();
  }
}

// --- Función para iniciar la NEGOCIACION HTTP de SignalR ---
void connectToSignalR() {
  Serial.print("Iniciando negociacion SignalR via HTTP GET a: ");
  Serial.print(websocketHost); Serial.print(signalrNegotiatePath); Serial.println("...");

  WiFiClientSecure client; // Usar WiFiClientSecure para HTTPS (recomendado para Render)

  // *** Para HTTPS/SSL (wss://), necesitas validar el certificado ***
  // client.setInsecure(); // Deshabilita la validacion del certificado (NO SEGURO)
  // client.setFingerprint("YOUR_FINGERPRINT"); // Ejemplo para validacion por huella

  if (!client.connect(websocketHost, websocketPort)) {
    Serial.println("Error de conexion TCP/SSL para negociacion.");
    Serial.print("Verifica host ("); Serial.print(websocketHost);
    Serial.print("), puerto ("); Serial.print(websocketPort);
    Serial.println(") y si SSL es necesario.");
    return;
  }

  String request = "GET " + String(signalrNegotiatePath) + " HTTP/1.1\r\n" +
                   "Host: " + String(websocketHost) + "\r\n" +
                   "Connection: close\r\n\r\n";

  Serial.println("Enviando peticion HTTP de negociacion...");
  client.print(request);

  String response = "";
  unsigned long timeout = millis() + 5000;
  while (client.connected() && millis() < timeout) {
    while (client.available()) {
      response += client.read();
    }
  }
  client.stop();

  Serial.println("Respuesta de negociacion recibida:");
  Serial.println(response);

  int bodyStart = response.indexOf("\r\n\r\n");
  if (bodyStart == -1) {
    Serial.println("Error: No se encontro el final de los encabezados HTTP en la respuesta de negociacion.");
    return;
  }
  String jsonResponse = response.substring(bodyStart + 4);

  Serial.print("Cuerpo JSON de negociacion: "); Serial.println(jsonResponse);

  StaticJsonDocument doc; // Capacidad para el documento JSON
  DeserializationError error = deserializeJson(doc, jsonResponse);

  if (error) {
    Serial.print(F("Error al parsear JSON de negociacion: "));
    Serial.println(error.c_str());
    return;
  }

  const char* connectionId = doc["connectionId"];

  if (connectionId) {
    signalrConnectionId = connectionId;
    Serial.print("Negociacion SignalR exitosa! Connection ID: "); Serial.println(signalrConnectionId);

    // --- Ahora, iniciar la conexion WebSocket usando el connectionId ---
    startWebSocket(signalrConnectionId.c_str());

  } else {
    Serial.println("Error: No se pudo obtener connectionId en la respuesta de negociacion.");
    Serial.println("Verifica si la API de SignalR esta corriendo y accesible en /myhub/negotiate.");
     Serial.println("Verifica el codigo de respuesta HTTP y el cuerpo JSON.");
  }
}

// --- Función para iniciar la conexion WebSocket ---
void startWebSocket(const char* connectionId) {
    Serial.print("Iniciando conexion WebSocket a ");
    Serial.print(websocketHost); Serial.print(":"); Serial.print(websocketPort);
    Serial.print(websocketPath); Serial.print("?id="); Serial.println(connectionId);

    String websocketUrl = String(websocketPath) + "?id=" + String(connectionId);

    // Iniciar la conexion WebSocket
    // begin(host, port, path, protocol, extraHeaders, fingerprint, reconnectInterval)
    // Para SSL/TLS (wss), usa puerto 443.
    webSocket.begin(websocketHost, websocketPort, websocketUrl.c_str(), "", ""); // Últimos 2: protocol y extraHeaders (protocol vacio por ahora)

    // Si usas SSL, necesitas habilitar SSL/TLS en WebSocketsClient
    // Busca en la documentacion de WebSocketsClient: setSSL(), setSecure(), etc.
    // webSocket.setSecure(); // <-- Esto podría ser necesario para wss://

    Serial.println("Inicio de conexion WebSocket exitoso. Esperando eventos (WS_EVT_CONNECT, WS_EVT_DATA, etc.)...");
    isWebSocketConnected = true;
    isSignalRNegotiated = true;
}


// --- Manejador de eventos de WebSockets ---
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("WebSocket desconectado.");
            isWebSocketConnected = false;
            isSignalRNegotiated = false;
            break;

        case WStype_CONNECTED:
            Serial.print("WebSocket conectado a url: ");
            Serial.println((char*)payload);
            break;

        case WStype_TEXT:
            Serial.print("WebSocket Texto recibido: ");
            Serial.println((char*)payload);

            StaticJsonDocument doc; // Capacidad para el documento JSON
            DeserializationError error = deserializeJson(doc, (char*)payload);

            if (error) {
                Serial.print(F("Error al parsear JSON del mensaje WebSocket: "));
                Serial.println(error.c_str());
                return;
            }

            // Manejo basico de mensajes SignalR (Ping/Pong y Invocation)
            if (!doc.isNull()) {
                int messageType = doc["type"];

                if (messageType == 1) { // Type 1: Invocation (API llama a metodo en cliente)
                     const char* target = doc["target"];
                     JsonArray arguments = doc["arguments"].as<JsonArray>(); // Obtener array de argumentos

                     Serial.print("  SignalR Invocation - Target: "); Serial.println(target);

                     if (strcmp(target, "ReceiveMessage") == 0) {
                         if (arguments.size() == 2) {
                             String user = arguments[0].as<String>();
                             String message = arguments[1].as<String>();
                             Serial.print("    De: "); Serial.println(user);
                             Serial.print("    Mensaje: "); Serial.println(message);

                             if (user.equals("App")) {
                                 Serial.print("     Comando desde App: "); Serial.println(message);
                                 // if (message.equals("ON_LED")) { digitalWrite(LED_BUILTIN, HIGH); }
                                 // else if (message.equals("OFF_LED")) { digitalWrite(LED_BUILTIN, LOW); }
                             }
                         }
                     }
                     else if (strcmp(target, "ControlPin") == 0) {
                        if (arguments.size() == 2) {
                            int pin = arguments[0].as<int>();
                            int value = arguments[1].as<int>();
                            Serial.print("    Comando Pin: "); Serial.print(pin);
                            Serial.print("    Valor: "); Serial.println(value);
                            // digitalWrite(pin, value);
                        }
                     }

                } else if (messageType == 6) { // Type 6: Ping/Pong
                     Serial.println("SignalR Ping recibido."); // <--- CAMBIO Debug.println a Serial.println
                }
                // Puedes manejar otros tipos de mensajes SignalR si es necesario
            }
            break;

        case WStype_BIN:
            Serial.println("WebSocket Binario recibido.");
            break;

        case WStype_ERROR:
             Serial.print("WebSocket Error: "); Serial.println((char*)payload);
            isWebSocketConnected = false;
            isSignalRNegotiated = false;
            break;
    }
}

// --- Función ayudante para enviar una invocacion de metodo al Hub de la API ---
void sendSignalRInvocation(const char* targetMethod, JsonDocument& arguments) {
    if (!isWebSocketConnected) {
        Serial.println("Error: No conectado a WebSocket para enviar mensaje SignalR.");
        return;
    }

    StaticJsonDocument doc; // Capacidad para el documento JSON
    doc["target"] = targetMethod;
    doc["arguments"] = arguments; // JsonDocument pasada por referencia
    doc["type"] = 1; // Tipo de mensaje SignalR: 1 = Invocation

    String jsonMessage;
    serializeJson(doc, jsonMessage);

    Serial.print("Enviando mensaje SignalR: "); Serial.println(jsonMessage);
    webSocket.sendTXT(jsonMessage);
}