// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
// ******************************************************************
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  (c) 2002-2003 Aaron Robinson <caustik@caustik.com>
// *  (c) 2016 Patrick van Logchem <pvanlogchem@gmail.com>
// *
// *  All rights reserved
// *
// ******************************************************************

#define LOG_PREFIX CXBXR_MODULE::PS
#include <common\util\CxbxUtil.h>


#include <core\kernel\exports\xboxkrnl.h> // For PsCreateSystemThreadEx, etc.
#include <process.h> // For __beginthreadex(), etc.
#include <float.h> // For _controlfp constants

#include "Logging.h" // For LOG_FUNC()
#include "EmuKrnlLogging.h"
#include "core\kernel\init\CxbxKrnl.h" // For CxbxKrnl_TLS
#include "core\kernel\support\Emu.h" // For EmuLog(LOG_LEVEL::WARNING, )
#include "core\kernel\support\EmuFS.h" // For EmuGenerateFS

// prevent name collisions
namespace NtDll
{
#include "core\kernel\support\EmuNtDll.h"
};

#define PSP_MAX_CREATE_THREAD_NOTIFY 16 /* TODO : Should be 8 */

// PsCreateSystemThread proxy parameters
typedef struct _PCSTProxyParam
{
	IN PVOID  StartRoutine;
	IN PVOID  StartContext;
	IN PVOID  SystemRoutine;
}
PCSTProxyParam;

// Global Variable(s)
extern PVOID g_pfnThreadNotification[PSP_MAX_CREATE_THREAD_NOTIFY] = { NULL };
extern int g_iThreadNotificationCount = 0;

// Separate function for logging, otherwise in PCSTProxy __try wont work (Compiler Error C2712)
void LOG_PCSTProxy
(
	PVOID StartRoutine,
	PVOID StartContext,
	PVOID SystemRoutine
)
{
	LOG_FUNC_BEGIN
		LOG_FUNC_ARG(StartRoutine)
		LOG_FUNC_ARG(StartContext)
		LOG_FUNC_ARG(SystemRoutine)
		LOG_FUNC_END;
}

// Overload which doesn't change affinity
void InitXboxThread()
{
	// initialize FS segment selector
	EmuGenerateFS(CxbxKrnl_TLS, CxbxKrnl_TLSData);

	_controlfp(_PC_53, _MCW_PC); // Set Precision control to 53 bits (verified setting)
	_controlfp(_RC_NEAR, _MCW_RC); // Set Rounding control to near (unsure about this)
}

// PsCreateSystemThread proxy procedure
// Dxbx Note : The signature of PCSTProxy should conform to System.TThreadFunc !
static unsigned int WINAPI PCSTProxy
(
	IN PVOID Parameter
)
{
	CxbxSetThreadName("PsCreateSystemThread Proxy");

	PCSTProxyParam *iPCSTProxyParam = static_cast<PCSTProxyParam*>(Parameter);

	// Copy params to the stack so they can be freed
	PCSTProxyParam params = *iPCSTProxyParam;
	delete iPCSTProxyParam;

	LOG_PCSTProxy(
		params.StartRoutine,
		params.StartContext,
		params.SystemRoutine);


	// Do minimal thread initialization
	InitXboxThread();

	auto routine = (xbox::PKSYSTEM_ROUTINE)params.SystemRoutine;
	// Debugging notice : When the below line shows up with an Exception dialog and a
	// message like: "Exception thrown at 0x00026190 in cxbx.exe: 0xC0000005: Access
	// violation reading location 0xFD001804.", then this is AS-DESIGNED behaviour!
	// (To avoid repetitions, uncheck "Break when this exception type is thrown").
	routine(xbox::PKSTART_ROUTINE(params.StartRoutine), params.StartContext);

	// This will also handle thread notification :
	LOG_TEST_CASE("Thread returned from SystemRoutine");
	xbox::PsTerminateSystemThread(X_STATUS_SUCCESS);

	return 0; // will never be reached
}

// Placeholder system function, instead of XapiThreadStartup
void PspSystemThreadStartup
(
	IN xbox::PKSTART_ROUTINE StartRoutine,
	IN PVOID StartContext
)
{
	// TODO : Call PspUnhandledExceptionInSystemThread(GetExceptionInformation())
	(StartRoutine)(StartContext);

	xbox::PsTerminateSystemThread(X_STATUS_SUCCESS);
}

// ******************************************************************
// * 0x00FE - PsCreateSystemThread()
// ******************************************************************
XBSYSAPI EXPORTNUM(254) xbox::ntstatus_xt NTAPI xbox::PsCreateSystemThread
(
	OUT PHANDLE         ThreadHandle,
	OUT PDWORD          ThreadId OPTIONAL,
	IN  PKSTART_ROUTINE StartRoutine,
	IN  PVOID           StartContext,
	IN  boolean_xt         DebuggerThread
)
{
	LOG_FORWARD("PsCreateSystemThreadEx");

	return PsCreateSystemThreadEx(
		/*OUT*/ThreadHandle,
		/*ThreadExtensionSize=*/0,
		/*KernelStackSize=*/KERNEL_STACK_SIZE,
		/*TlsDataSize=*/0,
		/*OUT*/ThreadId,
		/*StartRoutine=*/StartRoutine,
		StartContext,
		/*CreateSuspended=*/FALSE,
		/*DebuggerThread=*/DebuggerThread,
		/*SystemRoutine=*/PspSystemThreadStartup // instead of XapiThreadStartup
		);
}

// ******************************************************************
// * 0x00FF - PsCreateSystemThreadEx()
// ******************************************************************
// Creates a system thread.
// ThreadHandle: Receives the thread handle
// ThreadExtensionSize: Unsure how this works (everything I've seen uses 0)
// KernelStackSize: Size of the allocation for both stack and TLS data
// TlsDataSize: Size within KernelStackSize to use as TLS data
// ThreadId: Receives the thread ID number
// StartRoutine: Called when the thread is created (by XapiThreadStartup)
// StartContext: Parameter StartRoutine
// CreateSuspended: TRUE to create the thread as a suspended thread
// DebuggerThread: TRUE to allocate the stack from Debug Kit memory
// SystemRoutine: System function (normally XapiThreadStartup) called when the thread is created
//
// New to the XBOX.
XBSYSAPI EXPORTNUM(255) xbox::ntstatus_xt NTAPI xbox::PsCreateSystemThreadEx
(
	OUT PHANDLE         ThreadHandle,
	IN  ulong_xt           ThreadExtensionSize,
	IN  ulong_xt           KernelStackSize,
	IN  ulong_xt           TlsDataSize,
	OUT PDWORD          ThreadId OPTIONAL,
	IN  PKSTART_ROUTINE StartRoutine,
	IN  PVOID           StartContext,
	IN  boolean_xt         CreateSuspended,
	IN  boolean_xt         DebuggerThread,
	IN  PKSYSTEM_ROUTINE SystemRoutine OPTIONAL
)
{
	LOG_FUNC_BEGIN
		LOG_FUNC_ARG_OUT(ThreadHandle)
		LOG_FUNC_ARG(ThreadExtensionSize)
		LOG_FUNC_ARG(KernelStackSize)
		LOG_FUNC_ARG(TlsDataSize)
		LOG_FUNC_ARG_OUT(ThreadId)
		LOG_FUNC_ARG(StartRoutine)
		LOG_FUNC_ARG(StartContext)
		LOG_FUNC_ARG(CreateSuspended)
		LOG_FUNC_ARG(DebuggerThread)
		LOG_FUNC_ARG(SystemRoutine)
		LOG_FUNC_END;

	// TODO : Arguments to use : TlsDataSize, DebuggerThread

	// use default kernel stack size if lesser specified
	if (KernelStackSize < KERNEL_STACK_SIZE)
		KernelStackSize = KERNEL_STACK_SIZE;

	// Double the stack size, this is to account for the overhead HLE patching adds to the stack
	KernelStackSize *= 2;

	// round up to the next page boundary if un-aligned
	KernelStackSize = RoundUp(KernelStackSize, PAGE_SIZE);

    // create thread, using our special proxy technique
    {
        DWORD dwThreadId = 0;

        // PCSTProxy is responsible for cleaning up this pointer
		PCSTProxyParam *iPCSTProxyParam = new PCSTProxyParam;

        iPCSTProxyParam->StartRoutine = (PVOID)StartRoutine;
        iPCSTProxyParam->StartContext = StartContext;
        iPCSTProxyParam->SystemRoutine = (PVOID)SystemRoutine; // NULL, XapiThreadStartup or unknown?

		/*
		// call thread notification routine(s)
		if (g_iThreadNotificationCount != 0)
		{
			for (int i = 0; i < 16; i++)
			{
				// TODO: This is *very* wrong, ps notification routines are NOT the same as XApi notification routines
				// TODO: XAPI notification routines are already handeld by XapiThreadStartup and don't need to be called by us
				// TODO: This type of notification routine is PCREATE_THREAD_NOTIFY_ROUTINE, which takes an ETHREAD pointer as well as Thread ID as input
				// TODO: This is impossible to support currently, as we do not create or register Xbox ETHREAD objects, so we're better to skip it entirely!
				xbox::XTHREAD_NOTIFY_PROC pfnNotificationRoutine = (xbox::XTHREAD_NOTIFY_PROC)g_pfnThreadNotification[i];

				// If the routine doesn't exist, don't execute it!
				if (pfnNotificationRoutine == NULL)
					continue;

				EmuLog(LOG_LEVEL::DEBUG, "Calling pfnNotificationRoutine[%d] (0x%.8X)", g_iThreadNotificationCount, pfnNotificationRoutine);

				pfnNotificationRoutine(TRUE);
			}
		}*/

        HANDLE handle = reinterpret_cast<HANDLE>(_beginthreadex(NULL, KernelStackSize, PCSTProxy, iPCSTProxyParam, CREATE_SUSPENDED, reinterpret_cast<unsigned int*>(&dwThreadId)));
		if (handle == NULL) {
			delete iPCSTProxyParam;
			RETURN(X_STATUS_INSUFFICIENT_RESOURCES);
		}
		*ThreadHandle = handle;
        if (ThreadId != NULL)
            *ThreadId = dwThreadId;

		g_AffinityPolicy->SetAffinityXbox(handle);
		CxbxKrnlRegisterThread(handle);

		// Now that ThreadId is populated and affinity is changed, resume the thread (unless the guest passed CREATE_SUSPENDED)
		if (!CreateSuspended) {
			ResumeThread(handle);
		}

		// Note : DO NOT use iPCSTProxyParam anymore, since ownership is transferred to the proxy (which frees it too)

		// Log ThreadID identical to how GetCurrentThreadID() is rendered :
		EmuLog(LOG_LEVEL::DEBUG, "Created Xbox proxy thread. Handle : 0x%X, ThreadId : [0x%.4X]", handle, dwThreadId);
	}

	RETURN(X_STATUS_SUCCESS);
}

// ******************************************************************
// * 0x0100 - PsQueryStatistics()
// ******************************************************************
XBSYSAPI EXPORTNUM(256) xbox::ntstatus_xt NTAPI xbox::PsQueryStatistics
(
	IN OUT PPS_STATISTICS ProcessStatistics
)
{
	LOG_FUNC_ONE_ARG_OUT(ProcessStatistics);

	NTSTATUS ret = X_STATUS_SUCCESS;

	if (ProcessStatistics->Length == sizeof(PS_STATISTICS)) {
		LOG_INCOMPLETE(); // TODO : Return number of threads and handles that currently exist
		ProcessStatistics->ThreadCount = 1;
		ProcessStatistics->HandleCount = 1;
	} else {
		ret = STATUS_INVALID_PARAMETER;
	}

	RETURN(ret);
}

// ******************************************************************
// * 0x0101 - PsSetCreateThreadNotifyRoutine()
// ******************************************************************
XBSYSAPI EXPORTNUM(257) xbox::ntstatus_xt NTAPI xbox::PsSetCreateThreadNotifyRoutine
(
	IN PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine
)
{
	LOG_FUNC_ONE_ARG(NotifyRoutine);

	NTSTATUS ret = X_STATUS_INSUFFICIENT_RESOURCES;

	// Taken from xbox::EmuXRegisterThreadNotifyRoutine (perhaps that can be removed now) :

	// I honestly don't expect this to happen, but if it does...
	if (g_iThreadNotificationCount >= PSP_MAX_CREATE_THREAD_NOTIFY)
		CxbxrKrnlAbort("Too many thread notification routines installed\n");

	// Find an empty spot in the thread notification array
	for (int i = 0; i < PSP_MAX_CREATE_THREAD_NOTIFY; i++)
	{
		// If we find one, then add it to the array, and break the loop so
		// that we don't accidently register the same routine twice!
		if (g_pfnThreadNotification[i] == NULL)
		{
			g_pfnThreadNotification[i] = (PVOID)NotifyRoutine;
			g_iThreadNotificationCount++;
			ret = X_STATUS_SUCCESS;
			break;
		}
	}

	RETURN(ret);
}

// ******************************************************************
// * 0x0102 - PsTerminateSystemThread()
// ******************************************************************
// Exits the current system thread.  Must be called from a system thread.
//
// Differences from NT: None.
XBSYSAPI EXPORTNUM(258) xbox::void_xt NTAPI xbox::PsTerminateSystemThread
(
	IN ntstatus_xt ExitStatus
)
{
	LOG_FUNC_ONE_ARG(ExitStatus);

	/*
	// call thread notification routine(s)
	if (g_iThreadNotificationCount != 0)
	{
		for (int i = 0; i < 16; i++)
		{
			xbox::XTHREAD_NOTIFY_PROC pfnNotificationRoutine = (xbox::XTHREAD_NOTIFY_PROC)g_pfnThreadNotification[i];

			// If the routine doesn't exist, don't execute it!
			if (pfnNotificationRoutine == NULL)
				continue;

			EmuLog(LOG_LEVEL::DEBUG, "Calling pfnNotificationRoutine[%d] (0x%.8X)", g_iThreadNotificationCount, pfnNotificationRoutine);

			pfnNotificationRoutine(FALSE);
		}
	}*/

	EmuKeFreePcr();
	_endthreadex(ExitStatus);
	// ExitThread(ExitStatus);
	// CxbxKrnlTerminateThread();
}

// ******************************************************************
// * 0x0103 - PsThreadObjectType
// ******************************************************************
XBSYSAPI EXPORTNUM(259) xbox::OBJECT_TYPE VOLATILE xbox::PsThreadObjectType =
{
	xbox::ExAllocatePoolWithTag,
	xbox::ExFreePool,
	NULL,
	NULL,
	NULL,
	(PVOID)offsetof(xbox::KTHREAD, Header),
	'erhT' // = first four characters of "Thread" in reverse
};
