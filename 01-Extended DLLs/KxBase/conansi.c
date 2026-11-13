///////////////////////////////////////////////////////////////////////////////
//
// Module Name:
//
//     conansi.c
//
// Abstract:
//
//     Contains support code for processing ANSI escape sequences on a
//     Windows console.
//
// Author:
//
//     vxiiduu (05-May-2026)
//
// Environment:
//
//     Win32
//
// Revision History:
//
//     vxiiduu              05-May-2026  Initial creation.
//
///////////////////////////////////////////////////////////////////////////////

#include "buildcfg.h"
#include "kxbasep.h"

#define ESC (0x1B)
#define BEL (0x07)
#define VXKEX_CONSOLE_ANSI_OUTPUT	1

#define MAX_CSI_PARAMETER_COUNT		16

//
// Mouse support.
//
// Windows console applications receive MOUSE_EVENT input records, while
// virtual terminal applications expect xterm mouse reports to arrive as
// characters on the input stream. Windows 7 supports neither virtual terminal
// input nor virtual terminal mouse reporting, so when an application enables
// a mouse tracking mode we turn on ENABLE_MOUSE_INPUT behind its back and
// translate MOUSE_EVENT records into escape sequences when input is read.
//

#define MOUSE_TRACKING_OFF			0
#define MOUSE_TRACKING_X10			9
#define MOUSE_TRACKING_NORMAL		1000
#define MOUSE_TRACKING_BUTTON		1002
#define MOUSE_TRACKING_ANY			1003

#define MOUSE_ENCODING_X10			9
#define MOUSE_ENCODING_UTF8			1005
#define MOUSE_ENCODING_SGR			1006
#define MOUSE_ENCODING_URXVT		1015

//
// Global variables for ANSI escape sequence support.
//

STATIC ULONG g_ConsoleANSISupport = 0; // VXKEX_CONSOLE_*
STATIC COORD g_SavedCursorPosition; // 1-based, viewport-relative.
STATIC WORD g_DefaultTextAttributes;
STATIC BOOLEAN g_CsiPrivateMarker; // set when the CSI sequence starts with '?'

//
// Global variables for mouse support.
//

STATIC ULONG g_MouseTrackingMode; // MOUSE_TRACKING_*; 0 means disabled.
STATIC ULONG g_MouseEncoding = MOUSE_ENCODING_X10;
STATIC DWORD g_MouseButtonState; // button state at the previous mouse event.
STATIC COORD g_MousePosition = { -1, -1 };
STATIC BOOLEAN g_MouseInputEnabled; // did we turn on ENABLE_MOUSE_INPUT?
STATIC BOOLEAN g_MouseQuickEditWasEnabled;
STATIC WCHAR g_InputPending[64]; // translated mouse report not fully consumed.
STATIC ULONG g_InputPendingCch;

STATIC VOID RefreshConsolePalette(
	IN	HANDLE	ConsoleHandle);

BOOL IsConsoleHandle(
	IN	HANDLE	Handle)
{
	DWORD Mode;
	if ((GetFileType(Handle) & FILE_TYPE_CHAR) == 0) return FALSE;
	return GetConsoleMode(Handle, &Mode);
}

BOOLEAN BaseIsConsoleAnsiSupportEnabled(
	IN	HANDLE	ConsoleHandle)
{
	return !!g_ConsoleANSISupport;
}

BOOLEAN BaseEnableConsoleAnsiSupport(
	IN	HANDLE	ConsoleHandle)
{
	BOOLEAN Success;

	ASSERT (IsConsoleHandle(ConsoleHandle));

	Success = VTGetCursorPosition(ConsoleHandle, &g_SavedCursorPosition);
	ASSERT (Success);

	if (!Success) {
		// Don't give up on ANSI support just because the cursor position
		// query failed. Some console handles are unusual, and a usable
		// default is better than passing escape sequences through raw.
		g_SavedCursorPosition.X = 1;
		g_SavedCursorPosition.Y = 1;
	}

	//
	// We need to save the current console attributes instead of using a
	// hardcoded value such as white on black, because apps started from
	// e.g. Powershell will need to take into account the pre-existing white
	// on blue text attributes. Otherwise any attempt to reset text attributes
	// will cause ugliness.
	//
	// Also, the user can change the default console colors, and we want to
	// behave correctly in that situation.
	//

	Success = GetConsoleTextAttribute(ConsoleHandle, &g_DefaultTextAttributes);
	ASSERT (Success);

	if (!Success) {
		g_DefaultTextAttributes =
			FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
	}

	// Remove reverse video attribute if it happens to be set.
	// It's very unlikely, but if it were, then SGR 0 would end up enabling
	// reverse video, which is unwanted.
	g_DefaultTextAttributes &= ~COMMON_LVB_REVERSE_VIDEO;

	// Use the palette the console actually renders with, so that 256-colour
	// and 24-bit colour sequences are approximated against real colours.
	RefreshConsolePalette(ConsoleHandle);

	g_ConsoleANSISupport |= VXKEX_CONSOLE_ANSI_OUTPUT;
	KexLogDebugEvent(L"Enabled escape sequence processing");
	return TRUE;
}

BOOLEAN BaseDisableConsoleAnsiSupport(
	IN	HANDLE	ConsoleHandle)
{
	g_ConsoleANSISupport &= ~VXKEX_CONSOLE_ANSI_OUTPUT;
	KexLogDebugEvent(L"Disabled escape sequence processing");
	return TRUE;
}

BOOLEAN BaseIsMouseTrackingEnabled(
	VOID)
{
	return (g_MouseTrackingMode != 0);
}

BOOLEAN BaseIsConsoleInputTranslationNeeded(
	VOID)
{
	return (g_MouseTrackingMode != 0) || (g_InputPendingCch != 0);
}

STATIC VOID UpdateConsoleMouseInputMode(
	VOID)
{
	HANDLE InputHandle;
	DWORD Mode;

	InputHandle = GetStdHandle(STD_INPUT_HANDLE);

	if (!IsConsoleInputHandle(InputHandle)) {
		return;
	}

	if (!GetConsoleMode(InputHandle, &Mode)) {
		return;
	}

	if (g_MouseTrackingMode != 0) {
		if (Mode & ENABLE_MOUSE_INPUT) {
			// The application enabled mouse input by itself.
			return;
		}

		Mode |= ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;

		//
		// QuickEdit would turn mouse clicks into text selection instead of
		// reporting them to the application, so it has to be turned off while
		// mouse reporting is active.
		//

		if (Mode & ENABLE_QUICK_EDIT_MODE) {
			Mode &= ~ENABLE_QUICK_EDIT_MODE;
			g_MouseQuickEditWasEnabled = TRUE;
		}

		if (SetConsoleMode(InputHandle, Mode)) {
			g_MouseInputEnabled = TRUE;
		}
	} else if (g_MouseInputEnabled) {
		Mode &= ~ENABLE_MOUSE_INPUT;

		if (g_MouseQuickEditWasEnabled) {
			Mode |= ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS;
			g_MouseQuickEditWasEnabled = FALSE;
		}

		SetConsoleMode(InputHandle, Mode);
		g_MouseInputEnabled = FALSE;
	}
}

VOID BaseDisableConsoleMouseInput(
	VOID)
{
	g_MouseTrackingMode = MOUSE_TRACKING_OFF;
	g_MouseButtonState = 0;
	g_InputPendingCch = 0;

	UpdateConsoleMouseInputMode();
}

STATIC ULONG AppendDecimalNumber(
	OUT	PWCHAR	Buffer,
	IN	ULONG	BufferCch,
	IN	ULONG	Number)
{
	WCHAR Digits[10];
	ULONG DigitCount;
	ULONG TotalCch;

	DigitCount = 0;

	do {
		Digits[DigitCount++] = (WCHAR) (L'0' + (Number % 10));
		Number /= 10;
	} until (Number == 0);

	TotalCch = DigitCount;

	while (DigitCount != 0 && BufferCch != 0) {
		*Buffer++ = Digits[--DigitCount];
		--BufferCch;
	}

	return TotalCch;
}

STATIC ULONG MouseButtonFromState(
	IN	DWORD	ButtonState)
{
	if (ButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
		return 0;
	} else if (ButtonState & FROM_LEFT_2ND_BUTTON_PRESSED) {
		return 1;
	} else if (ButtonState & RIGHTMOST_BUTTON_PRESSED) {
		return 2;
	} else if (ButtonState & FROM_LEFT_3RD_BUTTON_PRESSED) {
		return 6;
	} else if (ButtonState & FROM_LEFT_4TH_BUTTON_PRESSED) {
		return 7;
	}

	return 0;
}

BOOLEAN BaseTranslateMouseEvent(
	IN	PMOUSE_EVENT_RECORD	MouseEvent,
	OUT	PWCHAR				Buffer,
	IN	ULONG				BufferCch,
	OUT	PULONG				CchOut)
{
	DWORD ButtonState;
	DWORD Released;
	ULONG Button;
	ULONG LegacyButton;
	ULONG Modifier;
	ULONG X;
	ULONG Y;
	ULONG Cch;
	BOOLEAN IsRelease;
	BOOLEAN IsMotion;

	*CchOut = 0;

	if (g_MouseTrackingMode == MOUSE_TRACKING_OFF) {
		return FALSE;
	}

	// A single report is never longer than 24 characters.
	if (BufferCch < 24) {
		return FALSE;
	}

	IsRelease = FALSE;
	IsMotion = (MouseEvent->dwEventFlags == MOUSE_MOVED);
	Button = 0;

	ButtonState = MouseEvent->dwButtonState & (
		FROM_LEFT_1ST_BUTTON_PRESSED |
		FROM_LEFT_2ND_BUTTON_PRESSED |
		FROM_LEFT_3RD_BUTTON_PRESSED |
		FROM_LEFT_4TH_BUTTON_PRESSED |
		RIGHTMOST_BUTTON_PRESSED);

	if (MouseEvent->dwEventFlags & MOUSE_WHEELED) {
		SHORT Delta;

		Delta = (SHORT) HIWORD(MouseEvent->dwButtonState);
		Button = (Delta > 0) ? 64 : 65;
	} else if (MouseEvent->dwEventFlags & MOUSE_HWHEELED) {
		SHORT Delta;

		Delta = (SHORT) HIWORD(MouseEvent->dwButtonState);
		Button = (Delta > 0) ? 67 : 66;
	} else {
		Released = g_MouseButtonState & ~ButtonState;
		g_MouseButtonState = ButtonState;

		if (Released != 0) {
			Button = MouseButtonFromState(Released);
			IsRelease = TRUE;
		} else {
			Button = MouseButtonFromState(ButtonState);

			if (IsMotion) {
				if (g_MouseTrackingMode == MOUSE_TRACKING_X10 ||
					g_MouseTrackingMode == MOUSE_TRACKING_NORMAL) {

					return FALSE;
				}

				if (g_MouseTrackingMode == MOUSE_TRACKING_BUTTON &&
					ButtonState == 0) {

					return FALSE;
				}

				// Motion reports are marked by setting bit 5.
				Button |= 32;
			}
		}
	}

	if (IsMotion &&
		g_MousePosition.X == MouseEvent->dwMousePosition.X &&
		g_MousePosition.Y == MouseEvent->dwMousePosition.Y) {

		return FALSE;
	}

	g_MousePosition = MouseEvent->dwMousePosition;

	Modifier = 0;

	if (MouseEvent->dwControlKeyState & SHIFT_PRESSED) {
		Modifier |= 4;
	}

	if (MouseEvent->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) {
		Modifier |= 8;
	}

	if (MouseEvent->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
		Modifier |= 16;
	}

	Button += Modifier;

	X = (ULONG) max(MouseEvent->dwMousePosition.X + 1, 1);
	Y = (ULONG) max(MouseEvent->dwMousePosition.Y + 1, 1);

	// X10 and urxvt report releases as button 3.
	LegacyButton = IsRelease ? 3 : Button;

	Cch = 0;
	Buffer[Cch++] = ESC;

	switch (g_MouseEncoding) {
	case MOUSE_ENCODING_SGR:
		Buffer[Cch++] = '[';
		Buffer[Cch++] = '<';
		Cch += AppendDecimalNumber(&Buffer[Cch], BufferCch - Cch, Button);
		Buffer[Cch++] = ';';
		Cch += AppendDecimalNumber(&Buffer[Cch], BufferCch - Cch, X);
		Buffer[Cch++] = ';';
		Cch += AppendDecimalNumber(&Buffer[Cch], BufferCch - Cch, Y);
		Buffer[Cch++] = IsRelease ? 'm' : 'M';
		break;
	case MOUSE_ENCODING_URXVT:
		Buffer[Cch++] = '[';
		Cch += AppendDecimalNumber(&Buffer[Cch], BufferCch - Cch, LegacyButton);
		Buffer[Cch++] = ';';
		Cch += AppendDecimalNumber(&Buffer[Cch], BufferCch - Cch, X);
		Buffer[Cch++] = ';';
		Cch += AppendDecimalNumber(&Buffer[Cch], BufferCch - Cch, Y);
		Buffer[Cch++] = 'M';
		break;
	default:
		//
		// X10 encoding. Note that MOUSE_ENCODING_UTF8 is handled the same
		// way: console input is converted through the console code page, so
		// only 7-bit ASCII can be delivered reliably.
		//

		if (X > 223) {
			X = 223;
		}

		if (Y > 223) {
			Y = 223;
		}

		Buffer[Cch++] = '[';
		Buffer[Cch++] = 'M';
		Buffer[Cch++] = (WCHAR) (32 + LegacyButton);
		Buffer[Cch++] = (WCHAR) (32 + X);
		Buffer[Cch++] = (WCHAR) (32 + Y);
		break;
	}

	*CchOut = Cch;
	return TRUE;
}

STATIC VOID BuildSyntheticKeyEvent(
	OUT	PINPUT_RECORD	Record,
	IN	WCHAR			Character,
	IN	BOOLEAN			Unicode)
{
	RtlZeroMemory(Record, sizeof(*Record));

	Record->EventType = KEY_EVENT;
	Record->Event.KeyEvent.bKeyDown = TRUE;
	Record->Event.KeyEvent.wRepeatCount = 1;

	if (Unicode) {
		Record->Event.KeyEvent.uChar.UnicodeChar = Character;
	} else {
		Record->Event.KeyEvent.uChar.AsciiChar = (CHAR) Character;
	}
}

STATIC BOOL ReadConsoleInputWithMouseAorW(
	IN	HANDLE			ConsoleHandle,
	OUT	PINPUT_RECORD	Buffer,
	IN	ULONG			Length,
	OUT	PULONG			NumberEventsRead,
	IN	BOOLEAN			Unicode)
{
	INPUT_RECORD Record;
	ULONG LocalNumberEventsRead;
	ULONG RecordsRead;
	ULONG Produced;
	ULONG CchTranslated;
	ULONG Index;
	DWORD Available;
	BOOL Success;

	if (!NumberEventsRead) {
		NumberEventsRead = &LocalNumberEventsRead;
	}

	*NumberEventsRead = 0;

	if (!BaseIsConsoleInputTranslationNeeded() ||
		!IsConsoleInputHandle(ConsoleHandle)) {

		if (Unicode) {
			return ReadConsoleInputW(
				ConsoleHandle,
				Buffer,
				Length,
				NumberEventsRead);
		} else {
			return ReadConsoleInputA(
				ConsoleHandle,
				Buffer,
				Length,
				NumberEventsRead);
		}
	}

	Produced = 0;

	while (Produced < Length) {
		//
		// Hand out characters left over from a previously translated mouse
		// event before reading anything else.
		//

		while (g_InputPendingCch != 0 && Produced < Length) {
			BuildSyntheticKeyEvent(
				&Buffer[Produced],
				g_InputPending[0],
				Unicode);

			++Produced;

			for (Index = 1; Index < g_InputPendingCch; ++Index) {
				g_InputPending[Index - 1] = g_InputPending[Index];
			}

			--g_InputPendingCch;
		}

		if (Produced == Length) {
			break;
		}

		//
		// Do not block for more input if we already have something to return,
		// but do consume records which are already available.
		//

		if (Produced != 0) {
			if (!GetNumberOfConsoleInputEvents(ConsoleHandle, &Available) ||
				Available == 0) {

				break;
			}
		}

		if (Unicode) {
			Success = ReadConsoleInputW(
				ConsoleHandle,
				&Record,
				1,
				&RecordsRead);
		} else {
			Success = ReadConsoleInputA(
				ConsoleHandle,
				&Record,
				1,
				&RecordsRead);
		}

		if (!Success) {
			if (Produced != 0) {
				break;
			}

			return FALSE;
		}

		if (RecordsRead == 0) {
			break;
		}

		if (Record.EventType == MOUSE_EVENT && BaseIsMouseTrackingEnabled()) {
			BaseTranslateMouseEvent(
				&Record.Event.MouseEvent,
				g_InputPending,
				ARRAYSIZE(g_InputPending),
				&CchTranslated);

			continue;
		}

		Buffer[Produced++] = Record;
	}

	*NumberEventsRead = Produced;
	return TRUE;
}

KXBASEAPI BOOL WINAPI Ext_ReadConsoleInputA(
	IN	HANDLE			ConsoleHandle,
	OUT	PINPUT_RECORD	Buffer,
	IN	ULONG			Length,
	OUT	PULONG			NumberEventsRead)
{
	return ReadConsoleInputWithMouseAorW(
		ConsoleHandle,
		Buffer,
		Length,
		NumberEventsRead,
		FALSE);
}

KXBASEAPI BOOL WINAPI Ext_ReadConsoleInputW(
	IN	HANDLE			ConsoleHandle,
	OUT	PINPUT_RECORD	Buffer,
	IN	ULONG			Length,
	OUT	PULONG			NumberEventsRead)
{
	return ReadConsoleInputWithMouseAorW(
		ConsoleHandle,
		Buffer,
		Length,
		NumberEventsRead,
		TRUE);
}

STATIC ULONG ParseNumber(
	IN	PCWCHAR	Characters,
	IN	ULONG	MaxCch,
	OUT	PULONG	NumberCch OPTIONAL)
{
	ULONG Accumulator;
	ULONG Index;

	Accumulator = 0;
	Index = 0;

	until (!iswdigit(Characters[Index]) || Index >= MaxCch) {
		Accumulator *= 10;
		Accumulator += Characters[Index] - '0';
		++Index;
	}

	if (NumberCch) {
		*NumberCch = Index;
	}

	return Accumulator;
}

STATIC ULONG GetCchToNextEscapeOrEndOfBufferAorW(
	IN	PCVOID	Buffer,
	IN	ULONG	BufferCch,
	IN	BOOLEAN	Unicode)
{
	ULONG Index;

	Index = 0;

	if (Unicode) {
		PCWCHAR BufferW;

		BufferW = (PCWCHAR) Buffer;

		until (BufferCch == 0) {
			if (BufferW[Index] == ESC) {
				return Index;
			}

			--BufferCch;
			++Index;
		}
	} else {
		PCSTR BufferA;

		BufferA = (PCSTR) Buffer;

		until (BufferCch == 0) {
			if (BufferA[Index] == ESC) {
				return Index;
			}

			--BufferCch;
			++Index;
		}
	}

	// If we get here it means that there's no escape sequence
	return Index;
}

STATIC BYTE g_ConsolePalette[16][3] = {
	{   0,   0,   0 }, // Black
	{   0,   0, 128 }, // Dark blue
	{   0, 128,   0 }, // Dark green
	{   0, 128, 128 }, // Dark cyan
	{ 128,   0,   0 }, // Dark red
	{ 128,   0, 128 }, // Dark magenta
	{ 128, 128,   0 }, // Dark yellow
	{ 192, 192, 192 }, // Gray
	{ 128, 128, 128 }, // Dark gray
	{   0,   0, 255 }, // Blue
	{   0, 255,   0 }, // Green
	{   0, 255, 255 }, // Cyan
	{ 255,   0,   0 }, // Red
	{ 255,   0, 255 }, // Magenta
	{ 255, 255,   0 }, // Yellow
	{ 255, 255, 255 }  // White
};

//
// Fetch the colours which the console actually uses. This matters because the
// console palette can be customised (e.g. by the user or by a colour scheme),
// and approximating a 256-colour or 24-bit colour against the built-in palette
// would then produce visibly wrong results.
//
STATIC VOID RefreshConsolePalette(
	IN	HANDLE	ConsoleHandle)
{
	CONSOLE_SCREEN_BUFFER_INFOEX Sbi;
	ULONG Index;
	COLORREF Color;

	RtlZeroMemory(&Sbi, sizeof(Sbi));
	Sbi.cbSize = sizeof(Sbi);

	if (!GetConsoleScreenBufferInfoEx(ConsoleHandle, &Sbi)) {
		return;
	}

	for (Index = 0; Index < ARRAYSIZE(Sbi.ColorTable); ++Index) {
		Color = Sbi.ColorTable[Index];

		g_ConsolePalette[Index][0] = (BYTE) (Color & 0xFF);
		g_ConsolePalette[Index][1] = (BYTE) ((Color >> 8) & 0xFF);
		g_ConsolePalette[Index][2] = (BYTE) ((Color >> 16) & 0xFF);
	}
}

STATIC WORD ConsoleColorFromRGB(
	IN	BYTE	Red,
	IN	BYTE	Green,
	IN	BYTE	Blue,
	IN	BOOLEAN	Background)
{
	ULONG Index;
	ULONG BestIndex;
	ULONG BestDistance;
	WORD Attribute;

	BestIndex = 0;
	BestDistance = (ULONG) -1;

	for (Index = 0; Index < ARRAYSIZE(g_ConsolePalette); ++Index) {
		LONG RedDelta;
		LONG GreenDelta;
		LONG BlueDelta;
		ULONG Distance;

		RedDelta = (LONG) Red - g_ConsolePalette[Index][0];
		GreenDelta = (LONG) Green - g_ConsolePalette[Index][1];
		BlueDelta = (LONG) Blue - g_ConsolePalette[Index][2];

		Distance = (ULONG) (
			RedDelta * RedDelta +
			GreenDelta * GreenDelta +
			BlueDelta * BlueDelta);

		if (Distance < BestDistance) {
			BestDistance = Distance;
			BestIndex = Index;
		}
	}

	if (Background) {
		Attribute = (WORD) ((BestIndex & 0x7) << 4);

		if (BestIndex & 0x8) {
			Attribute |= BACKGROUND_INTENSITY;
		}
	} else {
		Attribute = (WORD) (BestIndex & 0x7);

		if (BestIndex & 0x8) {
			Attribute |= FOREGROUND_INTENSITY;
		}
	}

	return Attribute;
}

STATIC VOID XtermColorToRGB(
	IN	ULONG	Color,
	OUT	PBYTE	Red,
	OUT	PBYTE	Green,
	OUT	PBYTE	Blue)
{
	STATIC CONST BYTE Levels[6] = { 0, 95, 135, 175, 215, 255 };

	if (Color < 16) {
		*Red = g_ConsolePalette[Color][0];
		*Green = g_ConsolePalette[Color][1];
		*Blue = g_ConsolePalette[Color][2];
	} else if (Color < 232) {
		ULONG Index;

		Index = Color - 16;

		*Red = Levels[(Index / 36) % 6];
		*Green = Levels[(Index / 6) % 6];
		*Blue = Levels[Index % 6];
	} else {
		BYTE Gray;

		Gray = (BYTE) (8 + (Color - 232) * 10);

		*Red = Gray;
		*Green = Gray;
		*Blue = Gray;
	}
}

STATIC WORD ApplyConsoleColor(
	IN	WORD	TextAttribute,
	IN	WORD	ColorAttribute,
	IN	BOOLEAN	Background)
{
	if (Background) {
		TextAttribute &= ~(BACKGROUND_INTENSITY |
						   BACKGROUND_RED |
						   BACKGROUND_GREEN |
						   BACKGROUND_BLUE);
	} else {
		TextAttribute &= ~(FOREGROUND_INTENSITY |
						   FOREGROUND_RED |
						   FOREGROUND_GREEN |
						   FOREGROUND_BLUE);
	}

	return TextAttribute | ColorAttribute;
}

STATIC BOOLEAN ExecuteCSISetGraphicsRendition(
	IN	HANDLE	ConsoleHandle,
	IN	SHORT	CsiParameters[MAX_CSI_PARAMETER_COUNT],
	IN	ULONG	ParameterCount)
{
	WORD TextAttribute;
	ULONG ParameterIndex;

	GetConsoleTextAttribute(ConsoleHandle, &TextAttribute);

	// set parameter count to 1 if it is 0.
	// if there are no parameters we treat that the same as one parameter with
	// the value 0. Note that the Parameters array is initialized to all zeros
	// if there are no parameters, which is why this works.
	ParameterCount = max(ParameterCount, 1);

	for (ParameterIndex = 0; ParameterIndex < ParameterCount; ++ParameterIndex) {
		SHORT Parameter;

		Parameter = CsiParameters[ParameterIndex];

		if (TextAttribute & COMMON_LVB_REVERSE_VIDEO) {
			// Swap the handling of foreground and background colors
			if (Parameter >= 30 && Parameter <= 39 ||
				Parameter >= 90 && Parameter <= 97) {

				Parameter += 10;
			} else if (Parameter >= 40 && Parameter <= 49 ||
						Parameter >= 100 && Parameter <= 107) {

				Parameter -= 10;
			}
		}

		if (Parameter >= 30 && Parameter <= 39) {
			// setting non-bright foreground color
			TextAttribute &= ~(FOREGROUND_INTENSITY |
								FOREGROUND_RED |
								FOREGROUND_GREEN |
								FOREGROUND_BLUE);
		} else if (Parameter >= 40 && Parameter <= 49) {
			// setting non-bright background color
			TextAttribute &= ~(BACKGROUND_INTENSITY |
								BACKGROUND_RED |
								BACKGROUND_GREEN |
								BACKGROUND_BLUE);
		} else if (Parameter >= 90 && Parameter <= 97) {
			// setting bright foreground color
			TextAttribute |= FOREGROUND_INTENSITY;
			TextAttribute &= ~(FOREGROUND_RED |
								FOREGROUND_GREEN |
								FOREGROUND_BLUE);
			Parameter -= 60;
		} else if (Parameter >= 100 && Parameter <= 107) {
			// setting bright background color
			TextAttribute |= BACKGROUND_INTENSITY;
			TextAttribute &= ~(BACKGROUND_RED |
								BACKGROUND_GREEN |
								BACKGROUND_BLUE);
			Parameter -= 60;
		}

		switch (Parameter) {
		case 0:		// Reset
			TextAttribute = g_DefaultTextAttributes;
			break;
		case 1:		// Bright
			TextAttribute |= FOREGROUND_INTENSITY;
			break;
		case 2:		// Faint
			// The console has no dim attribute. Dropping the intensity bit is
			// closer to what the application asked for than ignoring the
			// sequence altogether.
			TextAttribute &= ~FOREGROUND_INTENSITY;
			break;
		case 4:		// Underline
			TextAttribute |= COMMON_LVB_UNDERSCORE;
			break;
		case 7:		// Reverse video (negative)
			// This flag has no effect on the console but we just use it to remember
			// the fact that reverse video is enabled
			TextAttribute |= COMMON_LVB_REVERSE_VIDEO;
			// Swap foreground and background colors
			*(PBYTE) &TextAttribute = _rotl8((BYTE) TextAttribute, 4);
			break;
		case 27:	// Turn off reverse video (positive)
			TextAttribute &= ~COMMON_LVB_REVERSE_VIDEO;
			*(PBYTE) &TextAttribute = _rotl8((BYTE) TextAttribute, 4);
			break;
		case 21:	// Double underline
			TextAttribute &= ~FOREGROUND_INTENSITY;
			TextAttribute |= COMMON_LVB_UNDERSCORE;
			break;
		case 22:	// No bright
			TextAttribute &= ~FOREGROUND_INTENSITY;
			break;
		case 24:	// Underline off
			TextAttribute &= ~COMMON_LVB_UNDERSCORE;
			break;
		case 31:	// Red
			TextAttribute |= FOREGROUND_RED;
			break;
		case 32:	// Green
			TextAttribute |= FOREGROUND_GREEN;
			break;
		case 33:	// Yellow
			TextAttribute |= FOREGROUND_RED | FOREGROUND_GREEN;
			break;
		case 34:	// Blue
			TextAttribute |= FOREGROUND_BLUE;
			break;
		case 35:	// Magenta
			TextAttribute |= FOREGROUND_RED | FOREGROUND_BLUE;
			break;
		case 36:	// Cyan
			TextAttribute |= FOREGROUND_GREEN | FOREGROUND_BLUE;
			break;
		case 37:	// White
			TextAttribute |= FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
			break;
		case 39:	// Foreground reset
			TextAttribute |= g_DefaultTextAttributes & (FOREGROUND_INTENSITY |
														FOREGROUND_RED |
														FOREGROUND_GREEN |
														FOREGROUND_BLUE);
			break;
		case 41:	// Background red
			TextAttribute |= BACKGROUND_RED;
			break;
		case 42:	// Background green
			TextAttribute |= BACKGROUND_GREEN;
			break;
		case 43:	// Background yellow
			TextAttribute |= BACKGROUND_RED | BACKGROUND_GREEN;
			break;
		case 44:	// Background blue
			TextAttribute |= BACKGROUND_BLUE;
			break;
		case 45:	// Background magenta
			TextAttribute |= BACKGROUND_RED | BACKGROUND_BLUE;
			break;
		case 46:	// Background cyan
			TextAttribute |= BACKGROUND_GREEN | BACKGROUND_BLUE;
			break;
		case 47:	// Background white
			TextAttribute |= BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
			break;
		case 49:	// Background reset
			TextAttribute |= g_DefaultTextAttributes & (BACKGROUND_INTENSITY |
														BACKGROUND_RED |
														BACKGROUND_GREEN |
														BACKGROUND_BLUE);
			break;
		case 38:	// Extended foreground color setting
		case 48:	// Extended background color setting
		{
			BOOLEAN Background;
			ULONG ColorModeIndex;
			SHORT ColorMode;

			Background = (Parameter == 48);
			ColorModeIndex = ParameterIndex + 1;

			if (ColorModeIndex < ParameterCount) {
				ColorMode = CsiParameters[ColorModeIndex];
			} else {
				ColorMode = -1;
			}

			if (ColorMode == 5 && ColorModeIndex + 1 < ParameterCount) {
				BYTE Red;
				BYTE Green;
				BYTE Blue;
				WORD ColorAttribute;

				XtermColorToRGB(
					(ULONG) (USHORT) CsiParameters[ColorModeIndex + 1],
					&Red,
					&Green,
					&Blue);

				ColorAttribute = ConsoleColorFromRGB(
					Red,
					Green,
					Blue,
					Background);

				TextAttribute = ApplyConsoleColor(
					TextAttribute,
					ColorAttribute,
					Background);

				ParameterIndex = ColorModeIndex + 1;
				break;
			}

			if (ColorMode == 2 && ColorModeIndex + 3 < ParameterCount) {
				BYTE Red;
				BYTE Green;
				BYTE Blue;
				WORD ColorAttribute;

				Red = (BYTE) (USHORT) CsiParameters[ColorModeIndex + 1];
				Green = (BYTE) (USHORT) CsiParameters[ColorModeIndex + 2];
				Blue = (BYTE) (USHORT) CsiParameters[ColorModeIndex + 3];

				ColorAttribute = ConsoleColorFromRGB(
					Red,
					Green,
					Blue,
					Background);

				TextAttribute = ApplyConsoleColor(
					TextAttribute,
					ColorAttribute,
					Background);

				ParameterIndex = ColorModeIndex + 3;
				break;
			}

			// Malformed or unsupported extended color setting. Consume the
			// rest of the SGR sequence instead of interpreting its arguments
			// as separate text attributes.
			KexLogDebugEvent(
				L"Extended %s color setting ignored",
				Background ? L"background" : L"foreground");
			KexDebugCheckpoint();
			return FALSE;
		}
		}
	}

	return SetConsoleTextAttribute(ConsoleHandle, TextAttribute);
}

STATIC BOOLEAN ExecuteCSIErase(
	IN	HANDLE	ConsoleHandle,
	IN	SHORT	Parameter,
	IN	BOOLEAN	LineOnly)
{
	BOOLEAN Success;
	CONSOLE_SCREEN_BUFFER_INFO Sbi;
	COORD EraseStart;
	COORD EraseEnd;
	ULONG EraseCount;

	Success = GetConsoleScreenBufferInfo(ConsoleHandle, &Sbi);
	if (!Success) {
		return FALSE;
	}

	switch (Parameter) {
	case 0:
		EraseStart = Sbi.dwCursorPosition;
		EraseEnd.X = Sbi.dwSize.X - 1;

		if (LineOnly) {
			// erase from cursor position to end of current line
			EraseEnd.Y = Sbi.dwCursorPosition.Y;
		} else {
			// erase from cursor position to end of display
			EraseEnd.Y = Sbi.srWindow.Bottom;
		}

		break;
	case 1:
		EraseEnd = Sbi.dwCursorPosition;
		EraseStart.X = 0;

		if (LineOnly) {
			// erase from beginning of line to cursor
			EraseStart.Y = Sbi.dwCursorPosition.Y;
		} else {
			// erase from beginning of display to cursor
			EraseStart.Y = Sbi.srWindow.Top;
		}

		break;
	case 2:
		EraseStart.X = 0;
		EraseEnd.X = Sbi.dwSize.X - 1;

		if (LineOnly) {
			// erase whole line
			EraseStart.Y = Sbi.dwCursorPosition.Y;
			EraseEnd.Y = Sbi.dwCursorPosition.Y;
		} else {
			// erase whole screen
			EraseStart.Y = Sbi.srWindow.Top;
			EraseEnd.Y = Sbi.srWindow.Bottom;
		}

		break;
	default:
		KexLogDebugEvent(L"Invalid parameter %hu to CSI 'J' or 'K' escape", Parameter);
		return FALSE;
	}

	EraseCount = (EraseEnd.Y - EraseStart.Y) * Sbi.dwSize.X;
	EraseCount += EraseEnd.X - EraseStart.X + 1;

	Success = FillConsoleOutputCharacter(
		ConsoleHandle,
		' ',
		EraseCount,
		EraseStart,
		&EraseCount);
	
	if (!Success) {
		return Success;
	}

	return FillConsoleOutputAttribute(
		ConsoleHandle,
		Sbi.wAttributes,
		EraseCount,
		EraseStart,
		&EraseCount);
}

STATIC BOOLEAN ExecuteCSI(
	IN	HANDLE	ConsoleHandle,
	IN	WCHAR	Command,
	IN	SHORT	CsiParameters[MAX_CSI_PARAMETER_COUNT],
	IN	ULONG	ParameterCount)
{
	COORD Coord;

	switch (Command) {
	case 'A': // Cursor Up
		Coord.X = 0;
		Coord.Y = -max(1, CsiParameters[0]);
		return VTMoveCursorRelative(ConsoleHandle, Coord);
	case 'B': // Cursor Down
		Coord.X = 0;
		Coord.Y = max(1, CsiParameters[0]);
		return VTMoveCursorRelative(ConsoleHandle, Coord);
	case 'C': // Cursor Forward (Right)
		Coord.X = max(1, CsiParameters[0]);
		Coord.Y = 0;
		return VTMoveCursorRelative(ConsoleHandle, Coord);
	case 'D': // Cursor Backward (Left)
		Coord.X = -max(1, CsiParameters[0]);
		Coord.Y = 0;
		return VTMoveCursorRelative(ConsoleHandle, Coord);
	case 'E': // Cursor Next Line
		return VTMoveCursorVerticalRelativeWithHorizontalReset(
			ConsoleHandle,
			max(1, CsiParameters[0]));
	case 'F': // Cursor Previous Line
		return VTMoveCursorVerticalRelativeWithHorizontalReset(
			ConsoleHandle,
			-max(1, CsiParameters[0]));
	case 'G': // Cursor Horizontal Absolute
		return VTSetCursorHorizontalPosition(ConsoleHandle, CsiParameters[0]);
	case 'd': // Vertical Line Position Absolute
		return VTSetCursorVerticalPosition(ConsoleHandle, CsiParameters[0]);
	case 'H': // Cursor Position
	case 'f': // Horizontal Vertical Position
		Coord.Y = CsiParameters[0];
		Coord.X = CsiParameters[1];
		return VTSetCursorPosition(ConsoleHandle, Coord);
	case 'J': // Erase in Display
		return ExecuteCSIErase(ConsoleHandle, CsiParameters[0], FALSE);
	case 'K': // Erase in Line
		return ExecuteCSIErase(ConsoleHandle, CsiParameters[0], TRUE);
	case 'c': // Device Attributes
		if (CsiParameters[0] == 0) {
			BOOLEAN Success;

			Success = InjectStringToConsoleInput(
				GetStdHandle(STD_INPUT_HANDLE),
				L"\x1b[?1;0c");

			ASSERT (Success);
			return Success;
		} else {
			KexLogDebugEvent(L"Invalid parameter %hu to CSI 'c' escape", CsiParameters[0]);
			KexDebugCheckpoint();
			return FALSE;
		}

		break;
	case 'h': // Set Mode
	case 'l': // Reset Mode
	{
		ULONG ModeIndex;

		if (!g_CsiPrivateMarker) {
			// Only private (DEC) modes are supported. Regular ANSI modes
			// such as insert/replace are meaningless on the Windows console.
			return FALSE;
		}

		for (ModeIndex = 0; ModeIndex < ParameterCount; ++ModeIndex) {
			switch (CsiParameters[ModeIndex]) {
			case 25: { // Show Cursor (h) / Hide Cursor (l)
				CONSOLE_CURSOR_INFO CursorInfo;

				if (GetConsoleCursorInfo(ConsoleHandle, &CursorInfo)) {
					CursorInfo.bVisible = (Command == 'h');
					SetConsoleCursorInfo(ConsoleHandle, &CursorInfo);
				}

				break;
			}
			case 9:		// X10 mouse reporting
			case 1000:	// Normal mouse tracking
			case 1002:	// Button-event mouse tracking
			case 1003:	// Any-event mouse tracking
				if (Command == 'h') {
					g_MouseTrackingMode = (ULONG) CsiParameters[ModeIndex];
					g_MouseButtonState = 0;
				} else if (g_MouseTrackingMode == (ULONG) CsiParameters[ModeIndex]) {
					g_MouseTrackingMode = MOUSE_TRACKING_OFF;
					g_MouseButtonState = 0;
				}

				UpdateConsoleMouseInputMode();
				break;
			case 1005:	// UTF-8 mouse coordinates
			case 1006:	// SGR mouse reporting
			case 1015:	// urxvt mouse reporting
				if (Command == 'h') {
					g_MouseEncoding = (ULONG) CsiParameters[ModeIndex];
				} else if (g_MouseEncoding == (ULONG) CsiParameters[ModeIndex]) {
					g_MouseEncoding = MOUSE_ENCODING_X10;
				}

				break;
			case 1004: { // Focus event reporting
				HANDLE InputHandle;

				InputHandle = GetStdHandle(STD_INPUT_HANDLE);

				if (IsConsoleInputHandle(InputHandle)) {
					InjectStringToConsoleInput(
						InputHandle,
						(Command == 'h') ? L"\x1b[I" : L"\x1b[O");
				}

				break;
			}
			case 1049: { // Alternate Screen Buffer (approximated by a clear)
				if (Command == 'h') {
					CONSOLE_SCREEN_BUFFER_INFO Sbi;
					COORD Origin = { 0, 0 };
					DWORD CellCount;
					DWORD CellsWritten;

					if (GetConsoleScreenBufferInfo(ConsoleHandle, &Sbi)) {
						CellCount = (DWORD) Sbi.dwSize.X * (DWORD) Sbi.dwSize.Y;

						FillConsoleOutputCharacter(
							ConsoleHandle,
							' ',
							CellCount,
							Origin,
							&CellsWritten);

						FillConsoleOutputAttribute(
							ConsoleHandle,
							g_DefaultTextAttributes,
							CellCount,
							Origin,
							&CellsWritten);

						SetConsoleCursorPosition(ConsoleHandle, Origin);
					}
				}

				break;
			}
			default:
				// Bracketed paste, synchronized updates and other modes have no
				// Windows console equivalent; ignore them.
				break;
			}
		}

		return TRUE;
	}
	case 'm': // Set Graphics Rendition (SGR)
		return ExecuteCSISetGraphicsRendition(ConsoleHandle, CsiParameters, ParameterCount);
	case 'n': // Device Status Report
		if (CsiParameters[0] == 6) {
			HANDLE InputHandle;
			WCHAR Reply[16];
			ULONG ReplyCch;
			COORD Position;

			InputHandle = GetStdHandle(STD_INPUT_HANDLE);

			if (!VTGetCursorPosition(ConsoleHandle, &Position) ||
				!IsConsoleInputHandle(InputHandle)) {

				return FALSE;
			}

			// Reply with the 1-based cursor position, e.g. ESC [ 12 ; 40 R
			ReplyCch = 0;
			Reply[ReplyCch++] = ESC;
			Reply[ReplyCch++] = '[';
			ReplyCch += AppendDecimalNumber(
				&Reply[ReplyCch],
				ARRAYSIZE(Reply) - ReplyCch,
				(ULONG) Position.Y);
			Reply[ReplyCch++] = ';';
			ReplyCch += AppendDecimalNumber(
				&Reply[ReplyCch],
				ARRAYSIZE(Reply) - ReplyCch,
				(ULONG) Position.X);
			Reply[ReplyCch++] = 'R';
			Reply[ReplyCch] = '\0';

			return InjectStringToConsoleInput(InputHandle, Reply);
		} else if (CsiParameters[0] == 5) { // Operating Status Report
			HANDLE InputHandle;

			InputHandle = GetStdHandle(STD_INPUT_HANDLE);

			if (!IsConsoleInputHandle(InputHandle)) {
				return FALSE;
			}

			return InjectStringToConsoleInput(InputHandle, L"\x1b[0n");
		}

		return FALSE;
	case 's': // Save Cursor (ANSI.SYS)
		return VTGetCursorPosition(ConsoleHandle, &g_SavedCursorPosition);
	case 'u': // Restore Cursor (ANSI.SYS)
		return VTSetCursorPosition(ConsoleHandle, g_SavedCursorPosition);
	default:
		KexLogDebugEvent(L"Unrecognized CSI ANSI escape '%c'", Command);
		KexDebugCheckpoint();
		return FALSE;
	}
}

STATIC BOOLEAN ProcessEscapeSequenceAorW(
	IN	HANDLE	ConsoleHandle,
	IN	PCVOID	Buffer,
	IN	ULONG	BufferCch,
	OUT	PULONG	CchProcessedOut OPTIONAL,
	IN	BOOLEAN	Unicode)
{
	WCHAR EscapeSequence[260];
	ULONG EscapeSequenceCch;
	ULONG CchProcessed;
	ULONG Index;

	ASSERT (BufferCch != 0);

	CchProcessed = 0;
	Index = 0;

	//
	// Fetch the escape sequence into the buffer, and convert to Unicode if it
	// is in ANSI format.
	// The buffer size is the maximum reasonable length of an escape sequence.
	//

	EscapeSequenceCch = min(BufferCch, ARRAYSIZE(EscapeSequence));

	if (EscapeSequenceCch < 2) {
		// Minimum length of an escape sequence is 2 characters.
		// If it's 1 character then we will just ignore it.
		// This means that there was just a stray ESC character at the end of
		// the buffer passed to WriteConsole.
		CchProcessed = EscapeSequenceCch;
		goto Exit;
	}
	
	if (Unicode) {
		RtlCopyMemory(EscapeSequence, Buffer, EscapeSequenceCch * sizeof(WCHAR));
	} else {
		EscapeSequenceCch = MultiByteToWideChar(
			CP_ACP,
			0,
			(PCSTR) Buffer,
			min(BufferCch, ARRAYSIZE(EscapeSequence) - 1),
			EscapeSequence,
			ARRAYSIZE(EscapeSequence));

		ASSERT (EscapeSequenceCch != 0);

		if (EscapeSequenceCch == 0) {
			// We're screwed
			return FALSE;
		}
	}

	//
	// Parse escape sequence and act on it if necessary.
	// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
	//

	ASSERT (EscapeSequence[0] == ESC);
	ASSERT (EscapeSequenceCch >= 2);

	CchProcessed = 2;

	switch (EscapeSequence[1]) {
	case 'M': {
		COORD RelativeMovement;
		// Reverse Index.
		// Move the cursor up by one line and maintain horizontal position.
		RelativeMovement.X = 0;
		RelativeMovement.Y = -1;
		VTMoveCursorRelative(ConsoleHandle, RelativeMovement);
		break;
			  }
	case '7':
		// Save Cursor Position in Memory
		VTGetCursorPosition(ConsoleHandle, &g_SavedCursorPosition);
		break;
	case '8':
		// Restore Cursor Position from Memory
		VTSetCursorPosition(ConsoleHandle, g_SavedCursorPosition);
		break;
	case '[': {
		PWCHAR Csi;
		ULONG CsiLength;
		SHORT CsiParameters[MAX_CSI_PARAMETER_COUNT] = {0};
		WCHAR Command;
		ULONG ParameterCount;

		// CSI

		if (EscapeSequenceCch < 3) {
			KexLogDebugEvent(L"Unterminated CSI ANSI escape");
			goto Exit;
		}

		//
		// Determine length of CSI sequence
		//

		Csi = &EscapeSequence[2];
		CsiLength = 0;

		until (Csi[CsiLength] >= 0x40 && Csi[CsiLength] <= 0x7E) {
			++CsiLength;

			if (CchProcessed + CsiLength >= EscapeSequenceCch) {
				KexLogDebugEvent(L"Unterminated CSI ANSI escape");
				goto Exit;
			}
		}

		// add 1 for the terminating character, which is the command
		++CsiLength;
		CchProcessed += CsiLength;

		// final character determines command
		Command = Csi[CsiLength - 1];

		// remember whether this is a private (DEC) sequence such as "?25l"
		g_CsiPrivateMarker = (Csi[0] == '?');

		//
		// Parse CSI sequence parameters
		//

		Index = 0;
		ParameterCount = 0;

		while (CsiLength != 0) {
			ULONG Parameter;
			ULONG NumberCch;

			Parameter = ParseNumber(Csi, CsiLength, &NumberCch);
			Csi += NumberCch;
			CsiLength -= NumberCch;

			if (NumberCch == 0) {
				if (CsiLength > 0 && *Csi == ';') {
					// treat as default 0
					ASSUME (Parameter == 0);
				} else {
					// no more parameters
					break;
				}
			}

			if (CsiLength > 0 && *Csi == ';') {
				// skip past semicolon separator
				++Csi;
				--CsiLength;
			}

			if (ParameterCount >= ARRAYSIZE(CsiParameters)) {
				KexLogDebugEvent(L"Too many numeric parameters in CSI ANSI escape");
				goto Exit;
			}

			if (Parameter > SHRT_MAX) {
				KexLogDebugEvent(L"Malformed number in CSI ANSI escape");
				goto Exit;
			}

			CsiParameters[ParameterCount] = (SHORT) Parameter;
			++ParameterCount;
		}

		ExecuteCSI(ConsoleHandle, Command, CsiParameters, ParameterCount);
		break;
			  }
	case ']': {
		PWCHAR Osc;
		ULONG OscLength;
		ULONG NumberOfTerminatingCharacters;

		// OSC

		//
		// Determine length of OSC sequence.
		// OSC sequences are terminated by BEL or by ST (ESC \)
		//

		Osc = &EscapeSequence[2];
		OscLength = 0;

		until (Osc[OscLength] == BEL || Osc[OscLength] == ESC) {
			++OscLength;

			if (CchProcessed + OscLength >= EscapeSequenceCch) {
				KexLogDebugEvent(L"Unterminated OSC ANSI escape sequence");
				goto Exit;
			}
		}

		if (Osc[OscLength] == BEL) {
			// add the BEL character
			NumberOfTerminatingCharacters = 1;
		} else {
			ASSUME (Osc[OscLength] == ESC);
			NumberOfTerminatingCharacters = 2;
		}

		OscLength += NumberOfTerminatingCharacters;
		CchProcessed += OscLength;

		if ((Osc[0] == '0' || Osc[0] == '2') && Osc[1] == ';') {
			BOOL Success;
			WCHAR ConsoleTitle[256];
			ULONG ConsoleTitleCch;

			// Set Window Title

			ConsoleTitleCch = OscLength - 2 - NumberOfTerminatingCharacters;

			//
			// Copy and null-terminate the title string.
			// MS docs say that 255 is the maximum length.
			//

			RtlCopyMemory(
				ConsoleTitle,
				&Osc[2],
				ConsoleTitleCch * sizeof(WCHAR));

			ConsoleTitle[ConsoleTitleCch] = '\0';

			//
			// Set the console title.
			//

			Success = SetConsoleTitle(ConsoleTitle);
			ASSERT (Success);
		}

		break;
			  }
	default:
		// Unknown two character escape
		KexLogDebugEvent(L"Unrecognized two-character ANSI escape '%c'", EscapeSequence[1]);
		break;
	}

Exit:
	if (CchProcessedOut) {
		*CchProcessedOut = CchProcessed;
	}

	return TRUE;
}

BOOL WriteConsoleWithEscapeSequencesAorW(
	IN	HANDLE	ConsoleHandle,
	IN	PCVOID	Buffer,
	IN	ULONG	CchToWrite,
	OUT	PULONG	CchWrittenOut OPTIONAL,
	IN	PVOID	Reserved OPTIONAL,
	IN	BOOLEAN	Unicode)
{
	BOOL Success;
	ULONG CchWritten;
	ULONG CbOfChar;

	Success = TRUE;
	CchWritten = 0;
	CbOfChar = Unicode ? sizeof(WCHAR) : sizeof(CHAR);

	until (CchToWrite == 0) {
		ULONG CchToNextEscape;

		//
		// Write to the console until we hit an escape sequence or hit the
		// end of the input buffer.
		//

		CchToNextEscape = GetCchToNextEscapeOrEndOfBufferAorW(
			Buffer,
			CchToWrite,
			Unicode);

		if (CchToNextEscape > 0) {
			ULONG CchWrittenThisCall;

			Success = WriteConsoleAorW(
				ConsoleHandle,
				Buffer,
				CchToNextEscape,
				&CchWrittenThisCall,
				NULL,
				Unicode);

			if (!Success) {
				goto Exit;
			}

			CchWritten += CchWrittenThisCall;
			CchToWrite -= CchWrittenThisCall;
			Buffer = RVA_TO_VA(Buffer, CchWrittenThisCall * CbOfChar);
		}

		//
		// If there's still more characters, that means we've hit an escape
		// sequence.
		//

		if (CchToWrite > 0) {
			ULONG CchProcessedThisCall;

			Success = ProcessEscapeSequenceAorW(
				ConsoleHandle,
				Buffer,
				CchToWrite,
				&CchProcessedThisCall,
				Unicode);

			if (!Success) {
				goto Exit;
			}

			CchWritten += CchProcessedThisCall;
			CchToWrite -= CchProcessedThisCall;
			Buffer = RVA_TO_VA(Buffer, CchProcessedThisCall * CbOfChar);
		}
	}

Exit:
	if (CchWrittenOut) {
		*CchWrittenOut = CchWritten;
	}

	return Success;
}