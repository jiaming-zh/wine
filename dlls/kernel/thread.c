/*
 * Win32 threads
 *
 * Copyright 1996 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/types.h>
#ifdef HAVE_SYS_TIMES_H
#include <sys/times.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"
#include "module.h"
#include "thread.h"
#include "wine/winbase16.h"
#include "wine/library.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(thread);
WINE_DECLARE_DEBUG_CHANNEL(relay);


/* TEB of the initial thread */
static TEB initial_teb;
extern struct _PDB current_process;


/***********************************************************************
 *           THREAD_InitTEB
 *
 * Initialization of a newly created TEB.
 */
static BOOL THREAD_InitTEB( TEB *teb )
{
    teb->Tib.ExceptionList = (void *)~0UL;
    teb->Tib.StackBase     = (void *)~0UL;
    teb->Tib.Self          = &teb->Tib;
    teb->tibflags  = TEBF_WIN32;
    teb->exit_code = STILL_ACTIVE;
    teb->request_fd = -1;
    teb->reply_fd   = -1;
    teb->wait_fd[0] = -1;
    teb->wait_fd[1] = -1;
    teb->StaticUnicodeString.MaximumLength = sizeof(teb->StaticUnicodeBuffer);
    teb->StaticUnicodeString.Buffer = (PWSTR)teb->StaticUnicodeBuffer;
    InitializeListHead(&teb->TlsLinks);
    teb->teb_sel = wine_ldt_alloc_fs();
    return (teb->teb_sel != 0);
}


/***********************************************************************
 *           THREAD_InitStack
 *
 * Allocate the stack of a thread.
 */
TEB *THREAD_InitStack( TEB *teb, DWORD stack_size )
{
    DWORD old_prot, total_size;
    DWORD page_size = getpagesize();
    void *base;

    /* Allocate the stack */

    /* if size is smaller than default, get stack size from parent */
    if (stack_size < 1024 * 1024)
    {
        if (teb)
            stack_size = 1024 * 1024;  /* no parent */
        else
            stack_size = ((char *)NtCurrentTeb()->Tib.StackBase
                        - (char *)NtCurrentTeb()->DeallocationStack
                        - SIGNAL_STACK_SIZE - 3 * page_size);
    }

    /* FIXME: some Wine functions use a lot of stack, so we add 64Kb here */
    stack_size += 64 * 1024;

    /* Memory layout in allocated block:
     *
     *   size                 contents
     * 1 page              NOACCESS guard page
     * SIGNAL_STACK_SIZE   signal stack
     * 1 page              NOACCESS guard page
     * 1 page              PAGE_GUARD guard page
     * stack_size          normal stack
     * 1 page              TEB (except for initial thread)
     */

    stack_size = (stack_size + (page_size - 1)) & ~(page_size - 1);
    total_size = stack_size + SIGNAL_STACK_SIZE + 3 * page_size;
    if (!teb) total_size += page_size;

    if (!(base = VirtualAlloc( NULL, total_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE )))
        return NULL;

    if (!teb)
    {
        teb = (TEB *)((char *)base + total_size - page_size);
        if (!THREAD_InitTEB( teb ))
        {
            VirtualFree( base, 0, MEM_RELEASE );
            return NULL;
        }
    }

    teb->DeallocationStack = base;
    teb->signal_stack      = (char *)base + page_size;
    teb->Tib.StackBase     = (char *)base + 3 * page_size + SIGNAL_STACK_SIZE + stack_size;
    teb->Tib.StackLimit    = base;  /* note: limit is lower than base since the stack grows down */

    /* Setup guard pages */

    VirtualProtect( base, 1, PAGE_NOACCESS, &old_prot );
    VirtualProtect( (char *)teb->signal_stack + SIGNAL_STACK_SIZE, 1, PAGE_NOACCESS, &old_prot );
    VirtualProtect( (char *)teb->signal_stack + SIGNAL_STACK_SIZE + page_size, 1,
                    PAGE_EXECUTE_READWRITE | PAGE_GUARD, &old_prot );
    return teb;
}


/***********************************************************************
 *           THREAD_Init
 *
 * Setup the initial thread.
 *
 * NOTES: The first allocated TEB on NT is at 0x7ffde000.
 */
void THREAD_Init(void)
{
    static struct debug_info info;  /* debug info for initial thread */

    if (!initial_teb.Tib.Self)  /* do it only once */
    {
        THREAD_InitTEB( &initial_teb );
        assert( initial_teb.teb_sel );
        info.str_pos = info.strings;
        info.out_pos = info.output;
        initial_teb.debug_info = &info;
        initial_teb.Peb = (PEB *)&current_process;  /* FIXME */
        SYSDEPS_SetCurThread( &initial_teb );
    }
}

DECL_GLOBAL_CONSTRUCTOR(thread_init) { THREAD_Init(); }


/***********************************************************************
 *           THREAD_Start
 *
 * Start execution of a newly created thread. Does not return.
 */
static void THREAD_Start( TEB *teb )
{
    LPTHREAD_START_ROUTINE func = (LPTHREAD_START_ROUTINE)teb->entry_point;
    struct debug_info info;

    info.str_pos = info.strings;
    info.out_pos = info.output;
    teb->debug_info = &info;

    SYSDEPS_SetCurThread( teb );
    SIGNAL_Init();
    wine_server_init_thread();

    if (TRACE_ON(relay))
        DPRINTF("%04lx:Starting thread (entryproc=%p)\n", GetCurrentThreadId(), func );

    __TRY
    {
        MODULE_DllThreadAttach( NULL );
        ExitThread( func( NtCurrentTeb()->entry_arg ) );
    }
    __EXCEPT(UnhandledExceptionFilter)
    {
        TerminateThread( GetCurrentThread(), GetExceptionCode() );
    }
    __ENDTRY
}


/***********************************************************************
 *           CreateThread   (KERNEL32.@)
 */
HANDLE WINAPI CreateThread( SECURITY_ATTRIBUTES *sa, SIZE_T stack,
                            LPTHREAD_START_ROUTINE start, LPVOID param,
                            DWORD flags, LPDWORD id )
{
    HANDLE handle = 0;
    TEB *teb;
    DWORD tid = 0;
    int request_pipe[2];

    if (pipe( request_pipe ) == -1)
    {
        SetLastError( ERROR_TOO_MANY_OPEN_FILES );
        return 0;
    }
    fcntl( request_pipe[1], F_SETFD, 1 ); /* set close on exec flag */
    wine_server_send_fd( request_pipe[0] );

    SERVER_START_REQ( new_thread )
    {
        req->suspend    = ((flags & CREATE_SUSPENDED) != 0);
        req->inherit    = (sa && (sa->nLength>=sizeof(*sa)) && sa->bInheritHandle);
        req->request_fd = request_pipe[0];
        if (!wine_server_call_err( req ))
        {
            handle = reply->handle;
            tid = reply->tid;
        }
        close( request_pipe[0] );
    }
    SERVER_END_REQ;

    if (!handle || !(teb = THREAD_InitStack( NULL, stack )))
    {
        close( request_pipe[1] );
        return 0;
    }

    teb->Peb         = NtCurrentTeb()->Peb;
    teb->ClientId.UniqueThread = (HANDLE)tid;
    teb->request_fd  = request_pipe[1];
    teb->entry_point = start;
    teb->entry_arg   = param;
    teb->htask16     = GetCurrentTask();
    RtlAcquirePebLock();
    InsertHeadList( &NtCurrentTeb()->TlsLinks, &teb->TlsLinks );
    RtlReleasePebLock();

    if (id) *id = tid;
    if (SYSDEPS_SpawnThread( THREAD_Start, teb ) == -1)
    {
        CloseHandle( handle );
        close( request_pipe[1] );
        RtlAcquirePebLock();
        RemoveEntryList( &teb->TlsLinks );
        RtlReleasePebLock();
        wine_ldt_free_fs( teb->teb_sel );
        VirtualFree( teb->DeallocationStack, 0, MEM_RELEASE );
        return 0;
    }
    return handle;
}


/***********************************************************************
 * OpenThread  [KERNEL32.@]   Retrieves a handle to a thread from its thread id
 */
HANDLE WINAPI OpenThread( DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId )
{
    HANDLE ret = 0;
    SERVER_START_REQ( open_thread )
    {
        req->tid     = dwThreadId;
        req->access  = dwDesiredAccess;
        req->inherit = bInheritHandle;
        if (!wine_server_call_err( req )) ret = reply->handle;
    }
    SERVER_END_REQ;
    return ret;
}


/***********************************************************************
 * ExitThread [KERNEL32.@]  Ends a thread
 *
 * RETURNS
 *    None
 */
void WINAPI ExitThread( DWORD code ) /* [in] Exit code for this thread */
{
    BOOL last;
    SERVER_START_REQ( terminate_thread )
    {
        /* send the exit code to the server */
        req->handle    = GetCurrentThread();
        req->exit_code = code;
        wine_server_call( req );
        last = reply->last;
    }
    SERVER_END_REQ;

    if (last)
    {
        LdrShutdownProcess();
        exit( code );
    }
    else
    {
        LdrShutdownThread();
        RtlAcquirePebLock();
        RemoveEntryList( &NtCurrentTeb()->TlsLinks );
        RtlReleasePebLock();
        SYSDEPS_ExitThread( code );
    }
}


/**********************************************************************
 * TerminateThread [KERNEL32.@]  Terminates a thread
 *
 * RETURNS
 *    Success: TRUE
 *    Failure: FALSE
 */
BOOL WINAPI TerminateThread( HANDLE handle,    /* [in] Handle to thread */
                             DWORD exit_code)  /* [in] Exit code for thread */
{
    NTSTATUS status = NtTerminateThread( handle, exit_code );
    if (status) SetLastError( RtlNtStatusToDosError(status) );
    return !status;
}


/***********************************************************************
 *           FreeLibraryAndExitThread (KERNEL32.@)
 */
void WINAPI FreeLibraryAndExitThread(HINSTANCE hLibModule, DWORD dwExitCode)
{
    FreeLibrary(hLibModule);
    ExitThread(dwExitCode);
}


/**********************************************************************
 *		GetExitCodeThread (KERNEL32.@)
 *
 * Gets termination status of thread.
 *
 * RETURNS
 *    Success: TRUE
 *    Failure: FALSE
 */
BOOL WINAPI GetExitCodeThread(
    HANDLE hthread, /* [in]  Handle to thread */
    LPDWORD exitcode) /* [out] Address to receive termination status */
{
    THREAD_BASIC_INFORMATION info;
    NTSTATUS status = NtQueryInformationThread( hthread, ThreadBasicInformation,
                                                &info, sizeof(info), NULL );

    if (status)
    {
        SetLastError( RtlNtStatusToDosError(status) );
        return FALSE;
    }
    if (exitcode) *exitcode = info.ExitStatus;
    return TRUE;
}


/***********************************************************************
 * SetThreadContext [KERNEL32.@]  Sets context of thread.
 *
 * RETURNS
 *    Success: TRUE
 *    Failure: FALSE
 */
BOOL WINAPI SetThreadContext( HANDLE handle,           /* [in]  Handle to thread with context */
                              const CONTEXT *context ) /* [in] Address of context structure */
{
    NTSTATUS status = NtSetContextThread( handle, context );
    if (status) SetLastError( RtlNtStatusToDosError(status) );
    return !status;
}


/***********************************************************************
 * GetThreadContext [KERNEL32.@]  Retrieves context of thread.
 *
 * RETURNS
 *    Success: TRUE
 *    Failure: FALSE
 */
BOOL WINAPI GetThreadContext( HANDLE handle,     /* [in]  Handle to thread with context */
                              CONTEXT *context ) /* [out] Address of context structure */
{
    NTSTATUS status = NtGetContextThread( handle, context );
    if (status) SetLastError( RtlNtStatusToDosError(status) );
    return !status;
}


/**********************************************************************
 * SuspendThread [KERNEL32.@]  Suspends a thread.
 *
 * RETURNS
 *    Success: Previous suspend count
 *    Failure: 0xFFFFFFFF
 */
DWORD WINAPI SuspendThread( HANDLE hthread ) /* [in] Handle to the thread */
{
    DWORD ret;
    NTSTATUS status = NtSuspendThread( hthread, &ret );

    if (status)
    {
        ret = ~0U;
        SetLastError( RtlNtStatusToDosError(status) );
    }
    return ret;
}


/**********************************************************************
 * ResumeThread [KERNEL32.@]  Resumes a thread.
 *
 * Decrements a thread's suspend count.  When count is zero, the
 * execution of the thread is resumed.
 *
 * RETURNS
 *    Success: Previous suspend count
 *    Failure: 0xFFFFFFFF
 *    Already running: 0
 */
DWORD WINAPI ResumeThread( HANDLE hthread ) /* [in] Identifies thread to restart */
{
    DWORD ret;
    NTSTATUS status = NtResumeThread( hthread, &ret );

    if (status)
    {
        ret = ~0U;
        SetLastError( RtlNtStatusToDosError(status) );
    }
    return ret;
}


/**********************************************************************
 * GetThreadPriority [KERNEL32.@]  Returns priority for thread.
 *
 * RETURNS
 *    Success: Thread's priority level.
 *    Failure: THREAD_PRIORITY_ERROR_RETURN
 */
INT WINAPI GetThreadPriority(
    HANDLE hthread) /* [in] Handle to thread */
{
    THREAD_BASIC_INFORMATION info;
    NTSTATUS status = NtQueryInformationThread( hthread, ThreadBasicInformation,
                                                &info, sizeof(info), NULL );

    if (status)
    {
        SetLastError( RtlNtStatusToDosError(status) );
        return THREAD_PRIORITY_ERROR_RETURN;
    }
    return info.Priority;
}


/**********************************************************************
 * SetThreadPriority [KERNEL32.@]  Sets priority for thread.
 *
 * RETURNS
 *    Success: TRUE
 *    Failure: FALSE
 */
BOOL WINAPI SetThreadPriority(
    HANDLE hthread, /* [in] Handle to thread */
    INT priority)   /* [in] Thread priority level */
{
    BOOL ret;
    SERVER_START_REQ( set_thread_info )
    {
        req->handle   = hthread;
        req->priority = priority;
        req->mask     = SET_THREAD_INFO_PRIORITY;
        ret = !wine_server_call_err( req );
    }
    SERVER_END_REQ;
    return ret;
}


/**********************************************************************
 * GetThreadPriorityBoost [KERNEL32.@]  Returns priority boost for thread.
 *
 * Always reports that priority boost is disabled.
 *
 * RETURNS
 *    Success: TRUE.
 *    Failure: FALSE
 */
BOOL WINAPI GetThreadPriorityBoost(
    HANDLE hthread, /* [in] Handle to thread */
    PBOOL pstate)   /* [out] pointer to var that receives the boost state */
{
    if (pstate) *pstate = FALSE;
    return NO_ERROR;
}


/**********************************************************************
 * SetThreadPriorityBoost [KERNEL32.@]  Sets priority boost for thread.
 *
 * Priority boost is not implemented. Thsi function always returns
 * FALSE and sets last error to ERROR_CALL_NOT_IMPLEMENTED
 *
 * RETURNS
 *    Always returns FALSE to indicate a failure
 */
BOOL WINAPI SetThreadPriorityBoost(
    HANDLE hthread, /* [in] Handle to thread */
    BOOL disable)   /* [in] TRUE to disable priority boost */
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}


/**********************************************************************
 *           SetThreadAffinityMask   (KERNEL32.@)
 */
DWORD WINAPI SetThreadAffinityMask( HANDLE hThread, DWORD dwThreadAffinityMask )
{
    DWORD ret;
    SERVER_START_REQ( set_thread_info )
    {
        req->handle   = hThread;
        req->affinity = dwThreadAffinityMask;
        req->mask     = SET_THREAD_INFO_AFFINITY;
        ret = !wine_server_call_err( req );
        /* FIXME: should return previous value */
    }
    SERVER_END_REQ;
    return ret;
}


/**********************************************************************
 * SetThreadIdealProcessor [KERNEL32.@]  Obtains timing information.
 *
 * RETURNS
 *    Success: Value of last call to SetThreadIdealProcessor
 *    Failure: -1
 */
DWORD WINAPI SetThreadIdealProcessor(
    HANDLE hThread,          /* [in] Specifies the thread of interest */
    DWORD dwIdealProcessor)  /* [in] Specifies the new preferred processor */
{
    FIXME("(%p): stub\n",hThread);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return -1L;
}


/* callback for QueueUserAPC */
static void CALLBACK call_user_apc( ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3 )
{
    PAPCFUNC func = (PAPCFUNC)arg1;
    func( arg2 );
}

/***********************************************************************
 *              QueueUserAPC  (KERNEL32.@)
 */
DWORD WINAPI QueueUserAPC( PAPCFUNC func, HANDLE hthread, ULONG_PTR data )
{
    NTSTATUS status = NtQueueApcThread( hthread, call_user_apc, (ULONG_PTR)func, data, 0 );

    if (status) SetLastError( RtlNtStatusToDosError(status) );
    return !status;
}


/**********************************************************************
 * GetThreadTimes [KERNEL32.@]  Obtains timing information.
 *
 * RETURNS
 *    Success: TRUE
 *    Failure: FALSE
 */
BOOL WINAPI GetThreadTimes(
    HANDLE thread,         /* [in]  Specifies the thread of interest */
    LPFILETIME creationtime, /* [out] When the thread was created */
    LPFILETIME exittime,     /* [out] When the thread was destroyed */
    LPFILETIME kerneltime,   /* [out] Time thread spent in kernel mode */
    LPFILETIME usertime)     /* [out] Time thread spent in user mode */
{
    BOOL ret = TRUE;

    if (creationtime || exittime)
    {
        /* We need to do a server call to get the creation time or exit time */
        /* This works on any thread */

        SERVER_START_REQ( get_thread_info )
        {
            req->handle = thread;
            req->tid_in = 0;
            if ((ret = !wine_server_call_err( req )))
            {
                if (creationtime)
                    RtlSecondsSince1970ToTime( reply->creation_time, (LARGE_INTEGER*)creationtime );
                if (exittime)
                    RtlSecondsSince1970ToTime( reply->exit_time, (LARGE_INTEGER*)exittime );
            }
        }
        SERVER_END_REQ;
    }
    if (ret && (kerneltime || usertime))
    {
        /* We call times(2) for kernel time or user time */
        /* We can only (portably) do this for the current thread */
        if (thread == GetCurrentThread())
        {
            ULONGLONG time;
            struct tms time_buf;
            long clocks_per_sec = sysconf(_SC_CLK_TCK);

            times(&time_buf);
            if (kerneltime)
            {
                time = (ULONGLONG)time_buf.tms_stime * 10000000 / clocks_per_sec;
                kerneltime->dwHighDateTime = time >> 32;
                kerneltime->dwLowDateTime = (DWORD)time;
            }
            if (usertime)
            {
                time = (ULONGLONG)time_buf.tms_utime * 10000000 / clocks_per_sec;
                usertime->dwHighDateTime = time >> 32;
                usertime->dwLowDateTime = (DWORD)time;
            }
        }
        else
        {
            if (kerneltime) kerneltime->dwHighDateTime = kerneltime->dwLowDateTime = 0;
            if (usertime) usertime->dwHighDateTime = usertime->dwLowDateTime = 0;
            FIXME("Cannot get kerneltime or usertime of other threads\n");
        }
    }
    return ret;
}


/**********************************************************************
 * VWin32_BoostThreadGroup [KERNEL.535]
 */
VOID WINAPI VWin32_BoostThreadGroup( DWORD threadId, INT boost )
{
    FIXME("(0x%08lx,%d): stub\n", threadId, boost);
}


/**********************************************************************
 * VWin32_BoostThreadStatic [KERNEL.536]
 */
VOID WINAPI VWin32_BoostThreadStatic( DWORD threadId, INT boost )
{
    FIXME("(0x%08lx,%d): stub\n", threadId, boost);
}


/***********************************************************************
 * GetCurrentThread [KERNEL32.@]  Gets pseudohandle for current thread
 *
 * RETURNS
 *    Pseudohandle for the current thread
 */
#undef GetCurrentThread
HANDLE WINAPI GetCurrentThread(void)
{
    return (HANDLE)0xfffffffe;
}
