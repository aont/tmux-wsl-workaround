#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct StdinThreadContext {
    HANDLE source;
    HANDLE destination;
} StdinThreadContext;

static char const *ShiftCommandLine(char const *cmdline) {
    BOOL backslash_preceding = FALSE, inside_double_quote = FALSE, after_argv0 = FALSE;
    for (int i = 0;; i++) {
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

static DWORD WINAPI StdinThreadProc(LPVOID parameter) {
    StdinThreadContext *context = (StdinThreadContext *)parameter;
    for (;;) {
        BYTE buffer[4096];
        DWORD bytes_read = 0;
        if (!ReadFile(context->source, buffer, sizeof(buffer), &bytes_read, NULL) || bytes_read == 0) break;
        if (!WriteAll(context->destination, buffer, bytes_read)) break;
    }
    if (context->destination != NULL) {
        CloseHandle(context->destination);
        context->destination = NULL;
    }
    return 0;
}

int main(void) {
    char const *child_command_line = ShiftCommandLine(GetCommandLineA());
    char *mutable_command_line = NULL;
    SECURITY_ATTRIBUTES sa;
    HANDLE stdout_read = NULL, stdout_write = NULL, stdin_read = NULL, stdin_write = NULL;
    HANDLE parent_stdin = INVALID_HANDLE_VALUE, parent_stdout = INVALID_HANDLE_VALUE, stdin_thread = NULL;
    StdinThreadContext stdin_context;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    int exit_code = EXIT_FAILURE;

    ZeroMemory(&process_info, sizeof(process_info));
    ZeroMemory(&stdin_context, sizeof(stdin_context));
    if (*child_command_line == '\0') { fprintf(stderr, "No command line to execute was specified.\n"); goto cleanup; }
    mutable_command_line = _strdup(child_command_line);
    if (mutable_command_line == NULL) { fprintf(stderr, "Could not allocate memory for the command line.\n"); goto cleanup; }

    parent_stdin = GetStdHandle(STD_INPUT_HANDLE);
    parent_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (parent_stdin == NULL || parent_stdin == INVALID_HANDLE_VALUE) { ShowWin32Error("GetStdHandle(stdin)"); goto cleanup; }
    if (parent_stdout == NULL || parent_stdout == INVALID_HANDLE_VALUE) { ShowWin32Error("GetStdHandle(stdout)"); goto cleanup; }

    ZeroMemory(&sa, sizeof(sa)); sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) { ShowWin32Error("CreatePipe(stdout)"); goto cleanup; }
    if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) { ShowWin32Error("SetHandleInformation(stdout)"); goto cleanup; }
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) { ShowWin32Error("CreatePipe(stdin)"); goto cleanup; }
    if (!SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) { ShowWin32Error("SetHandleInformation(stdin)"); goto cleanup; }

    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_SHOWMINIMIZED;
    startup_info.hStdInput = stdin_read;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stdout_write;
    if (!CreateProcessA(NULL, mutable_command_line, NULL, NULL, TRUE, CREATE_NEW_CONSOLE,
            NULL, NULL, &startup_info, &process_info)) { ShowWin32Error("CreateProcessA"); goto cleanup; }

    CloseHandle(stdout_write); stdout_write = NULL;
    CloseHandle(stdin_read); stdin_read = NULL;
    CloseHandle(process_info.hThread); process_info.hThread = NULL;
    stdin_context.source = parent_stdin; stdin_context.destination = stdin_write;
    stdin_thread = CreateThread(NULL, 0, StdinThreadProc, &stdin_context, 0, NULL);
    if (stdin_thread == NULL) { ShowWin32Error("CreateThread(stdin)"); goto cleanup; }
    stdin_write = NULL;

    for (;;) {
        BYTE buffer[4096]; DWORD bytes_read = 0;
        if (!ReadFile(stdout_read, buffer, sizeof(buffer), &bytes_read, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            ShowWin32Error("ReadFile(stdout)"); goto cleanup;
        }
        if (bytes_read == 0) break;
        if (!WriteAll(parent_stdout, buffer, bytes_read)) { ShowWin32Error("WriteFile(parent stdout)"); goto cleanup; }
    }
    if (WaitForSingleObject(process_info.hProcess, INFINITE) == WAIT_FAILED) { ShowWin32Error("WaitForSingleObject(child)"); goto cleanup; }
    if (stdin_thread != NULL) {
        CancelSynchronousIo(stdin_thread); WaitForSingleObject(stdin_thread, INFINITE);
        CloseHandle(stdin_thread); stdin_thread = NULL;
    }
    { DWORD child_exit_code = 0;
      if (!GetExitCodeProcess(process_info.hProcess, &child_exit_code)) { ShowWin32Error("GetExitCodeProcess"); goto cleanup; }
      exit_code = (int)child_exit_code; }

cleanup:
    if (stdin_thread != NULL) { CancelSynchronousIo(stdin_thread); WaitForSingleObject(stdin_thread, INFINITE); CloseHandle(stdin_thread); }
    if (process_info.hThread != NULL) CloseHandle(process_info.hThread);
    if (process_info.hProcess != NULL) CloseHandle(process_info.hProcess);
    if (stdout_write != NULL) CloseHandle(stdout_write);
    if (stdout_read != NULL) CloseHandle(stdout_read);
    if (stdin_read != NULL) CloseHandle(stdin_read);
    if (stdin_write != NULL) CloseHandle(stdin_write);
    free(mutable_command_line);
    return exit_code;
}
