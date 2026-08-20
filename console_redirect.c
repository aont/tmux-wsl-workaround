#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AsyncRead {
    HANDLE source;
    HANDLE destination;
    OVERLAPPED overlapped;
    BYTE buffer[4096];
    BOOL active;
    char const *name;
} AsyncRead;

static char const *ShiftCommandLine(char const *cmdline) {
    BOOL backslash_preceding = FALSE, inside_double_quote = FALSE, after_argv0 = FALSE;
    int i;
    for (i = 0;; i++) {
        char const c = cmdline[i];
        if (after_argv0) {
            if (c != ' ' && c != '\t') return cmdline + i;
        } else if (inside_double_quote) {
            if (c == '\\') backslash_preceding = !backslash_preceding;
            else if (c == '"') {
                if (!backslash_preceding) inside_double_quote = FALSE;
                else backslash_preceding = FALSE;
            } else if (c == '\0') return cmdline + i;
            else backslash_preceding = FALSE;
        } else {
            if (c == '\\') backslash_preceding = !backslash_preceding;
            else if (c == '"') {
                if (!backslash_preceding) inside_double_quote = TRUE;
                else backslash_preceding = FALSE;
            } else if (c == ' ' || c == '\t') after_argv0 = TRUE;
            else if (c == '\0') return cmdline + i;
            else backslash_preceding = FALSE;
        }
    }
}

static void ShowWin32Error(char const *operation) {
    DWORD error = GetLastError();
    LPSTR message = NULL;
    if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, (LPSTR)&message, 0, NULL)) {
        fprintf(stderr, "%s failed (error %lu): %s\n", operation, (unsigned long)error, message);
        LocalFree(message);
    } else {
        fprintf(stderr, "%s failed with error %lu.\n", operation, (unsigned long)error);
    }
}

static BOOL WriteAll(HANDLE handle, void const *data, DWORD size) {
    BYTE const *cursor = (BYTE const *)data;
    while (size != 0) {
        DWORD written = 0;
        if (!WriteFile(handle, cursor, size, &written, NULL)) return FALSE;
        if (written == 0) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
        cursor += written;
        size -= written;
    }
    return TRUE;
}

static BOOL CreateOverlappedPipe(HANDLE *server, HANDLE *client, DWORD server_access,
        DWORD client_access, BOOL client_inherits) {
    static LONG pipe_number;
    char name[128];
    SECURITY_ATTRIBUTES sa;
    OVERLAPPED connect_overlapped;
    BOOL connect_pending;
    DWORD ignored;
    sprintf(name, "\\\\.\\pipe\\tmux-wsl-workaround-%lu-%ld",
        (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&pipe_number));
    *server = CreateNamedPipeA(name, server_access | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    if (*server == INVALID_HANDLE_VALUE) return FALSE;
    ZeroMemory(&connect_overlapped, sizeof(connect_overlapped));
    connect_overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (connect_overlapped.hEvent == NULL) {
        CloseHandle(*server); *server = NULL; return FALSE;
    }
    connect_pending = !ConnectNamedPipe(*server, &connect_overlapped);
    if (connect_pending && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(connect_overlapped.hEvent); CloseHandle(*server); *server = NULL;
        return FALSE;
    }
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = client_inherits;
    *client = CreateFileA(name, client_access, 0, &sa, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (*client == INVALID_HANDLE_VALUE) {
        CancelIoEx(*server, &connect_overlapped);
        CloseHandle(connect_overlapped.hEvent); CloseHandle(*server); *server = NULL;
        return FALSE;
    }
    if (connect_pending && !GetOverlappedResult(*server, &connect_overlapped, &ignored, TRUE)) {
        CloseHandle(*client); CloseHandle(*server); *client = NULL; *server = NULL;
        CloseHandle(connect_overlapped.hEvent);
        return FALSE;
    }
    CloseHandle(connect_overlapped.hEvent);
    return TRUE;
}

static BOOL StartRead(AsyncRead *read) {
    DWORD bytes_read;
    ResetEvent(read->overlapped.hEvent);
    read->active = TRUE;
    if (ReadFile(read->source, read->buffer, sizeof(read->buffer), &bytes_read,
            &read->overlapped)) return TRUE;
    if (GetLastError() == ERROR_IO_PENDING) return TRUE;
    read->active = FALSE;
    return FALSE;
}

static BOOL IsEndOfPipe(DWORD error) {
    return error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_OPERATION_ABORTED;
}

int main(void) {
    char const *child_command_line = ShiftCommandLine(GetCommandLineA());
    char *mutable_command_line = NULL;
    HANDLE stdout_read = NULL, stdout_write = NULL, stderr_read = NULL, stderr_write = NULL;
    HANDLE stdin_read = NULL, stdin_write = NULL;
    HANDLE parent_stdin, parent_stdout, parent_stderr;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    AsyncRead reads[3];
    BOOL child_done = FALSE;
    int exit_code = EXIT_FAILURE;
    int i;

    ZeroMemory(&process_info, sizeof(process_info));
    ZeroMemory(reads, sizeof(reads));
    if (*child_command_line == '\0') { fprintf(stderr, "No command line to execute was specified.\n"); goto cleanup; }
    mutable_command_line = _strdup(child_command_line);
    if (mutable_command_line == NULL) { fprintf(stderr, "Could not allocate memory for the command line.\n"); goto cleanup; }
    parent_stdin = GetStdHandle(STD_INPUT_HANDLE);
    parent_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    parent_stderr = GetStdHandle(STD_ERROR_HANDLE);
    if (parent_stdin == NULL || parent_stdin == INVALID_HANDLE_VALUE) { ShowWin32Error("GetStdHandle(stdin)"); goto cleanup; }
    if (parent_stdout == NULL || parent_stdout == INVALID_HANDLE_VALUE) { ShowWin32Error("GetStdHandle(stdout)"); goto cleanup; }
    if (parent_stderr == NULL || parent_stderr == INVALID_HANDLE_VALUE) { ShowWin32Error("GetStdHandle(stderr)"); goto cleanup; }

    if (!CreateOverlappedPipe(&stdout_read, &stdout_write, PIPE_ACCESS_INBOUND,
            GENERIC_WRITE, TRUE)) { ShowWin32Error("CreateNamedPipe(stdout)"); goto cleanup; }
    if (!CreateOverlappedPipe(&stderr_read, &stderr_write, PIPE_ACCESS_INBOUND,
            GENERIC_WRITE, TRUE)) { ShowWin32Error("CreateNamedPipe(stderr)"); goto cleanup; }
    if (!CreateOverlappedPipe(&stdin_write, &stdin_read, PIPE_ACCESS_OUTBOUND,
            GENERIC_READ, TRUE)) { ShowWin32Error("CreateNamedPipe(stdin)"); goto cleanup; }

    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_SHOWMINIMIZED;
    startup_info.hStdInput = stdin_read;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stderr_write;
    if (!CreateProcessA(NULL, mutable_command_line, NULL, NULL, TRUE, CREATE_NEW_CONSOLE,
            NULL, NULL, &startup_info, &process_info)) { ShowWin32Error("CreateProcessA"); goto cleanup; }
    CloseHandle(stdout_write); stdout_write = NULL;
    CloseHandle(stderr_write); stderr_write = NULL;
    CloseHandle(stdin_read); stdin_read = NULL;
    CloseHandle(process_info.hThread); process_info.hThread = NULL;

    reads[0].source = parent_stdin; reads[0].destination = stdin_write; reads[0].name = "ReadFile(stdin)";
    reads[1].source = stdout_read; reads[1].destination = parent_stdout; reads[1].name = "ReadFile(stdout)";
    reads[2].source = stderr_read; reads[2].destination = parent_stderr; reads[2].name = "ReadFile(stderr)";
    for (i = 0; i < 3; i++) {
        reads[i].overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (reads[i].overlapped.hEvent == NULL) { ShowWin32Error("CreateEvent"); goto cleanup; }
        if (!StartRead(&reads[i])) { ShowWin32Error(reads[i].name); goto cleanup; }
    }

    while (reads[1].active || reads[2].active || !child_done) {
        HANDLE events[4];
        int indexes[4];
        DWORD count = 0, wait_result;
        for (i = 0; i < 3; i++) if (reads[i].active) {
            events[count] = reads[i].overlapped.hEvent; indexes[count++] = i;
        }
        if (!child_done) { events[count] = process_info.hProcess; indexes[count++] = 3; }
        wait_result = WaitForMultipleObjects(count, events, FALSE, INFINITE);
        if (wait_result < WAIT_OBJECT_0 || wait_result >= WAIT_OBJECT_0 + count) {
            ShowWin32Error("WaitForMultipleObjects"); goto cleanup;
        }
        i = indexes[wait_result - WAIT_OBJECT_0];
        if (i == 3) {
            child_done = TRUE;
            if (reads[0].active) CancelIoEx(reads[0].source, &reads[0].overlapped);
            if (stdin_write != NULL) { CloseHandle(stdin_write); stdin_write = NULL; }
            continue;
        }
        {
            DWORD bytes_read = 0;
            AsyncRead *read = &reads[i];
            read->active = FALSE;
            if (!GetOverlappedResult(read->source, &read->overlapped, &bytes_read, FALSE)) {
                DWORD error = GetLastError();
                if (!IsEndOfPipe(error)) { SetLastError(error); ShowWin32Error(read->name); goto cleanup; }
                if (i == 0 && stdin_write != NULL) { CloseHandle(stdin_write); stdin_write = NULL; }
                continue;
            }
            if (bytes_read == 0) {
                if (i == 0 && stdin_write != NULL) { CloseHandle(stdin_write); stdin_write = NULL; }
                continue;
            }
            if (!WriteAll(read->destination, read->buffer, bytes_read)) {
                if (i == 0 && IsEndOfPipe(GetLastError())) {
                    if (stdin_write != NULL) { CloseHandle(stdin_write); stdin_write = NULL; }
                    continue;
                }
                ShowWin32Error(i == 0 ? "WriteFile(child stdin)" :
                    (i == 1 ? "WriteFile(parent stdout)" : "WriteFile(parent stderr)"));
                goto cleanup;
            }
            if (!StartRead(read)) { ShowWin32Error(read->name); goto cleanup; }
        }
    }
    {
        DWORD child_exit_code;
        if (!GetExitCodeProcess(process_info.hProcess, &child_exit_code)) { ShowWin32Error("GetExitCodeProcess"); goto cleanup; }
        exit_code = (int)child_exit_code;
    }

cleanup:
    for (i = 0; i < 3; i++) {
        if (reads[i].active) {
            DWORD ignored;
            CancelIoEx(reads[i].source, &reads[i].overlapped);
            GetOverlappedResult(reads[i].source, &reads[i].overlapped, &ignored, TRUE);
        }
        if (reads[i].overlapped.hEvent != NULL) CloseHandle(reads[i].overlapped.hEvent);
    }
    if (process_info.hThread != NULL) CloseHandle(process_info.hThread);
    if (process_info.hProcess != NULL) CloseHandle(process_info.hProcess);
    if (stdout_write != NULL) CloseHandle(stdout_write);
    if (stdout_read != NULL) CloseHandle(stdout_read);
    if (stderr_write != NULL) CloseHandle(stderr_write);
    if (stderr_read != NULL) CloseHandle(stderr_read);
    if (stdin_read != NULL) CloseHandle(stdin_read);
    if (stdin_write != NULL) CloseHandle(stdin_write);
    free(mutable_command_line);
    return exit_code;
}
