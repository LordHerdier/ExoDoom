//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//



#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdarg.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef ORIGCODE
#include "SDL.h"
#endif

#include "config.h"

#include "deh_str.h"
#include "doomtype.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "i_joystick.h"
#include "i_sound.h"
#include "i_timer.h"
#include "i_video.h"

#include "i_system.h"

#include "w_wad.h"
#include "z_zone.h"

/* ExoDoom (SCRUM-83): I_Error and I_Quit report through the serial syscall
 * and halt, because there is no stderr, no zenity and no process to exit
 * from on bare metal.  The machinery lives in src/doom_panic.c -- outside
 * the vendored tree on purpose, so this file's patch stays small and so the
 * logic is reachable by a unit test (nothing under src/doom/ links into
 * build/exodoom).  The quoted include resolves via build.sh's -I src. */
#include "doom_panic.h"

#ifdef __MACOSX__
#include <CoreFoundation/CFUserNotification.h>
#endif

#define DEFAULT_RAM 6 /* MiB */
#define MIN_RAM     6  /* MiB */


typedef struct atexit_listentry_s atexit_listentry_t;

struct atexit_listentry_s
{
    atexit_func_t func;
    boolean run_on_error;
    atexit_listentry_t *next;
};

static atexit_listentry_t *exit_funcs = NULL;

void I_AtExit(atexit_func_t func, boolean run_on_error)
{
    atexit_listentry_t *entry;

    entry = malloc(sizeof(*entry));

    entry->func = func;
    entry->run_on_error = run_on_error;
    entry->next = exit_funcs;
    exit_funcs = entry;
}

// Tactile feedback function, probably used for the Logitech Cyberman

void I_Tactile(int on, int off, int total)
{
}

// Zone memory auto-allocation function that allocates the zone size
// by trying progressively smaller zone sizes until one is found that
// works.

static byte *AutoAllocMemory(int *size, int default_ram, int min_ram)
{
    byte *zonemem;

    // Allocate the zone memory.  This loop tries progressively smaller
    // zone sizes until a size is found that can be allocated.
    // If we used the -mb command line parameter, only the parameter
    // provided is accepted.

    zonemem = NULL;

    while (zonemem == NULL)
    {
        // We need a reasonable minimum amount of RAM to start.

        if (default_ram < min_ram)
        {
            I_Error("Unable to allocate %i MiB of RAM for zone", default_ram);
        }

        // Try to allocate the zone memory.

        *size = default_ram * 1024 * 1024;

        zonemem = malloc(*size);

        // Failed to allocate?  Reduce zone size until we reach a size
        // that is acceptable.

        if (zonemem == NULL)
        {
            default_ram -= 1;
        }
    }

    return zonemem;
}

byte *I_ZoneBase (int *size)
{
    byte *zonemem;
    int min_ram, default_ram;
    int p;

    //!
    // @arg <mb>
    //
    // Specify the heap size, in MiB (default 16).
    //

    p = M_CheckParmWithArgs("-mb", 1);

    if (p > 0)
    {
        default_ram = atoi(myargv[p+1]);
        min_ram = default_ram;
    }
    else
    {
        default_ram = DEFAULT_RAM;
        min_ram = MIN_RAM;
    }

    zonemem = AutoAllocMemory(size, default_ram, min_ram);

    printf("zone memory: %p, %x allocated for zone\n", 
           zonemem, *size);

    return zonemem;
}

void I_PrintBanner(char *msg)
{
    int i;
    int spaces = 35 - (strlen(msg) / 2);

    for (i=0; i<spaces; ++i)
        putchar(' ');

    puts(msg);
}

void I_PrintDivider(void)
{
    int i;

    for (i=0; i<75; ++i)
    {
        putchar('=');
    }

    putchar('\n');
}

void I_PrintStartupBanner(char *gamedescription)
{
    I_PrintDivider();
    I_PrintBanner(gamedescription);
    I_PrintDivider();
    
    printf(
    " " PACKAGE_NAME " is free software, covered by the GNU General Public\n"
    " License.  There is NO warranty; not even for MERCHANTABILITY or FITNESS\n"
    " FOR A PARTICULAR PURPOSE. You are welcome to change and distribute\n"
    " copies under certain conditions. See the source for more information.\n");

    I_PrintDivider();
}

// 
// I_ConsoleStdout
//
// Returns true if stdout is a real console, false if it is a file
//

boolean I_ConsoleStdout(void)
{
#ifdef _WIN32
    // SDL "helpfully" always redirects stdout to a file.
    return 0;
#else
#if ORIGCODE
    return isatty(fileno(stdout));
#else
	return 0;
#endif
#endif
}

//
// I_Init
//
/*
void I_Init (void)
{
    I_CheckIsScreensaver();
    I_InitTimer();
    I_InitJoystick();
}
void I_BindVariables(void)
{
    I_BindVideoVariables();
    I_BindJoystickVariables();
    I_BindSoundVariables();
}
*/

//
// I_Quit
//

void I_Quit (void)
{
    atexit_listentry_t *entry;

    // Run through all exit functions
 
    entry = exit_funcs; 

    while (entry != NULL)
    {
        entry->func();
        entry = entry->next;
    }

#if ORIGCODE
    SDL_Quit();

    exit(0);
#endif

    /* ExoDoom (SCRUM-83).  Upstream falls out of I_Quit here and lets the
     * ORIGCODE exit(0) end the process; with that block compiled out the
     * function would simply return, and Doom's caller (M_QuitResponse) has
     * nothing left to run.  Announce it and stop instead -- there is no
     * scheduler to return to and nothing else to do. */
    doom_halt("I_Quit: Doom exited cleanly.");
}

/* ExoDoom (SCRUM-83): the zenity error-box path is dead on bare metal --
 * it shells out via system(), which has no meaning without a process model
 * or a filesystem, and I_Error below no longer calls it.  Gated out rather
 * than deleted so a re-vendor diff stays readable, and gated at all because
 * ZenityAvailable/EscapeShellString/ZenityErrorBox are static: left behind
 * with their only caller gone they would each warn "defined but not used"
 * under the -Wall -Wextra that docker/scripts/build-doom.sh passes. */
#if ORIGCODE && !defined(_WIN32) && !defined(__MACOSX__) && !defined(__DJGPP__)
#define ZENITY_BINARY "/usr/bin/zenity"

// returns non-zero if zenity is available

static int ZenityAvailable(void)
{
    return system(ZENITY_BINARY " --help >/dev/null 2>&1") == 0;
}

// Escape special characters in the given string so that they can be
// safely enclosed in shell quotes.

static char *EscapeShellString(char *string)
{
    char *result;
    char *r, *s;

    // In the worst case, every character might be escaped.
    result = malloc(strlen(string) * 2 + 3);
    r = result;

    // Enclosing quotes.
    *r = '"';
    ++r;

    for (s = string; *s != '\0'; ++s)
    {
        // From the bash manual:
        //
        //  "Enclosing characters in double quotes preserves the literal
        //   value of all characters within the quotes, with the exception
        //   of $, `, \, and, when history expansion is enabled, !."
        //
        // Therefore, escape these characters by prefixing with a backslash.

        if (strchr("$`\\!", *s) != NULL)
        {
            *r = '\\';
            ++r;
        }

        *r = *s;
        ++r;
    }

    // Enclosing quotes.
    *r = '"';
    ++r;
    *r = '\0';

    return result;
}

// Open a native error box with a message using zenity

static int ZenityErrorBox(char *message)
{
    int result;
    char *escaped_message;
    char *errorboxpath;
    static size_t errorboxpath_size;

    if (!ZenityAvailable())
    {
        return 0;
    }

    escaped_message = EscapeShellString(message);

    errorboxpath_size = strlen(ZENITY_BINARY) + strlen(escaped_message) + 19;
    errorboxpath = malloc(errorboxpath_size);
    M_snprintf(errorboxpath, errorboxpath_size, "%s --error --text=%s",
               ZENITY_BINARY, escaped_message);

    result = system(errorboxpath);

    free(errorboxpath);
    free(escaped_message);

    return result;
}

#endif /* ORIGCODE && !_WIN32 && !__MACOSX__ && !__DJGPP__ */


//
// I_Error
//

static boolean already_quitting = false;

void I_Error (char *error, ...)
{
    va_list argptr;
    atexit_listentry_t *entry;

    /* ExoDoom (SCRUM-83).  Upstream writes the message to stderr, copies it
     * into msgbuf for a zenity dialog, and exits.  None of that exists here,
     * so the whole body is replaced by: announce, run the error-time atexit
     * handlers, halt.
     *
     * What is deliberately gone, and why:
     *
     *   - the stderr writes (vfprintf/fprintf/fflush).  There is no stderr;
     *     doom_panic_begin() routes through exo_serial_write (#8) instead,
     *     which is the only output channel a ring-3 LibOS has.
     *   - msgbuf[512].  It existed only to feed the GUI dialog.  Dropping it
     *     also drops half a kilobyte of stack from the failure path, which
     *     matters more here than upstream: a launched LibOS gets exactly one
     *     4 KiB stack page (LIBOS_LAUNCH_STACK_VADDR, src/libos_launch.h).
     *   - ZenityErrorBox / M_ParmExists("-nogui") / I_ConsoleStdout.  A
     *     desktop dialog shelled out through system().
     *   - exit(-1).  doom_panic_halt() replaces it and does not return; on
     *     ring 3 it goes through exo_exit (#20) so the context's pages and
     *     framebuffer binding are actually released, then spins in case that
     *     syscall is unbound.
     *
     * The atexit walk is kept as-is, and it sits BETWEEN the two halves of
     * the panic on purpose: the message goes out first, because those
     * handlers are shutdown code running on a machine that has just declared
     * itself broken, and one of them faulting would otherwise take the
     * explanation with it. */
    if (already_quitting)
    {
        /* doom_panic_begin() has its own re-entry guard, and that is the one
         * that actually stops the recursion: it prints a fixed notice and
         * halts rather than returning.  Upstream's exit(-1) here is inside
         * #if ORIGCODE, so this branch used to fall straight through and
         * re-run the entire function. */
        va_start(argptr, error);
        doom_panic_begin("I_Error (recursive): ", error, argptr);
        va_end(argptr);
    }

    already_quitting = true;

    va_start(argptr, error);
    doom_panic_begin("I_Error: ", error, argptr);
    va_end(argptr);

    /* Shutdown. Here might be other errors. */
    entry = exit_funcs;

    while (entry != NULL)
    {
        if (entry->run_on_error)
        {
            entry->func();
        }

        entry = entry->next;
    }

    doom_panic_halt();
}

//
// Read Access Violation emulation.
//
// From PrBoom+, by entryway.
//

// C:\>debug
// -d 0:0
//
// DOS 6.22:
// 0000:0000  (57 92 19 00) F4 06 70 00-(16 00)
// DOS 7.1:
// 0000:0000  (9E 0F C9 00) 65 04 70 00-(16 00)
// Win98:
// 0000:0000  (9E 0F C9 00) 65 04 70 00-(16 00)
// DOSBox under XP:
// 0000:0000  (00 00 00 F1) ?? ?? ?? 00-(07 00)

#define DOS_MEM_DUMP_SIZE 10

static const unsigned char mem_dump_dos622[DOS_MEM_DUMP_SIZE] = {
  0x57, 0x92, 0x19, 0x00, 0xF4, 0x06, 0x70, 0x00, 0x16, 0x00};
static const unsigned char mem_dump_win98[DOS_MEM_DUMP_SIZE] = {
  0x9E, 0x0F, 0xC9, 0x00, 0x65, 0x04, 0x70, 0x00, 0x16, 0x00};
static const unsigned char mem_dump_dosbox[DOS_MEM_DUMP_SIZE] = {
  0x00, 0x00, 0x00, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00};
static unsigned char mem_dump_custom[DOS_MEM_DUMP_SIZE];

static const unsigned char *dos_mem_dump = mem_dump_dos622;

boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    static boolean firsttime = true;

    if (firsttime)
    {
        int p, i, val;

        firsttime = false;
        i = 0;

        //!
        // @category compat
        // @arg <version>
        //
        // Specify DOS version to emulate for NULL pointer dereference
        // emulation.  Supported versions are: dos622, dos71, dosbox.
        // The default is to emulate DOS 7.1 (Windows 98).
        //

        p = M_CheckParmWithArgs("-setmem", 1);

        if (p > 0)
        {
            if (!strcasecmp(myargv[p + 1], "dos622"))
            {
                dos_mem_dump = mem_dump_dos622;
            }
            if (!strcasecmp(myargv[p + 1], "dos71"))
            {
                dos_mem_dump = mem_dump_win98;
            }
            else if (!strcasecmp(myargv[p + 1], "dosbox"))
            {
                dos_mem_dump = mem_dump_dosbox;
            }
            else
            {
                for (i = 0; i < DOS_MEM_DUMP_SIZE; ++i)
                {
                    ++p;

                    if (p >= myargc || myargv[p][0] == '-')
                    {
                        break;
                    }

                    M_StrToInt(myargv[p], &val);
                    mem_dump_custom[i++] = (unsigned char) val;
                }

                dos_mem_dump = mem_dump_custom;
            }
        }
    }

    switch (size)
    {
    case 1:
        *((unsigned char *) value) = dos_mem_dump[offset];
        return true;
    case 2:
        *((unsigned short *) value) = dos_mem_dump[offset]
                                    | (dos_mem_dump[offset + 1] << 8);
        return true;
    case 4:
        *((unsigned int *) value) = dos_mem_dump[offset]
                                  | (dos_mem_dump[offset + 1] << 8)
                                  | (dos_mem_dump[offset + 2] << 16)
                                  | (dos_mem_dump[offset + 3] << 24);
        return true;
    }

    return false;
}

