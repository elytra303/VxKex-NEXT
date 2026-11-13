///////////////////////////////////////////////////////////////////////////////
//
// Module Name:
//
//     console.c
//
// Abstract:
//
//     Console functions.
//
//     A bit of background information: Console "handles" are not real kernel
//     handles on Windows 7 (they are on Windows 10 though). They are a
//     separate type of handle which only works with console functions.
//
//     Console handles always have the least significant bit set, while of course,
//     kernel handles are always divisible by 4. Therefore it is possible to
//     quickly determine what is a console handle and what is not.
//
// Author:
//
//     vxiiduu (29-Apr-2026)
//
// Environment:
//
//     Win32
//
// Revision History:
//
//     vxiiduu              29-Apr-2026  Initial creation.
//                                       Add Ext_GetConsoleTitleA/W and
//                                       Ext_GetConsoleOriginalTitleA/W
//     vxiiduu              04-May-2026  Add initial ANSI escape sequence support.
//
///////////////////////////////////////////////////////////////////////////////

#include "buildcfg.h"
#include "kxbasep.h"

#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x4
#define ENABLE_VIRTUAL_TERMINAL_INPUT		0x200

STATIC BOOLEAN BufferContainsEscape(
	IN	PCVOID	Buffer,
	IN	ULONG	Cch,
	IN	BOOLEAN	Unicode)
{
	ULONG Index;

	if (!Buffer || !Cch) {
		return FALSE;
	}

	if (Unicode) {
		for (Index = 0; Index < Cch; ++Index) {
			if (((PCWCHAR) Buffer)[Index] == L'\x1B') {
				return TRUE;
			}
		}
	} else {
		for (Index = 0; Index < Cch; ++Index) {
			if (((PCSTR) Buffer)[Index] == 0x1B) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

KXBASEAPI BOOL WINAPI Ext_WriteConsoleA(
	IN	HANDLE	ConsoleHandle,
	IN	PCVOID	Buffer,
	IN	ULONG	CchToWrite,
	OUT	PULONG	CchWritten OPTIONAL,
	IN	PVOID	Reserved OPTIONAL)
{
	if (!BaseIsConsoleAnsiSupportEnabled(ConsoleHandle) &&
		IsConsoleOutputHandle(ConsoleHandle) &&
		BufferContainsEscape(Buffer, CchToWrite, FALSE)) {

		BaseEnableConsoleAnsiSupport(ConsoleHandle);
	}

	if (BaseIsConsoleAnsiSupportEnabled(ConsoleHandle)) {
		return WriteConsoleWithEscapeSequencesAorW(
			ConsoleHandle,
			Buffer,
			CchToWrite,
			CchWritten,
			Reserved,
			FALSE);
	}

	return WriteConsoleA(
		ConsoleHandle,
		Buffer,
		CchToWrite,
		CchWritten,
		Reserved);
}

KXBASEAPI BOOL WINAPI Ext_WriteConsoleW(
	IN	HANDLE	ConsoleHandle,
	IN	PCVOID	Buffer,
	IN	ULONG	CchToWrite,
	OUT	PULONG	CchWritten OPTIONAL,
	IN	PVOID	Reserved OPTIONAL)
{
	if (!BaseIsConsoleAnsiSupportEnabled(ConsoleHandle) &&
		IsConsoleOutputHandle(ConsoleHandle) &&
		BufferContainsEscape(Buffer, CchToWrite, TRUE)) {

		BaseEnableConsoleAnsiSupport(ConsoleHandle);
	}

	if (BaseIsConsoleAnsiSupportEnabled(ConsoleHandle)) {
		return WriteConsoleWithEscapeSequencesAorW(
			ConsoleHandle,
			Buffer,
			CchToWrite,
			CchWritten,
			Reserved,
			TRUE);
	}

	return WriteConsoleW(
		ConsoleHandle,
		Buffer,
		CchToWrite,
		CchWritten,
		Reserved);
}

KXBASEAPI BOOL WINAPI Ext_GetConsoleMode(
	IN	HANDLE	ConsoleHandle,
	OUT	PULONG	Mode)
{
	BOOL Success;

	Success = GetConsoleMode(ConsoleHandle, Mode);

	if (Success) {
		if (IsConsoleOutputHandle(ConsoleHandle)) {
			*Mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		} else if (ConsoleHandle == GetStdHandle(STD_INPUT_HANDLE)) {
			//
			// Hide the flags which we set on the application's behalf while
			// mouse reporting is active.
			//

			*Mode &= ~ENABLE_MOUSE_INPUT;
		}
	}

	return Success;
}

KXBASEAPI BOOL WINAPI Ext_SetConsoleMode(
	IN	HANDLE	ConsoleHandle,
	IN	ULONG	Mode)
{
	BOOL Success;
	BOOLEAN EnableAnsiOutput;
	BOOLEAN DisableAnsiOutput;

	EnableAnsiOutput = FALSE;
	DisableAnsiOutput = FALSE;

	if (IsConsoleOutputHandle(ConsoleHandle)) {
		if (Mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) {
			ASSERT (Mode & ENABLE_PROCESSED_OUTPUT);
			Mode &= ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
			EnableAnsiOutput = TRUE;
		} else {
			DisableAnsiOutput = TRUE;
		}
	} else {
		//
		// Windows 7 has no virtual terminal input mode. Drop the flag instead of
		// failing the whole call, so that applications which request raw VT
		// input at least keep their console modes. Mouse reporting is
		// synthesized by conansi.c and does not require this flag.
		//

		Mode &= ~ENABLE_VIRTUAL_TERMINAL_INPUT;

		if (BaseIsMouseTrackingEnabled()) {
			Mode |= ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
			Mode &= ~ENABLE_QUICK_EDIT_MODE;
		}
	}

	Success = SetConsoleMode(ConsoleHandle, Mode);

	if (Success) {
		if (EnableAnsiOutput) {
			BaseEnableConsoleAnsiSupport(ConsoleHandle);
		} else if (DisableAnsiOutput) {
			BaseDisableConsoleAnsiSupport(ConsoleHandle);
		}
	}

	return Success;
}

KXBASEAPI BOOL WINAPI Ext_FreeConsole(
	VOID)
{
	BaseDisableConsoleMouseInput();
	BaseDisableConsoleAnsiSupport(GetStdHandle(STD_OUTPUT_HANDLE));
	return FreeConsole();
}

STATIC BOOL IsConsoleAttached(
	VOID)
{
	return (GetConsoleWindow() != NULL);
}

//
// The GetConsoleTitle family of functions returns 0 on error, and according
// to MS documentation, it is supposed to also set the last-error code to
// indicate the nature of the error. However, this is not always true.
//
// When GetConsoleTitle is called in the situation where a program is not
// attached to a console, it returns 0 and leaves the buffer unmodified.
// The last-error code in this case varies significantly across different
// versions of Windows:
//
//   NT4, NT5.2 and Vista: ERROR_INVALID_ACCESS
//   Windows 7: <Not modified>
//   Windows 8.1: ERROR_NOT_SUPPORTED
//   Windows 10: ERROR_SEM_NOT_FOUND
//
// It is clear that Microsoft didn't bother to definitively set an error
// code for this particular edge case. In the case of Windows 7, we are
// particularly unlucky because GetConsoleTitle does not change the previous
// value of the last-error code, which means that the value applications see
// is effectively undefined, and is often ERROR_SUCCESS.
//
// This is an ambiguous result, because when GetConsoleTitle is called when
// the application is attached to a console with a blank title, it also returns
// 0 and sets ERROR_SUCCESS. The difference is that it also places a null
// terminator in the caller-supplied buffer.
//
// - Electron uses the libuv library (statically linked).
// - Libuv contains a function which is called uv_get_process_title.
// - Before 05/02/2026, uv_get_process_title called GetConsoleTitleW and
//   simply checked for a zero return value to indicate an error.
// - Between 15/06/2025 and 05/02/2026, uv_get_process_title would not only
//   check for a zero return value, but it would also check that GetLastError()
//   does *not* return zero, as follows:
//
//   static int uv__get_process_title(void) {
//     WCHAR title_w[MAX_TITLE_LENGTH];
//     DWORD wlen;
//     DWORD err;
//
//     SetLastError(ERROR_SUCCESS);
//     wlen = GetConsoleTitleW(title_w, sizeof(title_w) / sizeof(WCHAR));
//     if (wlen == 0) {
//       err = GetLastError();
//       if (err != 0)
//         return uv_translate_sys_error(err);
//     }
//
//     return uv__convert_utf16_to_utf8(title_w, wlen, &process_title);
//   }
//
// - For these specific builds of libuv, calling uv_get_process_title when
//   the process is not attached to a console will result in libuv thinking
//   that GetConsoleTitle has succeeded, when in fact, it has not.
// - libuv will then proceed to treat the undefined contents of the title_w
//   stack buffer as a string, and potentially crash.
// - On 05/02/2026, a commit was made to libuv which made uv_get_process_title
//   stop using GetConsoleTitleW altogether. Instead, it uses GetModuleFileName
//   to figure out the name of the process EXE. Therefore the bug no longer
//   occurs.
//
// An application which is known to exhibit this issue is Blockbench 5.1.4.
// The symptom is that the app crashes shortly after launching, but when you
// start the app from a command prompt (the command prompt must be run with
// VxKex to bypass CPIW subsystem check), the app runs fine.
//
// In order to fix the issue for this application, we will simply ensure that
// when GetConsoleTitle returns 0 and has an error code of ERROR_SUCCESS, then
// we will always null-terminate the caller-supplied buffer if there is space
// to do so.
//

STATIC VOID FixGetConsoleTitleAorW(
	OUT	PVOID	ConsoleTitle,
	IN	ULONG	BufferCch,
	IN	ULONG	CharacterCb,
	IN	ULONG	ReturnLength)
{
	if (ReturnLength == 0 && RtlGetLastWin32Error() == ERROR_SUCCESS && BufferCch >= 1) {
		if (!IsConsoleAttached()) {
			// Console is not attached, so this is an error.
			// We'll go with the Win10 error value, but as I said it changes
			// wildly across different Windows versions, so the exact value doesn't
			// really matter.
			RtlSetLastWin32Error(ERROR_SEM_NOT_FOUND);
		} else {
			// Console is attached but the function failed for some reason.
			// Write one character's worth of zeros into the console title buffer.
			RtlZeroMemory(ConsoleTitle, CharacterCb);
		}
	}
}

KXBASEAPI ULONG WINAPI Ext_GetConsoleTitleA(
	OUT	PSTR	ConsoleTitle,
	IN	ULONG	BufferCch)
{
	ULONG ReturnLength;

	ReturnLength = GetConsoleTitleA(ConsoleTitle, BufferCch);
	FixGetConsoleTitleAorW(ConsoleTitle, BufferCch, sizeof(*ConsoleTitle), ReturnLength);

	return ReturnLength;
}

KXBASEAPI ULONG WINAPI Ext_GetConsoleTitleW(
	OUT	PWSTR	ConsoleTitle,
	IN	ULONG	BufferCch)
{
	ULONG ReturnLength;

	ReturnLength = GetConsoleTitleW(ConsoleTitle, BufferCch);
	FixGetConsoleTitleAorW(ConsoleTitle, BufferCch, sizeof(*ConsoleTitle), ReturnLength);

	return ReturnLength;
}

KXBASEAPI ULONG WINAPI Ext_GetConsoleOriginalTitleA(
	OUT	PSTR	ConsoleTitle,
	IN	ULONG	BufferCch)
{
	ULONG ReturnLength;

	ReturnLength = GetConsoleOriginalTitleA(ConsoleTitle, BufferCch);
	FixGetConsoleTitleAorW(ConsoleTitle, BufferCch, sizeof(*ConsoleTitle), ReturnLength);

	return ReturnLength;
}

KXBASEAPI ULONG WINAPI Ext_GetConsoleOriginalTitleW(
	OUT	PWSTR	ConsoleTitle,
	IN	ULONG	BufferCch)
{
	ULONG ReturnLength;

	ReturnLength = GetConsoleOriginalTitleW(ConsoleTitle, BufferCch);
	FixGetConsoleTitleAorW(ConsoleTitle, BufferCch, sizeof(*ConsoleTitle), ReturnLength);

	return ReturnLength;
}

KXBASEAPI HRESULT WINAPI Ext_CreatePseudoConsole(
	IN	COORD	Size,
	IN	HANDLE	InputHandle,
	IN	HANDLE	OutputHandle,
	IN	ULONG	Flags,
	OUT	HPCON	*PseudoConsole)
{
	UNREFERENCED_PARAMETER(Size);
	UNREFERENCED_PARAMETER(InputHandle);
	UNREFERENCED_PARAMETER(OutputHandle);
	UNREFERENCED_PARAMETER(Flags);
	UNREFERENCED_PARAMETER(PseudoConsole);

	return E_NOTIMPL;
}

KXBASEAPI HRESULT WINAPI Ext_ResizePseudoConsole(
	IN	HPCON	PseudoConsole,
	IN	COORD	Size)
{
	UNREFERENCED_PARAMETER(PseudoConsole);
	UNREFERENCED_PARAMETER(Size);

	return E_NOTIMPL;
}

KXBASEAPI VOID WINAPI Ext_ClosePseudoConsole(
	IN	HPCON	PseudoConsole)
{
	UNREFERENCED_PARAMETER(PseudoConsole);
}