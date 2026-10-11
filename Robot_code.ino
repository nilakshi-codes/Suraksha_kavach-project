#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESP32Servo.h>

// Wi-Fi Local Hotspot Network Credentials (For watching the dashboard)
const char* ssid = "Vivo v50 (@mon)";
const char* password = "manamidey26jan";

// L298N Motor Driver Pins
const int IN1 = 12; const int IN2 = 13; const int IN3 = 26; const int IN4 = 27;

// Ultrasonic Sensor Pins
const int TRIG_PIN = 5; const int ECHO_PIN = 18;

// Servo Configuration
Servo myServo;
const int SERVO_PIN = 19;

// State Machine and Timing Variables
enum RobotState { DRIVE_FORWARD, PROCESS_STOP, PROCESS_REVERSE, PROCESS_SCAN, CHOOSE_PATH, FINAL_PIVOT };
RobotState currentState = DRIVE_FORWARD;

unsigned long stateStartTime = 0;
unsigned long lastSensorReadTime = 0;
const unsigned long SENSOR_INTERVAL = 60; // Read distance smoothly every 60ms

const int OBSTACLE_DIST = 25; // Stop threshold in centimeters
int distance = 0;
int rightDistance = 0;
int leftDistance = 0;

String obstacleStatus = "Clear";
String robotStateStr = "FORWARD";

AsyncWebServer server(80);

// --- FORWARD DECLARATIONS ---
void moveForward();
void moveBackward();
void turnRight();
void turnLeft();
void moveStop();

// Pure Telemetry HTML Dashboard Code (No manual control buttons, read-only tracking)
const char index_html[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
  <title>GuardianMesh Telemetry Hub</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #0f172a; color: #ffffff; text-align: center; margin: 0; padding: 20px; }
    h1 { color: #00e6ff; margin-bottom: 5px; font-weight: 600; font-size: 26px; }
    p { color: #64748b; margin-bottom: 40px; font-size: 14px; }
    .container { display: flex; flex-wrap: wrap; justify-content: center; gap: 20px; max-width: 900px; margin: 0 auto; }
    .card { background: #1e293b; border-radius: 16px; padding: 25px; width: 240px; box-shadow: 0 10px 25px rgba(0,0,0,0.4); border-top: 6px solid #475569; transition: 0.3s; }
    .card h3 { margin: 0 0 15px 0; color: #94a3b8; text-transform: uppercase; font-size: 12px; letter-spacing: 1px; }
    .value { font-size: 28px; font-weight: 800; }
    .unit { font-size: 16px; color: #475569; margin-left: 4px; }
    .text-danger { color: #f43f5e; }
    .text-success { color: #10b981; }
  </style>
</head>
<body>
  <h1>🤖 GuardianMesh Autonomous Tracking</h1>
  <p>100% Self-Driving Mission Status Telemetry Hub</p>
  
  <div class="container">
    <div class="card" id="obsCard"><h3>Proximity Status</h3><div class="value" id="obstacle">Loading...</div></div>
    <div class="card" id="distCard"><h3>Sonar Clearance</h3><div class="value"><span id="distanceValue">0</span><span class="unit">cm</span></div></div>
    <div class="card" id="modeCard"><h3>Current Action</h3><div class="value" id="driveState" style="color: #c084fc;">FORWARD</div></div>
  </div>

  <script>
    setInterval(function() {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          const obsElem = document.getElementById("obstacle");
          obsElem.innerText = data.obstacle;
          if(data.obstacle === "OBSTACLE DETECTED") {
            obsElem.className = "value text-danger";
            document.getElementById("obsCard").style.borderTopColor = "#f43f5e";
          } else {
            obsElem.className = "value text-success";
            document.getElementById("obsCard").style.borderTopColor = "#10b981";
          }
          document.getElementById("distanceValue").innerText = data.distance;
          document.getElementById("distCard").style.borderTopColor = (parseInt(data.distance) <= 25) ? "#f43f5e" : "#10b981";
          document.getElementById("driveState").innerText = data.movement;
        });
    }, 200);
  </script>
</body>
</html>
)rawhtml";

void setup() {
  Serial.begin(115200);

  // Initialize Hardware Output Pins
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT); pinMode(ECHO_PIN, INPUT);
  
  myServo.attach(SERVO_PIN);
  myServo.write(90); // Center sensor mount
  
  // Connect to Wi-Fi to serve the monitoring webpage
  Serial.print("Connecting to Wi-Fi Network: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\n----------------------------------------------");
  Serial.println(" SUCCESS: Wi-Fi Live Link Ready!");
  Serial.print(" Open Browser To URL: http://"); Serial.println(WiFi.localIP());
  Serial.println("----------------------------------------------");

  // Server Routing Endpoints
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", index_html);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"obstacle\":\"" + obstacleStatus + "\",";
    json += "\"distance\":" + String(distance) + ",";
    json += "\"movement\":\"" + robotStateStr + "\"";
    json += "}";
    request->send(200, "application/json", json);
  });

  server.begin();
  
  // Initialize automatic state variables
  currentState = DRIVE_FORWARD;
  stateStartTime = millis();
}

void loop() {
  unsigned long currentTime = millis();

  // Polling block: Continuously reads distance via sensor in background
  if (currentTime - lastSensorReadTime >= SENSOR_INTERVAL) {
    lastSensorReadTime = currentTime;
    digitalWrite(TRIG_PIN, LOW);   delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);  delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH, 30000); 
    int freshDist = duration * 0.034 / 2;
    distance = (freshDist == 0) ? 250 : freshDist;
  }

  // 100% Autonomous Navigation State Machine Layout
  switch (currentState) {
    case DRIVE_FORWARD:
      obstacleStatus = "Path Clear";
      robotStateStr = "FORWARD";
      moveForward();
      if (distance <= OBSTACLE_DIST) {
        obstacleStatus = "OBSTACLE DETECTED";
        robotStateStr = "STOPPED";
        moveStop();
        currentState = PROCESS_STOP;
        stateStartTime = currentTime;
      }
      break;

    case PROCESS_STOP:
      if (currentTime - stateStartTime >= 300) { 
        robotStateStr = "REVERSING";
        moveBackward();
        currentState = PROCESS_REVERSE;
        stateStartTime = currentTime;
      }
      break;

    case PROCESS_REVERSE:
      if (currentTime - stateStartTime >= 400) { 
        robotStateStr = "SCANNING PATHS";
        moveStop();
        myServo.write(15); // Turn head right
        currentState = PROCESS_SCAN;
        stateStartTime = currentTime;
      }
      break;

    case PROCESS_SCAN:
      if (currentTime - stateStartTime == 400) {
        rightDistance = distance;
        myServo.write(165); // Turn head left
      }
      if (currentTime - stateStartTime >= 800) {
        leftDistance = distance;
        myServo.write(90); // Re-center head
        currentState = CHOOSE_PATH;
      }
      break;

    case CHOOSE_PATH:
      if (rightDistance >= leftDistance) {
        robotStateStr = "AUTONOMOUS RIGHT";
        turnRight();
      } else {
        robotStateStr = "AUTONOMOUS LEFT";
        turnLeft();
      }
      currentState = FINAL_PIVOT;
      stateStartTime = currentTime;
      break;

    case FINAL_PIVOT:
      if (currentTime - stateStartTime >= 500) { 
        moveStop();
        distance = 250; 
        currentState = DRIVE_FORWARD;
      }
      break;
  }
}

// Locomotion Drivers
void moveForward() { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW); }
void moveBackward() { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
void turnRight() { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
void turnLeft() { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW); }
void moveStop() { digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);  digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW); }