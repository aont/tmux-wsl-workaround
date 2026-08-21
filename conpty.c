#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

/* Older MinGW-w64 headers omit the ConPTY declarations despite exporting the APIs. */
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
typedef HANDLE HPCON;
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
WINBASEAPI HRESULT WINAPI CreatePseudoConsole(COORD, HANDLE, HANDLE, DWORD, HPCON *);
WINBASEAPI HRESULT WINAPI ResizePseudoConsole(HPCON, COORD);
WINBASEAPI VOID WINAPI ClosePseudoConsole(HPCON);
#endif

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

#define DEFAULT_COLS 120
#define DEFAULT_ROWS 30
#define COPY_BUFFER_SIZE 16384
#define RESIZE_POLL_MS 100

typedef struct CONSOLE_STATE {
    HANDLE hIn;
    HANDLE hOut;
    DWORD inMode;
    DWORD outMode;
    UINT inCP;
    UINT outCP;
    BOOL inIsConsole;
    BOOL outIsConsole;
    BOOL changedInMode;
    BOOL changedOutMode;
    BOOL changedInCP;
    BOOL changedOutCP;
} CONSOLE_STATE;

typedef struct COPY_CTX {
    HANDLE src;
    HANDLE dst;
    HANDLE writeFailureEvent;
    BOOL closeSrcOnExit;
    BOOL closeDstOnExit;
} COPY_CTX;

typedef struct RESIZE_CTX {
    HPCON hpc;
    HANDLE hConsoleOut;
    HANDLE hStopEvent;
    COORD last;
} RESIZE_CTX;

static void print_win32_error(const wchar_t *where, DWORD err)
{
    wchar_t *msg = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;

    FormatMessageW(flags, NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    if (msg != NULL) {
        fwprintf(stderr, L"%ls: error %lu: %ls", where, (unsigned long)err, msg);
        LocalFree(msg);
    } else {
        fwprintf(stderr, L"%ls: error %lu\n", where, (unsigned long)err);
    }
}

static void print_hresult_error(const wchar_t *where, HRESULT hr)
{
    fwprintf(stderr, L"%ls: HRESULT 0x%08lX\n", where, (unsigned long)hr);
}

static BOOL write_all(HANDLE h, const void *buf, DWORD len)
{
    const BYTE *p = (const BYTE *)buf;
    DWORD total = 0;

    while (total < len) {
        DWORD written = 0;
        if (!WriteFile(h, p + total, len - total, &written, NULL)) {
            return FALSE;
        }
        if (written == 0) {
            SetLastError(ERROR_WRITE_FAULT);
            return FALSE;
        }
        total += written;
    }
    return TRUE;
}

static DWORD WINAPI copy_thread(LPVOID param)
{
    COPY_CTX *ctx = (COPY_CTX *)param;
    BYTE buf[COPY_BUFFER_SIZE];

    for (;;) {
        DWORD n = 0;
        BOOL ok = ReadFile(ctx->src, buf, (DWORD)sizeof(buf), &n, NULL);

        if (!ok || n == 0) {
            break;
        }

        if (!write_all(ctx->dst, buf, n)) {
            if (ctx->writeFailureEvent != NULL) {
                SetEvent(ctx->writeFailureEvent);
            }
            break;
        }
    }

    if (ctx->closeSrcOnExit && ctx->src != NULL && ctx->src != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->src);
        ctx->src = NULL;
    }
    if (ctx->closeDstOnExit && ctx->dst != NULL && ctx->dst != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->dst);
        ctx->dst = NULL;
    }

    return 0;
}

static BOOL get_console_window_size(HANDLE hConsole, COORD *size)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    SHORT cols;
    SHORT rows;

    if (!GetConsoleScreenBufferInfo(hConsole, &info)) {
        return FALSE;
    }

    cols = (SHORT)(info.srWindow.Right - info.srWindow.Left + 1);
    rows = (SHORT)(info.srWindow.Bottom - info.srWindow.Top + 1);
    if (cols <= 0 || rows <= 0) {
        return FALSE;
    }

    size->X = cols;
    size->Y = rows;
    return TRUE;
}

static DWORD WINAPI resize_thread(LPVOID param)
{
    RESIZE_CTX *ctx = (RESIZE_CTX *)param;

    for (;;) {
        DWORD wr = WaitForSingleObject(ctx->hStopEvent, RESIZE_POLL_MS);
        COORD now;

        if (wr == WAIT_OBJECT_0) {
            break;
        }

        if (get_console_window_size(ctx->hConsoleOut, &now)) {
            if (now.X != ctx->last.X || now.Y != ctx->last.Y) {
                if (SUCCEEDED(ResizePseudoConsole(ctx->hpc, now))) {
                    ctx->last = now;
                }
            }
        }
    }

    return 0;
}

static void restore_console(CONSOLE_STATE *state)
{
    if (state->changedInMode) {
        SetConsoleMode(state->hIn, state->inMode);
    }
    if (state->changedOutMode) {
        SetConsoleMode(state->hOut, state->outMode);
    }
    if (state->changedInCP) {
        SetConsoleCP(state->inCP);
    }
    if (state->changedOutCP) {
        SetConsoleOutputCP(state->outCP);
    }
}

static void configure_console(CONSOLE_STATE *state)
{
    DWORD mode;

    ZeroMemory(state, sizeof(*state));
    state->hIn = GetStdHandle(STD_INPUT_HANDLE);
    state->hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    if (state->hIn != INVALID_HANDLE_VALUE && state->hIn != NULL &&
        GetConsoleMode(state->hIn, &mode)) {
        DWORD newMode;

        state->inIsConsole = TRUE;
        state->inMode = mode;
        state->inCP = GetConsoleCP();

        /*
         * Raw-ish VT input:
         * - disable line buffering and local echo
         * - disable processed Ctrl+C so byte 0x03 can flow into ConPTY
         * - request VT sequences for special keys
         */
        newMode = mode;
        newMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        newMode |= ENABLE_VIRTUAL_TERMINAL_INPUT;

        if (SetConsoleMode(state->hIn, newMode)) {
            state->changedInMode = TRUE;
        }
        if (state->inCP != CP_UTF8 && SetConsoleCP(CP_UTF8)) {
            state->changedInCP = TRUE;
        }
    }

    if (state->hOut != INVALID_HANDLE_VALUE && state->hOut != NULL &&
        GetConsoleMode(state->hOut, &mode)) {
        DWORD newMode;

        state->outIsConsole = TRUE;
        state->outMode = mode;
        state->outCP = GetConsoleOutputCP();

        newMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (SetConsoleMode(state->hOut, newMode)) {
            state->changedOutMode = TRUE;
        }
        if (state->outCP != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) {
            state->changedOutCP = TRUE;
        }
    }
}

static wchar_t *build_command_line(int argc, wchar_t **argv, int first)
{
    SIZE_T cap = 1;
    SIZE_T pos = 0;
    wchar_t *out;
    int i;

    for (i = first; i < argc; ++i) {
        SIZE_T n = wcslen(argv[i]);
        if (n > (SIZE_MAX - cap - 4) / 2) {
            return NULL;
        }
        cap += n * 2 + 4;
    }

    out = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (out == NULL) {
        return NULL;
    }

    for (i = first; i < argc; ++i) {
        const wchar_t *arg = argv[i];
        BOOL quote = FALSE;
        const wchar_t *s;

        if (i != first) {
            out[pos++] = L' ';
        }

        if (*arg == L'\0' || wcspbrk(arg, L" \t\"") != NULL) {
            quote = TRUE;
        }

        if (!quote) {
            SIZE_T n = wcslen(arg);
            memcpy(out + pos, arg, n * sizeof(wchar_t));
            pos += n;
            continue;
        }

        out[pos++] = L'\"';
        s = arg;
        while (*s != L'\0') {
            SIZE_T slashes = 0;

            while (*s == L'\\') {
                ++slashes;
                ++s;
            }

            if (*s == L'\"') {
                SIZE_T k;
                for (k = 0; k < slashes * 2 + 1; ++k) {
                    out[pos++] = L'\\';
                }
                out[pos++] = L'\"';
                ++s;
            } else if (*s == L'\0') {
                SIZE_T k;
                for (k = 0; k < slashes * 2; ++k) {
                    out[pos++] = L'\\';
                }
                break;
            } else {
                SIZE_T k;
                for (k = 0; k < slashes; ++k) {
                    out[pos++] = L'\\';
                }
                out[pos++] = *s++;
            }
        }
        out[pos++] = L'\"';
    }

    out[pos] = L'\0';
    return out;
}

static BOOL parse_short(const wchar_t *text, SHORT *value)
{
    wchar_t *end = NULL;
    long v = wcstol(text, &end, 10);

    if (text == end || end == NULL || *end != L'\0' || v < 1 || v > SHRT_MAX) {
        return FALSE;
    }

    *value = (SHORT)v;
    return TRUE;
}

static void usage(const wchar_t *exe)
{
    fwprintf(stderr,
        L"Usage:\n"
        L"  %ls [--cols N] [--rows N] [--no-resize] [--] command [args...]\n"
        L"  %ls                         (starts %%COMSPEC%% / cmd.exe)\n\n"
        L"Examples:\n"
        L"  %ls powershell.exe -NoLogo\n"
        L"  %ls -- cmd.exe /k ver\n"
        L"  type input.txt | %ls --cols 100 --rows 40 mytool.exe\n",
        exe, exe, exe, exe, exe);
}

int wmain(int argc, wchar_t **argv)
{
    int rc = 1;
    int firstCommand = 1;
    BOOL autoResize = TRUE;
    BOOL colsSet = FALSE;
    BOOL rowsSet = FALSE;
    SHORT cols = DEFAULT_COLS;
    SHORT rows = DEFAULT_ROWS;
    wchar_t *commandLine = NULL;
    wchar_t fallbackCmd[MAX_PATH];

    HANDLE hPtyInRead = INVALID_HANDLE_VALUE;
    HANDLE hPtyInWrite = INVALID_HANDLE_VALUE;
    HANDLE hPtyOutRead = INVALID_HANDLE_VALUE;
    HANDLE hPtyOutWrite = INVALID_HANDLE_VALUE;
    HPCON hpc = NULL;

    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T attrSize = 0;
    BOOL attrListInitialized = FALSE;

    HANDLE hInputThread = NULL;
    HANDLE hOutputThread = NULL;
    HANDLE hResizeThread = NULL;
    HANDLE hResizeStop = NULL;
    HANDLE hOutputWriteFailure = NULL;
    COPY_CTX inputCtx;
    COPY_CTX outputCtx;
    RESIZE_CTX resizeCtx;
    CONSOLE_STATE consoleState;

    COORD initialSize;
    HRESULT hr;
    DWORD childExitCode = 1;
    int i;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    ZeroMemory(&inputCtx, sizeof(inputCtx));
    ZeroMemory(&outputCtx, sizeof(outputCtx));
    ZeroMemory(&resizeCtx, sizeof(resizeCtx));
    ZeroMemory(&consoleState, sizeof(consoleState));

    for (i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--") == 0) {
            firstCommand = i + 1;
            break;
        }
        if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (wcscmp(argv[i], L"--no-resize") == 0) {
            autoResize = FALSE;
            firstCommand = i + 1;
            continue;
        }
        if (wcscmp(argv[i], L"--cols") == 0) {
            if (i + 1 >= argc || !parse_short(argv[i + 1], &cols)) {
                fwprintf(stderr, L"Invalid --cols value.\n");
                return 2;
            }
            colsSet = TRUE;
            ++i;
            firstCommand = i + 1;
            continue;
        }
        if (wcscmp(argv[i], L"--rows") == 0) {
            if (i + 1 >= argc || !parse_short(argv[i + 1], &rows)) {
                fwprintf(stderr, L"Invalid --rows value.\n");
                return 2;
            }
            rowsSet = TRUE;
            ++i;
            firstCommand = i + 1;
            continue;
        }

        firstCommand = i;
        break;
    }

    configure_console(&consoleState);

    initialSize.X = cols;
    initialSize.Y = rows;
    if (consoleState.outIsConsole) {
        COORD detected;
        if (get_console_window_size(consoleState.hOut, &detected)) {
            if (!colsSet) initialSize.X = detected.X;
            if (!rowsSet) initialSize.Y = detected.Y;
        }
    }

    if (firstCommand < argc) {
        commandLine = build_command_line(argc, argv, firstCommand);
        if (commandLine == NULL) {
            fwprintf(stderr, L"Failed to build child command line.\n");
            goto cleanup;
        }
    } else {
        DWORD n = GetEnvironmentVariableW(L"COMSPEC", fallbackCmd, (DWORD)(sizeof(fallbackCmd) / sizeof(fallbackCmd[0])));
        if (n == 0 || n >= (DWORD)(sizeof(fallbackCmd) / sizeof(fallbackCmd[0]))) {
            wcscpy_s(fallbackCmd, sizeof(fallbackCmd) / sizeof(fallbackCmd[0]), L"cmd.exe");
        }
        {
            wchar_t *tmpv[1];
            tmpv[0] = fallbackCmd;
            commandLine = build_command_line(1, tmpv, 0);
        }
        if (commandLine == NULL) {
            fwprintf(stderr, L"Failed to build default command line.\n");
            goto cleanup;
        }
    }

    if (!CreatePipe(&hPtyInRead, &hPtyInWrite, NULL, 0)) {
        print_win32_error(L"CreatePipe(ConPTY input)", GetLastError());
        goto cleanup;
    }
    if (!CreatePipe(&hPtyOutRead, &hPtyOutWrite, NULL, 0)) {
        print_win32_error(L"CreatePipe(ConPTY output)", GetLastError());
        goto cleanup;
    }

    hr = CreatePseudoConsole(initialSize, hPtyInRead, hPtyOutWrite, 0, &hpc);
    if (FAILED(hr)) {
        print_hresult_error(L"CreatePseudoConsole", hr);
        goto cleanup;
    }

    /* ConPTY duplicates its ends internally. The host keeps only its pipe ends. */
    CloseHandle(hPtyInRead);
    hPtyInRead = INVALID_HANDLE_VALUE;
    CloseHandle(hPtyOutWrite);
    hPtyOutWrite = INVALID_HANDLE_VALUE;

    si.StartupInfo.cb = sizeof(si);
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    if (attrSize == 0) {
        print_win32_error(L"InitializeProcThreadAttributeList(size)", GetLastError());
        goto cleanup;
    }

    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)
        HeapAlloc(GetProcessHeap(), 0, attrSize);
    if (si.lpAttributeList == NULL) {
        fwprintf(stderr, L"HeapAlloc(attribute list) failed.\n");
        goto cleanup;
    }

    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize)) {
        print_win32_error(L"InitializeProcThreadAttributeList", GetLastError());
        goto cleanup;
    }
    attrListInitialized = TRUE;

    if (!UpdateProcThreadAttribute(
            si.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            hpc,
            sizeof(hpc),
            NULL,
            NULL)) {
        print_win32_error(L"UpdateProcThreadAttribute(PSEUDOCONSOLE)", GetLastError());
        goto cleanup;
    }

    if (!CreateProcessW(
            NULL,
            commandLine,
            NULL,
            NULL,
            FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            NULL,
            NULL,
            &si.StartupInfo,
            &pi)) {
        print_win32_error(L"CreateProcessW", GetLastError());
        goto cleanup;
    }

    /* Attribute list is no longer needed after CreateProcessW. */
    DeleteProcThreadAttributeList(si.lpAttributeList);
    attrListInitialized = FALSE;
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    si.lpAttributeList = NULL;

    CloseHandle(pi.hThread);
    pi.hThread = NULL;

    inputCtx.src = consoleState.hIn;
    inputCtx.dst = hPtyInWrite;
    inputCtx.writeFailureEvent = NULL;
    inputCtx.closeSrcOnExit = FALSE;
    inputCtx.closeDstOnExit = TRUE;
    hInputThread = CreateThread(NULL, 0, copy_thread, &inputCtx, 0, NULL);
    if (hInputThread == NULL) {
        print_win32_error(L"CreateThread(input)", GetLastError());
        goto running_cleanup;
    }
    /* input thread owns this handle from now on */
    hPtyInWrite = INVALID_HANDLE_VALUE;

    hOutputWriteFailure = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (hOutputWriteFailure == NULL) {
        print_win32_error(L"CreateEvent(output failure)", GetLastError());
        goto running_cleanup;
    }

    outputCtx.src = hPtyOutRead;
    outputCtx.dst = consoleState.hOut;
    outputCtx.writeFailureEvent = hOutputWriteFailure;
    outputCtx.closeSrcOnExit = TRUE;
    outputCtx.closeDstOnExit = FALSE;
    hOutputThread = CreateThread(NULL, 0, copy_thread, &outputCtx, 0, NULL);
    if (hOutputThread == NULL) {
        print_win32_error(L"CreateThread(output)", GetLastError());
        goto running_cleanup;
    }
    /* output thread owns this handle from now on */
    hPtyOutRead = INVALID_HANDLE_VALUE;

    if (autoResize && consoleState.outIsConsole) {
        hResizeStop = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (hResizeStop != NULL) {
            resizeCtx.hpc = hpc;
            resizeCtx.hConsoleOut = consoleState.hOut;
            resizeCtx.hStopEvent = hResizeStop;
            resizeCtx.last = initialSize;
            hResizeThread = CreateThread(NULL, 0, resize_thread, &resizeCtx, 0, NULL);
        }
    }

    {
        HANDLE waits[2];
        DWORD wr;
        waits[0] = pi.hProcess;
        waits[1] = hOutputWriteFailure;
        wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0) {
            if (!GetExitCodeProcess(pi.hProcess, &childExitCode)) {
                childExitCode = 1;
            }
            rc = (int)childExitCode;
        } else {
            /* stdout disappeared/broke: tear down the PTY instead of deadlocking. */
            rc = 1;
        }
    }

running_cleanup:
    if (hResizeStop != NULL) {
        SetEvent(hResizeStop);
    }
    if (hResizeThread != NULL) {
        WaitForSingleObject(hResizeThread, INFINITE);
        CloseHandle(hResizeThread);
        hResizeThread = NULL;
    }
    if (hResizeStop != NULL) {
        CloseHandle(hResizeStop);
        hResizeStop = NULL;
    }

    /*
     * Stop stdin forwarding first. CancelSynchronousIo unblocks a pending
     * synchronous ReadFile on this thread in the normal console/pipe cases.
     */
    if (hInputThread != NULL) {
        CancelSynchronousIo(hInputThread);
        if (WaitForSingleObject(hInputThread, 1000) == WAIT_OBJECT_0) {
            CloseHandle(hInputThread);
            hInputThread = NULL;
        } else {
            /* Do not wait forever on a hostile/non-cancellable input handle. */
            CloseHandle(hInputThread);
            hInputThread = NULL;
        }
    }

    /*
     * Keep the output thread draining while ClosePseudoConsole runs.
     * This avoids the documented pre-Windows-11-24H2 close deadlock.
     */
    if (hOutputThread == NULL && hPtyOutRead != INVALID_HANDLE_VALUE) {
        /* No reader exists, so close the output pipe before ClosePseudoConsole. */
        CloseHandle(hPtyOutRead);
        hPtyOutRead = INVALID_HANDLE_VALUE;
    }

    if (hpc != NULL) {
        ClosePseudoConsole(hpc);
        hpc = NULL;
    }

    if (hOutputThread != NULL) {
        WaitForSingleObject(hOutputThread, INFINITE);
        CloseHandle(hOutputThread);
        hOutputThread = NULL;
    }
    if (hOutputWriteFailure != NULL) {
        CloseHandle(hOutputWriteFailure);
        hOutputWriteFailure = NULL;
    }

cleanup:
    if (pi.hThread != NULL) CloseHandle(pi.hThread);
    if (pi.hProcess != NULL) CloseHandle(pi.hProcess);

    if (si.lpAttributeList != NULL) {
        if (attrListInitialized) {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            attrListInitialized = FALSE;
        }
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        si.lpAttributeList = NULL;
    }

    if (hpc != NULL) {
        /* No output thread exists here, so close our read side before closing ConPTY. */
        if (hPtyOutRead != INVALID_HANDLE_VALUE) {
            CloseHandle(hPtyOutRead);
            hPtyOutRead = INVALID_HANDLE_VALUE;
        }
        ClosePseudoConsole(hpc);
        hpc = NULL;
    }

    if (hOutputWriteFailure != NULL) CloseHandle(hOutputWriteFailure);

    if (hPtyInRead != INVALID_HANDLE_VALUE) CloseHandle(hPtyInRead);
    if (hPtyInWrite != INVALID_HANDLE_VALUE) CloseHandle(hPtyInWrite);
    if (hPtyOutRead != INVALID_HANDLE_VALUE) CloseHandle(hPtyOutRead);
    if (hPtyOutWrite != INVALID_HANDLE_VALUE) CloseHandle(hPtyOutWrite);

    if (commandLine != NULL) {
        HeapFree(GetProcessHeap(), 0, commandLine);
    }

    restore_console(&consoleState);
    return rc;
}
