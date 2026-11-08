# Payload que reabre um app pelo title ID. TITLE_ID vem do jogo (tools/game_info.py --campo ps4_tid_dev).
#   make PS4_PAYLOAD_SDK=~/ps4sdk/ps4-payload-sdk TITLE_ID=SLOPnnnnn
ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined)
endif
ELF := lancar.elf
CFLAGS := -Wall -O2 -DTITLE_ID='"$(TITLE_ID)"'
all: $(ELF)
$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^ -lSceSystemService -lSceUserService
clean:
	rm -f $(ELF)
