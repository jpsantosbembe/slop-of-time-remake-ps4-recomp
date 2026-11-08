set pagination off
set confirm off
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIG34 nostop noprint pass
handle SIG35 nostop noprint pass
handle SIG36 nostop noprint pass
set print thread-events off
set print inferior-events off
dprintf vk_pool_init,"NPOOL init %#lx %#lx %#x\n", *(long*)($rdi+8), *(long*)($rdi+16), *(int*)($rdi+4)
dprintf vk_pool_fini,"NPOOL fini %#lx %#lx %#x\n", *(long*)($rdi+8), *(long*)($rdi+16), *(int*)($rdi+4)
dprintf *mem_map_physical,"NPHYS map %#lx %#lx\n", $rdi, $rsi
dprintf *mem_unmap_physical,"NPHYS unmap %#lx %#lx\n", $rdi, $rsi
dprintf mem_set_heap_size,"NHEAP %#lx\n", $rdi
run
