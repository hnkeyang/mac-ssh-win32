/*
	MacConnect - GUI MAC-Telnet Connect utility for Windows
	Copyright (C) 2024

	Based on MacSSH by Jo-Philipp Wich <jow@openwrt.org>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
*/

#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ws2tcpip.h>

#include "protocol.h"
#include "interfaces.h"
#include "utils.h"
#include "mndp.h"
#include "mactelnet_conn.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define PROGRAM_NAME "MacConnect"
#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "1.0"
#endif

#define IDM_CONTEXT_COPY 4001
#define IDM_CONTEXT_CONNECT 4002

#define IDC_MAC_ADDRESS 101
#define IDC_CONNECT_BTN 106
#define IDC_REFRESH_BTN 107
#define IDC_DEVICE_LIST 108

#define WM_MNDP_COMPLETE (WM_USER + 1)
#define WM_TERMINAL_DATA (WM_USER + 2)
#define WM_CONNECTION_STATUS (WM_USER + 3)

#define MAX_DEVICES 256
#define COL_MAC 0
#define COL_IP 1
#define COL_IPV6_LOCAL 2
#define COL_IDENTITY 3
#define COL_VERSION 4
#define COL_BOARD 5
#define COL_UPTIME 6

/* Terminal window dimensions */
#define TERM_WIDTH 80
#define TERM_HEIGHT 25
#define CHAR_WIDTH 9
#define CHAR_HEIGHT 16
#define TERM_FONT_SIZE 14
#define CURSOR_BLINK_MS 500

/* Device info structure */
struct device_info {
	unsigned char mac[ETH_ALEN];
	char mac_str[18];
	char ip_str[16];
	char ipv6_local_str[INET6_ADDRSTRLEN];
	char ipv6_global_str[INET6_ADDRSTRLEN];
	char identity[MT_MNDP_MAX_STRING_LENGTH];
	char version[MT_MNDP_MAX_STRING_LENGTH];
	char platform[MT_MNDP_MAX_STRING_LENGTH];
	char hardware[MT_MNDP_MAX_STRING_LENGTH];
	unsigned int uptime;
};

/* Terminal buffer structure */
struct terminal {
	HWND hWnd;
	HANDLE hThread;
	char screen[TERM_HEIGHT][TERM_WIDTH];
	int cursor_x;
	int cursor_y;
	int saved_cursor_x;
	int saved_cursor_y;
	COLORREF fg_color;
	COLORREF bg_color;
	HFONT hFont;
	BOOL cursor_visible;
	BOOL escape_mode;
	BOOL osc_mode;     /* TRUE while consuming OSC string (ESC ] ... BEL) */
	char escape_seq[128];
	int escape_len;
	int char_w;
	int char_h;
	int scroll_top;    /* Top row of scroll region (0-based) */
	int scroll_bottom; /* Bottom row of scroll region (0-based) */
};

/* Connection context for callbacks */
struct conn_context {
	HWND hTerminal;
	HWND hStatus;
	mactelnet_conn_t *conn;
	int connected;
};

/* Global variables */
static HINSTANCE hInst;
static HWND hMainWnd;
static HWND hDeviceList;
static HWND hMacEdit;
static HWND hConnectBtn;
static HWND hRefreshBtn;
static HWND hStatusBar;
static struct device_info devices[MAX_DEVICES];
static int device_count = 0;
static CRITICAL_SECTION device_cs;
static HANDLE hDiscoverThread = NULL;
static volatile BOOL discovering = FALSE;
static int g_clicked_column = 0;

/* Terminal window globals */
static struct terminal g_term = {0};
static struct conn_context g_conn_ctx = {0};
static mactelnet_conn_t g_conn;

/* Function prototypes */
static BOOL InitApplication(HINSTANCE hInstance);
static BOOL InitInstance(HINSTANCE hInstance, int nCmdShow);
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK TerminalWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static void CreateMainControls(HWND hWnd);
static void ResizeControls(HWND hWnd, int width, int height);
static void RefreshDeviceList(void);
static DWORD WINAPI DiscoverThreadProc(LPVOID lpParam);
static void AddDeviceToList(struct mndphost *host);
static void UpdateDeviceListView(void);
static void OnDeviceSelected(int index);
static void OnConnect(void);
static void FormatUptime(unsigned int uptime, char *buf, size_t buflen);
static void CopySelectedCellToClipboard(void);
static void CopySelectedRowToClipboard(void);

/* Terminal functions */
static BOOL CreateTerminalWindow(void);
static void TerminalInit(void);
static void TerminalPaint(HWND hWnd);
static void TerminalPutChar(char c);
static void TerminalProcessData(const unsigned char *data, int len);
static DWORD WINAPI TerminalThreadProc(LPVOID lpParam);

/* Connection callbacks */
static void OnDataReceived(const unsigned char *data, int len, void *userdata);
static void OnStatusChanged(int status, const char *msg, void *userdata);

/* WinMain entry point */
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	MSG msg;
	WSADATA wsd;

	WSAStartup(MAKEWORD(2, 2), &wsd);

	INITCOMMONCONTROLSEX iccex;
	iccex.dwSize = sizeof(iccex);
	iccex.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
	InitCommonControlsEx(&iccex);

	InitializeCriticalSection(&device_cs);
	mactelnet_conn_init(&g_conn);

	if (!InitApplication(hInstance))
		return FALSE;

	if (!InitInstance(hInstance, nCmdShow))
		return FALSE;

	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	DeleteCriticalSection(&device_cs);
	WSACleanup();

	return (int)msg.wParam;
}

/* Initialize application */
static BOOL InitApplication(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;

	/* Main window class */
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = MainWndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = "MacConnectMainWnd";
	wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

	if (!RegisterClassEx(&wcex))
		return FALSE;

	/* Terminal window class */
	wcex.lpfnWndProc = TerminalWndProc;
	wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wcex.lpszClassName = "MacConnectTerminal";
	wcex.hCursor = LoadCursor(NULL, IDC_IBEAM);

	return RegisterClassEx(&wcex);
}

/* Initialize instance */
static BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	hInst = hInstance;

	hMainWnd = CreateWindow(
		"MacConnectMainWnd",
		PROGRAM_NAME " v" PROGRAM_VERSION,
		WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
		CW_USEDEFAULT, 0, 900, 600,
		NULL, NULL, hInstance, NULL);

	if (!hMainWnd)
		return FALSE;

	ShowWindow(hMainWnd, nCmdShow);
	UpdateWindow(hMainWnd);

	return TRUE;
}

/* Create main window controls */
static void CreateMainControls(HWND hWnd)
{
	HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	
	/* MAC Address label and edit */
	CreateWindow("STATIC", "MAC Address:",
		WS_VISIBLE | WS_CHILD | SS_LEFT,
		10, 10, 80, 20, hWnd, NULL, hInst, NULL);
	
	hMacEdit = CreateWindow("EDIT", "",
		WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
		100, 10, 200, 20, hWnd, (HMENU)IDC_MAC_ADDRESS, hInst, NULL);
	
	/* Connect button */
	hConnectBtn = CreateWindow("BUTTON", "Connect",
		WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
		320, 8, 100, 26, hWnd, (HMENU)IDC_CONNECT_BTN, hInst, NULL);
	
	/* Refresh button */
	hRefreshBtn = CreateWindow("BUTTON", "Refresh",
		WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
		430, 8, 100, 26, hWnd, (HMENU)IDC_REFRESH_BTN, hInst, NULL);
	
	/* Device list view */
	hDeviceList = CreateWindow(WC_LISTVIEW, "",
		WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
		10, 50, 860, 450, hWnd, (HMENU)IDC_DEVICE_LIST, hInst, NULL);
	
	ListView_SetExtendedListViewStyle(hDeviceList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	
	/* Set up list view columns */
	LVCOLUMN lvc;
	lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	
	lvc.iSubItem = COL_MAC;
	lvc.pszText = "MAC Address";
	lvc.cx = 125;
	ListView_InsertColumn(hDeviceList, COL_MAC, &lvc);
	
	lvc.iSubItem = COL_IP;
	lvc.pszText = "IP Address";
	lvc.cx = 100;
	ListView_InsertColumn(hDeviceList, COL_IP, &lvc);
	
	lvc.iSubItem = COL_IPV6_LOCAL;
	lvc.pszText = "IPv6";
	lvc.cx = 160;
	ListView_InsertColumn(hDeviceList, COL_IPV6_LOCAL, &lvc);
	
	lvc.iSubItem = COL_IDENTITY;
	lvc.pszText = "Identity";
	lvc.cx = 130;
	ListView_InsertColumn(hDeviceList, COL_IDENTITY, &lvc);
	
	lvc.iSubItem = COL_VERSION;
	lvc.pszText = "Version";
	lvc.cx = 90;
	ListView_InsertColumn(hDeviceList, COL_VERSION, &lvc);
	
	lvc.iSubItem = COL_BOARD;
	lvc.pszText = "Board";
	lvc.cx = 90;
	ListView_InsertColumn(hDeviceList, COL_BOARD, &lvc);
	
	lvc.iSubItem = COL_UPTIME;
	lvc.pszText = "Uptime";
	lvc.cx = 100;
	ListView_InsertColumn(hDeviceList, COL_UPTIME, &lvc);
	
	/* Status bar */
	hStatusBar = CreateWindow(STATUSCLASSNAME, NULL,
		WS_VISIBLE | WS_CHILD | SBARS_SIZEGRIP,
		0, 0, 0, 0, hWnd, NULL, hInst, NULL);
	
	int parts[] = { 300, 600, -1 };
	SendMessage(hStatusBar, SB_SETPARTS, 3, (LPARAM)parts);
	SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"Ready");
	
	/* Set fonts */
	SendMessage(hMacEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(hConnectBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(hRefreshBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessage(hDeviceList, WM_SETFONT, (WPARAM)hFont, TRUE);
}

/* Resize controls */
static void ResizeControls(HWND hWnd, int width, int height)
{
	if (hDeviceList)
		SetWindowPos(hDeviceList, NULL, 10, 50, width - 40, height - 130, SWP_NOZORDER);
	
	if (hStatusBar)
		SendMessage(hStatusBar, WM_SIZE, 0, 0);
}

/* Format uptime */
static void FormatUptime(unsigned int uptime, char *buf, size_t buflen)
{
	int days = uptime / 86400;
	int hours = (uptime % 86400) / 3600;
	int mins = (uptime % 3600) / 60;
	int secs = uptime % 60;
	
	if (days > 0)
		snprintf(buf, buflen, "%dd %02d:%02d:%02d", days, hours, mins, secs);
	else
		snprintf(buf, buflen, "%02d:%02d:%02d", hours, mins, secs);
}

/* Add device to list */
static void AddDeviceToList(struct mndphost *host)
{
	if (device_count >= MAX_DEVICES)
		return;
	
	EnterCriticalSection(&device_cs);
	
	struct device_info *dev = &devices[device_count];
	memcpy(dev->mac, host->address, ETH_ALEN);
	
	snprintf(dev->mac_str, sizeof(dev->mac_str),
		"%02X:%02X:%02X:%02X:%02X:%02X",
		host->address[0], host->address[1], host->address[2],
		host->address[3], host->address[4], host->address[5]);
	
	if (host->has_ipv4 && host->ipv4_addr) {
		strncpy(dev->ip_str, inet_ntoa(*host->ipv4_addr), sizeof(dev->ip_str) - 1);
		dev->ip_str[sizeof(dev->ip_str) - 1] = '\0';
	} else {
		strcpy(dev->ip_str, "");
	}
	
	if (host->has_ipv6_local && host->ipv6_local) {
		inet_ntop(AF_INET6, host->ipv6_local, dev->ipv6_local_str, sizeof(dev->ipv6_local_str));
	} else {
		strcpy(dev->ipv6_local_str, "");
	}
	
	if (host->has_ipv6_global && host->ipv6_global) {
		inet_ntop(AF_INET6, host->ipv6_global, dev->ipv6_global_str, sizeof(dev->ipv6_global_str));
	} else {
		strcpy(dev->ipv6_global_str, "");
	}
	
	strncpy(dev->identity, host->identity ? host->identity : "", sizeof(dev->identity) - 1);
	dev->identity[sizeof(dev->identity) - 1] = '\0';
	
	strncpy(dev->version, host->version ? host->version : "", sizeof(dev->version) - 1);
	dev->version[sizeof(dev->version) - 1] = '\0';
	
	strncpy(dev->platform, host->platform ? host->platform : "", sizeof(dev->platform) - 1);
	dev->platform[sizeof(dev->platform) - 1] = '\0';
	
	strncpy(dev->hardware, host->hardware ? host->hardware : "", sizeof(dev->hardware) - 1);
	dev->hardware[sizeof(dev->hardware) - 1] = '\0';
	
	dev->uptime = host->uptime;
	
	device_count++;
	
	LeaveCriticalSection(&device_cs);
}

/* Update list view */
static void UpdateDeviceListView(void)
{
	char uptime_str[64];
	
	EnterCriticalSection(&device_cs);
	
	ListView_DeleteAllItems(hDeviceList);
	
	for (int i = 0; i < device_count; i++) {
		LVITEM lvi;
		lvi.mask = LVIF_TEXT | LVIF_PARAM;
		lvi.iItem = i;
		lvi.iSubItem = 0;
		lvi.pszText = devices[i].mac_str;
		lvi.lParam = i;
		
		int item = ListView_InsertItem(hDeviceList, &lvi);
		
		ListView_SetItemText(hDeviceList, item, COL_IP, devices[i].ip_str);
		ListView_SetItemText(hDeviceList, item, COL_IPV6_LOCAL, devices[i].ipv6_local_str);
		ListView_SetItemText(hDeviceList, item, COL_IDENTITY, devices[i].identity);
		ListView_SetItemText(hDeviceList, item, COL_VERSION, devices[i].version);
		ListView_SetItemText(hDeviceList, item, COL_BOARD, devices[i].hardware);
		
		FormatUptime(devices[i].uptime, uptime_str, sizeof(uptime_str));
		ListView_SetItemText(hDeviceList, item, COL_UPTIME, uptime_str);
	}
	
	LeaveCriticalSection(&device_cs);
}

/* Discovery thread */
static DWORD WINAPI DiscoverThreadProc(LPVOID lpParam)
{
	struct mndphost *host;
	
	discovering = TRUE;
	
	mndp_free_hosts();
	net_enum_ifaces();
	
	int found = mndp_discover(3);
	
	if (found > 0) {
		list_for_each_entry(host, &mndphosts, list) {
			AddDeviceToList(host);
		}
	}
	
	PostMessage(hMainWnd, WM_MNDP_COMPLETE, (WPARAM)found, 0);
	
	discovering = FALSE;
	return 0;
}

/* Refresh device list */
static void RefreshDeviceList(void)
{
	if (discovering)
		return;
	
	/* Clear list immediately so the user sees a fresh start */
	ListView_DeleteAllItems(hDeviceList);
	EnterCriticalSection(&device_cs);
	device_count = 0;
	LeaveCriticalSection(&device_cs);
	
	SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"Discovering devices...");
	EnableWindow(hRefreshBtn, FALSE);
	
	if (hDiscoverThread) {
		CloseHandle(hDiscoverThread);
	}
	
	hDiscoverThread = CreateThread(NULL, 0, DiscoverThreadProc, NULL, 0, NULL);
}

/* Device selection */
static void OnDeviceSelected(int index)
{
	if (index < 0 || index >= device_count)
		return;
	
	SetWindowText(hMacEdit, devices[index].mac_str);
}

/* Copy functions */
static void CopySelectedCellToClipboard(void)
{
	int selected = ListView_GetNextItem(hDeviceList, -1, LVNI_SELECTED);
	if (selected < 0 || selected >= device_count)
		return;
	
	char buffer[256];
	LVITEM lvi;
	lvi.iItem = selected;
	lvi.iSubItem = g_clicked_column;
	lvi.mask = LVIF_TEXT;
	lvi.pszText = buffer;
	lvi.cchTextMax = sizeof(buffer);
	ListView_GetItem(hDeviceList, &lvi);
	
	if (!OpenClipboard(hMainWnd))
		return;
	
	EmptyClipboard();
	
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, strlen(buffer) + 1);
	if (hMem) {
		char *pMem = (char *)GlobalLock(hMem);
		if (pMem) {
			strcpy(pMem, buffer);
			GlobalUnlock(hMem);
			SetClipboardData(CF_TEXT, hMem);
		}
	}
	
	CloseClipboard();
}

static void CopySelectedRowToClipboard(void)
{
	int selected = ListView_GetNextItem(hDeviceList, -1, LVNI_SELECTED);
	if (selected < 0 || selected >= device_count)
		return;
	
	struct device_info *dev = &devices[selected];
	char buffer[512];
	char uptime_str[64];
	
	FormatUptime(dev->uptime, uptime_str, sizeof(uptime_str));
	
	snprintf(buffer, sizeof(buffer),
		"%s\t%s\t%s\t%s\t%s\t%s\t%s",
		dev->mac_str, dev->ip_str, dev->ipv6_local_str,
		dev->identity, dev->version, dev->hardware, uptime_str);
	
	if (!OpenClipboard(hMainWnd))
		return;
	
	EmptyClipboard();
	
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, strlen(buffer) + 1);
	if (hMem) {
		char *pMem = (char *)GlobalLock(hMem);
		if (pMem) {
			strcpy(pMem, buffer);
			GlobalUnlock(hMem);
			SetClipboardData(CF_TEXT, hMem);
		}
	}
	
	CloseClipboard();
}

/* Terminal window functions */
static void TerminalInit(void)
{
	memset(g_term.screen, ' ', sizeof(g_term.screen));
	g_term.cursor_x = 0;
	g_term.cursor_y = 0;
	g_term.saved_cursor_x = 0;
	g_term.saved_cursor_y = 0;
	g_term.fg_color = RGB(192, 192, 192);
	g_term.bg_color = RGB(0, 0, 0);
	g_term.cursor_visible = TRUE;
	g_term.escape_mode = FALSE;
	g_term.osc_mode = FALSE;
	g_term.escape_len = 0;
	g_term.char_w = CHAR_WIDTH;
	g_term.char_h = CHAR_HEIGHT;
	g_term.scroll_top = 0;
	g_term.scroll_bottom = TERM_HEIGHT - 1;
}

static void TerminalPaint(HWND hWnd)
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hWnd, &ps);
	
	RECT rect;
	GetClientRect(hWnd, &rect);
	
	int cw = g_term.char_w;
	int ch = g_term.char_h;
	
	/* Double-buffer: render to offscreen bitmap, then blit */
	HDC hdcMem = CreateCompatibleDC(hdc);
	HBITMAP hBmp = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
	HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);
	
	/* Fill entire background */
	HBRUSH hBrush = CreateSolidBrush(g_term.bg_color);
	FillRect(hdcMem, &rect, hBrush);
	DeleteObject(hBrush);
	
	/* Set up font and colors */
	SelectObject(hdcMem, g_term.hFont);
	SetTextColor(hdcMem, g_term.fg_color);
	SetBkColor(hdcMem, g_term.bg_color);
	SetBkMode(hdcMem, OPAQUE);
	
	/* Draw every row as full TERM_WIDTH to avoid ghost characters.
	 * Force uniform dx spacing so cursor pixel position matches screen buffer column. */
	char line[TERM_WIDTH + 1];
	INT dx[TERM_WIDTH];
	for (int x = 0; x < TERM_WIDTH; x++) dx[x] = cw;
	for (int y = 0; y < TERM_HEIGHT; y++) {
		memcpy(line, g_term.screen[y], TERM_WIDTH);
		/* Replace non-printable chars with space */
		for (int x = 0; x < TERM_WIDTH; x++) {
			if ((unsigned char)line[x] < 32)
				line[x] = ' ';
		}
		line[TERM_WIDTH] = '\0';
		ExtTextOut(hdcMem, 0, y * ch, ETO_OPAQUE, NULL, line, TERM_WIDTH, dx);
	}
	
	/* Draw cursor block (inverted) */
	if (g_term.cursor_visible) {
		RECT cursorRect;
		cursorRect.left = g_term.cursor_x * cw;
		cursorRect.top = g_term.cursor_y * ch;
		cursorRect.right = cursorRect.left + cw;
		cursorRect.bottom = cursorRect.top + ch;
		InvertRect(hdcMem, &cursorRect);
	}
	
	/* Blit offscreen buffer to screen */
	BitBlt(hdc, 0, 0, rect.right, rect.bottom, hdcMem, 0, 0, SRCCOPY);
	
	/* Cleanup */
	SelectObject(hdcMem, hOldBmp);
	DeleteObject(hBmp);
	DeleteDC(hdcMem);
	
	EndPaint(hWnd, &ps);
}

static void TerminalScrollUp(void)
{
	int top = g_term.scroll_top;
	int bot = g_term.scroll_bottom;
	if (bot > top)
		memmove(g_term.screen[top], g_term.screen[top + 1], (bot - top) * TERM_WIDTH);
	memset(g_term.screen[bot], ' ', TERM_WIDTH);
}

static void TerminalScrollDown(void)
{
	int top = g_term.scroll_top;
	int bot = g_term.scroll_bottom;
	if (bot > top)
		memmove(g_term.screen[top + 1], g_term.screen[top], (bot - top) * TERM_WIDTH);
	memset(g_term.screen[top], ' ', TERM_WIDTH);
}

static void TerminalEraseLine(void)
{
	memset(g_term.screen[g_term.cursor_y], ' ', TERM_WIDTH);
}

static void TerminalEraseScreen(void)
{
	for (int y = 0; y < TERM_HEIGHT; y++)
		memset(g_term.screen[y], ' ', TERM_WIDTH);
	g_term.cursor_x = 0;
	g_term.cursor_y = 0;
}

static void TerminalProcessEscape(void)
{
	if (g_term.escape_len < 2)
		return;

	/* 2-char escape sequences: ESC + letter, no '[' */
	if (g_term.escape_seq[1] != '[') {
		switch ((unsigned char)g_term.escape_seq[1]) {
		case 'M': /* Reverse Index */
			if (g_term.cursor_y <= g_term.scroll_top)
				TerminalScrollDown();
			else
				g_term.cursor_y--;
			break;
		case 'D': /* Index */
			if (g_term.cursor_y >= g_term.scroll_bottom)
				TerminalScrollUp();
			else
				g_term.cursor_y++;
			break;
		case '7': /* Save cursor */
			g_term.saved_cursor_x = g_term.cursor_x;
			g_term.saved_cursor_y = g_term.cursor_y;
			break;
		case '8': /* Restore cursor */
			g_term.cursor_x = g_term.saved_cursor_x;
			g_term.cursor_y = g_term.saved_cursor_y;
			break;
		}
		return;
	}

	/* CSI: need at least ESC [ type */
	if (g_term.escape_len < 3)
		return;

	char type = g_term.escape_seq[g_term.escape_len - 1];

	/* Copy params area (skip 'ESC [') into mutable buffer, strip trailing type */
	char pb[128];
	strncpy(pb, g_term.escape_seq + 2, sizeof(pb) - 1);
	pb[sizeof(pb) - 1] = '\0';
	int pblen = (int)strlen(pb);
	if (pblen > 0) pb[pblen - 1] = '\0'; /* remove the type char */
	char *p = pb;
	if (*p == '?' || *p == '>') p++; /* skip private prefix */

	switch (type) {
	case 'H': case 'f': {
		int row = 1, col = 1;
		char *semi = strchr(p, ';');
		if (semi) { *semi = '\0'; row = atoi(p); col = atoi(semi + 1); }
		else { row = atoi(p); }
		if (row < 1) row = 1; if (col < 1) col = 1;
		g_term.cursor_y = row - 1;
		g_term.cursor_x = col - 1;
		if (g_term.cursor_y >= TERM_HEIGHT) g_term.cursor_y = TERM_HEIGHT - 1;
		if (g_term.cursor_x >= TERM_WIDTH)  g_term.cursor_x = TERM_WIDTH - 1;
		break;
	}
	case 'A': { int n = atoi(p); if (n<1) n=1; g_term.cursor_y -= n; if (g_term.cursor_y < 0) g_term.cursor_y = 0; break; }
	case 'B': { int n = atoi(p); if (n<1) n=1; g_term.cursor_y += n; if (g_term.cursor_y >= TERM_HEIGHT) g_term.cursor_y = TERM_HEIGHT-1; break; }
	case 'C': { int n = atoi(p); if (n<1) n=1; g_term.cursor_x += n; if (g_term.cursor_x >= TERM_WIDTH) g_term.cursor_x = TERM_WIDTH-1; break; }
	case 'D': { int n = atoi(p); if (n<1) n=1; g_term.cursor_x -= n; if (g_term.cursor_x < 0) g_term.cursor_x = 0; break; }
	case 'E': { int n = atoi(p); if (n<1) n=1; g_term.cursor_y += n; g_term.cursor_x = 0; if (g_term.cursor_y >= TERM_HEIGHT) g_term.cursor_y = TERM_HEIGHT-1; break; }
	case 'F': { int n = atoi(p); if (n<1) n=1; g_term.cursor_y -= n; g_term.cursor_x = 0; if (g_term.cursor_y < 0) g_term.cursor_y = 0; break; }
	case 'G': { /* Cursor Horizontal Absolute */
		int col = atoi(p); if (col < 1) col = 1;
		g_term.cursor_x = col - 1;
		if (g_term.cursor_x >= TERM_WIDTH) g_term.cursor_x = TERM_WIDTH - 1;
		break;
	}
	case 'd': { /* Cursor Vertical Absolute */
		int row = atoi(p); if (row < 1) row = 1;
		g_term.cursor_y = row - 1;
		if (g_term.cursor_y >= TERM_HEIGHT) g_term.cursor_y = TERM_HEIGHT - 1;
		break;
	}
	case 'J': {
		int mode = (*p >= '0' && *p <= '9') ? (*p - '0') : 0;
		if (mode == 2) { TerminalEraseScreen(); }
		else if (mode == 1) {
			for (int y = 0; y < g_term.cursor_y; y++) memset(g_term.screen[y], ' ', TERM_WIDTH);
			memset(g_term.screen[g_term.cursor_y], ' ', g_term.cursor_x + 1);
		} else {
			memset(&g_term.screen[g_term.cursor_y][g_term.cursor_x], ' ', TERM_WIDTH - g_term.cursor_x);
			for (int y = g_term.cursor_y + 1; y < TERM_HEIGHT; y++) memset(g_term.screen[y], ' ', TERM_WIDTH);
		}
		break;
	}
	case 'K': {
		int mode = (*p >= '0' && *p <= '9') ? (*p - '0') : 0;
		if (mode == 2) TerminalEraseLine();
		else if (mode == 1) memset(g_term.screen[g_term.cursor_y], ' ', g_term.cursor_x + 1);
		else memset(&g_term.screen[g_term.cursor_y][g_term.cursor_x], ' ', TERM_WIDTH - g_term.cursor_x);
		break;
	}
	case 'L': { int n = atoi(p); if (n<1) n=1; for (int i=0;i<n;i++) TerminalScrollDown(); break; }
	case 'M': { int n = atoi(p); if (n<1) n=1; for (int i=0;i<n;i++) TerminalScrollUp();   break; }
	case 'P': { /* Delete characters */
		int n = atoi(p); if (n<1) n=1;
		int cx = g_term.cursor_x;
		if (cx + n > TERM_WIDTH) n = TERM_WIDTH - cx;
		memmove(&g_term.screen[g_term.cursor_y][cx], &g_term.screen[g_term.cursor_y][cx+n], TERM_WIDTH-cx-n);
		memset(&g_term.screen[g_term.cursor_y][TERM_WIDTH-n], ' ', n);
		break;
	}
	case '@': { /* Insert characters */
		int n = atoi(p); if (n<1) n=1;
		int cx = g_term.cursor_x;
		if (cx + n > TERM_WIDTH) n = TERM_WIDTH - cx;
		memmove(&g_term.screen[g_term.cursor_y][cx+n], &g_term.screen[g_term.cursor_y][cx], TERM_WIDTH-cx-n);
		memset(&g_term.screen[g_term.cursor_y][cx], ' ', n);
		break;
	}
	case 'r': { /* Set scrolling region */
		int top = 1, bot = TERM_HEIGHT;
		char *semi = strchr(p, ';');
		if (semi) { *semi = '\0'; top = atoi(p); bot = atoi(semi + 1); }
		if (top < 1) top = 1;
		if (bot > TERM_HEIGHT) bot = TERM_HEIGHT;
		if (top < bot) { g_term.scroll_top = top - 1; g_term.scroll_bottom = bot - 1; }
		g_term.cursor_x = 0; g_term.cursor_y = g_term.scroll_top;
		break;
	}
	case 'n': { /* Device Status Report — respond with cursor position */
		int mode = (*p >= '0' && *p <= '9') ? (*p - '0') : 0;
		if (mode == 6) {
			char resp[32];
			snprintf(resp, sizeof(resp), "\x1b[%d;%dR", g_term.cursor_y+1, g_term.cursor_x+1);
			mactelnet_send(&g_conn, (unsigned char *)resp, (int)strlen(resp));
		}
		break;
	}
	case 'X': { /* Erase N characters (ECH) — no cursor move */
		int n = atoi(p); if (n < 1) n = 1;
		int cx = g_term.cursor_x;
		if (cx + n > TERM_WIDTH) n = TERM_WIDTH - cx;
		memset(&g_term.screen[g_term.cursor_y][cx], ' ', n);
		break;
	}
	case 'h': /* DEC private set */
		if (*pb == '?') {
			int mode = atoi(p);
			if (mode == 25) g_term.cursor_visible = TRUE;
			/* 1049: alt screen — not supported, ignore */
		}
		break;
	case 'l': /* DEC private reset */
		if (*pb == '?') {
			int mode = atoi(p);
			if (mode == 25) g_term.cursor_visible = FALSE;
		}
		break;
	case 's': g_term.saved_cursor_x = g_term.cursor_x; g_term.saved_cursor_y = g_term.cursor_y; break;
	case 'u': g_term.cursor_x = g_term.saved_cursor_x; g_term.cursor_y = g_term.saved_cursor_y; break;
	default: break; /* ignore h, l, m, etc. */
	}
}

/* Process a single character */
static void TerminalPutChar(char c)
{
	/* OSC mode: swallow everything until BEL or ESC (start of ESC \) */
	if (g_term.osc_mode) {
		if (c == '\a') {
			/* BEL terminates OSC */
			g_term.osc_mode = FALSE;
		} else if (c == 0x1B) {
			/* ESC could be start of ST (ESC \); exit OSC and begin new escape */
			g_term.osc_mode = FALSE;
			g_term.escape_mode = TRUE;
			g_term.escape_len = 0;
			g_term.escape_seq[g_term.escape_len++] = c;
			g_term.escape_seq[g_term.escape_len] = '\0';
		}
		/* all other chars inside OSC are silently consumed */
		return;
	}

	if (g_term.escape_mode) {
		if (g_term.escape_len < (int)sizeof(g_term.escape_seq) - 1) {
			g_term.escape_seq[g_term.escape_len++] = c;
			g_term.escape_seq[g_term.escape_len] = '\0';
		}
		if (g_term.escape_len == 2) {
			if (c == '[') return; /* CSI: collect params */
			if (c == ']') {
				/* OSC: ESC ] — consume until BEL or ESC \ */
				g_term.osc_mode = TRUE;
				g_term.escape_mode = FALSE;
				g_term.escape_len = 0;
				return;
			}
			if (c == '(' || c == ')' || c == '*' || c == '+') {
				/* Charset designation: ESC ( X — need one more char (the set id) */
				return;
			}
			/* 2-char ESC sequence complete (ESC M, ESC D, ESC 7, ESC 8, etc.) */
			TerminalProcessEscape();
			g_term.escape_mode = FALSE; g_term.escape_len = 0;
			return;
		}
		/* 3-char charset designation: ESC ( X — consume the set-id char silently */
		if (g_term.escape_len == 3) {
			char sec = g_term.escape_seq[1];
			if (sec == '(' || sec == ')' || sec == '*' || sec == '+') {
				g_term.escape_mode = FALSE; g_term.escape_len = 0;
				return;
			}
		}
		/* CSI: done on letter terminator */
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '@' || c == '`') {
			TerminalProcessEscape();
			g_term.escape_mode = FALSE; g_term.escape_len = 0;
		}
		return;
	}

	if (c == 0x1B) {
		g_term.escape_mode = TRUE; g_term.escape_len = 0;
		g_term.escape_seq[g_term.escape_len++] = c;
		g_term.escape_seq[g_term.escape_len] = '\0';
		return;
	}

	if (c == '\a') return;
	if (c == '\r') { g_term.cursor_x = 0; return; }

	if (c == '\n') {
		/* LF: also reset cursor_x. PTY ONLCR should give us \r\n, but bare \n
		 * from some sequences needs to land at column 0 to avoid garbling. */
		g_term.cursor_x = 0;
		if (g_term.cursor_y >= g_term.scroll_bottom)
			TerminalScrollUp();
		else
			g_term.cursor_y++;
		return;
	}

	if (c == '\b') { if (g_term.cursor_x > 0) g_term.cursor_x--; return; }

	if (c == '\t') {
		int nx = (g_term.cursor_x + 8) & ~7;
		if (nx >= TERM_WIDTH) nx = TERM_WIDTH - 1;
		while (g_term.cursor_x < nx)
			g_term.screen[g_term.cursor_y][g_term.cursor_x++] = ' ';
		return;
	}

	if ((unsigned char)c >= 32) {
		if (g_term.cursor_x >= TERM_WIDTH) {
			g_term.cursor_x = 0;
			if (g_term.cursor_y >= g_term.scroll_bottom) TerminalScrollUp();
			else g_term.cursor_y++;
		}
		g_term.screen[g_term.cursor_y][g_term.cursor_x++] = c;
	}
}

static void TerminalProcessData(const unsigned char *data, int len)
{
	for (int i = 0; i < len; i++) {
		TerminalPutChar((char)data[i]);
	}
	
	if (g_term.hWnd)
		InvalidateRect(g_term.hWnd, NULL, FALSE);
}

static LRESULT CALLBACK TerminalWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_CREATE:
		g_term.hWnd = hWnd;
		TerminalInit();
		/* Create fixed-pitch font matching PuTTY style */
		g_term.hFont = CreateFont(
			-TERM_FONT_SIZE,        /* Height - negative for character height */
			0,                      /* Width - 0 for default */
			0, 0,                   /* Escapement and orientation */
			FW_NORMAL,              /* Weight */
			FALSE,                  /* Italic */
			FALSE,                  /* Underline */
			FALSE,                  /* StrikeOut */
			DEFAULT_CHARSET,        /* Charset */
			OUT_OUTLINE_PRECIS,     /* OutPrecision */
			CLIP_DEFAULT_PRECIS,    /* ClipPrecision */
			CLEARTYPE_QUALITY,      /* Quality - ClearType for smooth text */
			FIXED_PITCH | FF_MODERN,/* PitchAndFamily */
			"Consolas");            /* FaceName */
		
		/* Measure actual character size from font metrics */
		{
			HDC hdc = GetDC(hWnd);
			HFONT hOldFont = (HFONT)SelectObject(hdc, g_term.hFont);
			TEXTMETRIC tm;
			GetTextMetrics(hdc, &tm);
			g_term.char_w = tm.tmAveCharWidth;
			g_term.char_h = tm.tmHeight + tm.tmExternalLeading;
			SelectObject(hdc, hOldFont);
			ReleaseDC(hWnd, hdc);
		}
		
		/* Resize window to fit exactly TERM_WIDTH x TERM_HEIGHT characters */
		{
			int client_w = TERM_WIDTH * g_term.char_w;
			int client_h = TERM_HEIGHT * g_term.char_h;
			RECT wr = {0, 0, client_w, client_h};
			AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);
			SetWindowPos(hWnd, NULL, 0, 0,
				wr.right - wr.left, wr.bottom - wr.top,
				SWP_NOMOVE | SWP_NOZORDER);
		}
		
		/* Start cursor blink timer */
		SetTimer(hWnd, 1, CURSOR_BLINK_MS, NULL);
		return 0;
		
	case WM_PAINT:
		TerminalPaint(hWnd);
		return 0;
		
	case WM_CHAR:
		/* Send character to server */
		if (g_conn_ctx.connected && g_conn.connected) {
			unsigned char c = (unsigned char)wParam;
			mactelnet_send(&g_conn, &c, 1);
			/* Debug: show last sent byte in title */
			{
				char title[64];
				snprintf(title, sizeof(title), "MacConnect Terminal [sent 0x%02X]", c);
				SetWindowText(hWnd, title);
			}
		}
		return 0;
		
	case WM_KEYDOWN:
		/* Handle special keys NOT generated by WM_CHAR (arrows, etc.) */
		/* VK_RETURN, VK_BACK, VK_TAB are handled in WM_CHAR - do NOT handle here */
		if (g_conn_ctx.connected && g_conn.connected) {
			unsigned char seq[8];
			int len = 0;
			
			switch (wParam) {
			case VK_UP:
				seq[0] = 0x1B; seq[1] = '['; seq[2] = 'A';
				len = 3;
				break;
			case VK_DOWN:
				seq[0] = 0x1B; seq[1] = '['; seq[2] = 'B';
				len = 3;
				break;
			case VK_RIGHT:
				seq[0] = 0x1B; seq[1] = '['; seq[2] = 'C';
				len = 3;
				break;
			case VK_LEFT:
				seq[0] = 0x1B; seq[1] = '['; seq[2] = 'D';
				len = 3;
				break;
			case VK_HOME:
				seq[0] = 0x1B; seq[1] = '['; seq[2] = 'H';
				len = 3;
				break;
			case VK_END:
				seq[0] = 0x1B; seq[1] = '['; seq[2] = 'F';
				len = 3;
				break;
			case VK_DELETE:
				seq[0] = 0x1B; seq[1] = '['; seq[2] = '3'; seq[3] = '~';
				len = 4;
				break;
			}
			
			if (len > 0)
				mactelnet_send(&g_conn, seq, len);
		}
		return 0;
		
	case WM_TIMER:
		if (wParam == 1) {
			/* Cursor blink timer - just toggle and repaint cursor cell */
			g_term.cursor_visible = !g_term.cursor_visible;
			if (g_term.hWnd) {
				RECT cursorRect;
				cursorRect.left = g_term.cursor_x * g_term.char_w;
				cursorRect.top = g_term.cursor_y * g_term.char_h;
				cursorRect.right = cursorRect.left + g_term.char_w;
				cursorRect.bottom = cursorRect.top + g_term.char_h;
				InvalidateRect(g_term.hWnd, &cursorRect, FALSE);
			}
		}
		return 0;
		
	case WM_DESTROY:
		KillTimer(hWnd, 1);
		if (g_term.hFont)
			DeleteObject(g_term.hFont);
		g_term.hWnd = NULL;
		mactelnet_disconnect(&g_conn);
		g_conn_ctx.connected = 0;
		return 0;
		
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
}

static DWORD WINAPI TerminalThreadProc(LPVOID lpParam)
{
	while (g_conn_ctx.connected && g_conn.connected) {
		if (mactelnet_run(&g_conn, 10) < 0)
			break;
	}
	
	return 0;
}

static BOOL CreateTerminalWindow(void)
{
	if (g_term.hWnd && IsWindow(g_term.hWnd)) {
		SetForegroundWindow(g_term.hWnd);
		return TRUE;
	}
	
	int width = TERM_WIDTH * CHAR_WIDTH + 20;
	int height = TERM_HEIGHT * CHAR_HEIGHT + 40;
	
	g_term.hWnd = CreateWindow(
		"MacConnectTerminal",
		"MacConnect Terminal",
		WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
		CW_USEDEFAULT, CW_USEDEFAULT, width, height,
		NULL, NULL, hInst, NULL);
	
	if (!g_term.hWnd)
		return FALSE;
	
	ShowWindow(g_term.hWnd, SW_SHOW);
	UpdateWindow(g_term.hWnd);
	SetForegroundWindow(g_term.hWnd);
	SetFocus(g_term.hWnd);
	
	return TRUE;
}

/* Connection callbacks */
static void OnDataReceived(const unsigned char *data, int len, void *userdata)
{
	struct conn_context *ctx = (struct conn_context *)userdata;
	
	/* Post message to main thread to update terminal */
	unsigned char *buf = malloc(len);
	if (buf) {
		memcpy(buf, data, len);
		PostMessage(hMainWnd, WM_TERMINAL_DATA, (WPARAM)buf, len);
	}
}

static void OnStatusChanged(int status, const char *msg, void *userdata)
{
	struct conn_context *ctx = (struct conn_context *)userdata;
	
	PostMessage(hMainWnd, WM_CONNECTION_STATUS, (WPARAM)status, (LPARAM)_strdup(msg));
}

/* Handle connect */
static void OnConnect(void)
{
	char mac_str[64];
	unsigned char mac[ETH_ALEN];
	
	GetWindowText(hMacEdit, mac_str, sizeof(mac_str));
	
	if (strlen(mac_str) == 0) {
		MessageBox(hMainWnd, "Please select a device from the list.", "Error", MB_OK | MB_ICONWARNING);
		return;
	}
	
	if (!parse_mac(mac_str, mac)) {
		MessageBox(hMainWnd, "Invalid MAC address.", "Error", MB_OK | MB_ICONERROR);
		return;
	}
	
	/* Create terminal window */
	if (!CreateTerminalWindow()) {
		MessageBox(hMainWnd, "Failed to create terminal window.", "Error", MB_OK | MB_ICONERROR);
		return;
	}
	
	/* Initialize connection */
	mactelnet_conn_init(&g_conn);
	g_conn_ctx.connected = 1;
	g_conn_ctx.hTerminal = g_term.hWnd;
	g_conn_ctx.conn = &g_conn;
	
	/* Set callbacks */
	mactelnet_set_callbacks(OnDataReceived, OnStatusChanged, &g_conn_ctx);
	
	/* Connect */
	SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"Connecting...");
	
	if (mactelnet_connect(&g_conn, mac, 5) < 0) {
		MessageBox(hMainWnd, "Failed to connect to device.\nPlease check the MAC address and try again.", 
			"Connection Failed", MB_OK | MB_ICONERROR);
		if (g_term.hWnd) {
			DestroyWindow(g_term.hWnd);
			g_term.hWnd = NULL;
		}
		g_conn_ctx.connected = 0;
		SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"Connection failed");
		return;
	}
	
	/* Start terminal thread */
	CreateThread(NULL, 0, TerminalThreadProc, NULL, 0, NULL);
	
	SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"Connected");
}

/* Main window procedure */
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_CREATE:
		CreateMainControls(hWnd);
		PostMessage(hWnd, WM_COMMAND, IDC_REFRESH_BTN, 0);
		return 0;
	
	case WM_SIZE:
		ResizeControls(hWnd, LOWORD(lParam), HIWORD(lParam));
		return 0;
	
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_CONNECT_BTN:
		case IDM_CONTEXT_CONNECT:
			OnConnect();
			return 0;
		
		case IDC_REFRESH_BTN:
			RefreshDeviceList();
			return 0;
		
		case IDM_CONTEXT_COPY:
			CopySelectedCellToClipboard();
			return 0;
		}
		return 0;
	
	case WM_NOTIFY:
		if (((LPNMHDR)lParam)->idFrom == IDC_DEVICE_LIST) {
			LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
			if (pnmv->hdr.code == LVN_ITEMCHANGED) {
				if (pnmv->uNewState & LVIS_SELECTED) {
					OnDeviceSelected(pnmv->iItem);
				}
			}
			if (pnmv->hdr.code == NM_DBLCLK) {
				OnConnect();
			}
			if (pnmv->hdr.code == NM_RCLICK) {
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(hDeviceList, &pt);
				
				LVHITTESTINFO hit;
				hit.pt = pt;
				ListView_SubItemHitTest(hDeviceList, &hit);
				
				if (hit.iItem >= 0) {
					ListView_SetItemState(hDeviceList, hit.iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
					OnDeviceSelected(hit.iItem);
					
					g_clicked_column = hit.iSubItem;
					
					HMENU hMenu = CreatePopupMenu();
					AppendMenu(hMenu, MF_STRING, IDM_CONTEXT_COPY, "Copy");
					AppendMenu(hMenu, MF_STRING, IDM_CONTEXT_CONNECT, "Connect");
					
					GetCursorPos(&pt);
					TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
					DestroyMenu(hMenu);
				}
			}
		}
		return 0;
	
	case WM_CHAR:
		/* Forward keyboard input to active terminal connection
		 * (handles case when main window has focus instead of terminal) */
		if (g_conn_ctx.connected && g_conn.connected && g_term.hWnd) {
			unsigned char c = (unsigned char)wParam;
			mactelnet_send(&g_conn, &c, 1);
			/* Debug: show last sent byte in title */
			{
				char title[64];
				snprintf(title, sizeof(title), "MacConnect Terminal [sent 0x%02X]", c);
				SetWindowText(g_term.hWnd, title);
			}
		}
		return 0;
	
	case WM_MNDP_COMPLETE:
		UpdateDeviceListView();
		EnableWindow(hRefreshBtn, TRUE);
		
		char status[128];
		snprintf(status, sizeof(status), "Found %d device(s)", (int)wParam);
		SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)status);
		return 0;
	
	case WM_TERMINAL_DATA: {
		unsigned char *data = (unsigned char *)wParam;
		int len = (int)lParam;
		TerminalProcessData(data, len);
		free(data);
		return 0;
	}
	
	case WM_CONNECTION_STATUS: {
		char *msg = (char *)lParam;
		int status = (int)wParam;
		
		SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)msg);
		
		if (status == MT_STATUS_DISCONNECTED) {
			/* Clean disconnect (e.g. user typed 'exit') — close terminal silently */
			if (g_term.hWnd) {
				DestroyWindow(g_term.hWnd);
				g_term.hWnd = NULL;
			}
			g_conn_ctx.connected = 0;
		} else if (status == MT_STATUS_ERROR || status == MT_STATUS_TIMEOUT) {
			MessageBox(hMainWnd, msg, "Connection Error", MB_OK | MB_ICONERROR);
			if (g_term.hWnd) {
				DestroyWindow(g_term.hWnd);
				g_term.hWnd = NULL;
			}
			g_conn_ctx.connected = 0;
		}
		
		free(msg);
		return 0;
	}
	
	case WM_DESTROY:
		if (hDiscoverThread) {
			CloseHandle(hDiscoverThread);
		}
		mactelnet_disconnect(&g_conn);
		PostQuitMessage(0);
		return 0;
	
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
}
