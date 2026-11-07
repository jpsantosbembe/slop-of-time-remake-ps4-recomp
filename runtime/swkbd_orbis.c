// ocarina-ps4: o teclado de software (swkbd.h) no PS4. STUB: a entrada de texto volta como cancelada, sem janela.
// O caminho de verdade é o diálogo IME do sistema (sceImeDialogInit / sceImeDialogGetResult, libSceImeDialog), que
// devolve UTF-16 como o applet do Switch; fica para quando o jogo precisar de texto de verdade.
#ifdef __ORBIS__
#include "host.h"
#include "swkbd.h"

int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n) {
    (void)req, (void)out, (void)cap;
    *n = 0;
    rt_log("swkbd: sem teclado de texto no PS4 ainda: a entrada volta cancelada");
    return 0;
}
#endif
