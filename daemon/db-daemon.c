
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#ifndef WINNT
#include <sys/socket.h>
#include <sys/un.h>
#else
#include <windows.h>
#include <sddl.h>
#include <Lmcons.h>
#include <tchar.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include "buffer.h"

#ifndef WINNT
#ifdef HAVE_SYSTEMD
/* For now link with systemd unconditionaly (all Fedora versions are using it,
 * Archlinux also). But if someone needs no systemd in dependencies,
 * it can be easily turned off, check the code in main() - conditions on
 * getenv("NOTIFY_SOCKET").
 */
#include <systemd/sd-daemon.h>
#endif
#endif

#ifdef WINNT
/* on Linux we have systemd or other redirection from startup script, but on
 * Windows we haven't - so log directly to file.
 * Service should be started with user profile as working directory, so we can
 * safely use relative path (especially useful for WNI)
 */
#define USE_LOGFILE
#define LOGFILE_PATH "qubesdb.log"
#endif

#include <qubesdb.h>
#include "qubesdb_internal.h"

int sigterm_received = 0;

int init_vchan(struct db_daemon_data *d);

void sigterm_handler(int s) {
    sigterm_received = 1;
}

#ifdef WINNT
/** Helper function to report errors. Similar to perror, but uses GetLastError() instead of errno
 * @param prefix Error message prefix
 */
void winnt_perror(char *prefix) {
    size_t  cchErrorTextSize;
    PUCHAR  pMessage = NULL;
    char   szMessage[2048];
    int     ret;
    ULONG   uErrorCode;

    uErrorCode = GetLastError();

    memset(szMessage, 0, sizeof(szMessage));
    cchErrorTextSize = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            uErrorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)&pMessage,
            0,
            NULL);
    if (!cchErrorTextSize) {
        if (!snprintf(szMessage, RTL_NUMBER_OF(szMessage), " failed with error %d\n", uErrorCode))
            return;
    } else {

        ret = snprintf(
                szMessage,
                RTL_NUMBER_OF(szMessage),
                " failed with error %d: %s%s",
                uErrorCode,
                pMessage,
                ((cchErrorTextSize >= 1) && (0x0a == pMessage[cchErrorTextSize - 1])) ? "" : "\n");
        LocalFree(pMessage);

        if (!ret)
            return;
    }

    fprintf(stderr, "%s%s", prefix, szMessage);
}
#endif

/** Register new client
 * @param d Daemon global data
 * @param c Socket of new client
 * @return 1 on success, 0 on failure
 */
int add_client(struct db_daemon_data *d, client_socket_t c
#ifdef WINNT
        , HANDLE socket_event
#endif
        ) {
    struct client *client;

    client = malloc(sizeof(*client));
    if (!client) {
        fprintf(stderr, "ERROR: cannot allocate memory for new client\n");
        return 0;
    }
    client->fd = c;
#ifdef WINNT
    client->pending_io = 0;
    memset(&client->overlapped_read, 0, sizeof(client->overlapped_read));
    client->overlapped_read.hEvent = socket_event;
#else
    client->write_queue = buffer_create();
    if (!client->write_queue) {
        fprintf(stderr, "ERROR: cannot allocate memory for new client buffer\n");
        free(client);
        return 0;
    }
#endif
    client->next = d->client_list;
    d->client_list = client;

    return handle_client_connect(d, client);
}

/** Disconnect client
 * @param d Daemon global data
 * @param c Socket of client to disconnect
 * @return 1 on success, 0 on failure
 */
int disconnect_client(struct db_daemon_data *d, struct client *c) {
    struct client *client, *prev_client;

    if (!handle_client_disconnect(d, c))
        return 0;

#ifdef WINNT
    DisconnectNamedPipe(c->fd);
    CloseHandle(c->fd);
#else
    close(c->fd);
    buffer_free(c->write_queue);
#endif

    client = d->client_list;
    prev_client = NULL;
    while (client) {
        if (client == c) {
            if (prev_client)
                prev_client->next = client->next;
            else
                d->client_list = client->next;
            free(client);
            break;
        }
        prev_client = client;
        client = client->next;
    }

    return 1;
}

#ifdef WINNT
/** Prepare server socket for new client connection
 * @param d Daemon global data
 * @return 1 on success, 0 on failure
 */
static int prepare_socket_for_new_client(struct db_daemon_data *d) {

    if (!d->socket_sa) {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
                    //TEXT("S:(ML;;NW;;;LW)D:(A;;FA;;;SY)(A;;FA;;;CO)"),
                    TEXT("D:(A;;FA;;;SY)(A;;FA;;;CO)"),
                    SDDL_REVISION_1,
                    &d->socket_sa,
                    NULL)) {
            perror("ConvertStringSecurityDescriptorToSecurityDescriptor");
            return 0;
        }
    }

    d->socket_inst = CreateNamedPipe(
            d->socket_path,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_MAX_INSTANCES,
            4096, // output buffer size
            4096, // input buffer size
            PIPE_TIMEOUT, // client time-out
            d->socket_sa);
    if (d->socket_inst == INVALID_HANDLE_VALUE) {
        perror("CreateNamedPipe");
        return 0;
    }

    memset(&d->socket_inst_wait, 0, sizeof(OVERLAPPED));
    d->socket_inst_wait.hEvent = CreateEvent(
            NULL, // default security attribute
            FALSE, // auto-reset event
            FALSE, // initial state = not signaled
            NULL); // unnamed event object

    if (d->socket_inst_wait.hEvent == INVALID_HANDLE_VALUE) {
        CloseHandle(d->socket_inst);
        perror("CreateEvent");
        return 0;
    }

    if (!ConnectNamedPipe(d->socket_inst, &d->socket_inst_wait)) {
        switch (GetLastError()) {
            case ERROR_IO_PENDING:
                break;
            case ERROR_PIPE_CONNECTED:
                SetEvent(d->socket_inst_wait.hEvent);
                break;
            default:
                CloseHandle(d->socket_inst);
                CloseHandle(d->socket_inst_wait.hEvent);
                perror("ConnectNamedPipe");
                return 0;
        }
    }
    return 1;
}
#endif /* WINNT */


/** Receive new client connection and register such client
 * @param d Daemon global data
 * @return 1 on success, 0 on failure
 */
int accept_new_client(struct db_daemon_data *d) {
    client_socket_t new_client_fd;
#ifndef WINNT
    struct sockaddr_un peer;
    unsigned int addrlen;
#else
    HANDLE socket_event;
    DWORD unused;
#endif

#ifdef WINNT
    new_client_fd = d->socket_inst;
    /* reuse already created event object */
    socket_event = d->socket_inst_wait.hEvent;
    if (!GetOverlappedResult(d->socket_inst, &d->socket_inst_wait, &unused, FALSE)) {
        perror("ConnectToNewClient");
        exit(1);
    }
    if (!prepare_socket_for_new_client(d))
        exit(1);
    return add_client(d, new_client_fd, socket_event);
#else /* !WINNT */
    addrlen = sizeof(peer);
    new_client_fd = accept(d->socket_fd, (struct sockaddr *) &peer, &addrlen);
    if (new_client_fd == -1) {
        perror("unix accept");
        exit(1);
    }
    return add_client(d, new_client_fd);
#endif /* !WINNT */
}

#ifdef WINNT
/* read_events must be large enough - at least client count + 2 */
int fill_events_for_wait(struct db_daemon_data *d,
        HANDLE * read_events) {
    struct client *client;
    int max_ev = 0;

    read_events[max_ev++] = d->socket_inst_wait.hEvent;
    if (d->vchan) {
        read_events[max_ev++] = libvchan_fd_for_select(d->vchan);
    }

    client = d->client_list;
    while (client) {
        if (!client->pending_io) {
            if (!ReadFile(client->fd, client->read_buffer,
                        sizeof(struct qdb_hdr), NULL, &client->overlapped_read)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    /* TODO: remove the client? */
                    client = client->next;
                    continue;
                }
            }
            client->pending_io = 1;
        }
        read_events[max_ev++] = client->overlapped_read.hEvent;
        client = client->next;
    }
    return max_ev;
}

int mainloop(struct db_daemon_data *d) {
    struct client *client;
    int event_count;
    HANDLE read_events[QDB_MAX_CLIENTS+2];
    int ret;

    while (1) {
        event_count = fill_events_for_wait(d, read_events);

        /* TODO: add one more event for service termination */
        ret = WaitForMultipleObjects(event_count, read_events, FALSE, INFINITE);
        if (ret >= event_count) {
            /* client could have disconnected just before select call, so
             * ignore this error and retry
             * FIXME: This probably will loop indefinitelly */
            /* TODO: implement above comment */
            perror("WaitForMultipleObjects");
            break;
        }

        if (d->vchan) {
            if (WaitForSingleObject(libvchan_fd_for_select(d->vchan), 0)
                    == WAIT_OBJECT_0)
                libvchan_wait(d->vchan);
            if (d->remote_connected && !libvchan_is_open(d->vchan)) {
                fprintf(stderr, "vchan closed\n");
                if (!d->remote_name) {
                    /* In the VM, wait for possible qubesdb-daemon dom0 restart.
                     * This can be a case for DispVM  */
                    /* FIXME: in such case dom0 daemon will have no entries
                     * currently present in VM instance; perhaps we should
                     * clear VM instance? */
                    if (!init_vchan(d)) {
                        fprintf(stderr, "vchan reconnection failed\n");
                        break;
                    }
                    /* request database sync from dom0 */
                    if (!request_full_db_sync(d)) {
                        fprintf(stderr, "FATAL: failed to request DB sync\n");
                        exit(1);
                    }
                    d->multiread_requested = 1;
                } else {
                    break;
                }
                break;
            }
            if (d->remote_connected || libvchan_is_open(d->vchan) == 1)
                while (libvchan_data_ready(d->vchan)) {
                    if (!handle_vchan_data(d)) {
                        fprintf(stderr, "FATAL: vchan data processing failed\n");
                        exit(1);
                    }
                }
        }

        /* check if there is some data from a client
         * 0 - listening "socket"
         * 1 - vchan event (if connected)
         */
        if (ret > 0 && (!d->vchan || ret > 1)) {
            client = d->client_list;
            while (client) {
                if (client->pending_io && read_events[ret] == client->overlapped_read.hEvent) {
                    DWORD got_bytes;
                    struct client *client_to_remove = NULL;
                    if (!GetOverlappedResult(client->fd, &client->overlapped_read, &got_bytes, FALSE)) {
                        perror("client read");
                        client_to_remove = client->fd;
                    }
                    client->pending_io = 0;
                    if (!handle_client_data(d, client, client->read_buffer, got_bytes)) {
                        client_to_remove = client;
                    }
                    if (client_to_remove) {
                        client = client->next;
                        disconnect_client(d, client_to_remove);
                        continue;
                    }
                }
                client = client->next;
            }
        }

        if (ret == 0) {
            accept_new_client(d);
        }
    }
    return 1;
}


int init_server_socket(struct db_daemon_data *d) {
    /* In dom0 listen only on "local" socket */
    if (d->remote_name && d->remote_domid != 0) {
        _stprintf_s(d->socket_path, MAX_FILE_PATH,
                QDB_DAEMON_PATH_PATTERN, d->remote_name);
    } else {
#ifdef BACKEND_VMM_wni
        /* on WNI we don't have separate namespace for each VM (all is in the
         * single system) */
        DWORD user_name_len = UNLEN + 1;
        TCHAR user_name[user_name_len];

        if (!GetUserName(user_name, &user_name_len)) {
            perror("GetUserName");
            return 0;
        }
        _stprintf_s(d->socket_path, MAX_FILE_PATH,
                QDB_DAEMON_LOCAL_PATH, user_name);
#else
        _stprintf_s(d->socket_path, MAX_FILE_PATH,
                QDB_DAEMON_LOCAL_PATH);
#endif
    }

    return prepare_socket_for_new_client(d);
}

#else /* !WINNT */

int fill_fdsets_for_select(struct db_daemon_data *d,
        fd_set * read_fdset, fd_set * write_fdset) {
    struct client *client;
    int max_fd;

    FD_ZERO(read_fdset);
    FD_ZERO(write_fdset);
    FD_SET(d->socket_fd, read_fdset);
    max_fd = d->socket_fd;
    if (d->vchan) {
        FD_SET(libvchan_fd_for_select(d->vchan), read_fdset);
        if (libvchan_fd_for_select(d->vchan) > max_fd)
            max_fd = libvchan_fd_for_select(d->vchan);
    }

    client = d->client_list;
    while (client) {
        /* Do not read commands from client, which have some buffered data,
         * first try to send them all. If client do not handle write buffering
         * properly, it can cause a deadlock there, but at least qubesdb-daemon
         * will still handle other requests */
        if (buffer_datacount(client->write_queue))
            FD_SET(client->fd, write_fdset);
        else
            FD_SET(client->fd, read_fdset);
        if (client->fd > max_fd)
            max_fd = client->fd;
        client = client->next;
    }
    return max_fd;
}

int mainloop(struct db_daemon_data *d) {
    fd_set read_fdset;
    fd_set write_fdset;
    struct client *client;
    int max_fd;
    int ret;
    sigset_t sigterm_mask;
    sigset_t oldmask;
    struct timespec ts = { 10, 0 };

    sigemptyset(&sigterm_mask);
    sigaddset(&sigterm_mask, SIGTERM);
    
    while (1) {
        max_fd = fill_fdsets_for_select(d, &read_fdset, &write_fdset);

        if (sigprocmask(SIG_BLOCK, &sigterm_mask, &oldmask) < 0) {
            perror("sigprocmask");
            break;
        }
        if (sigterm_received) {
            fprintf(stderr, "terminating\n");
            break;
        }
        ret = pselect(max_fd+1, &read_fdset, &write_fdset, NULL, &ts, &oldmask);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            /* client could have disconnected just before select call, so
             * ignore this error and retry
             * FIXME: This probably will loop indefinitelly */
            if (errno == EBADF)
                continue;
            perror("select");
            break;
        }
        /* restore signal mask */
        sigprocmask(SIG_SETMASK, &oldmask, NULL);

        if (d->vchan) {
            if (FD_ISSET(libvchan_fd_for_select(d->vchan), &read_fdset))
                libvchan_wait(d->vchan);
            if (d->remote_connected && !libvchan_is_open(d->vchan)) {
                fprintf(stderr, "vchan closed\n");
                if (!d->remote_name) {
                    /* In the VM, wait for possible qubesdb-daemon dom0 restart.
                     * This can be a case for DispVM  */
                    /* FIXME: in such case dom0 daemon will have no entries
                     * currently present in VM instance; perhaps we should
                     * clear VM instance? */
                    if (!init_vchan(d)) {
                        fprintf(stderr, "vchan reconnection failed\n");
                        break;
                    }
                    /* request database sync from dom0 */
                    if (!request_full_db_sync(d)) {
                        fprintf(stderr, "FATAL: failed to request DB sync\n");
                        exit(1);
                    }
                    d->multiread_requested = 1;
                } else {
                    break;
                }
            }
            while (libvchan_data_ready(d->vchan)) {
                if (!handle_vchan_data(d)) {
                    fprintf(stderr, "FATAL: vchan data processing failed\n");
                    exit(1);
                }
            }
        }

        client = d->client_list;
        while (client) {
            if (FD_ISSET(client->fd, &write_fdset)) {
                /* just send bufferred data, possibly not all of them */
                write_client_buffered(client, NULL, 0);
            }
            if (FD_ISSET(client->fd, &read_fdset)) {
                if (!handle_client_data(d, client, NULL, 0)) {
                    struct client *client_to_remove = client;
                    client = client->next;
                    disconnect_client(d, client_to_remove);
                    continue;
                }
            }
            client = client->next;
        }

        if (FD_ISSET(d->socket_fd, &read_fdset)) {
            accept_new_client(d);
        }
    }
    return 1;
}

#define MAX_FILE_PATH 256
int init_server_socket(struct db_daemon_data *d) {
    char socket_address[MAX_FILE_PATH];
    struct sockaddr_un sockname;
    int s;
    mode_t old_umask;

    if (d->remote_name) {
        snprintf(socket_address, MAX_FILE_PATH,
                QDB_DAEMON_PATH_PATTERN, d->remote_name);
        if (d->remote_domid == 0) {
            /* the same daemon as both VM and Admin parts */
            unlink(QDB_DAEMON_LOCAL_PATH);
            if (symlink(socket_address, QDB_DAEMON_LOCAL_PATH) < 0) {
                perror("symlink " QDB_DAEMON_LOCAL_PATH);
                return 0;
            }
        }
    } else {
        snprintf(socket_address, MAX_FILE_PATH,
                QDB_DAEMON_LOCAL_PATH);
    }

    unlink(socket_address);

    /* make socket available for anyone */
    old_umask = umask(0);

    s = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&sockname, 0, sizeof(sockname));
    sockname.sun_family = AF_UNIX;
    memcpy(sockname.sun_path, socket_address, strlen(socket_address));

    if (bind(s, (struct sockaddr *) &sockname, sizeof(sockname)) == -1) {
        printf("bind() failed\n");
        close(s);
        return 0;
    }
//      chmod(sockname.sun_path, 0666);
    if (listen(s, SERVER_SOCKET_BACKLOG) == -1) {
        perror("listen() failed\n");
        close(s);
        return 0;
    }
    d->socket_fd = s;
    umask(old_umask);
    return 1;
}
 
#endif /* !WINNT */

int init_vchan(struct db_daemon_data *d) {

    if (d->vchan) {
        libvchan_close(d->vchan);
        d->vchan = NULL;
    }
    if (d->remote_name) {
        /* dom0 part: listen for connection */
        if (d->remote_domid == 0) {
            /* do not connect from dom0 to dom0 */
            d->vchan = NULL;
            return 1;
        }
        d->vchan = libvchan_server_init(d->remote_domid, QUBESDB_VCHAN_PORT, 4096, 4096);
        if (!d->vchan)
            return 0;
        d->remote_connected = 0;
    } else {
        /* VM part: connect to admin domain */
        d->vchan = libvchan_client_init(d->remote_domid, QUBESDB_VCHAN_PORT);
        if (!d->vchan)
            return 0;
        d->remote_connected = 1;
    }
    return 1;
}

int create_pidfile(struct db_daemon_data *d) {
    char pidfile_name[256];
    FILE *pidfile;
    mode_t old_umask;

    if (!d->remote_name)
        snprintf(pidfile_name, sizeof(pidfile_name),
                "/var/run/qubes/qubesdb.pid");
    else
        snprintf(pidfile_name, sizeof(pidfile_name),
                "/var/run/qubes/qubesdb.%s.pid", d->remote_name);

    old_umask = umask(0002);
    pidfile = fopen(pidfile_name, "w");
    umask(old_umask);
    if (!pidfile) {
        perror("pidfile create");
        return 0;
    }
    fprintf(pidfile, "%d\n", getpid());
    fclose(pidfile);
    return 1;
}

void remove_pidfile(struct db_daemon_data *d) {
    char pidfile_name[256];

    if (!d->remote_name)
        snprintf(pidfile_name, sizeof(pidfile_name),
                "/var/run/qubes/qubesdb.pid");
    else
        snprintf(pidfile_name, sizeof(pidfile_name),
                "/var/run/qubes/qubesdb.%s.pid", d->remote_name);

    unlink(pidfile_name);
}

void close_server_socket(struct db_daemon_data *d) {
#ifndef WINNT
    struct sockaddr_un sockname;
    socklen_t addrlen;

    if (d->socket_fd < 0)
        /* already closed */
        return ;
    addrlen = sizeof(sockname);
    if (getsockname(d->socket_fd, (struct sockaddr *)&sockname, &addrlen) < 0)
        /* just do not remove socket when cannot get its path */
        return;

    close(d->socket_fd);
    unlink(sockname.sun_path);
#else
    if (d->socket_inst != INVALID_HANDLE_VALUE) {
        /* cancel ConnectNamedPipe */
        CancelIo(d->socket_inst);
        CloseHandle(d->socket_inst);
        LocalFree(d->socket_sa);
        d->socket_sa = NULL;
    }
#endif
}

void usage(char *argv0) {
    fprintf(stderr, "Usage: %s <remote-domid> [<remote-name>]\n", argv0);
    fprintf(stderr, "       Give <remote-name> only in dom0\n");
}

int main(int argc, char **argv) {
    struct db_daemon_data d;
    int ready_pipe[2] = {0, 0};
#ifndef WINNT
    pid_t pid;
#endif
    int ret;

    if (argc != 2 && argc != 3 && argc != 4) {
        usage(argv[0]);
        exit(1);
    }

    memset(&d, 0, sizeof(d));

    d.remote_domid = atoi(argv[1]);
    if (argc >= 3 && strlen(argv[2]) > 0)
        d.remote_name = argv[2];
    else
        d.remote_name = NULL;

    /* if not running under SystemD, fork and use pipe() to notify parent about
     * sucessful start */
    /* FIXME: OS dependent code */
#ifndef WINNT
#ifdef HAVE_SYSTEMD
    if (!getenv("NOTIFY_SOCKET")) {
#else
    if (1) {
#endif
        char buf[6];
        char log_path[MAX_FILE_PATH];
        int log_fd;
        mode_t old_umask;

        if (pipe(ready_pipe) < 0) {
            perror("pipe");
            exit(1);
        }
        switch (pid = fork()) {
            case -1:
                perror("fork");
                exit(1);
            case 0:
                close(ready_pipe[0]);
                if (d.remote_name)
                    snprintf(log_path, sizeof(log_path), "/var/log/qubes/qubesdb.%s.log", d.remote_name);
                else
                    snprintf(log_path, sizeof(log_path), "/var/log/qubes/qubesdb.log");

                close(0);
                old_umask = umask(0);
                log_fd = open(log_path, O_WRONLY | O_CREAT, 0664);
                umask(old_umask);
                if (log_fd < 0) {
                    perror("open logfile");
                    exit(1);
                }
                dup2(log_fd, 1);
                dup2(log_fd, 2);
                close(log_fd);

                setsid();

                break;
            default:
                close(ready_pipe[1]);
                if (read(ready_pipe[0], buf, sizeof(buf)) < strlen("ready")) {
                    fprintf(stderr, "startup failed\n");
                    exit(1);
                }
                exit(0);
        }
    }

    /* setup graceful shutdown handling */
    signal(SIGTERM, sigterm_handler);
#else /* WINNT */
    /* start new process in the background and use the pipe to communicate with
     * it;
     * do this only in "dom0", VM process is already started as detached/service
     */
    if (d.remote_name) {
        if (argc < 4) {
            /* parent process */
            char buf[6];
            char own_path[256]; // MAX_PATH
            int own_path_len;

            if (!(own_path_len=GetModuleFileNameA(NULL,
                            own_path, sizeof(own_path)))) {
                perror("GetModuleFileName");
                exit(1);
            }

            if (_pipe(ready_pipe, 256 /* buffer size */, O_BINARY) == -1) {
                perror("_pipe");
                exit(1);
            }

            snprintf(buf, sizeof(buf), "%d", ready_pipe[1]);
            if (_spawnl(P_NOWAIT, own_path, "qubesdb-daemon", argv[1],
                        d.remote_name ? d.remote_name : "",
                        buf, NULL) < 0) {
                perror(own_path);
                exit(1);
            }
            close(ready_pipe[1]);
            if (read(ready_pipe[0], buf, sizeof(buf)) < strlen("ready")) {
                fprintf(stderr, "startup failed\n");
                exit(1);
            }
            exit(0);
        } else {
            ready_pipe[1] = atoi(argv[3]);
            FreeConsole();
        }
    }
#endif

#ifdef USE_LOGFILE
    /* ignore errors - no place to report them, just leave stderr connected to
     * the terminal(?) */
    freopen(LOGFILE_PATH, "a", stderr);
#endif

    d.db = qubesdb_init(write_client_buffered);
    if (!d.db) {
        fprintf(stderr, "FATAL: database initialization failed\n");
        exit(1);
    }

    if (!init_server_socket(&d)) {
        fprintf(stderr, "FATAL: server socket initialization failed\n");
        exit(1);
    }

    if (!init_vchan(&d)) {
        fprintf(stderr, "FATAL: vchan initialization failed\n");
        exit(1);
    }

    if (!d.remote_name) {
        /* request database sync from dom0 */
        if (!request_full_db_sync(&d)) {
            fprintf(stderr, "FATAL: failed to request DB sync\n");
            exit(1);
        }
        d.multiread_requested = 1;
        /* wait for complete response */
        while (d.multiread_requested) {
            if (!handle_vchan_data(&d)) {
                fprintf(stderr, "FATAL: vchan error\n");
                exit(1);
            }
        }
    }

    /* now ready for serving requests, notify parent */
    /* FIXME: OS dependent code */
#ifndef WINNT
#ifdef HAVE_SYSTEMD
    if (getenv("NOTIFY_SOCKET")) {
        sd_notify(1, "READY=1");
    } else
#endif
    {
        if (write(ready_pipe[1], "ready", strlen("ready")) != strlen("ready"))
            perror("failed to notify parent");
        close(ready_pipe[1]);
    }

    create_pidfile(&d);
#else /* WINNT */
    if (d.remote_name) {
        write(ready_pipe[1], "ready", strlen("ready"));
        close(ready_pipe[1]);
    }
#endif

    ret = !mainloop(&d);

    if (d.vchan)
        libvchan_close(d.vchan);

    close_server_socket(&d);

#ifndef WINNT
    remove_pidfile(&d);
#endif

    return ret;
}
