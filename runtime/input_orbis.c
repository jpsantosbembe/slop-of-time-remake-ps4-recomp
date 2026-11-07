// ocarina-ps4: a entrada do PS4. Duas metades:
// - o gamepad: a fatia do SDL3 que input.c usa (input_pad_orbis.h), sobre scePad. Um controle, o do usuário que abriu
//   o app; input.c chama tudo sob o lock dele, então o estado aqui não tem trava própria;
// - o teclado e o mouse do sistema de janelas (input_wsys.h e o resto de input.h que input_sdl.c dá no PC): o PS4 não
//   tem, então nenhuma tecla está apertada e o cursor nunca é capturado.
#ifdef __ORBIS__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_pad_orbis.h"
#include "input_wsys.h"
#include "platform/platform.h"

// scePad e sceUserService com as assinaturas do SDK (orbis/Pad.h, orbis/UserService.h); o OrbisPadData do SDK
// (orbis/_types/pad.h) tem o layout que a libScePad preenche.
#include <orbis/_types/pad.h>
extern int32_t scePadInit(void);
extern int32_t scePadOpen(int32_t user, int32_t type, int32_t index, void *param);
extern int32_t scePadClose(int32_t handle);
extern int32_t scePadReadState(int32_t handle, OrbisPadData *data);
extern int32_t scePadSetVibration(int32_t handle, const OrbisPadVibeParam *param);
extern int32_t sceUserServiceInitialize(void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *user);

enum { PAD_ID = 1 };   // o único SDL_JoystickID que este lado dá

static struct {
    int inited;                // scePadInit e o usuário: 1 feito, -1 falhou
    int32_t user, handle;      // handle < 0: fechado
    int open;                  // SDL_OpenGamepad devolveu o controle
    OrbisPadData st;           // a última leitura (SDL_UpdateGamepads)
    int connected;
    uint64_t rumble_until;     // plat_now_ns em que a vibração pedida acaba; 0 parada
    char err[96];
} P = {.handle = -1};

static void set_err(const char *what, int32_t e) { snprintf(P.err, sizeof P.err, "%s: %#x", what, (unsigned)e); }

bool SDL_Init(uint32_t flags) {
    (void)flags;
    if (P.inited) return P.inited > 0;
    P.inited = -1;
    int32_t e = scePadInit();
    if (e < 0) return set_err("scePadInit", e), false;
    struct { uint32_t priority; } up = {0x2ff};   // OrbisUserServiceInitializeParams, prioridade FIFO mais baixa
    sceUserServiceInitialize(&up);               // já iniciado por outra parte do processo: segue igual
    if ((e = sceUserServiceGetInitialUser(&P.user)) < 0) return set_err("sceUserServiceGetInitialUser", e), false;
    P.inited = 1;
    return true;
}

const char *SDL_GetError(void) { return P.err[0] ? P.err : "scePad"; }
void SDL_free(void *p) { free(p); }

// Tenta abrir o controle do usuário a cada varredura de input.c (RESCAN_MS) enquanto não houver um conectado.
static void try_open(void) {
    if (P.inited <= 0) return;
    if (P.handle < 0) {
        int32_t h = scePadOpen(P.user, 0, 0, NULL);   // ORBIS_PAD_PORT_TYPE_STANDARD, índice 0
        if (h < 0) { set_err("scePadOpen", h); return; }
        P.handle = h;
    }
    memset(&P.st, 0, sizeof P.st);
    P.connected = scePadReadState(P.handle, &P.st) >= 0 && P.st.connected;
}

SDL_JoystickID *SDL_GetGamepads(int *count) {
    *count = 0;
    try_open();
    if (!P.connected) return NULL;
    SDL_JoystickID *ids = malloc(2 * sizeof *ids);
    if (!ids) return NULL;
    ids[0] = PAD_ID, ids[1] = 0;
    *count = 1;
    return ids;
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    if (id != PAD_ID || !P.connected) return NULL;
    P.open = 1;
    return (SDL_Gamepad *)&P;
}

// O handle do scePad fica aberto: o controle volta pelo mesmo handle quando reconecta.
void SDL_CloseGamepad(SDL_Gamepad *g) {
    (void)g;
    P.open = 0;
}

bool SDL_GamepadConnected(SDL_Gamepad *g) { return g && P.open && P.connected; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *g) { return g ? PAD_ID : 0; }
const char *SDL_GetGamepadName(SDL_Gamepad *g) { (void)g; return "DualShock 4 (scePad)"; }
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *g) { return g ? 1 : 0; }

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value) {
    if (props && !strcmp(name, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN)) return true;
    return default_value;
}

SDL_GamepadButtonLabel SDL_GetGamepadButtonLabel(SDL_Gamepad *g, SDL_GamepadButton b) {
    (void)g;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return SDL_GAMEPAD_BUTTON_LABEL_CROSS;
    case SDL_GAMEPAD_BUTTON_EAST: return SDL_GAMEPAD_BUTTON_LABEL_CIRCLE;
    case SDL_GAMEPAD_BUTTON_WEST: return SDL_GAMEPAD_BUTTON_LABEL_SQUARE;
    case SDL_GAMEPAD_BUTTON_NORTH: return SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE;
    default: return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    }
}

static void vibrate(uint8_t low, uint8_t high) {
    OrbisPadVibeParam v = {.lgMotor = low, .smMotor = high};
    if (P.handle >= 0) scePadSetVibration(P.handle, &v);
}

// Uma leitura por chamada (input.c chama a cada poll e na thread de movimento, a 250 Hz); também encerra a vibração
// vencida, como o SDL faz no seu update.
void SDL_UpdateGamepads(void) {
    if (P.handle < 0) return;
    OrbisPadData d;
    if (scePadReadState(P.handle, &d) >= 0) {
        P.st = d;
        P.connected = d.connected != 0;
    } else {
        P.connected = 0;
    }
    if (P.rumble_until && plat_now_ns() >= P.rumble_until) {
        P.rumble_until = 0;
        vibrate(0, 0);
    }
}

static uint32_t button_bit(SDL_GamepadButton b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ORBIS_PAD_BUTTON_CROSS;
    case SDL_GAMEPAD_BUTTON_EAST: return ORBIS_PAD_BUTTON_CIRCLE;
    case SDL_GAMEPAD_BUTTON_WEST: return ORBIS_PAD_BUTTON_SQUARE;
    case SDL_GAMEPAD_BUTTON_NORTH: return ORBIS_PAD_BUTTON_TRIANGLE;
    case SDL_GAMEPAD_BUTTON_BACK: return ORBIS_PAD_BUTTON_TOUCH_PAD;   // o Minus do Switch: o clique do touchpad
    case SDL_GAMEPAD_BUTTON_START: return ORBIS_PAD_BUTTON_OPTIONS;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ORBIS_PAD_BUTTON_L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ORBIS_PAD_BUTTON_R3;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ORBIS_PAD_BUTTON_L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ORBIS_PAD_BUTTON_R1;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ORBIS_PAD_BUTTON_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ORBIS_PAD_BUTTON_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ORBIS_PAD_BUTTON_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ORBIS_PAD_BUTTON_RIGHT;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return ORBIS_PAD_BUTTON_TOUCH_PAD;
    default: return 0;   // guide (o botão PS é do sistema), paddles: não existem no DS4
    }
}

bool SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton b) {
    return g && P.connected && (P.st.buttons & button_bit(b)) != 0;
}

// Os eixos do DS4 vão de 0 a 255 com o centro em 128 e +y para baixo, como os do SDL: só a escala muda. Os gatilhos
// analógicos (L2, R2) vão de 0 a 255.
static Sint16 stick_axis(uint8_t v) {
    int x = ((int)v - 128) * 32767 / 127;
    return (Sint16)(x < -32768 ? -32768 : x > 32767 ? 32767 : x);
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis a) {
    if (!g || !P.connected) return 0;
    switch (a) {
    case SDL_GAMEPAD_AXIS_LEFTX: return stick_axis(P.st.leftStick.x);
    case SDL_GAMEPAD_AXIS_LEFTY: return stick_axis(P.st.leftStick.y);
    case SDL_GAMEPAD_AXIS_RIGHTX: return stick_axis(P.st.rightStick.x);
    case SDL_GAMEPAD_AXIS_RIGHTY: return stick_axis(P.st.rightStick.y);
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return (Sint16)(P.st.analogButtons.l2 * 32767 / 255);
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return (Sint16)(P.st.analogButtons.r2 * 32767 / 255);
    default: return 0;
    }
}

bool SDL_RumbleGamepad(SDL_Gamepad *g, uint16_t low, uint16_t high, uint32_t ms) {
    if (!g || P.handle < 0) return false;
    vibrate((uint8_t)(low >> 8), (uint8_t)(high >> 8));
    P.rumble_until = (low || high) && ms ? plat_now_ns() + (uint64_t)ms * 1000000u : 0;
    return true;
}

bool SDL_RumbleGamepadTriggers(SDL_Gamepad *g, uint16_t l, uint16_t r, uint32_t ms) {
    (void)g, (void)l, (void)r, (void)ms;
    return false;   // o DS4 não tem motores nos gatilhos
}

// Sensores: ainda não. O OrbisPadData traz aceleração e velocidade angular, mas a orientação dos eixos do DS4 em
// relação aos do SDL não foi medida; até lá o sensor de seis eixos do jogo lê o controle parado.
bool SDL_GamepadHasSensor(SDL_Gamepad *g, SDL_SensorType type) { (void)g, (void)type; return false; }
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *g, SDL_SensorType type, bool on) { (void)g, (void)type, (void)on; return false; }
float SDL_GetGamepadSensorDataRate(SDL_Gamepad *g, SDL_SensorType type) { (void)g, (void)type; return 0; }
bool SDL_GetGamepadSensorData(SDL_Gamepad *g, SDL_SensorType type, float *data, int n) {
    (void)g, (void)type, (void)data, (void)n;
    return false;
}
void SDL_SetEventEnabled(uint32_t type, bool on) { (void)type, (void)on; }

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *d) {
    (void)d;
    snprintf(P.err, sizeof P.err, "sem controle virtual no PS4");
    return 0;
}
SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID id) { (void)id; return NULL; }
bool SDL_SetJoystickVirtualAxis(SDL_Joystick *j, int axis, Sint16 value) { (void)j, (void)axis, (void)value; return false; }

// ---- teclado e mouse (input_wsys.h e input.h): o PS4 não tem
int ikb_foreground(void) { return 0; }
int ikb_down(int key) { (void)key; return 0; }
int ikb_escape(void) { return 0; }
int ikb_captured(void) { return 0; }
void ikb_mouse_take(int32_t *dx, int32_t *dy) { *dx = *dy = 0; }
void ikb_cursor_update(void) {}
void ikb_menu_closed(void) {}
uint32_t input_key_char(int key) { (void)key; return 0; }
int input_flight_key(void) { return 0; }
void input_cursor_suspend(int on) { (void)on; }
int input_cursor_released(void) { return 1; }
#endif
