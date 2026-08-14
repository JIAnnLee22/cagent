/*
 * tests/test_server.h — throwaway local HTTP server helper (tests only).
 */

#ifndef CAGENT_TESTS_TEST_SERVER_H
#define CAGENT_TESTS_TEST_SERVER_H

#include <stddef.h>
#include <sys/types.h>

/* Bind a free loopback port and return it (race-prone but fine for
 * tests). -1 on failure. */
int test_server_find_free_port(void);

/* Fork + exec python3 serving POST and GET endpoints that reply with `body`
 * (chunked in 7-byte writes) and the given HTTP status. Returns the pid. */
pid_t test_server_start(int port, const char* body, int status);

/* Poll-connect until the server accepts; 0 on ready, -1 on timeout. */
int test_server_wait(int port, int timeout_ms);

void test_server_stop(pid_t pid);

#endif /* CAGENT_TESTS_TEST_SERVER_H */
