#include <ps5Controller.h>

/*
  Tanque com 3 “pontes H” acionadas por RELÉS (2 relés por motor: IN1/IN2)
  - Esteira esquerda: 2 relés (DIR A / DIR B)
  - Esteira direita: 2 relés (DIR A / DIR B)
  - Torre: 2 relés (DIR A / DIR B)
  - Farol: 1 relé (toggle no L2)
  - R2: 1 relé pulsado por 1s (SÓ após liberar sequência X -> Bola -> Triângulo -> Quadrado)

  Segurança:
  - Se perder conexão do controle: DESLIGA TUDO imediatamente.
  - Se continuar sem conexão por 15s: RESETA o ESP32.
*/

// ======================= PINOS (AJUSTE AQUI) =======================
#define LED_PIN 27

// Esteira ESQUERDA (ponte H via relés)
#define REL_LEFT_IN1  4
#define REL_LEFT_IN2  5

// Esteira DIREITA (ponte H via relés)
#define REL_RIGHT_IN1 18
#define REL_RIGHT_IN2 19

// TORRE (ponte H via relés)
#define REL_TURRET_IN1 21
#define REL_TURRET_IN2 22

// FAROL
#define REL_FARO  23

// RELÉ DE PULSO (R2 após liberar)
#define REL_PULSE  25

// ======================= CONFIG =======================
const uint32_t UNLOCK_STEP_TIMEOUT_MS = 5000;   // tempo máximo entre etapas X -> O -> △ -> □
const uint32_t PULSE_MS = 1000;                 // pulso do relé
const uint32_t LED_BLINK_MS = 250;              // pisca do LED enquanto desconectado
const uint32_t DISCONNECT_RESET_MS = 15000;     // reset após 15s sem controle

// Se seus relés forem “ativo em LOW”, mude para LOW/HIGH abaixo.
const uint8_t REL_ON  = HIGH;
const uint8_t REL_OFF = LOW;

// ======================= ESTADOS =======================
bool farolOn = false;

// bordas (edge detect)
bool prevL2 = false;
bool prevR2 = false;

bool prevX = false;
bool prevCircle = false;
bool prevTriangle = false;
bool prevSquare = false;

// Unlock sequence state
enum UnlockStep { WAIT_X, WAIT_CIRCLE, WAIT_TRIANGLE, WAIT_SQUARE, UNLOCKED };
UnlockStep unlockStep = WAIT_X;
uint32_t unlockLastStepMs = 0;

// Pulso do relé (sem delay bloqueante)
bool pulseActive = false;
uint32_t pulseStartMs = 0;

// Controle de desconexão
uint32_t disconnectStartMs = 0;
uint32_t lastLedBlinkMs = 0;
bool ledState = false;

// ======================= HELPERS =======================
void relayWrite(uint8_t pin, bool on) {
  digitalWrite(pin, on ? REL_ON : REL_OFF);
}

void stopMotor(uint8_t in1, uint8_t in2) {
  relayWrite(in1, false);
  relayWrite(in2, false);
}

// dir: -1 = ré, 0 = stop, +1 = frente
void setMotor(uint8_t in1, uint8_t in2, int dir) {
  if (dir == 0) {
    stopMotor(in1, in2);
  } else if (dir > 0) { // frente
    relayWrite(in1, true);
    relayWrite(in2, false);
  } else { // ré
    relayWrite(in1, false);
    relayWrite(in2, true);
  }
}

void stopAllRelays() {
  stopMotor(REL_LEFT_IN1, REL_LEFT_IN2);
  stopMotor(REL_RIGHT_IN1, REL_RIGHT_IN2);
  stopMotor(REL_TURRET_IN1, REL_TURRET_IN2);

  farolOn = false;
  relayWrite(REL_FARO, false);

  relayWrite(REL_PULSE, false);
  pulseActive = false;

  // reset sequência
  unlockStep = WAIT_X;
  unlockLastStepMs = 0;

  // reset edges
  prevL2 = prevR2 = false;
  prevX = prevCircle = prevTriangle = prevSquare = false;
}

bool risingEdge(bool now, bool &prev) {
  bool rise = (now && !prev);
  prev = now;
  return rise;
}

void resetUnlockIfTimeout() {
  if (unlockStep != WAIT_X && unlockStep != UNLOCKED) {
    if (millis() - unlockLastStepMs > UNLOCK_STEP_TIMEOUT_MS) {
      unlockStep = WAIT_X;
    }
  }
}

void handleUnlockSequence(bool xNow, bool circleNow, bool triNow, bool squareNow) {
  resetUnlockIfTimeout();

  bool xRise = risingEdge(xNow, prevX);
  bool cRise = risingEdge(circleNow, prevCircle);
  bool tRise = risingEdge(triNow, prevTriangle);
  bool sRise = risingEdge(squareNow, prevSquare);

  switch (unlockStep) {
    case WAIT_X:
      if (xRise) {
        unlockStep = WAIT_CIRCLE;
        unlockLastStepMs = millis();
      }
      break;

    case WAIT_CIRCLE:
      if (cRise) {
        unlockStep = WAIT_TRIANGLE;
        unlockLastStepMs = millis();
      }
      break;

    case WAIT_TRIANGLE:
      if (tRise) {
        unlockStep = WAIT_SQUARE;
        unlockLastStepMs = millis();
      }
      break;

    case WAIT_SQUARE:
      // Quadrado nessa etapa só LIBERA o R2
      if (sRise) {
        unlockStep = UNLOCKED;
        unlockLastStepMs = millis();
        Serial.println("Sequencia liberada! Agora R2 pulsa o rele por 1s.");
      }
      break;

    case UNLOCKED:
      // nada
      break;
  }
}

void handlePulseRelayByR2(bool r2Now) {
  // Só pode pulsar se estiver UNLOCKED
  if (unlockStep != UNLOCKED) return;

  bool r2Rise = risingEdge(r2Now, prevR2);

  if (r2Rise && !pulseActive) {
    pulseActive = true;
    pulseStartMs = millis();
    relayWrite(REL_PULSE, true);
    Serial.println("R2: pulso rele (1s)");
  }

  if (pulseActive && (millis() - pulseStartMs >= PULSE_MS)) {
    pulseActive = false;
    relayWrite(REL_PULSE, false);
  }
}

void handleFarolToggleByL2(bool l2Now) {
  if (risingEdge(l2Now, prevL2)) {
    farolOn = !farolOn;
    relayWrite(REL_FARO, farolOn);
    Serial.println(farolOn ? "Farol: LIGADO" : "Farol: DESLIGADO");
  }
}

void handleDisconnectedState() {
  // segurança imediata
  stopAllRelays();

  // marca início da desconexão
  if (disconnectStartMs == 0) {
    disconnectStartMs = millis();
    lastLedBlinkMs = millis();
    ledState = false;
    digitalWrite(LED_PIN, ledState);
    Serial.println("Controle desconectado! Desligando tudo e aguardando reconexao...");
  }

  // pisca LED (não-bloqueante)
  if (millis() - lastLedBlinkMs >= LED_BLINK_MS) {
    lastLedBlinkMs = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }

  // reset após 15s sem controle
  if (millis() - disconnectStartMs >= DISCONNECT_RESET_MS) {
    Serial.println("Sem controle por 15s. Reiniciando ESP32...");
    ESP.restart();
  }
}

// ======================= SETUP/LOOP =======================
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);

  pinMode(REL_LEFT_IN1, OUTPUT);
  pinMode(REL_LEFT_IN2, OUTPUT);

  pinMode(REL_RIGHT_IN1, OUTPUT);
  pinMode(REL_RIGHT_IN2, OUTPUT);

  pinMode(REL_TURRET_IN1, OUTPUT);
  pinMode(REL_TURRET_IN2, OUTPUT);

  pinMode(REL_FARO, OUTPUT);
  pinMode(REL_PULSE, OUTPUT);

  stopAllRelays();
  digitalWrite(LED_PIN, LOW);

  // Substitua pelo MAC do seu controle
  ps5.begin("58:10:31:bd:52:38");
  Serial.println("Pronto para conectar ao controle PS5...");
}

void loop() {
  if (!ps5.isConnected()) {
    handleDisconnectedState();
    return;
  }

  // voltou a conectar
  if (disconnectStartMs != 0) {
    Serial.println("Controle reconectado!");
    disconnectStartMs = 0;
    ledState = true;
  }

  // Conectado: LED aceso
  digitalWrite(LED_PIN, HIGH);

  // ======================= LEITURA DE BOTÕES =======================
  bool up    = ps5.Up();
  bool down  = ps5.Down();
  bool left  = ps5.Left();
  bool right = ps5.Right();

  bool L1 = ps5.L1();
  bool R1 = ps5.R1();

  bool L2 = ps5.L2();          // toggle farol
  bool R2 = ps5.R2();          // pulso 1s (após unlock)

  bool X  = ps5.Cross();       // X
  bool O  = ps5.Circle();      // Bola
  bool T  = ps5.Triangle();    // Triângulo
  bool SQ = ps5.Square();      // Quadrado (libera)

  // ======================= FAROL (L2 toggle) =======================
  handleFarolToggleByL2(L2);

  // ======================= SEQUÊNCIA DE LIBERAÇÃO =======================
  handleUnlockSequence(X, O, T, SQ);

  // ======================= PULSO (R2 após liberar) =======================
  handlePulseRelayByR2(R2);

  // ======================= CONTROLE DAS ESTEIRAS =======================
  // Prioridade: CIMA/BAIXO (duas esteiras) > ESQ/DIR (uma esteira)
  int leftDir = 0;
  int rightDir = 0;

  if (up && !down) {
    leftDir = +1;
    rightDir = +1;
  } else if (down && !up) {
    leftDir = -1;
    rightDir = -1;
  } else if (left && !right) {
    // seta esquerda move a esteira direita (pra frente)
    leftDir = 0;
    rightDir = +1;
  } else if (right && !left) {
    // seta direita move a esteira esquerda (pra frente)
    leftDir = +1;
    rightDir = 0;
  } else {
    leftDir = 0;
    rightDir = 0;
  }

  setMotor(REL_LEFT_IN1, REL_LEFT_IN2, leftDir);
  setMotor(REL_RIGHT_IN1, REL_RIGHT_IN2, rightDir);

  // ======================= CONTROLE DA TORRE =======================
  // L1 esquerda, R1 direita (se ambos, para)
  int turretDir = 0;
  if (L1 && !R1) turretDir = -1;
  else if (R1 && !L1) turretDir = +1;
  else turretDir = 0;

  setMotor(REL_TURRET_IN1, REL_TURRET_IN2, turretDir);
}
