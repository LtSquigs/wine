/*
 * Task termination utility
 *
 * Copyright 2008 Andrew Riedi
 * Copyright 2010 Andrew Nguyen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdlib.h>
#include <windows.h>
#include <tlhelp32.h>
#include <wine/debug.h>

#include "taskkill.h"

WINE_DEFAULT_DEBUG_CHANNEL(taskkill);

static BOOL force_termination = FALSE;

static WCHAR **task_list;
static unsigned int task_count;

static struct
{
    PROCESSENTRY32W p;
    BOOL matched;
}
*process_list;
static unsigned int process_count;

struct pid_close_info
{
    DWORD pid;
    BOOL found;
};

static int taskkill_vprintfW(const WCHAR *msg, va_list va_args)
{
    int wlen;
    DWORD count;
    WCHAR msg_buffer[8192];

    wlen = FormatMessageW(FORMAT_MESSAGE_FROM_STRING, msg, 0, 0, msg_buffer,
                          ARRAY_SIZE(msg_buffer), &va_args);

    if (!WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), msg_buffer, wlen, &count, NULL))
    {
        DWORD len;
        char *msgA;

        /* On Windows WriteConsoleW() fails if the output is redirected. So fall
         * back to WriteFile() using OEM code page.
         */
        len = WideCharToMultiByte(GetOEMCP(), 0, msg_buffer, wlen,
            NULL, 0, NULL, NULL);
        msgA = malloc(len);
        if (!msgA)
            return 0;

        WideCharToMultiByte(GetOEMCP(), 0, msg_buffer, wlen, msgA, len, NULL, NULL);
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), msgA, len, &count, FALSE);
        free(msgA);
    }

    return count;
}

static int WINAPIV taskkill_printfW(const WCHAR *msg, ...)
{
    va_list va_args;
    int len;

    va_start(va_args, msg);
    len = taskkill_vprintfW(msg, va_args);
    va_end(va_args);

    return len;
}

static int WINAPIV taskkill_message_printfW(int msg, ...)
{
    va_list va_args;
    WCHAR msg_buffer[8192];
    int len;

    LoadStringW(GetModuleHandleW(NULL), msg, msg_buffer, ARRAY_SIZE(msg_buffer));

    va_start(va_args, msg);
    len = taskkill_vprintfW(msg_buffer, va_args);
    va_end(va_args);

    return len;
}

static int taskkill_message(int msg)
{
    WCHAR msg_buffer[8192];

    LoadStringW(GetModuleHandleW(NULL), msg, msg_buffer, ARRAY_SIZE(msg_buffer));

    return taskkill_printfW(L"%1", msg_buffer);
}

/* Post WM_CLOSE to all top-level windows belonging to the process with specified PID. */
static BOOL CALLBACK pid_enum_proc(HWND hwnd, LPARAM lParam)
{
    struct pid_close_info *info = (struct pid_close_info *)lParam;
    DWORD hwnd_pid;

    GetWindowThreadProcessId(hwnd, &hwnd_pid);

    if (hwnd_pid == info->pid)
    {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        info->found = TRUE;
    }

    return TRUE;
}

static BOOL enumerate_processes(void)
{
    unsigned int alloc_count = 128;
    void *realloc_list;
    HANDLE snapshot;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return FALSE;

    process_list = malloc(alloc_count * sizeof(*process_list));
    if (!process_list)
        return FALSE;

    process_list[0].p.dwSize = sizeof(process_list[0].p);
    if (!Process32FirstW(snapshot, &process_list[0].p))
        return FALSE;

    do
    {
        process_list[process_count++].matched = FALSE;
        if (process_count == alloc_count)
        {
            alloc_count *= 2;
            realloc_list = realloc(process_list, alloc_count * sizeof(*process_list));
            if (!realloc_list)
                return FALSE;
            process_list = realloc_list;
        }
        process_list[process_count].p.dwSize = sizeof(process_list[process_count].p);
    } while (Process32NextW(snapshot, &process_list[process_count].p));
    CloseHandle(snapshot);
    return TRUE;
}

static BOOL get_task_pid(const WCHAR *str, BOOL *is_numeric, WCHAR *process_name, int *status_code, DWORD *pid)
{
    DWORD self_pid = GetCurrentProcessId();
    const WCHAR *p = str;
    unsigned int i;

    *is_numeric = TRUE;
    while (*p)
    {
        if (!iswdigit(*p++))
        {
            *is_numeric = FALSE;
            break;
        }
    }

    if (*is_numeric)
    {
        *pid = wcstol(str, NULL, 10);
        for (i = 0; i < process_count; ++i)
        {
            if (process_list[i].p.th32ProcessID == *pid)
                break;
        }
        if (i == process_count || process_list[i].matched)
            goto not_found;
        process_list[i].matched = TRUE;
        if (*pid == self_pid)
        {
            taskkill_message(STRING_SELF_TERMINATION);
            *status_code = 1;
            return FALSE;
        }
        return TRUE;
    }

    for (i = 0; i < process_count; ++i)
    {
        if (!wcsicmp(process_list[i].p.szExeFile, str) && !process_list[i].matched)
        {
            process_list[i].matched = TRUE;
            if (process_list[i].p.th32ProcessID == self_pid)
            {
                taskkill_message(STRING_SELF_TERMINATION);
                *status_code = 1;
                return FALSE;
            }
            *pid = process_list[i].p.th32ProcessID;
            wcscpy(process_name, process_list[i].p.szExeFile);
            return TRUE;
        }
    }

not_found:
    taskkill_message_printfW(STRING_SEARCH_FAILED, str);
    *status_code = 128;
    return FALSE;
}

/* The implemented task enumeration and termination behavior does not
 * exactly match native behavior. On Windows:
 *
 * In the case of terminating by process name, specifying a particular
 * process name more times than the number of running instances causes
 * all instances to be terminated, but termination failure messages to
 * be printed as many times as the difference between the specification
 * quantity and the number of running instances.
 *
 * Successful terminations are all listed first in order, with failing
 * terminations being listed at the end.
 *
 * A PID of zero causes taskkill to warn about the inability to terminate
 * system processes. */
static int send_close_messages(void)
{
    WCHAR process_name[MAX_PATH];
    struct pid_close_info info;
    unsigned int i;
    int status_code = 0;
    BOOL is_numeric;

    for (i = 0; i < task_count; i++)
    {
        if (!get_task_pid(task_list[i], &is_numeric, process_name, &status_code, &info.pid))
            continue;

        info.found = FALSE;
        EnumWindows(pid_enum_proc, (LPARAM)&info);
        if (info.found)
        {
            if (is_numeric)
                taskkill_message_printfW(STRING_CLOSE_PID_SEARCH, info.pid);
            else
                taskkill_message_printfW(STRING_CLOSE_PROC_SRCH, process_name, info.pid);
            continue;
        }
        taskkill_message_printfW(STRING_SEARCH_FAILED, task_list[i]);
        status_code = 128;
    }

    return status_code;
}

static int terminate_processes(void)
{
    WCHAR process_name[MAX_PATH];
    unsigned int i;
    int status_code = 0;
    BOOL is_numeric;
    HANDLE process;
    DWORD pid;

    for (i = 0; i < task_count; i++)
    {
        if (!get_task_pid(task_list[i], &is_numeric, process_name, &status_code, &pid))
            continue;

        process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!process)
        {
            taskkill_message_printfW(STRING_SEARCH_FAILED, task_list[i]);
            status_code = 128;
            continue;
        }
        if (!TerminateProcess(process, 1))
        {
            taskkill_message_printfW(STRING_TERMINATE_FAILED, task_list[i]);
            status_code = 1;
            CloseHandle(process);
            continue;
        }
        if (is_numeric)
            taskkill_message_printfW(STRING_TERM_PID_SEARCH, pid);
        else
            taskkill_message_printfW(STRING_TERM_PROC_SEARCH, task_list[i], pid);
        CloseHandle(process);
    }
    return status_code;
}

static BOOL add_to_task_list(WCHAR *name)
{
    static unsigned int list_size = 16;

    if (!task_list)
    {
        task_list = malloc(list_size * sizeof(*task_list));
        if (!task_list)
            return FALSE;
    }
    else if (task_count == list_size)
    {
        void *realloc_list;

        list_size *= 2;
        realloc_list = realloc(task_list, list_size * sizeof(*task_list));
        if (!realloc_list)
            return FALSE;

        task_list = realloc_list;
    }

    task_list[task_count++] = name;
    return TRUE;
}

/* FIXME Argument processing does not match behavior observed on Windows.
 * Stringent argument counting and processing is performed, and unrecognized
 * options are detected as parameters when placed after options that accept one. */
static BOOL process_arguments(int argc, WCHAR *argv[])
{
    if (argc > 1)
    {
        int i;
        WCHAR *argdata;
        BOOL has_im = FALSE, has_pid = FALSE;

        /* Only the lone help option is recognized. */
        if (argc == 2)
        {
            argdata = argv[1];
            if ((*argdata == '/' || *argdata == '-') && !lstrcmpW(L"?", argdata + 1))
            {
                taskkill_message(STRING_USAGE);
                exit(0);
            }
        }

        for (i = 1; i < argc; i++)
        {
            BOOL got_im = FALSE, got_pid = FALSE;

            argdata = argv[i];
            if (*argdata != '/' && *argdata != '-')
                goto invalid;
            argdata++;

            if (!wcsicmp(L"t", argdata))
                WINE_FIXME("argument T not supported\n");
            if (!wcsicmp(L"f", argdata))
                force_termination = TRUE;
            /* Options /IM and /PID appear to behave identically, except for
             * the fact that they cannot be specified at the same time. */
            else if ((got_im = !wcsicmp(L"im", argdata)) ||
                     (got_pid = !wcsicmp(L"pid", argdata)))
            {
                if (!argv[i + 1])
                {
                    taskkill_message_printfW(STRING_MISSING_PARAM, argv[i]);
                    taskkill_message(STRING_USAGE);
                    return FALSE;
                }

                if (got_im) has_im = TRUE;
                if (got_pid) has_pid = TRUE;

                if (has_im && has_pid)
                {
                    taskkill_message(STRING_MUTUAL_EXCLUSIVE);
                    taskkill_message(STRING_USAGE);
                    return FALSE;
                }

                if (!add_to_task_list(argv[i + 1]))
                    return FALSE;
                i++;
            }
            else
            {
                invalid:
                taskkill_message(STRING_INVALID_OPTION);
                taskkill_message(STRING_USAGE);
                return FALSE;
            }
        }
    }
    else
    {
        taskkill_message(STRING_MISSING_OPTION);
        taskkill_message(STRING_USAGE);
        return FALSE;
    }

    return TRUE;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    if (!process_arguments(argc, argv))
        return 1;

    if (!enumerate_processes())
    {
        taskkill_message(STRING_ENUM_FAILED);
        return 1;
    }

    if (force_termination)
        return terminate_processes();
    return send_close_messages();
}
