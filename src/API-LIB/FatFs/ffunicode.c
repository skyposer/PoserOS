/*----------------------------------------------------------------------------/
/  Minimal LFN character conversion for PoserOS                              /
/                                                                            /
/  FatFs R0.15 expects ff_oem2uni(), ff_uni2oem() and ff_wtoupper() from      /
/  ffunicode.c to be present when FF_USE_LFN is enabled. This build only      /
/  deals with ASCII file names, so the full OEM code-page tables are not      /
/  needed: characters below 0x80 map to themselves and anything else falls    /
/  back to '?'. Up-casing handles ASCII a-z only.                             /
/----------------------------------------------------------------------------*/

#include "ff.h"

#if FF_USE_LFN

/* ANSI/OEM (code page) --> Unicode */
WCHAR ff_oem2uni (
    WCHAR oem,  /* ANSI/OEM code to be converted */
    WORD cp     /* Code page (unused, ASCII only) */
)
{
    (void)cp;
    return (oem < 0x80) ? oem : (WCHAR)'?';
}

/* Unicode --> ANSI/OEM (code page) */
WCHAR ff_uni2oem (
    DWORD uni,  /* Unicode to be converted */
    WORD cp     /* Code page (unused, ASCII only) */
)
{
    (void)cp;
    return (uni < 0x80) ? (WCHAR)uni : (WCHAR)'?';
}

/* Unicode up-case conversion */
DWORD ff_wtoupper (
    DWORD uni   /* Unicode character to be up-cased */
)
{
    if (uni >= 'a' && uni <= 'z')
        return uni - 0x20;
    return uni;
}

#endif /* FF_USE_LFN */
