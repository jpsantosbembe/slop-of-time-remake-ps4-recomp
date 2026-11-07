// ocarina-ps4: a fatia da API de gamepad do SDL3 que runtime/input.c usa, implementada sobre o scePad do PS4 em
// runtime/input_orbis.c. Assim input.c (mapa de botões, zona morta, layout, scripts, vibração, menu) roda igual no PS4,
// só trocando o include: os nomes, os tipos e as convenções são os do SDL3 (eixos -32768..32767 com +y para baixo,
// gatilhos 0..32767, botões por posição). Não é o SDL: só o que input.c chama, com o mesmo contrato.
//
// O controle é o DualShock 4 do usuário que abriu o app (sceUserServiceGetInitialUser), aberto por scePadOpen. Rótulos
// PlayStation (cruz ao sul, círculo a leste), vibração pelos dois motores, sem paddles. Sensores de movimento: não
// expostos ainda (SDL_GamepadHasSensor responde falso, o sensor de seis eixos do jogo lê o controle parado). O
// controle virtual de teste (TOTK_TEST_PAD) não existe aqui.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef uint32_t SDL_JoystickID;
typedef uint32_t SDL_PropertiesID;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_Joystick SDL_Joystick;

#define SDL_INIT_GAMEPAD 0x2000u
#define SDL_STANDARD_GRAVITY 9.80665f
#define SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN "SDL.joystick.cap.rumble"
#define SDL_EVENT_GAMEPAD_SENSOR_UPDATE 0x659u

typedef enum {
    SDL_GAMEPAD_BUTTON_INVALID = -1,
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_MISC1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, SDL_GAMEPAD_BUTTON_TOUCHPAD,
    SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;

typedef enum {
    SDL_GAMEPAD_AXIS_INVALID = -1,
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    SDL_GAMEPAD_AXIS_COUNT
} SDL_GamepadAxis;

typedef enum {
    SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN, SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_LABEL_X,
    SDL_GAMEPAD_BUTTON_LABEL_Y, SDL_GAMEPAD_BUTTON_LABEL_CROSS, SDL_GAMEPAD_BUTTON_LABEL_CIRCLE,
    SDL_GAMEPAD_BUTTON_LABEL_SQUARE, SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE
} SDL_GamepadButtonLabel;

typedef enum { SDL_SENSOR_INVALID = -1, SDL_SENSOR_UNKNOWN, SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO } SDL_SensorType;
typedef enum { SDL_JOYSTICK_TYPE_UNKNOWN, SDL_JOYSTICK_TYPE_GAMEPAD } SDL_JoystickType;

// TOTK_TEST_PAD: o descritor existe para input.c compilar; SDL_AttachVirtualJoystick sempre falha no PS4.
typedef struct {
    uint32_t version;
    uint16_t type, padding, vendor_id, product_id, naxes, nbuttons;
    const char *name;
} SDL_VirtualJoystickDesc;
#define SDL_INIT_INTERFACE(iface) (*(iface) = (SDL_VirtualJoystickDesc){.version = sizeof *(iface)})

bool SDL_Init(uint32_t flags);
const char *SDL_GetError(void);
void SDL_free(void *p);
SDL_JoystickID *SDL_GetGamepads(int *count);
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id);
void SDL_CloseGamepad(SDL_Gamepad *g);
bool SDL_GamepadConnected(SDL_Gamepad *g);
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *g);
const char *SDL_GetGamepadName(SDL_Gamepad *g);
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *g);
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value);
SDL_GamepadButtonLabel SDL_GetGamepadButtonLabel(SDL_Gamepad *g, SDL_GamepadButton b);
void SDL_UpdateGamepads(void);
bool SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton b);
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis a);
bool SDL_RumbleGamepad(SDL_Gamepad *g, uint16_t low, uint16_t high, uint32_t ms);
bool SDL_RumbleGamepadTriggers(SDL_Gamepad *g, uint16_t left, uint16_t right, uint32_t ms);
bool SDL_GamepadHasSensor(SDL_Gamepad *g, SDL_SensorType type);
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *g, SDL_SensorType type, bool on);
float SDL_GetGamepadSensorDataRate(SDL_Gamepad *g, SDL_SensorType type);
bool SDL_GetGamepadSensorData(SDL_Gamepad *g, SDL_SensorType type, float *data, int n);
void SDL_SetEventEnabled(uint32_t type, bool on);
SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc *d);
SDL_Joystick *SDL_OpenJoystick(SDL_JoystickID id);
bool SDL_SetJoystickVirtualAxis(SDL_Joystick *j, int axis, Sint16 value);

#ifdef __cplusplus
}
#endif
