/*
 * win32_compat.h - POSIX implementation of generic Win32 host primitives
 *
 * The Xbox kernel HLE (src/kernel) is built on top of a handful of *generic*
 * OS primitives -- threads, events, mutexes, semaphores, interlocked atomics,
 * a heap, timers, time queries. These are NOT Xbox semantics; they are host
 * services that any emulator needs. This layer implements that exact subset
 * honestly on POSIX so the Xbox-specific HLE code above it is unchanged and
 * stays verifiable.
 *
 * Linux-only: on Windows these come from the real <windows.h>.
 */

#ifndef WIN32_COMPAT_H
#define WIN32_COMPAT_H

#if !defined(_WIN32)

#include "xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants --------------------------------------------------------- */
#define WAIT_OBJECT_0        0x00000000u
#define WAIT_ABANDONED_0     0x00000080u
#define WAIT_ABANDONED       0x00000080u
#define WAIT_IO_COMPLETION   0x000000C0u
#define WAIT_TIMEOUT         0x00000102u
#define WAIT_FAILED          0xFFFFFFFFu
#ifndef INFINITE
#define INFINITE             0xFFFFFFFFu
#endif

#define CREATE_SUSPENDED         0x00000004u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000u

#define THREAD_PRIORITY_IDLE          (-15)
#define THREAD_PRIORITY_LOWEST        (-2)
#define THREAD_PRIORITY_BELOW_NORMAL  (-1)
#define THREAD_PRIORITY_NORMAL        0
#define THREAD_PRIORITY_ABOVE_NORMAL  1
#define THREAD_PRIORITY_HIGHEST       2
#define THREAD_PRIORITY_TIME_CRITICAL 15

#define DUPLICATE_CLOSE_SOURCE   0x00000001u
#define DUPLICATE_SAME_ACCESS    0x00000002u

#define WT_EXECUTEONLYONCE       0x00000008u
#define WT_EXECUTEDEFAULT        0x00000000u

/* VirtualAlloc / VirtualProtect */
#define MEM_COMMIT     0x00001000u
#define MEM_RESERVE    0x00002000u
#define MEM_DECOMMIT   0x00004000u
#define MEM_RELEASE    0x00008000u
#define MEM_TOP_DOWN   0x00100000u
#define PAGE_NOACCESS          0x01u
#define PAGE_READONLY          0x02u
#define PAGE_READWRITE         0x04u
#define PAGE_WRITECOPY         0x08u
#define PAGE_EXECUTE           0x10u
#define PAGE_EXECUTE_READ      0x20u
#define PAGE_EXECUTE_READWRITE 0x40u

/* Code-page ids accepted by MultiByteToWideChar (only UTF8/ACP are honoured) */
#define CP_ACP    0u
#define CP_UTF8   65001u

/* ---- Callback typedefs ------------------------------------------------- */
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID lpParameter);
typedef VOID  (WINAPI *PAPCFUNC)(ULONG_PTR Parameter);
typedef VOID  (WINAPI *WAITORTIMERCALLBACK)(PVOID lpParameter, BOOLEAN TimerOrWaitFired);
typedef void  *LPSECURITY_ATTRIBUTES;
typedef void  *PTP_CALLBACK_INSTANCE;
typedef VOID  (WINAPI *PTP_SIMPLE_CALLBACK)(PTP_CALLBACK_INSTANCE Instance, PVOID Context);

/* ---- Last-error -------------------------------------------------------- */
DWORD GetLastError(void);
VOID  SetLastError(DWORD dwErrCode);

/* ---- Interlocked atomics ---------------------------------------------- */
LONG InterlockedIncrement(volatile LONG *Addend);
LONG InterlockedDecrement(volatile LONG *Addend);
LONG InterlockedExchange(volatile LONG *Target, LONG Value);
LONG InterlockedExchangeAdd(volatile LONG *Addend, LONG Value);
LONG InterlockedCompareExchange(volatile LONG *Dest, LONG Exchange, LONG Comparand);
PVOID InterlockedCompareExchangePointer(PVOID volatile *Dest, PVOID Exchange, PVOID Comparand);

/* ---- Critical sections ------------------------------------------------- */
VOID InitializeCriticalSection(LPCRITICAL_SECTION cs);
VOID InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin);
VOID EnterCriticalSection(LPCRITICAL_SECTION cs);
VOID LeaveCriticalSection(LPCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);
VOID DeleteCriticalSection(LPCRITICAL_SECTION cs);

/* ---- Condition variables (paired with a CRITICAL_SECTION) ----------- */
VOID InitializeConditionVariable(PCONDITION_VARIABLE cv);
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms);
VOID WakeConditionVariable(PCONDITION_VARIABLE cv);
VOID WakeAllConditionVariable(PCONDITION_VARIABLE cv);

/* ---- Generic handle lifetime ------------------------------------------ */
BOOL  CloseHandle(HANDLE h);
BOOL  DuplicateHandle(HANDLE srcProc, HANDLE src, HANDLE dstProc,
                      PHANDLE dst, DWORD access, BOOL inherit, DWORD options);

/* fd-backed handle, used by the file-I/O HLE (kernel_file.c). Lets file
 * handles flow through the same CloseHandle path as every other object. */
HANDLE      w32_open_handle(int fd, const char *host_path);
int         w32_handle_fd(HANDLE h);
const char *w32_handle_path(HANDLE h);

/* ---- Events ----------------------------------------------------------- */
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR name);
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name);
BOOL   SetEvent(HANDLE h);
BOOL   ResetEvent(HANDLE h);
BOOL   PulseEvent(HANDLE h);

/* ---- Semaphores ------------------------------------------------------- */
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name);
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name);
BOOL   ReleaseSemaphore(HANDLE h, LONG releaseCount, PLONG previousCount);

/* ---- Mutexes ---------------------------------------------------------- */
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name);
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name);
BOOL   ReleaseMutex(HANDLE h);

/* ---- Waiting ---------------------------------------------------------- */
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms);
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll,
                               DWORD ms, BOOL alertable);

/* ---- Threads ---------------------------------------------------------- */
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId);
VOID   ExitThread(DWORD exitCode);
BOOL   GetExitCodeThread(HANDLE h, LPDWORD exitCode);
DWORD  ResumeThread(HANDLE h);
DWORD  SuspendThread(HANDLE h);
BOOL   TerminateThread(HANDLE h, DWORD exitCode);
DWORD  GetCurrentThreadId(void);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
DWORD  GetCurrentProcessId(void);
BOOL   SetThreadPriority(HANDLE h, int priority);
int    GetThreadPriority(HANDLE h);
VOID   SwitchToThread(void);
DWORD  QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data);

/* ---- Sleep ------------------------------------------------------------ */
VOID  Sleep(DWORD ms);
DWORD SleepEx(DWORD ms, BOOL alertable);

/* ---- Timer queues ----------------------------------------------------- */
HANDLE CreateTimerQueue(void);
BOOL   DeleteTimerQueue(HANDLE timerQueue);
BOOL   CreateTimerQueueTimer(PHANDLE newTimer, HANDLE timerQueue,
                             WAITORTIMERCALLBACK callback, PVOID param,
                             DWORD dueTime, DWORD period, ULONG flags);
BOOL   DeleteTimerQueueTimer(HANDLE timerQueue, HANDLE timer, HANDLE completionEvent);
BOOL   ChangeTimerQueueTimer(HANDLE timerQueue, HANDLE timer, ULONG dueTime, ULONG period);
BOOL   TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                   PVOID context, PVOID env);

/* ---- Heap ------------------------------------------------------------- */
HANDLE GetProcessHeap(void);
HANDLE HeapCreate(DWORD options, SIZE_T initial, SIZE_T maximum);
BOOL   HeapDestroy(HANDLE heap);
LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes);
BOOL   HeapFree(HANDLE heap, DWORD flags, LPVOID mem);
SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem);
#define HEAP_ZERO_MEMORY 0x00000008u

/* ---- Virtual memory --------------------------------------------------- */
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect);
BOOL   VirtualFree(LPVOID address, SIZE_T size, DWORD freeType);
BOOL   VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect);

/* ---- Time ------------------------------------------------------------- */
VOID  GetSystemTimeAsFileTime(LPFILETIME ft);
VOID  GetSystemTime(LPSYSTEMTIME st);
VOID  GetLocalTime(LPSYSTEMTIME st);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
BOOL  QueryPerformanceCounter(PLARGE_INTEGER count);
BOOL  QueryPerformanceFrequency(PLARGE_INTEGER freq);

/* ---- Misc ------------------------------------------------------------- */
VOID  OutputDebugStringA(LPCSTR str);
VOID  OutputDebugStringW(LPCWSTR str);
VOID  ExitProcess(UINT exitCode);
BOOL  TerminateProcess(HANDLE process, UINT exitCode);
VOID  SecureZeroMemory(PVOID ptr, SIZE_T cnt);
unsigned int _clearfp(void);   /* clear pending FPU exception flags */

/* MSVC stack alloca alias. <alloca.h> provides the underlying function. */
#include <alloca.h>
#define _alloca(n) alloca(n)

/* ---- Win32 file API (POSIX-backed) ----------------------------------- */
#define GENERIC_READ             0x80000000u
#define GENERIC_WRITE            0x40000000u
#define GENERIC_EXECUTE          0x20000000u
#define GENERIC_ALL              0x10000000u
#define FILE_SHARE_READ          0x00000001u
#define FILE_SHARE_WRITE         0x00000002u
#define FILE_SHARE_DELETE        0x00000004u
#define CREATE_NEW               1
#define CREATE_ALWAYS            2
#define OPEN_EXISTING            3
#define OPEN_ALWAYS              4
#define TRUNCATE_EXISTING        5
#define FILE_ATTRIBUTE_READONLY  0x00000001u
#define FILE_ATTRIBUTE_HIDDEN    0x00000002u
#define FILE_ATTRIBUTE_SYSTEM    0x00000004u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_ARCHIVE   0x00000020u
#define FILE_ATTRIBUTE_NORMAL    0x00000080u
#define INVALID_FILE_SIZE        0xFFFFFFFFu

/* The file API lives in win32_compat_file.c (docs Update 677): Windows paths
 * ("Z:\home\..." maps to "/home/...", backslashes), case-insensitive lookup
 * of path components, positioned I/O through OVERLAPPED. */
#define INVALID_FILE_ATTRIBUTES  ((DWORD)-1)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define FILE_ATTRIBUTE_DEVICE    0x00000040u
#define FILE_ATTRIBUTE_SPARSE_FILE 0x00000200u
#define FILE_READ_DATA           0x0001u
#define FILE_LIST_DIRECTORY      0x0001u
#define FILE_WRITE_DATA          0x0002u
#define FILE_ADD_FILE            0x0002u
#define FILE_APPEND_DATA         0x0004u
#define FILE_READ_EA             0x0008u
#define FILE_WRITE_EA            0x0010u
#define FILE_EXECUTE             0x0020u
#define FILE_TRAVERSE            0x0020u
#define FILE_READ_ATTRIBUTES     0x0080u
#define FILE_WRITE_ATTRIBUTES    0x0100u
#ifndef DELETE
#define DELETE                   0x00010000u
#endif
#ifndef SYNCHRONIZE
#define SYNCHRONIZE              0x00100000u
#endif
#define FILE_ALL_ACCESS          0x001F01FFu
#define FILE_GENERIC_READ        0x00120089u
#define FILE_GENERIC_WRITE       0x00120116u
#define FILE_FLAG_WRITE_THROUGH       0x80000000u
#define FILE_FLAG_OVERLAPPED          0x40000000u
#define FILE_FLAG_NO_BUFFERING        0x20000000u
#define FILE_FLAG_RANDOM_ACCESS       0x10000000u
#define FILE_FLAG_SEQUENTIAL_SCAN     0x08000000u
#define FILE_FLAG_DELETE_ON_CLOSE     0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS    0x02000000u
#define FILE_NAME_NORMALIZED     0x0u
#define FILE_NAME_OPENED         0x8u
#define VOLUME_NAME_DOS          0x0u
#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_COPY_ALLOWED     0x2u
#define MOVEFILE_WRITE_THROUGH    0x8u
#define FSCTL_SET_SPARSE         0x000900C4u
#define CSIDL_APPDATA            0x001A
#define CSIDL_LOCAL_APPDATA      0x001C
#define CSIDL_PERSONAL           0x0005
#define ERROR_HANDLE_DISK_FULL   39u
#define ERROR_DIR_NOT_EMPTY      145u
#define ERROR_BAD_PATHNAME       161u
#define ERROR_DISK_FULL          112u
#define ERROR_SHARING_VIOLATION  32u
#define ERROR_NEGATIVE_SEEK      131u
#define ERROR_DIRECTORY          267u
#ifndef ERROR_TIMEOUT
#define ERROR_TIMEOUT            1460u
#endif

typedef struct _OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
typedef VOID (*LPOVERLAPPED_COMPLETION_ROUTINE)(DWORD err, DWORD n, LPOVERLAPPED ov);

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks;
    DWORD nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
typedef WIN32_FIND_DATAA WIN32_FIND_DATA, *LPWIN32_FIND_DATA;

typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel }
    GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef enum _FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo = 0, FileStandardInfo = 1, FileNameInfo = 2, FileRenameInfo = 3,
    FileDispositionInfo = 4, FileAllocationInfo = 5, FileEndOfFileInfo = 6
} FILE_INFO_BY_HANDLE_CLASS;
typedef struct _FILE_DISPOSITION_INFO { BOOLEAN DeleteFile; } FILE_DISPOSITION_INFO;
typedef struct _FILE_ALLOCATION_INFO { LARGE_INTEGER AllocationSize; } FILE_ALLOCATION_INFO;
typedef struct _FILE_END_OF_FILE_INFO { LARGE_INTEGER EndOfFile; } FILE_END_OF_FILE_INFO;
typedef struct _FILE_BASIC_INFO {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    DWORD FileAttributes;
} FILE_BASIC_INFO;

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ);
HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ);
BOOL   ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD nread, LPOVERLAPPED ov);
BOOL   WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD nwritten, LPOVERLAPPED ov);
DWORD  GetFileSize(HANDLE h, LPDWORD high);
BOOL   GetFileSizeEx(HANDLE h, PLARGE_INTEGER size);
BOOL   FlushFileBuffers(HANDLE h);
DWORD  SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method);
BOOL   SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newpos, DWORD method);
BOOL   SetEndOfFile(HANDLE h);
BOOL   GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info);
BOOL   SetFileInformationByHandle(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls,
                                  LPVOID info, DWORD size);
BOOL   SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w);
DWORD  GetFinalPathNameByHandleA(HANDLE h, LPSTR buf, DWORD n, DWORD flags);
DWORD  GetFinalPathNameByHandleW(HANDLE h, LPWSTR buf, DWORD n, DWORD flags);
BOOL   CancelIo(HANDLE h);
BOOL   DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_size, LPVOID out,
                       DWORD out_size, LPDWORD returned, LPOVERLAPPED ov);
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data);
BOOL   FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data);
BOOL   FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW data);
BOOL   FindClose(HANDLE h);
DWORD  GetFileAttributesA(LPCSTR path);
DWORD  GetFileAttributesW(LPCWSTR path);
BOOL   GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID info);
BOOL   GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID info);
BOOL   SetFileAttributesA(LPCSTR path, DWORD attr);
BOOL   SetFileAttributesW(LPCWSTR path, DWORD attr);
BOOL   CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL   CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa);
BOOL   RemoveDirectoryA(LPCSTR path);
BOOL   RemoveDirectoryW(LPCWSTR path);
BOOL   DeleteFileA(LPCSTR path);
BOOL   DeleteFileW(LPCWSTR path);
BOOL   MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL   MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL   MoveFileA(LPCSTR from, LPCSTR to);
BOOL   CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
BOOL   CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists);
DWORD  GetModuleFileNameA(HMODULE mod, LPSTR buf, DWORD n);
DWORD  GetModuleFileNameW(HMODULE mod, LPWSTR buf, DWORD n);
DWORD  GetCurrentDirectoryA(DWORD n, LPSTR buf);
DWORD  GetCurrentDirectoryW(DWORD n, LPWSTR buf);
BOOL   SetCurrentDirectoryA(LPCSTR path);
BOOL   SetCurrentDirectoryW(LPCWSTR path);
DWORD  GetFullPathNameA(LPCSTR path, DWORD n, LPSTR buf, LPSTR *file_part);
BOOL   GetDiskFreeSpaceExA(LPCSTR path, PULARGE_INTEGER avail, PULARGE_INTEGER total,
                           PULARGE_INTEGER free_bytes);
BOOL   GetDiskFreeSpaceExW(LPCWSTR path, PULARGE_INTEGER avail, PULARGE_INTEGER total,
                           PULARGE_INTEGER free_bytes);
HRESULT SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR path);
HRESULT SHGetFolderPathA(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path);
/* host path of a Windows-style path ("Z:\a\b" or "C:\x" or "\a" -> "/a/b"),
 * resolving existing components case-insensitively; returns out */
char *w32_host_path(const char *win_path, char *out, size_t out_size);
/* the Windows-style form ("Z:\...") of a host path; returns out */
char *w32_win_path(const char *host_path, char *out, size_t out_size);

/* ---- Keyboard + window helpers (stubs on POSIX) --------------------- */
SHORT GetAsyncKeyState(int vKey);
HWND  FindWindowA(LPCSTR className, LPCSTR windowName);
HWND  GetActiveWindow(void);
BOOL  SetWindowTextA(HWND hwnd, LPCSTR text);
int   GetWindowTextA(HWND hwnd, LPSTR text, int count);
typedef BOOL (CALLBACK *WNDENUMPROC)(HWND hwnd, LPARAM lParam);
BOOL  EnumWindows(WNDENUMPROC enumProc, LPARAM lParam);

/* MessageBox + flags (stderr stub on POSIX). */
int   MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type);
#define MB_OK              0x00000000u
#define MB_OKCANCEL        0x00000001u
#define MB_ICONERROR       0x00000010u
#define MB_ICONWARNING     0x00000030u
#define MB_ICONINFORMATION 0x00000040u
#define MB_ICONQUESTION    0x00000020u
#define MB_OKCANCEL        0x00000001u
#define MB_YESNO           0x00000004u
#define MB_DEFBUTTON2      0x00000100u
#define MB_TOPMOST         0x00040000u
#define MB_SETFOREGROUND   0x00010000u
#define IDOK     1
#define IDCANCEL 2
#define IDYES    6
#define IDNO     7

/* Win32 virtual-key codes (subset the game checks via GetAsyncKeyState) */
#define VK_LBUTTON   0x01
#define VK_RBUTTON   0x02
#define VK_CANCEL    0x03
#define VK_MBUTTON   0x04
#define VK_BACK      0x08
#define VK_TAB       0x09
#define VK_RETURN    0x0D
#define VK_SHIFT     0x10
#define VK_CONTROL   0x11
#define VK_MENU      0x12
#define VK_PAUSE     0x13
#define VK_CAPITAL   0x14
#define VK_ESCAPE    0x1B
#define VK_SPACE     0x20
#define VK_PRIOR     0x21
#define VK_NEXT      0x22
#define VK_END       0x23
#define VK_HOME      0x24
#define VK_LEFT      0x25
#define VK_UP        0x26
#define VK_RIGHT     0x27
#define VK_DOWN      0x28
#define VK_INSERT    0x2D
#define VK_DELETE    0x2E
#define VK_NUMPAD0   0x60
#define VK_NUMPAD1   0x61
#define VK_NUMPAD2   0x62
#define VK_NUMPAD3   0x63
#define VK_NUMPAD4   0x64
#define VK_NUMPAD5   0x65
#define VK_NUMPAD6   0x66
#define VK_NUMPAD7   0x67
#define VK_NUMPAD8   0x68
#define VK_NUMPAD9   0x69
#define VK_MULTIPLY  0x6A
#define VK_ADD       0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT  0x6D
#define VK_DECIMAL   0x6E
#define VK_DIVIDE    0x6F
#define VK_F1        0x70
#define VK_F2        0x71
#define VK_F3        0x72
#define VK_F4        0x73
#define VK_F5        0x74
#define VK_F6        0x75
#define VK_F7        0x76
#define VK_F8        0x77
#define VK_F9        0x78
#define VK_F10       0x79
#define VK_F11       0x7A
#define VK_F12       0x7B
#define VK_NUMLOCK   0x90
#define VK_SCROLL    0x91
#define VK_LSHIFT    0xA0
#define VK_RSHIFT    0xA1
#define VK_LCONTROL  0xA2
#define VK_RCONTROL  0xA3
#define VK_LMENU     0xA4
#define VK_RMENU     0xA5
#define VK_OEM_PLUS  0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE

/* ---- Message-loop shims (PeekMessage etc. -- always "no messages") ---- */
typedef struct tagMSG {
    HWND     hwnd;
    UINT     message;
    WPARAM   wParam;
    LPARAM   lParam;
    DWORD    time;
    POINT    pt;
} MSG, *PMSG, *LPMSG;
#define PM_NOREMOVE  0x0000
#define PM_REMOVE    0x0001
#define WM_QUIT      0x0012
#define WM_KEYDOWN   0x0100
#define WM_KEYUP     0x0101
#define WM_CHAR      0x0102
BOOL    PeekMessageA(LPMSG msg, HWND wnd, UINT min, UINT max, UINT flags);
BOOL    TranslateMessage(const MSG *msg);
LRESULT DispatchMessageA(const MSG *msg);

/* ---- XInput (stub on POSIX -- real gamepad goes through input_compat) -- */
typedef struct _XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
typedef struct _XINPUT_STATE {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;
typedef struct _XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
} XINPUT_VIBRATION, *PXINPUT_VIBRATION;
typedef struct _XINPUT_CAPABILITIES {
    BYTE Type, SubType;
    WORD Flags;
    XINPUT_GAMEPAD Gamepad;
    XINPUT_VIBRATION Vibration;
} XINPUT_CAPABILITIES;
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state);
DWORD XInputSetState(DWORD idx, XINPUT_VIBRATION *v);
DWORD XInputGetCapabilities(DWORD idx, DWORD flags, XINPUT_CAPABILITIES *caps);
/* fed by the host window thread (src/video/fb_present_sdl.c) */
void  w32_input_set_key(int vk, int down);
void  w32_input_set_pad(int idx, const XINPUT_STATE *st, int connected);
LONG  w32_input_get_rumble(int idx, XINPUT_VIBRATION *v);
void  w32_set_foreground_window(HWND h);
/* affinity of threads CreateThread starts from now on (0 = the process's
 * initial affinity); see win32_compat.c */
void  w32_set_default_thread_affinity(DWORD_PTR mask);
#include <pthread.h>
int   w32_thread_pthread(HANDLE h, pthread_t *out);
SHORT GetKeyState(int vKey);
HWND  GetForegroundWindow(void);
DWORD GetWindowThreadProcessId(HWND h, LPDWORD pid);

#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000
int   MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR mb, int mbCount,
                          LPWSTR wide, int wideCount);
int   WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR wide, int wideCount,
                          LPSTR mb, int mbCount, LPCSTR defChar, PBOOL usedDef);

/* ---- Win32 error codes (subset used by RtlNtStatusToDosError) --------- */
#define ERROR_SUCCESS                 0u
#define ERROR_INVALID_FUNCTION        1u
#define ERROR_FILE_NOT_FOUND          2u
#define ERROR_PATH_NOT_FOUND          3u
#define ERROR_ACCESS_DENIED           5u
#define ERROR_INVALID_HANDLE          6u
#define ERROR_NOT_ENOUGH_MEMORY       8u
#define ERROR_NO_MORE_FILES           18u
#define ERROR_GEN_FAILURE             31u
#define ERROR_HANDLE_EOF              38u
#define ERROR_NOT_SUPPORTED           50u
#define ERROR_FILE_EXISTS             80u
#define ERROR_INVALID_PARAMETER       87u
#define ERROR_CALL_NOT_IMPLEMENTED    120u
#define ERROR_INSUFFICIENT_BUFFER     122u
#define ERROR_ALREADY_EXISTS          183u
#define ERROR_MORE_DATA               234u
#define ERROR_NOT_OWNER               288u
#define ERROR_MR_MID_NOT_FOUND        317u
#define ERROR_IO_PENDING              997u
#define ERROR_CANCELLED               1223u
#define ERROR_NO_SYSTEM_RESOURCES     1450u
#define ERROR_COMMITMENT_LIMIT        1455u
#define ERROR_DEVICE_NOT_CONNECTED    1167u

/* ---- COM HRESULT codes ------------------------------------------------ */
#ifndef S_OK
#define S_OK            ((HRESULT)0x00000000L)
#define S_FALSE         ((HRESULT)0x00000001L)
#define E_NOTIMPL       ((HRESULT)0x80004001L)
#define E_NOINTERFACE   ((HRESULT)0x80004002L)
#define E_POINTER       ((HRESULT)0x80004003L)
#define E_ABORT         ((HRESULT)0x80004004L)
#define E_FAIL          ((HRESULT)0x80004005L)
#define E_ACCESSDENIED  ((HRESULT)0x80070005L)
#define E_HANDLE        ((HRESULT)0x80070006L)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000EL)
#define E_INVALIDARG    ((HRESULT)0x80070057L)
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)
#endif

/* ---- Exception records (SEH compile-shim; not yet emulated) ----------- */
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD {
    DWORD     ExceptionCode;
    DWORD     ExceptionFlags;
    struct _EXCEPTION_RECORD *ExceptionRecord;
    PVOID     ExceptionAddress;
    DWORD     NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
VOID RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args);

/* Win32 VEH on POSIX (docs Update 677): SIGSEGV/SIGBUS/SIGILL/SIGFPE are
 * turned into an EXCEPTION_RECORD + CONTEXT (xbox_winnt.h), the registered
 * handlers run in order, and the CONTEXT they may have changed (MMIO
 * emulation steps the PC) is written back to the signal's ucontext. */
typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT          ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS info);
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS info);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f);
PVOID AddVectoredExceptionHandler(ULONG FirstHandler, PVECTORED_EXCEPTION_HANDLER Handler);
ULONG RemoveVectoredExceptionHandler(PVOID Handle);

#define EXCEPTION_CONTINUE_SEARCH       0
#define EXCEPTION_EXECUTE_HANDLER       1
#define EXCEPTION_CONTINUE_EXECUTION  (-1)
#define EXCEPTION_ACCESS_VIOLATION       0xC0000005u
#define EXCEPTION_STACK_OVERFLOW         0xC00000FDu
#define EXCEPTION_ILLEGAL_INSTRUCTION    0xC000001Du
#define EXCEPTION_INT_DIVIDE_BY_ZERO     0xC0000094u
#define EXCEPTION_FLT_DIVIDE_BY_ZERO     0xC000008Eu
#define EXCEPTION_FLT_INVALID_OPERATION  0xC0000090u
#define EXCEPTION_FLT_STACK_CHECK        0xC0000092u
#define EXCEPTION_FLT_OVERFLOW           0xC0000091u
#define EXCEPTION_FLT_UNDERFLOW          0xC0000093u
#define EXCEPTION_FLT_INEXACT_RESULT     0xC000008Fu
#define EXCEPTION_FLT_DENORMAL_OPERAND   0xC000008Du

/* ---- Memory query ----------------------------------------------------- */
typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID  BaseAddress;
    PVOID  AllocationBase;
    DWORD  AllocationProtect;
    SIZE_T RegionSize;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length);

typedef struct _MEMORYSTATUSEX {
    DWORD     dwLength;
    DWORD     dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX buffer);

/* ---- File mapping (memfd-backed; gives true aliased mirror views) ----- */
#define FILE_MAP_COPY        0x00000001u
#define FILE_MAP_WRITE       0x00000002u
#define FILE_MAP_READ        0x00000004u
#define FILE_MAP_ALL_ACCESS  0x000F001Fu
HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name);
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name);
LPVOID MapViewOfFile(HANDLE mapping, DWORD access,
                     DWORD offHigh, DWORD offLow, SIZE_T count);
LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access,
                       DWORD offHigh, DWORD offLow, SIZE_T count, LPVOID baseAddr);
BOOL   UnmapViewOfFile(LPCVOID baseAddr);
#define CreateFileMapping CreateFileMappingA

/* ---- Aligned allocation ---------------------------------------------- */
void *_aligned_malloc(SIZE_T size, SIZE_T alignment);
void  _aligned_free(void *ptr);

/* ---- Case-insensitive string compare --------------------------------- */
int _stricmp(const char *a, const char *b);
int _strnicmp(const char *a, const char *b, SIZE_T n);

/* ---- Wide-string helpers (operate on the 16-bit Xbox WCHAR) ----------
 * Named xbox_wcs* (not macros over wcs*) so they never collide with the
 * 32-bit-wchar_t CRT functions in <wchar.h>. On Windows xbox_winnt.h maps
 * these names to the real CRT functions. */
SIZE_T xbox_wcslen(const WCHAR *s);
int    xbox_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n);
WCHAR *xbox_wcscat(WCHAR *dst, const WCHAR *src);
WCHAR *xbox_wcscpy(WCHAR *dst, const WCHAR *src);

/* ---- Time conversion ------------------------------------------------- */
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft);
BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st);

/* ---- Multimedia (winmm) shim: waveOut stubs that always report failure.
 * The APU has a waveOut fallback path; on Linux it just stays inactive
 * and the (real) SDL2 audio path will replace it later. */
typedef DWORD  MMRESULT;
typedef void  *HWAVEOUT;
#define MMSYSERR_NOERROR     0
#define MMSYSERR_INVALPARAM 11
#define WAVE_FORMAT_PCM      1
#define WAVE_MAPPER          ((UINT)-1)
#define CALLBACK_NULL        0x00000000
#define WHDR_DONE            0x00000001
#define WHDR_INQUEUE         0x00000010

typedef struct tWAVEFORMATEX {
    WORD  wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD  nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX;

typedef struct tWAVEHDR {
    LPSTR     lpData;
    DWORD     dwBufferLength;
    DWORD     dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD     dwFlags;
    DWORD     dwLoops;
    struct tWAVEHDR *lpNext;
    DWORD_PTR reserved;
} WAVEHDR;

static inline MMRESULT waveOutOpen(HWAVEOUT *h, UINT id, const WAVEFORMATEX *f,
                                   DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{ (void)h;(void)id;(void)f;(void)cb;(void)inst;(void)flags; return MMSYSERR_INVALPARAM; }
static inline MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{ (void)h;(void)hdr;(void)sz; return MMSYSERR_NOERROR; }
static inline MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{ (void)h;(void)hdr;(void)sz; return MMSYSERR_NOERROR; }
static inline MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{ (void)h;(void)hdr;(void)sz; return MMSYSERR_NOERROR; }
static inline MMRESULT waveOutReset(HWAVEOUT h) { (void)h; return MMSYSERR_NOERROR; }
static inline MMRESULT waveOutClose(HWAVEOUT h) { (void)h; return MMSYSERR_NOERROR; }

#ifdef __cplusplus
}
#endif

/* ===================================================================== */
/* Additions for the SCDA ARM port (docs Update 677, win32_compat_ext.c)  */
/* ===================================================================== */
#include <stdio.h>
#include <strings.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef DWORD_PTR *PDWORD_PTR;

/* slim reader/writer lock -- a pthread_rwlock_t behind one pointer */
typedef struct { PVOID Ptr; } SRWLOCK, *PSRWLOCK;
#define SRWLOCK_INIT { NULL }
VOID InitializeSRWLock(PSRWLOCK l);
VOID AcquireSRWLockExclusive(PSRWLOCK l);
VOID AcquireSRWLockShared(PSRWLOCK l);
VOID ReleaseSRWLockExclusive(PSRWLOCK l);
VOID ReleaseSRWLockShared(PSRWLOCK l);
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK l);
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK l);
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags);

/* one-time initialisation */
typedef struct { volatile LONG State; PVOID Context; } INIT_ONCE, *PINIT_ONCE, *LPINIT_ONCE;
#define INIT_ONCE_STATIC_INIT { 0, NULL }
typedef BOOL (*PINIT_ONCE_FN)(PINIT_ONCE once, PVOID param, PVOID *context);
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, LPVOID *context);

/* thread access rights and context */
#ifndef THREAD_ALL_ACCESS
#define THREAD_TERMINATE            0x0001u
#define THREAD_SUSPEND_RESUME       0x0002u
#define THREAD_GET_CONTEXT          0x0008u
#define THREAD_SET_CONTEXT          0x0010u
#define THREAD_SET_INFORMATION      0x0020u
#define THREAD_QUERY_INFORMATION    0x0040u
#define THREAD_ALL_ACCESS           0x1FFFFFu
#endif
#ifndef THREAD_PRIORITY_ERROR_RETURN
#define THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF
#endif
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
BOOL GetThreadContext(HANDLE thread, PCONTEXT ctx);   /* FALSE: no thread hijack on POSIX */
BOOL SetThreadContext(HANDLE thread, const CONTEXT *ctx);
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
BOOL SetThreadStackGuarantee(PULONG size);
HRESULT SetThreadDescription(HANDLE thread, LPCWSTR name);
BOOL GetProcessAffinityMask(HANDLE proc, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask);
BOOL SetPriorityClass(HANDLE proc, DWORD cls);
#ifndef HIGH_PRIORITY_CLASS
#define NORMAL_PRIORITY_CLASS 0x20u
#define HIGH_PRIORITY_CLASS   0x80u
#define ABOVE_NORMAL_PRIORITY_CLASS 0x8000u
#endif

/* waitable timers (an event set by a helper thread at the due time) */
#ifndef TIMER_ALL_ACCESS
#define TIMER_ALL_ACCESS 0x1F0003u
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2u
#define CREATE_WAITABLE_TIMER_MANUAL_RESET    0x1u
#endif
typedef VOID (*PTIMERAPCROUTINE)(LPVOID arg, DWORD low, DWORD high);
HANDLE CreateWaitableTimerA(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCSTR name);
HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manual, LPCWSTR name);
HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE t, const LARGE_INTEGER *due, LONG period_ms,
                      PTIMERAPCROUTINE apc, LPVOID arg, BOOL resume);
BOOL CancelWaitableTimer(HANDLE t);
#define SetWaitableTimerEx(t, due, period, apc, arg, wake, delay)     SetWaitableTimer((t), (due), (period), (apc), (arg), FALSE)

/* processes: the title relaunch starts a new instance of this executable */
typedef struct _STARTUPINFOA { DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars,
          dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2;
    LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; }
    STARTUPINFOA, *LPSTARTUPINFOA;
typedef struct _STARTUPINFOW { DWORD cb; LPWSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars,
          dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2;
    LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; }
    STARTUPINFOW, *LPSTARTUPINFOW;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess, hThread;
    DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
BOOL CreateProcessA(LPCSTR app, LPSTR cmdline, LPSECURITY_ATTRIBUTES pa,
                    LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
                    LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi);
BOOL CreateProcessW(LPCWSTR app, LPWSTR cmdline, LPSECURITY_ATTRIBUTES pa,
                    LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
                    LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
BOOL GetExitCodeProcess(HANDLE proc, LPDWORD code);
LPSTR GetCommandLineA(void);
LPWSTR GetCommandLineW(void);

/* memory constants */
#ifndef MEM_FREE
#define MEM_FREE    0x10000u
#define MEM_PRIVATE 0x20000u
#define MEM_MAPPED  0x40000u
#define MEM_IMAGE   0x1000000u
#endif
#ifndef MAXIMUM_WAIT_OBJECTS
#define MAXIMUM_WAIT_OBJECTS 64
#endif
#ifndef FILE_BEGIN
#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2
#endif

/* MSVC "secure" CRT on POSIX */
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
#ifndef _SH_DENYNO
#define _SH_DENYNO 0x40
#define _SH_DENYWR 0x20
#endif
typedef int errno_t;
errno_t strcpy_s(char *dst, size_t size, const char *src);
errno_t strncpy_s(char *dst, size_t size, const char *src, size_t count);
errno_t strcat_s(char *dst, size_t size, const char *src);
errno_t strncat_s(char *dst, size_t size, const char *src, size_t count);
int sprintf_s(char *dst, size_t size, const char *fmt, ...);
int _snprintf_s(char *dst, size_t size, size_t count, const char *fmt, ...);
int vsprintf_s(char *dst, size_t size, const char *fmt, va_list ap);
int _vsnprintf_s(char *dst, size_t size, size_t count, const char *fmt, va_list ap);
errno_t fopen_s(FILE **fp, const char *path, const char *mode);
FILE *_fsopen(const char *path, const char *mode, int shflag);
errno_t _putenv_s(const char *name, const char *value);
errno_t _dupenv_s(char **buf, size_t *len, const char *name);
errno_t getenv_s(size_t *len, char *buf, size_t size, const char *name);
errno_t memcpy_s(void *dst, size_t dsize, const void *src, size_t count);
/* _stricmp/_strnicmp are real functions (win32_compat.c); a macro onto
 * strcasecmp turned their definitions into a self-recursive strcasecmp */
#ifndef stricmp
#define stricmp   _stricmp
#define strnicmp  _strnicmp
#endif
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef __debugbreak
#define __debugbreak() __builtin_trap()
#endif

/* 64-bit interlocked operations */
static inline LONG64 InterlockedIncrement64(volatile LONG64 *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedDecrement64(volatile LONG64 *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedExchange64(volatile LONG64 *p, LONG64 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedExchangeAdd64(volatile LONG64 *p, LONG64 v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedAdd64(volatile LONG64 *p, LONG64 v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
static inline LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 x, LONG64 c)
{
    __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return c;
}
/* MSVC _Interlocked* intrinsics. LONG is 32 bit as on Windows; callers that
 * cast to "long volatile *" (64 bit on LP64) still get a 32-bit operation,
 * which is what their Windows build does. */
#define _InterlockedOr(p, v)    __atomic_fetch_or((volatile LONG *)(p), (LONG)(v), __ATOMIC_SEQ_CST)
#define _InterlockedAnd(p, v)   __atomic_fetch_and((volatile LONG *)(p), (LONG)(v), __ATOMIC_SEQ_CST)
#define _InterlockedExchange(p, v) __atomic_exchange_n((volatile LONG *)(p), (LONG)(v), __ATOMIC_SEQ_CST)
#define _InterlockedIncrement(p) __atomic_add_fetch((volatile LONG *)(p), 1, __ATOMIC_SEQ_CST)
#define _InterlockedDecrement(p) __atomic_sub_fetch((volatile LONG *)(p), 1, __ATOMIC_SEQ_CST)
#define _InterlockedExchangeAdd(p, v) __atomic_fetch_add((volatile LONG *)(p), (LONG)(v), __ATOMIC_SEQ_CST)
#define _InterlockedCompareExchange(p, x, c) InterlockedCompareExchange((volatile LONG *)(p), (LONG)(x), (LONG)(c))
#define _InterlockedIncrement64(p) InterlockedIncrement64((volatile LONG64 *)(p))
#define _InterlockedExchangeAdd64(p, v) InterlockedExchangeAdd64((volatile LONG64 *)(p), (LONG64)(v))
#define _InterlockedExchange64(p, v) InterlockedExchange64((volatile LONG64 *)(p), (LONG64)(v))
#define _InterlockedCompareExchange64(p, x, c) InterlockedCompareExchange64((volatile LONG64 *)(p), (LONG64)(x), (LONG64)(c))
static inline PVOID InterlockedExchangePointer(PVOID volatile *p, PVOID v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
#ifndef _ReturnAddress
#define _ReturnAddress() __builtin_return_address(0)
#endif
USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG count, PVOID *frames, PULONG hash);
BOOL   GetCursorPos(LPPOINT p);
BOOL   ScreenToClient(HWND hwnd, LPPOINT p);
#ifndef _ReadWriteBarrier
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")
#endif
#ifndef MemoryBarrier
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
#ifndef YieldProcessor
#if defined(__aarch64__)
#define YieldProcessor() __asm__ __volatile__("yield")
#elif defined(__x86_64__) || defined(__i386__)
#define YieldProcessor() __builtin_ia32_pause()
#else
#define YieldProcessor() ((void)0)
#endif
#endif

/* WaitOnAddress family on futexes */
BOOL  WaitOnAddress(volatile VOID *addr, PVOID compare, SIZE_T size, DWORD ms);
VOID  WakeByAddressSingle(PVOID addr);
VOID  WakeByAddressAll(PVOID addr);

/* further process / thread / debug calls */
DWORD   GetThreadId(HANDLE thread);
HMODULE GetModuleHandleA(LPCSTR name);
HMODULE GetModuleHandleW(LPCWSTR name);
void   *GetProcAddress(HMODULE mod, LPCSTR name);
/* shared objects through dlopen; a Windows-style path is mapped */
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x8u
HMODULE LoadLibraryA(LPCSTR path);
HMODULE LoadLibraryExW(LPCWSTR path, HANDLE file, DWORD flags);
BOOL    FreeLibrary(HMODULE mod);
BOOL    SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
VOID    GetSystemTimePreciseAsFileTime(LPFILETIME ft);
BOOL    IsDebuggerPresent(void);
VOID    DebugBreak(void);
BOOL    IsWindowVisible(HWND hwnd);
BOOL    ShowWindow(HWND hwnd, int cmd);
HWND    GetConsoleWindow(void);
DWORD   GetConsoleProcessList(LPDWORD list, DWORD count);
BOOL    GetProcessTimes(HANDLE process, LPFILETIME creation, LPFILETIME exit_time,
                        LPFILETIME kernel, LPFILETIME user);
#ifndef SW_HIDE
#define SW_HIDE       0
#define SW_SHOWNORMAL 1
#define SW_SHOW       5
#endif
#ifndef PAGE_GUARD
#define PAGE_GUARD    0x100u
#endif
BOOL    AllowSetForegroundWindow(DWORD pid);
int     SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, LPSECURITY_ATTRIBUTES sa);
errno_t _strlwr_s(char *s, size_t size);
#ifndef strtok_s
#define strtok_s strtok_r
#endif

/* MSVC printf semantics ("%S" = 16-bit wide string, I64 prefixes) */
int w32_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int w32_vfprintf(FILE *f, const char *fmt, va_list ap);
int w32_fprintf(FILE *f, const char *fmt, ...);
/* fopen with a Windows-style path (see w32_host_path) */
FILE *w32_fopen(const char *path, const char *mode);
#ifndef W32_NO_FOPEN_MACRO
#define fopen w32_fopen
#endif

/* 16-bit wide CRT. With -fshort-wchar (POSIX builds of this tree) L"..." is
 * 16 bit like WCHAR, and the wcs* names the runtime uses are routed here,
 * away from glibc's 4-byte implementations. */
#if defined(__SIZEOF_WCHAR_T__) && __SIZEOF_WCHAR_T__ == 2
#include <wchar.h>
int    xbox_wcscmp(const WCHAR *a, const WCHAR *b);
int    xbox_wcsicmp(const WCHAR *a, const WCHAR *b);
int    xbox_wcsnicmp(const WCHAR *a, const WCHAR *b, size_t n);
WCHAR *xbox_wcschr(const WCHAR *s, WCHAR c);
WCHAR *xbox_wcsrchr(const WCHAR *s, WCHAR c);
errno_t xbox_wcscpy_s(WCHAR *dst, size_t size, const WCHAR *src);
errno_t xbox_wcsncpy_s(WCHAR *dst, size_t size, const WCHAR *src, size_t count);
errno_t xbox_wcscat_s(WCHAR *dst, size_t size, const WCHAR *src);
int    xbox_swprintf_s(WCHAR *buf, size_t size, const WCHAR *fmt, ...);
int    xbox_vswprintf_s(WCHAR *buf, size_t size, const WCHAR *fmt, va_list ap);
#define wcslen     xbox_wcslen
#define wcscmp     xbox_wcscmp
#define wcsncmp    xbox_wcsncmp
#define wcscpy     xbox_wcscpy
#define wcscat     xbox_wcscat
#define wcschr     xbox_wcschr
#define wcsrchr    xbox_wcsrchr
#define _wcsicmp   xbox_wcsicmp
#define _wcsnicmp  xbox_wcsnicmp
#define wcscpy_s   xbox_wcscpy_s
#define wcsncpy_s  xbox_wcsncpy_s
#define wcscat_s   xbox_wcscat_s
#define swprintf_s xbox_swprintf_s
#define vswprintf_s xbox_vswprintf_s
#endif

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* WIN32_COMPAT_H */
