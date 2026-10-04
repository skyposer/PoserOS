#include "API-LIB/ALLAPI.h"

void kernel_main(void)
{
    clearScreen();

    init_memory();
    init_storage();
    init_shell();

    ioprint("boot> ");
    ioinput(boot_buf, BOOTBUF_SIZE);

    start_shell();

    hltsleep();
}
