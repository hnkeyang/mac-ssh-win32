/*
	MacConnect - GUI MAC-Telnet SSH Connect utility for Windows
	Copyright (C) 2024

	Based on MacSSH by Jo-Philipp Wich <jow@openwrt.org>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
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

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define PROGRAM_NAME "MacConnect"
#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "1.0"
#endif

#define IDM_FILE_EXIT 1001
#define IDM_TOOLS_REFRESH 2001
#define IDM_TOOLS_CONNECT 2002
#define IDM_HELP_ABOUT 3001
#define IDM_CONTEXT_COPY 4001
#define IDM_CONTEXT_CONNECT 4002

#define IDC_MAC_ADDRESS 101
#define IDC_LOGIN 102
#define IDC_PASSWORD 103
#define IDC_KEEP_PASSWORD 104
#define IDC_AUTO_RECONNECT 105
#define IDC_CONNECT_BTN 106
#define IDC_REFRESH_BTN 107
#define IDC_DEVICE_LIST 108
#define IDC_IP_ADDRESS 109

#define WM_MNDP_DISCOVERED (WM_USER + 1)
#define WM_MNDP_COMPLETE (WM_USER + 2)

#define MAX_DEVICES 256
#define COL_MAC 0
#define COL_IP 1
#define COL_IPV6_LOCAL 2
#define COL_IDENTITY 3
#define COL_VERSION 4
#define COL_BOARD 5
#define COL_UPTIME 6

/* Device info structure for GUI */
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
static int g_clicked_column = 0; /* Track which column was clicked for copy */

/* Function prototypes */
static BOOL InitApplication(HINSTANCE hInstance);
static BOOL InitInstance(HINSTANCE hInstance, int nCmdShow);
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static void CreateMainControls(HWND hWnd);
static void ResizeControls(HWND hWnd, int width, int height);
static void RefreshDeviceList(void);
static DWORD WINAPI DiscoverThreadProc(LPVOID lpParam);
static void AddDeviceToList(struct mndphost *host);
static void UpdateDeviceListView(void);
static void OnDeviceSelected(int index);
static void OnConnect(void);
static void FormatUptime(unsigned int uptime, char *buf, size_t buflen);
static BOOL LaunchSSHClient(const char *mac);
static void CopySelectedCellToClipboard(void);
static void CopySelectedRowToClipboard(void);

/* WinMain entry point */
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	MSG msg;
	HACCEL hAccelTable;
	WSADATA wsd;

	/* Initialize Winsock */
	WSAStartup(MAKEWORD(2, 2), &wsd);

	/* Initialize Common Controls */
	INITCOMMONCONTROLSEX iccex;
	iccex.dwSize = sizeof(iccex);
	iccex.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
	InitCommonControlsEx(&iccex);

	/* Initialize critical section */
	InitializeCriticalSection(&device_cs);

	/* Initialize application */
	if (!InitApplication(hInstance))
		return FALSE;

	/* Initialize instance */
	if (!InitInstance(hInstance, nCmdShow))
		return FALSE;

	/* Load accelerators */
	hAccelTable = LoadAccelerators(hInstance, NULL);

	/* Message loop */
	while (GetMessage(&msg, NULL, 0, 0)) {
		if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	/* Cleanup */
	DeleteCriticalSection(&device_cs);
	WSACleanup();

	return (int)msg.wParam;
}

/* Initialize application */
static BOOL InitApplication(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;

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
	int y = 10;
	
	/* MAC Address label and edit (read-only for copy) */
	CreateWindow("STATIC", "MAC Address:",
		WS_VISIBLE | WS_CHILD | SS_LEFT,
		10, y, 80, 20, hWnd, NULL, hInst, NULL);
	
	hMacEdit = CreateWindow("EDIT", "",
		WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
		100, y, 200, 20, hWnd, (HMENU)IDC_MAC_ADDRESS, hInst, NULL);
	
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
	lvc.cx = 170;
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

/* Resize controls when window is resized */
static void ResizeControls(HWND hWnd, int width, int height)
{
	if (hDeviceList)
		SetWindowPos(hDeviceList, NULL, 10, 50, width - 40, height - 130, SWP_NOZORDER);
	
	if (hStatusBar)
		SendMessage(hStatusBar, WM_SIZE, 0, 0);
}

/* Format uptime to readable string */
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

/* Add device to internal list */
static void AddDeviceToList(struct mndphost *host)
{
	if (device_count >= MAX_DEVICES)
		return;
	
	EnterCriticalSection(&device_cs);
	
	struct device_info *dev = &devices[device_count];
	memcpy(dev->mac, host->address, ETH_ALEN);
	
	/* Format MAC address */
	snprintf(dev->mac_str, sizeof(dev->mac_str),
		"%02X:%02X:%02X:%02X:%02X:%02X",
		host->address[0], host->address[1], host->address[2],
		host->address[3], host->address[4], host->address[5]);
	
	/* IP address */
	if (host->has_ipv4 && host->ipv4_addr) {
		strncpy(dev->ip_str, inet_ntoa(*host->ipv4_addr), sizeof(dev->ip_str) - 1);
		dev->ip_str[sizeof(dev->ip_str) - 1] = '\0';
	} else {
		strcpy(dev->ip_str, "");
	}
	
	/* IPv6 Local address */
	if (host->has_ipv6_local && host->ipv6_local) {
		inet_ntop(AF_INET6, host->ipv6_local, dev->ipv6_local_str, sizeof(dev->ipv6_local_str));
	} else {
		strcpy(dev->ipv6_local_str, "");
	}
	
	/* IPv6 Global address */
	if (host->has_ipv6_global && host->ipv6_global) {
		inet_ntop(AF_INET6, host->ipv6_global, dev->ipv6_global_str, sizeof(dev->ipv6_global_str));
	} else {
		strcpy(dev->ipv6_global_str, "");
	}
	
	/* Other fields */
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

/* Update list view with current devices */
static void UpdateDeviceListView(void)
{
	char uptime_str[64];
	
	EnterCriticalSection(&device_cs);
	
	/* Clear existing items */
	ListView_DeleteAllItems(hDeviceList);
	
	/* Add devices */
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

/* Discovery thread procedure */
static DWORD WINAPI DiscoverThreadProc(LPVOID lpParam)
{
	struct mndphost *host;
	
	discovering = TRUE;
	
	/* Clear existing devices */
	EnterCriticalSection(&device_cs);
	device_count = 0;
	LeaveCriticalSection(&device_cs);
	
	/* Free old MNDP hosts */
	mndp_free_hosts();
	
	/* Enumerate interfaces */
	net_enum_ifaces();
	
	/* Discover devices */
	int found = mndp_discover(3);
	
	if (found > 0) {
		list_for_each_entry(host, &mndphosts, list) {
			AddDeviceToList(host);
		}
	}
	
	/* Notify main window */
	PostMessage(hMainWnd, WM_MNDP_COMPLETE, (WPARAM)found, 0);
	
	discovering = FALSE;
	return 0;
}

/* Refresh device list */
static void RefreshDeviceList(void)
{
	if (discovering)
		return;
	
	SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"Discovering devices...");
	EnableWindow(hRefreshBtn, FALSE);
	
	/* Start discovery thread */
	if (hDiscoverThread) {
		CloseHandle(hDiscoverThread);
	}
	
	hDiscoverThread = CreateThread(NULL, 0, DiscoverThreadProc, NULL, 0, NULL);
}

/* Handle device selection */
static void OnDeviceSelected(int index)
{
	if (index < 0 || index >= device_count)
		return;
	
	SetWindowText(hMacEdit, devices[index].mac_str);
}

/* Launch SSH client */
static BOOL LaunchSSHClient(const char *mac)
{
	char cmdline[512];
	char exe_path[MAX_PATH];
	
	/* Get path to mactelnet.exe (same directory as MacConnect) */
	GetModuleFileName(NULL, exe_path, MAX_PATH);
	char *last_slash = strrchr(exe_path, '\\');
	if (last_slash)
		*(last_slash + 1) = '\0';
	
	strcat(exe_path, "mactelnet.exe");
	
	/* Build command line - just MAC address, no auth (user can type in console) */
	snprintf(cmdline, sizeof(cmdline),
		"\"%s\" %s -N",
		exe_path, mac);
	
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));
	
	if (!CreateProcess(NULL, cmdline, NULL, NULL, FALSE,
		CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
		MessageBox(hMainWnd, "Failed to launch SSH client.", "Error", MB_OK | MB_ICONERROR);
		return FALSE;
	}
	
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	
	return TRUE;
}

/* Copy selected cell to clipboard */
static void CopySelectedCellToClipboard(void)
{
	int selected = ListView_GetNextItem(hDeviceList, -1, LVNI_SELECTED);
	if (selected < 0 || selected >= device_count)
		return;
	
	/* Get cell text from the clicked column */
	char buffer[256];
	LVITEM lvi;
	lvi.iItem = selected;
	lvi.iSubItem = g_clicked_column;
	lvi.mask = LVIF_TEXT;
	lvi.pszText = buffer;
	lvi.cchTextMax = sizeof(buffer);
	ListView_GetItem(hDeviceList, &lvi);
	
	/* Open clipboard */
	if (!OpenClipboard(hMainWnd))
		return;
	
	EmptyClipboard();
	
	/* Allocate global memory */
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

/* Copy selected row to clipboard (for backward compatibility) */
static void CopySelectedRowToClipboard(void)
{
	int selected = ListView_GetNextItem(hDeviceList, -1, LVNI_SELECTED);
	if (selected < 0 || selected >= device_count)
		return;
	
	struct device_info *dev = &devices[selected];
	char buffer[512];
	char uptime_str[64];
	
	FormatUptime(dev->uptime, uptime_str, sizeof(uptime_str));
	
	/* Format: MAC, IP, IPv6, Identity, Version, Board, Uptime */
	snprintf(buffer, sizeof(buffer),
		"%s\t%s\t%s\t%s\t%s\t%s\t%s",
		dev->mac_str,
		dev->ip_str,
		dev->ipv6_local_str,
		dev->identity,
		dev->version,
		dev->hardware,
		uptime_str);
	
	/* Open clipboard */
	if (!OpenClipboard(hMainWnd))
		return;
	
	EmptyClipboard();
	
	/* Allocate global memory */
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

/* Handle connect button */
static void OnConnect(void)
{
	char mac[64];
	
	GetWindowText(hMacEdit, mac, sizeof(mac));
	
	if (strlen(mac) == 0) {
		MessageBox(hMainWnd, "Please select a device from the list.", "Error", MB_OK | MB_ICONWARNING);
		return;
	}
	
	SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"Connecting...");
	
	if (LaunchSSHClient(mac)) {
		SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"SSH client launched");
	} else {
		SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)"Connection failed");
	}
}

/* Main window procedure */
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_CREATE:
		CreateMainControls(hWnd);
		/* Auto refresh on startup */
		PostMessage(hWnd, WM_COMMAND, IDC_REFRESH_BTN, 0);
		return 0;
	
	case WM_SIZE:
		ResizeControls(hWnd, LOWORD(lParam), HIWORD(lParam));
		return 0;
	
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_CONNECT_BTN:
		case IDM_CONTEXT_CONNECT: /* Connect from context menu */
			OnConnect();
			return 0;
		
		case IDC_REFRESH_BTN:
			RefreshDeviceList();
			return 0;
		
		case IDM_CONTEXT_COPY: /* Copy from context menu */
			CopySelectedCellToClipboard();
			return 0;
		
		case IDM_FILE_EXIT:
			PostQuitMessage(0);
			return 0;
		
		case IDM_HELP_ABOUT:
			MessageBox(hWnd, PROGRAM_NAME " v" PROGRAM_VERSION "\n\n"
				"GUI MAC-Telnet client for Windows\n"
				"Based on MacSSH by Jo-Philipp Wich",
				"About", MB_OK | MB_ICONINFORMATION);
			return 0;
		}
		return 0;
	
	case WM_KEYDOWN:
		/* Handle Ctrl+C keyboard shortcut */
		if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'C') {
			CopySelectedRowToClipboard();
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
			/* Handle right-click context menu */
			if (pnmv->hdr.code == NM_RCLICK) {
				/* Get cursor position and find which subitem was clicked */
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(hDeviceList, &pt);
				
				LVHITTESTINFO hit;
				hit.pt = pt;
				ListView_SubItemHitTest(hDeviceList, &hit);
				
				if (hit.iItem >= 0) {
					/* Select the row that was right-clicked */
					ListView_SetItemState(hDeviceList, hit.iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
					OnDeviceSelected(hit.iItem);
					
					/* Store which column was clicked */
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
	
	case WM_MNDP_COMPLETE:
		UpdateDeviceListView();
		EnableWindow(hRefreshBtn, TRUE);
		
		char status[128];
		snprintf(status, sizeof(status), "Found %d device(s)", (int)wParam);
		SendMessage(hStatusBar, SB_SETTEXT, 0, (LPARAM)status);
		return 0;
	
	case WM_DESTROY:
		if (hDiscoverThread) {
			CloseHandle(hDiscoverThread);
		}
		PostQuitMessage(0);
		return 0;
	
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
}
