// Payload "lancar": abre um app pelo title ID (dado na compilação: -DTITLE_ID="\"SLOPnnnnn\""), para reabrir o app de
// desenvolvimento depois que a GPU travada o derruba, sem ninguém tocar no console. Vai pelo BinLoader do GoldHEN
// (porta 9090) ou por um elfldr (9021). Avisa na tela o resultado.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#ifndef TITLE_ID
#error "compile com -DTITLE_ID=\"\\\"SLOPnnnnn\\\"\""
#endif

typedef struct {
  int type, req_id, priority, msg_id, target_id, user_id, unk1, unk2, app_id, error_num, unk3;
  char use_icon;
  char message[1024], icon_uri[1024], unk[1024];
} notify_request_t;

typedef struct {
  uint32_t sz;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} LncAppParam;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceLncUtilLaunchApp(const char *title_id, const char **argv, LncAppParam *param);
int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int *user);

static void say(const char *msg) {
  notify_request_t req;
  bzero(&req, sizeof req);
  req.target_id = -1;
  strncpy(req.message, msg, sizeof req.message - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

int main(void) {
  int user = -1;
  sceUserServiceInitialize(NULL);
  sceUserServiceGetForegroundUser(&user);
  LncAppParam p;
  bzero(&p, sizeof p);
  p.sz = sizeof p;
  p.user_id = (uint32_t)user;
  int r = sceLncUtilLaunchApp(TITLE_ID, NULL, &p);
  char m[128];
  snprintf(m, sizeof m, "lancar: %s -> %#x", TITLE_ID, (unsigned)r);
  say(m);
  printf("%s\n", m);
  return r;
}
