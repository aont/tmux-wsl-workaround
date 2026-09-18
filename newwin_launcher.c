#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    char const *child_command_line = ShiftCommandLine(GetCommandLineA());
    char *mutable_command_line = NULL;
    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    int exit_code = EXIT_FAILURE;

    ZeroMemory(&process_info, sizeof(process_info));

    if (*child_command_line == '\0') {
        fprintf(stderr, "No command line to execute was specified.\n");
        goto cleanup;
    }

    mutable_command_line = _strdup(child_command_line);
    if (mutable_command_line == NULL) {
        fprintf(stderr, "Could not allocate memory for the command line.\n");
        goto cleanup;
    }

    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);

    /*
     * Redirection disabled:
     * - Do not duplicate/inherit this process's stdin/stdout/stderr.
     * - Do not use STARTF_USESTDHANDLES.
     *
     * CREATE_NEW_CONSOLE gives the child its own console. Console applications
     * can therefore use the standard handles associated with that new console.
     */
    startup_info.dwFlags = STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_SHOWMINIMIZED;

    if (!CreateProcessA(
            NULL,
            mutable_command_line,
            NULL,
            NULL,
            FALSE,              /* handle inheritance disabled */
            CREATE_NEW_CONSOLE,
            NULL,
            NULL,
            &startup_info,
            &process_info)) {
        ShowWin32Error("CreateProcessA");
        goto cleanup;
    }

    CloseHandle(process_info.hThread);
    process_info.hThread = NULL;

    if (WaitForSingleObject(process_info.hProcess, INFINITE) == WAIT_FAILED) {
        ShowWin32Error("WaitForSingleObject(child)");
        goto cleanup;
    }

    {
        DWORD child_exit_code = 0;
        if (!GetExitCodeProcess(process_info.hProcess, &child_exit_code)) {
            ShowWin32Error("GetExitCodeProcess");
            goto cleanup;
        }
        exit_code = (int)child_exit_code;
    }

cleanup:
    if (process_info.hThread != NULL) CloseHandle(process_info.hThread);
    if (process_info.hProcess != NULL) CloseHandle(process_info.hProcess);
    free(mutable_command_line);
    return exit_code;
}
