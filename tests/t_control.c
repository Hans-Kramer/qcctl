// SPDX-License-Identifier: MPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 Hans Kramer
// SPDX-FileContributor: Claude Opus 5 (Anthropic), paired on the behavioural suite
//
// The control socket is mode 0666 - any user on the board can connect to it - and the only
// thing that makes that safe is that there is no vocabulary for lowering cooling. That is a
// privilege boundary resting on two string comparisons, so it is asserted here directly
// rather than through the daemon: t_daemon.sh exercises the accepted words with a real
// client, and this covers what happens to everything else.

#define _GNU_SOURCE
#include "../src/control.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <signal.h>

// sun_path is 108 bytes, so the path this test builds must fit inside it.
static char dir[64], sockpath[96];

/* Connect a client the way an unprivileged caller does. -1 on failure. */
static int client_connect(void)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sockpath);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read a reply with a deadline. Every read here is of a socket the SERVER is expected to
 * have written to or closed; a regression that leaves it open and silent would otherwise
 * block this test forever, and a hung test reports nothing at all - in CI it burns the job
 * and names no assertion. -2 means nothing arrived in time, which is itself a result.
 *
 * Found by mutating the capacity check: both forms of that bug hung the suite. */
static int cli_read(int fd, char *buf, size_t len)
{
    struct pollfd pf = { .fd = fd, .events = POLLIN };
    int r = poll(&pf, 1, 500);

    if (r <= 0)
        return -2;
    return (int)read(fd, buf, len);
}

/* One service pass over whatever is ready, as the daemon's loop does it. */
static int service(struct control *c)
{
    struct pollfd fds[CONTROL_MAXCLI + 1];
    int n = control_pollfds(c, fds, CONTROL_MAXCLI + 1);

    if (n <= 0)
        return control_holds(c);
    /* 0 means "report what is ready now": every assertion here acts on a write that has
     * already been made, so there is nothing to wait for. */
    poll(fds, (unsigned)n, 0);
    return control_service(c, fds, n);
}

/* Send a request and let the server act on it: ONE service pass, which is the read. The
 * accept is a separate pass and every caller does it before getting here - a caller that
 * skipped it would be servicing the accept with this call and reading nothing. */
static int send_req(struct control *c, int fd, const char *req)
{
    if (write(fd, req, strlen(req)) < 0)
        return -1;
    return service(c);
}

static void t_the_socket_is_world_writable(void)
{
    struct control c;
    struct stat st;
    char err[256];

    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    if (stat(sockpath, &st) == 0) {
        /* 0666 is the design, not an oversight, and it is only defensible because of the
         * restriction the rest of this file asserts. */
        T_EQ_INT(st.st_mode & 0666, 0666, "it is readable and writable by anyone");
        T_EQ_INT(st.st_mode & 0111, 0, "and executable by nobody");
    } else {
        T_FAIL("%s", "the socket was not created");
    }
    control_close(&c);
}

static void t_coolmax_holds_for_the_connection(void)
{
    struct control c;
    char err[256], reply[64];
    int fd, n;

    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    T_EQ_INT(control_holds(&c), 0, "nothing is holding to begin with");

    fd = client_connect();
    T_TRUE(fd >= 0, "a client connects");
    service(&c);                        /* accept */
    T_EQ_INT(control_holds(&c), 0, "connecting alone does not hold anything");

    T_EQ_INT(send_req(&c, fd, CONTROL_REQ_COOLMAX), 1, "COOLMAX is accepted and holds");
    n = cli_read(fd, reply, sizeof reply - 1);
    T_TRUE(n > 0, "the client gets a reply");
    if (n > 0) {
        reply[n] = '\0';
        T_EQ_STR(reply, "OK\n", "which is OK");
    }

    /* The hold IS the connection: there is no request to end one, so closing is the only
     * way, and that is what makes a client that crashes safe. */
    close(fd);
    service(&c);
    T_EQ_INT(control_holds(&c), 0, "closing the connection drops the hold");
    control_close(&c);
}

static void t_two_clients_hold_independently(void)
{
    struct control c;
    char err[256];
    int a, b;

    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    a = client_connect();
    service(&c);
    send_req(&c, a, CONTROL_REQ_COOLMAX);
    b = client_connect();
    service(&c);
    T_EQ_INT(send_req(&c, b, CONTROL_REQ_COOLMAX), 2, "two clients hold two holds");

    /* One going away must not release the fan while the other is still asking for it. */
    close(a);
    service(&c);
    T_EQ_INT(control_holds(&c), 1, "one leaving leaves the other holding");
    close(b);
    service(&c);
    T_EQ_INT(control_holds(&c), 0, "and the last one releases");
    control_close(&c);
}

static void t_everything_else_is_refused(void)
{
    /* The whole point: an unprivileged caller may ask for MORE cooling and has no way to
     * ask for less. Each of these is a plausible thing an attacker or a typo would send. */
    static const char *const refused[] = {
        "COOLMIN\n",            /* the obvious inverse */
        "COOLMAX OFF\n",        /* an argument where none is taken */
        "coolmax\n",            /* the right word, wrong case */
        "RELEASE\n",
        "STOP\n",
        "FAN 0\n",
        "\n",
        "COOLMA\n",             /* a prefix of the real one */
    };
    struct control c;
    char err[256], reply[64];

    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        int fd, n, holds;
        char what[80];

        T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
        fd = client_connect();
        service(&c);
        holds = send_req(&c, fd, refused[i]);

        snprintf(what, sizeof what, "'%s' holds nothing", refused[i]);
        for (char *p = what; *p; p++)
            if (*p == '\n')
                *p = ' ';
        T_EQ_INT(holds, 0, what);

        n = cli_read(fd, reply, sizeof reply - 1);
        if (n > 0) {
            reply[n] = '\0';
            T_HAS(reply, "ERR", "and is told ERR");
        } else {
            T_FAIL("%s", "a refused request got no reply at all");
        }
        close(fd);
        control_close(&c);
    }
}

static void t_coolmax_with_trailing_junk(void)
{
    struct control c;
    char err[256], big[320];
    int fd;

    /* The accepted form is a prefix match, so trailing bytes do not prevent the hold - but
     * only while they fit in the server's single 63-byte read. This used to be documented
     * as though it were general ("anything after it is still holding"), which is false: a
     * longer payload leaves a tail in the socket, the tail is read as a SECOND request on
     * the next pass, and that request matches nothing, so the hold is granted and then
     * taken away. Both halves are pinned, because only one of them was true. */
    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    fd = client_connect();
    service(&c);
    T_EQ_INT(send_req(&c, fd, "COOLMAX\nand more"), 1,
             "trailing bytes within one read still hold");
    T_EQ_INT(service(&c), 1, "and stay held, because nothing is left to read");
    close(fd);
    service(&c);

    memset(big, 'x', sizeof big);
    memcpy(big, CONTROL_REQ_COOLMAX, strlen(CONTROL_REQ_COOLMAX));
    big[sizeof big - 1] = '\0';
    fd = client_connect();
    service(&c);
    T_EQ_INT(send_req(&c, fd, big), 1, "a payload past the read buffer holds on the first pass");
    T_EQ_INT(service(&c), 0, "and loses the hold on the next, the tail being an unknown request");
    close(fd);
    service(&c);
    control_close(&c);
}

static void t_rebaseline_raises_a_flag_and_holds_nothing(void)
{
    struct control c;
    char err[256];
    int fd;

    /* Named for what it checks. This file cannot test "one-shot": control.c only ever RAISES
     * the flag, and the daemon's loop is what consumes and clears it - so the clearing is
     * daemon behaviour and belongs to a test that runs the loop. What is checkable here is
     * that the request is accepted, that it does NOT take the fan (unlike COOLMAX), that it
     * survives the client leaving, and that control.c does not clear it itself. */
    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    T_EQ_INT(c.rebaseline, 0, "the flag starts down");
    fd = client_connect();
    service(&c);
    T_EQ_INT(send_req(&c, fd, CONTROL_REQ_REBASELINE), 0,
             "REBASELINE is accepted without holding the fan");
    T_EQ_INT(c.rebaseline, 1, "and raises the flag");

    /* It is a request, not a hold: the daemon must still see it after the client is gone,
     * because the caller is a CLI invocation that exits immediately. */
    close(fd);
    service(&c);
    T_EQ_INT(c.rebaseline, 1, "which outlives the connection that asked for it");
    T_EQ_INT(control_holds(&c), 0, "with no hold left behind");
    control_close(&c);
}

/* ---- the capacity boundary ------------------------------------------------------- */

static void t_at_capacity_a_new_client_is_refused_and_the_rest_keep_their_holds(void)
{
    struct control c;
    char err[256], reply[64];
    int fd[CONTROL_MAXCLI + 1], i, n;

    /* CONTROL_MAXCLI is the whole defence of a 0666 socket against a client that simply
     * keeps connecting, and nothing reached it: removing the capacity check leaves the slot
     * index at -1, where cli[-1] aliases listen_fd and holding[-1] aliases the last client,
     * and the daemon HANGS rather than failing. A hang is the one outcome a test cannot
     * report, so the boundary is pinned here.
     *
     * Every connection is serviced as it is made, so the accept queue never backs up: the
     * refusal under test is the slot table being full, not the listen backlog. */
    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    for (i = 0; i < CONTROL_MAXCLI; i++) {
        fd[i] = client_connect();
        service(&c);
        send_req(&c, fd[i], CONTROL_REQ_COOLMAX);
    }
    T_EQ_INT(control_holds(&c), CONTROL_MAXCLI, "every slot can hold at once");

    /* One more than fits. */
    fd[CONTROL_MAXCLI] = client_connect();
    T_TRUE(fd[CONTROL_MAXCLI] >= 0, "a client past capacity still connects");
    service(&c);
    T_EQ_INT(control_holds(&c), CONTROL_MAXCLI,
             "but gains no slot, so the holds are unchanged");

    /* It is accepted and closed without a reply, so the client sees end-of-file. That is
     * the contract a caller has to cope with: not an ERR, just a closed connection.
     *
     * Nothing is written to it first. The server has already closed its end, so a write
     * would earn SIGPIPE and kill this process before it could report anything - which is
     * exactly what happened the first time this case was written. */
    n = cli_read(fd[CONTROL_MAXCLI], reply, sizeof reply - 1);
    if (n == -2)
        T_FAIL("%s", "the client past capacity was left connected and silent");
    else
        T_EQ_INT(n, 0, "and is told by end-of-file, not by an error reply");
    close(fd[CONTROL_MAXCLI]);

    /* The table is intact, not corrupted: a slot freed by a departing client is reusable. */
    close(fd[0]);
    service(&c);
    T_EQ_INT(control_holds(&c), CONTROL_MAXCLI - 1, "a departure frees exactly one hold");
    fd[0] = client_connect();
    service(&c);
    T_EQ_INT(send_req(&c, fd[0], CONTROL_REQ_COOLMAX), CONTROL_MAXCLI,
             "and the freed slot is handed to the next client");

    for (i = 0; i < CONTROL_MAXCLI; i++)
        close(fd[i]);
    service(&c);
    control_close(&c);
}

static void t_clients_that_say_nothing_still_occupy_slots(void)
{
    struct control c;
    char err[256], reply[64];
    int quiet[CONTROL_MAXCLI], asker, n;
    int i;

    /* THE QUESTION TWO AUDITORS DISAGREED ABOUT, settled here by measuring it.
     *
     * The socket is 0666 by design, on the argument that a request can only ever ask for
     * MORE cooling. That argument is about what a client can make the daemon DO; it says
     * nothing about a client taking up room. There is no idle timeout anywhere, so a
     * process that connects and never speaks holds its slot for as long as it lives.
     *
     * Whatever the answer is, it belongs in a test rather than in an argument. */
    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    for (i = 0; i < CONTROL_MAXCLI; i++) {
        quiet[i] = client_connect();
        service(&c);                    /* accepted, and says nothing at all */
    }
    T_EQ_INT(control_holds(&c), 0, "clients that send nothing hold nothing");

    /* And now someone who actually wants cooling. */
    asker = client_connect();
    T_TRUE(asker >= 0, "a ninth client still connects");
    service(&c);
    /* The write itself fails: the server took the connection, found no slot, and closed it
     * before anything was sent - so this is EPIPE, not a refusal message. (SIGPIPE is
     * ignored in main(), or that write would kill the test instead of returning.) */
    (void)send_req(&c, asker, CONTROL_REQ_COOLMAX);
    T_EQ_INT(control_holds(&c), 0,
             "with every slot occupied by silent clients, a request gets no hold");
    n = cli_read(asker, reply, sizeof reply - 1);
    T_EQ_INT(n, 0, "and the connection is already closed - no reply, not even an ERR");

    /* So this IS a denial, and it is recorded as one: an unprivileged process can stop
     * another unprivileged process raising the fan. What it cannot do is lower cooling or
     * disable the zone - the kernel's own governor runs underneath this the whole time and
     * is never touched by the socket - so what is lost is the EXTRA cooling --cool-max asks
     * for, not the board's thermal protection. Written down in TODO.md rather than fixed,
     * because every fix (an idle timeout, a per-uid quota) adds a way for a hold that is
     * wanted to be dropped. */
    for (i = 0; i < CONTROL_MAXCLI; i++)
        close(quiet[i]);
    service(&c);
    T_EQ_INT(control_holds(&c), 0, "freeing the slots does not revive a closed connection");
    close(asker);

    /* But a NEW client, once the slots are free, is served normally - so the condition is
     * occupancy and nothing worse. */
    asker = client_connect();
    service(&c);
    T_EQ_INT(send_req(&c, asker, CONTROL_REQ_COOLMAX), 1,
             "and once the slots are free, cooling is granted again");
    close(asker);
    service(&c);
    control_close(&c);
}

/* ---- the vocabulary, as a closed set --------------------------------------------- */

enum verdict { REFUSED, HELD, REBASED };

/* The SPECIFICATION, written here once and pinned: a request holds the fan only if it is
 * "COOLMAX\n" with anything after it, or exactly "COOLMAX"; it asks for a re-baseline only
 * on the same two forms of "REBASELINE"; everything else is refused. This is not the code's
 * own logic consulted a second time - it is a fixed rule the code is measured against, so a
 * vocabulary that WIDENS (a case-insensitive compare, a third verb) disagrees with it and
 * reddens, which is what asserting a closed set means. */
static enum verdict expected_for(const char *req)
{
    if (!strncmp(req, CONTROL_REQ_COOLMAX, strlen(CONTROL_REQ_COOLMAX))
        || !strcmp(req, "COOLMAX"))
        return HELD;
    if (!strncmp(req, CONTROL_REQ_REBASELINE, strlen(CONTROL_REQ_REBASELINE))
        || !strcmp(req, "REBASELINE"))
        return REBASED;
    return REFUSED;
}

static void t_the_accepted_vocabulary_is_exactly_two_words(void)
{
    static const char *const candidates[] = {
        /* the accepted forms, including the one a client that writes the newline
         * separately ends up sending */
        "COOLMAX\n", "COOLMAX", "COOLMAX\nand more", "REBASELINE\n", "REBASELINE",
        /* the same words, wrong in one way each */
        "coolmax\n", "CoolMax\n", "COOLMA\n", "COOLMAXX\n", " COOLMAX\n",
        "COOLMAX OFF\n", "\tCOOLMAX\n", "rebaseline\n", "ReBaseLine\n",
        "REBASELIN\n", "REBASELINEE\n", "REBASELINE NOW\n",
        /* and words nobody should be able to invent */
        "COOLMIN\n", "COOLOFF\n", "FANOFF\n", "FAN 0\n", "STOP\n", "RELEASE\n",
        "QUIT\n", "HELP\n", "SET cur_state 0\n", "0\n", "\n", "X",
    };
    struct control c;
    char err[256], reply[64];

    /* One socket for the whole set: a fresh CLIENT per candidate is what matters, and
     * re-opening the listener each time would count 29 identical "the socket opens" checks
     * as coverage. */
    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");

    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        enum verdict want = expected_for(candidates[i]);
        enum verdict got;
        char what[128], shown[48];
        int fd, holds, n;
        size_t j;

        /* The re-baseline flag is a latch the daemon clears, so it is cleared here between
         * candidates - otherwise one REBASELINE makes every later refusal look accepted. */
        c.rebaseline = 0;
        fd = client_connect();
        service(&c);
        holds = send_req(&c, fd, candidates[i]);
        got = holds > 0 ? HELD : (c.rebaseline ? REBASED : REFUSED);

        /* A refusal must also be told to the client, or an unprivileged caller is left
         * waiting on a reply that is never coming. */
        if (want == REFUSED) {
            n = cli_read(fd, reply, sizeof reply - 1);
            if (n > 0) {
                reply[n] = '\0';
                T_HAS(reply, "ERR", "a refusal is reported to the client");
            } else {
                T_FAIL("%s", "a refused request got no reply at all");
            }
        }

        snprintf(shown, sizeof shown, "%s", candidates[i]);
        for (j = 0; shown[j]; j++)
            if (shown[j] == '\n' || shown[j] == '\t')
                shown[j] = ' ';
        snprintf(what, sizeof what, "'%s' is %s", shown,
                 want == HELD ? "a hold" : want == REBASED ? "a re-baseline" : "refused");
        T_EQ_INT((int)got, (int)want, what);

        /* Release the slot before the next candidate, whichever way this one ended. */
        close(fd);
        service(&c);
    }
    control_close(&c);
}

static void t_only_the_first_request_in_a_buffer_is_honoured(void)
{
    struct control c;
    char err[256];
    int fd;

    /* A deliberate limit, documented in control.h rather than widened: each read is matched
     * as a whole, so a client that batches two requests has only its first honoured. Pinned
     * so that a future change to the parser is a decision and not a surprise. */
    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    fd = client_connect();
    service(&c);
    T_EQ_INT(send_req(&c, fd, "COOLMAX\nREBASELINE\n"), 1,
             "two requests in one write: the first is honoured");
    T_EQ_INT(c.rebaseline, 0, "and the second is not seen");
    close(fd);
    service(&c);

    /* The other way round, so this is about position and not about which word it is. */
    c.rebaseline = 0;
    fd = client_connect();
    service(&c);
    T_EQ_INT(send_req(&c, fd, "REBASELINE\nCOOLMAX\n"), 0,
             "the other order: the hold is not taken");
    T_EQ_INT(c.rebaseline, 1, "and the re-baseline is");
    close(fd);
    service(&c);
    control_close(&c);
}

static void t_drop_holds_closes_them(void)
{
    struct control c;
    char err[256];
    int a, b;

    /* The daemon drops every hold when it said OK and then could not take the zone. The
     * client finds out because its connection goes away, which is the only signal there
     * is - there is no "sorry" message in the vocabulary. */
    T_EQ_INT(control_open(&c, sockpath, err, sizeof err), 0, "the socket opens");
    a = client_connect();
    service(&c);
    send_req(&c, a, CONTROL_REQ_COOLMAX);
    b = client_connect();
    service(&c);
    send_req(&c, b, CONTROL_REQ_COOLMAX);

    T_EQ_INT(control_drop_holds(&c), 2, "both holds are dropped");
    T_EQ_INT(control_holds(&c), 0, "and none is left holding");
    close(a);
    close(b);
    control_close(&c);
}

int main(void)
{
    char cmd[128];

    /* A closed peer must fail a write, not kill the test: the capacity case talks to a
     * connection the server has already dropped, and a signal death reports nothing at all. */
    signal(SIGPIPE, SIG_IGN);

    snprintf(dir, sizeof dir, "/tmp/qcctl-t_control.%d", (int)getpid());
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot create %s\n", dir);
        return 2;
    }
    snprintf(sockpath, sizeof sockpath, "%s/control", dir);

    t_the_socket_is_world_writable();
    t_coolmax_holds_for_the_connection();
    t_two_clients_hold_independently();
    t_everything_else_is_refused();
    t_coolmax_with_trailing_junk();
    t_rebaseline_raises_a_flag_and_holds_nothing();
    t_at_capacity_a_new_client_is_refused_and_the_rest_keep_their_holds();
    t_clients_that_say_nothing_still_occupy_slots();
    t_the_accepted_vocabulary_is_exactly_two_words();
    t_only_the_first_request_in_a_buffer_is_honoured();
    t_drop_holds_closes_them();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: %s was left behind\n", dir);
    return t_summary("control");
}
