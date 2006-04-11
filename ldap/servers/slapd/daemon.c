/** BEGIN COPYRIGHT BLOCK
 * This Program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; version 2 of the License.
 * 
 * This Program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this Program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA 02111-1307 USA.
 * 
 * In addition, as a special exception, Red Hat, Inc. gives You the additional
 * right to link the code of this Program with code not covered under the GNU
 * General Public License ("Non-GPL Code") and to distribute linked combinations
 * including the two, subject to the limitations in this paragraph. Non-GPL Code
 * permitted under this exception must only link to the code of this Program
 * through those well defined interfaces identified in the file named EXCEPTION
 * found in the source code files (the "Approved Interfaces"). The files of
 * Non-GPL Code may instantiate templates or use macros or inline functions from
 * the Approved Interfaces without causing the resulting work to be covered by
 * the GNU General Public License. Only Red Hat, Inc. may make changes or
 * additions to the list of Approved Interfaces. You must obey the GNU General
 * Public License in all respects for all of the Program code and other code used
 * in conjunction with the Program except the Non-GPL Code covered by this
 * exception. If you modify this file, you may extend this exception to your
 * version of the file, but you are not obligated to do so. If you do not wish to
 * provide this exception without modification, you must delete this exception
 * statement from your version and license this file solely under the GPL without
 * exception. 
 * 
 * 
 * Copyright (C) 2001 Sun Microsystems, Inc. Used by permission.
 * Copyright (C) 2005 Red Hat, Inc.
 * All rights reserved.
 * END COPYRIGHT BLOCK **/
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h> /* for getpid */
#include "proto-ntutil.h"
#include "ntslapdmessages.h"
#else
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <pthread.h>
#endif
#include <time.h>
#include <signal.h>
#if defined(IRIX6_2) || defined(IRIX6_3)
#include <sys/param.h>
#endif
#if defined(_AIX)
#include <sys/select.h>
#include <sys/param.h>
#endif
#include <fcntl.h>
#define TCPLEN_T	int
#if !defined( _WIN32 )
#ifdef NEED_FILIO
#include <sys/filio.h>
#else /* NEED_FILIO */
#include <sys/ioctl.h>
#endif /* NEED_FILIO */
#endif /* !defined( _WIN32 ) */
/* for some reason, linux tty stuff defines CTIME */
#ifdef LINUX
#undef CTIME
#endif
#include "slap.h"
#include "slapi-plugin.h"

#include "snmp_collator.h"
#include <private/pprio.h>

#if defined( NET_SSL )
#include <ssl.h>
#endif /* defined(NET_SSL) */

#include "fe.h"

/*
 * Define the backlog number for use in listen() call.
 * We use the same definition as in ldapserver/include/base/systems.h
 */
#ifndef DAEMON_LISTEN_SIZE
#define DAEMON_LISTEN_SIZE 128
#endif

#if defined (LDAP_IOCP)
#define	SLAPD_WAKEUP_TIMER	250
#else
#define	SLAPD_WAKEUP_TIMER	250
#endif

int slapd_wakeup_timer = SLAPD_WAKEUP_TIMER; /* time in ms to wakeup */
#ifdef notdef /* GGOODREPL */
/* 
 * time in secs to do housekeeping: 
 * this must be greater than slapd_wakeup_timer 
 */
short	slapd_housekeeping_timer = 10;
#endif /* notdef GGOODREPL */

/* Do we support timeout on socket send() ? */
int have_send_timeouts = 0;

PRFileDesc*		signalpipe[2];
static int writesignalpipe = SLAPD_INVALID_SOCKET;
static int readsignalpipe = SLAPD_INVALID_SOCKET;

#define FDS_SIGNAL_PIPE 0
#define FDS_N_TCPS      1
#define FDS_S_TCPS      2

static int get_configured_connection_table_size();
#ifdef RESOLVER_NEEDS_LOW_FILE_DESCRIPTORS
static void get_loopback_by_addr( void );
#endif

#ifdef XP_WIN32
static int createlistensocket(unsigned short port, const PRNetAddr *listenaddr);
#endif
static PRFileDesc *createprlistensocket(unsigned short port,
	const PRNetAddr *listenaddr, int secure);
static const char *netaddr2string(const PRNetAddr *addr, char *addrbuf,
	size_t addrbuflen);
static void	set_shutdown (int);
static void setup_pr_read_pds(Connection_Table *ct, PRFileDesc *n_tcps, PRFileDesc *s_tcps, PRIntn *num_to_read);

#ifdef HPUX10
static void* catch_signals();
#endif

#if defined( _WIN32 )
HANDLE  hServDoneEvent = NULL;
#endif

static int createsignalpipe( void );


#if defined( _WIN32 )
/* Set an event to hook the NT Service termination */
void *slapd_service_exit_wait()
{
#if defined( PURIFYING )

#include <sys/types.h> 
#include <sys/stat.h>

	char module[_MAX_FNAME];
	char exit_file_name[_MAX_FNAME];
	char drive[_MAX_DRIVE];
	char dir[_MAX_DIR];
	char fname[_MAX_FNAME];
	char ext[_MAX_EXT];
	struct stat statbuf;

	memset( module, 0, sizeof( module ) );
	memset( exit_file_name, 0, sizeof( exit_file_name ) );

	GetModuleFileName(GetModuleHandle( NULL ), module, sizeof( module ) );

	_splitpath( module, drive, dir, fname, ext );

	PR_snprintf( exit_file_name, sizeof(exit_file_name), "%s%s%s", drive, dir, "exitnow.txt" );

    LDAPDebug( LDAP_DEBUG_ANY, "PURIFYING - Create %s to terminate the process.\n", exit_file_name, 0, 0 );

	while ( TRUE )
	{
		if( stat( exit_file_name, &statbuf ) < 0)
		{
			Sleep( 5000 );  /* 5 Seconds */
			continue;
		}
	    LDAPDebug( LDAP_DEBUG_ANY, "slapd shutting down immediately, "
		"\"%s\" exists - don't forget to delete it\n", exit_file_name, 0, 0 );
		g_set_shutdown( SLAPI_SHUTDOWN_SIGNAL );
		return NULL;
	}

#else /*  PURIFYING  */

	DWORD dwWait;
	char szDoneEvent[256];

	PR_snprintf(szDoneEvent, sizeof(szDoneEvent), "NS_%s", pszServerName);

	hServDoneEvent = CreateEvent( NULL,			// default security attributes (LocalSystem)
								  TRUE,			// manual reset event
								  FALSE,		// not-signalled
								  szDoneEvent );// named after the service itself.

    /*  Wait indefinitely until hServDoneEvent is signaled. */
    dwWait = WaitForSingleObject( hServDoneEvent,  // event object
								  INFINITE );      // wait indefinitely

	/* The termination event has been signalled, log this occurrence, and signal to exit. */
	ReportSlapdEvent( EVENTLOG_INFORMATION_TYPE, MSG_SERVER_SHUTDOWN_STARTING, 0, NULL );

	g_set_shutdown( SLAPI_SHUTDOWN_SIGNAL );
	return NULL;
#endif /* PURIFYING  */
}
#endif /* _WIN32 */

static char *
get_pid_file()
{
    return(pid_file);
}

static int daemon_configure_send_timeout(int s,size_t timeout /* Miliseconds*/)
{
	/* Currently this function is only good for NT, and expects the s argument to be a SOCKET */
#if defined(_WIN32)
	return setsockopt(
		s,
		SOL_SOCKET,
		SO_SNDTIMEO,
		(char*) &timeout,
		sizeof(timeout)
		);
#else
	return 0;
#endif
}

#if defined (_WIN32)
/* This function is a workaround for accept problem on NT. 
   Accept call fires on NT during syn scan even though the connection is not
   open. This causes a resource leak. For more details, see bug 391414.
   Experimentally, we determined that, in case of syn scan, the local     
   address is set to 0. This in undocumented and my change in the future
    
   The function returns 0 if this is normal connection
                        1 if this is syn_scan connection
                       -1 in case of any other error
 */
static int 
syn_scan (int sock)
{
    int rc;
    struct sockaddr_in addr;
    int size = sizeof (addr);

    if (sock == SLAPD_INVALID_SOCKET)
        return -1;

    rc = getsockname (sock, (struct sockaddr*)&addr,  &size); 
    if (rc != 0)
        return -1;
    else if (addr.sin_addr.s_addr == 0)
        return 1;
    else
        return 0;
}

#endif

static int
accept_and_configure(int s, PRFileDesc *pr_acceptfd, PRNetAddr *pr_netaddr, 
	int addrlen, int secure, PRFileDesc **pr_clonefd)
{
	int ns = 0;

	PRIntervalTime pr_timeout = PR_MillisecondsToInterval(slapd_wakeup_timer);

#if !defined( XP_WIN32 )
	(*pr_clonefd) = PR_Accept(pr_acceptfd, pr_netaddr, pr_timeout);
	if( !(*pr_clonefd) ) {
		PRErrorCode prerr = PR_GetError();
		LDAPDebug( LDAP_DEBUG_ANY, "PR_Accept() failed, "
				SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
				prerr, slapd_pr_strerror(prerr), 0 );
		return(SLAPD_INVALID_SOCKET);
	}

	ns = configure_pr_socket( pr_clonefd, secure );

#else
	if( secure ) {
		(*pr_clonefd) = PR_Accept(pr_acceptfd, pr_netaddr, pr_timeout);
		if( !(*pr_clonefd) ) {
			PRErrorCode prerr = PR_GetError();
			LDAPDebug( LDAP_DEBUG_ANY, "PR_Accept() failed, "
				SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n", 
			    prerr, slapd_pr_strerror(prerr), 0 );

			/* Bug 613324: Call PR_NT_CancelIo if an error occurs */
			if( (prerr == PR_IO_TIMEOUT_ERROR ) ||
			    (prerr == PR_PENDING_INTERRUPT_ERROR) ) {
				if( (PR_NT_CancelIo( pr_acceptfd )) != PR_SUCCESS) {
					prerr = PR_GetError();
					LDAPDebug( LDAP_DEBUG_ANY, 
						"PR_NT_CancelIo() failed, "
						SLAPI_COMPONENT_NAME_NSPR 
						" error %d (%s)\n",
						prerr, slapd_pr_strerror(prerr), 0 );
				}
			}
			return(SLAPD_INVALID_SOCKET);
		}

		ns = configure_pr_socket( pr_clonefd, secure );

	} else { 
	        struct sockaddr *addr;

			addr = (struct sockaddr *) slapi_ch_malloc( sizeof(struct sockaddr) );
		ns = accept (s, addr, (TCPLEN_T *)&addrlen);

		if (ns == SLAPD_INVALID_SOCKET) {
			int oserr = errno;
			
			LDAPDebug( LDAP_DEBUG_ANY,
				   "accept(%d) failed errno %d (%s)\n",
				   s, oserr, slapd_system_strerror(oserr));
		}

        else if (syn_scan (ns))
        {
            /* this is a work around for accept problem with SYN scan on NT.
            See bug 391414 for more details */
            LDAPDebug(LDAP_DEBUG_ANY, "syn-scan request is received - ignored\n", 0, 0, 0);			    
            closesocket (ns);
            ns = SLAPD_INVALID_SOCKET;
        }

		if ( PR_SetNetAddr(PR_IpAddrNull, PR_AF_INET6, ((struct sockaddr_in *)addr)->sin_port, pr_netaddr)
		     != PR_SUCCESS ) {
			int oserr = PR_GetError();
			LDAPDebug( LDAP_DEBUG_ANY, "PR_SetNetAddr() failed, "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					oserr, slapd_pr_strerror(oserr), 0 );
		} else {
		        PR_ConvertIPv4AddrToIPv6(((struct sockaddr_in *)addr)->sin_addr.s_addr, &(pr_netaddr->ipv6.ip));
		}

		(*pr_clonefd) = NULL;

		slapi_ch_free( (void **)&addr );
		configure_ns_socket( &ns );
	}
#endif
	
	return ns;
}

/* 
 * This is the shiny new re-born daemon function, without all the hair
 */
#ifdef _WIN32
static void setup_read_fds(Connection_Table *ct, fd_set *readfds, int n_tcps, int s_tcps );
static void handle_read_ready(Connection_Table *ct, fd_set *readfds);
static void set_timeval_ms(struct timeval *t, int ms);
#endif
/* GGOODREPL static void handle_timeout( void ); */
static void handle_pr_read_ready(Connection_Table *ct, PRIntn num_poll);
static int handle_new_connection(Connection_Table *ct, int tcps, PRFileDesc *pr_acceptfd, int secure );
#ifdef _WIN32
static void unfurl_banners(Connection_Table *ct,daemon_ports_t *ports, int n_tcps, PRFileDesc *s_tcps);
#else
static void unfurl_banners(Connection_Table *ct,daemon_ports_t *ports, PRFileDesc *n_tcps, PRFileDesc *s_tcps);
#endif
static int write_pid_file();
static int init_shutdown_detect();
#ifdef _WIN32
static int clear_signal(fd_set *readfdset);
#else
static int clear_signal(struct POLL_STRUCT *fds);
#endif

/* Globals which are used to store the sockets between
 * calls to daemon_pre_setuid_init() and the daemon thread
 * creation. */

int daemon_pre_setuid_init(daemon_ports_t *ports)
{
	int	rc = 0;

	if (0 != ports->n_port) {
#if defined( XP_WIN32 )
		ports->n_socket = createlistensocket((unsigned short)ports->n_port,
											 &ports->n_listenaddr);
#else
		ports->n_socket = createprlistensocket(ports->n_port,
											 &ports->n_listenaddr, 0);
#endif
	}

	if ( config_get_security() && (0 != ports->s_port) ) {
		ports->s_socket = createprlistensocket((unsigned short)ports->s_port,
		    &ports->s_listenaddr, 1);
#ifdef XP_WIN32
		ports->s_socket_native = PR_FileDesc2NativeHandle(ports->s_socket);
#endif
	} else {
	    ports->s_socket = SLAPD_INVALID_SOCKET;
#ifdef XP_WIN32
	    ports->s_socket_native = SLAPD_INVALID_SOCKET;
#endif
	}

	return( rc );
}


/* Decide whether we're running on a platform which supports send with timeouts */
static void detect_timeout_support()
{
	/* Currently we know that NT4.0 or higher DOES support timeouts */
#if defined _WIN32
	/* Get the OS revision */
	OSVERSIONINFO ver;
	ver.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&ver);
	if (ver.dwPlatformId == VER_PLATFORM_WIN32_NT && ver.dwMajorVersion >= 4) {
		have_send_timeouts = 1;
	}
#else
	/* Some UNIXen do, but for now I don't feel confident which , and whether timeouts really work there */
#endif
}


/*
 * The time_shutdown static variable is used to signal the time thread
 * to shutdown.  We used to shut down the time thread when g_get_shutdown()
 * returned a non-zero value, but that caused the clock to stop, so to speak,
 * and all error log entries to have the same timestamp once the shutdown
 * process began.
 */
static int time_shutdown = 0;

void * 
time_thread(void *nothing)
{
    PRIntervalTime    interval;

    interval = PR_SecondsToInterval(1);

    while(!time_shutdown) {
        poll_current_time();
        csngen_update_time ();
        DS_Sleep(interval);
    }

    /*NOTREACHED*/
    return(NULL);
}


void slapd_daemon( daemon_ports_t *ports )
{
	/* We are passed a pair of ports---one for regular connections, the
	 * other for SSL connections.
	 */
	/* Previously there was a ton of code #defined on NET_SSL. 
	 * This looked horrible, so now I'm doing it this way:
	 * If you want me to do SSL, pass me something in the ssl port number.
	 * If you don't, pass me zero.
	 */

#if defined( XP_WIN32 )
	int n_tcps = 0;
	int s_tcps_native = 0;
#else
	PRFileDesc *n_tcps = NULL; 
	PRFileDesc *tcps = 0;
#endif
	PRFileDesc *s_tcps = NULL; 
	PRIntn num_poll = 0;
	PRIntervalTime pr_timeout = PR_MillisecondsToInterval(slapd_wakeup_timer);	
	PRThread *time_thread_p;
	int threads;
	int in_referral_mode = config_check_referral_mode();

	int connection_table_size = get_configured_connection_table_size();
	the_connection_table= connection_table_new(connection_table_size);

#ifdef RESOLVER_NEEDS_LOW_FILE_DESCRIPTORS
	/*
	 * Some DNS resolver implementations, such as the one built into
	 * Solaris <= 8, need to use one or more low numbered file
	 * descriptors internally (probably because they use a deficient
	 * implementation of stdio).  So we make a call now that uses the
	 * resolver so it has an opportunity to grab whatever low file
	 * descriptors it needs (before we use up all of the low numbered
	 * ones for incoming client connections and so on).
	 */
	get_loopback_by_addr();
#endif

	/* Retrieve the sockets from their hiding place */
	n_tcps = ports->n_socket;
	s_tcps = ports->s_socket;
#ifdef XP_WIN32
	s_tcps_native = ports->s_socket_native;
#endif
	
	createsignalpipe();

	init_shutdown_detect();

#if defined( XP_WIN32 )
	if ( (n_tcps == SLAPD_INVALID_SOCKET) && 
#else
	if ( (n_tcps == NULL) && 
#endif
	    (s_tcps == NULL) ) {	/* nothing to do */
	    LDAPDebug( LDAP_DEBUG_ANY,
		"no port to listen on\n", 0, 0, 0 );
	    exit( 1 );
	}

	unfurl_banners(the_connection_table,ports,n_tcps,s_tcps);
	init_op_threads ();
	detect_timeout_support();

    /* Start the time thread */
    time_thread_p = PR_CreateThread(PR_SYSTEM_THREAD,
		(VFP) (void *) time_thread, NULL,
        PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, 
        PR_JOINABLE_THREAD, 
        SLAPD_DEFAULT_THREAD_STACKSIZE);
    if ( NULL == time_thread_p ) {
		PRErrorCode errorCode = PR_GetError();
		LDAPDebug(LDAP_DEBUG_ANY, "Unable to create time thread - Shutting Down ("
				SLAPI_COMPONENT_NAME_NSPR " error %d - %s)\n",
				errorCode, slapd_pr_strerror(errorCode), 0);
		g_set_shutdown( SLAPI_SHUTDOWN_EXIT );
	}

	/* We are now ready to accept imcoming connections */
#if defined( XP_WIN32 )
	if ( n_tcps != SLAPD_INVALID_SOCKET
				&& listen( n_tcps, DAEMON_LISTEN_SIZE ) == -1 ) {
		int		oserr = errno;
		char	addrbuf[ 256 ];

		slapi_log_error(SLAPI_LOG_FATAL, "slapd_daemon",
			"listen() on %s port %d failed: OS error %d (%s)\n",
			netaddr2string(&ports->n_listenaddr, addrbuf, sizeof(addrbuf)),
			ports->n_port, oserr, slapd_system_strerror( oserr ) );
		g_set_shutdown( SLAPI_SHUTDOWN_EXIT );
	}
#else
	if ( n_tcps != NULL
				&& PR_Listen( n_tcps, DAEMON_LISTEN_SIZE ) == PR_FAILURE) {
		PRErrorCode prerr = PR_GetError();
		char		addrbuf[ 256 ];

		slapi_log_error(SLAPI_LOG_FATAL, "slapd_daemon",
				"PR_Listen() on %s port %d failed: %s error %d (%s)\n",
				netaddr2string(&ports->n_listenaddr, addrbuf, sizeof(addrbuf)),
				ports->n_port, SLAPI_COMPONENT_NAME_NSPR, prerr,
				slapd_pr_strerror( prerr ));
		g_set_shutdown( SLAPI_SHUTDOWN_EXIT );
	}
#endif

	if ( s_tcps != NULL
				&& PR_Listen( s_tcps, DAEMON_LISTEN_SIZE ) == PR_FAILURE ) {
		PRErrorCode prerr = PR_GetError();
		char		addrbuf[ 256 ];

		slapi_log_error(SLAPI_LOG_FATAL, "slapd_daemon",
				"PR_Listen() on %s port %d failed: %s error %d (%s)\n",
				netaddr2string(&ports->s_listenaddr, addrbuf, sizeof(addrbuf)),
				ports->s_port, SLAPI_COMPONENT_NAME_NSPR, prerr,
				slapd_pr_strerror( prerr ));
		g_set_shutdown( SLAPI_SHUTDOWN_EXIT );
	}

	/* Now we write the pid file, indicating that the server is finally and listening for connections */
	write_pid_file();

	/* The meat of the operation is in a loop on a call to select */
	while(!g_get_shutdown())
	{
#ifdef _WIN32
		fd_set			readfds;
		struct timeval	wakeup_timer;
		int			oserr;
#endif
		int select_return = 0;
		int secure = 0; /* is a new connection an SSL one ? */
#ifndef _WIN32
		PRErrorCode prerr;
#endif

#ifdef _WIN32
		set_timeval_ms(&wakeup_timer, slapd_wakeup_timer);
		setup_read_fds(the_connection_table,&readfds,n_tcps, s_tcps_native);
		/* This select needs to timeout to give the server a chance to test for shutdown */
		select_return = select(connection_table_size, &readfds, NULL, 0, &wakeup_timer);
#else
		setup_pr_read_pds(the_connection_table,n_tcps,s_tcps,&num_poll);
		select_return = POLL_FN(the_connection_table->fd, num_poll, pr_timeout);
#endif
		switch (select_return) {
		case 0: /* Timeout */
			/* GGOODREPL handle_timeout(); */
			break;
		case -1: /* Error */
#ifdef _WIN32
			oserr = errno;

			LDAPDebug( LDAP_DEBUG_TRACE,
			    "select failed errno %d (%s)\n", oserr,
			    slapd_system_strerror(oserr), 0 );
#else
			prerr = PR_GetError();
			LDAPDebug( LDAP_DEBUG_TRACE, "PR_Poll() failed, "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					prerr, slapd_system_strerror(prerr), 0 );
#endif
			break;
		default: /* either a new connection or some new data ready */
			/* Figure out if we are dealing with one of the listen sockets */
#ifdef _WIN32
			/* If so, then handle a new connection */
			if ( n_tcps != SLAPD_INVALID_SOCKET && FD_ISSET( n_tcps,&readfds ) ) {
				handle_new_connection(the_connection_table,n_tcps,NULL,0);
			} 
			/* If so, then handle a new connection */
			if ( s_tcps != SLAPD_INVALID_SOCKET && FD_ISSET( s_tcps_native,&readfds ) ) {
				handle_new_connection(the_connection_table,SLAPD_INVALID_SOCKET,s_tcps,1);
			} 
			/* handle new data ready */
			handle_read_ready(the_connection_table,&readfds);
			clear_signal(&readfds);
#else
			tcps = NULL;
            /* info for n_tcps is always in fd[FDS_N_TCPS] and info for s_tcps is always
             * in fd[FDS_S_TCPS] */
			if( n_tcps != NULL && 
				the_connection_table->fd[FDS_N_TCPS].out_flags & SLAPD_POLL_FLAGS ) {
				tcps = n_tcps;
			} else if ( s_tcps != NULL && 
				the_connection_table->fd[FDS_S_TCPS].out_flags & SLAPD_POLL_FLAGS ) {
				tcps = s_tcps;
				secure = 1;
			}
			/* If so, then handle a new connection */
			if ( tcps != NULL ) {
				handle_new_connection(the_connection_table,SLAPD_INVALID_SOCKET,tcps,secure);
			}
			/* handle new data ready */
			handle_pr_read_ready(the_connection_table, connection_table_size);
			clear_signal(the_connection_table->fd);
#endif
			break;
		}

	}
	/* We get here when the server is shutting down */
	/* Do what we have to do before death */

	connection_table_abandon_all_operations(the_connection_table);	/* abandon all operations in progress */
	
	if ( ! in_referral_mode ) {
		ps_stop_psearch_system(); /* stop any persistent searches */
	}

#ifdef _WIN32
	if ( n_tcps != SLAPD_INVALID_SOCKET ) {
		closesocket( n_tcps );
#else
	if ( n_tcps != NULL ) {
		PR_Close( n_tcps );
#endif
	}
	if ( s_tcps != NULL ) {
 		PR_Close( s_tcps );
	}

	/* Might compete with housecleaning thread, but so far so good */
	be_flushall();
	op_thread_cleanup();
	housekeeping_stop(); /* Run this after op_thread_cleanup() logged sth */

#ifndef _WIN32
	if ( active_threads > 0 ) {
		LDAPDebug( LDAP_DEBUG_ANY,
			"slapd shutting down - waiting for %d thread%s to terminate\n",
			active_threads, ( active_threads > 1 ) ? "s" : "", 0 );
	}
#endif

	threads = active_threads;
	while ( active_threads > 0 ) {
		PRPollDesc xpd;
		char x;
		int spe = 0;

		/* try to read from the signal pipe, in case threads are
		 * blocked on it. */
		xpd.fd = signalpipe[0];
		xpd.in_flags = PR_POLL_READ;
		xpd.out_flags = 0;
		spe = PR_Poll(&xpd, 1, PR_INTERVAL_NO_WAIT);
		if (spe > 0) {
		    spe = PR_Read(signalpipe[0], &x, 1);
		    if (spe < 0) {
		        PRErrorCode prerr = PR_GetError();
				LDAPDebug( LDAP_DEBUG_ANY, "listener could not clear signal pipe, " 
						SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
						prerr, slapd_system_strerror(prerr), 0 );
			break;
		    }
		} else if (spe == -1) {
		    PRErrorCode prerr = PR_GetError();
		    LDAPDebug( LDAP_DEBUG_ANY, "PR_Poll() failed, "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					prerr, slapd_system_strerror(prerr), 0 );
		    break;
		} else {
		    /* no data */
		}
		DS_Sleep(PR_INTERVAL_NO_WAIT);
		if ( threads != active_threads )  {
			LDAPDebug( LDAP_DEBUG_TRACE,
					"slapd shutting down - waiting for %d threads to terminate\n",
					active_threads, 0, 0 );
			threads = active_threads;
		}
	}

	LDAPDebug( LDAP_DEBUG_ANY,
	    "slapd shutting down - closing down internal subsystems and plugins\n",
	    0, 0, 0 );

    log_access_flush();

	/* let backends do whatever cleanup they need to do */
	LDAPDebug( LDAP_DEBUG_TRACE,"slapd shutting down - waiting for backends to close down\n", 0, 0,0 );

	eq_stop();
	if ( ! in_referral_mode ) {
		task_shutdown();
		uniqueIDGenCleanup ();   
	}

	plugin_closeall( 1 /* Close Backends */, 1 /* Close Gloabls */); 

	if ( ! in_referral_mode ) {
		/* Close SNMP collator after the plugins closed... 
		 * Replication plugin still performs internal ops that
		 * may try to increment snmp stats.
		 * Fix for defect 523780
		 */
		snmp_collator_stop();
		mapping_tree_free ();
	}

	be_cleanupall (); 
    LDAPDebug( LDAP_DEBUG_TRACE, "slapd shutting down - backends closed down\n",
			0, 0, 0 );
	referrals_free();

	connection_table_free(the_connection_table);
	the_connection_table= NULL;

	/* tell the time thread to shutdown and then wait for it */
	time_shutdown = 1;
	PR_JoinThread( time_thread_p );

#ifdef _WIN32
	WSACleanup();
#endif
}

int signal_listner()
{
	/* Replaces previous macro---called to bump the thread out of select */
#if defined( _WIN32 )
	if ( PR_Write( signalpipe[1], "", 1) != 1 ) {
			/* this now means that the pipe is full
			 * this is not a problem just go-on
			 */
			LDAPDebug( LDAP_DEBUG_CONNS,
				"listener could not write to signal pipe %d\n",
				errno, 0, 0 );
	}
	
#else
	if ( write( writesignalpipe, "", 1) != 1 ) {
			/* this now means that the pipe is full
			 * this is not a problem just go-on
			 */
			LDAPDebug( LDAP_DEBUG_CONNS,
				"listener could not write to signal pipe %d\n",
				errno, 0, 0 );
	}
#endif
	return( 0 );
}

#ifdef _WIN32
static int clear_signal(fd_set *readfdset)
#else
static int clear_signal(struct POLL_STRUCT *fds)
#endif
{
#ifdef _WIN32
	if ( FD_ISSET(readsignalpipe, readfdset)) {
#else
	if ( fds[FDS_SIGNAL_PIPE].out_flags & SLAPD_POLL_FLAGS ) {
#endif
		char	buf[200];

		LDAPDebug( LDAP_DEBUG_CONNS,
			"listener got signaled\n",
			0, 0, 0 );
#ifdef _WIN32
		if ( PR_Read( signalpipe[0], buf, 20 ) < 1 ) {
#else
		if ( read( readsignalpipe, buf, 200 ) < 1 ) {
#endif
			LDAPDebug( LDAP_DEBUG_ANY,
				"listener could not clear signal pipe\n",
				0, 0, 0 );
		}
	} 
	return 0;
}

#ifdef _WIN32
static void set_timeval_ms(struct timeval *t, int ms)
{
	t->tv_sec = ms/1000;
	t->tv_usec = (ms % 1000)*1000;
}
#endif

#ifdef _WIN32
static void setup_read_fds(Connection_Table *ct, fd_set *readfds, int n_tcps, int s_tcps)
{
	Connection *c= NULL;
	Connection *next= NULL;
	int accept_new_connections;
	static int last_accept_new_connections = -1;
   	slapdFrontendConfig_t *slapdFrontendConfig = getFrontendConfig();

	LBER_SOCKET socketdesc = SLAPD_INVALID_SOCKET;

	FD_ZERO( readfds );

	accept_new_connections = ((ct->size - g_get_current_conn_count())
	    > slapdFrontendConfig->reservedescriptors);
	if ( ! accept_new_connections ) {
		if ( last_accept_new_connections ) {
			LDAPDebug( LDAP_DEBUG_ANY, "Not listening for new "
			    "connections - too many fds open\n", 0, 0, 0 );
		}
	} else {
		if ( ! last_accept_new_connections &&
		    last_accept_new_connections != -1 ) {
			LDAPDebug( LDAP_DEBUG_ANY, "Listening for new "
			    "connections again\n", 0, 0, 0 );
		}
	}
	last_accept_new_connections = accept_new_connections;
	if (n_tcps != SLAPD_INVALID_SOCKET && accept_new_connections) {
		FD_SET( n_tcps, readfds );
		LDAPDebug( LDAP_DEBUG_HOUSE,
			"listening for connections on %d\n", n_tcps, 0, 0 );
	}
	if (s_tcps != SLAPD_INVALID_SOCKET && accept_new_connections) {
		FD_SET( s_tcps, readfds );
		LDAPDebug( LDAP_DEBUG_HOUSE,
			"listening for connections on %d\n", s_tcps, 0, 0 );
	}

	if ((s_tcps != SLAPD_INVALID_SOCKET)
		 && (readsignalpipe != SLAPD_INVALID_SOCKET)) {
		FD_SET( readsignalpipe, readfds );
	}

	/* Walk down the list of active connections to find 
	 * out which connections we should poll over.  If a connection
	 * is no longer in use, we should remove it from the linked 
	 * list. */
	c= connection_table_get_first_active_connection (ct);
	while (c)
    {
	    next = connection_table_get_next_active_connection (ct, c);
	    if ( c->c_mutex == NULL )
	    {
		    connection_table_move_connection_out_of_active_list(ct,c);
	    }
	    else
	    {
	        PR_Lock( c->c_mutex );
			if ( c->c_flags & CONN_FLAG_CLOSING )
			{
			    /* A worker thread has marked that this connection
			     * should be closed by calling disconnect_server. 
				 * move this connection out of the active list
				 * the last thread to use the connection will close it
			     */
				connection_table_move_connection_out_of_active_list(ct,c);
			}
			else if ( c->c_sd == SLAPD_INVALID_SOCKET )
			{
				connection_table_move_connection_out_of_active_list(ct,c);
			}
			else
			{
#if defined(LDAP_IOCP)	 /* When we have IO completion ports, we don't want to do this */
			    if ( !c->c_gettingber && (c->c_flags & CONN_FLAG_SSL) )
#else
			    if ( !c->c_gettingber )
#endif
				{
					FD_SET( c->c_sd, readfds );
			    }
			}
			PR_Unlock( c->c_mutex );
	    }
		c = next;
	}
}
#endif   /* _WIN32 */

static int first_time_setup_pr_read_pds = 1;
static void
setup_pr_read_pds(Connection_Table *ct, PRFileDesc *n_tcps, PRFileDesc *s_tcps, PRIntn *num_to_read)
{
	Connection *c= NULL;
	Connection *next= NULL;
	LBER_SOCKET socketdesc = SLAPD_INVALID_SOCKET;
	int accept_new_connections;
	static int last_accept_new_connections = -1;
	PRIntn count = 0;
	slapdFrontendConfig_t *slapdFrontendConfig = getFrontendConfig();
	int max_threads_per_conn = config_get_maxthreadsperconn();

	accept_new_connections = ((ct->size - g_get_current_conn_count())
	    > slapdFrontendConfig->reservedescriptors);
	if ( ! accept_new_connections ) {
		if ( last_accept_new_connections ) {
			LDAPDebug( LDAP_DEBUG_ANY, "Not listening for new "
			    "connections - too many fds open\n", 0, 0, 0 );
			/* reinitialize n_tcps and s_tcps to the pds */
			first_time_setup_pr_read_pds = 1;
		}
	} else {
		if ( ! last_accept_new_connections &&
		    last_accept_new_connections != -1 ) {
			LDAPDebug( LDAP_DEBUG_ANY, "Listening for new "
			    "connections again\n", 0, 0, 0 );
			/* reinitialize n_tcps and s_tcps to the pds */
			first_time_setup_pr_read_pds = 1;
		}
	}
	last_accept_new_connections = accept_new_connections;


    /* initialize the mapping from connection table entries to fds entries */
	if (first_time_setup_pr_read_pds)
	{
		int i;
	    for (i = 0; i < ct->size; i++)
	    {
	        ct->c[i].c_fdi = SLAPD_INVALID_SOCKET_INDEX;
	    }

    /* The fds entry for n_tcps is always FDS_N_TCPS */
    if (n_tcps != NULL && accept_new_connections)
    {
		ct->fd[FDS_N_TCPS].fd = n_tcps;
		ct->fd[FDS_N_TCPS].in_flags = SLAPD_POLL_FLAGS;
		ct->fd[FDS_N_TCPS].out_flags = 0;
		LDAPDebug( LDAP_DEBUG_HOUSE, 
			"listening for connections on %d\n", socketdesc, 0, 0 );
    } else {
        ct->fd[FDS_N_TCPS].fd = NULL;
    }

    /* The fds entry for s_tcps is always FDS_S_TCPS */
	if (s_tcps != NULL && accept_new_connections)
	{
		ct->fd[FDS_S_TCPS].fd = s_tcps;
		ct->fd[FDS_S_TCPS].in_flags = SLAPD_POLL_FLAGS;
		ct->fd[FDS_S_TCPS].out_flags = 0;
		LDAPDebug( LDAP_DEBUG_HOUSE, 
			"listening for SSL connections on %d\n", socketdesc, 0, 0 );
    } else {
        ct->fd[FDS_S_TCPS].fd = NULL;
    }

#if !defined(_WIN32) 
	/* The fds entry for the signalpipe is always FDS_SIGNAL_PIPE */
	ct->fd[FDS_SIGNAL_PIPE].fd = signalpipe[0];
	ct->fd[FDS_SIGNAL_PIPE].in_flags = SLAPD_POLL_FLAGS;
	ct->fd[FDS_SIGNAL_PIPE].out_flags = 0;
#else
    ct->fd[FDS_SIGNAL_PIPE].fd = NULL;
#endif
	first_time_setup_pr_read_pds = 0;
	}

    /* count is the number of entries we've place in the fds array.
     * we always put n_tcps in slot FDS_N_TCPS, s_tcps in slot
     * FDS_S_TCPS and the signal pipe in slot FDS_SIGNAL_PIPE
     * so we now set count to 3 */
    count = 3;
    
    /* Walk down the list of active connections to find 
	 * out which connections we should poll over.  If a connection
	 * is no longer in use, we should remove it from the linked 
	 * list. */
	c = connection_table_get_first_active_connection (ct);
	while (c) 
    {
	    next = connection_table_get_next_active_connection (ct, c);
	    if ( c->c_mutex == NULL )
	    {
		    connection_table_move_connection_out_of_active_list(ct,c);
	    }
	    else
	    {
	        PR_Lock( c->c_mutex );
			if (c->c_flags & CONN_FLAG_CLOSING)
			{
			    /* A worker thread has marked that this connection
			     * should be closed by calling disconnect_server. 
				 * move this connection out of the active list
				 * the last thread to use the connection will close it
			     */
				connection_table_move_connection_out_of_active_list(ct,c);
			}
			else if ( c->c_sd == SLAPD_INVALID_SOCKET )
			{
				connection_table_move_connection_out_of_active_list(ct,c);
			}
			else if ( c->c_prfd != NULL)
			{
				if ((!c->c_gettingber)
						 && (c->c_threadnumber < max_threads_per_conn))
				{
					ct->fd[count].fd = c->c_prfd;
					ct->fd[count].in_flags = SLAPD_POLL_FLAGS;
	                /* slot i of the connection table is mapped to slot
	                 * count of the fds array */
	                c->c_fdi = count;
	                count++;
				}
	    		else
				{
	        		c->c_fdi = SLAPD_INVALID_SOCKET_INDEX;
	    		}
			}
			PR_Unlock( c->c_mutex );
	    }
		c = next;
	}

	if( num_to_read )
		(*num_to_read) = count;

}

#ifdef notdef /* GGOODREPL */
static void
handle_timeout( void )
{
	static time_t prevtime = 0;
	static time_t housekeeping_fire_time = 0;
	time_t curtime = current_time();

	if (0 == prevtime) {
		prevtime = time (&housekeeping_fire_time);		
	}

	if ( difftime(curtime, prevtime) >= 
		slapd_housekeeping_timer ) {
		int	num_active_threads;

		snmp_collator_update();

		prevtime = curtime;
		num_active_threads = active_threads;
		if ( (num_active_threads == 0)  || 
			(difftime(curtime, housekeeping_fire_time) >= 
		slapd_housekeeping_timer*3) ) {
		housekeeping_fire_time = curtime;
			housekeeping_start(curtime);
		}
	}

}
#endif /* notdef */


static int	idletimeout_reslimit_handle = -1;

/*
 * Register the idletimeout with the binder-based resource limits
 * subsystem. A SLAPI_RESLIMIT_STATUS_... code is returned.
 */
int
daemon_register_reslimits( void )
{
	return( slapi_reslimit_register( SLAPI_RESLIMIT_TYPE_INT, "nsIdleTimeout",
			&idletimeout_reslimit_handle ));
}


/*
 * Compute the idle timeout for the connection.
 *
 * Note: this function must always be called with conn->c_mutex locked.
 */
static int
compute_idletimeout( slapdFrontendConfig_t *fecfg, Connection *conn )
{
	int		idletimeout;

	if ( slapi_reslimit_get_integer_limit( conn, idletimeout_reslimit_handle,
            &idletimeout ) != SLAPI_RESLIMIT_STATUS_SUCCESS ) {
		/*
		 * no limit associated with binder/connection or some other error
		 * occurred.  use the default idle timeout.
	 	 */
		if ( conn->c_isroot ) {
			idletimeout = 0;	/* no limit for Directory Manager */
		} else {
			idletimeout = fecfg->idletimeout;
		}
	}

	return( idletimeout );
}


#ifdef _WIN32
static void
handle_read_ready(Connection_Table *ct, fd_set *readfds)
{
	Connection *c= NULL;
	time_t curtime = current_time();
	slapdFrontendConfig_t *slapdFrontendConfig = getFrontendConfig();
	int idletimeout;

#ifdef LDAP_DEBUG
	if ( slapd_ldap_debug & LDAP_DEBUG_CONNS )
	{
		connection_table_dump_activity_to_errors_log(ct);
	}
#endif /* LDAP_DEBUG */


	/* Instead of going through the whole connection table to see which
	 * connections we can read from, we'll only check the slots in the
	 * linked list */
	c = connection_table_get_first_active_connection (ct);
	while ( c!=NULL )
	{
	    if ( c->c_mutex != NULL )
		{
		    PR_Lock( c->c_mutex );
		    if (connection_is_active_nolock (c) && c->c_gettingber == 0 )
		    {
		        /* read activity */
		        short readready= ( FD_ISSET( c->c_sd, readfds ) );

				/* read activity */
				if ( readready )
				{
					LDAPDebug( LDAP_DEBUG_CONNS, "read activity on %d\n", c->c_ci, 0, 0 );
					c->c_idlesince = curtime;

					/* This is where the work happens ! */
					connection_activity( c );

					/* idle timeout */
				}
				else if (( idletimeout = compute_idletimeout(
						slapdFrontendConfig, c )) > 0 &&
						(curtime - c->c_idlesince) >= idletimeout &&
						NULL == c->c_ops )
				{
					disconnect_server_nomutex( c, c->c_connid, -1,
								   SLAPD_DISCONNECT_IDLE_TIMEOUT, EAGAIN );
				}
			}
			PR_Unlock( c->c_mutex );
		}
		c = connection_table_get_next_active_connection (ct, c);
	}
}
#endif   /* _WIN32 */


static void
handle_pr_read_ready(Connection_Table *ct, PRIntn num_poll)
{
	Connection *c;
	time_t curtime = current_time();
	slapdFrontendConfig_t *slapdFrontendConfig = getFrontendConfig();
	int idletimeout;
#if defined( XP_WIN32 )
	int i;
#endif

#if LDAP_DEBUG 
	if ( slapd_ldap_debug & LDAP_DEBUG_CONNS )
	{
		connection_table_dump_activity_to_errors_log(ct);
	}
#endif /* LDAP_DEBUG */

#if defined( XP_WIN32 )
	/*
	 * WIN32: this function is only called for SSL connections and
	 * num_poll indicates exactly how many PR fds we polled on.
	 */
	for ( i = 0; i < num_poll; i++ )
	{
		short readready;
		readready = (ct->fd[i].out_flags & SLAPD_POLL_FLAGS);

		/* Find the connection we are referring to */
		for ( c = connection_table_get_first_active_connection (ct); c != NULL; 
              c = connection_table_get_next_active_connection (ct, c) )
		{
			if ( c->c_mutex != NULL )
			{
				PR_Lock( c->c_mutex );
				if ( c->c_prfd == ct->fd[i].fd )
				{
					break;	/* c_mutex is still locked! */
				}
				PR_Unlock( c->c_mutex );
			}
		}

		if ( c == NULL )
		{	/* connection not found! */
			LDAPDebug( LDAP_DEBUG_CONNS, "handle_pr_read_ready: "
			    "connection not found for poll slot %d\n", i,0,0 );
		}
		else
		{
			/* c_mutex is still locked... check for activity and errors */
			if ( !readready && ct->fd[i].out_flags && c->c_prfd == ct->fd[i].fd )
			{
				/* some error occured */
				LDAPDebug( LDAP_DEBUG_CONNS,
					"poll says connection on sd %d is bad "
					"(closing)\n", c->c_sd, 0, 0 );
				disconnect_server_nomutex( c, c->c_connid, -1, SLAPD_DISCONNECT_POLL, EPIPE );
			}
			else if ( readready && c->c_prfd == ct->fd[i].fd )
			{
				/* read activity */
				LDAPDebug( LDAP_DEBUG_CONNS,
					"read activity on %d\n", i, 0, 0 );
				c->c_idlesince = curtime;

				/* This is where the work happens ! */
				connection_activity( c );
			}
			else if (( idletimeout = compute_idletimeout( slapdFrontendConfig,
					c )) > 0 &&
					c->c_prfd == ct->fd[i].fd &&
					(curtime - c->c_idlesince) >= idletimeout &&
					NULL == c->c_ops )
			{
				/* idle timeout */
				disconnect_server_nomutex( c, c->c_connid, -1,
							   SLAPD_DISCONNECT_IDLE_TIMEOUT, EAGAIN );
			}

			PR_Unlock( c->c_mutex );
		}
	}
#else

	/*
	 * non-WIN32: this function is called for all connections, so we
	 * traverse the entire active connection list to find any errors,
	 * activity, etc.
	 */
	for ( c = connection_table_get_first_active_connection (ct); c != NULL; 
          c = connection_table_get_next_active_connection (ct, c) )
	{
		if ( c->c_mutex != NULL )
		{
			PR_Lock( c->c_mutex );
			if ( connection_is_active_nolock (c) && c->c_gettingber == 0 )
			{
			    PRInt16 out_flags;
				short readready;

	            if (c->c_fdi != SLAPD_INVALID_SOCKET_INDEX)
	            {
	                out_flags = ct->fd[c->c_fdi].out_flags;
	            }
	            else
	            {
	                out_flags = 0;
	            }

				readready = ( out_flags & SLAPD_POLL_FLAGS );

				if ( !readready && out_flags )
				{
					/* some error occured */
					LDAPDebug( LDAP_DEBUG_CONNS,
					    "POLL_FN() says connection on sd %d is bad "
					    "(closing)\n", c->c_sd, 0, 0 );
					disconnect_server_nomutex( c, c->c_connid, -1,
								   SLAPD_DISCONNECT_POLL, EPIPE );
				}
				else if ( readready )
				{
					/* read activity */
					LDAPDebug( LDAP_DEBUG_CONNS,
					    "read activity on %d\n", c->c_ci, 0, 0 );
					c->c_idlesince = curtime;

					/* This is where the work happens ! */
					/* MAB: 25 jan 01, error handling added */
					if ((connection_activity( c )) == -1) {
						/* This might happen as a result of
						 * trying to acquire a closing connection
						 */
						LDAPDebug (LDAP_DEBUG_ANY,
							"connection_activity: abandoning conn %d as fd=%d is already closing\n",
							c->c_connid,c->c_sd,0); 
						/* The call disconnect_server should do nothing,
						 * as the connection c should be already set to CLOSING */
						disconnect_server_nomutex( c, c->c_connid, -1,
									   SLAPD_DISCONNECT_POLL, EPIPE );
					}
				}
				else if (( idletimeout = compute_idletimeout(
						slapdFrontendConfig, c )) > 0 &&
						(curtime - c->c_idlesince) >= idletimeout &&
						NULL == c->c_ops )
				{
					/* idle timeout */
					disconnect_server_nomutex( c, c->c_connid, -1,
								   SLAPD_DISCONNECT_IDLE_TIMEOUT, EAGAIN );
				}
			}
			PR_Unlock( c->c_mutex );
		}
	}
#endif
}

/*
 * wrapper functions required so we can implement ioblock_timeout and
 * avoid blocking forever.
 */

#define SLAPD_POLLIN 0
#define SLAPD_POLLOUT 1

/* Return 1 if the given handle is ready for input or output,
 * or if it becomes ready within g_ioblock_timeout [msec].
 * Return -1 if handle is not ready and g_ioblock_timeout > 0,
 * or something goes seriously wrong.  Otherwise, return 0.
 * If -1 is returned, PR_GetError() explains why.
 * Revision: handle changed to void * to allow 64bit support
 */
static int
slapd_poll( void *handle, int output, int secure )
{
    int		rc;
	int ioblock_timeout = config_get_ioblocktimeout();
	
#if defined( XP_WIN32 )
	if( !secure ) {
		fd_set		handle_set;
		struct timeval	timeout;
		int windows_handle = (int) handle;

		memset (&timeout, 0, sizeof(timeout));
		if (ioblock_timeout > 0) {
			timeout.tv_sec = ioblock_timeout / 1000;
			timeout.tv_usec = (ioblock_timeout % 1000) * 1000;
		}
		FD_ZERO(&handle_set);
		FD_SET(windows_handle, &handle_set);
		rc = output ? select(FD_SETSIZE, NULL, &handle_set, NULL, &timeout)
			: select(FD_SETSIZE, &handle_set, NULL, NULL, &timeout);
	} else {
		struct POLL_STRUCT	pr_pd;
		PRIntervalTime	timeout = PR_MillisecondsToInterval( ioblock_timeout );

		if (timeout < 0) timeout = 0;
		pr_pd.fd = (PRFileDesc *)handle;
		pr_pd.in_flags = output ? PR_POLL_WRITE : PR_POLL_READ;
		pr_pd.out_flags = 0;
		rc = POLL_FN(&pr_pd, 1, timeout);
	}
#else
    struct POLL_STRUCT	pr_pd;
    PRIntervalTime	timeout = PR_MillisecondsToInterval(ioblock_timeout);

    if (timeout < 0) timeout = 0;
    pr_pd.fd = (PRFileDesc *)handle;
    pr_pd.in_flags = output ? PR_POLL_WRITE : PR_POLL_READ;
    pr_pd.out_flags = 0;
    rc = POLL_FN(&pr_pd, 1, timeout);
#endif

    if (rc < 0) {
#if defined( XP_WIN32 )
	if( !secure ) {
		int	oserr = errno;

		LDAPDebug(LDAP_DEBUG_CONNS, "slapd_poll(%d) error %d (%s)\n",
			  handle, oserr, slapd_system_strerror(oserr));
		if ( SLAPD_SYSTEM_WOULD_BLOCK_ERROR(oserr)) {
		    rc = 0;		/* try again */
		}
	} else {
		PRErrorCode prerr = PR_GetError();
		LDAPDebug(LDAP_DEBUG_CONNS, "slapd_poll(%d) "
				SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
				handle, prerr, slapd_pr_strerror(prerr));
		if ( prerr == PR_PENDING_INTERRUPT_ERROR ||
			SLAPD_PR_WOULD_BLOCK_ERROR(prerr)) {
		    rc = 0;		/* try again */
		}
	}
#else
	PRErrorCode prerr = PR_GetError();
	LDAPDebug(LDAP_DEBUG_ANY, "slapd_poll(%d) "
			SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
			handle, prerr, slapd_pr_strerror(prerr));
	if ( prerr == PR_PENDING_INTERRUPT_ERROR ||
		SLAPD_PR_WOULD_BLOCK_ERROR(prerr)) {
	    rc = 0;		/* try again */
	}
#endif

    } else if (rc == 0 && ioblock_timeout > 0) {
	PRIntn ihandle;
#if !defined( XP_WIN32 )
	ihandle = PR_FileDesc2NativeHandle((PRFileDesc *)handle);
#else
	if( secure )
		ihandle = PR_FileDesc2NativeHandle((PRFileDesc *)handle);
	else
		ihandle = (PRIntn)handle;
#endif
	LDAPDebug(LDAP_DEBUG_ANY, "slapd_poll(%d) timed out\n",
		  ihandle, 0, 0);
#if defined( XP_WIN32 )
	/*
	 * Bug 624303 - This connection will be cleaned up soon.
	 * During cleanup (see connection_cleanup()), SSL3_SendAlert()
	 * will be called by PR_Close(), and its default wTimeout
	 * in sslSocket associated with the handle
	 * is no time out (I gave up after waited for 30 minutes).
	 * It was during this closing period that server won't
	 * response to new connection requests.
	 * PR_Send() null is a hack here to change the default wTimeout
	 * (see ssl_Send()) to one second which affects PR_Close()
	 * only in the current scenario.
	 */ 
	if( secure ) {
		PR_Send ((PRFileDesc *)handle, NULL, 0, 0, PR_SecondsToInterval(1));
	}
#endif
	PR_SetError(PR_IO_TIMEOUT_ERROR, EAGAIN); /* timeout */
	rc = -1;
    }
    return rc;
}

/* The following 4 functions each read or write count bytes from or to
 * a socket handle.  If all goes well, they return the same count;
 * otherwise they return -1 and PR_GetError() explains the problem.
 * Revision: handle changed to struct lextiof_socket_private * and first
 * argument which used to be handle is now ignored.
 */
int
secure_read_function( int ignore, void *buffer, int count, struct lextiof_socket_private *handle )
{
    int  gotbytes = 0;
    int     bytes;
    int ioblock_timeout = config_get_ioblocktimeout();
	PRIntervalTime pr_timeout = PR_MillisecondsToInterval(ioblock_timeout);	

    if (handle == SLAPD_INVALID_SOCKET) {
		PR_SetError(PR_NOT_SOCKET_ERROR, EBADF);
    } else {
	while (1) {
		bytes = PR_Recv( (PRFileDesc *)handle, (char *)buffer + gotbytes, 
                          count - gotbytes, 0, pr_timeout );
		if (bytes > 0) {
		    gotbytes += bytes;
		} else if (bytes < 0) {
		    PRErrorCode prerr = PR_GetError();

#ifdef _WIN32
            /* we need to do this because on NT, once an I/O
               times out on an NSPR socket, that socket must
               be closed before any other I/O can happen in
               this thread.
             */
            if (prerr == PR_IO_TIMEOUT_ERROR){
                Connection *conn = connection_table_get_connection_from_fd(the_connection_table,(PRFileDesc *)handle);
                if (conn == NULL)
                    return -1;

                disconnect_server (conn, conn->c_connid, -1, SLAPD_DISCONNECT_NTSSL_TIMEOUT, 0);
				/* Disconnect_server just tells the poll thread that the 
				 * socket should be closed.  We'll sleep 2 seconds here to 
				 * to make sure that the poll thread has time to run 
				 * and close this socket. */
				DS_Sleep(PR_SecondsToInterval(2));
				
                LDAPDebug(LDAP_DEBUG_CONNS, "SSL PR_Recv(%d) "
						SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
			            handle, prerr, slapd_pr_strerror(prerr));

                return -1;
            }
#endif

		    LDAPDebug(LDAP_DEBUG_CONNS,
			    "SSL PR_Recv(%d) error %d (%s)\n",
			    handle, prerr, slapd_pr_strerror(prerr));
		    if ( !SLAPD_PR_WOULD_BLOCK_ERROR(prerr) ) {
			break;		/* fatal error */
		    }
		} else if (gotbytes < count) {
		    LDAPDebug(LDAP_DEBUG_CONNS,
			    "SSL PR_Recv(%d) 0 (EOF)\n", /* disconnected */
			    handle, 0, 0);
		    PR_SetError(PR_PIPE_ERROR, EPIPE);
		    break;
		}
		if (gotbytes == count) { /* success */
		    return count;
		} else if (gotbytes > count) { /* too many bytes */
		    PR_SetError(PR_BUFFER_OVERFLOW_ERROR, EMSGSIZE);
		    break;
		} else if (slapd_poll(handle, SLAPD_POLLIN, 1) < 0) { /* error */
		    break;
		}
	}
    }
    return -1;
}


/*
 * Revision: handle changed to struct lextiof_socket_private * and first
 * argument which used to be handle is now ignored. 
 */
int
secure_write_function( int ignore, const void *buffer, int count, struct lextiof_socket_private *handle )
{
    int  sentbytes = 0;
    int     bytes;

    if (handle == SLAPD_INVALID_SOCKET) {
		PR_SetError(PR_NOT_SOCKET_ERROR, EBADF);
    } else {
	while (1) {
		if (slapd_poll(handle, SLAPD_POLLOUT, 1) < 0) { /* error */
		    break;
		}
		bytes = PR_Write((PRFileDesc *)handle, (char *)buffer + sentbytes,
		    count - sentbytes); 
		if (bytes > 0) {
			sentbytes += bytes;
		} else if (bytes < 0) {
		    PRErrorCode prerr = PR_GetError();
		    LDAPDebug(LDAP_DEBUG_CONNS, "SSL PR_Write(%d) "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					handle, prerr, slapd_pr_strerror( prerr ));
		    if ( !SLAPD_PR_WOULD_BLOCK_ERROR(prerr)) {
			break;		/* fatal error */
		    }
		} else if (sentbytes < count) {
		    LDAPDebug( LDAP_DEBUG_CONNS,
			    "SSL PR_Write(%d) 0\n", /* ??? */ handle, 0, 0);
		    PR_SetError(PR_PIPE_ERROR, EPIPE);
		    break;
		}
		if (sentbytes == count) { /* success */
		    return count;
		} else if (sentbytes > count) { /* too many bytes */
		    PR_SetError(PR_BUFFER_OVERFLOW_ERROR, EMSGSIZE);
		    break;
		}
	}
    }
    return -1;
}

/* stub functions required because we need to call send/recv on NT,
 * but the SDK requires functions with a read/write signature.
 * Revision: handle changed to struct lextiof_socket_private * and first
 * argument which used to be handle is now ignored.
 */
int
read_function(int ignore, void *buffer, int count, struct lextiof_socket_private *handle )
{
    int  gotbytes = 0;
    int     bytes;
#if !defined( XP_WIN32 )
	PRIntervalTime pr_timeout = PR_MillisecondsToInterval(1000);	
#endif

    if (handle == SLAPD_INVALID_SOCKET) {
		PR_SetError(PR_NOT_SOCKET_ERROR, EBADF);
    } else {
	while (1) {
#if !defined( XP_WIN32 )
		bytes = PR_Recv((PRFileDesc *)handle, (char *)buffer + gotbytes,
		    count - gotbytes, 0, pr_timeout);
#else
		bytes = recv((int)handle, (char *)buffer + gotbytes,
		    count - gotbytes, 0);
#endif
		if (bytes > 0) {
			gotbytes += bytes;
		} else if (bytes < 0) {
#if !defined( XP_WIN32 )
		    PRErrorCode prerr = PR_GetError();

		    LDAPDebug(LDAP_DEBUG_CONNS, "PR_Recv(%d) "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					handle, prerr, slapd_pr_strerror( prerr ));
		    if ( !SLAPD_PR_WOULD_BLOCK_ERROR(prerr)) {
#else
		    int oserr = errno;

		    LDAPDebug(LDAP_DEBUG_CONNS, "recv(%d) OS error %d (%s)\n",
			      handle, oserr, slapd_system_strerror(oserr));
		    if ( !SLAPD_SYSTEM_WOULD_BLOCK_ERROR(oserr)) {
			PR_SetError(PR_UNKNOWN_ERROR, oserr);
#endif
			break;		/* fatal error */
		    }
		} else if (gotbytes < count) {	/* disconnected */
#if !defined( XP_WIN32 )
		    LDAPDebug(LDAP_DEBUG_CONNS, "PR_Recv(%d) 0 (EOF)\n",
			      handle, 0, 0);
#else
		    LDAPDebug(LDAP_DEBUG_CONNS, "recv(%d) 0 (EOF)\n",
			      handle, 0, 0);
#endif
		    PR_SetError(PR_PIPE_ERROR, EPIPE);
		    break;
		}
		if (gotbytes == count) { /* success */
		    return count;
		} else if (gotbytes > count) { /* too many bytes */
		    PR_SetError(PR_BUFFER_OVERFLOW_ERROR, EMSGSIZE);
		    break;
		}
		/* we did not get the whole PDU
		 * call slapd_poll before starting a new read to get
		 * sure some new data have been received and 
		 * thus avoid active looping in the while 
		 */
		if (slapd_poll(handle, SLAPD_POLLIN, 0) < 0) {
		    break;
		}
	}
    }
    return -1;
}

/*
Slapd's old (3.x) network I/O code works something like this: 
  when I want to send some data to the client, I will call send(). 
  That might block for a long time, resulting in thread pool starvation, 
  so let's not call it unless we're sure that data can be buffered 
  locally. The mechanism for achieving this is to call select() 
  (poll() on UNIX), on the target socket, passing a short timeout 
  (configurable via cn=config). 

  Now, this means that to send some data we're making two system 
  calls. Slowness results. 

  I did some research and found the following in the MSDN
  that NT4.0 and beyond do support the configuration of a send timeout
  on sockets, so this is code which makes use of that and saves the
  call to select.
*/

/*Revision: handle changed from int to void * to allow 64bit support
 *
 */
static int send_with_timeout(void *handle, const char * buffer, int count,int *bytes_sent) 
{
	int ret = 0;
#if defined( XP_WIN32 )
	*bytes_sent = send((SOCKET)handle, buffer,count,0);
#else
	*bytes_sent = PR_Write((PRFileDesc *)handle,buffer,count);
	if (*bytes_sent < 0)
	{
		PRErrorCode prerr = PR_GetError();
		if (SLAPD_PR_WOULD_BLOCK_ERROR(prerr))
		{
			if ((ret = slapd_poll(handle, SLAPD_POLLOUT, 0)) < 0)
			{ /* error */
				*bytes_sent = 0;
				return ret;
			}

		}
	}
#endif		

	return ret;
}

int
write_function(int ignore, const void *buffer, int count, struct lextiof_socket_private *handle)
{
    int  sentbytes = 0;
    int     bytes;


    if (handle == SLAPD_INVALID_SOCKET) {
	PR_SetError(PR_NOT_SOCKET_ERROR, EBADF);
    } else {
	while (1) {
		
		if (send_with_timeout(handle, (char *)buffer + sentbytes, count - sentbytes,&bytes) < 0) { /* error */
		    break;
		}
		if (bytes > 0) {
			sentbytes += bytes;
		} else if (bytes < 0) {
#if !defined( XP_WIN32 )
		    PRErrorCode prerr = PR_GetError();

		    LDAPDebug(LDAP_DEBUG_CONNS, "PR_Write(%d) "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					handle, prerr, slapd_pr_strerror(prerr));
		    if ( !SLAPD_PR_WOULD_BLOCK_ERROR(prerr)) {
#else
		    int oserr = errno; /* DBDB this is almost certainly wrong, should be a call to WSAGetLastError() */

		    LDAPDebug(LDAP_DEBUG_CONNS, "send(%d) error %d (%s)\n",
			      handle, oserr, slapd_system_strerror(oserr));
		    if ( !SLAPD_SYSTEM_WOULD_BLOCK_ERROR(oserr)) {
			PR_SetError(PR_UNKNOWN_ERROR, oserr);
#endif
			break;		/* fatal error */
		    }
		} else if (sentbytes < count) {
		    LDAPDebug(LDAP_DEBUG_CONNS, "send(%d) 0\n", /* ??? */
			      handle, 0, 0);
		    PR_SetError(PR_PIPE_ERROR, EPIPE);
		    break;
		}
		if (sentbytes == count) { /* success */
		    return count;
		} else if (sentbytes > count) { /* too many bytes */
		    PR_SetError(PR_BUFFER_OVERFLOW_ERROR, EMSGSIZE);
		    break;
		}
	}
    }
    return -1;
}

int connection_type = -1; /* The type number assigned by the Factory for 'Connection' */

void
daemon_register_connection()
{
    if(connection_type==-1)
	{
	    /* The factory is given the name of the object type, in
		 * return for a type handle. Whenever the object is created
		 * or destroyed the factory is called with the handle so
		 * that it may call the constructors or destructors registered
		 * with it.
		 */
        connection_type= factory_register_type(SLAPI_EXT_CONNECTION,offsetof(Connection,c_extension));
	}
}
	
/* NOTE: this routine is not reentrant */
static int
handle_new_connection(Connection_Table *ct, int tcps, PRFileDesc *pr_acceptfd, int secure)
{
	int ns = 0;
	Connection *conn = NULL;
	/*	struct sockaddr_in	from;*/
	PRNetAddr from;
	PRFileDesc *pr_clonefd = NULL;

	if ( (ns = accept_and_configure( tcps, pr_acceptfd, &from,
		sizeof(from), secure, &pr_clonefd)) == SLAPD_INVALID_SOCKET ) {
		return -1;
	}

	/* get a new Connection from the Connection Table */
	conn= connection_table_get_connection(ct,ns);
	if(conn==NULL)
	{
		PR_Close(pr_acceptfd);
		return -1;
	}
	PR_Lock( conn->c_mutex );

#if !defined( XP_WIN32 )
	ber_sockbuf_set_option(conn->c_sb,LBER_SOCKBUF_OPT_DESC,&pr_clonefd);
#else
	if( !secure )
		ber_sockbuf_set_option(conn->c_sb,LBER_SOCKBUF_OPT_DESC,&ns);
	else
		ber_sockbuf_set_option(conn->c_sb,LBER_SOCKBUF_OPT_DESC,&pr_clonefd);
#endif

	conn->c_sd = ns;
	conn->c_prfd = pr_clonefd;
	conn->c_flags &= ~CONN_FLAG_CLOSING;

	/* Store the fact that this new connection is an SSL connection */
	if (secure) {
		conn->c_flags |= CONN_FLAG_SSL;
	}

#ifndef _WIN32
	/*
	 * clear the "returned events" field in ns' slot within the poll fds
	 * array so that handle_read_ready() doesn't look at out_flags for an
	 * old connection by mistake and do something bad such as close the
	 * connection we just accepted.
	 */

    /* Dont have to worry about this now because of our mapping from 
     * the connection table to the fds array.  This new connection
     * won't have a mapping. */
	/* fds[ns].out_flags = 0; */
#endif

	if (secure) {  
		/*structure added to enable 64bit support changed from 
		 *the commented code that follows each of the next two
		 *blocks of code
 		 */
		struct lber_x_ext_io_fns func_pointers;
		memset(&func_pointers, 0, sizeof(func_pointers));
		func_pointers.lbextiofn_size = LBER_X_EXTIO_FNS_SIZE; 
		func_pointers.lbextiofn_read = secure_read_function;
		func_pointers.lbextiofn_write = secure_write_function;
		func_pointers.lbextiofn_writev = NULL;
		func_pointers.lbextiofn_socket_arg = (struct lextiof_socket_private *) pr_clonefd;
		ber_sockbuf_set_option( conn->c_sb,
			LBER_SOCKBUF_OPT_EXT_IO_FNS, &func_pointers);
 
		/* changed here by Cheston
		ber_sockbuf_set_option( conn->c_sb,
			LBER_SOCKBUF_OPT_READ_FN, (void *)secure_read_function );
		ber_sockbuf_set_option( conn->c_sb,
			LBER_SOCKBUF_OPT_WRITE_FN, (void *)secure_write_function );
		*/
	} else {
		struct lber_x_ext_io_fns func_pointers;
		memset(&func_pointers, 0, sizeof(func_pointers));
		func_pointers.lbextiofn_size = LBER_X_EXTIO_FNS_SIZE;
		func_pointers.lbextiofn_read = read_function;
		func_pointers.lbextiofn_write = write_function;
		func_pointers.lbextiofn_writev = NULL;
#ifdef _WIN32
		func_pointers.lbextiofn_socket_arg = (struct lextiof_socket_private *) ns;	
#else
		func_pointers.lbextiofn_socket_arg = (struct lextiof_socket_private *) pr_clonefd;	
#endif
		ber_sockbuf_set_option( conn->c_sb,
			LBER_SOCKBUF_OPT_EXT_IO_FNS, &func_pointers);	
		/*
		ber_sockbuf_set_option( conn->c_sb,
			LBER_SOCKBUF_OPT_READ_FN, (void *)read_function );
		ber_sockbuf_set_option( conn->c_sb,
			LBER_SOCKBUF_OPT_WRITE_FN, (void *)write_function );
		*/
	}

#if defined(NET_SSL)
	if( secure && config_get_SSLclientAuth() != SLAPD_SSLCLIENTAUTH_OFF ) { 
	    /* Prepare to handle the client's certificate (if any): */
		int rv;

		rv = slapd_ssl_handshakeCallback (conn->c_prfd, (void*)handle_handshake_done, conn);
        
		if (rv < 0) {
			PRErrorCode prerr = PR_GetError();
			LDAPDebug (LDAP_DEBUG_ANY, "SSL_HandshakeCallback() %d "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					rv, prerr, slapd_pr_strerror( prerr ));
		}
		rv = slapd_ssl_badCertHook (conn->c_prfd, (void*)handle_bad_certificate, conn);

		if (rv < 0) {
			PRErrorCode prerr = PR_GetError();
			LDAPDebug (LDAP_DEBUG_ANY, "SSL_BadCertHook(%i) %i "
					SLAPI_COMPONENT_NAME_NSPR " error %d\n",
					conn->c_sd, rv, prerr);
		}
	}
#endif

	connection_reset(conn, ns, &from, sizeof(from), secure);

	/* Call the plugin extension constructors */
	conn->c_extension = factory_create_extension(connection_type,conn,NULL /* Parent */);


	/* Add this connection slot to the doubly linked list of active connections.  This
	 * list is used to find the connections that should be used in the poll call. This 
	 * connection will be added directly after slot 0 which serves as the head of the list */
	if ( conn != NULL && conn->c_next == NULL && conn->c_prev == NULL )
	{
		/* Now give the new connection to the connection code */
		connection_table_move_connection_on_to_active_list(the_connection_table,conn);
	}

	PR_Unlock( conn->c_mutex );

	connection_new_private(conn);

	g_increment_current_conn_count();

	return 0;
}

static int init_shutdown_detect()
{

#ifdef _WIN32
	PRThread *service_exit_wait_tid;
#else
  /* First of all, we must reset the signal mask to get rid of any blockages
   * the process may have inherited from its parent (such as the console), which
   * might result in the process not delivering those blocked signals, and thus, 
   * misbehaving.... 
   */
  {
    int rc;
    sigset_t proc_mask;
        
    LDAPDebug( LDAP_DEBUG_TRACE, "Reseting signal mask....\n", 0, 0, 0);
    (void)sigemptyset( &proc_mask );
    rc = pthread_sigmask( SIG_SETMASK, &proc_mask, NULL );
    LDAPDebug( LDAP_DEBUG_TRACE, " %s \n", 
	       rc ? "Failed to reset signal mask":"....Done (signal mask reset)!!", 0, 0 );
  }
#endif
  
#ifdef _WIN32

	/* Create a thread to wait on the Win32 event which will 
	   be signalled by the watchdog when the Service is 
	   being halted. */
	service_exit_wait_tid = PR_CreateThread( PR_USER_THREAD, 
		(VFP) (void *) slapd_service_exit_wait, (void *) NULL, 
		PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD, PR_UNJOINABLE_THREAD, 
		SLAPD_DEFAULT_THREAD_STACKSIZE);
	if( service_exit_wait_tid == NULL ) {
		LDAPDebug( LDAP_DEBUG_ANY,
		"Error: PR_CreateThread(slapd_service_exit_wait) failed\n", 0, 0, 0 );
	}
#elif defined ( HPUX10 )
    PR_CreateThread ( PR_USER_THREAD,
                      catch_signals,
                      NULL,
                      PR_PRIORITY_NORMAL,
                      PR_GLOBAL_THREAD,
                      PR_UNJOINABLE_THREAD,
                      SLAPD_DEFAULT_THREAD_STACKSIZE);
#else
#ifdef HPUX11
	/* In the optimized builds for HPUX, the signal handler doesn't seem 
	 * to get set correctly unless the primordial thread gets a chance 
	 * to run before we make the call to SIGNAL.  (At this point the 
	 * the primordial thread has spawned the daemon thread which called
	 * this function.)  The call to DS_Sleep will give the primordial 
	 * thread a chance to run.
	 */	
	DS_Sleep(0);
#endif
	(void) SIGNAL( SIGPIPE, SIG_IGN );
	(void) SIGNAL( SIGCHLD, slapd_wait4child );
#ifndef LINUX
	/* linux uses USR1/USR2 for thread synchronization, so we aren't
	 * allowed to mess with those.
	 */
	(void) SIGNAL( SIGUSR1, slapd_do_nothing );
	(void) SIGNAL( SIGUSR2, set_shutdown );
#endif
	(void) SIGNAL( SIGTERM, set_shutdown );
	(void) SIGNAL( SIGHUP,  set_shutdown );
#endif /* _WIN32 */
	return 0;
}

#if defined( XP_WIN32 )
static void
unfurl_banners(Connection_Table *ct,daemon_ports_t *ports, int n_tcps, PRFileDesc *s_tcps)
#else
static void
unfurl_banners(Connection_Table *ct,daemon_ports_t *ports, PRFileDesc *n_tcps, PRFileDesc *s_tcps)
#endif
{
	slapdFrontendConfig_t	*slapdFrontendConfig = getFrontendConfig();
	char					addrbuf[ 256 ];

	if ( ct->size <= slapdFrontendConfig->reservedescriptors ) {
#ifdef _WIN32
		LDAPDebug( LDAP_DEBUG_ANY,
		    "ERROR: Not enough descriptors to accept any connections. "
		    "This may be because the maxdescriptors configuration "
		    "directive is too small, or the reservedescriptors "
		    "configuration directive is too large. "
		    "Try increasing the number of descriptors available to "
		    "the slapd process. The current value is %d. %d "
		    "descriptors are currently reserved for internal "
		    "slapd use, so the total number of descriptors available "
		    "to the process must be greater than %d.\n",
		    ct->size, slapdFrontendConfig->reservedescriptors, slapdFrontendConfig->reservedescriptors );
#else /* _WIN32 */
		LDAPDebug( LDAP_DEBUG_ANY,
		    "ERROR: Not enough descriptors to accept any connections. "
		    "This may be because the maxdescriptors configuration "
		    "directive is too small, the hard limit on descriptors is "
		    "too small (see limit(1)), or the reservedescriptors "
		    "configuration directive is too large. "
		    "Try increasing the number of descriptors available to "
		    "the slapd process. The current value is %d. %d "
		    "descriptors are currently reserved for internal "
		    "slapd use, so the total number of descriptors available "
		    "to the process must be greater than %d.\n",
		    ct->size, slapdFrontendConfig->reservedescriptors, slapdFrontendConfig->reservedescriptors );
#endif /* _WIN32 */
		exit( 1 );
	}

	/*
	 * This final startup message gives a definite signal to the admin
	 * program that the server is up.  It must contain the string
	 * "slapd started." because some of the administrative programs
	 * depend on this.  See ldap/admin/lib/dsalib_updown.c.
	 */
#if !defined( XP_WIN32 )
	if ( n_tcps != NULL ) {					/* standard LDAP */
#else
	if ( n_tcps != SLAPD_INVALID_SOCKET ) {	/* standard LDAP */
#endif

		LDAPDebug( LDAP_DEBUG_ANY,
		    "slapd started.  Listening on %s port %d for LDAP requests\n",
			netaddr2string(&ports->n_listenaddr, addrbuf, sizeof(addrbuf)),
		    ports->n_port, 0 );
	}

	if ( s_tcps != NULL ) {					/* LDAP over SSL; separate port */
		LDAPDebug( LDAP_DEBUG_ANY,
		    "Listening on %s port %d for LDAPS requests\n",
			netaddr2string(&ports->s_listenaddr, addrbuf, sizeof(addrbuf)),
		    ports->s_port, 0 );
	}
}

#if defined( _WIN32 )
/* On Windows, we signal the SCM when we're ready to accept connections */
static int
write_pid_file()
{
	if( SlapdIsAService() )
	{
		/* Initialization complete and successful. Set service to running */
		LDAPServerStatus.dwCurrentState	= SERVICE_RUNNING;
		LDAPServerStatus.dwCheckPoint = 0;
		LDAPServerStatus.dwWaitHint = 0;
			
		if (!SetServiceStatus(hLDAPServerServiceStatus, &LDAPServerStatus)) {
			ReportSlapdEvent(EVENTLOG_INFORMATION_TYPE, MSG_SERVER_START_FAILED, 1, 
				"Could not set Service status.");
			exit(1);
		}
	}

	ReportSlapdEvent(EVENTLOG_INFORMATION_TYPE, MSG_SERVER_STARTED, 0, NULL );
	return 0;
}
#else /* WIN32 */
/* On UNIX, we create a file with our PID in it */
static int
write_pid_file()
{
	FILE *fp = NULL;
	/* 
	 * The following section of code is closely coupled with the 
	 * admin programs. Please do not make changes here without
	 * consulting the start/stop code for the admin code.
	 */
	if ( (fp = fopen( get_pid_file(), "w" )) != NULL ) {
		fprintf( fp, "%d\n", getpid() );
		fclose( fp );
		return 0;
	} else
	{
		return -1;
	}
}
#endif /* WIN32 */

static void
set_shutdown (int sig)
{ 
    /* don't log anything from a signal handler:
     * you could be holding a lock when the signal was trapped.  more
     * specifically, you could be holding the logfile lock (and deadlock
     * yourself).
     */
#if 0
    LDAPDebug( LDAP_DEBUG_ANY, "slapd got shutdown signal\n", 0, 0, 0 );
#endif
	g_set_shutdown( SLAPI_SHUTDOWN_SIGNAL );
#ifndef _WIN32
#ifndef LINUX
	/* don't mess with USR1/USR2 on linux, used by libpthread */
	(void) SIGNAL( SIGUSR2, set_shutdown );
#endif
	(void) SIGNAL( SIGTERM, set_shutdown );
	(void) SIGNAL( SIGHUP,  set_shutdown );
#endif
}

#ifndef LINUX
void
slapd_do_nothing (int sig)
{
    /* don't log anything from a signal handler:
     * you could be holding a lock when the signal was trapped.  more
     * specifically, you could be holding the logfile lock (and deadlock
     * yourself).
     */
#if 0
	LDAPDebug( LDAP_DEBUG_TRACE, "slapd got SIGUSR1\n", 0, 0, 0 );
#endif
#ifndef _WIN32
	(void) SIGNAL( SIGUSR1, slapd_do_nothing );
#endif

#if 0
	/*
	 * Actually do a little more: dump the conn struct and 
	 * send it to a tmp file
	 */
	connection_table_dump(connection_table);
#endif
}
#endif   /* LINUX */

#ifndef _WIN32
void
slapd_wait4child(int sig)
{
        WAITSTATUSTYPE     status;

    /* don't log anything from a signal handler:
     * you could be holding a lock when the signal was trapped.  more
     * specifically, you could be holding the logfile lock (and deadlock
     * yourself).
     */
#if 0
        LDAPDebug( LDAP_DEBUG_ARGS, "listener: catching SIGCHLD\n", 0, 0, 0 );
#endif
#ifdef USE_WAITPID
        while (waitpid ((pid_t) -1, 0, WAIT_FLAGS) > 0)
#else /* USE_WAITPID */
        while ( wait3( &status, WAIT_FLAGS, 0 ) > 0 )
#endif /* USE_WAITPID */
                ;       /* NULL */

        (void) SIGNAL( SIGCHLD, slapd_wait4child );
}
#endif

#ifdef XP_WIN32
static int
createlistensocket(unsigned short port, const PRNetAddr *listenaddr)
{
	int					tcps;
	struct sockaddr_in	addr;
	char				*logname = "createlistensocket";
	char				addrbuf[ 256 ];

	if (!port) goto suppressed;

	PR_ASSERT( listenaddr != NULL );

	/* create TCP socket */
	if ((tcps = socket(AF_INET, SOCK_STREAM, 0))
		== SLAPD_INVALID_SOCKET) {
		int oserr = errno;

		slapi_log_error(SLAPI_LOG_FATAL, logname,
			"socket() failed: OS error %d (%s)\n",
			oserr, slapd_system_strerror( oserr ));
		goto failed;
	}
	
	/* initialize listener address */
	(void) memset( (void *) &addr, '\0', sizeof(addr) );
	addr.sin_family = AF_INET;
	addr.sin_port = htons( port );
	if (listenaddr->raw.family == PR_AF_INET) {
		addr.sin_addr.s_addr = listenaddr->inet.ip;
	} else if (PR_IsNetAddrType(listenaddr,PR_IpAddrAny)) {
		addr.sin_addr.s_addr = INADDR_ANY;
	} else {
		if (!PR_IsNetAddrType(listenaddr,PR_IpAddrV4Mapped)) {
			/*
			 * When Win32 supports IPv6, we will be able to use IPv6
			 * addresses here. But not yet.
			 */
			slapi_log_error(SLAPI_LOG_FATAL, logname,
					"unable to listen on %s port %d (IPv6 addresses "
					"are not supported on this platform)\n",
					netaddr2string(listenaddr, addrbuf, sizeof(addrbuf)),
					port );
			goto failed;
		}

		addr.sin_addr.s_addr = listenaddr->ipv6.ip.pr_s6_addr32[3];
	}

	LDAPDebug( LDAP_DEBUG_TRACE, "%s - binding to %s:%d\n",
	    logname, inet_ntoa( addr.sin_addr ), port )

	if ( bind( tcps, (struct sockaddr *) &addr, sizeof(addr) ) == -1 ) {
		int oserr = errno;

		slapi_log_error(SLAPI_LOG_FATAL, logname,
			"bind() on %s port %d failed: OS error %d (%s)\n",
			inet_ntoa( addr.sin_addr ), port, oserr,
			slapd_system_strerror( oserr ));
		goto failed;
	}

	return tcps;

failed:
		WSACleanup();
		exit( 1 );
suppressed:
		return -1;
}  /* createlistensocket */
#endif   /* XP_WIN32 */


static PRFileDesc *
createprlistensocket(unsigned short port, const PRNetAddr *listenaddr,
		int secure)
{
	PRFileDesc			*sock;
	PRNetAddr			sa_server;
	PRErrorCode			prerr = 0;
	PRSocketOptionData	pr_socketoption;
	char				addrbuf[ 256 ];
	char				*logname = "createprlistensocket";

	if (!port) goto suppressed;

	PR_ASSERT( listenaddr != NULL );

	/* create TCP socket */
	if ((sock = PR_OpenTCPSocket(PR_AF_INET6)) == SLAPD_INVALID_SOCKET) {
		prerr = PR_GetError();
		slapi_log_error(SLAPI_LOG_FATAL, logname,
		    "PR_OpenTCPSocket(PR_AF_INET6) failed: %s error %d (%s)\n",
		    SLAPI_COMPONENT_NAME_NSPR, prerr, slapd_pr_strerror(prerr));
		goto failed;
	}

	pr_socketoption.option = PR_SockOpt_Reuseaddr;
	pr_socketoption.value.reuse_addr = 1;
	if ( PR_SetSocketOption(sock, &pr_socketoption ) == PR_FAILURE) {
		prerr = PR_GetError();
		slapi_log_error(SLAPI_LOG_FATAL, logname,
			"PR_SetSocketOption(PR_SockOpt_Reuseaddr) failed: %s error %d (%s)\n",
		    SLAPI_COMPONENT_NAME_NSPR, prerr, slapd_pr_strerror( prerr ));
		goto failed;	
	}

	/* set up listener address, including port */
	memcpy(&sa_server, listenaddr, sizeof(sa_server));
	if ( PR_SetNetAddr(PR_IpAddrNull, PR_AF_INET6, port, &sa_server)
				!= PR_SUCCESS ) {
		prerr = PR_GetError();
		slapi_log_error(SLAPI_LOG_FATAL, logname,
				"PR_SetNetAddr() failed: %s error %d (%s)\n",
				SLAPI_COMPONENT_NAME_NSPR,
				prerr, slapd_pr_strerror(prerr));
		goto failed;
	}

	if ( PR_Bind(sock, &sa_server) == PR_FAILURE) {
		prerr = PR_GetError();
		slapi_log_error(SLAPI_LOG_FATAL, logname,
				"PR_Bind() on %s port %d failed: %s error %d (%s)\n",
				netaddr2string(&sa_server, addrbuf, sizeof(addrbuf)), port,
				SLAPI_COMPONENT_NAME_NSPR, prerr, slapd_pr_strerror(prerr));
		goto failed;	
	}

	return( sock );

failed:
#ifdef XP_WIN32
	WSACleanup();
#endif   /* XP_WIN32 */
	exit( 1 );

suppressed:
	return (PRFileDesc *)-1;
}  /* createprlistensocket */


/*
 * Initialize the *addr structure based on listenhost.
 * Returns: 0 if successful and -1 if not (after logging an error message).
 */
int
slapd_listenhost2addr(const char *listenhost, PRNetAddr *addr)
{
	char			*logname = "slapd_listenhost2addr";
	PRErrorCode		prerr = 0;
	PRHostEnt		hent;
	char			hbuf[ PR_NETDB_BUF_SIZE ];

	PR_ASSERT( addr != NULL );

	if (NULL == listenhost) {
		/* listen on all interfaces */
		if ( PR_SUCCESS != PR_SetNetAddr(PR_IpAddrAny, PR_AF_INET6, 0, addr)) {
			prerr = PR_GetError();
			slapi_log_error( SLAPI_LOG_FATAL, logname,
					"PR_SetNetAddr(PR_IpAddrAny) failed - %s error %d (%s)\n",
					SLAPI_COMPONENT_NAME_NSPR, prerr, slapd_pr_strerror(prerr));
			goto failed;
		}
	} else if (PR_SUCCESS == PR_StringToNetAddr(listenhost, addr)) {
		if (PR_AF_INET == PR_NetAddrFamily(addr)) {
			PRUint32	ipv4ip = addr->inet.ip;
			memset(addr, 0, sizeof(PRNetAddr));
			PR_ConvertIPv4AddrToIPv6(ipv4ip, &addr->ipv6.ip);
			addr->ipv6.family = PR_AF_INET6;
		}
	} else if (PR_SUCCESS == PR_GetIPNodeByName(listenhost,
				PR_AF_INET6, PR_AI_DEFAULT | PR_AI_ALL,
				hbuf, sizeof(hbuf), &hent )) {
		/* just use the first IP address returned */
		if (PR_EnumerateHostEnt(0, &hent, 0, addr) < 0) {
			slapi_log_error( SLAPI_LOG_FATAL, logname,
					"PR_EnumerateHostEnt() failed - %s error %d (%s)\n",
					SLAPI_COMPONENT_NAME_NSPR, prerr, slapd_pr_strerror(prerr));
			goto failed;
		}
	} else {	/* failure */
		slapi_log_error( SLAPI_LOG_FATAL, logname,
				"PR_GetIPNodeByName(%s) failed - %s error %d (%s)\n",
				listenhost, SLAPI_COMPONENT_NAME_NSPR, prerr,
				slapd_pr_strerror(prerr));
		goto failed;
	}

	return( 0 );

failed:
	return( -1 );
}


/*
 * Map addr to a string equivalent and place the result in addrbuf.
 */
static const char *
netaddr2string(const PRNetAddr *addr, char *addrbuf, size_t addrbuflen)
{
	const char	*retstr;

	if (NULL == addr || PR_IsNetAddrType(addr, PR_IpAddrAny)) {
		retstr = "All Interfaces";
	} else if (PR_IsNetAddrType(addr, PR_IpAddrLoopback)) {
		if ( addr->raw.family == PR_AF_INET6 &&
					!PR_IsNetAddrType(addr, PR_IpAddrV4Mapped)) {
			retstr = "IPv6 Loopback";
		} else {
			retstr = "Loopback";
		}
	} else if (PR_SUCCESS == PR_NetAddrToString( addr, addrbuf, addrbuflen)) {
		if (0 == strncmp( addrbuf, "::ffff:", 7 )) {
			/* IPv4 address mapped into IPv6 address space */
			retstr = addrbuf + 7;
		} else {
			/* full blown IPv6 address */
			retstr = addrbuf;
		}
	} else {	/* punt */
		retstr = "address conversion failed";
	}

	return(retstr);
}


static int
createsignalpipe( void )
{
#if defined( _WIN32 )
	if ( PR_NewTCPSocketPair(&signalpipe[0])) {
		PRErrorCode prerr = PR_GetError();
		LDAPDebug( LDAP_DEBUG_ANY, "PR_CreatePipe() failed, "
			SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
			prerr, slapd_pr_strerror(prerr), SLAPD_DEFAULT_THREAD_STACKSIZE );
		return( -1 );
	}
	writesignalpipe = PR_FileDesc2NativeHandle(signalpipe[1]);
	readsignalpipe = PR_FileDesc2NativeHandle(signalpipe[0]);
#else
	if ( PR_CreatePipe( &signalpipe[0], &signalpipe[1] ) != 0 ) {
		PRErrorCode prerr = PR_GetError();
		LDAPDebug( LDAP_DEBUG_ANY, "PR_CreatePipe() failed, "
			SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
		    prerr, slapd_pr_strerror(prerr), SLAPD_DEFAULT_THREAD_STACKSIZE );
		return( -1 );
	}
	writesignalpipe = PR_FileDesc2NativeHandle(signalpipe[1]);
	readsignalpipe = PR_FileDesc2NativeHandle(signalpipe[0]);
	fcntl(writesignalpipe, F_SETFD, O_NONBLOCK);
	fcntl(readsignalpipe, F_SETFD, O_NONBLOCK);
#endif

	return( 0 );
} 


#ifdef HPUX10
#include <pthread.h> /* for sigwait */
/*
 * Set up a thread to catch signals
 * SIGUSR1 (ignore), SIGCHLD (call slapd_wait4child),
 * SIGUSR2 (set slapd_shutdown), SIGTERM (set slapd_shutdown),
 * SIGHUP (set slapd_shutdown)
 */
static void *
catch_signals()
{
    sigset_t    caught_signals;
    int         sig;
 
    sigemptyset( &caught_signals );
 
    while ( !g_get_shutdown() ) {
 
	  /* Set the signals we're interested in catching */
        sigaddset( &caught_signals, SIGUSR1 );
        sigaddset( &caught_signals, SIGCHLD );
        sigaddset( &caught_signals, SIGUSR2 );
        sigaddset( &caught_signals, SIGTERM );
        sigaddset( &caught_signals, SIGHUP );
 
        (void)sigprocmask( SIG_BLOCK, &caught_signals, NULL );
 
        if (( sig = sigwait( &caught_signals )) < 0 ) {
            LDAPDebug( LDAP_DEBUG_ANY, "catch_signals: sigwait returned -1\n",
                    0, 0, 0 );
            continue;
        } else {
            LDAPDebug( LDAP_DEBUG_TRACE, "catch_signals: detected signal %d\n",
                    sig, 0, 0 );
            switch ( sig ) {
            case SIGUSR1:
                continue;       /* ignore SIGUSR1 */
            case SIGUSR2:       /* fallthrough */
            case SIGTERM:       /* fallthrough */
            case SIGHUP:
                g_set_shutdown( SLAPI_SHUTDOWN_SIGNAL );
                return NULL;
            case SIGCHLD:
                slapd_wait4child( sig );
                break;
            default:
                LDAPDebug( LDAP_DEBUG_ANY,
                    "catch_signals: unknown signal (%d) received\n",
                    sig, 0, 0 );
            }
        }
    }
}
#endif /* HPUX */
 
static int
get_configured_connection_table_size()
{
	int size;
	size = config_get_conntablesize();

/*
 * Cap the table size at nsslapd-maxdescriptors.
 */
#if !defined(_WIN32) && !defined(AIX)
	{
		int maxdesc = config_get_maxdescriptors();

		if ( maxdesc >= 0 && size > maxdesc ) {
			size = maxdesc;
		}
	}
#endif

	return size;
}




PRFileDesc * get_ssl_listener_fd()
{
  PRFileDesc * listener;

  listener = the_connection_table->fd[FDS_S_TCPS].fd;

  return listener;
}



int configure_pr_socket( PRFileDesc **pr_socket, int secure )
{
	int ns = 0;
	int reservedescriptors = config_get_reservedescriptors();
	int enable_nagle = config_get_nagle();

	PRSocketOptionData pr_socketoption;
  
#if defined(LINUX)
	/* On Linux we use TCP_CORK so we must enable nagle */
	enable_nagle = 1;
#endif

	ns = PR_FileDesc2NativeHandle( *pr_socket );
	
#if !defined(_WIN32)
	/*
	 * Some OS or third party libraries may require that low
	 * numbered file descriptors be available, e.g., the DNS resolver
	 * library on most operating systems. Therefore, we try to
	 * replace the file descriptor returned by accept() with a
	 * higher numbered one.  If this fails, we log an error and
	 * continue (not considered a truly fatal error).
	 */
	if ( reservedescriptors > 0 && ns < reservedescriptors ) {
		int		newfd = fcntl( ns, F_DUPFD, reservedescriptors );

		if ( newfd > 0 ) {
			PRFileDesc	*nspr_layer_fd = PR_GetIdentitiesLayer( *pr_socket,
															PR_NSPR_IO_LAYER );
			if ( NULL == nspr_layer_fd ) {
				slapi_log_error( SLAPI_LOG_FATAL, "configure_pr_socket",
						"Unable to move socket file descriptor %d above %d:"
						" PR_GetIdentitiesLayer( %p, PR_NSPR_IO_LAYER )"
						" failed\n", ns, reservedescriptors, *pr_socket );
				close( newfd );	/* can't fix things up in NSPR -- close copy */
			} else {
				PR_ChangeFileDescNativeHandle( nspr_layer_fd, newfd );
				close( ns );	/* dup succeeded -- close the original */
				ns = newfd;
			}
		} else {
			int oserr = errno;
			slapi_log_error(SLAPI_LOG_FATAL, "configure_pr_socket",
				"Unable to move socket file descriptor %d above %d:"
				" OS error %d (%s)\n", ns, reservedescriptors, oserr,
				slapd_system_strerror( oserr ) );
		}
	}
#endif /* !_WIN32 */

	if ( secure ) {
	  
		pr_socketoption.option = PR_SockOpt_Nonblocking;
		pr_socketoption.value.non_blocking = 0;
		if ( PR_SetSocketOption( *pr_socket, &pr_socketoption ) == PR_FAILURE ) {
			PRErrorCode prerr = PR_GetError();
			LDAPDebug( LDAP_DEBUG_ANY,
					"PR_SetSocketOption(PR_SockOpt_Nonblocking) failed, "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					prerr, slapd_pr_strerror(prerr), 0 );
		}
	} else {
		/* We always want to have non-blocking I/O */
			pr_socketoption.option = PR_SockOpt_Nonblocking;
			pr_socketoption.value.non_blocking = 1;
			if ( PR_SetSocketOption( *pr_socket, &pr_socketoption ) == PR_FAILURE ) {
			     PRErrorCode prerr = PR_GetError();
			     LDAPDebug( LDAP_DEBUG_ANY,
					"PR_SetSocketOption(PR_SockOpt_Nonblocking) failed, "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					prerr, slapd_pr_strerror(prerr), 0 );
			}
		 
		 if ( have_send_timeouts ) {
		        daemon_configure_send_timeout(ns,config_get_ioblocktimeout());
		 }

	} /* else (secure) */


	if ( !enable_nagle ) {

		 pr_socketoption.option = PR_SockOpt_NoDelay;
		 pr_socketoption.value.no_delay = 1;
		 if ( PR_SetSocketOption( *pr_socket, &pr_socketoption ) == PR_FAILURE) {
			PRErrorCode prerr = PR_GetError();
			LDAPDebug( LDAP_DEBUG_ANY,
				   "PR_SetSocketOption(PR_SockOpt_NoDelay) failed, "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					prerr, slapd_pr_strerror( prerr ), 0 );
		 }
	} else {
		 pr_socketoption.option = PR_SockOpt_NoDelay;
		 pr_socketoption.value.no_delay = 0;
		 if ( PR_SetSocketOption( *pr_socket, &pr_socketoption ) == PR_FAILURE) {
			PRErrorCode prerr = PR_GetError();
			LDAPDebug( LDAP_DEBUG_ANY,
				   "PR_SetSocketOption(PR_SockOpt_NoDelay) failed, "
					SLAPI_COMPONENT_NAME_NSPR " error %d (%s)\n",
					prerr, slapd_pr_strerror( prerr ), 0 );
		 }
	} /* else (!enable_nagle) */
		 
	
	return ns;
	       
}




void configure_ns_socket( int * ns )
{

	int enable_nagle = config_get_nagle();
        int on;

#if defined(LINUX)
	/* On Linux we use TCP_CORK so we must enable nagle */
	enable_nagle = 1;
#endif

	if ( have_send_timeouts ) {
		daemon_configure_send_timeout( *ns, config_get_ioblocktimeout() );
	}


	if ( !enable_nagle ) {
	        on = 1;
	        setsockopt( *ns, IPPROTO_TCP, TCP_NODELAY, (char * ) &on, sizeof(on) );
	} else {
	        on = 0;
		setsockopt( *ns, IPPROTO_TCP, TCP_NODELAY, (char * ) &on, sizeof(on) );
	} /* else (!enable_nagle) */
		 
	
	return;
	       
}


#ifdef RESOLVER_NEEDS_LOW_FILE_DESCRIPTORS
/*
 * A function that uses the DNS resolver in a simple way.  This is only
 * used to ensure that the DNS resolver has opened its files, etc.
 * using low numbered file descriptors.
 */
static void
get_loopback_by_addr( void )
{
#ifdef GETHOSTBYADDR_BUF_T
    struct hostent		hp;
	GETHOSTBYADDR_BUF_T	hbuf;
#endif
    unsigned long	ipaddr;
    struct in_addr	ia;
    int				herrno, rc = 0;

    memset( (char *)&hp, 0, sizeof(hp));
    ipaddr = htonl( INADDR_LOOPBACK );
    (void) GETHOSTBYADDR( (char *)&ipaddr, sizeof( ipaddr ),
	    AF_INET, &hp, hbuf, sizeof(hbuf), &herrno );
}
#endif /* RESOLVER_NEEDS_LOW_FILE_DESCRIPTORS */
