/*
 * PoserOS | Powered By VM
 * Author: Poser(破晓)
 * Copyright (c) 2026 skyposer
 * Part of the code is AI-assisted.
 * Licensed under MIT, see project LICENSE.
 *
 * This project includes FatFs in /API-LIB/FatFs/, following its 1-clause BSD license.
 */


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
